/*
 * i18n.h - 轻量 gettext 风格国际化（语言包来自 DLL 内置资源）
 *
 * 设计目标（与项目约定一致）：
 *   1. 源码里所有展示字符串统一写成 _("msgid")，msgid 用英文原文；
 *      中文译文放在 lang_res/zh.json，随 DLL 资源嵌入。
 *   2. 不加载外部 langpack.dll：zh.json 通过 .rc 以 RCDATA("JSON_ZH") 链进
 *      vbdmcp.dll，运行时用 GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
 *      传入本模块函数地址取得"当前 DLL 自身模块句柄"，再 FindResource/LoadResource。
 *   3. 字符串查找走 khash（src/khash.h）哈希表；I18n_Clear() 释放表内 strdup
 *      的字符串，避免内存泄漏（DLL 卸载前会调用一次）。
 *
 * 用法：
 *      I18n_Refresh();                     // 按注册表 CustomLanguage 选语言
 *      puts(_("VB Decompiler MCP Server"));
 *      puts(_F("Version: %s", "2.0.0"));
 *      SetDlgItemTextA(hDlg, IDC_OK, _A("OK"));   // ANSI 控件用 GBK 字节
 *
 * 约定说明：
 *   - `_()`  返回 **UTF-8**，用于 MCP JSON 报文、日志等非 ANSI 场景；
 *   - `_A()` 是 `_()` 的 ANSI(GBK) 版本，用于 MessageBoxA / SetDlgItemTextA /
 *     CreateWindowExA 等 Win32 A 系列接口（与旧 vbdmcp_t() 语义一致）；
 *   - `_F()`  = 先翻译再格式化（UTF-8）；`_AF()` = 先翻译再格式化（GBK）。
 *     临时结果放在轮转缓冲池中，**只能立即使用**，不要跨语句长期持有。
 *   - 工具名（vbd_read_* / vbd_write_*）是协议标识符，禁止翻译。
 *
 * 语言包取值的语义（与 tools/extract_i18n.py 一并维护，改动请同时改两边）：
 *      键缺失 或 值为 null  → 未翻译，`_()` 返回 msgid 本身（英文原文）
 *      值是 ""              → **故意留空**，`_()` 返回空串（不是未翻译！）；
 *                             仅当 msgid 本身没有 % 占位符时才允许，否则视为误操作
 *                             并回退原文（留空会丢参数导致错参/崩溃）
 *      非空文本             → 译文；占位符与 msgid 不一致时整条拒绝并回退原文
 */
#ifndef VBDMCP_I18N_H
#define VBDMCP_I18N_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 语言枚举
 *
 * 取值与 VB Decompiler 注册表
 *   HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\Options
 *     \CustomLanguage
 * 里的字符串**一一对应**（见 I18n_LangName()）。宿主把用户选的语言写进
 * 那个值，我们原样匹配即可，不需要再维护映射表。
 *
 * 目前只有简体中文嵌了语言包（JSON_ZH）；其余语言先占位，
 * 没有语言包时 `_()` 直接返回 msgid（即英文原文），不会显示空白。
 * 将来新增语言只需：加一个 lang_res/<xx>.json + 在 res/vbdmcp.rc 里加一条
 * RCDATA + 在 I18n_LangResourceName() 里补一个名字。
 * ============================================================ */
typedef enum {
    I18N_LANG_EN = 0,             /* English            English */
    I18N_LANG_AR,                 /* Arabic             Arabic */
    I18N_LANG_PT_BR,              /* Brazilian Portuguese Brazilian Portuguese */
    I18N_LANG_ZH_SIMPLIFIED,      /* ChineseSimplified  ChineseSimplified */
    I18N_LANG_ZH_TRADITIONAL,     /* ChineseTraditional ChineseTraditional */
    I18N_LANG_CS,                 /* Czech              Czech */
    I18N_LANG_DA,                 /* Danish             Danish */
    I18N_LANG_NL,                 /* Dutch              Dutch */
    I18N_LANG_FR,                 /* French             French / French (native tongue v3) */
    I18N_LANG_DE,                 /* German             German */
    I18N_LANG_EL,                 /* Greek              Greek */
    I18N_LANG_HE,                 /* Hebrew             Hebrew */
    I18N_LANG_ID,                 /* Indonesian         Indonesian */
    I18N_LANG_IT,                 /* Italian            Italian */
    I18N_LANG_JA,                 /* Japanese           Japanese */
    I18N_LANG_KU,                 /* Kurdish (sorani)   Kurdish (sorani) */
    I18N_LANG_FA,                 /* Persian            Persian */
    I18N_LANG_PL,                 /* Polish             Polish */
    I18N_LANG_PT,                 /* Portuguese         Portuguese */
    I18N_LANG_RU,                 /* Russian            Russian */
    I18N_LANG_SK,                 /* Slovak             Slovak */
    I18N_LANG_ES,                 /* Spanish            Spanish */
    I18N_LANG_TR,                 /* Turkish            Turkish */
    I18N_LANG_UK,                 /* Ukrainian          Ukrainian */
    I18N_LANG_COUNT
} I18nLang;

