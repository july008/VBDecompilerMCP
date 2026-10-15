/*
 * vbdmcp.h - VB Decompiler MCP 服务器公共定义（C++17）
 * vbdmcp.h - Common definitions of the VB Decompiler MCP server (C++17)
 *
 * 本项目是 VB Decompiler 插件，作为 MCP（Model Context Protocol）服务器运行，
 * 采用 **Streamable HTTP** 传输（服务端 SSE 推送 + 客户端 POST 请求）
 * 与 JSON-RPC 2.0 协议，全部 JSON 由 yyjson 处理（无手写序列化）。
 *
 * This project is a VB Decompiler plugin that runs as an MCP (Model Context
 * Protocol) server over **Streamable HTTP** (SSE push from the server, POST from
 * the client) with JSON-RPC 2.0. All JSON is handled by yyjson (no hand-written
 * serialization).
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
 * Architecture and threading model (hard constraints):
 *   [network thread] (dedicated background thread, mcp_http.cpp)
 *       accept / select / HTTP parsing / transport handling. It never touches
 *       UI controls, UI objects or the VB Decompiler callback engine.
 *   [UI main thread] (the thread running the VB Decompiler message loop)
 *       Every MCP tool call is dispatched here through a hidden message window
 *       and a task queue (UiTask); the network thread is then woken up to write
 *       the HTTP response. Supports timeout / cancellation token / concurrency cap.
 *       The VB Decompiler callback engine (PluginEngine) can only be called from
 *       the main thread - that is why every tool execution must run there.
 *
 * 安全模型：默认只监听 127.0.0.1；默认不返回任何 CORS 头（禁止 `*`）；
 * 首次工具调用需用户在弹窗中授权；write_* 工具需在设置窗体显式开启写入权限。
 *
 * Security model: listens on 127.0.0.1 only by default; returns no CORS headers
 * by default ("*" is forbidden); the first tool call needs user approval in a
 * dialog; write_* tools require write access enabled in the settings dialog.
 */
#ifndef VBDMCP_H
#define VBDMCP_H

/* 必须先包含 winsock2.h，再包含 windows.h，否则 windows.h 会通过
 * winsock1.h 间接引入旧版 winsock 头与 winsock2.h 冲突。
 * winsock2.h must be included before windows.h; otherwise windows.h pulls in
 * the old winsock headers through winsock1.h and conflicts with winsock2.h. */
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
 * 版本与协议标识 / Version and protocol identifiers
 * ============================================================ */

#define VBDMCP_PLUGIN_NAME    "VB Decompiler MCP Server"
#define VBDMCP_AUTHOR         "vbdmcp project, admin@vb-decompiler.org"
#define VBDMCP_VERSION        "2.0.0"
/* 协议版本：initialize 时按客户端请求的 params.protocolVersion 协商，
 * 支持集合 = { 2025-06-18, 2025-03-26, 2024-11-05 }（见 mcp_http.cpp
 * 的 protocol_version_supported 与 mcp_rpc.cpp 的版本协商）。
 *   - 2025-06-18 与 2025-03-26 对本实现的差异仅为可选特性（结构化输出 /
 *     elicitation 等），可以直接接受；
 *   - 2024-11-05 用于兼容仍基于 2024-11 SDK 的旧客户端；
 *   - 客户端请求不在集合内（或未指定）时回 VBDMCP_PROTOCOL。
 * 客户端会在 initialize 响应后把选中的版本放进后续请求的
 * MCP-Protocol-Version 头，因此头校验必须接受整个集合。
 *
 * Protocol versioning: negotiated from the client's params.protocolVersion
 * during initialize. Supported set = { 2025-06-18, 2025-03-26, 2024-11-05 }
 * (see protocol_version_supported in mcp_http.cpp and the negotiation in
 * mcp_rpc.cpp).
 *   - the differences of 2025-06-18 / 2025-03-26 from this implementation are
 *     optional features only (structured output / elicitation ...), so they are
 *     accepted directly;
 *   - 2024-11-05 keeps older 2024-11 SDK clients working;
 *   - an unsupported or missing request version falls back to VBDMCP_PROTOCOL.
 * After initialize the client puts the chosen version into the
 * MCP-Protocol-Version header of later requests, so header validation must
 * accept the whole set. */
