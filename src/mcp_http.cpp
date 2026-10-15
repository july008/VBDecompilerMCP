/*
 * mcp_http.cpp - MCP Streamable HTTP 传输层（C++17）
 * mcp_http.cpp - MCP Streamable HTTP transport layer (C++17)
 *
 * ============================== 线程模型（硬约束） ==============================
 *   [网络线程]  后台独立线程：accept / select / HTTP 解析 / 传输层协议处理。
 *               **绝不触碰任何 UI 控件、UI 对象或 VB Decompiler Callback Engine。**
 *   [UI 主线程]  VB Decompiler 消息循环所在线程：执行全部 MCP 工具调用。
 *               网络线程把请求封装成 UiTask，通过消息窗口 PostMessage 投递；
 *               执行完成后唤醒网络线程回写 HTTP 响应。
 *
 *   [network thread]  dedicated background thread: accept / select / HTTP
 *                     parsing / transport handling. It NEVER touches UI controls,
 *                     UI objects or the VB Decompiler callback engine.
 *   [UI main thread]  the thread running the VB Decompiler message loop: runs all
 *                     MCP tool calls. The network thread wraps requests into
 *                     UiTasks and posts them through a message window; on
 *                     completion the network thread is woken to write the HTTP
 *                     response.
 *
 *   - 所有需要 UI / 工程数据的请求都经 UiTask 投递，绝不跨线程直接调用；
 *   - 每个任务支持超时（VBDMCP_TASK_TIMEOUT_MS，默认 30s）与取消令牌
 *     （客户端断开 / 超时 / 服务停止时置位；尚未执行的任务会被直接丢弃）；
 *   - 并发上限 VBDMCP_MAX_PENDING_TASKS：队列满时立即返回 429 + MCP 繁忙错误，
 *     避免多个 MCP 请求同时触发大项目反编译导致 UI 卡死 / 竞态。
 *
 *   - every request that needs UI / project data goes through a UiTask, never a
 *     direct cross-thread call;
 *   - each task supports a timeout (VBDMCP_TASK_TIMEOUT_MS, 30s by default) and a
 *     cancellation token (set on client disconnect / timeout / server stop; tasks
 *     not started yet are dropped);
 *   - concurrency cap VBDMCP_MAX_PENDING_TASKS: a full queue answers 429 + MCP
 *     busy immediately, so parallel requests cannot freeze the UI or race on a
 *     large decompilation job.
 *
 * ============================ 传输规范（MCP 2025-03-26） ============================
 *   POST /mcp        JSON-RPC 请求（Accept 需含 application/json）
 *   GET  /mcp        升级为 SSE 长连接（Accept 需含 text/event-stream）
 *   DELETE /mcp      终止会话
 *   OPTIONS *        仅当 Origin 命中用户配置的可信列表才回 CORS 头
 *   GET  /health     健康检查（非 MCP 端点，yyjson 构造）
 *
 *   POST /mcp        JSON-RPC request (Accept must include application/json)
 *   GET  /mcp        upgrade to an SSE stream (Accept must include text/event-stream)
 *   DELETE /mcp      terminate the session
 *   OPTIONS *        CORS headers only when Origin matches the configured list
 *   GET  /health     health check (not an MCP endpoint, built with yyjson)
 *
 * 安全模型：默认只监听 127.0.0.1；默认不返回任何 CORS 头（禁止 `*`）；
 * 请求体大小上限 + 全局基础限流 + 畸形 JSON 由 yyjson 安全拒绝。
 *
 * Security model: listens on 127.0.0.1 only by default; no CORS headers by
 * default ("*" is forbidden); body size limits plus a global base rate limit, and
 * malformed JSON is rejected safely by yyjson.
 */
#include "vbdmcp.h"
#include "mcp_tools.h"
#include "i18n.h"
#include "vbdmcp_mem.hpp"
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
 * 全局状态定义 / Global state definitions
 * ============================================================ */
std::atomic<int> g_enabled{ VBDMCP_DEFAULT_ENABLED };
std::atomic<int> g_port{ VBDMCP_DEFAULT_PORT };
/* 允许 MCP 客户端写入项目数据（UI 配置，默认只读）。UI 主线程写、网络线程读。
 * Allow MCP clients to write project data (UI setting, read-only by default);
 * written by the UI main thread and read by the network thread. */
std::atomic<int> g_allow_write{ VBDMCP_DEFAULT_ALLOW_WRITE };

namespace {

/* ---------------- 常量 / Constants ---------------- */
constexpr int kSelectTimeoutMs  = 50;      /* 循环轮询间隔（同时保证关停响应及时）
                                              poll interval (also keeps shutdown responsive) */
/* SSE 保活注释帧间隔。取 5 秒（而非 15 秒）：心跳越积极，客户端越早确认
 * 流是活的，也避免与客户端的空闲判定"踩在同一秒"（曾观察到客户端约 15 秒
 * 即重连）。开销仅为每 5 秒 8 字节。
 *
 * SSE keep-alive comment frame interval. 5s (not 15s): a livelier heartbeat lets
 * clients confirm the stream earlier and avoids colliding with client idle
 * detection (clients were observed reconnecting after about 15 seconds). The cost
 * is a mere 8 bytes every 5 seconds. */
constexpr int kSseHeartbeatMs   = 5000;
constexpr int kRateLimitPerSec  = 200;     /* 全局基础限流 / global base rate limit */
constexpr int kTaskQueueLimit   = VBDMCP_MAX_PENDING_TASKS;
/* 单连接待发缓冲上限：SSE 通知/心跳只追加不等待，客户端不读时必须有上限，
 * 否则一个僵死连接就能把进程内存吃光。超限直接断开该连接。
 *
 * Per-connection pending-send buffer cap. SSE notifications / heartbeats append
 * without waiting, so a cap is mandatory: one stuck client could otherwise eat
 * the process memory. Exceeding it drops the connection. */
constexpr size_t kMaxSendBufferBytes = 4u * 1024 * 1024;

/* ---------------- 绑定地址（UI 线程写，网络线程读） / Bind address (UI writes, network thread reads) ---------------- */
std::mutex  g_bind_mu;
std::string g_bind_addr = VBDMCP_DEFAULT_BIND;

/* ---------------- 协议版本 / Protocol version ---------------- */
/* 协议版本串：UI 线程在 initialize 时写入（vbdmcp_http_set_protocol_version），
 * 网络线程在 /health 里读。
 * UI 线程分配一份不可变字符串并原子发布指针，读者永远看到完整字符串；
 * 旧值故意不释放（进程级、每次会话才换一次，几十字节）。
 *
 * Protocol version string: written by the UI thread during initialize
 * (vbdmcp_http_set_protocol_version) and read by the network thread in /health.
 * The UI thread publishes an immutable string through an atomic pointer, so
 * readers always see a complete string; old values are intentionally not freed
 * (process lifetime, one change per session, a few dozen bytes). */
std::atomic<const char*> g_active_protocol_version{ VBDMCP_PROTOCOL };

/* ---------------- 连接对象 / Connection object ---------------- */
struct Connection;

/* ============================================================
 * UI 任务（网络线程 → UI 主线程的单向投递单元）
 * UI task (one-way dispatch unit from the network thread to the UI main thread)
 * ============================================================ */
struct UiTask {
    std::string       body;       /* 原始 JSON-RPC 请求体 / raw JSON-RPC request body */
    std::string       peer;       /* 客户端 "ip:port"，授权弹窗展示 / client "ip:port" shown in the auth dialog */
    std::string       response;   /* UI 主线程写回的响应体 / response body written by the UI thread */
    int               http_status = 200;
    std::atomic<bool> cancelled{ false };   /* 取消令牌（网络线程置位）/ cancellation token (set by the network thread) */
    std::atomic<bool> done{ false };
    /* 由 UI 线程在解析请求后回标：该请求是 initialize。
     * 网络线程据此分配 Mcp-Session-Id（见 handle_mcp_post）。
     *
     * Back-flagged by the UI thread after parsing: the request is an initialize.
     * The network thread allocates the Mcp-Session-Id based on this flag
     * (see handle_mcp_post). */
    std::atomic<bool> is_initialize{ false };
    std::chrono::steady_clock::time_point enqueue;

