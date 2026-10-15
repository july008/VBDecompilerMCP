/*
 * mcp_rpc.cpp - JSON-RPC 2.0 分发 + MCP 协议层（yyjson 版）
 *
 * 处理以下 JSON-RPC 方法（MCP 协议要求）：
 *   - initialize           → serverInfo + capabilities
 *   - ping                 → {}
 *   - tools/list           → 动态工具清单（只读模式仅 vb_read_*）
 *   - tools/call           → 调用工具（含权限二次校验 + 会话准入授权）
 *   - notifications/*      → 不返回响应（HTTP 202 空 body）
 *
 * 错误处理：
 *   - 协议层错误使用 MCP/JSON-RPC 标准错误对象 {code, message}，全部经 yyjson 构建。
 *     VBMCP_ERR_PERMISSION / VBMCP_ERR_INVALID_PARAMS / VBMCP_ERR_TIMEOUT /
 *     VBMCP_ERR_INTERNAL 分别映射到标准/保留错误码。
 *   - 工具业务错误（如项目未加载）走 tools/call 的 isError 内容通道，不占用错误通道。
 *   - 所有入口 try/except 保护（SEH），单个请求失败不会让 VB Decompiler UI 进程崩溃。
 *
 * 线程模型：全部同步在 VB Decompiler 主线程（WSAAsyncSelect 消息循环）执行，
 * 避免 Callback Engine 线程安全问题；授权弹窗同样在主线程弹出。
 */
#include "vbmcp.h"
#include "i18n.h"
#include "mcp_tools.h"   /* 内含 yyjson.h */
#include <atomic>
#include <exception>
#include <stdio.h>

/* ============================================================
 * 会话准入授权（首次 callTool 弹窗）
 *
 * listTools / initialize / ping 属于预检，不弹窗；只有真正调用工具
 * （tools/call）才触发一次授权弹窗。授权只控制会话准入，不替代
 * "允许写入"开关。
 *
 * 状态用 std::atomic：写入方是 UI 主线程（弹窗），重置方可能是后台网络线程
 * （所有连接断开 / 服务停止），读取方是 UI 主线程。
 * ============================================================ */
static std::atomic<bool> g_session_authorized{ false };  /* 本次会话已授权 */
static std::atomic<bool> g_session_denied{ false };      /* 用户已拒绝：拒绝所有后续请求 */

int vbmcp_session_authorize(const char *client_info)
{
    if (g_session_authorized.load()) return 1;
    if (g_session_denied.load())     return 0;

    /* _AF：先翻译再格式化，并转成 ANSI(GBK) 供 MessageBoxA 使用 */
    char *msg = _AF("An MCP client is trying to access this VB Decompiler project data.\n\nClient: %s\n\nChoose \"Yes\" to allow this session, or \"No\" to deny all requests.\n(Read-only mode is still enforced separately by the write permission switch.)",
                    client_info ? client_info : "unknown");

    /* UI 主线程弹窗：Yes = 允许本次会话，No = 拒绝。
     * 参数选择（有实测教训）：
     *   - 用 MB_SETFOREGROUND，**不要** MB_TOPMOST：TOPMOST 的模态框在
     *     宿主自己也有模态窗口（设置窗体等）时会出现"置顶但无法正常激活"
     *     的怪象，表现为标题栏拖不动、按钮点不动（用户实测反馈）；
     *   - owner 必须是有效窗口：优先宿主主窗口，拿不到时退回当前前台窗口，
     *     避免产生无 owner 的孤立对话框（Z 序与激活都会更不可靠）。 */
    HWND owner = (g_main_hwnd && IsWindow(g_main_hwnd)) ? g_main_hwnd
                                                       : GetForegroundWindow();
    const int r = MessageBoxA(owner, msg,
                              _A("VB Decompiler MCP Server - Access Request"),
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    if (r == IDYES) {
        g_session_authorized.store(true);
        vbmcp_logf(VBMCP_LOG_INFO, "session authorized by user (client=%s)",
                   client_info ? client_info : "unknown");
        return 1;
    }
    g_session_denied.store(true);
    vbmcp_logf(VBMCP_LOG_WARN, "session denied by user (client=%s)",
               client_info ? client_info : "unknown");
    return 0;
}

void vbmcp_session_reset(void)
{
    g_session_authorized.store(false);
    g_session_denied.store(false);
}

/* ============================================================
 * 响应构造（yyjson）
 * ============================================================ */

/* 序列化并释放 mutable doc（带 ALLOW_INVALID_UNICODE，因结果可能含 GBK 字节）。 */
static char *rpc_serialize(yyjson_mut_doc *doc)
{
    char *s = NULL;
    if (doc) {
        s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
        yyjson_mut_doc_free(doc);
    }
    return s;
}

/* 新建响应 doc：{"jsonrpc":"2.0","id":<id>}。id 缺失时写 null。 */
static yyjson_mut_doc *resp_new(yyjson_mut_val **root_out, const yyjson_val *idv)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) { yyjson_mut_doc_free(doc); return NULL; }
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    yyjson_mut_val *mid = idv ? yyjson_val_mut_copy(doc, idv) : NULL;
    yyjson_mut_obj_add_val(doc, root, "id", mid ? mid : yyjson_mut_null(doc));

    *root_out = root;
    return doc;
}

