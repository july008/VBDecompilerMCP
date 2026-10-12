/*
 * vbmcp.h - VB Decompiler MCP服务器公共定义
 *
 * C语言实现的VB Decompiler插件，作为MCP（Model Context Protocol）服务器运行。
 * 该插件由VB Decompiler在加载时调用 VBDecompilerPluginLoad 入口点启动，
 * 在内部通过隐藏窗口 + WSAAsyncSelect 实现非阻塞HTTP服务器（SSE 可选 + POST），
 * 遵循MCP Streamable HTTP传输与JSON-RPC 2.0协议。
 *
 * 架构：
 *   - 主线程（VB Decompiler）：socket事件派发、HTTP请求解析与发送、JSON-RPC分发。
 *   - 工作线程：执行MCP工具调用（通过Callback Engine访问VB Decompiler），
 *     完成后PostMessage回主线程发送响应（避免阻塞VB Decompiler主线程UI）。
 *   - VB Decompiler的Callback Engine（PluginEngine）必须从VB Decompiler的
 *     线程上下文调用（避免竞争），所以工具调用必须投递到主线程执行。
 */
#ifndef VBMCP_H
#define VBMCP_H

/* 必须先包含 winsock2.h，再包含 windows.h，否则 windows.h 会通过
 * winsock1.h 间接引入旧版 winsock 头与 winsock2.h 冲突。 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

/* ============================================================
 * 版本与协议标识
 * ============================================================ */
/* Avoid _WINSOCK_DEPRECATED_NO_WARNINGS check for WSAAsyncSelect (deprecated but still needed for Win32 message-window IO). */
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif

#define VBMCP_PLUGIN_NAME    "VB Decompiler MCP Server"
#define VBMCP_AUTHOR         "vbmcp project, admin@vb-decompiler.org"
#define VBMCP_VERSION        "1.0.0"
/* 默认协议版本：本实现优先支持 2025-03-26（Streamable HTTP 规范）。
 * 同时也支持 2024-11-05 以兼容 CodeBuddy 等仍基于 2024-11 SDK 的客户端。
 * initialize 处理函数会按客户端请求版本协商。 */
#define VBMCP_PROTOCOL       "2025-03-26"
#define VBMCP_PROTOCOL_LEGACY "2024-11-05"
#define VBMCP_SESSION_ID     "vbmcp-session"

/* 跨文件函数 */
void vbmcp_http_set_protocol_version(const char *ver);

/* ============================================================
 * 默认设置（可在VB Decompiler选项页修改）
 * ============================================================ */
#define VBMCP_DEFAULT_ENABLED   0   /* 默认禁用：首次安装时启用由用户在设置窗体里勾选 */
#define VBMCP_DEFAULT_BIND      "127.0.0.1"
#define VBMCP_DEFAULT_PORT      3023

/* ============================================================
 * 选项页控件ID（避免与VB Decompiler内部冲突，从1000起）
 * ============================================================ */
#define VBMCP_OPT_ENABLED     1100
#define VBMCP_OPT_LABEL_IP    1101
#define VBMCP_OPT_BIND        1102
#define VBMCP_OPT_LABEL_PORT  1103
#define VBMCP_OPT_PORT        1104
#define VBMCP_OPT_RESTORE     1105

/* ============================================================
 * 连接与缓冲区限制
 * ============================================================ */
#define VBMCP_MAX_CONNECTIONS  64
#define VBMCP_RECV_BUFSIZE     (64 * 1024)
#define VBMCP_MAX_BODY_SIZE    (64 * 1024 * 1024)
/* project_text 单次写入上限：HTTP body 上限的 1/4，避免一次性把 VB Decompiler
 * 内部缓冲塞爆。超过则工具返回错误，客户端可分段写入。 */
#define VBMCP_MAX_PROJECT_TEXT (16 * 1024 * 1024)

/* ============================================================
 * 窗口消息
 * ============================================================ */
#define VBMCP_WM_LISTEN      (WM_USER + 10)  /* 监听 socket 事件 */
#define VBMCP_WM_CLIENT      (WM_USER + 11)  /* 客户端 socket 事件 */
#define VBMCP_WM_TOOL_DONE   (WM_USER + 12)  /* 工具调用完成（lParam=结果） */

#define VBMCP_TIMER_CHECK    0x3A01          /* 监听 socket 周期性健康检查 */

/* ============================================================
 * 全局状态（在 plugin_main.c / mcp_http.c 中定义）
 * ============================================================ */
extern int   g_enabled;
extern int   g_port;
extern char *g_bindings;
extern HWND  g_mcp_hwnd;
extern SOCKET g_listen_sock;

/* ============================================================
 * 简易内存分配（plugin_main.c 实现）
 * 全部用 CRT malloc/free，VB Decompiler进程关闭时会一并回收
 * ============================================================ */
void *vbmcp_alloc(size_t n);
void *vbmcp_calloc(size_t n);
char *vbmcp_strdup(const char *s);
char *vbmcp_strndup(const char *s, size_t n);
void  vbmcp_free(void *p);

/* ============================================================
 * 调试日志：写入 %TEMP%\vbmcp.log
 * 通过设置环境变量 VBMCP_DEBUG_LOG=1 启用
 * ============================================================ */
void vbmcp_log(const char *fmt, ...);

/* ============================================================
 * 字符串工具
 * ============================================================ */
int  vb_streq(const char *a, const char *b);
int  vb_str_empty(const char *s);

/* 修剪字符串两端的空白（返回新分配字符串） */
char *vb_str_trim_dup(const char *s, size_t n);

