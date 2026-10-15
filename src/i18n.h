/*
 * i18n.h - 轻量 gettext 风格国际化（语言包来自 DLL 内置资源）
 * i18n.h - Lightweight gettext-style i18n (language packs live in DLL resources)
 *
 * 设计目标：
 *   1. 源码里所有展示字符串统一写成 _("msgid")，msgid 用英文原文；
 *      中文译文放在 lang_res/zh.json，随 DLL 资源嵌入。
 *   2. 不加载外部 langpack.dll：zh.json 通过 .rc 以 RCDATA("JSON_ZH") 链进
 *      vbdmcp.dll，运行时用 GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
 *      传入本模块函数地址取得"当前 DLL 自身模块句柄"，再 FindResource/LoadResource。
 *   3. 字符串查找走 khash（src/khash.h）哈希表；I18n_Clear() 释放表内 strdup
 *      的字符串，避免内存泄漏（DLL 卸载前会调用一次）。
 *
 * Design goals:
 *   1. Every user-visible string in the sources is written as _("msgid") with an
 *      English msgid; Chinese translations live in lang_res/zh.json and are
 *      embedded into the DLL as a resource.
 *   2. No external langpack.dll: zh.json is linked into vbdmcp.dll as
 *      RCDATA("JSON_ZH") by the .rc; at runtime
 *      GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) with an address
 *      inside this module resolves the DLL's own module handle for
 *      FindResource / LoadResource.
 *   3. Lookups go through a khash table (src/khash.h); I18n_Clear() frees the
 *      strdup'ed strings to avoid leaks (called once before DLL unload).
 *
 * 用法：
 *      I18n_Refresh();                     // 按注册表 CustomLanguage 选语言
 *      puts(_("VB Decompiler MCP Server"));
 *      puts(_F("Version: %s", "2.0.0"));
 *      SetDlgItemTextA(hDlg, IDC_OK, _A("OK"));   // ANSI 控件用 GBK 字节
 *
 * Usage:
 *      I18n_Refresh();                     // pick the language from CustomLanguage
 *      puts(_("VB Decompiler MCP Server"));
 *      puts(_F("Version: %s", "2.0.0"));
 *      SetDlgItemTextA(hDlg, IDC_OK, _A("OK"));   // ANSI controls want GBK bytes
 *
 * 约定说明（文本边界，改字符串前先看这里）：
 *   - `_()`  返回 **UTF-8**，用于 MCP JSON 报文与其它 UTF-8 场景；
 *   - `_A()` 是 `_()` 的 ANSI(GBK) 版本，用于 MessageBoxA / SetDlgItemTextA /
 *     CreateWindowExA 等 Win32 A 系列接口，以及**宿主引擎的 SetValue**
 *     （如状态栏 vbd_set_status）—— 把 `_()` 的 UTF-8 交给这些接口会显示乱码。
 *     译文若在当前 ANSI 代码页里表示不了（如中文系统的波斯语），`_A()`
 *     **自动回退英文原文**，绝不把一串 '?' 交给宿主界面；
 *   - `_W()` 是 `_()` 的 **UTF-16** 版本，用于本插件**自己的 Unicode 窗口**
 *     （DialogBoxParamW / SetWindowTextW / MessageBoxW）：这些窗口是 Unicode
 *     控件，一旦经 ANSI 转换就会整串退化成 '?' —— 波斯语 / 阿拉伯语 / 希伯来语
 *     在 CP936 上完全不可表示（设置对话框即属此类，已从 A 系列迁到 W 系列）；
 *   - `_F()`  = 先翻译再格式化（UTF-8）；`_AF()` = 先翻译再格式化（GBK）。
 *     临时结果放在轮转缓冲池中，**只能立即使用**，不要跨语句长期持有。
 *   - 工具名（vbd_read_* / vbd_write_*）与其它协议标识符（JSON 键、HTTP 头）
 *     禁止翻译。
 *   - **日志（vbdmcp_logf / vbdmcp_log）固定英文、不走 i18n**：日志是排障元信息，
 *     不应随界面语言变化；initialize 的 instructions 面向模型，同样保持英文。
 *
 * Conventions (text boundaries - read this before changing strings):
 *   - `_()` returns **UTF-8**, for MCP JSON messages and other UTF-8 use.
 *   - `_A()` is the ANSI(GBK) variant of `_()`, for Win32 A APIs such as
 *     MessageBoxA / SetDlgItemTextA / CreateWindowExA and for the host engine's
 *     SetValue (e.g. the status bar, vbd_set_status) - handing UTF-8 from `_()`
 *     to those surfaces shows mojibake. If a translation is not representable in
 *     the current ANSI code page (Persian on a CP936 machine), `_A()` falls back
 *     to the English msgid rather than handing a wall of '?' to the host.
 *   - `_W()` is the **UTF-16** variant of `_()`, for the plugin's **own Unicode
 *     windows** (DialogBoxParamW / SetWindowTextW / MessageBoxW): those controls
 *     are Unicode, and anything routed through ANSI collapses to '?' - Persian,
 *     Arabic and Hebrew are not representable in CP936 at all (the settings
 *     dialog is such a surface and has been migrated to the W family).
 *   - `_F()` translates then formats (UTF-8); `_AF()` translates then formats
 *     (GBK). Results live in rotating buffers and must be used immediately,
 *     never held across statements.
 *   - Tool names (vbd_read_* / vbd_write_*) and other protocol identifiers (JSON
 *     keys, HTTP headers) must never be translated.
 *   - **Logs (vbdmcp_logf / vbdmcp_log) are always English and never i18n'd**:
 *     they are diagnostics metadata and must not follow the UI language; the
 *     initialize "instructions" field is model-facing and stays English too.
 *
 * 语言包取值的语义（与 tools/extract_i18n.py 一并维护，改动请同时改两边）：
 *      键缺失 或 值为 null  → 未翻译，`_()` 返回 msgid 本身（英文原文）
 *      值是 ""              → **故意留空**，`_()` 返回空串（不是未翻译！）；
 *                             仅当 msgid 本身没有 % 占位符时才允许，否则视为误操作
 *                             并回退原文（留空会丢参数导致错参/崩溃）
 *      非空文本             → 译文；占位符与 msgid 不一致时整条拒绝并回退原文
 *
 * Language pack value semantics (kept in sync with tools/extract_i18n.py - change
 * both together):
 *      key missing or value null -> untranslated; `_()` returns the msgid itself
 *      value ""                  -> INTENTIONALLY empty; `_()` returns an empty
 *                                   string (this is NOT "untranslated"). Allowed
 *                                   only when the msgid has no % placeholder;
 *                                   otherwise it counts as a mistake and the
 *                                   original is used (dropping arguments would
 *                                   corrupt or crash the printf call)
 *      non-empty text            -> translation; rejected when the placeholders
 *                                   do not match the msgid (falls back to the
 *                                   original)
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
 * 除英文外每种语言都已嵌入语言包（JSON_XX）；英文无需包（msgid 就是英文）。
 * 没有语言包时 `_()` 直接返回 msgid（即英文原文），不会显示空白。
 * 新增语言只需三步（缺一不可）：加一个 lang_res/<xx>.json + 在 res/vbdmcp.rc
 * 里加一条 RCDATA + 在 i18n.cpp 的 kLangs 表对应行填上资源名。
 *
 * Language enum.
 *
 * The values map one-to-one to the strings in the VB Decompiler registry value
 *   HKCU\...\VB Decompiler\Options\CustomLanguage     (see I18n_LangName()).
 * The host writes the user's choice there and we match it verbatim, so no extra
 * mapping table is needed.
 *
 * Every language except English has an embedded pack (JSON_XX); English needs
 * none because the msgid is already English. Without a pack `_()` returns the
 * msgid, never blank text. Adding a language later takes all three of: one
 * lang_res/<xx>.json, one RCDATA entry in res/vbdmcp.rc, and one row (with the
 * resource name) in the kLangs table in i18n.cpp.
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

/* I18nMode 是 I18nLang 的别名（API 签名沿用这个名字）；EN/ZH 取值不变。
 * I18nMode is an alias of I18nLang (the API signatures keep using this name);
 * the EN/ZH values are unchanged. */
