/*
 * mcp_http.cpp - MCP Streamable HTTP 传输层（C++17）
 *
 * ============================== 线程模型（硬约束） ==============================
 *   [网络线程]  后台独立线程：accept / select / HTTP 解析 / 传输层协议处理。
 *               **绝不触碰任何 UI 控件、UI 对象或 VB Decompiler Callback Engine。**
 *   [UI 主线程]  VB Decompiler 消息循环所在线程：执行全部 MCP 工具调用。
 *               网络线程把请求封装成 UiTask，通过消息窗口 PostMessage 投递；
 *               执行完成后唤醒网络线程回写 HTTP 响应。
 *
 *   - 所有需要 UI / 工程数据的请求都经 UiTask 投递，绝不跨线程直接调用；
 *   - 每个任务支持超时（VBMCP_TASK_TIMEOUT_MS，默认 30s）与取消令牌
 *     （客户端断开 / 超时 / 服务停止时置位；尚未执行的任务会被直接丢弃）；
 *   - 并发上限 VBMCP_MAX_PENDING_TASKS：队列满时立即返回 429 + MCP 繁忙错误，
 *     避免多个 MCP 请求同时触发大项目反编译导致 UI 卡死 / 竞态。
 *
 * ============================ 传输规范（MCP 2025-03-26） ============================
 *   POST /mcp        JSON-RPC 请求（Accept 需含 application/json）
 *   GET  /mcp        升级为 SSE 长连接（Accept 需含 text/event-stream）
 *   DELETE /mcp      终止会话
 *   OPTIONS *        仅当 Origin 命中用户配置的可信列表才回 CORS 头
 *   GET  /health     健康检查（非 MCP 端点，yyjson 构造）
 *
 * 安全模型：默认只监听 127.0.0.1；默认不返回任何 CORS 头（禁止 `*`）；
 * 请求体大小上限 + 全局基础限流 + 畸形 JSON 由 yyjson 安全拒绝。
 */
#include "vbmcp.h"
#include "mcp_tools.h"
#include "i18n.h"
#include "vbmcp_mem.hpp"
#include "yyjson.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ============================================================
 * 全局状态定义
 * ============================================================ */
std::atomic<int> g_enabled{ VBMCP_DEFAULT_ENABLED };
std::atomic<int> g_port{ VBMCP_DEFAULT_PORT };
/* 允许 MCP 客户端写入项目数据（UI 配置，默认只读）。UI 主线程写、网络线程读。 */
std::atomic<int> g_allow_write{ VBMCP_DEFAULT_ALLOW_WRITE };

namespace {

/* ---------------- 常量 ---------------- */
constexpr int kSelectTimeoutMs  = 50;      /* 循环轮询间隔（同时保证关停响应及时） */
constexpr int kSseHeartbeatMs   = 15000;   /* SSE 保活注释帧间隔 */
constexpr int kRateLimitPerSec  = 200;     /* 全局基础限流 */
constexpr int kTaskQueueLimit   = VBMCP_MAX_PENDING_TASKS;

/* ---------------- 绑定地址（UI 线程写，网络线程读） ---------------- */
std::mutex  g_bind_mu;
std::string g_bind_addr = VBMCP_DEFAULT_BIND;

/* ---------------- 协议版本 ---------------- */
/* 协议版本串：UI 线程在 initialize 时写入（vbmcp_http_set_protocol_version），
 * 网络线程在 /health 里读。原来是 char[32] + strncpy_s —— 写 32 字节过程中
 * 并发读可能拿到半截字符串（和 i18n 那次同类问题）。
 * 现在改成"UI 线程分配一份、原子发布指针"：读者永远看到完整字符串。
 * 旧值故意不释放（进程级、每客户端会话才换一次，几十字节）。 */
std::atomic<const char*> g_active_protocol_version{ VBMCP_PROTOCOL };

/* ---------------- 连接对象 ---------------- */
struct Connection;

/* ============================================================
 * UI 任务（网络线程 → UI 主线程的单向投递单元）
 * ============================================================ */
struct UiTask {
    std::string       body;       /* 原始 JSON-RPC 请求体 */
    std::string       peer;       /* 客户端 "ip:port"，授权弹窗展示 */
    std::string       response;   /* UI 主线程写回的响应体 */
    int               http_status = 200;
    std::atomic<bool> cancelled{ false };   /* 取消令牌（网络线程置位） */
    std::atomic<bool> done{ false };
    std::chrono::steady_clock::time_point enqueue;