    std::mutex              mtx;   /* 只用于"等待完成"，不保护上面字段 / only guards the completion wait, not the fields above */
    std::condition_variable cv;
};

struct Connection {
    vbdmcp::SocketHandle sock;
    std::string   peer;
    std::string   recv_buf;
    std::string   send_buf;
    std::size_t   send_off = 0;
    std::string   session_id;                 /* MCP 会话 id（initialize 分配）/ MCP session id (allocated by initialize) */
    bool          close_after_send = false;
    bool          is_sse = false;
    bool          want_write = false;
    bool          dead = false;               /* 主动断开标记（如 SSE 缓冲超限）/ forced-close flag (e.g. SSE buffer overflow) */
    std::shared_ptr<UiTask> pending;
    std::chrono::steady_clock::time_point last_active;
};

/* ---------------- 任务队列（网络线程写，UI 主线程读） / Task queue (network thread writes, UI main thread reads) ---------------- */
std::mutex                            g_task_mu;
std::vector<std::shared_ptr<UiTask>>  g_task_queue;
HWND                                  g_task_hwnd = nullptr;

/* ---------------- 网络线程状态 / Network thread state ---------------- */
std::thread       g_net_thread;
std::atomic<bool> g_net_running{ false };
std::atomic<bool> g_net_inited{ false };
std::atomic<bool> g_winsock_inited{ false };
/* bind 完成（含失败）通知：vbdmcp_server_start 等待它，替代 Sleep 忙等
 * Bind completion (success or failure) notification awaited by vbdmcp_server_start,
 * replacing a Sleep-based busy wait. */
std::mutex              g_net_init_mu;
std::condition_variable g_net_init_cv;
std::atomic<bool>       g_net_init_done{ false };

/* ---------------- tools/list_changed 广播暂存 / Tools-list-changed broadcast staging ---------------- */
std::mutex              g_list_changed_mu;
std::string             g_list_changed_payload;
std::atomic<bool>       g_list_changed_pending{ false };

/* ---------------- 基础限流 / Base rate limiting ---------------- */
DWORD g_rate_sec   = 0;
int   g_rate_count = 0;

/* 当前会话是否已被使用（所有客户端断开时重置会话准入授权）。
 * Whether the current session was ever used (authorization is reset when all
 * clients disconnect). */
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
 *
 * 列表来自注册表 CorsOrigins，进程内缓存：origin_is_trusted 只在网络线程调用，
 * 缓存也由网络线程独占读写（先写字符串、再置 loaded）。
 * 失效：服务 stop/start 时由 UI 线程复位标志（此时网络线程已 join）。
 *
 * Trusted CORS origins (never "*").
 *
 * The list comes from the CorsOrigins registry value and is cached in-process:
 * origin_is_trusted runs on the network thread only and owns the cache (write the
 * string first, then set "loaded"). Invalidation: the UI thread resets the flag on
 * server stop/start, after the network thread has been joined.
 * ============================================================ */
std::string       g_cors_list;
std::atomic<bool> g_cors_loaded{ false };

const std::string& cors_list_cached()
{
    if (!g_cors_loaded.load(std::memory_order_acquire)) {
        char list[1024] = {0};
        vbdmcp_config_get_cors_origins(list, sizeof(list));
        g_cors_list = list;
        g_cors_loaded.store(true, std::memory_order_release);
    }
    return g_cors_list;
}

void cors_cache_invalidate()
{
    g_cors_loaded.store(false, std::memory_order_release);
}

bool origin_is_trusted(const std::string& origin)
{
    if (origin.empty()) return false;
    if (_stricmp(origin.c_str(), "null") == 0) return false;

    const std::string& list = cors_list_cached();
    if (list.empty()) return false;   /* 未配置可信列表 → 一律不发 CORS 头
                                         no trusted list configured -> never send CORS headers */

    const char* p = list.c_str();
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char* start = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - start);
        while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t')) n--;
        if (n == 1 && start[0] == '*') continue;   /* 忽略通配：禁止 "*" / wildcard ignored: "*" is forbidden */
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
 * HTTP 报文工具 / HTTP message helpers
 * ============================================================ */
/* HTTP reason phrase：按 RFC 惯例固定英文，**不做 i18n**（客户端只读状态码，
 * reason phrase 仅作可读性补充；翻译反而会偏离协议惯例）。
 * 面向用户的错误说明放在 JSON-RPC error.message 里，那里走 _() 翻译。
 *
 * HTTP reason phrases: kept in English by RFC convention and deliberately NOT
 * localized (clients act on the status code; the reason phrase is informational).
 * User-facing explanations live in the JSON-RPC error.message field, which does
 * go through _(). */
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

/* 用 yyjson 构造 JSON-RPC 标准错误文档（禁止手写 JSON 序列化）
 * Builds a standard JSON-RPC error document with yyjson (no hand-written JSON). */
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
        port = ntohs(a6->sin6_port);   /* C++ 下 sockaddr_in6 不与 sockaddr_in 组成联合体
                                          in C++ sockaddr_in6 is not part of a union with sockaddr_in */
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
 * 连接写出 / Connection output
 * ============================================================ */
int vbdmcp_flush(Connection* c)
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
    vbdmcp_flush(c);
}

