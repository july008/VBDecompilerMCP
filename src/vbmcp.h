/*
 * vbmcp.h - VB Decompiler MCP 服务器公共定义（C++17）
 *
 * 本项目是 VB Decompiler 插件，作为 MCP（Model Context Protocol）服务器运行，
 * 采用 **Streamable HTTP** 传输（服务端 SSE 推送 + 客户端 POST 请求）
 * 与 JSON-RPC 2.0 协议，全部 JSON 由 yyjson 处理（无手写序列化）。
 *
 * 架构与线程模型（硬约束）：
 *   [网络线程]（后台独立线程，mcp_http.cpp）
 *       accept / select / HTTP 解析 / 传输层协议处理。
 *       绝不触碰 UI 控件、UI 对象或 VB Decompiler Callback Engine。
 *   [UI 主线程]（VB Decompiler 消息循环所在线程）
 *       全部 MCP 工具调用都通过隐藏消息窗口 + 任务队列（UiTask）投递到这里执行，
 *       完成后唤醒网络线程回写 HTTP 响应；支持超时 / 取消令牌 / 并发上限。
 *       VB Decompiler 的 Callback Engine（PluginEngine）只能在主线程调用，
 *       这是所有工具执行必须回到主线程的根本原因。
 *
 * 安全模型：默认只监听 127.0.0.1；默认不返回任何 CORS 头（禁止 `*`）；
 * 首次工具调用需用户在弹窗中授权；write_* 工具需在设置窗体显式开启写入权限。
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
#define VBMCP_VERSION        "2.0.0"
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
/* 安全模型硬约束：默认只监听回环地址 127.0.0.1。
 * 绝不默认监听 0.0.0.0；用户如需开放局域网，必须在设置窗体高级选项里
 * 手动修改，且会弹出高危警告。 */
#define VBMCP_DEFAULT_BIND      "127.0.0.1"
#define VBMCP_LOOPBACK_BIND     "127.0.0.1"
#define VBMCP_DEFAULT_PORT      3023
#define VBMCP_DEFAULT_ALLOW_WRITE 0  /* 默认只读模式 */

/* ============================================================
 * MCP 标准错误分类码
 *
 * -32600 ~ -32603 为 JSON-RPC 2.0 标准错误码；
 * -32000 ~ -32099 为 JSON-RPC 规范保留给实现自定义的服务端错误码。
 * 这里只使用标准/保留区间，保证 MCP 客户端能统一解析。
 *   VBMCP_ERR_PERMISSION      会话未授权 或 只读模式下调用 write_*
 *   VBMCP_ERR_INVALID_PARAMS  参数非法 / 未知工具
 *   VBMCP_ERR_TIMEOUT         任务超时
 *   VBMCP_ERR_INTERNAL        服务端内部错误
 * ============================================================ */
#define VBMCP_ERR_OK               0
#define VBMCP_ERR_PERMISSION      (-32001)
#define VBMCP_ERR_TIMEOUT         (-32002)
#define VBMCP_ERR_INTERNAL        (-32603)
#define VBMCP_ERR_INVALID_PARAMS  (-32602)
#define VBMCP_ERR_PARSE           (-32700)
#define VBMCP_ERR_INVALID_REQUEST (-32600)
#define VBMCP_ERR_METHOD_NOT_FOUND (-32601)
/* 工具业务错误（如项目未加载）：作为 tools/call 的 isError:true 结果返回，
 * 不占用 JSON-RPC 错误通道。 */
#define VBMCP_ERR_BUSINESS        (-32020)
/* 并发任务超限（UI 任务队列已满）—— MCP 保留区间内的服务端错误码 */
#define VBMCP_ERR_BUSY            (-32003)

/* ============================================================
 * 选项页控件ID（避免与VB Decompiler内部冲突，从1000起）
 * ============================================================ */

/* ============================================================
 * 连接、请求与任务队列限制
 * ============================================================ */
#define VBMCP_MAX_CONNECTIONS  64
/* 单个 HTTP 请求头部长度上限（防御超长/畸形头部） */
#define VBMCP_MAX_HEADER_SIZE  (16 * 1024)
#define VBMCP_MAX_BODY_SIZE    (64 * 1024 * 1024)
/* 同时在 UI 主线程排队/执行的 MCP 任务上限：超出立即返回 429 / 繁忙错误，
 * 防止多个 MCP 请求同时触发大项目反编译把 UI 卡死。 */
