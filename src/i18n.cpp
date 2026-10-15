/*
 * i18n.cpp - 轻量 gettext 风格国际化实现（C++17）
 *
 * 数据流：
 *   DLL 内置资源 RCDATA("JSON_ZH")  ==  lang_res/zh.json（构建时由 .rc 链入）
 *        │  FindResourceA / LoadResource / LockResource（只读，需拷出再解析）
 *        ▼
 *   yyjson 解析 {"msgid": "msgstr", ...}
 *        │  key/value 各自 strdup（所有权归哈希表，I18n_Clear 时统一 free）
 *        ▼
 *   khash（src/khash.h）哈希表  ──►  I18n_GetText()：命中返回译文，
 *                                          未命中或译文为空则回退 msgid
 *
 * 语言来源：注册表 HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\Options
 *          的 CustomLanguage，取值就是宿主里的语言名（"ChineseSimplified"、
 *          "French (native tongue v3)" 等，见 kLangs 表），原样匹配即可。
 *          目前只有简体中文嵌了语言包；其余语言已占位，缺包时 `_()` 返回 msgid
 *          （英文原文），不会显示空白。新增语言见 kLangs 表注释。
 *
 * 线程模型：所有 i18n 调用都发生在 VB Decompiler 的 UI 主线程
 *         （设置窗体 + MCP 任务队列都在主线程执行），因此哈希表无需加锁。
 *         DLL 卸载时 DllMain 会置"停机标志"，此后 `_()` 直接返回 msgid，
 *         避免其它线程正在查表时被 I18n_Clear() 释放。
 */
#include "i18n.h"
#include "vbdmcp.h"          /* vbdmcp_logf / vbdmcp_alloc */
#include "khash.h"
#include "yyjson.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/* 注册表路径：与 1.reg 实际内容一致 */
#define I18N_LANG_REG_KEY   "SOFTWARE\\VB and VBA Program Settings\\VB Decompiler\\Options"
#define I18N_LANG_REG_VALUE "CustomLanguage"
/* 语言名/资源名的映射见下方 kLangs 表：注册表原文 ↔ 枚举 ↔ RCDATA 名。
 * 语言包只从资源加载，**不做磁盘回退**：资源缺失就回英文 msgid（见 I18n_SetMode）。 */

namespace {

/* ---------------- 全局状态 ----------------
 * g_mode / g_trans 用原子发布：写方是 UI 线程（换语言 / 清空），
 * 读方除 UI 线程外还包括 MCP 网络线程（mcp_http.cpp 在网络线程调用
 * _F() 生成错误消息）。表本身"发布后不再改动"——旧表只退休不释放，
 * 因此读者拿到的永远是一张完整的表，无需加锁。
 * g_json / g_source 仅 UI 线程访问（外部无调用点），保持普通变量。 */
std::atomic<bool>   g_shutting_down{ false };
std::atomic<unsigned> g_generation{ 1 };   /* 表世代号：每次重建/清空自增 */
std::atomic<I18nMode> g_mode{ I18N_MODE_EN };
std::atomic<khash_t*> g_trans{ nullptr };
yyjson_doc         *g_json = nullptr;
std::string         g_source = "none";

/* ---------------- 轮转缓冲池（ANSI / 格式化结果） ----------------
 * Win32 A 系列接口与日志都是"立即消费"语义，用轮转缓冲避免嵌套调用互相覆盖。
 * 16 槽 × 4KB = 64KB 常驻，足够覆盖一帧内所有控件创建。 */
constexpr int      kFmtSlotCount = 16;
constexpr size_t   kFmtSlotSize  = 4096;

struct FmtPool {
    char  slots[kFmtSlotCount][kFmtSlotSize];
    int   next = 0;

    char *slot()
    {
        char *p = slots[next];
        next = (next + 1) % kFmtSlotCount;
        p[0] = '\0';
        return p;
    }
};
/* thread_local：每个线程一份轮转缓冲，_A()/_AF() 因此也是线程安全的
 * （若用共享池，多线程同时格式化会互相覆盖，拿到别人的半截字符串）。 */
thread_local FmtPool g_pool;

/* UTF-8 → ANSI(GBK)：ASCII 快速路径直接复用输入指针 */
const char *to_ansi(const char *utf8)
{
    if (!utf8) return "";
    bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; ++p) {
        if (*p >= 0x80) { ascii = false; break; }
    }
    if (ascii) return utf8;