typedef I18nLang I18nMode;
#define I18N_MODE_EN I18N_LANG_EN
#define I18N_MODE_ZH I18N_LANG_ZH_SIMPLIFIED

/* ---------------- 语言名 <-> 枚举 / Language name <-> enum ---------------- */

/* 注册表 CustomLanguage 的字符串 → 枚举；未知/缺失 → I18N_LANG_EN。
 * 大小写不敏感，两端空白忽略；也接受 "zh"/"ja" 这类两字母代码。
 *
 * Registry CustomLanguage string -> enum; unknown/missing -> I18N_LANG_EN.
 * Case-insensitive, surrounding whitespace ignored; two-letter codes such as
 * "zh"/"ja" are accepted too. */
I18nLang    I18n_LangFromName(const char *name);

/* 枚举 → 注册表里使用的字符串（未在列表里的语言回退成 "English"）
 * Enum -> the string used in the registry (unknown languages fall back to "English"). */
const char *I18n_LangName(I18nLang lang);

/* 枚举 → 两字母代码（日志 / VBDMCP_LANG 环境变量用，如 zh、ja、pt-BR）
 * Enum -> short code (for logs / the VBDMCP_LANG environment variable: zh, ja, pt-BR). */
const char *I18n_LangCode(I18nLang lang);