#define VBDMCP_PROTOCOL            "2025-03-26"
#define VBDMCP_PROTOCOL_2025_06_18 "2025-06-18"
#define VBDMCP_PROTOCOL_2025_03_26 "2025-03-26"
#define VBDMCP_PROTOCOL_LEGACY     "2024-11-05"
#define VBDMCP_SESSION_ID     "vbdmcp-session"

/* 跨文件函数 / Cross-file function */
void vbdmcp_http_set_protocol_version(const char *ver);

/* ============================================================
 * 默认设置（可在 VB Decompiler 选项页修改）
 * Defaults (changeable in the VB Decompiler options dialog)
 * ============================================================ */
#define VBDMCP_DEFAULT_ENABLED   0   /* 默认禁用：首次安装时启用由用户在设置窗体里勾选
                                        disabled by default; the user enables it in the settings dialog */
/* 安全模型硬约束：默认只监听回环地址 127.0.0.1。
 * 绝不默认监听 0.0.0.0；用户如需开放局域网，必须在设置窗体高级选项里
 * 手动修改，且会弹出高危警告。
 *
 * Hard security constraint: listens on the loopback address 127.0.0.1 only.
 * 0.0.0.0 is never a default; LAN exposure must be enabled manually in the
 * advanced settings and triggers a high-risk warning. */
#define VBDMCP_DEFAULT_BIND      "127.0.0.1"
#define VBDMCP_LOOPBACK_BIND     "127.0.0.1"
#define VBDMCP_DEFAULT_PORT      3023
#define VBDMCP_DEFAULT_ALLOW_WRITE 0  /* 默认只读模式 / read-only by default */

/* ============================================================
 * MCP 标准错误分类码 / MCP error codes
 *
 * -32600 ~ -32603 为 JSON-RPC 2.0 标准错误码；
 * -32000 ~ -32099 为 JSON-RPC 规范保留给实现自定义的服务端错误码。
 * 这里只使用标准/保留区间，保证 MCP 客户端能统一解析。
 *   VBDMCP_ERR_PERMISSION      会话未授权 或 只读模式下调用 write_*
 *   VBDMCP_ERR_INVALID_PARAMS  参数非法 / 未知工具
 *   VBDMCP_ERR_TIMEOUT         任务超时
 *   VBDMCP_ERR_INTERNAL        服务端内部错误
 *
 * -32600..-32603 are the standard JSON-RPC 2.0 error codes; -32000..-32099 are
 * reserved by the JSON-RPC spec for implementation-defined server errors. Only
 * these ranges are used, so MCP clients can parse them uniformly.
 *   VBDMCP_ERR_PERMISSION      session unauthorized, or write_* in read-only mode
 *   VBDMCP_ERR_INVALID_PARAMS  invalid parameters / unknown tool
 *   VBDMCP_ERR_TIMEOUT         task timeout
 *   VBDMCP_ERR_INTERNAL        internal server error
 * ============================================================ */
#define VBDMCP_ERR_OK               0
#define VBDMCP_ERR_PERMISSION      (-32001)
#define VBDMCP_ERR_TIMEOUT         (-32002)
#define VBDMCP_ERR_INTERNAL        (-32603)
#define VBDMCP_ERR_INVALID_PARAMS  (-32602)
#define VBDMCP_ERR_PARSE           (-32700)
#define VBDMCP_ERR_INVALID_REQUEST (-32600)
#define VBDMCP_ERR_METHOD_NOT_FOUND (-32601)
/* 工具业务错误（如项目未加载）：作为 tools/call 的 isError:true 结果返回，
 * 不占用 JSON-RPC 错误通道。
 * Tool business errors (e.g. project not loaded): returned as tools/call
 * isError:true results, never through the JSON-RPC error channel. */
#define VBDMCP_ERR_BUSINESS        (-32020)
/* 并发任务超限（UI 任务队列已满）—— MCP 保留区间内的服务端错误码
 * Concurrency limit hit (UI task queue full) - server error inside the reserved range. */
