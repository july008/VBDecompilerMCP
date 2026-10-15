/*
 * i18n.cpp - 轻量 gettext 风格国际化实现（C++17）
 * i18n.cpp - Lightweight gettext-style i18n implementation (C++17)
 *
 * 数据流：
 *   DLL 内置资源 RCDATA("JSON_<CODE>")  ==  lang_res/<code>.json（由 .rc 逐语言链入）
 *        │  FindResourceA / LoadResource / LockResource（只读，需拷出再解析）
 *        ▼
 *   yyjson 解析 {"msgid": "msgstr", ...}
 *        │  key/value 各自 strdup（所有权归哈希表，I18n_Clear 时统一 free）
 *        ▼
 *   khash（src/khash.h）哈希表  ──►  I18n_GetText()：命中返回译文，
 *                                          未命中或译文为空则回退 msgid
 *
 * Data flow:
 *   embedded resource RCDATA("JSON_<CODE>")  ==  lang_res/<code>.json (one per language, linked by the .rc)
 *        |  FindResourceA / LoadResource / LockResource (read-only, copy before parsing)
 *        v
 *   yyjson parses {"msgid": "msgstr", ...}
 *        |  key/value are strdup'ed (owned by the hash table, freed by I18n_Clear)
 *        v
 *   khash table (src/khash.h)  -->  I18n_GetText(): hit returns the translation,
 *                                   miss (or empty translation) falls back to the msgid
 *
 * 语言来源：注册表 HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\Options
 *          的 CustomLanguage，取值就是宿主里的语言名（"ChineseSimplified"、
 *          "French (native tongue v3)" 等，见 kLangs 表），原样匹配即可。
 *          除英文外 22 种语言都已嵌入语言包（英文无需包：msgid 就是英文）；
 *          缺包时 `_()` 返回 msgid（英文原文），不会显示空白。
 *
 * Language source: the CustomLanguage value under
 *           HKCU\...\VB Decompiler\Options. Its text is the language name used by
 *           the host ("ChineseSimplified", "French (native tongue v3)" ... see the
 *           kLangs table) and is matched verbatim. Every language except English
 *           has an embedded pack (English needs none - the msgid is English);
 *           without a pack `_()` returns the msgid, never blank text.
 *
 * 线程模型：查表是"发布后只读"的无锁设计 —— 写方是 UI 线程（换语言 / 清空），
 *         读方还包括 MCP 网络线程（mcp_http.cpp 在后台线程用 _F() 生成错误
 *         消息）。换语言时新建一张表替换全局指针，旧表挂进退休列表、直到
 *         I18n_Clear / DLL 卸载才释放，因此读者永远看到一张完整的表；
 *         `_A()`/`_AF()` 的轮转缓冲池是 thread_local。
 *
 * Threading: lookups use a lock-free "publish once, read-only afterwards" design.
 *           The UI thread writes (language switch / clear); readers include the
 *           MCP network thread (mcp_http.cpp builds error messages with _F() off
 *           the UI thread). A switch publishes a new table and retires the old
 *           one into a list freed only by I18n_Clear / DLL unload, so readers
 *           always see a complete table; the `_A()`/`_AF()` rotating buffers are
 *           thread_local.
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

/* 注册表路径：语言子键与取值名（VB Decompiler 自己写入）
 * Registry path: the language subkey and value name (written by VB Decompiler) */
#define I18N_LANG_REG_KEY   "SOFTWARE\\VB and VBA Program Settings\\VB Decompiler\\Options"
#define I18N_LANG_REG_VALUE "CustomLanguage"
/* 语言名 / 资源名的映射见下方 kLangs 表：注册表原文 ↔ 枚举 ↔ RCDATA 名。
 * 语言包只从资源加载，**不做磁盘回退**：资源缺失就回英文 msgid（见 I18n_SetMode）。
 * The language name / resource mapping lives in the kLangs table below: registry
 * text <-> enum <-> RCDATA name. Packs load from resources only, with NO disk
 * fallback: a missing resource means English msgids (see I18n_SetMode). */