    std::mutex              mtx;   /* 只用于"等待完成"，不保护上面字段 */
    std::condition_variable cv;
};

struct Connection {
    vbmcp::SocketHandle sock;
    std::string   peer;
    std::string   recv_buf;
    std::string   send_buf;
    std::size_t   send_off = 0;
    std::string   session_id;                 /* MCP 会话 id（initialize 分配） */
    bool          session_id_pending = false; /* 需要在响应里回带会话 id */
    bool          close_after_send = false;
    bool          is_sse = false;
    bool          want_write = false;
    std::shared_ptr<UiTask> pending;
    std::chrono::steady_clock::time_point last_active;
};

/* ---------------- 任务队列（网络线程写，UI 主线程读） ---------------- */
std::mutex                            g_task_mu;
std::vector<std::shared_ptr<UiTask>>  g_task_queue;
HWND                                  g_task_hwnd = nullptr;

/* ---------------- 网络线程状态 ---------------- */
std::thread       g_net_thread;
std::atomic<bool> g_net_running{ false };
std::atomic<bool> g_net_inited{ false };
std::atomic<bool> g_winsock_inited{ false };

/* ---------------- tools/list_changed 广播暂存 ---------------- */
std::mutex              g_list_changed_mu;
std::string             g_list_changed_payload;
std::atomic<bool>       g_list_changed_pending{ false };

/* ---------------- 基础限流 ---------------- */
DWORD g_rate_sec   = 0;
int   g_rate_count = 0;

/* true once a client has used the current session (reset authorization when all
   clients disconnect). */
std::atomic<bool> g_session_was_used{ false };

bool rate_limit_allow()
{
    DWORD sec = GetTickCount() / 1000;
    if (sec != g_rate_sec) { g_rate_sec = sec; g_rate_count = 0; }
    if (g_rate_count >= kRateLimitPerSec) return false;
    g_rate_count++;
    return true;
}

/* ============================================================
 * 可信 CORS origin（绝不使用 "*"）
 * ============================================================ */
bool origin_is_trusted(const std::string& origin)
{
    if (origin.empty()) return false;
    if (_stricmp(origin.c_str(), "null") == 0) return false;

    char list[1024] = {0};
    vbmcp_config_get_cors_origins(list, sizeof(list));
    if (!list[0]) return false;   /* 未配置可信列表 → 一律不发 CORS 头 */

    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char* start = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - start);
        while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t')) n--;
        if (n == 1 && start[0] == '*') continue;   /* 忽略通配：禁止 "*" */
        if (n == origin.size() && _strnicmp(start, origin.c_str(), n) == 0) return true;
    }
    return false;
}

std::string cors_headers(const std::string& origin)
{
    if (!origin_is_trusted(origin)) return std::string();
    return "Access-Control-Allow-Origin: " + origin +
           "\r\nVary: Origin\r\n"
           "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type, Accept, "
           "MCP-Protocol-Version, Mcp-Session-Id\r\n"
           "Access-Control-Max-Age: 86400\r\n";
}

/* ============================================================
 * HTTP 报文工具
 * ============================================================ */
const char* status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

void append_int(std::string& out, long long v)
{
    char buf[24];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", v);
    out += buf;
}

/* 用 yyjson 构造 JSON-RPC 标准错误文档（禁止手写 JSON 序列化） */
std::string json_error_document(int code, const char* message)
{
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) return "{}";
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_null(doc, root, "id");
    yyjson_mut_val* err = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, err, "code", code);
    yyjson_mut_obj_add_strcpy(doc, err, "message", message ? message : "error");
    yyjson_mut_obj_add_val(doc, root, "error", err);

    char* s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, nullptr);
    std::string out(s ? s : "{}");
    yyjson_mut_doc_free(doc);
    return out;
}

std::string peer_name(SOCKET s)
{
    struct sockaddr_storage ss;
    int slen = (int)sizeof(ss);
    if (getpeername(s, (struct sockaddr*)&ss, &slen) != 0) return "unknown";

    char host[48] = {0};
    unsigned short port = 0;
    if (ss.ss_family == AF_INET) {
        auto* a = (struct sockaddr_in*)&ss;
        InetNtopA(AF_INET, &a->sin_addr, host, sizeof(host));
        port = ntohs(a->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        auto* a6 = (struct sockaddr_in6*)&ss;
        InetNtopA(AF_INET6, &a6->sin6_addr, host, sizeof(host));
        port = ntohs(a6->sin6_port);   /* C++ 下 sockaddr_in6 不与 sockaddr_in 组成联合体 */
    }
    if (!host[0]) return "unknown";

    char out[80];
    _snprintf_s(out, sizeof(out), _TRUNCATE, "%s:%u", host, port);
    return out;
}

std::string make_session_id()
{
    GUID g{};
    CoCreateGuid(&g);
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                (unsigned long)g.Data1, (unsigned short)g.Data2,
                (unsigned short)g.Data3,
                (unsigned)g.Data4[0], (unsigned)g.Data4[1],
                (unsigned)g.Data4[2], (unsigned)g.Data4[3], (unsigned)g.Data4[4],
                (unsigned)g.Data4[5], (unsigned)g.Data4[6], (unsigned)g.Data4[7]);
    return std::string(buf);
}

/* ============================================================
 * 连接写出
 * ============================================================ */
int vbmcp_flush(Connection* c)
{
    while (c->send_off < c->send_buf.size()) {
        int r = send(c->sock.get(), c->send_buf.data() + c->send_off,
                     (int)(c->send_buf.size() - c->send_off), 0);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) { c->want_write = true; return 1; }
            return 0;
        }
        if (r == 0) return 0;
        c->send_off += (size_t)r;
    }
    c->send_buf.clear();
    c->send_off = 0;
    c->want_write = false;
    return 1;
}