/* ============================================================
 * VB Decompiler SDK 封装（vbdec_sdk.h）
 * 全部调用通过 g_engine 在主线程执行（避免Callback Engine线程安全问题）
 * ============================================================ */

/* vlType 命令常量（与官方SDK一致） */
enum vb_vlType {
    VB_GetVBProject = 1,
    VB_SetVBProject = 2,
    VB_GetFileName = 3,
    VB_IsNativeCompilation = 4,
    VB_ClearAllBuffers = 5,
    VB_GetCompiler = 6,
    VB_IsPacked = 7,
    VB_SetStackCheckBoxValue = 8,
    VB_SetAnalyzerCheckBoxValue = 9,
    VB_GetVBFormName = 10,
    VB_SetVBFormName = 11,
    VB_GetVBForm = 12,
    VB_SetVBForm = 13,
    VB_GetVBFormCount = 14,
    VB_GetSubMain = 20,
    VB_SetSubMain = 21,
    VB_GetModuleName = 30,
    VB_SetModuleName = 31,
    VB_GetModule = 32,
    VB_SetModule = 33,
    VB_GetModuleStringReferences = 34,
    VB_SetModuleStringReferences = 35,
    VB_GetModuleCount = 36,
    VB_GetModuleFunctionName = 40,
    VB_SetModuleFunctionName = 41,
    VB_GetModuleFunctionAddress = 42,
    VB_SetModuleFunctionAddress = 43,
    VB_GetModuleFunction = 44,
    VB_SetModuleFunction = 45,
    VB_GetModuleFunctionStrRef = 46,
    VB_SetModuleFunctionStrRef = 47,
    VB_GetModuleFunctionCount = 48,
    VB_GetActiveText = 50,
    VB_SetActiveText = 51,
    VB_GetActiveDisasmText = 52,
    VB_SetActiveDisasmText = 53,
    VB_SetActiveTextLine = 54,
    VB_GetActiveModuleCoordinats = 55,
    VB_GetVBDecompilerPath = 56,
    VB_GetModuleFunctionCode = 57,
    VB_SetStatusBarText = 58,
    VB_GetFrxIconCount = 60,
    VB_GetFrxIconOffset = 61,
    VB_GetFrxIconSize = 62,
    VB_GetModuleFunctionDisasm = 70,
    VB_SetModuleFunctionDisasm = 71,
    VB_UpdateAll = 100
};

/* 引擎函数类型 */
typedef void* (__stdcall *TVBPluginEngine)(int vlType, int vlNumber, int vlFnNumber, void *vlNewValue);

/* 引擎指针（plugin_main.c 初始化） */
extern TVBPluginEngine g_engine;

/* 初始化引擎：在 VBDecompilerPluginLoad 调用 */
void vb_sdk_init(TVBPluginEngine engine);

/* GetValue：返回值是 new alloc 的 ANSI 字符串（调用方 vbmcp_free） */
char *vb_get_value(int vlType, int vlNumber, int vlFnNumber);

/* SetValue：ANSI 字符串入参 */
void vb_set_value(int vlType, const char *value, int vlNumber, int vlFnNumber);

/* SetStatusBarText（快捷封装） */
void vb_set_status(const char *msg);

/* 项目加载状态探测（0=未加载，1=已加载）。SDK 不暴露 ReloadFile 等 API，
 * "Plugins 菜单点确定"是 VB Decompiler 私有路径，vbmcp 无法模拟；
 * 如果返回 0，调用方应返回带解决方法的错误给 MCP 客户端。 */
int vb_is_project_loaded(void);

/* ============================================================
 * HTTP 服务器与 JSON-RPC 处理（mcp_http.h / mcp_rpc.h）
 * ============================================================ */
int  vbmcp_server_start(void);
void vbmcp_server_stop(void);

/* 服务器是否正在运行（vbmcp_listen_sock != INVALID_SOCKET） */
int  vbmcp_server_is_running(void);

/* 根据开关应用设置：enable=1 时启动或按需重启（端口变化），enable=0 时停止。
 * 返回 1 表示已应用、0 表示失败（如端口被占用）。失败时旧服务器会被关停。 */
int  vbmcp_server_apply(int enable, int port);

/* 处理一个完整的 JSON-RPC 2.0 body，返回响应 JSON 字符串（new alloc）。
 * 返回 "" 表示 notification（应 200 空 body），返回 NULL 表示解析/未支持错误。 */
char *vbmcp_process_jsonrpc(const char *body);

/* ============================================================
 * 配置（config.c）
 * 文件：%LOCALAPPDATA%\vbmcp\settings.ini
 * ============================================================ */
void vbmcp_config_load(int *port, char *bind_out, size_t bind_cap);
void vbmcp_config_save( int port, const char *bind);

/* ============================================================
 * UI：设置对话框（options_dialog.c）
 *
 * 注意：菜单项本身由 VB Decompiler 自动添加（读取 VBDecompilerPluginName
 *   的返回值），我们不挂 subclass。用户点击菜单 → VB Decompiler 再次调用
 *   VBDecompilerPluginLoad → plugin_main.c 在第 2+ 次调用时弹设置窗体。
 * ============================================================ */

/* 弹出设置窗体（模态）。允许在窗体打开前 VB Decompiler 没启动时调用。 */
void vbmcp_options_dialog(HWND hParent);

/* 进程退出时清理设置窗体的 GDI 资源（字体等）。在 DllMain DLL_PROCESS_DETACH 调用。 */
void vbmcp_options_dialog_cleanup(void);

#endif /* VBMCP_H */