namespace {

/* ---------------- 全局状态 / Global state ----------------
 * g_mode / g_trans 用原子发布：写方是 UI 线程（换语言 / 清空），读方除 UI 线程
 * 外还包括 MCP 网络线程（见文件头线程模型）。表本身"发布后不再改动"——旧表只
 * 退休不释放，因此读者拿到的永远是一张完整的表，无需加锁。
 * g_json / g_source 仅 UI 线程访问（外部无调用点），保持普通变量。
 *
 * g_mode / g_trans are published atomically: the UI thread writes (language
 * switch / clear) while readers include the MCP network thread (see the file
 * header). A published table is immutable - retired tables are not freed - so
 * readers always see a complete table without locking. g_json / g_source are
 * touched by the UI thread only and stay plain variables. */
std::atomic<bool>   g_shutting_down{ false };
std::atomic<unsigned> g_generation{ 1 };   /* 表世代号：每次重建/清空自增 / table generation, bumped on rebuild/clear */
std::atomic<I18nMode> g_mode{ I18N_MODE_EN };
std::atomic<khash_t*> g_trans{ nullptr };
yyjson_doc         *g_json = nullptr;
std::string         g_source = "none";

/* ---------------- 轮转缓冲池（ANSI / 格式化结果） ----------------
 * Win32 A 系列接口与日志都是"立即消费"语义，用轮转缓冲避免嵌套调用互相覆盖。
 * 16 槽 × 4KB = 64KB 常驻，足够覆盖一帧内所有控件创建。
 *
 * Rotating buffers (ANSI / formatted results).
 * Win32 A APIs and logging consume the text immediately; rotation keeps nested
 * calls from overwriting each other. 16 slots x 4KB = 64KB resident, enough for
 * every control created within one frame. */
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
 * （若用共享池，多线程同时格式化会互相覆盖，拿到别人的半截字符串）。
 * thread_local: one pool per thread makes _A()/_AF() thread-safe (a shared pool
 * would let concurrent formatting overwrite each other's partial strings). */
thread_local FmtPool g_pool;

/* UTF-8 → ANSI(GBK)：ASCII 快速路径直接复用输入指针。
 * lossy 非空时回填"是否有字符在当前 ANSI 代码页里无法表示"——调用方据此回退
 * 英文（见 I18n_GetTextAnsi），避免把整串 '?' 交给宿主界面。
 *
 * UTF-8 -> ANSI(GBK); the ASCII fast path reuses the input pointer directly.
 * When lossy is non-NULL it reports whether some character is not representable
 * in the current ANSI code page, so callers can fall back to English (see
 * I18n_GetTextAnsi) instead of handing a wall of '?' to a host surface. */
const char *to_ansi(const char *utf8, bool *lossy = nullptr)
{
    if (lossy) *lossy = false;
    if (!utf8) return "";
    bool ascii = true;
    for (const unsigned char *p = (const unsigned char *)utf8; *p; ++p) {
        if (*p >= 0x80) { ascii = false; break; }
    }
    if (ascii) return utf8;

    char *out = g_pool.slot();
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen <= 0) {
        /* 转换失败：截断复制，至少不崩（用户会看到部分乱码）
         * Conversion failed: copy truncated - never crash (text may be garbled). */
        _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", utf8);
        return out;
    }
    std::wstring wbuf;
    wbuf.resize((size_t)wlen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wbuf[0], wlen) <= 0) {
        _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", utf8);
        return out;
    }
    /* lpUsedDefaultChar 会告诉我们是否有字符被替换成了 '?'（CP936 遇到波斯语
     * 就是这种情况）——只有真正的"表示不了"才算 lossy。
     *
     * lpUsedDefaultChar tells us whether any character was replaced by '?', which
     * is exactly what happens to Persian on CP936; only then is it lossy. */
    BOOL used_default = FALSE;
    const int glen = WideCharToMultiByte(CP_ACP, 0, &wbuf[0], wlen - 1, out,
                                         (int)kFmtSlotSize - 1, nullptr,
                                         lossy ? &used_default : nullptr);
    if (glen <= 0) {
        if (lossy) *lossy = true;          /* 转换失败同样是"表示不了" / also lossy */
        _snprintf_s(out, kFmtSlotSize, _TRUNCATE, "%s", utf8);
        return out;
    }
    if (lossy) *lossy = (used_default != FALSE);
    out[glen] = '\0';
    return out;
}