void http_send_raw(Connection* c, int status, const char* content_type,
                   const std::string& origin, const std::string& extra_headers,
                   const std::string& body, bool keep_alive)
{
    std::string head;
    head.reserve(512);
    head += "HTTP/1.1 ";
    append_int(head, status);
    head += ' ';
    head += status_text(status);
    head += "\r\nContent-Type: ";
    head += content_type;
    head += "\r\nContent-Length: ";
    append_int(head, (long long)body.size());
    head += "\r\nConnection: ";
    head += keep_alive ? "keep-alive" : "close";
    head += "\r\n";
    if (!extra_headers.empty()) { head += extra_headers; head += "\r\n"; }
    std::string cors = cors_headers(origin);
    if (!cors.empty()) { head += cors; head += "\r\n"; }
    head += "\r\n";

    c->send_buf += head;
    c->send_buf += body;
    c->close_after_send = !keep_alive;
    c->want_write = true;
    vbmcp_flush(c);
}

void http_send_json(Connection* c, int status, const std::string& origin,
                    const std::string& body, const std::string& extra_headers = std::string())
{
    http_send_raw(c, status, "application/json; charset=utf-8", origin,
                  extra_headers, body, false);
}

/* transport 层错误：HTTP 状态码承载传输语义，JSON-RPC 错误对象承载协议语义 */
void http_send_error(Connection* c, int http_status, int jsonrpc_code,
                     const char* message, const std::string& origin = std::string())
{
    http_send_json(c, http_status, origin, json_error_document(jsonrpc_code, message));
    vbmcp_logf(VBMCP_LOG_WARN, "transport error http=%d code=%d msg=%s (client=%s)",
               http_status, jsonrpc_code, message ? message : "", c->peer.c_str());
}

/* ============================================================
 * SSE（服务端推送）
 * ============================================================ */
void sse_begin(Connection* c, const std::string& origin)
{
    std::string extra;
    if (!c->session_id.empty()) extra = "Mcp-Session-Id: " + c->session_id;
    http_send_raw(c, 200, "text/event-stream; charset=utf-8", origin, extra,
                  ": vbmcp sse stream open\n\n", true);
    c->is_sse = true;
    c->last_active = std::chrono::steady_clock::now();
    vbmcp_logf(VBMCP_LOG_INFO, "sse stream opened (client=%s session=%s)",
               c->peer.c_str(), c->session_id.c_str());
}

void sse_push_json(Connection* c, const std::string& json)
{
    if (!c->is_sse) return;
    c->send_buf += "event: message\ndata: ";
    c->send_buf += json;
    c->send_buf += "\n\n";
    c->want_write = true;
    vbmcp_flush(c);
}

void sse_heartbeat_all(std::vector<std::unique_ptr<Connection>>& conns)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& c : conns) {
        if (!c || !c->is_sse || !c->sock.valid()) continue;
        const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - c->last_active).count();
        if (idle < kSseHeartbeatMs) continue;
        c->send_buf += ": ping\n\n";
        c->want_write = true;
        c->last_active = now;
        vbmcp_flush(c.get());
    }
}

/* 若 UI 主线程请求广播 tools/list_changed，则推送给所有 SSE 连接 */
void flush_list_changed_notification(std::vector<std::unique_ptr<Connection>>& conns)
{
    if (!g_list_changed_pending.exchange(false)) return;

    std::string payload;
    {
        std::lock_guard<std::mutex> lk(g_list_changed_mu);
        payload = g_list_changed_payload;
    }
    if (payload.empty()) return;

    for (auto& c : conns) {
        if (c && c->is_sse && c->sock.valid()) {
            sse_push_json(c.get(), payload);
            vbmcp_logf(VBMCP_LOG_INFO, "sse push notifications/tools/list_changed (client=%s)",
                       c->peer.c_str());
        }
    }
}

/* ============================================================
 * HTTP 请求解析
 * ============================================================ */
struct HttpRequest {
    std::string method, path, origin, content_type, accept;
    std::string session_id, protocol_version;
    std::string body;
    std::size_t total = 0;      /* headers + body 总字节数 */
};

std::string header_value(const std::string& headers, const char* name)
{
    const std::size_t name_len = strlen(name);
    std::size_t i = 0;
    while (i + 1 < headers.size() && !(headers[i] == '\r' && headers[i + 1] == '\n')) i++;
    i += 2;
    while (i + name_len + 1 < headers.size()) {
        if (_strnicmp(headers.c_str() + i, name, name_len) == 0 &&
            headers[i + name_len] == ':') {
            std::size_t p = i + name_len + 1;
            while (p < headers.size() && (headers[p] == ' ' || headers[p] == '\t')) p++;
            const std::size_t start = p;
            while (p < headers.size() && headers[p] != '\r' && headers[p] != '\n') p++;
            return headers.substr(start, p - start);
        }
        while (i + 1 < headers.size() && !(headers[i] == '\r' && headers[i + 1] == '\n')) i++;
        i += 2;
    }
    return std::string();
}