void http_send_json(Connection* c, int status, const std::string& origin,
                    const std::string& body, const std::string& extra_headers = std::string())
{
    http_send_raw(c, status, "application/json; charset=utf-8", origin,
                  extra_headers, body, false);
}

/* transport 层错误：HTTP 状态码承载传输语义，JSON-RPC 错误对象承载协议语义
 * Transport errors: the HTTP status carries transport semantics, the JSON-RPC
 * error object carries protocol semantics. */
void http_send_error(Connection* c, int http_status, int jsonrpc_code,
                     const char* message, const std::string& origin = std::string())
{
    http_send_json(c, http_status, origin, json_error_document(jsonrpc_code, message));
    vbdmcp_logf(VBDMCP_LOG_WARN, "transport error http=%d code=%d msg=%s (client=%s)",
               http_status, jsonrpc_code, message ? message : "", c->peer.c_str());
}

/* ============================================================
 * SSE（服务端推送）/ SSE (server push)
 * ============================================================ */
void sse_begin(Connection* c, const std::string& origin)
{
    /* SSE 响应头**绝不能带 Content-Length**：SSE 是无限流，响应体的结束由
     * 连接关闭界定。一旦写了长度，客户端（curl / Node SDK / CodeBuddy）
     * 读完首帧就认为响应已完成 —— 表现为立刻断开并反复重连，客户端侧看到
     * 的就是"连接不可用/无法连接"。这就是 http_send_raw 不能复用的原因。
     *
     * The SSE response headers must NEVER carry Content-Length: SSE is an
     * endless stream whose body ends when the connection closes. With a length,
     * clients (curl / Node SDK / CodeBuddy) would consider the response complete
     * after the first frame, disconnect immediately and reconnect in a loop -
     * "connection unavailable" from the client side. That is why http_send_raw
     * cannot be reused here. */
    std::string head;
    head.reserve(256);
    head += "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n";
    if (!c->session_id.empty()) {
        head += "Mcp-Session-Id: ";
        head += c->session_id;
        head += "\r\n";
    }
    const std::string cors = cors_headers(origin);
    if (!cors.empty()) { head += cors; head += "\r\n"; }
    head += "\r\n";

    c->send_buf += head;
    c->send_buf += ": vbdmcp sse stream open\n\n";
    c->close_after_send = false;
    c->want_write = true;
    vbdmcp_flush(c);

    c->is_sse = true;
    c->last_active = std::chrono::steady_clock::now();
    vbdmcp_logf(VBDMCP_LOG_INFO, "sse stream opened (client=%s session=%s)",
               c->peer.c_str(), c->session_id.c_str());
}

void sse_push_json(Connection* c, const std::string& json)
{
    if (!c->is_sse) return;
    if (c->send_buf.size() + json.size() > kMaxSendBufferBytes) {
        vbdmcp_logf(VBDMCP_LOG_WARN,
                   "sse send buffer overflow (%zu bytes pending), dropping client (client=%s)",
                   c->send_buf.size(), c->peer.c_str());
        c->dead = true;                 /* 主循环下一轮回收该连接 / reclaimed by the main loop on its next pass */
        return;
    }
    c->send_buf += "event: message\ndata: ";
    c->send_buf += json;
    c->send_buf += "\n\n";
    c->want_write = true;
    vbdmcp_flush(c);
}

void sse_heartbeat_all(std::vector<std::unique_ptr<Connection>>& conns)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& c : conns) {
        if (!c || !c->is_sse || !c->sock.valid()) continue;
        const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - c->last_active).count();
        if (idle < kSseHeartbeatMs) continue;
        if (c->send_buf.size() > kMaxSendBufferBytes) {
            vbdmcp_logf(VBDMCP_LOG_WARN,
                       "sse send buffer overflow (%zu bytes pending), dropping client (client=%s)",
                       c->send_buf.size(), c->peer.c_str());
            c->dead = true;
            continue;
        }
        c->send_buf += ": ping\n\n";
        c->want_write = true;
        c->last_active = now;
        vbdmcp_flush(c.get());
    }
}

/* 若 UI 主线程请求广播 tools/list_changed，则推送给所有 SSE 连接
 * Pushes tools/list_changed to every SSE connection when the UI main thread
 * requested the broadcast. */
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
            vbdmcp_logf(VBDMCP_LOG_INFO, "sse push notifications/tools/list_changed (client=%s)",
                       c->peer.c_str());
        }
    }
}

/* ============================================================
 * HTTP 请求解析 / HTTP request parsing
 * ============================================================ */
struct HttpRequest {
    std::string method, path, origin, content_type, accept;
    std::string session_id, protocol_version;
    std::string body;
    std::size_t total = 0;      /* headers + body 总字节数 / total bytes of headers + body */
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

/* 返回：1=完整请求，0=数据不足，-1=报文非法，-2=超长，-3=不支持的传输编码
 * Returns: 1 = complete request, 0 = not enough data, -1 = malformed,
 *          -2 = too large, -3 = unsupported transfer encoding. */
int parse_request(const std::string& buf, HttpRequest& out)
{
    const std::size_t hdr_end = buf.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        if (buf.size() > VBDMCP_MAX_HEADER_SIZE) return -1;
        return 0;
    }
    const std::size_t headers_len = hdr_end + 4;
    /* 头部长度必须独立校验：只靠"未找到分隔符时"的检查会放过
     * 一次到齐的超长头部（例如 1MB 头部 + 小 body 的单次 send）。
     *
     * The header length needs its own check: testing only "no separator found
     * yet" would let an oversized header arriving in one packet (e.g. a 1MB
     * header plus a small body in a single send) pass through. */
    if (headers_len > (std::size_t)VBDMCP_MAX_HEADER_SIZE) return -1;
    const std::string headers = buf.substr(0, headers_len);

    const std::size_t sp1 = headers.find(' ');
    if (sp1 == std::string::npos) return -1;
    const std::size_t sp2 = headers.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return -1;

    out.method = headers.substr(0, sp1);
    out.path   = headers.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q = out.path.find('?');
    if (q != std::string::npos) out.path.resize(q);
    if (out.path.size() > VBDMCP_MAX_HEADER_SIZE) return -1;