#define VBDMCP_ERR_BUSY            (-32003)

/* ============================================================
 * 连接、请求与任务队列限制 / Connection, request and task-queue limits
 * ============================================================ */
/* 并发连接上限。必须明显小于 FD_SETSIZE(64)：select 用的 fd_set 还要放下
 * listener，且 Windows 的 FD_SET 在满时**静默忽略**新句柄（那些连接会
 * 不被 select 监视、静默挂起）。60 + listener = 61，留出余量。
 *
 * Concurrent connection cap. Must stay well below FD_SETSIZE(64): the select
 * fd_set must also hold the listener, and Windows FD_SET silently ignores new
 * handles once full (those connections would hang unmonitored). 60 + listener
 * = 61 leaves headroom. */
#define VBDMCP_MAX_CONNECTIONS  60
/* 单个 HTTP 请求头部长度上限（防御超长/畸形头部）
 * Max HTTP header size (defends against oversized / malformed headers). */
#define VBDMCP_MAX_HEADER_SIZE  (16 * 1024)
#define VBDMCP_MAX_BODY_SIZE    (64 * 1024 * 1024)
/* 同时在 UI 主线程排队/执行的 MCP 任务上限：超出立即返回 429 / 繁忙错误，
 * 防止多个 MCP 请求同时触发大项目反编译把 UI 卡死。
 *
 * Max MCP tasks queued or running on the UI main thread; beyond it the server
 * answers 429 / busy immediately, so parallel requests cannot freeze the UI
 * through heavy decompilation jobs. */
#define VBDMCP_MAX_PENDING_TASKS 8
/* 单个 MCP 任务从入队到完成的最长时间（毫秒），可用 VBDMCP_TASK_TIMEOUT_MS 环境变量覆盖
 * Max time from enqueue to completion for one MCP task (ms); overridable with the
 * VBDMCP_TASK_TIMEOUT_MS environment variable. */
#define VBDMCP_TASK_TIMEOUT_MS  30000
/* project_text 单次写入上限：HTTP body 上限的 1/4，避免一次性把 VB Decompiler
 * 内部缓冲塞爆。超过则工具返回错误，客户端可分段写入。
 *
 * Single-write limit for project_text: one quarter of the HTTP body limit, so
 * the internal VB Decompiler buffers are never flooded at once. Larger payloads
 * get an error and clients can write in chunks. */
#define VBDMCP_MAX_PROJECT_TEXT (16 * 1024 * 1024)

/* ============================================================
 * 窗口消息 / Window messages
 * ============================================================ */
#define VBDMCP_WM_TASK         (WM_USER + 13)  /* 网络线程 → UI 主线程任务投递 / network thread -> UI main thread task dispatch */

/* ============================================================
 * 全局状态（在 plugin_main.cpp / mcp_http.cpp 中定义）
 * Global state (defined in plugin_main.cpp / mcp_http.cpp)
 *
 * 跨线程共享状态一律使用 std::atomic：
 *   - g_allow_write 由 UI 主线程写、网络线程读（tools/list 与 callTool 校验）；
 *   - 绑定地址用互斥量保护，通过 vbdmcp_bind_address() 访问。
 *
 * Everything shared across threads is std::atomic:
 *   - g_allow_write is written by the UI thread and read by the network thread
 *     (tools/list and callTool checks);
 *   - the bind address is mutex-protected and accessed via vbdmcp_bind_address().
 * ============================================================ */
#include <atomic>
#include <string>

extern std::atomic<int> g_enabled;
extern std::atomic<int> g_port;
extern std::atomic<int> g_allow_write;   /* 1 = 允许 MCP 客户端写入项目数据（UI 配置，默认 0）
                                            1 = MCP clients may write project data (UI setting, default 0) */

/* 当前监听地址快照（线程安全）/ 设置监听地址
 * Snapshot of the current bind address (thread-safe) / set the bind address. */
std::string vbdmcp_bind_address(void);
void        vbdmcp_set_bind_address(const char *addr);

