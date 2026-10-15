/*
 * vbdec_sdk.cpp - VB Decompiler Plugin SDK 封装（C++ 版）
 *
 * VB Decompiler 的 Callback Engine 函数原型（与官方VC SDK一致）：
 *     void* __stdcall Engine(int vlType, int vlNumber, int vlFnNumber, void *vlNewValue);
 *
 * 返回值：对于 Get 命令，返回 UTF-16 字符串指针；非字符串命令返回其他数据。
 *         字符串内存由 VB Decompiler 管理（不需要也不应该释放）。
 *
 * 线程安全：VB Decompiler 是 GUI 单线程应用，Callback Engine 只能从
 *           主线程（VB Decompiler 消息循环所在线程）调用。
 *           因此 vbd_get_value / vbd_set_value 只能在 UI 主线程调用 ——
 *           mcp_http.cpp 的网络线程通过 UiTask 队列把所有工具调用投递到主线程，
 *           后台线程绝不直接调用本文件里的任何函数。
 */
#include "vbdmcp.h"
#include <strsafe.h>
#include <excpt.h>     /* SEH（MSVC）：EXCEPTION_ACCESS_VIOLATION 等 */

/* 全局引擎函数指针 */
TVBPluginEngine g_engine = NULL;

/* 缓存最近一次 PluginLoad 收到的 engine 指针（用于 lazy init）。
 *
 * 背景：VB Decompiler 启动时直接打开项目（命令行参数 / 自动恢复），
 *       VBDecompilerPluginLoad 在**项目完全加载之前**被调用，
 *       plugin_engine 可能是 NULL，或者指向"未加载项目"的引擎。
 *       此时 g_engine 被设上，但所有 Get/Set 调用返回空/失败。
 *
 * 解决：PluginLoad 收到的 plugin_engine 总是缓存到 g_pending_engine。
 *       每次 MCP 工具调用前，如果 g_engine 还是 NULL 但 g_pending_engine 有值，
 *       就用缓存的 engine 重新初始化。
 *
 * 配合：MCP 服务器在 PluginName 第一次调用时就启动（不需要 plugin_engine），
 *       IDE 客户端立刻能连上、拿到工具列表（initialize + tools/list）。
 *       当 AI 真正调用工具时，触发 lazy init。
 */
static TVBPluginEngine g_pending_engine = NULL;

void vbd_sdk_init(TVBPluginEngine engine)
{
    /* 总是缓存，包括 NULL。PluginLoad 第二次调用时 plugin_engine 是有效的。 */
    g_pending_engine = engine;
    g_engine = engine;
    vbdmcp_log("vbd_sdk_init: engine=%p (pending cached)", (void *)engine);
}

/*
 * 内部辅助：调用引擎并把 UTF-16 返回值转换为 ANSI（Codepage = CP_ACP，与原SDK一致）。
 * 返回值为 new alloc 的 ANSI 字符串，调用方 vbdmcp_free。
 * 失败返回 NULL。
 */