/* 该语言是否已嵌入语言包；除英文（msgid 即英文，无需包）外均为真
 * Whether the language has an embedded pack; true for every language except
 * English (whose msgid is already English, so it needs no pack). */
int         I18n_LangHasPack(I18nLang lang);

/* 枚举 → RCDATA 资源名（未嵌入的语言返回 NULL）
 * Enum -> RCDATA resource name (NULL when no pack is embedded). */
const char *I18n_LangResourceName(I18nLang lang);

/* ---------------- 生命周期 / Lifecycle ---------------- */

/* 按注册表 ...\VB Decompiler\Options\CustomLanguage 选择语言
 * （也接受环境变量 VBDMCP_LANG 覆盖，取值同上）。
 * 每次调用都会重载语言包（设置窗体打开前用户可能改过语言）。
 *
 * Selects the language from ...\VB Decompiler\Options\CustomLanguage (the
 * VBDMCP_LANG environment variable overrides it, same value set). Each call
 * reloads the pack, since the user may have changed the host language. */
void I18n_Refresh(void);

/* 显式设置语言；从 DLL 内置资源加载该语言的语言包。
 * 返回 true 表示该语言有可用的翻译表；英文恒为 true；
 * 没有语言包的语言会退回英文（`_()` 返回 msgid）并返回 0。
 *
 * Sets the language explicitly and loads its pack from the embedded resources.
 * Returns true when a translation table is available; English always returns
 * true; languages without a pack fall back to English (`_()` returns msgids)
 * and return 0. */
int  I18n_SetMode(I18nMode mode);

I18nMode I18n_GetMode(void);

/* 释放翻译表（含 strdup 的 key/value 与 yyjson 文档）。
 * 释放后 `_()` 退化为返回 msgid。可在 DLL 卸载前调用一次。
 *
 * Releases the translation tables (strdup'ed key/value pairs and yyjson docs).
 * After that `_()` degrades to returning msgids. Call once before DLL unload. */
void I18n_Clear(void);

/* DLL 卸载专用：先关闭查表（其它线程的 `_()` 直接返回 msgid），再释放内存。
 * 必须在 DllMain(DLL_PROCESS_DETACH) 里调用。
 *
 * For DLL unload: disables lookups first (other threads' `_()` return msgids),
 * then frees memory. Must be called from DllMain(DLL_PROCESS_DETACH). */
void I18n_PrepareShutdown(void);

/* ---------------- 取文本 / Text getters ---------------- */

/* UTF-8（无翻译时返回 msgid 本身，生命周期为静态字符串）
 * UTF-8 (returns the msgid itself when untranslated; static lifetime). */
const char *I18n_GetText(const char *msgid);

/* ANSI / GBK（供 Win32 A 系列接口；返回轮转缓冲，立即使用）
 * ANSI / GBK (for Win32 A APIs; rotating buffer, use immediately). */
const char *I18n_GetTextAnsi(const char *msgid);

/* UTF-8 → UTF-16(WCHAR)：供本插件**自己的 Unicode 窗口**使用
 * （DialogBoxParamW / SetWindowTextW / MessageBoxW / TCITEMW ...）。
 * 返回 thread_local 轮转缓冲，与 _A() 同契约：**只能立即使用**，
 * 跨语句长期持有会被后续调用覆盖。
 *
 * UTF-8 -> UTF-16 (WCHAR), for the plugin's **own Unicode windows**
 * (DialogBoxParamW / SetWindowTextW / MessageBoxW / TCITEMW ...). Returns a
 * thread_local rotating buffer with the same contract as _A(): use it right
 * away - holding it across statements lets later calls overwrite it. */