/* 返回：1=完整请求，0=数据不足，-1=报文非法，-2=超长 */
int parse_request(const std::string& buf, HttpRequest& out)
{
    const std::size_t hdr_end = buf.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        if (buf.size() > VBMCP_MAX_HEADER_SIZE) return -1;
        return 0;
    }
    const std::size_t headers_len = hdr_end + 4;
    const std::string headers = buf.substr(0, headers_len);

    const std::size_t sp1 = headers.find(' ');
    if (sp1 == std::string::npos) return -1;
    const std::size_t sp2 = headers.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return -1;

    out.method = headers.substr(0, sp1);
    out.path   = headers.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = out.path.find('?');
    if (q != std::string::npos) out.path.resize(q);
    if (out.path.size() > VBMCP_MAX_HEADER_SIZE) return -1;

    out.origin           = header_value(headers, "Origin");
    out.content_type     = header_value(headers, "Content-Type");
    out.accept           = header_value(headers, "Accept");
    out.session_id       = header_value(headers, "Mcp-Session-Id");
    out.protocol_version = header_value(headers, "MCP-Protocol-Version");

    std::size_t content_length = 0;
    const std::string cl = header_value(headers, "Content-Length");
    if (!cl.empty()) {
        for (char ch : cl) {
            if (ch < '0' || ch > '9') return -1;
            content_length = content_length * 10 + (std::size_t)(ch - '0');
            if (content_length > VBMCP_MAX_BODY_SIZE) return -2;
        }
    }

    out.total = headers_len + content_length;
    if (buf.size() < out.total) return 0;
    out.body = buf.substr(headers_len, content_length);
    return 1;
}

bool protocol_version_supported(const std::string& v)
{
    if (v.empty()) return true;    /* 宽松：未携带则不校验 */
    return v == VBMCP_PROTOCOL || v == VBMCP_PROTOCOL_LEGACY;
}

bool accept_has(const std::string& accept, const char* token)
{
    return accept.find(token) != std::string::npos ||
           accept.find("*/*") != std::string::npos;
}

bool is_mcp_path(const std::string& p)
{
    return p == "/" || p == "/mcp" || p.empty();
}

/* ============================================================
 * 任务队列
 * ============================================================ */
bool task_queue_push(std::shared_ptr<UiTask> task)
{
    {
        std::lock_guard<std::mutex> lk(g_task_mu);
        if ((int)g_task_queue.size() >= kTaskQueueLimit) return false;
        g_task_queue.push_back(std::move(task));
    }
    if (g_task_hwnd && ::IsWindow(g_task_hwnd)) {
        ::PostMessage(g_task_hwnd, VBMCP_WM_TASK, 0, 0);
    }
    return true;
}

/* UI 主线程：在 VBMCP_WM_TASK 消息里调用，串行执行队列任务。 */
void task_pump_on_ui_thread()
{
    for (;;) {
        std::shared_ptr<UiTask> task;
        {
            std::lock_guard<std::mutex> lk(g_task_mu);
            if (g_task_queue.empty()) return;
            task = g_task_queue.front();
            g_task_queue.erase(g_task_queue.begin());
        }

        if (task->cancelled.load()) {
            /* 已被取消：排队期间客户端断开 / 超时 / 服务停止 —— 不执行业务 */
            task->response = json_error_document(VBMCP_ERR_BUSY, "request cancelled");
            task->http_status = 200;
        } else {
            /* MCP 请求入口 try-catch：任何 C++ 异常都不允许带崩 VB Decompiler UI */
            try {
                vbmcp::OwnedString resp(
                    vbmcp_process_jsonrpc(task->body.c_str(), task->peer.c_str()));
                if (!resp) {
                    task->http_status = 200;
                    task->response = json_error_document(VBMCP_ERR_INTERNAL, "out of memory");
                } else if (resp.get()[0] == '\0') {
                    task->http_status = 202;          /* notification：无响应体 */
                    task->response.clear();
                } else {
                    task->http_status = 200;
                    task->response = resp.get();
                }
            } catch (const std::exception& e) {
                vbmcp_logf(VBMCP_LOG_ERROR, "tool call threw std::exception: %s", e.what());
                task->http_status = 200;
                task->response = json_error_document(VBMCP_ERR_INTERNAL, "internal error");
            } catch (...) {
                vbmcp_logf(VBMCP_LOG_ERROR, "tool call threw unknown exception");
                task->http_status = 200;
                task->response = json_error_document(VBMCP_ERR_INTERNAL, "internal error");
            }
        }

        {
            std::lock_guard<std::mutex> lk(task->mtx);
            task->done.store(true);
        }
        task->cv.notify_all();
    }
}

