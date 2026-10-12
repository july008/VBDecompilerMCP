#include "vbmcp.h"
#include "lang.h"
#include <stdio.h>
#include <strsafe.h>
#include <stdarg.h>

/* g_engine 在 vbdec_sdk.c 中定义 */
extern TVBPluginEngine g_engine;

/* PluginName 调用计数：1=首次启动，>=2=每次显示菜单前都再问一次名字 */
static int g_name_count = 0;

/* PluginLoad 调用计数：1=首次启动，>=2=用户点菜单 */
static int g_load_count = 0;

/* ============================================================
 * DllMain 调试 trace（用 OutputDebugString 输出，不弹窗，避免 loader lock 死锁）
 * ============================================================ */
static void vbmcp_trace(const char *title, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ============================================================
 * 导出函数（注意 __stdcall 与 extern "C"）
 * ============================================================ */

__declspec(dllexport) void __stdcall VBDecompilerPluginName(
    HWND VBDecompilerHWND, HWND RichTextBoxHWND,
    char *buffer, int reserved)
{
    /* PluginName 是 VB Decompiler **第一个**回调的导出函数（早于 PluginLoad），
     * 因此这里是"启动 MCP 服务器"的**最早合理时机**。
     * 但 VB Decompiler 在启动后**每次显示 Plugins 菜单前都可能再问一次名字**
     * → 必须用计数器，只在第一次启动服务器。 */
    g_name_count++;

    if (g_name_count == 1) {
        int port = VBMCP_DEFAULT_PORT;
        char bind_buf[64] = {0};
        vbmcp_config_load( &port, bind_buf, sizeof(bind_buf));

        g_enabled = 0;   /* 永远默认禁用，由用户主动开启 */
        g_port    = port;
        if (g_bindings) vbmcp_free(g_bindings);
        g_bindings = vbmcp_strdup(bind_buf[0] ? bind_buf : VBMCP_DEFAULT_BIND);

        vbmcp_log("first PluginName: server NOT auto-started (user must enable via Plugins dialog)");
    }
    /* 后续 PluginName 调用（VB Decompiler 显示菜单前再问名字）只做语言刷新
     * 和写菜单文字，不动服务器。 */

    (void)VBDecompilerHWND; (void)RichTextBoxHWND; (void)reserved;
    if (!buffer) return;

    /* 关键：菜单项文字也是用户可见文本，必须按当前语言切换。 */
    vbmcp_lang_refresh();
    strncpy_s(buffer, 100, vbmcp_t(T_PLUGIN_NAME), _TRUNCATE);
}

__declspec(dllexport) void __stdcall VBDecompilerPluginAuthor(
    HWND VBDecompilerHWND, HWND RichTextBoxHWND,
    char *buffer, int reserved)
{
    (void)VBDecompilerHWND; (void)RichTextBoxHWND; (void)reserved;
    if (!buffer) return;
    vbmcp_lang_refresh();
    strncpy_s(buffer, 100, vbmcp_t(T_PLUGIN_AUTHOR), _TRUNCATE);
}

__declspec(dllexport) void __stdcall VBDecompilerPluginLoad(
    HWND VBDecompilerHWND, HWND RichTextBoxHWND,
    char *buffer, void *plugin_engine)
{
    /* 必须最先 ++g_load_count，否则后面的 if (g_load_count == 1) 永远不命中 */
    g_load_count++;

    (void)RichTextBoxHWND;
    (void)buffer;


    /* 0) 读注册表 CustomLanguage，决定后续所有 UI 文本语言 */
    vbmcp_lang_refresh();

    /* 1) 初始化引擎（用于访问 VB Decompiler 内部）。
     * plugin_engine 为 NULL 时也允许继续（启动那次可能还没建引擎），
     * 只是后续 MCP 工具调 g_engine 会失败，不影响服务器起停。 */
    if (plugin_engine) {
        vb_sdk_init((TVBPluginEngine)plugin_engine);
    } else {
        vbmcp_log("plugin_engine is NULL, skip engine init");
    }

    /* 2) 加载配置：默认 → 注册表（缺失字段保留默认） */
    {
        int port = VBMCP_DEFAULT_PORT;
        char bind_buf[64] = {0};
        vbmcp_config_load( &port, bind_buf, sizeof(bind_buf));

        /* 写回全局 */
        g_port = port;
        if (g_bindings) vbmcp_free(g_bindings);
        g_bindings = vbmcp_strdup(bind_buf[0] ? bind_buf : VBMCP_DEFAULT_BIND);
    }

    /* 4) 任何 PluginLoad 调用都弹设置窗体——兼作 MCP 服务器状态显示面板，
     * 用户每次打开都能看到端口/启停状态/复制 URL 给 AI 客户端。 */
    if (VBDecompilerHWND && IsWindow(VBDecompilerHWND)) {
        vbmcp_options_dialog(VBDecompilerHWND);
    } else {
        vbmcp_log("VBDecompilerHWND invalid, cannot show options dialog");
    }
}

/* ============================================================
 * DLL 入口
 * ============================================================ */

static HINSTANCE g_hInstance = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_hInstance = hModule;
        DisableThreadLibraryCalls(hModule);

        break;
    case DLL_PROCESS_DETACH:
        vbmcp_server_stop();
        vbmcp_options_dialog_cleanup();   /* 释放设置窗体的字体（GDI对象） */
        if (g_bindings) { vbmcp_free(g_bindings); g_bindings = NULL; }
        break;
    }
    return TRUE;
}