const wchar_t *I18n_ToWide(const char *utf8);

/* UTF-16 → UTF-8：返回 vbdmcp_alloc 的新串（调用方 vbdmcp_free）
 * UTF-16 -> UTF-8: returns a new vbdmcp_alloc'ed string (caller frees it). */
char *I18n_WideToUtf8(const wchar_t *w);

/* 先翻译再格式化（UTF-8）；fmt 必须是已翻译的格式串
 * Translate then format (UTF-8); fmt must already be translated. */
char *I18n_Format(const char *fmt, ...);

/* 先翻译再格式化（GBK）；用于 MessageBoxA 等
 * Translate then format (GBK); used for MessageBoxA etc. */
char *I18n_FormatAnsi(const char *fmt, ...);

/* **不做任何格式化**的拷贝（轮转缓冲，立即使用）。
 * _F()/_AF() 在"占位符与实参个数不一致"时走这里：宁可原样显示 "%s"，
 * 也不能让 vsnprintf 从栈上取垃圾指针。
 *
 * Plain copy with NO formatting (rotating buffer, use immediately).
 * _F()/_AF() fall back to this on a placeholder/argument count mismatch:
 * showing a literal "%s" beats letting vsnprintf read garbage from the stack. */
char *I18n_CopyText(const char *utf8);
char *I18n_CopyTextAnsi(const char *utf8);

/* ---------------- 诊断 / Diagnostics ---------------- */

/* 占位符兼容性校验：译文与原文的 printf 占位符序列是否一致。
 *   1 = 一致（或原文含字面量 % 无法校验）
 *   0 = 不一致 —— 该译文不可用，i18n 会自动回退英文原文
 * 说明：译文被人工编辑时，丢失/改动占位符是最高发的事故
 *      （"%d" 写成 "%s" 会按错误类型取参，轻则错乱重则崩溃）。
 *
 * Placeholder compatibility check: whether the translation's printf placeholder
 * sequence matches the original's.
 *   1 = match (or the original holds a literal %, unverifiable)
 *   0 = mismatch - the translation is unusable and i18n falls back to English
 * Editing translations by hand makes missing/altered placeholders the most
 * frequent accident ("%d" typed as "%s" reads the argument as the wrong type:
 * garbled output at best, a crash at worst). */
int I18n_FormatCompatible(const char *msgid, const char *translated);

/* 该格式串需要多少个实参（0 = 无占位符/含字面量 %）。
 * 供下面的 _F()/_AF() 模板核对实参个数，避免 printf 系函数缺参时崩溃。
 *
 * How many arguments the format string needs (0 = no placeholder / literal %).
 * Used by the _F()/_AF() templates to verify the argument count and keep
 * printf-style functions from crashing on missing arguments. */
int I18n_FormatArity(const char *fmt);

/* _F()/_AF() 发现"占位符个数 ≠ 实参个数"时调用它记日志并安全退化。
 * Called by _F()/_AF() on a placeholder/argument count mismatch: logs and
 * degrades safely. */
void I18n_LogFormatMismatch(const char *fmt, int need, int got);

/* 当前语言包中的词条数（EN 模式恒为 0）
 * Number of entries in the current pack (always 0 in EN mode). */
int I18n_TranslatedCount(void);

/* 语言包来源描述：resource:JSON_ZH / file:<path> / none（供 /health 与日志）
 * Pack source description: resource:JSON_ZH / file:<path> / none (for /health and logs). */
const char *I18n_LangPackSource(void);