/* UI 主线程的消息窗口：只负责把网络线程投递的任务搬到 UI 上执行 */
LRESULT CALLBACK task_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == VBMCP_WM_TASK) {
        task_pump_on_ui_thread();
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ============================================================
 * /health（yyjson 构造）
 * ============================================================ */
std::string build_health_document()
{
    char exepath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exepath, MAX_PATH);
    const char* fname = strrchr(exepath, '\\');
    fname = fname ? fname + 1 : exepath;

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) return "{}";
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ready");

    yyjson_mut_val* server = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, server, "name", VBMCP_PLUGIN_NAME);
    yyjson_mut_obj_add_strcpy(doc, server, "version", VBMCP_VERSION);
    yyjson_mut_obj_add_strcpy(doc, server, "protocolVersion",
                              g_active_protocol_version.load(std::memory_order_acquire));
    yyjson_mut_obj_add_val(doc, root, "server", server);

    yyjson_mut_val* arr  = yyjson_mut_arr(doc);
    yyjson_mut_val* inst = yyjson_mut_arr_add_obj(doc, arr);
    yyjson_mut_obj_add_strcpy(doc, inst, "name", fname);
    yyjson_mut_obj_add_strcpy(doc, inst, "arch", "Win32");
    yyjson_mut_obj_add_int(doc, inst, "bits", 32);
    yyjson_mut_obj_add_strcpy(doc, inst, "path", exepath);
    yyjson_mut_obj_add_int(doc, inst, "pid", (long long)GetCurrentProcessId());
    yyjson_mut_obj_add_bool(doc, inst, "local", vbmcp_bind_address() == VBMCP_LOOPBACK_BIND);
    yyjson_mut_obj_add_strcpy(doc, inst, "state", "ready");
    yyjson_mut_obj_add_bool(doc, inst, "readOnly", g_allow_write.load() == 0);
    yyjson_mut_obj_add_int(doc, inst, "tools_count",
                           mcp_local_tool_count(g_allow_write.load()));
    yyjson_mut_obj_add_val(doc, root, "instances", arr);
    yyjson_mut_obj_add_int(doc, root, "ready", 1);

    char* s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, nullptr);
    std::string out(s ? s : "{}");
    yyjson_mut_doc_free(doc);
    return out;
}

/* ============================================================
 * 请求处理（网络线程）
 * ============================================================ */
void handle_mcp_post(Connection* c, const HttpRequest& req)
{
    if (!accept_has(req.accept, "application/json")) {
        http_send_error(c, 406, VBMCP_ERR_INVALID_REQUEST,
                        "Accept must include application/json", req.origin);
        return;
    }
    if (!req.content_type.empty() &&
        req.content_type.find("application/json") == std::string::npos) {
        http_send_error(c, 415, VBMCP_ERR_INVALID_REQUEST,
                        "Content-Type must be application/json", req.origin);
        return;
    }
    if (!protocol_version_supported(req.protocol_version)) {
        http_send_error(c, 400, VBMCP_ERR_INVALID_REQUEST,
                        "unsupported MCP-Protocol-Version", req.origin);
        return;
    }
    if (!req.session_id.empty() && !c->session_id.empty() &&
        req.session_id != c->session_id) {
        http_send_error(c, 404, VBMCP_ERR_INVALID_REQUEST, "unknown session id", req.origin);
        return;
    }
    if (!rate_limit_allow()) {
        http_send_error(c, 429, VBMCP_ERR_BUSY, "Too many requests", req.origin);
        return;
    }

    /* 本实现不接 JSON-RPC 批量（数组）请求：按标准错误明确回绝 */
    std::size_t i = 0;
    while (i < req.body.size() && (req.body[i] == ' ' || req.body[i] == '\t')) i++;
    if (i < req.body.size() && req.body[i] == '[') {
        http_send_error(c, 400, VBMCP_ERR_INVALID_REQUEST,
                        "JSON-RPC batching is not supported", req.origin);
        return;
    }

    /* 同一连接同时只允许一个在飞任务：c->pending 是单槽，覆盖它会让上一个
     * 请求永远收不到响应（任务却仍在队列里照常执行，对 vb_write_* 还会造成
     * "客户端以为失败、实际已写入"的静默副作用）。 */
    if (c->pending) {
        http_send_error(c, 429, VBMCP_ERR_BUSY,
                        _F("Another request from this connection is still in flight; wait for its response before sending the next one."),
                        req.origin);
        vbmcp_logf(VBMCP_LOG_WARN,
                   "tool task rejected: connection already busy (client=%s)",
                   c->peer.c_str());
        return;
    }

    auto task = std::make_shared<UiTask>();
    task->body    = req.body;
    task->peer    = c->peer;
    task->enqueue = std::chrono::steady_clock::now();

    if (!task_queue_push(task)) {
        /* 队列满：直接用 _F()（翻译 + 占位符校验），网络线程可安全调用 */
        const char *msg =
            _F("Server busy: %d MCP tasks are already queued on the VB Decompiler UI thread (limit %d). The request was rejected to keep the UI responsive; retry in a moment.",
               kTaskQueueLimit, kTaskQueueLimit);
        http_send_error(c, 429, VBMCP_ERR_BUSY, msg, req.origin);
        vbmcp_logf(VBMCP_LOG_WARN, "tool task rejected: UI queue full (client=%s)",
                   c->peer.c_str());
        return;
    }

    /* initialize 成功后需要在响应里回带 Mcp-Session-Id（Streamable HTTP 规范） */
    if (req.body.find("\"initialize\"") != std::string::npos && c->session_id.empty()) {
        c->session_id = make_session_id();
        c->session_id_pending = true;
    }
    c->pending = task;
    g_session_was_used.store(true);
    vbmcp_logf(VBMCP_LOG_INFO, "tool task queued (%zu bytes, client=%s)",
               task->body.size(), c->peer.c_str());
}

