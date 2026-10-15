/*
 * vbdec_sdk.cpp - VB Decompiler Plugin SDK 封装（C++ 版）
 * vbdec_sdk.cpp - Wrapper around the VB Decompiler plugin SDK (C++)
 *
 * VB Decompiler 的 Callback Engine 函数原型（与官方 VC SDK 一致）：
 *     void* __stdcall Engine(int vlType, int vlNumber, int vlFnNumber, void *vlNewValue);
 *
 * 返回值：对于 Get 命令，返回 UTF-16 字符串指针；非字符串命令返回其他数据。
 *         字符串内存由 VB Decompiler 管理（不需要也不应该释放）。
 *
 * The VB Decompiler callback engine prototype (matching the official VC SDK):
 *     void* __stdcall Engine(int vlType, int vlNumber, int vlFnNumber, void *vlNewValue);
 *
 * Return value: Get commands return a UTF-16 string pointer, other commands
 * return other data. The string memory is owned by VB Decompiler (never free it).
 *
 * 线程安全：VB Decompiler 是 GUI 单线程应用，Callback Engine 只能从主线程
 *           （VB Decompiler 消息循环所在线程）调用。因此 vbd_get_value /
 *           vbd_set_value 只在 UI 主线程使用 —— 网络线程通过 UiTask 队列把
 *           工具调用投递到主线程，绝不直接调用本文件里的任何函数。
 *
 * Threading: VB Decompiler is a single-threaded GUI application; the callback
 * engine may only be called from the main thread (the one running the message
 * loop). vbd_get_value / vbd_set_value therefore run on the UI main thread only;
 * the network thread dispatches tool calls there through the UiTask queue and
 * never calls into this file directly.
 */
#include "vbdmcp.h"
#include <strsafe.h>
#include <excpt.h>     /* SEH（MSVC）：EXCEPTION_ACCESS_VIOLATION 等 / SEH on MSVC */

/* 全局引擎函数指针 / Global engine function pointer */
TVBPluginEngine g_engine = NULL;

/* 缓存最近一次 PluginLoad 收到的 engine 指针（用于 lazy init）。
 *
 * 背景：VB Decompiler 启动时直接打开项目（命令行参数 / 自动恢复），
 *       VBDecompilerPluginLoad 在**项目完全加载之前**被调用，
 *       plugin_engine 可能是 NULL，或者指向"未加载项目"的引擎。
 *       此时 g_engine 被设上，但所有 Get/Set 调用返回空/失败。
 *
 * 解决：PluginLoad 收到的 plugin_engine 总是缓存到 g_pending_engine；
 *       每次 MCP 工具调用前，如果 g_engine 还是 NULL 但 g_pending_engine
 *       有值，就用缓存的 engine 重新初始化（见 vbd_ensure_engine）。
 *
 * 配合：MCP 服务器在 PluginName 第一次调用时启动（不需要 plugin_engine），
 *       AI 客户端可立即连接并获取工具列表；真正调用工具时才触发 lazy init。
 *
 * Cached engine pointer from the last PluginLoad (used for lazy init).
 *
 * Background: when VB Decompiler opens a project at startup (command line /
 * auto-restore), VBDecompilerPluginLoad is called BEFORE the project finishes
 * loading, so plugin_engine may be NULL or point to an engine without a loaded
 * project; g_engine would then be set but every Get/Set call returns empty.
 *
 * Solution: every plugin_engine received by PluginLoad is cached into
 * g_pending_engine; before each MCP tool call, if g_engine is still NULL but
 * g_pending_engine has a value, the cached engine is re-installed
 * (see vbd_ensure_engine).
 *
 * The server itself starts on the first PluginName call (no engine needed), so
 * AI clients can connect and fetch the tool list immediately; the lazy init runs
 * when a tool is actually called.
 */
static TVBPluginEngine g_pending_engine = NULL;

void vbd_sdk_init(TVBPluginEngine engine)
{
    /* 总是缓存，包括 NULL：PluginLoad 第二次调用时 plugin_engine 才有效。
     * Always cache, including NULL: plugin_engine becomes valid on the second
     * PluginLoad call. */
    g_pending_engine = engine;
    g_engine = engine;
    vbdmcp_log("vbd_sdk_init: engine=%p (pending cached)", (void *)engine);
}