/* 往已建好的响应 root 上挂 error 对象。 */
static void add_error(yyjson_mut_doc *doc, yyjson_mut_val *root, int code, const char *msg)
{
    yyjson_mut_val *err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_strcpy(doc, err, "message", msg ? msg : "error");
    yyjson_mut_obj_add_val(doc, root, "error", err);
}

/* 独立构造一个错误响应（含 jsonrpc/id）。 */
static char *make_error_resp(const yyjson_val *idv, int code, const char *msg)
{
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = resp_new(&root, idv);
    if (!doc) return NULL;
    add_error(doc, root, code, msg);
    return rpc_serialize(doc);
}

/* 从工具错误负载 {"error":"..."} 中取出错误文本（new alloc，可为 NULL）。
 * 错误文本来自 lang 表（UTF-8），因此读侧解析安全。 */
static char *payload_error_text(const char *payload)
{
    if (!payload) return NULL;
    yyjson_doc *d = yyjson_read(payload, strlen(payload),
                                YYJSON_READ_ALLOW_INVALID_UNICODE);
    if (!d) return NULL;
    yyjson_val *r = yyjson_doc_get_root(d);
    const char *msg = yyjson_get_str(yyjson_obj_get(r, "error"));
    char *out = msg ? vbmcp_strdup(msg) : NULL;
    yyjson_doc_free(d);
    return out;
}

/* 把工具结果负载包装成 MCP tools/call 结果：
 *   {"content":[{"type":"text","text":"<payload>"}],"isError":bool} */
static void add_tool_result(yyjson_mut_doc *doc, yyjson_mut_val *root,
                            const char *payload, int is_error)
{
    yyjson_mut_val *result  = yyjson_mut_obj(doc);
    yyjson_mut_val *content = yyjson_mut_arr(doc);
    yyjson_mut_val *item    = yyjson_mut_arr_add_obj(doc, content);
    yyjson_mut_obj_add_strcpy(doc, item, "type", "text");
    yyjson_mut_obj_add_strcpy(doc, item, "text", payload ? payload : "");
    yyjson_mut_obj_add_val(doc, result, "content", content);
    yyjson_mut_obj_add_bool(doc, result, "isError", is_error);
    yyjson_mut_obj_add_val(doc, root, "result", result);
}

/* 把协商后的协议版本写入 HTTP 层全局变量，使后续响应头带 MCP-Protocol-Version。 */
void vbmcp_set_active_protocol_version(const char *ver)
{
    vbmcp_http_set_protocol_version(ver);
}

/* ============================================================
 * initialize 结果
 * ============================================================ */
/* 协议版本协商：客户端请求的版本在支持集合内 → 回同一版本（客户端会把它放进
 * 后续请求的 MCP-Protocol-Version 头）；否则回默认版本。支持集合必须与
 * mcp_http.cpp: protocol_version_supported() 保持一致。 */
static const char *negotiate_protocol_version(yyjson_val *req)
{
    yyjson_val *params = yyjson_obj_get(req, "params");
    yyjson_val *pv = (params && yyjson_is_obj(params))
                         ? yyjson_obj_get(params, "protocolVersion") : NULL;
    const char *want = (pv && yyjson_is_str(pv)) ? yyjson_get_str(pv) : NULL;
    if (want && (vb_streq(want, VBMCP_PROTOCOL_2025_06_18) ||
                 vb_streq(want, VBMCP_PROTOCOL_2025_03_26) ||
                 vb_streq(want, VBMCP_PROTOCOL_LEGACY))) {
        return want;
    }
    return VBMCP_PROTOCOL;
}