    out.origin           = header_value(headers, "Origin");
    out.content_type     = header_value(headers, "Content-Type");
    out.accept           = header_value(headers, "Accept");
    out.session_id       = header_value(headers, "Mcp-Session-Id");
    out.protocol_version = header_value(headers, "MCP-Protocol-Version");

    /* 不支持 Transfer-Encoding: chunked（MCP 客户端均使用 Content-Length）。
     * 明确返回 -3，避免被当成"空 body"而得到误导性的 parse error。
     *
     * Transfer-Encoding: chunked is not supported (MCP clients use
     * Content-Length). Returning -3 explicitly avoids treating the request as an
     * empty body and producing a misleading parse error. */
    const std::string te = header_value(headers, "Transfer-Encoding");
    if (te.find("chunked") != std::string::npos) return -3;

    std::size_t content_length = 0;
    const std::string cl = header_value(headers, "Content-Length");
    if (!cl.empty()) {
        for (char ch : cl) {
            if (ch < '0' || ch > '9') return -1;
            content_length = content_length * 10 + (std::size_t)(ch - '0');
            if (content_length > VBDMCP_MAX_BODY_SIZE) return -2;
        }
    }

    out.total = headers_len + content_length;
    if (buf.size() < out.total) return 0;
    out.body = buf.substr(headers_len, content_length);
    return 1;
}

bool protocol_version_supported(const std::string& v)
{
    if (v.empty()) return true;    /* 宽松：未携带则不校验 / lenient: no header, no check */
    /* 客户端在 initialize 响应之后会把协商到的版本放进该头；
     * 因此这里必须接受整个支持集合（否则新 SDK 客户端会被 400 拒掉，
     * 表现为"无法连接"）。
     *
     * Clients put the negotiated version into this header after the initialize
     * response, so the whole supported set must be accepted (a 400 here shows up
     * as "cannot connect" in newer SDK clients). */
    return v == VBDMCP_PROTOCOL_2025_06_18 ||
           v == VBDMCP_PROTOCOL_2025_03_26 ||
           v == VBDMCP_PROTOCOL_LEGACY;
}

bool accept_has(const std::string& accept, const char* token)
{
    return accept.find(token) != std::string::npos ||
           accept.find("*/*") != std::string::npos;
}

bool is_mcp_path(const std::string& p)
{
    /* MCP 端点路径。接受 "/"、"/mcp" 以及带多余尾部 "/" 的 "/mcp/"：
     * 客户端的端点写法/拼接方式不一（有的配置到根路径，有的带尾斜杠），
     * 尾斜杠曾会被 404 —— 这种"看起来一样"的路径最容易踩坑。
     *
     * MCP endpoint paths: "/", "/mcp" and "/mcp/" with a trailing slash are all
     * accepted, because clients write/join the endpoint differently (some use the
     * root path, some add a trailing slash) and a trailing slash used to get a
     * 404 - "identical-looking" paths are the easiest trap. */
    return p.empty() || p == "/" || p == "/mcp" || p == "/mcp/";
}

/* ============================================================
 * 任务队列 / Task queue
 * ============================================================ */
bool task_queue_push(std::shared_ptr<UiTask> task)
{
    {
        std::lock_guard<std::mutex> lk(g_task_mu);
        if ((int)g_task_queue.size() >= kTaskQueueLimit) return false;
        g_task_queue.push_back(std::move(task));
    }
    if (g_task_hwnd && ::IsWindow(g_task_hwnd)) {
        ::PostMessage(g_task_hwnd, VBDMCP_WM_TASK, 0, 0);
    }
    return true;
}

/* UI 主线程：在 VBDMCP_WM_TASK 消息里调用，串行执行队列任务。
 * UI main thread: called from the VBDMCP_WM_TASK message, runs queued tasks serially. */
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
            /* 已被取消：排队期间客户端断开 / 超时 / 服务停止 —— 不执行业务
             * Cancelled while queued (client gone / timeout / server stop): do not run it. */
            task->response = json_error_document(VBDMCP_ERR_BUSY, _("request cancelled"));
            task->http_status = 200;
        } else {
            /* MCP 请求入口 try-catch：任何 C++ 异常都不允许带崩 VB Decompiler UI
             * try-catch around the MCP entry point: no C++ exception may crash the UI. */
            try {
                int is_init = 0;
                vbdmcp::OwnedString resp(
                    vbdmcp_process_jsonrpc(task->body.c_str(), task->peer.c_str(), &is_init));
                if (is_init) task->is_initialize.store(true);
                if (!resp) {
                    task->http_status = 200;
                    task->response = json_error_document(VBDMCP_ERR_INTERNAL, _("out of memory"));
                } else if (resp.get()[0] == '\0') {
                    task->http_status = 202;          /* notification：无响应体 / notification: no response body */
                    task->response.clear();
                } else {
                    task->http_status = 200;
                    task->response = resp.get();
                }
            } catch (const std::exception& e) {
                vbdmcp_logf(VBDMCP_LOG_ERROR, "tool call threw std::exception: %s", e.what());
                task->http_status = 200;
                task->response = json_error_document(VBDMCP_ERR_INTERNAL, _("internal error"));
            } catch (...) {
                vbdmcp_logf(VBDMCP_LOG_ERROR, "tool call threw unknown exception");
                task->http_status = 200;
                task->response = json_error_document(VBDMCP_ERR_INTERNAL, _("internal error"));
            }
        }

        {
            std::lock_guard<std::mutex> lk(task->mtx);
            task->done.store(true);
        }
        task->cv.notify_all();
    }
}

/* UI 主线程的消息窗口：只负责把网络线程投递的任务搬到 UI 上执行
 * The UI thread's message window: its only job is moving tasks posted by the
 * network thread onto the UI thread for execution. */