/* 兼容旧接口名：I18nMode 就是 I18nLang，EN/ZH 取值不变 */
typedef I18nLang I18nMode;
#define I18N_MODE_EN I18N_LANG_EN
#define I18N_MODE_ZH I18N_LANG_ZH_SIMPLIFIED

/* ---------------- 语言名 <-> 枚举 ---------------- */

/* 注册表 CustomLanguage 的字符串 → 枚举；未知/缺失 → I18N_LANG_EN。
 * 大小写不敏感，两端空白忽略；也接受 "zh"/"ja" 这类两字母代码。 */
I18nLang    I18n_LangFromName(const char *name);

/* 枚举 → 注册表里使用的字符串（未在列表里的语言回退成 "English"） */
const char *I18n_LangName(I18nLang lang);

/* 枚举 → 两字母代码（日志 / VBDMCP_LANG 环境变量用，如 zh、ja、pt-BR） */
const char *I18n_LangCode(I18nLang lang);

/* 该语言是否已嵌入语言包；目前只有 zh-CN 为真 */
int         I18n_LangHasPack(I18nLang lang);

/* 枚举 → RCDATA 资源名（未嵌入的语言返回 NULL） */
const char *I18n_LangResourceName(I18nLang lang);

/* ---------------- 生命周期 ---------------- */

/* 按注册表 ...\VB Decompiler\Options\CustomLanguage 选择语言
 * （也接受环境变量 VBDMCP_LANG 覆盖，取值同上）。
 * 每次调用都会重载语言包（设置窗体打开前用户可能改过语言）。 */
void I18n_Refresh(void);

/* 显式设置语言；从 DLL 内置资源加载该语言的语言包。
 * 返回 true 表示该语言有可用的翻译表；英文恒为 true；
 * 没有语言包的语言会退回英文（`_()` 返回 msgid）并返回 0。 */
int  I18n_SetMode(I18nMode mode);

I18nMode I18n_GetMode(void);

/* 释放翻译表（含 strdup 的 key/value 与 yyjson 文档）。
 * 释放后 `_()` 退化为返回 msgid。可在 DLL 卸载前调用一次。 */
void I18n_Clear(void);

/* DLL 卸载专用：先关闭查表（其它线程的 `_()` 直接返回 msgid），再释放内存。
 * 必须在 DllMain(DLL_PROCESS_DETACH) 里调用。 */
void I18n_PrepareShutdown(void);

/* ---------------- 取文本 ---------------- */

/* UTF-8（无翻译时返回 msgid 本身，生命周期为静态字符串） */
const char *I18n_GetText(const char *msgid);

/* ANSI / GBK（供 Win32 A 系列接口；返回轮转缓冲，立即使用） */
const char *I18n_GetTextAnsi(const char *msgid);

/* 先翻译再格式化（UTF-8）；fmt 必须是已翻译的格式串 */
char *I18n_Format(const char *fmt, ...);

/* 先翻译再格式化（GBK）；用于 MessageBoxA 等 */
char *I18n_FormatAnsi(const char *fmt, ...);

/* **不做任何格式化**的拷贝（轮转缓冲，立即使用）。
 * _F()/_AF() 在"占位符与实参个数不一致"时走这里：宁可原样显示 "%s"，
 * 也不能让 vsnprintf 从栈上取垃圾指针。 */
char *I18n_CopyText(const char *utf8);
char *I18n_CopyTextAnsi(const char *utf8);

/* ---------------- 诊断 ---------------- */

/* 占位符兼容性校验：译文与原文的 printf 占位符序列是否一致。
 *   1 = 一致（或原文含字面量 % 无法校验）
 *   0 = 不一致 —— 该译文不可用，i18n 会自动回退英文原文
 * 说明：译文被人工编辑时，丢失/改动占位符是最高发的事故
 *      （"%d" 写成 "%s" 会按错误类型取参，轻则错乱重则崩溃）。 */