/* ---------------- 语言包解析 / Pack parsing ---------------- */

/* kh_foreach 回调：释放一对 strdup 出来的 key/value（哈希表本身不拥有内存）
 * kh_foreach callback: frees one strdup'ed key/value pair (the table does not
 * own that memory). */
static int g_freed_pairs = 0;
static void free_pair(const char *key, void *val, void *user)
{
    vbdmcp_free((void *)(size_t)key);
    vbdmcp_free(val);            /* vbdmcp_free(NULL) 安全：被拒绝的译文存 NULL
                                    vbdmcp_free(NULL) is safe: rejected entries store NULL */
    if (user) *((int *)user) += 1;
}

/* ============================================================
 * 表的生命周期：发布后**不再改动**，读操作因此是纯只读、无锁。
 *
 * 换语言时不在原地 free+重建（旧表可能正被网络线程读取 → use-after-free），
 * 而是：新建表 → 换上全局指针 → 旧表挂进 g_retired，**停机时统一释放**。
 * 换语言是低频操作（用户改宿主语言设置），滞留几张表（每张几十 KB）
 * 的内存代价可忽略，换来的是任何线程都能直接 `_()`。
 *
 * Table lifecycle: published tables are immutable, so reads are pure and lock-free.
 *
 * A language switch never frees/rebuilds in place (the network thread might be
 * reading the old table -> use-after-free). Instead: build a new table, swap the
 * global pointer, and move the old table into g_retired for release at shutdown.
 * Language switches are rare (the user changes the host language), and a few
 * retired tables (tens of KB each) are a negligible price for lock-free `_()`
 * from any thread.
 * ============================================================ */
static std::mutex             g_table_mu;
static std::vector<khash_t*>  g_retired_maps;
static std::vector<yyjson_doc*> g_retired_docs;

void clear_silent()
{
    /* 只下线当前表，不释放：可能还有线程正在读它。真正释放见 I18n_Clear。
     * Unpublish only, never free here: a thread may still be reading the table.
     * The actual release happens in I18n_Clear. */
    if (khash_t *t = g_trans.exchange(nullptr, std::memory_order_acq_rel)) {
        g_retired_maps.push_back(t);
    }
    if (g_json)  { g_retired_docs.push_back(g_json);  g_json  = nullptr; }
    g_source = "none";
    g_generation.fetch_add(1);   /* 表换代：调用方据此刷新自己缓存的译文 / new generation for callers caching translations */
}

/* 停机/卸载：此时确定没有读者了，把历任表一次性释放干净
 * Shutdown / unload: no readers remain, so release every retired table at once. */
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
 *
 * printf format string analysis. Produces:
 *   sig       normalized placeholder sequence (e.g. "%d%s%zu") used to compare a
 *             translation against its original;
 *   convs     conversion count;
 *   args      actual argument count (conversions + one per '*' width/precision);
 *   parseable=false when the string holds a literal '%' (e.g. the help text
 *             "%TEMP%\\vbdmcp.log"); placeholder checks are then skipped.
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
        if (*p == '%') continue;                          /* 字面量 %% / literal %% */

        /* Windows 环境变量写法（%APPDATA%、%USERPROFILE%）不是 printf 占位符：
         * 整段跳过。否则会先被当成转换符 "%A"，而结尾那个 % 后面是空格/行尾 →
         * 判成"不可解析"，译文随即被 I18n_FormatCompatible() 拒绝并整条丢弃。
         * 实际后果：接入向导里含环境变量的 Trae / WorkBuddy 步骤在**所有语言**
         * 里都回退成英文（日志里两条 "placeholder mismatch ... guide:trae/steps"
         * 与 guide:workbuddy/steps 就是这么来的）。
         *
         * Windows environment-variable syntax (%APPDATA%, %USERPROFILE%) is not a
         * printf placeholder: skip the whole token. Otherwise it is read as the
         * conversion "%A" and the trailing % (followed by a space or end of
         * string) makes the string unparseable, so I18n_FormatCompatible()
         * rejects and drops the translation. The visible effect: the Trae and
         * WorkBuddy onboarding steps - the ones that mention environment
         * variables - fell back to English in *every* language (the two
         * "placeholder mismatch ... guide:trae/steps" log lines). */
        {
            const char *q = p;                            /* p 已在 % 之后 */
            if (*q && (isalpha((unsigned char)*q) || *q == '_')) {
                ++q;
                while (*q && (isalnum((unsigned char)*q) || *q == '_')) ++q;
                if (*q == '%') { p = q + 1; continue; }   /* 环境变量：整段跳过 */
            }
        }

        int stars = 0;      /* '*' 从实参列表取值：%*.*f 共吃 3 个实参
                               '*' consumes an argument: %*.*f takes 3 in total */
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