void handle_request(Connection* c, const HttpRequest& req)
{
    vbmcp_logf(VBMCP_LOG_DEBUG, "http %s %s (client=%s, body=%zu)",
               req.method.c_str(), req.path.c_str(), c->peer.c_str(), req.body.size());

    if (req.method == "OPTIONS") {
        /* CORS 预检：未配置可信 origin 时不返回任何 CORS 头（禁止 `*`） */
        http_send_raw(c, 204, "text/plain", req.origin, cors_headers(req.origin), "", false);
        return;
    }

    if (req.path == "/health" && req.method == "GET") {
        http_send_json(c, 200, req.origin, build_health_document());
        return;
    }

    if (!is_mcp_path(req.path) && req.path != "/sse") {
        http_send_error(c, 404, VBMCP_ERR_INVALID_REQUEST, "Not Found", req.origin);
        return;
    }

    if (req.method == "GET") {
        if (!accept_has(req.accept, "text/event-stream")) {
            http_send_error(c, 406, VBMCP_ERR_INVALID_REQUEST,
                            "Accept must include text/event-stream", req.origin);
            return;
        }
        sse_begin(c, req.origin);
        return;
    }

    if (req.method == "DELETE") {
        if (c->pending) c->pending->cancelled.store(true);
        c->session_id.clear();
        c->session_id_pending = false;
        http_send_raw(c, 204, "text/plain", req.origin, "", "", false);
        vbmcp_logf(VBMCP_LOG_INFO, "session terminated by client (client=%s)", c->peer.c_str());
        return;
    }

    if (req.method == "POST") {
        handle_mcp_post(c, req);
        return;
    }

    http_send_error(c, 405, VBMCP_ERR_INVALID_REQUEST, "Method Not Allowed", req.origin);
}

/* ============================================================
 * socket IO
 * ============================================================ */
bool client_gone(Connection* c)
{
    fd_set fds; FD_ZERO(&fds); FD_SET(c->sock.get(), &fds);
    timeval tv{ 0, 0 };
    if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return false;

    char tmp[512];
    const int r = recv(c->sock.get(), tmp, sizeof(tmp), MSG_PEEK);
    if (r == 0) return true;                                     /* 对端 FIN */
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) return true;
    return false;
}

bool connection_on_readable(Connection* c)
{
    char tmp[8192];
    const int r = recv(c->sock.get(), tmp, sizeof(tmp), 0);
    if (r == SOCKET_ERROR) {
        const int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) return true;
        return false;
    }
    if (r == 0) return false;                    /* 对端关闭 */

    c->last_active = std::chrono::steady_clock::now();
    if (c->is_sse) return true;                  /* SSE 上的客户端数据忽略 */

    c->recv_buf.append(tmp, (std::size_t)r);
    if (c->recv_buf.size() > (std::size_t)VBMCP_MAX_BODY_SIZE + VBMCP_MAX_HEADER_SIZE) {
        http_send_error(c, 413, VBMCP_ERR_INVALID_REQUEST, "payload too large");
        return true;
    }

    for (;;) {
        HttpRequest req;
        const int st = parse_request(c->recv_buf, req);
        if (st == 0) return true;
        if (st == -2) {
            http_send_error(c, 413, VBMCP_ERR_INVALID_REQUEST, "payload too large");
            return true;
        }
        if (st < 0) {
            http_send_error(c, 400, VBMCP_ERR_PARSE, "malformed HTTP request");
            return true;
        }
        c->recv_buf.erase(0, req.total);
        handle_request(c, req);
        if (c->close_after_send || c->is_sse) return vbmcp_flush(c) != 0;
    }
}

/* 返回 false 表示连接应销毁 */
bool connection_poll_task(Connection* c, int timeout_ms)
{
    auto task = c->pending;
    if (!task) return true;

    if (task->done.load()) {
        std::string extra;
        if (c->session_id_pending && !c->session_id.empty()) {
            extra = "Mcp-Session-Id: " + c->session_id;
            c->session_id_pending = false;
        }
        c->pending.reset();
        if (task->http_status == 202 && task->response.empty()) {
            http_send_raw(c, 202, "application/json; charset=utf-8", "", extra, "", false);
        } else {
            http_send_json(c, task->http_status, "", task->response, extra);
        }
        return true;
    }

    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - task->enqueue).count();
    if (age >= timeout_ms) {
        task->cancelled.store(true);            /* 取消令牌：UI 侧排队任务会被丢弃 */
        const char *msg =
            _F("Task timed out: the request waited longer than %d seconds for the VB Decompiler UI thread. The operation was cancelled; try again with a smaller scope (for example a single module or function).",
               timeout_ms / 1000);
        c->pending.reset();
        http_send_json(c, 200, "", json_error_document(VBMCP_ERR_TIMEOUT, msg));
        vbmcp_logf(VBMCP_LOG_WARN, "tool task timeout after %lldms (client=%s)",
                   (long long)age, c->peer.c_str());
        return true;
    }

    if (client_gone(c)) {
        task->cancelled.store(true);            /* 客户端断开 → 及时释放后台任务 */
        vbmcp_logf(VBMCP_LOG_INFO, "client gone, task cancelled (client=%s)",
                   c->peer.c_str());
        c->pending.reset();
        return false;
    }

    std::unique_lock<std::mutex> lk(task->mtx);
    task->cv.wait_for(lk, std::chrono::milliseconds(kSelectTimeoutMs),
                      [&] { return task->done.load(); });
    return true;
}