/* VB Decompiler 主窗口句柄（VBDecompilerPluginLoad 传入并保存）。
 * 授权弹窗等 UI 操作在 UI 主线程执行时作为父窗口使用；可为 NULL。
 *
 * VB Decompiler main window handle (passed in and saved by
 * VBDecompilerPluginLoad). Used as the parent for UI operations on the UI main
 * thread (authorization dialog etc.); may be NULL. */
extern HWND  g_main_hwnd;

/* ============================================================
 * 简易内存分配（plugin_main.cpp 实现）
 * Simple allocation (implemented in util.cpp)
 * 全部用 CRT malloc/free，VB Decompiler 进程关闭时会一并回收
 * All CRT malloc/free; everything is reclaimed when the VB Decompiler process exits.
 * ============================================================ */
void *vbdmcp_alloc(size_t n);
void *vbdmcp_calloc(size_t n);
char *vbdmcp_strdup(const char *s);
void  vbdmcp_free(void *p);

/* 取**当前 DLL 自身**的模块句柄（传入本模块函数地址）。
 * 插件被 VB Decompiler 加载，宿主 hInstance 不是我们，
 * 所以加载本 DLL 的资源（语言包 / 对话框模板）必须用它。
 *
 * Module handle of THIS DLL (resolved from an address inside the module).
 * VB Decompiler loads the plugin, so the host hInstance is not ours; loading
 * this DLL's resources (language packs / dialog templates) must use this handle. */
HMODULE vbdmcp_self_module(void);

/* UTF-8 → ANSI(GBK) 转换：返回 vbdmcp_alloc 的新串（调用方 vbdmcp_free）。
 * i18n 里 `_()` 给 UTF-8、`_A()` 给 GBK；本函数用于转换"不在语言包内、
 * 但源码中是 UTF-8 字面量"的文本（如设置窗体内的客户端接入说明），
 * 以及 MCP 写入工具把客户端 UTF-8 参数转回引擎需要的 ANSI 字节。
 *
 * UTF-8 -> ANSI(GBK): returns a new alloc'ed string (caller frees with vbdmcp_free).
 * In i18n, `_()` yields UTF-8 and `_A()` yields GBK; this function converts text
 * that is not in the language pack but is a UTF-8 literal in the sources (e.g.
 * the client setup guides in the settings dialog), and converts client UTF-8
 * arguments back to the ANSI bytes the engine expects (write tools). */
char *vbdmcp_str_to_gbk(const char *src);

/* ANSI(GBK / CP_ACP) → UTF-8 转换：返回 vbdmcp_alloc 的新串（调用方 vbdmcp_free）。
 * VB Decompiler 引擎返回 ANSI 字节，而 MCP 报文必须是 UTF-8；工具层
 * （mcp_tools.cpp）在把引擎文本放进 JSON 之前用本函数转换（默认 utf-8 模式）。
 * 与 vbdmcp_str_to_gbk 互为逆向。
 *
 * ANSI(GBK / CP_ACP) -> UTF-8: returns a new alloc'ed string (caller frees with
 * vbdmcp_free). The VB Decompiler engine returns ANSI bytes while MCP messages
 * must be UTF-8; the tool layer (mcp_tools.cpp) converts engine text with this
 * function before it enters JSON (default utf-8 mode). Inverse of
 * vbdmcp_str_to_gbk. */
char *vbdmcp_acp_to_utf8(const char *src);

/* ============================================================
 * 分级日志：写入 %TEMP%\vbdmcp.log
 * Leveled log: writes to %TEMP%\vbdmcp.log
 * 默认关闭；设置环境变量 VBDMCP_DEBUG_LOG=1 启用。
 * 阈值由 VBDMCP_LOG_LEVEL 环境变量控制（0=ERROR..3=DEBUG，默认 2=INFO）。
 * Disabled by default; enable with VBDMCP_DEBUG_LOG=1.
 * Threshold via VBDMCP_LOG_LEVEL (0=ERROR..3=DEBUG, default 2=INFO).
 *
 * 安全约束：日志绝不写入项目源码/反编译文本等敏感内容，
 * 只记录连接、工具名、权限判定与错误分类等元信息。
 * Security rule: the log never contains project source or decompiled text - only
 * metadata such as connections, tool names, permission decisions and error classes.
 * ============================================================ */