/*
 * 内部辅助：调用引擎并把 UTF-16 返回值转换为 ANSI（CP_ACP，与原 SDK 一致）。
 * 返回 new alloc 的 ANSI 字符串（调用方 vbdmcp_free）；失败返回 NULL。
 *
 * Internal helper: calls the engine and converts the UTF-16 return value to
 * ANSI (CP_ACP, matching the original SDK). Returns a new alloc'ed ANSI string
 * (caller frees with vbdmcp_free); NULL on failure.
 */
static char *engine_get_to_ansi(int vlType, int vlNumber, int vlFnNumber)
{
    if (!g_engine) return NULL;
    /* SEH 保护：切换项目后 g_engine 可能已失效（指向已释放的内存），调用会
     * 直接触发 Access Violation，因此必须捕获并清空指针。
     * SEH guard: after a project switch g_engine may be stale (pointing to freed
     * memory) and the call would raise an Access Violation; catch it and clear
     * the pointers. */
    void *p = NULL;
    __try {
        p = g_engine(vlType, vlNumber, vlFnNumber, NULL);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        vbdmcp_log("engine_get_to_ansi: AV at g_engine (vlType=%d, vlNumber=%d), "
                  "clearing engine pointers", vlType, vlNumber);
        /* 清空防止下次再 AV；在用户重新触发 PluginLoad 之前，所有工具都会失败。
         * Clear to avoid repeated AVs; until PluginLoad runs again every tool fails. */
        g_engine = NULL;
        g_pending_engine = NULL;
        return NULL;
    }
    if (!p) return NULL;
    /* 假定返回值为 UTF-16 LE 字符串指针（以 NUL 终止）
     * The return value is assumed to be a NUL-terminated UTF-16 LE string. */
    const wchar_t *wstr = (const wchar_t *)p;
    int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *out = (char *)vbdmcp_alloc((size_t)len);
    if (!out) return NULL;
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, out, len, NULL, NULL);
    return out;
}

/*
 * 内部辅助：把 ANSI 字符串透传给引擎（用于 Set 命令）。
 * 返回值为引擎返回值（指针），具体含义由 vlType 决定。
 *
 * 重要：引擎在 Set 路径上**存储指针不拷贝**，且期望 ANSI 字节。
 * 不要转 UTF-16 —— 引擎会得到 `H\0e\0...`，对 ASCII 表现为"取首字节"或空串，
 * 对非 ASCII（含中文）直接乱码。
 * 因此这里 ANSI 透传 + 故意 leak（不用 vbdmcp_alloc，避免日后有人误调
 * vbdmcp_free 造成 use-after-free）。
 *
 * 代价与风险：每次 Set 漏一份字符串。普通字段很小，但 vbd_write_project_text
 * 之类工具单次可传 16MB —— 反复写大文件会累积常驻泄漏。因此这里累计泄漏量并
 * 在跨过阈值时告警，让"隐形泄漏"变成日志里看得见的事实。
 *
 * Internal helper: passes an ANSI string straight to the engine (Set commands).
 * The return value is the engine's; its meaning depends on vlType.
 *
 * Important: on the Set path the engine STORES the pointer WITHOUT copying and
 * expects ANSI bytes. Never convert to UTF-16: the engine would read
 * `H\0e\0...` as "first byte" or an empty string, and non-ASCII text (including
 * Chinese) would be garbled. Hence raw ANSI pass-through plus an intentional
 * leak (not vbdmcp_alloc, so a future vbdmcp_free call cannot cause
 * use-after-free).
 *
 * Cost and risk: every Set call leaks one string. Ordinary fields are small, but
 * tools like vbd_write_project_text can pass 16MB per call - repeated large
 * writes accumulate permanently. The leaked bytes are therefore accumulated and
 * a warning is logged when thresholds are crossed, turning the invisible leak
 * into a visible fact in the log.
 */
/* 已泄漏字节数（到达阈值时告警一次）
 * Leaked bytes so far (warn once per threshold) */
static std::atomic<size_t> g_set_leak_bytes{ 0 };
static void note_set_leak(size_t n)
{
    static const size_t kWarnAt[] = { 1u << 20, 16u << 20, 64u << 20, 256u << 20 };
    const size_t total = g_set_leak_bytes.fetch_add(n) + n;
    for (size_t thr : kWarnAt) {
        if (total >= thr && total - n < thr) {
            vbdmcp_logf(VBDMCP_LOG_WARN,
                       "engine Set leak reached %.1f MB (engine keeps every pointer; "
                       "large vbd_write_project_text calls are the main contributor)",
                       (double)total / (1024.0 * 1024.0));
            break;
        }
    }
}