/* ============================================================
 * 网络线程主循环
 * ============================================================ */
int task_timeout_ms()
{
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = VBMCP_TASK_TIMEOUT_MS;
    char env[16] = {0};
    if (GetEnvironmentVariableA("VBMCP_TASK_TIMEOUT_MS", env, sizeof(env)) > 0) {
        const int v = atoi(env);
        if (v >= 1000 && v <= 600000) cached = v;
    }
    return cached;
}

vbmcp::SocketHandle create_listen_socket(const std::string& bind_addr, int port)
{
    struct addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags    = AI_PASSIVE;

    char port_str[16];
    _snprintf_s(port_str, sizeof(port_str), _TRUNCATE, "%d", port);

    struct addrinfo* ai = nullptr;
    if (getaddrinfo(bind_addr.c_str(), port_str, &hints, &ai) != 0 || !ai)
        return vbmcp::SocketHandle();

    vbmcp::SocketHandle listener;
    for (struct addrinfo* p = ai; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        if (bind(s, p->ai_addr, (int)p->ai_addrlen) == SOCKET_ERROR) {
            vbmcp_logf(VBMCP_LOG_ERROR, "bind(%s:%d) failed err=%d",
                       bind_addr.c_str(), port, WSAGetLastError());
            closesocket(s);
            continue;
        }
        if (listen(s, SOMAXCONN) == SOCKET_ERROR) { closesocket(s); continue; }
        listener.reset(s);
        break;
    }
    freeaddrinfo(ai);
    return listener;
}

void network_thread_main()
{
    const std::string bind_addr = vbmcp_bind_address();
    const int port = g_port.load();

    vbmcp::SocketHandle listener = create_listen_socket(bind_addr, port);
    if (!listener) {
        vbmcp_logf(VBMCP_LOG_ERROR, "listen socket creation failed (%s:%d)",
                   bind_addr.c_str(), port);
        g_net_inited.store(false);
        return;
    }
    g_net_inited.store(true);
    vbmcp_logf(VBMCP_LOG_INFO, "MCP Streamable HTTP listening on %s:%d (thread=%lu)",
               bind_addr.c_str(), port, GetCurrentThreadId());

    std::vector<std::unique_ptr<Connection>> conns;
    const int timeout_ms = task_timeout_ms();

    while (g_net_running.load()) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        FD_SET(listener.get(), &rfds);
        for (auto& c : conns) {
            if (!c->sock.valid()) continue;
            FD_SET(c->sock.get(), &rfds);       /* 含 SSE：用于探测断开 */
            if (c->want_write && c->send_off < c->send_buf.size())
                FD_SET(c->sock.get(), &wfds);
        }

        timeval tv{ 0, kSelectTimeoutMs * 1000 };
        const int n = select(0, &rfds, &wfds, nullptr, &tv);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) continue;
            vbmcp_logf(VBMCP_LOG_ERROR, "select failed err=%d", WSAGetLastError());
            break;
        }

        if (FD_ISSET(listener.get(), &rfds)) {
            bool refused = false;
            while ((int)conns.size() < VBMCP_MAX_CONNECTIONS) {
                SOCKET cs = accept(listener.get(), nullptr, nullptr);
                if (cs == INVALID_SOCKET) break;
                int one = 1;
                setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
                auto conn = std::make_unique<Connection>();
                conn->sock.reset(cs);
                conn->peer = peer_name(cs);
                conn->last_active = std::chrono::steady_clock::now();
                vbmcp_logf(VBMCP_LOG_INFO, "client connected: %s", conn->peer.c_str());
                conns.push_back(std::move(conn));
                refused = false;                 /* 又收下了：允许下次再告警 */
            }
            /* 满额时新连接被内核直接丢掉，客户端只看到连接被重置。
             * 记一条日志（每次"开始满额"只记一次，否则每 50ms 轮询刷屏），
             * 否则"AI 客户端连不上"这类问题无从排查。 */
            if (!refused && (int)conns.size() >= VBMCP_MAX_CONNECTIONS) {
                refused = true;
                vbmcp_logf(VBMCP_LOG_WARN,
                           "accept: connection limit reached (%d), extra clients refused",
                           VBMCP_MAX_CONNECTIONS);
            }
        }

        for (auto it = conns.begin(); it != conns.end();) {
            Connection* c = it->get();
            bool keep = true;

            if (c->sock.valid() && FD_ISSET(c->sock.get(), &wfds)) {
                if (!vbmcp_flush(c)) keep = false;
            }
            if (keep && c->sock.valid() && FD_ISSET(c->sock.get(), &rfds)) {
                if (!connection_on_readable(c)) keep = false;
            }
            if (keep) keep = connection_poll_task(c, timeout_ms);
            if (keep && c->close_after_send && c->send_off >= c->send_buf.size()) keep = false;

            if (!keep) {
                if (c->pending) {
                    c->pending->cancelled.store(true);    /* 断开即取消后台任务 */
                    c->pending.reset();
                }
                vbmcp_logf(VBMCP_LOG_INFO, "client disconnected: %s", c->peer.c_str());
                c->sock.reset();
                it = conns.erase(it);
            } else {
                ++it;
            }
        }

        sse_heartbeat_all(conns);
        flush_list_changed_notification(conns);

        /* 所有客户端都已断开 → 会话准入授权失效：
         * 下一次新连接首次调用工具时重新弹窗确认（服务重启同理）。 */
        if (conns.empty() && g_session_was_used.exchange(false)) {
            vbmcp_session_reset();
            vbmcp_logf(VBMCP_LOG_INFO, "all clients gone: session authorization reset");
        }
    }

    for (auto& c : conns) {
        if (c->pending) {
            c->pending->cancelled.store(true);
            c->pending.reset();
        }
        c->sock.reset();
    }
    conns.clear();
    g_net_inited.store(false);
    vbmcp_logf(VBMCP_LOG_INFO, "network thread exited");
}