static void add_initialize_result(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                  const char *protocol_version)
{
    yyjson_mut_val *result = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, result, "protocolVersion",
                              protocol_version ? protocol_version : VBMCP_PROTOCOL);

    /* 只声明实际实现的能力。listChanged=1：写入权限变化时服务端会广播
     * notifications/tools/list_changed（客户端仅在声明 true 时监听该通知）；
     * prompts / resources 未实现，不再声明，避免客户端探测得到 -32601。 */
    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_val *tools = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, tools, "listChanged", 1);
    yyjson_mut_obj_add_val(doc, caps, "tools", tools);
    yyjson_mut_obj_add_val(doc, result, "capabilities", caps);

    yyjson_mut_val *info = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, info, "name", VBMCP_PLUGIN_NAME);
    yyjson_mut_obj_add_strcpy(doc, info, "version", VBMCP_VERSION);
    yyjson_mut_obj_add_val(doc, result, "serverInfo", info);

    yyjson_mut_obj_add_strcpy(doc, result, "instructions",
        "VB Decompiler MCP server. Tools operate on the currently opened "
        "decompiled VB project. Read tools (vb_read_*) are always available; "
        "write tools (vb_write_*) require the user to enable write access in "
        "the plugin settings. Get info first, then list/get/set modules, forms, "
        "functions. Text fields in tool results are UTF-8 by default; pass "
        "encoding:\"gbk\" to a tool to receive/send the project's raw ANSI "
        "bytes instead (legacy behavior, for code pages such as GBK).");

    yyjson_mut_obj_add_val(doc, root, "result", result);
}

/* ============================================================
 * tools/call 处理
 * ============================================================ */
static void handle_tools_call(yyjson_mut_doc *doc, yyjson_mut_val *root,
                              yyjson_val *req, const char *client_info)
{
    yyjson_val *params = yyjson_obj_get(req, "params");
    const char *tool_name = NULL;
    yyjson_val *arguments = NULL;
    if (params && yyjson_is_obj(params)) {
        yyjson_val *n = yyjson_obj_get(params, "name");
        if (n && yyjson_is_str(n)) tool_name = yyjson_get_str(n);
        arguments = yyjson_obj_get(params, "arguments");
    }

    if (!tool_name || !*tool_name) {
        add_error(doc, root, VBMCP_ERR_INVALID_PARAMS, _("Tool name is empty"));
        return;
    }

    /* 会话准入授权：仅真正调用工具时触发（listTools 属于预检，不弹窗）。
     * 本函数在 UI 主线程执行（任务队列保证），因此 MessageBox 是安全的。 */
    if (!vbmcp_session_authorize(client_info)) {
        vbmcp_logf(VBMCP_LOG_WARN, "tools/call rejected: session not authorized (tool=%s)",
                   tool_name);
        add_error(doc, root, VBMCP_ERR_PERMISSION, _("Access denied: the user rejected this MCP session. All subsequent requests from this session will be rejected."));
        return;
    }

    const int allow_write = g_allow_write.load();

    /* 防御兜底（双重权限校验的第二道）：即使客户端绕过 tools/list 强行
     * 调用 vb_write_*，callTool 入口也必须二次校验权限，否则直接返回
     * MCP 权限错误，绝不执行业务。 */
    if (mcp_tool_is_write(tool_name) && !allow_write) {
        vbmcp_logf(VBMCP_LOG_WARN,
                   "tools/call denied: write tool in read-only mode (tool=%s)", tool_name);
        add_error(doc, root, VBMCP_ERR_PERMISSION, _("Write access is disabled. All vb_write_* tools are unavailable in read-only mode. Ask the user to enable \"Allow MCP client to write project data\" in the VB Decompiler MCP settings."));
        return;
    }

    int ecode = VBMCP_ERR_OK;
    char *payload = mcp_dispatch_tool(tool_name, arguments, allow_write, &ecode);
    vbmcp_logf(VBMCP_LOG_INFO, "tools/call tool=%s write=%d err=%d",
               tool_name, mcp_tool_is_write(tool_name), ecode);

    if (!payload) {
        add_error(doc, root, VBMCP_ERR_INTERNAL, _("Out of memory"));
        return;
    }

    /* 权限 / 参数 / 内部错误 → JSON-RPC 标准错误通道。 */
    if (ecode == VBMCP_ERR_PERMISSION || ecode == VBMCP_ERR_INVALID_PARAMS ||
        ecode == VBMCP_ERR_INTERNAL  || ecode == VBMCP_ERR_TIMEOUT) {
        char *msg = payload_error_text(payload);
        add_error(doc, root, ecode, msg ? msg : "tool error");
        vbmcp_free(msg);
        vbmcp_free(payload);
        return;
    }

    /* 成功 / 业务错误 → tools/call 内容通道（isError 标记）。 */
    add_tool_result(doc, root, payload, ecode != VBMCP_ERR_OK);
    vbmcp_free(payload);
}