#define VBMCP_MAX_PENDING_TASKS 8
/* 单个 MCP 任务从入队到完成的最长时间（毫秒），可用 VBMCP_TASK_TIMEOUT_MS 覆盖 */
#define VBMCP_TASK_TIMEOUT_MS  30000
/* project_text 单次写入上限：HTTP body 上限的 1/4，避免一次性把 VB Decompiler
 * 内部缓冲塞爆。超过则工具返回错误，客户端可分段写入。 */
#define VBMCP_MAX_PROJECT_TEXT (16 * 1024 * 1024)

/* ============================================================
 * 窗口消息
 * ============================================================ */
#define VBMCP_WM_TASK         (WM_USER + 13)  /* 网络线程 → UI 主线程任务投递 */

/* ============================================================
 * 全局状态（在 plugin_main.cpp / mcp_http.cpp 中定义）
 *
 * 跨线程共享状态一律使用 std::atomic（C++ 迁移后的硬性要求）：
 *   - g_allow_write 由 UI 主线程写、网络线程读（tools/list 与 callTool 校验）；
 *   - 绑定地址用互斥量保护，通过 vbmcp_bind_address() 访问。
 * ============================================================ */
#include <atomic>
#include <string>

extern std::atomic<int> g_enabled;
extern std::atomic<int> g_port;
extern std::atomic<int> g_allow_write;   /* 1 = 允许 MCP 客户端写入项目数据（UI 配置，默认 0） */

/* 当前监听地址快照（线程安全）/ 设置监听地址 */
std::string vbmcp_bind_address(void);
void        vbmcp_set_bind_address(const char *addr);

/* VB Decompiler 主窗口句柄（VBDecompilerPluginLoad 传入并保存）。
 * 授权弹窗等 UI 操作在 UI 主线程执行时作为父窗口使用；可为 NULL。 */
extern HWND  g_main_hwnd;

/* ============================================================
 * 简易内存分配（plugin_main.cpp 实现）
 * 全部用 CRT malloc/free，VB Decompiler进程关闭时会一并回收
 * ============================================================ */
void *vbmcp_alloc(size_t n);
void *vbmcp_calloc(size_t n);
char *vbmcp_strdup(const char *s);
void  vbmcp_free(void *p);

/* 取**当前 DLL 自身**的模块句柄（传入本模块函数地址）。
 * 插件被 VB Decompiler 加载，宿主 hInstance 不是我们，
 * 所以加载本 DLL 的资源（语言包 / 对话框模板）必须用它。 */
HMODULE vbmcp_self_module(void);

/* UTF-8 → ANSI(GBK) 转换：返回 vbmcp_alloc 的新串（调用方 vbmcp_free）。
 * i18n 里 `_()` 给 UTF-8、`_A()` 给 GBK；本函数用于转换"不在语言包内、
 * 但源码中是 UTF-8 字面量"的文本（如设置窗体内的客户端接入说明），
 * 以及 MCP 写入工具把客户端 UTF-8 参数转回引擎需要的 ANSI 字节。 */
char *vbmcp_str_to_gbk(const char *src);

/* ANSI(GBK / CP_ACP) → UTF-8 转换：返回 vbmcp_alloc 的新串（调用方 vbmcp_free）。
 * VB Decompiler 引擎返回 ANSI 字节，而 MCP 报文必须是 UTF-8；工具层
 * （mcp_tools.cpp）在把引擎文本放进 JSON 之前用本函数转换（默认 utf-8 模式）。
 * 与 vbmcp_str_to_gbk 互为逆向。 */
char *vbmcp_acp_to_utf8(const char *src);

/* ============================================================
 * 分级日志：写入 %TEMP%\vbmcp.log
 * 默认关闭；设置环境变量 VBMCP_DEBUG_LOG=1 启用。
 * 阈值由 VBMCP_LOG_LEVEL 环境变量控制（0=ERROR..3=DEBUG，默认 2=INFO）。
 *
 * 安全约束：日志绝不写入项目源码/反编译文本等敏感内容，
 * 只记录连接、工具名、权限判定与错误分类等元信息。
 * ============================================================ */
#define VBMCP_LOG_ERROR 0
#define VBMCP_LOG_WARN  1
#define VBMCP_LOG_INFO  2
#define VBMCP_LOG_DEBUG 3

void vbmcp_logf(int level, const char *fmt, ...);
/* 等价于 vbmcp_logf(VBMCP_LOG_INFO, ...)：元信息类日志的统一入口 */
void vbmcp_log(const char *fmt, ...);