#define VBDMCP_LOG_ERROR 0
#define VBDMCP_LOG_WARN  1
#define VBDMCP_LOG_INFO  2
#define VBDMCP_LOG_DEBUG 3

void vbdmcp_logf(int level, const char *fmt, ...);
/* 等价于 vbdmcp_logf(VBDMCP_LOG_INFO, ...)：元信息类日志的统一入口
 * Equivalent to vbdmcp_logf(VBDMCP_LOG_INFO, ...): the common entry for informational logs. */
void vbdmcp_log(const char *fmt, ...);

/* ============================================================
 * 字符串工具 / String utilities
 * ============================================================ */
int  vbd_streq(const char *a, const char *b);

/* ============================================================
 * VB Decompiler SDK 封装（vbdec_sdk.cpp）
 * VB Decompiler SDK wrapper (vbdec_sdk.cpp)
 * 全部调用通过 g_engine 在主线程执行（避免 Callback Engine 线程安全问题）
 * Every call goes through g_engine on the main thread (callback-engine safety).
 * ============================================================ */

/* vlType 命令常量（与官方 SDK 一致）
 * vlType command constants (matching the official SDK). */
enum VBD_vlType {
    VBD_GetVBProject = 1,
    VBD_SetVBProject = 2,
    VBD_GetFileName = 3,
    VBD_IsNativeCompilation = 4,
    VBD_ClearAllBuffers = 5,
    VBD_GetCompiler = 6,
    VBD_IsPacked = 7,
    VBD_SetStackCheckBoxValue = 8,
    VBD_SetAnalyzerCheckBoxValue = 9,
    VBD_GetVBFormName = 10,
    VBD_SetVBFormName = 11,
    VBD_GetVBForm = 12,
    VBD_SetVBForm = 13,
    VBD_GetVBFormCount = 14,
    VBD_GetSubMain = 20,
    VBD_SetSubMain = 21,
    VBD_GetModuleName = 30,
    VBD_SetModuleName = 31,
    VBD_GetModule = 32,
    VBD_SetModule = 33,
    VBD_GetModuleStringReferences = 34,
    VBD_SetModuleStringReferences = 35,
    VBD_GetModuleCount = 36,
    VBD_GetModuleFunctionName = 40,
    VBD_SetModuleFunctionName = 41,
    VBD_GetModuleFunctionAddress = 42,
    VBD_SetModuleFunctionAddress = 43,
    VBD_GetModuleFunction = 44,
    VBD_SetModuleFunction = 45,
    VBD_GetModuleFunctionStrRef = 46,
    VBD_SetModuleFunctionStrRef = 47,
    VBD_GetModuleFunctionCount = 48,
    VBD_GetActiveText = 50,
    VBD_SetActiveText = 51,
    VBD_GetActiveDisasmText = 52,
    VBD_SetActiveDisasmText = 53,
    VBD_SetActiveTextLine = 54,
    VBD_GetActiveModuleCoordinats = 55,
    VBD_GetVBDecompilerPath = 56,
    VBD_GetModuleFunctionCode = 57,
    VBD_SetStatusBarText = 58,
    VBD_GetFrxIconCount = 60,
    VBD_GetFrxIconOffset = 61,
    VBD_GetFrxIconSize = 62,
    VBD_GetModuleFunctionDisasm = 70,
    VBD_SetModuleFunctionDisasm = 71,
    VBD_UpdateAll = 100
};

/* 引擎函数类型 / Engine function type */
typedef void* (__stdcall *TVBPluginEngine)(int vlType, int vlNumber, int vlFnNumber, void *vlNewValue);

/* 引擎指针（plugin_main.cpp 初始化）
 * Engine pointer (initialized in plugin_main.cpp). */
extern TVBPluginEngine g_engine;

/* 初始化引擎：在 VBDecompilerPluginLoad 调用
 * Initializes the engine: called from VBDecompilerPluginLoad. */
void vbd_sdk_init(TVBPluginEngine engine);