    char *out = g_pool.slot();
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 0) {
        /* 转换失败：截断复制，至少不崩（用户会看到部分乱码） */
        _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", utf8);
        return out;
    }
    std::wstring wbuf;
    wbuf.resize((size_t)wlen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wbuf[0], wlen) <= 0) {
        _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", utf8);
        return out;
    }
    const int glen = WideCharToMultiByte(CP_ACP, 0, &wbuf[0], wlen - 1, out,
                                         (int)kFmtSlotSize - 1, nullptr, nullptr);
    if (glen <= 0) {
        _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", utf8);
        return out;
    }
    out[glen] = '\0';
    return out;
}

/* ---------------- 语言包解析 ---------------- */

/* kh_foreach 回调：释放一对 strdup 出来的 key/value（哈希表本身不拥有内存） */
static int g_freed_pairs = 0;
static void free_pair(const char *key, void *val, void *user)
{
    vbdmcp_free((void *)(size_t)key);
    vbdmcp_free(val);            /* vbdmcp_free(NULL) 安全：被拒绝的译文存 NULL */
    if (user) *((int *)user) += 1;
}

/* ============================================================
 * 表的生命周期：发布后**不再改动**，读操作因此是纯只读、无锁。
 *
 * 换语言时不在原地 free+重建（旧表可能正被网络线程读取 → use-after-free），
 * 而是：新建表 → 换上全局指针 → 旧表挂进 g_retired，**停机时统一释放**。
 * 换语言是低频操作（用户改宿主语言设置），滞留几张表（每张几十 KB）
 * 的内存代价可忽略，换来的是任何线程都能直接 `_()`。
 * ============================================================ */
static std::mutex             g_table_mu;
static std::vector<khash_t*>  g_retired_maps;
static std::vector<yyjson_doc*> g_retired_docs;

void clear_silent()
{
    /* 只下线当前表，不释放：可能还有线程正在读它。真正释放见 I18n_Clear。 */
    if (khash_t *t = g_trans.exchange(nullptr, std::memory_order_acq_rel)) {
        g_retired_maps.push_back(t);
    }
    if (g_json)  { g_retired_docs.push_back(g_json);  g_json  = nullptr; }
    g_source = "none";
    g_generation.fetch_add(1);   /* 表换代：调用方据此刷新自己缓存的译文字面串 */
}

/* 停机/卸载：此时确定没有读者了，把历任表一次性释放干净 */
static void release_all_tables()
{
    for (khash_t *m : g_retired_maps) {
        if (!m) continue;
        g_freed_pairs = 0;
        kh_foreach(m, free_pair, &g_freed_pairs);
        kh_destroy(m);
    }
    g_retired_maps.clear();
    for (yyjson_doc *d : g_retired_docs) yyjson_doc_free(d);
    g_retired_docs.clear();
    if (khash_t *t = g_trans.exchange(nullptr)) {
        g_freed_pairs = 0;
        kh_foreach(t, free_pair, &g_freed_pairs);
        kh_destroy(t);
    }
    if (g_json) { yyjson_doc_free(g_json); g_json = nullptr; }
    g_source = "none";
    g_generation.fetch_add(1);
}

/* ============================================================
 * printf 格式串分析
 *
 * 产出三件事：
 *   sig       规范化占位符序列（如 "%d%s%zu"），用于比对译文与原文；
 *   convs     转换符个数；
 *   args      实际消耗的实参个数（转换符 + '*' 宽度/精度各额外吃 1 个）；
 *   parseable=false 表示串里有"字面量 %"(如帮助文本 "%TEMP%\\vbdmcp.log")，
 *             此时不做占位符校验，避免误伤正常文案。
 * ============================================================ */
struct FmtInfo {
    std::string sig;
    int  convs = 0;
    int  args = 0;
    bool parseable = true;
};

FmtInfo analyze_format(const char *fmt)
{
    FmtInfo info;
    if (!fmt) { info.parseable = false; return info; }

    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') continue;
        const char *start = p++;
        if (*p == '%') continue;                          /* 字面量 %% */

        int stars = 0;      /* '*' 从实参列表取值：%*.*f 共吃 3 个实参 */
        while (*p && strchr("-+ #0", *p)) ++p;            /* flags */
        while (*p && (isdigit((unsigned char)*p) || *p == '*')) {
            if (*p == '*') stars++;
            ++p;
        }
        if (*p == '.') {
            ++p;
            while (*p && (isdigit((unsigned char)*p) || *p == '*')) {
                if (*p == '*') stars++;
                ++p;
            }
        }
        while (*p && strchr("hhhlLqjzt", *p)) ++p;        /* length modifier */
        if (!*p || (!isalpha((unsigned char)*p) && *p != '%')) {
            info.parseable = false;
            return info;
        }
        info.sig.append(start, (size_t)(p - start + 1));
        info.convs++;
        info.args += 1 + stars;
    }
    return info;
}