/* 把 yyjson 顶层对象灌进哈希表（key/value 均 strdup）
 * Fills the hash table from the yyjson top-level object (key/value are strdup'ed). */
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
         * 错参/崩溃，这种"留空"几乎一定是误操作，回退原文更安全。
         *
         * Value semantics (matching the tools/extract_i18n.py convention):
         *   null / key missing -> untranslated, fall back to the English msgid
         *   ""                 -> INTENTIONALLY empty, return "" as-is (this is
         *                         NOT "untranslated")
         *   non-empty with matching placeholders -> translation
         * When the msgid has placeholders, "" is rejected: an empty translation
         * would drop arguments and corrupt or crash vsnprintf, so it is almost
         * certainly a mistake and the original is used instead. */
        if (!yyjson_is_str(v)) continue;   /* null / 数字 / 数组等：视为未翻译
                                              null / number / array ...: untranslated */

        const char *k_text = yyjson_get_str(k);
        const char *v_text = yyjson_get_str(v);

        char *val = nullptr;
        if (I18n_FormatCompatible(k_text, v_text)) {
            val = vbdmcp_strdup(v_text);      /* 含 ""：故意留空 / including "": intentionally empty */
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
            continue;                       /* key 都没建成功，整条丢弃 / key failed: drop the entry */
        }

        int ret = 0;
        kh_put(table, key, val, &ret);
        if (ret == 0) {          /* 极端情况：插入失败，避免泄漏 / insert failed: avoid the leak */
            vbdmcp_free(key);
            vbdmcp_free(val);
        }
    }
    /* 表建好后再原子发布：读者要么看到旧表，要么看到完整新表。
     * Publish atomically only after the table is complete: readers see either the
     * old table or the complete new one. */
    g_trans.store(table, std::memory_order_release);
    return true;
}

/* 加载某个语言的语言包：资源名由 I18n_LangResourceName() 给（没有则 false）
 * Loads the pack of one language; the resource name comes from
 * I18n_LangResourceName() (returns false when there is none). */
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

    /* yyjson 不可直接解析只读资源内存（原地解析会写入），先拷一份
     * yyjson cannot parse the read-only resource memory in place (it writes into
     * the buffer); copy it first. */
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
 * registry_name 是 VB Decompiler 注册表 CustomLanguage 的原文（宿主写什么
 * 我们就认什么），resource 是 RCDATA 资源名。除英文外全部有语言包；英文故意
 * 留空（msgid 即英文，无需资源）。新增语言：加一行 + 一个 lang_res/<code>.json
 * + rc 里一条 RCDATA，三者缺一不可（缺资源时该语言会回退英文并记日志）。
 *
 * Language name <-> enum.
 *
 * registry_name is the verbatim CustomLanguage text (match whatever the host
 * writes); resource is the RCDATA name. Every language except English has a
 * pack; English is left empty on purpose (the msgid is already English). Adding
 * a language takes one row here, one lang_res/<code>.json and one RCDATA entry
 * in the .rc - all three are required (a missing resource falls back to English
 * and logs a warning).
 * ============================================================ */