int I18n_FormatCompatible(const char *msgid, const char *translated);

/* 该格式串需要多少个实参（0 = 无占位符/含字面量 %）。
 * 供下面的 _F()/_AF() 模板核对实参个数，避免 printf 系函数缺参时崩溃。 */
int I18n_FormatArity(const char *fmt);

/* _F()/_AF() 发现"占位符个数 ≠ 实参个数"时调用它记日志并安全退化。 */
void I18n_LogFormatMismatch(const char *fmt, int need, int got);

/* 当前语言包中的词条数（EN 模式恒为 0） */
int I18n_TranslatedCount(void);

/* 语言包来源描述：resource:JSON_ZH / file:<path> / none（供 /health 与日志） */
const char *I18n_LangPackSource(void);

/* 语言包“世代号”：每次翻译表被重建/清空（I18n_Refresh / I18n_SetMode /
 * I18n_Clear）都会自增。仅供“想把译文缓存到自己缓冲区”的调用方判断
 * 是否过期。
 *
 * 线程安全：`_()` / `_A()` / `_F()` / `_AF()` 在**任何线程**都能调用（含 MCP 网络线程）。
 * 做法是“表发布后不再改动”：换语言时新建一张表替换全局指针，旧表挂进退休列表、
 * 直到 I18n_Clear / DLL 卸载才释放，因此查表始终是纯只读、无需加锁；`_A()`/`_AF()` 的轮转缓冲池是
 * thread_local。表的内容替换对读者而言只是“读到旧值或新值”，不会看到半张表。 */
unsigned I18n_Generation(void);

/* ---------------- 宏（gettext 风格） ---------------- */

#define _(msgid)        I18n_GetText(msgid)
#define N_(msgid)        (msgid)                 /* 仅供提取脚本扫描的静态标记 */
#define _A(msgid)       I18n_GetTextAnsi(msgid)

/* _F / _AF：先翻译再格式化，支持全部 printf 占位符（%s %d %zu %.2f %% ...）。
 * 走下面的变参模板，会自动核对"占位符所需实参个数 == 实际传入个数"：
 * 不一致时记 ERROR 日志并原样返回格式串（安全退化，不会因缺参而崩溃）。 */
#define _F(msgid, ...)   I18n_FormatChecked(_(msgid), ##__VA_ARGS__)
#define _AF(msgid, ...)  I18n_FormatCheckedAnsi(_(msgid), ##__VA_ARGS__)

#ifdef __cplusplus
}   /* extern "C" */

/* ------------------------------------------------------------------
 * _F() / _AF() 的实现（C++ 变参模板）
 *
 * 为什么要模板：printf 系函数在运行时无法知道调用方传了几个实参。
 * 如果格式串里有 "%s" 而调用方忘了传参，vsnprintf 会从栈上取垃圾指针，
 * 直接造成访问冲突。这里在格式化前先核对"占位符所需实参个数 == 实参个数"：
 *   - 不一致 → 记 ERROR 日志并原样返回格式串（安全退化，绝不崩溃）；
 *   - 无占位符 → 直接拷贝，省一次 vsnprintf；
 *   - 一致   → 交给 C 风格的 I18n_Format / I18n_FormatAnsi 真正格式化。
 * ------------------------------------------------------------------ */
template <typename... Args>
char *I18n_FormatChecked(const char *fmt, Args... args)
{
    const int need = I18n_FormatArity(fmt);
    /* 无占位符或实参个数对不上：一律走"纯拷贝"，绝不进 vsnprintf */
    if (need <= 0) return I18n_CopyText(fmt);
    if (need != (int)sizeof...(Args)) {
        I18n_LogFormatMismatch(fmt, need, (int)sizeof...(Args));
        return I18n_CopyText(fmt);        /* 安全退化：原样返回格式串 */
    }
    return I18n_Format(fmt, args...);
}

template <typename... Args>
char *I18n_FormatCheckedAnsi(const char *fmt, Args... args)
{
    const int need = I18n_FormatArity(fmt);
    if (need <= 0) return I18n_CopyTextAnsi(fmt);
    if (need != (int)sizeof...(Args)) {
        I18n_LogFormatMismatch(fmt, need, (int)sizeof...(Args));
        return I18n_CopyTextAnsi(fmt);    /* 安全退化：原样返回格式串 */
    }
    return I18n_FormatAnsi(fmt, args...);
}
#endif

#endif /* VBDMCP_I18N_H */