/* ============================================================
 * 字符串工具
 * ============================================================ */
int  vb_streq(const char *a, const char *b);

/* 修剪字符串两端的空白（返回新分配字符串） */

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

/* 引擎指针（plugin_main.cpp 初始化） */
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
 * HTTP 服务器与 JSON-RPC 处理（mcp_http.cpp / mcp_rpc.cpp）
 *
 * 线程模型：
 *   - mcp_http.cpp 内部有且只有一个后台网络线程负责 socket / HTTP；
 *   - JSON-RPC 方法处理（含全部工具调用）被封装成 UiTask 投递到 UI 主线程执行，
 *     网络线程等待结果后回写响应（支持超时 / 取消 / 并发上限）。
 * ============================================================ */
int  vbmcp_server_start(void);
void vbmcp_server_stop(void);
/* DllMain(DLL_PROCESS_DETACH) 专用：只置停机标志，不 join 线程（避免 loader 锁死锁） */
void vbmcp_server_signal_stop(void);

/* 服务器是否正在运行（网络线程已完成 bind） */
int  vbmcp_server_is_running(void);

/* 根据开关应用设置：enable=1 时启动或按需重启（端口变化），enable=0 时停止。
 * 返回 1 表示已应用、0 表示失败（如端口被占用）。失败时旧服务器会被关停。 */
int  vbmcp_server_apply(int enable, int port);

/* 处理一个完整的 JSON-RPC 2.0 body，返回响应 JSON 字符串（new alloc）。
 *   client_info       客户端连接信息（如 "127.0.0.1:52341"），用于首次 callTool
 *                     授权弹窗展示；可为 NULL。
 *   out_is_initialize [out] 可为 NULL；请求是 initialize 时置 1
 *                     （网络线程据此分配 Mcp-Session-Id）。
 * 返回 "" 表示 notification（应 202 空 body），返回 NULL 表示内存不足。
 * **必须在 UI 主线程调用**（会触发 VB Decompiler Callback Engine 与授权弹窗）。 */
char *vbmcp_process_jsonrpc(const char *body, const char *client_info,
                            int *out_is_initialize);

/* 权限（read/write 工具集）变化后广播 notifications/tools/list_changed，
 * 让已连接的 MCP 客户端自动重拉工具清单。由 UI 主线程调用。 */
void vbmcp_notify_tools_list_changed(void);

/* ============================================================
 * 会话准入授权（首次 callTool 弹窗）
 *
 * MCP 客户端在真正调用工具（tools/call）前会先 listTools 获取能力，
 * 该预检请求**不弹窗**。当第一条非 listTools 的业务请求到达时，才在
 * UI 主线程弹出授权对话框，展示客户端连接信息。
 *
 * 返回 1 = 本次会话已授权（放行）；0 = 用户拒绝（拒绝所有后续请求）。
 * 服务重启后授权状态重置，下次新连接再次弹窗。
 * 授权只控制会话准入，不替代"允许写入"开关。
 * ============================================================ */
int  vbmcp_session_authorize(const char *client_info);
void vbmcp_session_reset(void);

/* ============================================================
 * 配置（config.cpp）
 * 存储于注册表：
 *   HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\MCP
 * ============================================================ */
void vbmcp_config_load(int *port, char *bind_out, size_t bind_cap, int *allow_write);
void vbmcp_config_save(int port, const char *bind, int allow_write);

/* 可信 CORS origin 列表（逗号分隔；空串=未配置 → 不返回任何 CORS 头）。
 * 禁止使用 "*"。仅当请求 Origin 命中列表时才回该 origin。 */
void vbmcp_config_get_cors_origins(char *out, size_t cap);

/* ============================================================
 * UI：设置对话框（options_dialog.cpp）
 *
 * 注意：菜单项本身由 VB Decompiler 自动添加（读取 VBDecompilerPluginName
 *   的返回值），我们不挂 subclass。用户点击菜单 → VB Decompiler 再次调用
 *   VBDecompilerPluginLoad → plugin_main.cpp 在第 2+ 次调用时弹设置窗体。
 * ============================================================ */

/* 弹出设置窗体（模态）。允许在窗体打开前 VB Decompiler 没启动时调用。 */
void vbmcp_options_dialog(HWND hParent);

/* 进程退出时清理设置窗体的 GDI 资源（字体等）。在 DllMain DLL_PROCESS_DETACH 调用。 */
void vbmcp_options_dialog_cleanup(void);

#endif /* VBMCP_H */