bool extract_placeholders(const char *fmt, std::string *out)
{
    const FmtInfo info = analyze_format(fmt);
    if (!info.parseable) return false;
    if (out) *out = info.sig;
    return true;
}

/* 把 yyjson 顶层对象灌进哈希表（key/value 均 strdup） */
bool build_table(yyjson_doc *doc)
{
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!yyjson_is_obj(root)) return false;

    khash_t *table = kh_init_str(kh_str_hash_func, kh_str_hash_equal);
    if (!table) return false;

    yyjson_obj_iter it = yyjson_obj_iter_with(root);
    yyjson_val *k, *v;
    while ((k = yyjson_obj_iter_next(&it)) != nullptr) {
        v = yyjson_obj_iter_get_val(k);
        /* 取值语义（与 tools/extract_i18n.py 约定一致）：
         *   null / 键缺失   → 未翻译，回退英文原文（msgid）
         *   ""               → **故意留空**，原样返回空串（不是"未翻译"！）
         *   非空且占位符一致 → 译文
         * 但 msgid 自带占位符时，"" 会被拒绝：留空等于丢参数，会让 vsnprintf
         * 错参/崩溃，这种"留空"几乎一定是误操作，回退原文更安全。 */
        if (!yyjson_is_str(v)) continue;   /* null / 数字 / 数组等：视为未翻译 */

        const char *k_text = yyjson_get_str(k);
        const char *v_text = yyjson_get_str(v);

        char *val = nullptr;
        if (I18n_FormatCompatible(k_text, v_text)) {
            val = vbdmcp_strdup(v_text);      /* 含 ""：故意留空 */
        } else if (v_text[0] == '\0') {
            vbdmcp_logf(VBDMCP_LOG_WARN,
                       "i18n: empty translation rejected (msgid has placeholders): %s",
                       k_text);
        } else {
            vbdmcp_logf(VBDMCP_LOG_WARN,
                       "i18n: placeholder mismatch, translation rejected: %s", k_text);
        }

        char *key = vbdmcp_strdup(k_text);
        if (!key || (v_text[0] && !val)) {
            if (key) vbdmcp_free(key);
            if (val) vbdmcp_free(val);
            continue;                       /* key 都没建成功，整条丢弃 */
        }

        int ret = 0;
        kh_put(table, key, val, &ret);
        if (ret == 0) {          /* 极端情况：插入失败，避免泄漏 */
            vbdmcp_free(key);
            vbdmcp_free(val);
        }
    }
    /* 表建好后再原子发布：读者要么看到旧表，要么看到完整新表。 */
    g_trans.store(table, std::memory_order_release);
    return true;
}

/* 加载某个语言的语言包：资源名由 I18n_LangResourceName() 给（没有则 false） */
bool load_from_resource(const char *rsrc_name)
{
    if (!rsrc_name || !*rsrc_name) return false;

    HMODULE hSelf = vbdmcp_self_module();
    if (!hSelf) {
        vbdmcp_logf(VBDMCP_LOG_ERROR, "i18n: GetModuleHandleExA(from address) failed err=%lu",
                   GetLastError());
        return false;
    }

    HRSRC hRsrc = FindResourceA(hSelf, rsrc_name, RT_RCDATA);
    if (!hRsrc) {
        vbdmcp_logf(VBDMCP_LOG_ERROR, "i18n: RCDATA '%s' not found in module", rsrc_name);
        return false;
    }
    HGLOBAL hGlobal = LoadResource(hSelf, hRsrc);
    if (!hGlobal) return false;

    const DWORD len = SizeofResource(hSelf, hRsrc);
    const char *data = (const char *)LockResource(hGlobal);
    if (!data || len == 0) return false;

    /* yyjson 不可直接解析只读资源内存（原地解析会写入），先拷一份 */
    std::string buf(data, len);
    yyjson_doc *doc = yyjson_read(buf.c_str(), buf.size(),
                                  YYJSON_READ_NOFLAG);
    if (!doc) {
        vbdmcp_logf(VBDMCP_LOG_ERROR, "i18n: %s parse failed (resource, %u bytes)",
                   rsrc_name, len);
        return false;
    }
    g_json = doc;
    g_source = std::string("resource:") + rsrc_name;
    return build_table(doc);
}

