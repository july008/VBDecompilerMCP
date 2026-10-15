#include "vbdmcp.h"
#include "i18n.h"
#include <stdio.h>
#include <strsafe.h>
#include <stdarg.h>

/* g_engine 在 vbdec_sdk.cpp 中定义 */
extern TVBPluginEngine g_engine;

/* PluginName 调用计数：1=首次启动，>=2=每次显示菜单前都再问一次名字 */
static int g_name_count = 0;

/* VB Decompiler 主窗口句柄（vbdmcp.h 声明为 extern）。
 * 在 PluginName / PluginLoad 回调中保存，供 UI 弹窗（授权/告警）作父窗口。 */
HWND g_main_hwnd = NULL;

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

    /* 尽早保存 VB Decompiler 主窗口句柄（UI 弹窗父窗口） */
    if (VBDecompilerHWND && IsWindow(VBDecompilerHWND)) g_main_hwnd = VBDecompilerHWND;

    if (g_name_count == 1) {
        int port = VBDMCP_DEFAULT_PORT;
        int allow_write = VBDMCP_DEFAULT_ALLOW_WRITE;
        char bind_buf[64] = {0};
        vbdmcp_config_load(&port, bind_buf, sizeof(bind_buf), &allow_write);

        g_enabled.store(0);          /* 永远默认禁用，由用户主动开启 */
        g_port.store(port);
        g_allow_write.store(allow_write);
        vbdmcp_set_bind_address(bind_buf[0] ? bind_buf : VBDMCP_DEFAULT_BIND);

        vbdmcp_log("first PluginName: server NOT auto-started (user must enable via Plugins dialog)");
    }
    /* 后续 PluginName 调用（VB Decompiler 显示菜单前再问名字）只做语言刷新
     * 和写菜单文字，不动服务器。 */

    (void)VBDecompilerHWND; (void)RichTextBoxHWND; (void)reserved;
    if (!buffer) return;

    /* 关键：菜单项文字也是用户可见文本，必须按当前语言切换。 */
    I18n_Refresh();
    strncpy_s(buffer, 100, _A("VB Decompiler MCP Server"), _TRUNCATE);
}

__declspec(dllexport) void __stdcall VBDecompilerPluginAuthor(
    HWND VBDecompilerHWND, HWND RichTextBoxHWND,
    char *buffer, int reserved)
{
    (void)VBDecompilerHWND; (void)RichTextBoxHWND; (void)reserved;
    if (!buffer) return;
    I18n_Refresh();
    strncpy_s(buffer, 100, _A("vbdmcp project, admin@vb-decompiler.org"), _TRUNCATE);
}

__declspec(dllexport) void __stdcall VBDecompilerPluginLoad(
    HWND VBDecompilerHWND, HWND RichTextBoxHWND,
    char *buffer, void *plugin_engine)
{
    (void)RichTextBoxHWND;
    (void)buffer;

    /* 保存主窗口句柄（供授权/告警弹窗作父窗口） */
    if (VBDecompilerHWND && IsWindow(VBDecompilerHWND)) g_main_hwnd = VBDecompilerHWND;

    /* 0) 读注册表 CustomLanguage，决定后续所有 UI 文本语言 */
    I18n_Refresh();

    /* 1) 初始化引擎（用于访问 VB Decompiler 内部）。
     * plugin_engine 为 NULL 时也允许继续（启动那次可能还没建引擎），
     * 只是后续 MCP 工具调 g_engine 会失败，不影响服务器起停。 */
    if (plugin_engine) {
        vbd_sdk_init((TVBPluginEngine)plugin_engine);
    } else {
        vbdmcp_log("plugin_engine is NULL, skip engine init");
    }

    /* 2) 加载配置：默认 → 注册表（缺失字段保留默认） */
    {
        int port = VBDMCP_DEFAULT_PORT;
        int allow_write = VBDMCP_DEFAULT_ALLOW_WRITE;
        char bind_buf[64] = {0};
        vbdmcp_config_load(&port, bind_buf, sizeof(bind_buf), &allow_write);

        /* 写回全局 */
        g_port.store(port);
        g_allow_write.store(allow_write);
        vbdmcp_set_bind_address(bind_buf[0] ? bind_buf : VBDMCP_DEFAULT_BIND);
    }

    /* 4) 任何 PluginLoad 调用都弹设置窗体——兼作 MCP 服务器状态显示面板，
     * 用户每次打开都能看到端口/启停状态/复制 URL 给 AI 客户端。 */
    if (VBDecompilerHWND && IsWindow(VBDecompilerHWND)) {
        vbdmcp_options_dialog(VBDecompilerHWND);
    } else {
        vbdmcp_log("VBDecompilerHWND invalid, cannot show options dialog");
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
        /* 进程卸载：只置停机标志，绝不在loader lock 内 join 线程。
         * 进程即将退出，socket / 任务队列由系统整体回收。
         * i18n：先关闭查表（其它线程的 _() 立即返回 msgid），再释放
         * 哈希表内 strdup 的字符串，避免 DLL 长期重载的泄漏。
         * UI：释放设置窗体的字体/画刷 GDI 对象（否则每次重载都泄漏）。 */
        vbdmcp_server_signal_stop();
        I18n_PrepareShutdown();
        vbdmcp_options_dialog_cleanup();
        break;
    }
    return TRUE;
}