struct LangEntry {
    I18nLang     lang;
    const char  *registry_name;   /* CustomLanguage 的值 / CustomLanguage value */
    const char  *alt_name;       /* 宿主里的别名（可空）/ alternate host name (may be NULL) */
    const char  *code;           /* 两字母/带地区代码，用于日志与 VBDMCP_LANG
                                    short/region code for logs and VBDMCP_LANG */
    const char  *resource;       /* RCDATA 资源名；NULL = 暂无语言包
                                    RCDATA name; NULL = no pack yet */
};

static const LangEntry kLangs[] = {
    /* 英文故意不设包：msgid 本身就是英文，_() 直接返回 msgid，无需资源。
     * English intentionally has no pack: the msgid is already English, so _()
     * returns it as-is and no resource is needed. */
    { I18N_LANG_EN,              "English",              nullptr,                          "en",    nullptr     },
    { I18N_LANG_AR,              "Arabic",               nullptr,                          "ar",    "JSON_AR"   },
    { I18N_LANG_PT_BR,           "Brazilian Portuguese", nullptr,                          "pt-BR", "JSON_PT_BR"},
    { I18N_LANG_ZH_SIMPLIFIED,   "ChineseSimplified",    nullptr,                          "zh",    "JSON_ZH"   },
    { I18N_LANG_ZH_TRADITIONAL,  "ChineseTraditional",   nullptr,                          "zh-TW", "JSON_ZH_TW"},
    { I18N_LANG_CS,              "Czech",                nullptr,                          "cs",    "JSON_CS"   },
    { I18N_LANG_DA,              "Danish",               nullptr,                          "da",    "JSON_DA"   },
    { I18N_LANG_NL,              "Dutch",                nullptr,                          "nl",    "JSON_NL"   },
    /* 宿主里有 "French" 和 "French (native tongue v3)" 两个条目，都归到法语
     * the host lists both "French" and "French (native tongue v3)"; both map to French */
    { I18N_LANG_FR,              "French",               "French (native tongue v3)",      "fr",    "JSON_FR"   },
    { I18N_LANG_DE,              "German",               nullptr,                          "de",    "JSON_DE"   },
    { I18N_LANG_EL,              "Greek",                nullptr,                          "el",    "JSON_EL"   },
    { I18N_LANG_HE,              "Hebrew",               nullptr,                          "he",    "JSON_HE"   },
    { I18N_LANG_ID,              "Indonesian",           nullptr,                          "id",    "JSON_ID"   },
    { I18N_LANG_IT,              "Italian",              nullptr,                          "it",    "JSON_IT"   },
    { I18N_LANG_JA,              "Japanese",             nullptr,                          "ja",    "JSON_JA"   },
    { I18N_LANG_KU,              "Kurdish (sorani)",     "Kurdish",                        "ku",    "JSON_KU"   },
    { I18N_LANG_FA,              "Persian",              nullptr,                          "fa",    "JSON_FA"   },
    { I18N_LANG_PL,              "Polish",               nullptr,                          "pl",    "JSON_PL"   },
    { I18N_LANG_PT,              "Portuguese",           nullptr,                          "pt",    "JSON_PT"   },
    { I18N_LANG_RU,              "Russian",              nullptr,                          "ru",    "JSON_RU"   },
    { I18N_LANG_SK,              "Slovak",               nullptr,                          "sk",    "JSON_SK"   },
    { I18N_LANG_ES,              "Spanish",              nullptr,                          "es",    "JSON_ES"   },
    { I18N_LANG_TR,              "Turkish",              nullptr,                          "tr",    "JSON_TR"   },
    { I18N_LANG_UK,              "Ukrainian",            nullptr,                          "uk",    "JSON_UK"   },
};

static const LangEntry *lang_entry(I18nLang lang)
{
    const int n = (int)(sizeof(kLangs) / sizeof(kLangs[0]));
    for (int i = 0; i < n; i++) {
        if (kLangs[i].lang == lang) return &kLangs[i];
    }
    return &kLangs[0];   /* 未知语言 → 英文 / unknown language -> English */
}

/* 去掉两端空白（注册表值可能带空格）
 * Trims surrounding whitespace (registry values may be padded). */
static void trim_inplace(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

} // namespace