/* ============================================================
 * 语言名 <-> 枚举
 *
 * 左边是 VB Decompiler 注册表 CustomLanguage 的原文（宿主写什么我们就认什么），
 * 右边是 RCDATA 资源名；目前只有 ChineseSimplified 有语言包，其余先占位。
 * 新增语言：加一行 + 一个 lang_res/<code>.json + rc 里一条 RCDATA。
 * ============================================================ */
struct LangEntry {
    I18nLang     lang;
    const char  *registry_name;   /* CustomLanguage 的值 */
    const char  *alt_name;       /* 宿主里的别名（可空） */
    const char  *code;           /* 两字母/带地区代码，用于日志与 VBDMCP_LANG */
    const char  *resource;       /* RCDATA 资源名；NULL = 暂无语言包 */
};

static const LangEntry kLangs[] = {
    { I18N_LANG_EN,              "English",              nullptr,                          "en",    nullptr   },
    { I18N_LANG_AR,              "Arabic",               nullptr,                          "ar",    nullptr   },
    { I18N_LANG_PT_BR,           "Brazilian Portuguese", nullptr,                          "pt-BR", nullptr   },
    { I18N_LANG_ZH_SIMPLIFIED,   "ChineseSimplified",    nullptr,                          "zh",    "JSON_ZH" },
    { I18N_LANG_ZH_TRADITIONAL,  "ChineseTraditional",   nullptr,                          "zh-TW", nullptr   },
    { I18N_LANG_CS,              "Czech",                nullptr,                          "cs",    nullptr   },
    { I18N_LANG_DA,              "Danish",               nullptr,                          "da",    nullptr   },
    { I18N_LANG_NL,              "Dutch",                nullptr,                          "nl",    nullptr   },
    /* 宿主里有 "French" 和 "French (native tongue v3)" 两个条目，都归到法语 */
    { I18N_LANG_FR,              "French",               "French (native tongue v3)",      "fr",    nullptr   },
    { I18N_LANG_DE,              "German",               nullptr,                          "de",    nullptr   },
    { I18N_LANG_EL,              "Greek",                nullptr,                          "el",    nullptr   },
    { I18N_LANG_HE,              "Hebrew",               nullptr,                          "he",    nullptr   },
    { I18N_LANG_ID,              "Indonesian",           nullptr,                          "id",    nullptr   },
    { I18N_LANG_IT,              "Italian",              nullptr,                          "it",    nullptr   },
    { I18N_LANG_JA,              "Japanese",             nullptr,                          "ja",    nullptr   },
    { I18N_LANG_KU,              "Kurdish (sorani)",     "Kurdish",                        "ku",    nullptr   },
    { I18N_LANG_FA,              "Persian",              nullptr,                          "fa",    nullptr   },
    { I18N_LANG_PL,              "Polish",               nullptr,                          "pl",    nullptr   },
    { I18N_LANG_PT,              "Portuguese",           nullptr,                          "pt",    nullptr   },
    { I18N_LANG_RU,              "Russian",              nullptr,                          "ru",    nullptr   },
    { I18N_LANG_SK,              "Slovak",               nullptr,                          "sk",    nullptr   },
    { I18N_LANG_ES,              "Spanish",              nullptr,                          "es",    nullptr   },
    { I18N_LANG_TR,              "Turkish",              nullptr,                          "tr",    nullptr   },
    { I18N_LANG_UK,              "Ukrainian",            nullptr,                          "uk",    nullptr   },
};

static const LangEntry *lang_entry(I18nLang lang)
{
    const int n = (int)(sizeof(kLangs) / sizeof(kLangs[0]));
    for (int i = 0; i < n; i++) {
        if (kLangs[i].lang == lang) return &kLangs[i];
    }
    return &kLangs[0];   /* 未知语言 → 英文 */
}

/* 去掉两端空白（注册表值可能带空格） */
static void trim_inplace(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/* 注：原先还有一个 load_from_file() 做磁盘回退（读 lang_res\zh.json），
 * 已删除 —— 语言包只认 RCDATA，缺失就回英文 msgid，避免磁盘上的开发副本
 * 掩盖"资源没链进 DLL"的打包问题。 */

} // namespace

/* ============================================================
 * 语言名 <-> 枚举（对外接口，需与 i18n.h 的 extern "C" 声明一致，
 * 所以必须放在匿名命名空间之外）
 * ============================================================ */