void apply_globals()
{
    if (g_engine) {
        char msg[160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "MCP: %s on %s:%d",
                    vbmcp_server_is_running() ? "running" : "stopped",
                    vbmcp_bind_address().c_str(), g_port.load());
        vb_set_status(msg);
    }
}

} // namespace

/* ============================================================
 * 对外接口
 * ============================================================ */
void vbmcp_http_set_protocol_version(const char* ver)
{
    if (!ver || !*ver) return;
    const size_t n = strlen(ver);
    char *copy = (char *)vbmcp_alloc(n + 1);
    if (!copy) return;
    memcpy(copy, ver, n + 1);              /* 含结尾 NUL */
    g_active_protocol_version.store(copy, std::memory_order_release);
}

std::string vbmcp_bind_address()
{
    std::lock_guard<std::mutex> lk(g_bind_mu);
    return g_bind_addr.empty() ? std::string(VBMCP_DEFAULT_BIND) : g_bind_addr;
}

void vbmcp_set_bind_address(const char* addr)
{
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_bind_addr = (addr && *addr) ? addr : VBMCP_DEFAULT_BIND;
}

int vbmcp_server_start()
{
    if (g_net_running.load()) return 1;              /* 幂等 */
    if (!g_enabled.load()) return 1;

    if (!g_winsock_inited.load()) {
        WSADATA wsadata;
        if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
            vbmcp_logf(VBMCP_LOG_ERROR, "WSAStartup failed");
            return 0;
        }
        g_winsock_inited.store(true);
    }

    /* 消息窗口必须在 UI 主线程创建：网络线程 PostMessage 投递任务给它 */
    if (!g_task_hwnd) {
        static const char* kClass = "VBMCP_TASK_V1";
        WNDCLASS wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = task_wnd_proc;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassA(&wc);                          /* 已存在时忽略返回值 */
        g_task_hwnd = CreateWindowExA(0, kClass, kClass, 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!g_task_hwnd) {
            vbmcp_logf(VBMCP_LOG_ERROR, "task window creation failed");
            return 0;
        }
    }

    g_net_running.store(true);
    g_net_thread = std::thread(network_thread_main);

    for (int i = 0; i < 300 && !g_net_inited.load() && g_net_running.load(); ++i)
        Sleep(10);

    if (!g_net_inited.load()) {
        g_net_running.store(false);
        if (g_net_thread.joinable()) g_net_thread.join();
        return 0;
    }
    return 1;
}

void vbmcp_server_signal_stop()
{
    g_net_running.store(false);
}

void vbmcp_server_stop()
{
    if (g_net_running.exchange(false)) {
        if (g_net_thread.joinable()) g_net_thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(g_task_mu);
        for (auto& t : g_task_queue) t->cancelled.store(true);
        g_task_queue.clear();
    }
    vbmcp_session_reset();          /* 服务停止 → 会话准入授权状态重置 */

    if (g_task_hwnd) {
        ::DestroyWindow(g_task_hwnd);
        g_task_hwnd = nullptr;
    }

    /* 与 vbmcp_server_start 里的 WSAStartup 配对：反复"停服→启服"会累积
     * Winsock 引用计数（进程退出时系统会回收，但用户反复开关时不该泄漏）。
     * 注意：必须在网络线程 join 之后再调用。 */
    if (g_winsock_inited.exchange(false)) {
        WSACleanup();
    }
}

int vbmcp_server_is_running()
{
    return g_net_inited.load() ? 1 : 0;
}

int vbmcp_server_apply(int enable, int port)
{
    if (enable) {
        const bool port_changed = (port > 0 && port < 65536 && port != g_port.load());
        if (port_changed) g_port.store(port);
        g_enabled.store(1);

        if (vbmcp_server_is_running()) {
            if (port_changed) {
                vbmcp_logf(VBMCP_LOG_INFO, "port changed to %d, restarting server",
                           g_port.load());
                vbmcp_server_stop();
                if (!vbmcp_server_start()) return 0;
            }
            apply_globals();
            return 1;
        }
        if (!vbmcp_server_start()) return 0;
        apply_globals();
        return 1;
    }

    g_enabled.store(0);
    vbmcp_server_stop();
    apply_globals();
    return 1;
}

void vbmcp_notify_tools_list_changed()
{
    {
        std::lock_guard<std::mutex> lk(g_list_changed_mu);
        g_list_changed_payload =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}";
    }
    g_list_changed_pending.store(true);
}