/* ============================================================
 * UTF-8 → UTF-16：本插件 Unicode 窗口的底座（_W()/_FW()）
 *
 * 与 to_ansi 同一套契约：
 *   - thread_local 轮转池，跨语句长期持有会被后续调用覆盖；
 *   - 转换失败不崩：退化成"逐字节加宽"（画面难看，但绝不解引用坏指针）。
 * 这两个函数必须是**外部链接**（i18n.h 里是 extern "C" 声明），
 * 所以只能放在上面的匿名命名空间之外。
 *
 * UTF-8 -> UTF-16, the base of _W()/_FW() for the plugin's own Unicode windows.
 * Same contract as to_ansi (a thread_local rotating pool - do not hold the
 * pointer across statements; a never-crash fallback widens byte by byte when the
 * input is not valid UTF-8). Both definitions need external linkage (i18n.h
 * declares them inside extern "C"), so they must live outside the anonymous
 * namespace above.
 * ============================================================ */
namespace {
struct WidePool {
    static const int kSlots = 8;
    int next = 0;
    std::wstring slots[kSlots];

    std::wstring &slot()
    {
        std::wstring &s = slots[next];
        next = (next + 1) % kSlots;
        s.clear();
        return s;
    }
};
thread_local WidePool g_wide_pool;
}   /* namespace */

const wchar_t *I18n_ToWide(const char *utf8)
{
    if (!utf8) utf8 = "";
    std::wstring &out = g_wide_pool.slot();

    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wlen > 0) {
        out.resize((size_t)wlen);
        if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], wlen) > 0) {
            out.resize((size_t)wlen - 1);      /* 去掉结尾 NUL；c_str() 会补 */
            return out.c_str();
        }
        out.clear();
    }
    for (const unsigned char *p = (const unsigned char *)utf8; *p; ++p) {
        out.push_back((wchar_t)*p);
    }
    return out.c_str();
}

/* UTF-16 → UTF-8：返回 vbdmcp_alloc 的新串（调用方 vbdmcp_free）
 * UTF-16 -> UTF-8: returns a new vbdmcp_alloc'ed string (caller frees it). */
char *I18n_WideToUtf8(const wchar_t *w)
{
    if (!w) return vbdmcp_strdup("");
    const int len = (int)wcslen(w);
    if (len <= 0) return vbdmcp_strdup("");

    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return vbdmcp_strdup("");

    char *out = (char *)vbdmcp_alloc((size_t)n + 1);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, len, out, n, nullptr, nullptr);
    out[n] = '\0';
    return out;
}

/* ============================================================
 * 语言名 <-> 枚举（对外接口，需与 i18n.h 的 extern "C" 声明一致，
 * 所以必须放在匿名命名空间之外）
 *
 * Language name <-> enum (public API: must live outside the anonymous namespace
 * to match the extern "C" declarations in i18n.h).
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
    /* 也接受两字母代码（VBDMCP_LANG=ja / pt-BR 这种写法）
     * Two-letter codes are accepted too (VBDMCP_LANG=ja / pt-BR). */
    for (int i = 0; i < n; i++) {
        if (_stricmp(buf, kLangs[i].code) == 0) return kLangs[i].lang;
    }
    return I18N_LANG_EN;   /* 未知 → 英文（_() 返回 msgid）/ unknown -> English (_() returns the msgid) */
}

/* ============================================================
 * 对外接口 / Public API
 * ============================================================ */

/* 纯拷贝（不做格式化）：轮转缓冲，立即使用
 * Plain copy (no formatting): rotating buffer, use immediately. */
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
 * 供头文件里的 _F()/_AF() 变参模板做"占位符 vs 实参"核对。
 *
 * How many arguments the format string needs:
 *   >0  = N arguments (conversions + '*' width/precision specs)
 *   0   = no placeholder (or a literal %, unparseable) -> no formatting needed
 * Used by the _F()/_AF() templates in the header to check placeholders vs args. */
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
    if (!extract_placeholders(msgid, &sig_a)) return 1;      /* 原文含字面量 %：不校验 / literal % in the original: unverifiable */
    if (!extract_placeholders(translated, &sig_b)) return 0; /* 译文里出现非法 %：不可用 / illegal % in the translation: unusable */
    return sig_a == sig_b;
}