const char *I18n_LangResourceName(I18nLang lang)
{
    return lang_entry(lang)->resource;
}

int I18n_LangHasPack(I18nLang lang)
{
    const char *r = lang_entry(lang)->resource;
    return (r && *r) ? 1 : 0;
}

const char *I18n_LangName(I18nLang lang)
{
    return lang_entry(lang)->registry_name;
}

const char *I18n_LangCode(I18nLang lang)
{
    return lang_entry(lang)->code;
}

I18nLang I18n_LangFromName(const char *name)
{
    if (!name || !*name) return I18N_LANG_EN;

    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s", name);
    trim_inplace(buf);
    if (!buf[0]) return I18N_LANG_EN;

    const int n = (int)(sizeof(kLangs) / sizeof(kLangs[0]));
    for (int i = 0; i < n; i++) {
        if (_stricmp(buf, kLangs[i].registry_name) == 0) return kLangs[i].lang;
        if (kLangs[i].alt_name && _stricmp(buf, kLangs[i].alt_name) == 0) {
            return kLangs[i].lang;
        }
    }
    /* 也接受两字母代码（VBDMCP_LANG=ja / pt-BR 这种写法） */
    for (int i = 0; i < n; i++) {
        if (_stricmp(buf, kLangs[i].code) == 0) return kLangs[i].lang;
    }
    return I18N_LANG_EN;   /* 未知 → 英文（_() 返回 msgid） */
}

/* ============================================================
 * 对外接口
 * ============================================================ */

/* 纯拷贝（不做格式化）：轮转缓冲，立即使用 */
char *I18n_CopyText(const char *utf8)
{
    char *out = g_pool.slot();
    _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", utf8 ? utf8 : "");
    return out;
}

char *I18n_CopyTextAnsi(const char *utf8)
{
    char *out = g_pool.slot();
    const char *ansi = to_ansi(utf8 ? utf8 : "");
    _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", ansi);
    return out;
}

/* 该格式串需要多少个实参：
 *   >0  = 需要 N 个实参（转换符个数 + '*' 宽度/精度个数）
 *   0   = 没有占位符（或含字面量 %，无法解析）→ 不需要格式化
 * 供头文件里的 _F()/_AF() 变参模板做"占位符 vs 实参"核对。 */
int I18n_FormatArity(const char *fmt)
{
    const FmtInfo info = analyze_format(fmt);
    if (!info.parseable) return 0;
    return info.args;
}

void I18n_LogFormatMismatch(const char *fmt, int need, int got)
{
    vbdmcp_logf(VBDMCP_LOG_ERROR,
               "i18n: format/argument mismatch (needs %d, got %d): %s -- "
               "refusing to format (would read garbage from the stack)",
               need, got, fmt ? fmt : "(null)");
}

int I18n_FormatCompatible(const char *msgid, const char *translated)
{
    std::string sig_a, sig_b;
    if (!extract_placeholders(msgid, &sig_a)) return 1;      /* 原文含字面量 %：不校验 */
    if (!extract_placeholders(translated, &sig_b)) return 0; /* 译文里出现非法 % ：不可用 */
    return sig_a == sig_b;
}

void I18n_Clear()
{
    /* 停机/卸载：确认没有读者了，把历任表一次性释放干净 */
    std::lock_guard<std::mutex> lock(g_table_mu);
    release_all_tables();
    g_mode.store(I18N_LANG_EN);
}

int I18n_SetMode(I18nMode mode)
{
    /* 写路径串行：建表 + 发布期间不与另一次换语言交叉 */
    std::lock_guard<std::mutex> lock(g_table_mu);
    clear_silent();
    g_mode.store(mode);

    if (mode == I18N_LANG_EN) return 1;   /* 英文：_() 原样返回 msgid */

    const char *rsrc = I18n_LangResourceName(mode);
    if (rsrc && *rsrc && load_from_resource(rsrc)) {
        vbdmcp_logf(VBDMCP_LOG_INFO, "i18n: %s pack loaded from %s (%d entries)",
                   I18n_LangCode(mode), g_source.c_str(), I18n_TranslatedCount());
        return 1;
    }

    /* 该语言没有语言包（或资源缺失）→ 回英文：_(msgid) 直接返回 msgid 本身
     * （msgid 就是英文原文）。不做磁盘回退 —— 否则开发目录里的 lang_res\zh.json
     * 会掩盖"RCDATA 没链进 DLL"这种打包问题。 */
    if (!rsrc || !*rsrc) {
        vbdmcp_logf(VBDMCP_LOG_INFO,
                   "i18n: '%s' has no language pack yet, using msgid (English)",
                   I18n_LangName(mode));
    } else {
        vbdmcp_logf(VBDMCP_LOG_ERROR,
                   "i18n: pack unavailable for '%s' (RCDATA '%s' not found), "
                   "using msgid (English)", I18n_LangName(mode), rsrc);
    }
    g_mode.store(I18N_LANG_EN);
    return 0;
}