static void *engine_set_from_ansi(int vlType, int vlNumber, int vlFnNumber, const char *value)
{
    if (!g_engine) return NULL;
    if (!value) value = "";
    size_t n = strlen(value);
    char *buf = (char *)malloc(n + 1);
    if (!buf) return NULL;
    memcpy(buf, value, n);
    buf[n] = '\0';
    /* SEH 保护：同 engine_get_to_ansi（引擎失效时避免 AV）
     * SEH guard: same as engine_get_to_ansi (stale engine -> AV) */
    void *r = NULL;
    __try {
        r = g_engine(vlType, vlNumber, vlFnNumber, buf);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        vbdmcp_log("engine_set_from_ansi: AV at g_engine (vlType=%d, vlNumber=%d), "
                  "clearing engine pointers", vlType, vlNumber);
        g_engine = NULL;
        g_pending_engine = NULL;
        return NULL;
    }
    note_set_leak(n + 1);
    return r;
    /* 注：故意不 free(buf)。引擎内部存储该指针供后续显示 / UpdateAll 使用。
     * Note: buf is intentionally not freed; the engine keeps the pointer for
     * later display / UpdateAll calls. */
}

/*
 * MCP 工具调用前的 lazy init 钩子（单层）。
 * 设计背景见 g_pending_engine 处的说明。
 *
 * 约束：不得用引擎调用做"心跳 / 探测"—— g_engine 在用户切换项目后可能已失效，
 * 主动调用会直接 AV。项目状态探测请用 vbd_is_project_loaded()；重新挂上引擎
 * 依赖 "Plugins 菜单 → 配置窗口 → 点确定" 触发 PluginLoad 重入（SDK 不暴露
 * ReloadFile / RefreshEngine / ReInitPlugin 之类的 API）。
 *
 * Lazy init hook run before each MCP tool call (single layer).
 * See the comment on g_pending_engine for the background.
 *
 * Constraint: never use engine calls as a heartbeat/probe - g_engine may be stale
 * after the user switches projects and calling it can raise an AV. Use
 * vbd_is_project_loaded() for status checks; re-attaching the engine relies on
 * "Plugins menu -> settings dialog -> OK" triggering a PluginLoad re-entry (the
 * SDK exposes no ReloadFile / RefreshEngine / ReInitPlugin API).
 */
static void vbd_ensure_engine(void)
{
    if (!g_engine && g_pending_engine) {
        g_engine = g_pending_engine;
        vbdmcp_log("vbd_ensure_engine: lazy init from pending, engine=%p",
                  (void *)g_engine);
    }
}

/* --------- 公共 API / Public API --------- */

char *vbd_get_value(int vlType, int vlNumber, int vlFnNumber)
{
    vbd_ensure_engine();
    return engine_get_to_ansi(vlType, vlNumber, vlFnNumber);
}

void vbd_set_value(int vlType, const char *value, int vlNumber, int vlFnNumber)
{
    vbd_ensure_engine();
    engine_set_from_ansi(vlType, vlNumber, vlFnNumber, value);
}

void vbd_set_status(const char *msg)
{
    vbd_set_value(VBD_SetStatusBarText, msg, 0, 0);
}

/*
 * 项目加载状态探测（只读，不做修复）。
 *
 * 返回：1 = 已加载（VBD_GetFileName 返回非空字符串）；
 *       0 = 未加载或 g_engine 不可用。
 *
 * 返回 0 时调用方应回带解决方法的错误："请在 VB Decompiler 的 Plugins 菜单
 * 打开 vbdmcp 配置窗口并点确定，以触发 PluginLoad 重入"。
 *
 * Reads the project load state (read-only; it never tries to repair it).
 *
 * Returns 1 when loaded (VBD_GetFileName yields a non-empty string), 0 when not
 * loaded or g_engine is unavailable.
 *
 * Callers should answer 0 with an error containing the remedy: "open the vbdmcp
 * settings dialog from the Plugins menu and click OK to trigger a PluginLoad
 * re-entry".
 */
int vbd_is_project_loaded(void)
{
    vbd_ensure_engine();
    if (!g_engine) return 0;
    char *fn = engine_get_to_ansi(VBD_GetFileName, 0, 0);
    int loaded = (fn != NULL && fn[0] != '\0');
    if (fn) vbdmcp_free(fn);
    return loaded ? 1 : 0;
}