void I18n_Clear()
{
    /* 停机/卸载：确认没有读者了，把历任表一次性释放干净
     * Shutdown / unload: no readers remain, release every retired table at once. */
    std::lock_guard<std::mutex> lock(g_table_mu);
    release_all_tables();
    g_mode.store(I18N_LANG_EN);
}

int I18n_SetMode(I18nMode mode)
{
    /* 写路径串行：建表 + 发布期间不与另一次换语言交叉
     * Serialize the write path: building + publishing never interleaves with
     * another language switch. */
    std::lock_guard<std::mutex> lock(g_table_mu);
    clear_silent();
    g_mode.store(mode);

    if (mode == I18N_LANG_EN) return 1;   /* 英文：_() 原样返回 msgid / English: _() returns the msgid */

    const char *rsrc = I18n_LangResourceName(mode);
    if (rsrc && *rsrc && load_from_resource(rsrc)) {
        vbdmcp_logf(VBDMCP_LOG_INFO, "i18n: %s pack loaded from %s (%d entries)",
                   I18n_LangCode(mode), g_source.c_str(), I18n_TranslatedCount());
        return 1;
    }

    /* 该语言没有语言包（或资源缺失）→ 回英文：_(msgid) 直接返回 msgid 本身
     * （msgid 就是英文原文）。不做磁盘回退 —— 否则开发目录里的 lang_res\zh.json
     * 会掩盖"RCDATA 没链进 DLL"这种打包问题。
     *
     * No pack for this language (or the resource is missing) -> English: _(msgid)
     * returns the msgid itself (the English original). No disk fallback here: a
     * lang_res\zh.json in the dev tree would otherwise hide packaging bugs where
     * the RCDATA never made it into the DLL. */
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
     * 取值可以是注册表里的原文（如 "ChineseSimplified"）或两字母代码（zh/ja/pt-BR）。
     *
     * Debug/test override: the VBDMCP_LANG environment variable wins over the
     * registry CustomLanguage value. It accepts the registry text (e.g.
     * "ChineseSimplified") or a short code (zh / ja / pt-BR). */
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
    /* 未知/缺失的值一律当英文
     * Unknown / missing values count as English. */
    const I18nLang want = (st == ERROR_SUCCESS)
                              ? I18n_LangFromName(val) : I18N_LANG_EN;
    if (want == g_mode.load() && (want == I18N_LANG_EN || g_trans.load() != nullptr)) {
        return;                       /* 未变化：不重复加载 / unchanged: skip reload */
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

/* 仅 UI 线程调用（g_source 是普通字符串，不参与跨线程发布）。
 * UI thread only (g_source is a plain string, not published across threads). */
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
         * 未翻译的条目在 build_table 里就没有进哈希表（值为 null），落到下面回退。
         *
         * A hit returns immediately: an empty string is a legitimate
         * "intentionally empty" translation, not "untranslated". Untranslated
         * entries never enter the table (stored as null in build_table) and fall
         * through to the fallback below. */
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
    bool lossy = false;
    const char *ansi = to_ansi(I18n_GetText(msgid), &lossy);
    if (!lossy) return ansi;

    /* 译文含当前 ANSI 代码页表示不了的字符（如中文系统 + 波斯语）：宿主 A 系列
     * 接口只会显示一串 '?'，比英文原文更难读 —— 回退英文 msgid（纯 ASCII）。
     * 这些语言要有母语界面就必须走 W 系列（见 i18n.h 的文本边界）。
     *
     * The translation contains characters the current ANSI code page cannot
     * represent (Persian on a CP936 machine, say): the host's A-API surfaces would
     * show a wall of '?', which reads worse than English - so fall back to the
     * English msgid (pure ASCII). A native-language UI for such languages needs the
     * W family (see the text-boundary notes in i18n.h). */
    return to_ansi(msgid);
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

/* DllMain(DLL_PROCESS_DETACH) 用：先让所有查表退化为返回 msgid，再释放
 * For DllMain(DLL_PROCESS_DETACH): make every lookup return the msgid first, then release. */
void I18n_PrepareShutdown(void)
{
    g_shutting_down.store(true);
    I18n_Clear();
}