static char *engine_get_to_ansi(int vlType, int vlNumber, int vlFnNumber)
{
    if (!g_engine) return NULL;
    /* SEH 保护：g_engine 可能在 VB Decompiler 切换项目后被 reset，
     * 此时函数指针指向已释放内存，调用直接触发 Access Violation。
     * 实测遇到过 offset 11FC AV 崩溃。 */
    void *p = NULL;
    __try {
        p = g_engine(vlType, vlNumber, vlFnNumber, NULL);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
                EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        vbdmcp_log("engine_get_to_ansi: AV at g_engine (vlType=%d, vlNumber=%d), "
                  "clearing engine pointers", vlType, vlNumber);
        /* 清空防止下次再 AV（用户没点 Plugins 菜单之前所有工具都会失败） */
        g_engine = NULL;
        g_pending_engine = NULL;
        return NULL;
    }
    if (!p) return NULL;
    /* 假定返回值为 UTF-16 LE 字符串指针（以 NUL 终止） */
    const wchar_t *wstr = (const wchar_t *)p;
    int len = WideCharToMultiByte(CP_ACP, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (len <= 0) return NULL;
    char *out = (char *)vbdmcp_alloc((size_t)len);
    if (!out) return NULL;
    WideCharToMultiByte(CP_ACP, 0, wstr, -1, out, len, NULL, NULL);
    return out;
}

/*
 * 内部辅助：把 ANSI 字符串直接透传给引擎（用于 Set 命令）。
 * 返回值为引擎返回值（指针），具体含义由 vlType 决定。
 *
 * 重要：与 SDK 26 4 个 demo 对齐 ——
 *   - rust/only code  src/lib.rs 用 CString::into_raw（ANSI，leak）
 *   - rust/gui        src/lib.rs 用 CString::into_raw（ANSI，leak）
 *   - asm/masm  VBDM_SaveRTF.asm 用 LPSTR（ANSI，长生命周期）
 *   - vc/VBDecC.cpp 走 C++ wrapper（ANSI）
 *
 * 引擎在 Set 路径上**存储指针不拷贝**（demo 全部 leak 以避免 use-after-free）。
 * 我们也传 ANSI 透传 + 故意 leak（不用 vbdmcp_alloc，否则日后有人误调 vbdmcp_free）。
 * 代价与风险：每次 Set 漏一份字符串。普通字段（端口/索引/标志）很小，但
 * **vbd_write_project_text 之类工具单次可传 16MB** —— AI 客户端反复写大文件就是几百 MB 的常驻泄漏。
 * 所以这里累计泄漏量并在跨过阈值时告警，让“隐形泄漏”变成日志里看得见的事实。
 *
 * 不再转 UTF-16：之前转 UTF-16 后引擎读 ANSI 字节序会得到 `H\0e\0...` ，
 * 对 ASCII 表现为"取首字节"或空串，对非 ASCII（含中文）直接乱码。
 */
/* 已泄漏字节数（到达阈值时告警一次） */
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
    /* SEH 保护：同 engine_get_to_ansi 注释（避免 engine 失效 AV） */
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
    /* 注：故意不 free(buf)。引擎内部存储该指针供后续显示/UpdateAll 使用。 */
}

/*
 * MCP 工具调用前的 lazy init 钩子（单层）。
 *
 * 背景：VB Decompiler 启动时直接打开项目（命令行参数 / 自动恢复），
 *       VBDecompilerPluginLoad 在**项目完全加载之前**被调用，
 *       plugin_engine 可能是 NULL，或者指向"未加载项目"的引擎。
 *       此时 g_engine 被设上，但所有 Get/Set 调用返回空/失败。
 *
 * 解决：PluginLoad 收到的 plugin_engine 总是缓存到 g_pending_engine。
 *       每次 MCP 工具调用前，如果 g_engine 还是 NULL 但 g_pending_engine 有值，
 *       就用缓存的 engine 重新初始化。
 *
 * 历史 Layer 2 "heartbeat"（调 VBD_SetStatusBarText）已移除 —— 实测无法
 * 修复 "PluginLoad 没重入" 的问题，并且当 g_engine 指向已失效的函数指针
 * （用户切换项目后 engine 被 reset）时直接导致 Access Violation。
 * 现在依赖 "Plugins 菜单 → 配置窗口 → 点确定" 的手动工作流来触发
 * PluginLoad 重入（plugin_main.cpp::VBDecompilerPluginLoad）。
 *
 * 项目状态探测由调用方通过 vbd_is_project_loaded() 显式完成 —— SDK
 * 不暴露 "ReloadFile / RefreshEngine / ReInitPlugin" 这类 API。
 */
static void vbd_ensure_engine(void)
{
    if (!g_engine && g_pending_engine) {
        g_engine = g_pending_engine;
        vbdmcp_log("vbd_ensure_engine: lazy init from pending, engine=%p",
                  (void *)g_engine);
    }
}

/* --------- 公共API --------- */

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
 * 项目加载状态探测。
 *
 * 返回：
 *   1 = 已加载（VBD_GetFileName 返回非空字符串）
 *   0 = 未加载或 g_engine 不可用
 *
 * 用于 MCP 工具 handler 在调用前判断。如果返回 0，应给客户端返回
 * 带解决方法的错误："请在 VB Decompiler 的 Plugins 菜单打开 vbdmcp
 * 配置窗口，点确定以触发 PluginLoad 重入"。
 *
 * 注意：此函数**只读不修复**。SDK 不暴露 "ReloadFile / RefreshEngine"
 * 等 API，"Plugins 菜单点确定"是 VB Decompiler 私有路径，vbdmcp 无
 * 法从 DLL 侧模拟。
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