LRESULT CALLBACK task_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == VBDMCP_WM_TASK) {
        task_pump_on_ui_thread();
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ============================================================
 * /health（yyjson 构造）/ /health (built with yyjson)
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
    yyjson_mut_obj_add_strcpy(doc, server, "name", VBDMCP_PLUGIN_NAME);
    yyjson_mut_obj_add_strcpy(doc, server, "version", VBDMCP_VERSION);
    yyjson_mut_obj_add_strcpy(doc, server, "protocolVersion",
                              g_active_protocol_version.load(std::memory_order_acquire));
    yyjson_mut_obj_add_val(doc, root, "server", server);

    yyjson_mut_val* arr  = yyjson_mut_arr(doc);
    yyjson_mut_val* inst = yyjson_mut_arr_add_obj(doc, arr);
    yyjson_mut_obj_add_strcpy(doc, inst, "name", fname);
    yyjson_mut_obj_add_strcpy(doc, inst, "arch", "Win32");
    yyjson_mut_obj_add_int(doc, inst, "bits", 32);
    /* 非回环（局域网）监听时不暴露宿主绝对安装路径，只给文件名
     * On a non-loopback (LAN) listen address the host's absolute install path is
     * not exposed; only the file name is returned. */
    const bool loopback = (vbdmcp_bind_address() == VBDMCP_LOOPBACK_BIND);
    yyjson_mut_obj_add_strcpy(doc, inst, "path", loopback ? exepath : fname);
    yyjson_mut_obj_add_int(doc, inst, "pid", (long long)GetCurrentProcessId());
    yyjson_mut_obj_add_bool(doc, inst, "local", loopback);
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
 * 请求处理（网络线程）/ Request handling (network thread)
 * ============================================================ */
void handle_mcp_post(Connection* c, const HttpRequest& req)
{
    /* Accept 校验（宽松）：规范要求客户端同时声明 application/json 与
     * text/event-stream；实际客户端可能只声明其中之一（个别只发
     * text/event-stream，期望 SSE 响应），因此命中其一（或未携带）即放行 ——
     * 本服务器的响应体始终是 application/json。
     *
     * Lenient Accept check: the spec wants both application/json and
     * text/event-stream, but real clients may declare only one (some send just
     * text/event-stream expecting an SSE response), so one match (or a missing
     * header) is enough - this server always answers with application/json. */
    if (!req.accept.empty() &&
        !accept_has(req.accept, "application/json") &&
        !accept_has(req.accept, "text/event-stream")) {
        http_send_error(c, 406, VBDMCP_ERR_INVALID_REQUEST,
                        _("Accept must include application/json or text/event-stream"), req.origin);
        return;
    }
    if (!req.content_type.empty() &&
        req.content_type.find("application/json") == std::string::npos) {
        http_send_error(c, 415, VBDMCP_ERR_INVALID_REQUEST,
                        _("Content-Type must be application/json"), req.origin);
        return;
    }
    if (!protocol_version_supported(req.protocol_version)) {
        http_send_error(c, 400, VBDMCP_ERR_INVALID_REQUEST,
                        _("unsupported MCP-Protocol-Version"), req.origin);
        return;
    }
    if (!req.session_id.empty() && !c->session_id.empty() &&
        req.session_id != c->session_id) {
        http_send_error(c, 404, VBDMCP_ERR_INVALID_REQUEST, _("unknown session id"), req.origin);
        return;
    }
    if (!rate_limit_allow()) {
        http_send_error(c, 429, VBDMCP_ERR_BUSY, _("Too many requests"), req.origin);
        return;
    }

    /* 本实现不接 JSON-RPC 批量（数组）请求：按标准错误明确回绝
     * JSON-RPC batch (array) requests are not supported and are rejected with a
     * standard error. */
    std::size_t i = 0;
    while (i < req.body.size() && (req.body[i] == ' ' || req.body[i] == '\t')) i++;
    if (i < req.body.size() && req.body[i] == '[') {
        http_send_error(c, 400, VBDMCP_ERR_INVALID_REQUEST,
                        _("JSON-RPC batching is not supported"), req.origin);
        return;
    }

    /* 同一连接同时只允许一个在飞任务：c->pending 是单槽，覆盖它会让上一个
     * 请求永远收不到响应（任务却仍在队列里照常执行，对 vbd_write_* 还会造成
     * "客户端以为失败、实际已写入"的静默副作用）。
     *
     * One in-flight task per connection: c->pending is a single slot, and
     * overwriting it would leave the previous request unanswered (its task still
     * runs, and for vbd_write_* that silently means "client thinks it failed
     * while it actually wrote"). */
    if (c->pending) {
        http_send_error(c, 429, VBDMCP_ERR_BUSY,
                        _F("Another request from this connection is still in flight; wait for its response before sending the next one."),
                        req.origin);
        vbdmcp_logf(VBDMCP_LOG_WARN,
                   "tool task rejected: connection already busy (client=%s)",
                   c->peer.c_str());
        return;
    }

    auto task = std::make_shared<UiTask>();
    task->body    = req.body;
    task->peer    = c->peer;
    task->enqueue = std::chrono::steady_clock::now();

    if (!task_queue_push(task)) {
        /* 队列满：直接用 _F()（翻译 + 占位符校验），网络线程可安全调用
         * Queue full: _F() directly (translation + placeholder check) is safe to
         * call from the network thread. */
        const char *msg =
            _F("Server busy: %d MCP tasks are already queued on the VB Decompiler UI thread (limit %d). The request was rejected to keep the UI responsive; retry in a moment.",
               kTaskQueueLimit, kTaskQueueLimit);
        http_send_error(c, 429, VBDMCP_ERR_BUSY, msg, req.origin);
        vbdmcp_logf(VBDMCP_LOG_WARN, "tool task rejected: UI queue full (client=%s)",
                   c->peer.c_str());
        return;
    }

    /* 会话 id 不在这里分配：由 UI 线程执行完 initialize 后在任务对象上回标
     * （task->is_initialize），网络线程在响应就绪时再分配并回带 Mcp-Session-Id
     * （Streamable HTTP 规范）。这样无需解析请求体，也不会把含 "initialize"
     * 字样的普通工具请求（例如写入含该词的代码）误判为 initialize。
     *
     * The session id is not allocated here: the UI thread back-flags
     * task->is_initialize after running initialize, and the network thread
     * allocates and returns Mcp-Session-Id with the response (Streamable HTTP
     * spec). No body parsing is needed, and an ordinary tool request that merely
     * contains the word "initialize" (e.g. writing code with that word) is never
     * mistaken for it. */
    c->pending = task;
    g_session_was_used.store(true);
    vbdmcp_logf(VBDMCP_LOG_INFO, "tool task queued (%zu bytes, client=%s)",
               task->body.size(), c->peer.c_str());
}

void handle_request(Connection* c, const HttpRequest& req)
{
    /* DEBUG 级：把协商相关的头一并记下。排查"客户端连不上"时先开
     * VBDMCP_DEBUG_LOG=1，看这一行是否到达、以及 accept/协议版本/会话是否符合预期。
     *
     * DEBUG level: logs the negotiation-related headers. When hunting "client
     * cannot connect", enable VBDMCP_DEBUG_LOG=1 and check whether this line
     * arrives and whether accept / protocol version / session look as expected. */
    vbdmcp_logf(VBDMCP_LOG_DEBUG,
               "http %s %s (client=%s, body=%zu, accept='%s', ctype='%s', proto='%s', session=%s)",
               req.method.c_str(), req.path.c_str(), c->peer.c_str(), req.body.size(),
               req.accept.c_str(), req.content_type.c_str(),
               req.protocol_version.empty() ? "-" : req.protocol_version.c_str(),
               req.session_id.empty() ? "-" : "yes");

    if (req.method == "OPTIONS") {
        /* CORS 预检：未配置可信 origin 时不返回任何 CORS 头（禁止 `*`）
         * CORS preflight: no CORS headers at all when no trusted origin is
         * configured ("*" is forbidden). */
        http_send_raw(c, 204, "text/plain", req.origin, cors_headers(req.origin), "", false);
        return;
    }

    if (req.path == "/health" && req.method == "GET") {
        http_send_json(c, 200, req.origin, build_health_document());
        return;
    }

    if (!is_mcp_path(req.path) && req.path != "/sse") {
        http_send_error(c, 404, VBDMCP_ERR_INVALID_REQUEST, _("Not Found"), req.origin);
        return;
    }

    if (req.method == "GET") {
        if (!accept_has(req.accept, "text/event-stream")) {
            http_send_error(c, 406, VBDMCP_ERR_INVALID_REQUEST,
                            _("Accept must include text/event-stream"), req.origin);
            return;
        }
        sse_begin(c, req.origin);
        return;
    }

    if (req.method == "DELETE") {
        if (c->pending) c->pending->cancelled.store(true);
        c->session_id.clear();
        http_send_raw(c, 204, "text/plain", req.origin, "", "", false);
        vbdmcp_logf(VBDMCP_LOG_INFO, "session terminated by client (client=%s)", c->peer.c_str());
        return;
    }

    if (req.method == "POST") {
        handle_mcp_post(c, req);
        return;
    }

    http_send_error(c, 405, VBDMCP_ERR_INVALID_REQUEST, _("Method Not Allowed"), req.origin);
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
    if (r == 0) return true;                                     /* 对端 FIN / peer sent FIN */
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
    if (r == 0) return false;                    /* 对端关闭 / peer closed */

    c->last_active = std::chrono::steady_clock::now();
    if (c->is_sse) return true;                  /* SSE 上的客户端数据忽略 / client data on SSE is ignored */

    c->recv_buf.append(tmp, (std::size_t)r);
    if (c->recv_buf.size() > (std::size_t)VBDMCP_MAX_BODY_SIZE + VBDMCP_MAX_HEADER_SIZE) {
        http_send_error(c, 413, VBDMCP_ERR_INVALID_REQUEST, _("payload too large"));
        return true;
    }

    for (;;) {
        HttpRequest req;
        const int st = parse_request(c->recv_buf, req);
        if (st == 0) return true;
        if (st == -2) {
            http_send_error(c, 413, VBDMCP_ERR_INVALID_REQUEST, _("payload too large"));
            return true;
        }
        if (st == -3) {
            http_send_error(c, 400, VBDMCP_ERR_PARSE,
                            _("unsupported Transfer-Encoding: chunked; send Content-Length instead"));
            return true;
        }
        if (st < 0) {
            http_send_error(c, 400, VBDMCP_ERR_PARSE, _("malformed HTTP request"));
            return true;
        }
        c->recv_buf.erase(0, req.total);
        handle_request(c, req);
        if (c->close_after_send || c->is_sse) return vbdmcp_flush(c) != 0;
    }
}

/* 返回 false 表示连接应销毁 / false means the connection must be destroyed */
bool connection_poll_task(Connection* c, int timeout_ms)
{
    auto task = c->pending;
    if (!task) return true;

    if (task->done.load()) {
        std::string extra;
        /* initialize 成功 → 分配会话 id 并在本条响应里回带（规范要求）；
         * 后续请求的校验见 handle_mcp_post（仅当本连接已持有 id 时比对）。
         *
         * initialize succeeded -> allocate the session id and return it with this
         * response (spec requirement); later requests are validated in
         * handle_mcp_post (compared only when this connection already holds an
         * id). */
        if (task->is_initialize.load()) {
            if (c->session_id.empty()) c->session_id = make_session_id();
            extra = "Mcp-Session-Id: " + c->session_id;
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
        task->cancelled.store(true);            /* 取消令牌：UI 侧排队任务会被丢弃
                                                   cancellation token: the queued UI task is dropped */
        const char *msg =
            _F("Task timed out: the request waited longer than %d seconds for the VB Decompiler UI thread. The operation was cancelled; try again with a smaller scope (for example a single module or function).",
               timeout_ms / 1000);
        c->pending.reset();
        http_send_json(c, 200, "", json_error_document(VBDMCP_ERR_TIMEOUT, msg));
        vbdmcp_logf(VBDMCP_LOG_WARN, "tool task timeout after %lldms (client=%s)",
                   (long long)age, c->peer.c_str());
        return true;
    }

    if (client_gone(c)) {
        task->cancelled.store(true);            /* 客户端断开 → 及时释放后台任务
                                                   client gone -> release the background task promptly */
        vbdmcp_logf(VBDMCP_LOG_INFO, "client gone, task cancelled (client=%s)",
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
 * 网络线程主循环 / Network thread main loop
 * ============================================================ */
int task_timeout_ms()
{
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = VBDMCP_TASK_TIMEOUT_MS;
    char env[16] = {0};
    if (GetEnvironmentVariableA("VBDMCP_TASK_TIMEOUT_MS", env, sizeof(env)) > 0) {
        const int v = atoi(env);
        if (v >= 1000 && v <= 600000) cached = v;
    }
    return cached;
}

vbdmcp::SocketHandle create_listen_socket(const std::string& bind_addr, int port)
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
        return vbdmcp::SocketHandle();

    vbdmcp::SocketHandle listener;
    for (struct addrinfo* p = ai; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        if (bind(s, p->ai_addr, (int)p->ai_addrlen) == SOCKET_ERROR) {
            vbdmcp_logf(VBDMCP_LOG_ERROR, "bind(%s:%d) failed err=%d",
                       bind_addr.c_str(), port, WSAGetLastError());
            closesocket(s);
            continue;
        }
        if (listen(s, SOMAXCONN) == SOCKET_ERROR) { closesocket(s); continue; }
        /* 监听 socket 必须是非阻塞：select 报可读后要连续 accept 直到
         * WSAEWOULDBLOCK。若是阻塞模式，第二次 accept（此时已无等待连接）
         * 会永久挂起整个网络线程 —— 表现为"客户端能建立 TCP 连接，
         * 但服务器永远不回数据、也不检测断开"。
         *
         * The listening socket must be non-blocking: after select reports
         * readable, accept is called repeatedly until WSAEWOULDBLOCK. In blocking
         * mode the second accept (no pending connection left) would hang the
         * whole network thread - clients connect at TCP level but the server
         * never answers and never notices disconnects. */
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        listener.reset(s);
        break;
    }
    freeaddrinfo(ai);
    return listener;
}

/* bind 结果就绪（成功或失败）→ 唤醒 vbdmcp_server_start 的等待方
 * Bind result ready (success or failure) -> wake the waiter in vbdmcp_server_start. */
void signal_net_init_done()
{
    {
        std::lock_guard<std::mutex> lk(g_net_init_mu);
        g_net_init_done.store(true);
    }
    g_net_init_cv.notify_all();
}

void network_thread_main()
{
    const std::string bind_addr = vbdmcp_bind_address();
    const int port = g_port.load();

    vbdmcp::SocketHandle listener = create_listen_socket(bind_addr, port);
    if (!listener) {
        vbdmcp_logf(VBDMCP_LOG_ERROR, "listen socket creation failed (%s:%d)",
                   bind_addr.c_str(), port);
        g_net_inited.store(false);
        signal_net_init_done();
        return;
    }
    g_net_inited.store(true);
    signal_net_init_done();
    vbdmcp_logf(VBDMCP_LOG_INFO, "MCP Streamable HTTP listening on %s:%d (thread=%lu)",
               bind_addr.c_str(), port, GetCurrentThreadId());

    std::vector<std::unique_ptr<Connection>> conns;
    const int timeout_ms = task_timeout_ms();

    while (g_net_running.load()) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        FD_SET(listener.get(), &rfds);
        for (auto& c : conns) {
            if (!c->sock.valid()) continue;
            FD_SET(c->sock.get(), &rfds);       /* 含 SSE：用于探测断开 / including SSE: used to detect disconnects */
            if (c->want_write && c->send_off < c->send_buf.size())
                FD_SET(c->sock.get(), &wfds);
        }

        timeval tv{ 0, kSelectTimeoutMs * 1000 };
        const int n = select(0, &rfds, &wfds, nullptr, &tv);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEINTR) continue;
            vbdmcp_logf(VBDMCP_LOG_ERROR, "select failed err=%d", WSAGetLastError());
            break;
        }

        if (FD_ISSET(listener.get(), &rfds)) {
            bool refused = false;
            while ((int)conns.size() < VBDMCP_MAX_CONNECTIONS) {
                SOCKET cs = accept(listener.get(), nullptr, nullptr);
                if (cs == INVALID_SOCKET) break;
                int one = 1;
                setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
                /* 连接 socket 也显式设为非阻塞（不依赖是否继承监听 socket）：
                 * recv/send 的 WSAEWOULDBLOCK 分支据此工作，慢客户端也不会
                 * 把网络线程阻塞在大响应的 send 上。
                 *
                 * The connection socket is explicitly non-blocking too (never
                 * relying on inheritance from the listener): the WSAEWOULDBLOCK
                 * branches of recv/send depend on it, and a slow client cannot
                 * block the network thread inside send of a large response. */
                u_long nb = 1;
                ioctlsocket(cs, FIONBIO, &nb);
                auto conn = std::make_unique<Connection>();
                conn->sock.reset(cs);
                conn->peer = peer_name(cs);
                conn->last_active = std::chrono::steady_clock::now();
                vbdmcp_logf(VBDMCP_LOG_INFO, "client connected: %s", conn->peer.c_str());
                conns.push_back(std::move(conn));
                refused = false;                 /* 又收下了：允许下次再告警 / accepted again: allow the next warning */
            }
            /* 满额时新连接被内核直接丢掉，客户端只看到连接被重置。
             * 记一条日志（每次"开始满额"只记一次，否则每 50ms 轮询刷屏），
             * 否则"AI 客户端连不上"这类问题无从排查。
             *
             * At the limit the kernel drops new connections and clients just see
             * a reset. One log line per "limit reached" episode (logging every
             * 50ms poll would flood the file) - otherwise "AI client cannot
             * connect" is impossible to diagnose. */
            if (!refused && (int)conns.size() >= VBDMCP_MAX_CONNECTIONS) {
                refused = true;
                vbdmcp_logf(VBDMCP_LOG_WARN,
                           "accept: connection limit reached (%d), extra clients refused",
                           VBDMCP_MAX_CONNECTIONS);
            }
        }

        for (auto it = conns.begin(); it != conns.end();) {
            Connection* c = it->get();
            bool keep = true;

            if (c->sock.valid() && FD_ISSET(c->sock.get(), &wfds)) {
                if (!vbdmcp_flush(c)) keep = false;
            }
            if (keep && c->sock.valid() && FD_ISSET(c->sock.get(), &rfds)) {
                if (!connection_on_readable(c)) keep = false;
            }
            if (keep) keep = connection_poll_task(c, timeout_ms);
            if (keep && c->close_after_send && c->send_off >= c->send_buf.size()) keep = false;
            if (keep && c->dead) keep = false;      /* SSE 缓冲超限等主动断开 / forced close (SSE buffer overflow etc.) */

            if (!keep) {
                if (c->pending) {
                    c->pending->cancelled.store(true);    /* 断开即取消后台任务 / dropping the client cancels the background task */
                    c->pending.reset();
                }
                vbdmcp_logf(VBDMCP_LOG_INFO, "client disconnected: %s", c->peer.c_str());
                c->sock.reset();
                it = conns.erase(it);
            } else {
                ++it;
            }
        }

        sse_heartbeat_all(conns);
        flush_list_changed_notification(conns);

        /* 所有客户端都已断开 → 会话准入授权失效：
         * 下一次新连接首次调用工具时重新弹窗确认（服务重启同理）。
         *
         * Every client disconnected -> session authorization expires: the next
         * connection is asked again on its first tool call (same after a restart). */
        if (conns.empty() && g_session_was_used.exchange(false)) {
            vbdmcp_session_reset();
            vbdmcp_logf(VBDMCP_LOG_INFO, "all clients gone: session authorization reset");
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
    vbdmcp_logf(VBDMCP_LOG_INFO, "network thread exited");
}

void apply_globals()
{
    if (g_engine) {
        /* 宿主状态栏是 ANSI(A 系列)界面：必须用 _A()(GBK)，**不能**用 _()(UTF-8) ——
         * 把 UTF-8 译文交给引擎会显示乱码（"运行中" 变 "杩愯涓?"）。
         * 格式串不走 _F()：占位符由本行的 _snprintf_s 填充，_F() 只用于
         * "翻译 + 就地取实参"；_A() 是 16 槽 thread_local 轮转池，同一条语句里
         * 两次调用互不覆盖，纯 ASCII 时直接复用静态指针（不占槽位）。
         *
         * The host status bar is an ANSI (A-API) surface, so _A() (GBK) is required
         * and _() (UTF-8) must not be used - UTF-8 handed to the engine shows
         * mojibake. The format string does not go through _F(): its placeholders are
         * filled by the _snprintf_s on this line, and _F() is for "translate and
         * take the arguments right here". _A() uses a 16-slot thread_local rotating
         * pool, so two calls in one statement do not overwrite each other, and pure
         * ASCII text reuses the static pointer (consuming no slot). */
        char msg[160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    _A("MCP: %s on %s:%d"),
                    vbdmcp_server_is_running() ? _A("running") : _A("stopped"),
                    vbdmcp_bind_address().c_str(), g_port.load());
        vbd_set_status(msg);
    }
}

} // namespace

/* ============================================================
 * 对外接口 / Public API
 * ============================================================ */
void vbdmcp_http_set_protocol_version(const char* ver)
{
    if (!ver || !*ver) return;
    const size_t n = strlen(ver);
    char *copy = (char *)vbdmcp_alloc(n + 1);
    if (!copy) return;
    memcpy(copy, ver, n + 1);              /* 含结尾 NUL / including the trailing NUL */
    g_active_protocol_version.store(copy, std::memory_order_release);
}

std::string vbdmcp_bind_address()
{
    std::lock_guard<std::mutex> lk(g_bind_mu);
    return g_bind_addr.empty() ? std::string(VBDMCP_DEFAULT_BIND) : g_bind_addr;
}

void vbdmcp_set_bind_address(const char* addr)
{
    std::lock_guard<std::mutex> lk(g_bind_mu);
    g_bind_addr = (addr && *addr) ? addr : VBDMCP_DEFAULT_BIND;
}

int vbdmcp_server_start()
{
    if (g_net_running.load()) return 1;              /* 幂等 / idempotent */
    if (!g_enabled.load()) return 1;

    if (!g_winsock_inited.load()) {
        WSADATA wsadata;
        if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
            vbdmcp_logf(VBDMCP_LOG_ERROR, "WSAStartup failed");
            return 0;
        }
        g_winsock_inited.store(true);
    }

    /* 消息窗口必须在 UI 主线程创建：网络线程 PostMessage 投递任务给它
     * The message window must be created on the UI main thread: the network
     * thread PostMessages tasks to it. */
    if (!g_task_hwnd) {
        static const char* kClass = "VBDMCP_TASK_V1";
        WNDCLASS wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = task_wnd_proc;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassA(&wc);                          /* 已存在时忽略返回值 / return value ignored when the class exists */
        g_task_hwnd = CreateWindowExA(0, kClass, kClass, 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!g_task_hwnd) {
            vbdmcp_logf(VBDMCP_LOG_ERROR, "task window creation failed");
            return 0;
        }
    }

    g_net_init_done.store(false);
    g_net_running.store(true);
    g_net_thread = std::thread(network_thread_main);

    /* 等待 bind 结果：网络线程就绪后 notify（成功/失败都会），最多等 3 秒。
     * Wait for the bind result: the network thread notifies either way, with a
     * 3-second cap. */
    {
        std::unique_lock<std::mutex> lk(g_net_init_mu);
        g_net_init_cv.wait_for(lk, std::chrono::seconds(3),
                               [] { return g_net_init_done.load(); });
    }

    if (!g_net_inited.load()) {
        g_net_running.store(false);
        if (g_net_thread.joinable()) g_net_thread.join();
        return 0;
    }
    return 1;
}

void vbdmcp_server_signal_stop()
{
    g_net_running.store(false);
}

void vbdmcp_server_stop()
{
    if (g_net_running.exchange(false)) {
        if (g_net_thread.joinable()) g_net_thread.join();
    }
    g_net_init_done.store(false);
    cors_cache_invalidate();        /* 网络线程已 join：CORS 缓存失效，下次启动重读
                                       network thread joined: invalidate the CORS cache, re-read on next start */
    {
        std::lock_guard<std::mutex> lk(g_task_mu);
        for (auto& t : g_task_queue) t->cancelled.store(true);
        g_task_queue.clear();
    }
    vbdmcp_session_reset();          /* 服务停止 → 会话准入授权状态重置 / server stopped -> session authorization resets */

    if (g_task_hwnd) {
        ::DestroyWindow(g_task_hwnd);
        g_task_hwnd = nullptr;
    }

    /* 与 vbdmcp_server_start 里的 WSAStartup 配对：反复"停服→启服"会累积
     * Winsock 引用计数，用户反复开关时不该泄漏。必须在网络线程 join 之后再调用。
     *
     * Pairs with WSAStartup in vbdmcp_server_start: repeated stop/start cycles
     * would accumulate Winsock reference counts. Must run after the network
     * thread has been joined. */
    if (g_winsock_inited.exchange(false)) {
        WSACleanup();
    }
}

int vbdmcp_server_is_running()
{
    return g_net_inited.load() ? 1 : 0;
}

int vbdmcp_server_apply(int enable, int port)
{
    if (enable) {
        const bool port_changed = (port > 0 && port < 65536 && port != g_port.load());
        if (port_changed) g_port.store(port);
        g_enabled.store(1);

        if (vbdmcp_server_is_running()) {
            if (port_changed) {
                vbdmcp_logf(VBDMCP_LOG_INFO, "port changed to %d, restarting server",
                           g_port.load());
                vbdmcp_server_stop();
                if (!vbdmcp_server_start()) return 0;
            }
            apply_globals();
            return 1;
        }
        if (!vbdmcp_server_start()) return 0;
        apply_globals();
        return 1;
    }

    g_enabled.store(0);
    vbdmcp_server_stop();
    apply_globals();
    return 1;
}

void vbdmcp_notify_tools_list_changed()
{
    /* 服务未运行时不残留广播标志（否则下次启动的首个 SSE 连接会收到
     * 一次无意义的 list_changed 通知）。
     *
     * Never leave the broadcast flag set while the server is stopped (otherwise
     * the first SSE connection after the next start receives a meaningless
     * list_changed notification). */
    if (!vbdmcp_server_is_running()) return;
    {
        std::lock_guard<std::mutex> lk(g_list_changed_mu);
        g_list_changed_payload =
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}";
    }
    g_list_changed_pending.store(true);
}