/* 语言包"世代号"：每次翻译表被重建/清空（I18n_Refresh / I18n_SetMode /
 * I18n_Clear）都会自增。仅供"想把译文缓存到自己缓冲区"的调用方判断
 * 是否过期。
 *
 * 线程安全：`_()` / `_A()` / `_F()` / `_AF()` 在**任何线程**都能调用（含 MCP 网络线程）。
 * 做法是"表发布后不再改动"：换语言时新建一张表替换全局指针，旧表挂进退休列表、
 * 直到 I18n_Clear / DLL 卸载才释放，因此查表始终是纯只读、无需加锁；`_A()`/`_AF()` 的
 * 轮转缓冲池是 thread_local。表的内容替换对读者而言只是"读到旧值或新值"，
 * 不会看到半张表。
 *
 * Pack "generation": incremented whenever the tables are rebuilt or cleared
 * (I18n_Refresh / I18n_SetMode / I18n_Clear). Callers that cache translations in
 * their own buffers can use it to detect staleness.
 *
 * Thread safety: `_()` / `_A()` / `_F()` / `_AF()` may be called from ANY thread
 * (including the MCP network thread). Published tables are immutable: a language
 * switch replaces the global pointer and retires the old table into a list freed
 * only by I18n_Clear / DLL unload. Lookups are therefore pure reads needing no
 * lock, and the `_A()`/`_AF()` rotating buffers are thread_local. For readers a
 * table switch is just "old value or new value", never a half-built table. */
unsigned I18n_Generation(void);

/* ---------------- 宏（gettext 风格）/ Macros (gettext style) ---------------- */

#define _(msgid)        I18n_GetText(msgid)
#define N_(msgid)        (msgid)                 /* 仅供提取脚本扫描的静态标记 / static marker for the extraction script */
#define _A(msgid)       I18n_GetTextAnsi(msgid)

/* _F / _AF：先翻译再格式化，支持全部 printf 占位符（%s %d %zu %.2f %% ...）。
 * 走下面的变参模板，会自动核对"占位符所需实参个数 == 实际传入个数"：
 * 不一致时记 ERROR 日志并原样返回格式串（安全退化，不会因缺参而崩溃）。
 *
 * _F / _AF: translate then format; supports every printf placeholder
 * (%s %d %zu %.2f %% ...). The variadic templates below verify that the number
 * of required arguments matches the number passed in: on mismatch they log an
 * ERROR and return the format string as-is (safe degradation, never a crash). */
#define _F(msgid, ...)   I18n_FormatChecked(_(msgid), ##__VA_ARGS__)
#define _AF(msgid, ...)  I18n_FormatCheckedAnsi(_(msgid), ##__VA_ARGS__)

/* _W / _FW：_()/_F() 的 **UTF-16** 版本，专供本插件自己的 Unicode 窗口
 * （DialogBoxParamW / SetWindowTextW / MessageBoxW / TCITEMW ...）。
 * 宿主引擎的 A 系列接口仍用 _A()/_AF()，两者不要混用。
 *
 * _W / _FW: the **UTF-16** variants of _()/_F(), for the plugin's own Unicode
 * windows only (DialogBoxParamW / SetWindowTextW / MessageBoxW / TCITEMW ...).
 * The host engine's A-API surfaces still take _A()/_AF(); do not mix them. */
#define _W(msgid)        I18n_ToWide(_(msgid))
#define _FW(msgid, ...)  I18n_ToWide(I18n_FormatChecked(_(msgid), ##__VA_ARGS__))

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
 *
 * Implementation of _F() / _AF() (C++ variadic templates).
 *
 * Why templates: printf-style functions cannot know how many arguments the
 * caller passed at runtime. If the format string contains "%s" and the caller
 * forgot the argument, vsnprintf reads a garbage pointer from the stack and
 * crashes. The templates therefore check "required == passed" before formatting:
 *   - mismatch  -> log an ERROR and return the format string as-is (safe);
 *   - no placeholder -> plain copy, skipping vsnprintf;
 *   - match     -> hand over to the C-style I18n_Format / I18n_FormatAnsi.
 * ------------------------------------------------------------------ */
template <typename... Args>
char *I18n_FormatChecked(const char *fmt, Args... args)
{
    const int need = I18n_FormatArity(fmt);
    /* 无占位符或实参个数对不上：一律走"纯拷贝"，绝不进 vsnprintf
     * No placeholder or argument-count mismatch: plain copy, never vsnprintf */
    if (need <= 0) return I18n_CopyText(fmt);
    if (need != (int)sizeof...(Args)) {
        I18n_LogFormatMismatch(fmt, need, (int)sizeof...(Args));
        return I18n_CopyText(fmt);        /* 安全退化：原样返回格式串 / safe fallback */
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
        return I18n_CopyTextAnsi(fmt);    /* 安全退化：原样返回格式串 / safe fallback */
    }
    return I18n_FormatAnsi(fmt, args...);
}
#endif

#endif /* VBDMCP_I18N_H */