/* GetValue：返回值是 new alloc 的 ANSI 字符串（调用方 vbdmcp_free）
 * GetValue: returns a new alloc'ed ANSI string (caller frees with vbdmcp_free). */
char *vbd_get_value(int vlType, int vlNumber, int vlFnNumber);

/* SetValue：ANSI 字符串入参
 * SetValue: takes an ANSI string. */
void vbd_set_value(int vlType, const char *value, int vlNumber, int vlFnNumber);

/* SetStatusBarText（快捷封装）
 * SetStatusBarText (convenience wrapper). */
void vbd_set_status(const char *msg);

/* 项目加载状态探测（0=未加载，1=已加载）。SDK 不暴露 ReloadFile 等 API，
 * "Plugins 菜单点确定"是 VB Decompiler 私有路径，vbdmcp 无法模拟；
 * 如果返回 0，调用方应返回带解决方法的错误给 MCP 客户端。
 *
 * Project load probe (0 = not loaded, 1 = loaded). The SDK exposes no
 * ReloadFile API and "click OK in the Plugins dialog" is a VB Decompiler
 * private path that vbdmcp cannot simulate; on 0 the caller should return an
 * error with the remedy to the MCP client. */
int vbd_is_project_loaded(void);

/* ============================================================
 * HTTP 服务器与 JSON-RPC 处理（mcp_http.cpp / mcp_rpc.cpp）
 * HTTP server and JSON-RPC handling (mcp_http.cpp / mcp_rpc.cpp)
 *
 * 线程模型：
 *   - mcp_http.cpp 内部有且只有一个后台网络线程负责 socket / HTTP；
 *   - JSON-RPC 方法处理（含全部工具调用）被封装成 UiTask 投递到 UI 主线程执行，
 *     网络线程等待结果后回写响应（支持超时 / 取消 / 并发上限）。
 *
 * Threading:
 *   - mcp_http.cpp runs exactly one background network thread for socket / HTTP;
 *   - JSON-RPC method handling (including every tool call) is wrapped into a
 *     UiTask dispatched to the UI main thread; the network thread waits for the
 *     result and writes the response (timeout / cancel / concurrency cap).
 * ============================================================ */
int  vbdmcp_server_start(void);
void vbdmcp_server_stop(void);
/* DllMain(DLL_PROCESS_DETACH) 专用：只置停机标志，不 join 线程（避免 loader 锁死锁）
 * DllMain(DLL_PROCESS_DETACH) only: signal stop without joining (avoids loader-lock deadlock). */
void vbdmcp_server_signal_stop(void);

/* 服务器是否正在运行（网络线程已完成 bind）
 * Whether the server is running (the network thread finished binding). */
int  vbdmcp_server_is_running(void);

/* 根据开关应用设置：enable=1 时启动或按需重启（端口变化），enable=0 时停止。
 * 返回 1 表示已应用、0 表示失败（如端口被占用）。失败时旧服务器会被关停。
 *
 * Applies settings: enable=1 starts or restarts the server when the port changed;
 * enable=0 stops it. Returns 1 on success, 0 on failure (e.g. port in use); the
 * old server is stopped on failure. */
int  vbdmcp_server_apply(int enable, int port);

/* 处理一个完整的 JSON-RPC 2.0 body，返回响应 JSON 字符串（new alloc）。
 *   client_info       客户端连接信息（如 "127.0.0.1:52341"），用于首次 callTool
 *                     授权弹窗展示；可为 NULL。
 *   out_is_initialize [out] 可为 NULL；请求是 initialize 时置 1
 *                     （网络线程据此分配 Mcp-Session-Id）。
 * 返回 "" 表示 notification（应 202 空 body），返回 NULL 表示内存不足。
 * **必须在 UI 主线程调用**（会触发 VB Decompiler Callback Engine 与授权弹窗）。
 *
 * Processes one complete JSON-RPC 2.0 body and returns the response JSON string
 * (new alloc).
 *   client_info       client connection info (e.g. "127.0.0.1:52341") shown in the
 *                     first callTool authorization dialog; may be NULL.
 *   out_is_initialize [out] may be NULL; set to 1 for an initialize request
 *                     (the network thread uses it to allocate Mcp-Session-Id).
 * "" means a notification (answer 202 with an empty body); NULL means out of memory.
 * MUST be called on the UI main thread (it triggers the VB Decompiler callback
 * engine and the authorization dialog). */