/* ============================================================
 * 主要入口
 * ============================================================ */
static char *process_jsonrpc_impl(const char *body, const char *client_info,
                                  int *out_is_initialize);

char *vbmcp_process_jsonrpc(const char *body, const char *client_info,
                            int *out_is_initialize)
{
    /* 入口防御：任何 C++ 异常都必须转成 MCP 标准错误，
     * 绝不允许把 VB Decompiler UI 进程带崩（外层任务队列另有一层保护）。 */
    try {
        return process_jsonrpc_impl(body, client_info, out_is_initialize);
    } catch (const std::exception &e) {
        vbmcp_logf(VBMCP_LOG_ERROR, "jsonrpc threw std::exception: %s", e.what());
    } catch (...) {
        vbmcp_logf(VBMCP_LOG_ERROR, "jsonrpc threw unknown exception");
    }
    return make_error_resp(NULL, VBMCP_ERR_INTERNAL, "Internal error");
}

static char *process_jsonrpc_impl(const char *body, const char *client_info,
                                  int *out_is_initialize)
{
    if (!body || !*body)
        return make_error_resp(NULL, VBMCP_ERR_PARSE, "Parse error: empty body");

    /* 只解析一次；容忍非法 Unicode 与 BOM（客户端可能回传 GBK 项目文本）。 */
    yyjson_doc *req_doc = yyjson_read(body, strlen(body),
                                      YYJSON_READ_ALLOW_INVALID_UNICODE | YYJSON_READ_ALLOW_BOM);
    if (!req_doc)
        return make_error_resp(NULL, VBMCP_ERR_PARSE, "Parse error: invalid JSON");

    yyjson_val *req = yyjson_doc_get_root(req_doc);
    if (!req || !yyjson_is_obj(req)) {
        yyjson_doc_free(req_doc);
        return make_error_resp(NULL, VBMCP_ERR_INVALID_REQUEST, "Invalid request: not a JSON object");
    }

    yyjson_val *idv = yyjson_obj_get(req, "id");
    yyjson_val *m = yyjson_obj_get(req, "method");
    const char *method = (m && yyjson_is_str(m)) ? yyjson_get_str(m) : NULL;

    /* JSON-RPC 2.0：无 id 的请求（notification，含 notifications/* 全部方法
     * 以及未来扩展通知）一律不产生响应体 —— HTTP 层回 202 空 body。
     * 不再按方法名白名单，未知通知同样静默丢弃（绝不能给通知回错误对象）。 */
    if (!idv) {
        yyjson_doc_free(req_doc);
        return vbmcp_strdup("");
    }

    if (!method) {
        char *e = make_error_resp(idv, VBMCP_ERR_INVALID_REQUEST, "Missing method");
        yyjson_doc_free(req_doc);
        return e;
    }

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = resp_new(&root, idv);
    if (!doc) { yyjson_doc_free(req_doc); return NULL; }

    if (vb_streq(method, "initialize")) {
        if (out_is_initialize) *out_is_initialize = 1;   /* 网络线程据此分配会话 id */
        /* 版本协商：客户端请求版本在支持集合内则回它（SDK 会据此设置
         * 后续请求头）；否则回默认版本。/health 展示协商结果。 */
        const char *chosen = negotiate_protocol_version(req);
        vbmcp_set_active_protocol_version(chosen);
        add_initialize_result(doc, root, chosen);
    }
    else if (vb_streq(method, "ping")) {
        yyjson_mut_obj_add_val(doc, root, "result", yyjson_mut_obj(doc));
    }
    else if (vb_streq(method, "tools/list")) {
        /* 动态工具清单：只读模式仅 vb_read_*，开启写入后追加 vb_write_*。
         * 客户端据此动态隐藏 write 工具，无需重启连接。 */
        const int allow_write = g_allow_write.load();
        yyjson_mut_val *result = yyjson_mut_obj(doc);
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        mcp_build_tools_into(doc, arr, allow_write);
        yyjson_mut_obj_add_val(doc, result, "tools", arr);
        yyjson_mut_obj_add_val(doc, root, "result", result);
        vbmcp_logf(VBMCP_LOG_INFO, "tools/list allow_write=%d count=%d",
                   allow_write, mcp_local_tool_count(allow_write));
    }
    else if (vb_streq(method, "tools/call")) {
        handle_tools_call(doc, root, req, client_info);
    }
    else {
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "Method not found: %s", method);
        add_error(doc, root, VBMCP_ERR_METHOD_NOT_FOUND, buf);
    }

    char *out = rpc_serialize(doc);
    yyjson_doc_free(req_doc);
    return out;
}