void I18n_Refresh()
{
    /* 调试/测试覆盖：环境变量 VBDMCP_LANG 优先于注册表 CustomLanguage。
     * 取值可以是注册表里的原文（如 "ChineseSimplified"）或两字母代码（zh/ja/pt-BR）。 */
    char env[64] = {0};
    if (GetEnvironmentVariableA("VBDMCP_LANG", env, sizeof(env)) > 0) {
        const I18nLang want_env = I18n_LangFromName(env);
        if (want_env != g_mode.load() || (want_env != I18N_LANG_EN && !g_trans.load())) {
            I18n_SetMode(want_env);
            vbdmcp_logf(VBDMCP_LOG_INFO, "i18n: language=%s (from VBDMCP_LANG='%s')",
                       I18n_LangCode(I18n_GetMode()), env);
        }
        return;
    }

    char val[64] = {0};
    DWORD cb = sizeof(val);
    DWORD type = 0;
    const LSTATUS st = RegGetValueA(HKEY_CURRENT_USER, I18N_LANG_REG_KEY,
                                    I18N_LANG_REG_VALUE, RRF_RT_REG_SZ,
                                    &type, val, &cb);
    /* 未知/缺失的值一律当英文 */
    const I18nLang want = (st == ERROR_SUCCESS)
                              ? I18n_LangFromName(val) : I18N_LANG_EN;
    if (want == g_mode.load() && (want == I18N_LANG_EN || g_trans.load() != nullptr)) {
        return;                       /* 未变化：不重复加载 */
    }
    I18n_SetMode(want);
    vbdmcp_logf(VBDMCP_LOG_INFO, "i18n: language=%s name='%s' (reg='%s', err=%ld, pack=%s)",
               I18n_LangCode(I18n_GetMode()), I18n_LangName(I18n_GetMode()),
               val, (long)st, g_source.c_str());
}

I18nMode I18n_GetMode(void)
{
    return g_mode.load();
}

int I18n_TranslatedCount(void)
{
    return (int)kh_size(g_trans.load());
}

/* 仅 UI 线程调用（g_source 是普通字符串，不参与跨线程发布）。 */
const char *I18n_LangPackSource(void)
{
    return g_source.c_str();
}

unsigned I18n_Generation(void)
{
    return g_generation.load();
}

const char *I18n_GetText(const char *msgid)
{
    if (!msgid || !*msgid) return msgid ? msgid : "";
    if (g_shutting_down.load()) return msgid;
    if (g_mode.load(std::memory_order_relaxed) != I18N_LANG_EN) {
        /* 命中即返回：空串是"故意留空"的合法译文，不能当成未翻译。
         * 未翻译的条目在 build_table 里就没有进哈希表（值为 null），落到下面回退。 */
        khash_t *table = g_trans.load(std::memory_order_acquire);
        if (table) {
            char *tr = (char *)kh_get(table, msgid);
            if (tr) return tr;
        }
    }
    return msgid;
}

const char *I18n_GetTextAnsi(const char *msgid)
{
    return to_ansi(I18n_GetText(msgid));
}

char *I18n_Format(const char *fmt, ...)
{
    char *out = g_pool.slot();
    if (!fmt) return out;

    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out, kFmtSlotSize, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= kFmtSlotSize) out[kFmtSlotSize - 1] = '\0';
    return out;
}

char *I18n_FormatAnsi(const char *fmt, ...)
{
    char utf8[kFmtSlotSize];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(utf8, sizeof(utf8), fmt, ap);
        va_end(ap);
        if (n < 0 || (size_t)n >= sizeof(utf8)) utf8[sizeof(utf8) - 1] = '\0';
    } else {
        utf8[0] = '\0';
    }
    char *out = g_pool.slot();
    const char *ansi = to_ansi(utf8);
    _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", ansi);
    return out;
}

/* DllMain(DLL_PROCESS_DETACH) 用：先让所有查表退化为返回 msgid，再释放 */
void I18n_PrepareShutdown(void)
{
    g_shutting_down.store(true);
    I18n_Clear();
}