char *vbdmcp_process_jsonrpc(const char *body, const char *client_info,
                            int *out_is_initialize);

/* 权限（read/write 工具集）变化后广播 notifications/tools/list_changed，
 * 让已连接的 MCP 客户端自动重拉工具清单。由 UI 主线程调用。
 *
 * Broadcasts notifications/tools/list_changed after a permission (read/write tool
 * set) change, so connected MCP clients re-fetch the tool list. UI main thread only. */
void vbdmcp_notify_tools_list_changed(void);

/* ============================================================
 * 会话准入授权（首次 callTool 弹窗）
 * Session authorization (dialog on the first callTool)
 *
 * MCP 客户端在真正调用工具（tools/call）前会先 listTools 获取能力，
 * 该预检请求**不弹窗**。当第一条非 listTools 的业务请求到达时，才在
 * UI 主线程弹出授权对话框，展示客户端连接信息。
 *
 * An MCP client calls listTools first to discover capabilities; that pre-check
 * never prompts. The authorization dialog pops up on the UI main thread when the
 * first non-listTools business request arrives, showing the client connection info.
 *
 * 返回 1 = 本次会话已授权（放行）；0 = 用户拒绝（拒绝所有后续请求）。
 * 服务重启后授权状态重置，下次新连接再次弹窗。
 * 授权只控制会话准入，不替代"允许写入"开关。
 *
 * Returns 1 when the session is authorized, 0 when the user denied (all later
 * requests are rejected). A server restart resets the state so the next
 * connection is asked again. Authorization only gates session admission; it does
 * not replace the "allow write" switch.
 * ============================================================ */
int  vbdmcp_session_authorize(const char *client_info);
void vbdmcp_session_reset(void);

/* ============================================================
 * 配置（config.cpp）/ Configuration (config.cpp)
 * 存储于注册表 / Stored in the registry：
 *   HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\MCP
 * ============================================================ */
void vbdmcp_config_load(int *port, char *bind_out, size_t bind_cap, int *allow_write);
void vbdmcp_config_save(int port, const char *bind, int allow_write);

/* 可信 CORS origin 列表（逗号分隔；空串=未配置 → 不返回任何 CORS 头）。
 * 禁止使用 "*"。仅当请求 Origin 命中列表时才回该 origin。
 *
 * Trusted CORS origin list (comma-separated; empty = not configured -> no CORS
 * headers at all). "*" is forbidden. An origin is echoed only when it matches
 * the list. */
void vbdmcp_config_get_cors_origins(char *out, size_t cap);

/* ============================================================
 * UI：设置对话框（options_dialog.cpp）
 * UI: settings dialog (options_dialog.cpp)
 *
 * 注意：菜单项本身由 VB Decompiler 自动添加（读取 VBDecompilerPluginName
 *   的返回值），我们不挂 subclass。用户点击菜单 → VB Decompiler 再次调用
 *   VBDecompilerPluginLoad → plugin_main.cpp 在第 2+ 次调用时弹设置窗体。
 *
 * Note: the menu item is added by VB Decompiler itself (it reads the return value
 *   of VBDecompilerPluginName); no subclass is installed. Clicking the menu makes
 *   VB Decompiler call VBDecompilerPluginLoad again, and plugin_main.cpp opens
 *   the settings dialog on the 2nd and later calls.
 * ============================================================ */

/* 弹出设置窗体（模态）。允许在窗体打开前 VB Decompiler 没启动时调用。
 * Opens the settings dialog (modal). Safe to call before the server has started. */
void vbdmcp_options_dialog(HWND hParent);

/* 进程退出时清理设置窗体的 GDI 资源（字体等）。在 DllMain DLL_PROCESS_DETACH 调用。
 * Releases the settings dialog's GDI objects (font etc.) at process exit; called
 * from DllMain DLL_PROCESS_DETACH. */
void vbdmcp_options_dialog_cleanup(void);

#endif /* VBDMCP_H */
