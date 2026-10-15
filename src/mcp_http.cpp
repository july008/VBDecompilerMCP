/*
 * mcp_http.c - HTTP 服务器 + 事件循环
 *
 * 基于 Winsock + WSAAsyncSelect 实现非阻塞单线程事件驱动 HTTP 服务器。
 * 主线程（VB Decompiler 线程）串行处理所有 IO、HTTP 解析、JSON-RPC 分发。
 *
 * 关键考虑：
 *   - VB Decompiler 的 Callback Engine 不是线程安全的，HTTP 请求处理必须在
 *     VB Decompiler 主线程（消息循环所在线程）执行。本实现本身就是主线程，
 *     因此无需 PostMessage 跨线程投递。
 *   - 工作线程用于"未来可能的耗时操作"，但当前所有工具都是同步主线程完成。
 *
 * HTTP 端点：
 *   GET  /            → SSE 占位（"event: endpoint\ndata: /\n"）
 *   GET  /health      → {"status":"ok","service":"vbmcp"}
 *   POST /            → JSON-RPC 2.0（MCP 入口）
 *   OPTIONS *         → CORS 预检 204
 *   其它              → 404
 */
#include "vbmcp.h"
#include "json.h"
#include "mcp_tools.h"
#include <stdio.h>
#include <strsafe.h>

/* ============================================================
 * 全局状态定义
 * ============================================================ */
int   g_enabled   = VBMCP_DEFAULT_ENABLED;
int   g_port      = VBMCP_DEFAULT_PORT;
char *g_bindings  = NULL;
HWND  g_mcp_hwnd  = NULL;
SOCKET g_listen_sock = INVALID_SOCKET;

static int g_winsock_inited = 0;

/* 当前协商的 MCP 协议版本。initialize 后由 mcp_rpc 写入。
 * 用于在响应头里回 MCP-Protocol-Version（MCP 2025-03 规范要求
 * initialize 之后的所有响应都应带此头）。单线程访问无需锁。 */
static char g_active_protocol_version[32] = VBMCP_PROTOCOL;

void vbmcp_http_set_protocol_version(const char *ver)
{
    if (ver && *ver) {
        strncpy_s(g_active_protocol_version, sizeof(g_active_protocol_version),
                  ver, _TRUNCATE);
    }
}

/* ============================================================
 * 请求日志环形缓冲（用于 /debug/log 诊断）
 * 记录最近 64 条请求的 method/path/body/状态/响应摘要，
 * 单线程访问，无需锁。
 * ============================================================ */
#define VLOG_CAP       64
#define VLOG_BODY_MAX 1024
typedef struct vlog_entry {
    DWORD  tick;
    DWORD  seq;
    char   method[8];
    char   path[128];
    int    status;
    char   body_preview[VLOG_BODY_MAX + 1];
    char   resp_preview[256];
    size_t resp_len;
} vlog_entry_t;
static vlog_entry_t g_vlog[VLOG_CAP];
static int g_vlog_head = 0;   /* next write idx */
static int g_vlog_count = 0;  /* total written (capped at VLOG_CAP) */
static DWORD g_vlog_seq = 0;  /* increasing seq id */

/* 转义 body：换行/不可打印字符 → \n / \t / \\X / \?。用于 JSON 嵌入。 */
static void vlog_escape(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
    size_t di = 0;
    for (size_t si = 0; si < src_len && di + 2 < dst_cap; si++) {
        unsigned char c = (unsigned char)src[si];
        if (c == '\n')      { dst[di++] = '\\'; dst[di++] = 'n'; }
        else if (c == '\r') { dst[di++] = '\\'; dst[di++] = 'r'; }
        else if (c == '\t') { dst[di++] = '\\'; dst[di++] = 't'; }
        else if (c == '"')  { dst[di++] = '\\'; dst[di++] = '"'; }
        else if (c == '\\') { dst[di++] = '\\'; dst[di++] = '\\'; }
        else if (c < 0x20)  { /* skip */ }
        else                { dst[di++] = (char)c; }
    }
    dst[di] = '\0';
}

static void vlog_record(const char *method, const char *path,
                        const char *body, size_t body_len,
                        int status, const char *resp, size_t resp_len)
{
    vlog_entry_t *e = &g_vlog[g_vlog_head];
    e->tick = GetTickCount();
    e->seq  = g_vlog_seq++;
    e->status = status;
    e->resp_len = resp_len;
    strncpy_s(e->method, sizeof(e->method), method ? method : "?", _TRUNCATE);
    strncpy_s(e->path, sizeof(e->path), path ? path : "?", _TRUNCATE);
    vlog_escape(body ? body : "", body_len, e->body_preview, sizeof(e->body_preview));
    vlog_escape(resp ? resp : "", resp_len, e->resp_preview, sizeof(e->resp_preview));
    g_vlog_head = (g_vlog_head + 1) % VLOG_CAP;
    if (g_vlog_count < VLOG_CAP) g_vlog_count++;
}

/* ============================================================
 * 客户端结构（链表，单线程访问）
 * ============================================================ */

typedef struct mcp_client {
    SOCKET sock;
    struct mcp_client *prev, *next;
    /* 接收缓冲：累积已收到字节，解析完整 HTTP 请求 */
    char *recv_buf;
    size_t recv_len;
    size_t recv_cap;
    /* 发送缓冲 + 偏移 + 容量 */
    char *send_buf;
    size_t send_len;
    size_t send_off;
    size_t send_cap;
    int close_after_send;
} mcp_client_t;

static mcp_client_t *g_client_head = NULL;
static mcp_client_t *g_client_tail = NULL;
static long g_client_count = 0;

/* 主消息循环中所有访问都在同一线程，无需锁 */

/* ============================================================
 * 客户端链表操作
 * ============================================================ */

static mcp_client_t *client_create(SOCKET s)
{
    if (g_client_count >= VBMCP_MAX_CONNECTIONS) {
        closesocket(s);
        return NULL;
    }
    mcp_client_t *c = (mcp_client_t *)vbmcp_calloc(sizeof(*c));
    if (!c) { closesocket(s); return NULL; }
    c->sock = s;
    c->recv_cap = 4096;
    c->recv_buf = (char *)vbmcp_alloc(c->recv_cap);
    if (!c->recv_buf) { vbmcp_free(c); closesocket(s); return NULL; }
    if (g_client_tail) { g_client_tail->next = c; c->prev = g_client_tail; }
    else { g_client_head = c; c->prev = NULL; }
    g_client_tail = c;
    c->next = NULL;
    g_client_count++;
    return c;
}

static mcp_client_t *client_find(SOCKET s)
{
    for (mcp_client_t *c = g_client_head; c; c = c->next)
        if (c->sock == s) return c;
    return NULL;
}

static void client_destroy(mcp_client_t *c)
{
    if (!c) return;
    /* 从链表移除 */
    if (c->prev) c->prev->next = c->next;
    else g_client_head = c->next;
    if (c->next) c->next->prev = c->prev;
    else g_client_tail = c->prev;
    g_client_count--;

    /* 关闭 socket（在 WSAAsyncSelect 注销后） */
    shutdown(c->sock, SD_SEND);
    closesocket(c->sock);

    if (c->recv_buf) vbmcp_free(c->recv_buf);
    if (c->send_buf) vbmcp_free(c->send_buf);
    vbmcp_free(c);
}

/* ============================================================
 * 缓冲操作
 * ============================================================ */

static int recv_buf_ensure(mcp_client_t *c, size_t extra)
{
    if (c->recv_len + extra + 1 <= c->recv_cap) return 1;
    size_t nc = c->recv_cap;
    while (nc < c->recv_len + extra + 1) nc *= 2;
    char *nb = (char *)vbmcp_alloc(nc);
    if (!nb) return 0;
    if (c->recv_buf && c->recv_len)
        memcpy(nb, c->recv_buf, c->recv_len);
    vbmcp_free(c->recv_buf);
    c->recv_buf = nb;
    c->recv_cap = nc;
    return 1;
}

static int send_buf_append(mcp_client_t *c, const void *data, size_t n)
{
    if (!n) return 1;
    size_t need = c->send_len + n + 1;
    size_t cap = c->send_cap ? c->send_cap : 4096;
    while (cap < need) cap *= 2;
    char *nb = (char *)vbmcp_alloc(cap);
    if (!nb) return 0;
    size_t newcap = cap;
    if (c->send_buf && c->send_len)
        memcpy(nb, c->send_buf, c->send_len);
    memcpy(nb + c->send_len, data, n);
    vbmcp_free(c->send_buf);
    c->send_buf = nb;
    c->send_cap = newcap;
    c->send_len += n;
    c->send_buf[c->send_len] = '\0';
    return 1;
}

/* 非阻塞 flush：返回 1=ok, 0=error(销毁连接) */
static int client_flush(mcp_client_t *c)
{
    while (c->send_off < c->send_len) {
        int r = send(c->sock, c->send_buf + c->send_off, (int)(c->send_len - c->send_off), 0);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) return 1; /* 等待下次 FD_WRITE */
            return 0;
        }
        if (r == 0) return 0;
        c->send_off += (size_t)r;
    }
    /* 全部发完 */
    c->send_len = 0;
    c->send_off = 0;
    if (c->close_after_send) {
        shutdown(c->sock, SD_SEND);
    }
    return 1;
}

/* ============================================================
 * 构造 HTTP 响应并入队
 * ============================================================ */

static void http_send(mcp_client_t *c, int status_code, const char *status_text,
                      const char *content_type, const char *body, size_t body_len)
{
    char hdr[512];
    /* 对 application/json 严格不带 charset 参数：
     *   MCP Streamable HTTP 规范只规定 application/json，部分严格 MCP
     *   SDK（如 Anthropic TypeScript SDK）会按精确字符串匹配 Content-Type，
     *   发现 "application/json; charset=utf-8" 与期望不符就直接拒绝响应。
     * SSE 才需要 charset=utf-8（HTTP 协议要求 text/* 子类指定 charset）。
     * 调用方传非 application/json 时，仍会带 charset=utf-8（向前兼容）。 */
    int n;
    if (content_type && _stricmp(content_type, "application/json") == 0) {
        n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Mcp-Session-Id: %s\r\n"
            "MCP-Protocol-Version: %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Accept, MCP-Protocol-Version, Mcp-Session-Id\r\n"
            "\r\n",
            status_code, status_text,
            body_len,
            VBMCP_SESSION_ID,
            g_active_protocol_version);
    } else {
        n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "Mcp-Session-Id: %s\r\n"
            "MCP-Protocol-Version: %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Accept, MCP-Protocol-Version, Mcp-Session-Id\r\n"
            "\r\n",
            status_code, status_text,
            content_type, body_len,
            VBMCP_SESSION_ID,
            g_active_protocol_version);
    }
    if (n > 0) send_buf_append(c, hdr, (size_t)n);
    if (body && body_len > 0)
        send_buf_append(c, body, body_len);
    c->close_after_send = 1;
    client_flush(c);
}

/* 极简响应：只带 Content-Type + Content-Length + Connection: close。
 * 对照 iida-mcp（CodeBuddy 配合良好的实现），它只发这几个头。
 * 不带 Mcp-Session-Id / MCP-Protocol-Version / CORS —— 这些是可选头，
 * 严格客户端可能按精确字符串匹配，反而不接受。
 * 用于 POST / 的 JSON-RPC 响应。 */
static void http_send_minimal(mcp_client_t *c, int status_code, const char *status_text,
                              const char *content_type, const char *body, size_t body_len)
{
    char hdr[256];
    int n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);
    if (n > 0) send_buf_append(c, hdr, (size_t)n);
    if (body && body_len > 0)
        send_buf_append(c, body, body_len);
    c->close_after_send = 1;
    client_flush(c);
}

/* ============================================================
 * HTTP 请求行解析（提取 method, path）
 * ============================================================ */

/* ============================================================
 * HTTP 请求行解析（提取 method, path）
 * ============================================================ */

static void extract_method_path(const char *raw, size_t raw_len,
                                 char *method, size_t method_cap,
                                 char *path, size_t path_cap)
{
    method[0] = '\0';
    path[0] = '\0';
    const char *p = raw;
    const char *end = raw + raw_len;

    /* method */
    size_t mi = 0;
    while (p < end && *p != ' ' && *p != '\r' && *p != '\n' && mi < method_cap - 1)
        method[mi++] = *p++;
    method[mi] = '\0';
    while (p < end && *p == ' ') p++;

    /* path (含 query，我们剥除) */
    size_t pi = 0;
    while (p < end && *p != ' ' && *p != '\r' && *p != '\n' && pi < path_cap - 1)
        path[pi++] = *p++;
    path[pi] = '\0';
    char *q = strchr(path, '?');
    if (q) *q = '\0';
}

/* 在 recv_buf 中查找 \r\n\r\n 分隔 */
static const char *find_header_end(const char *buf, size_t len)
{
    if (len < 4) return NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i;
    }
    return NULL;
}

/* 解析 Content-Length（仅查找 header 部分） */
static size_t parse_content_length(const char *headers, size_t headers_len)
{
    const char *key = "Content-Length:";
    size_t key_len = 15;
    if (headers_len < key_len) return 0;
    for (size_t i = 0; i + key_len < headers_len; i++) {
        if (_strnicmp(headers + i, key, key_len) == 0 ||
            _strnicmp(headers + i, "content-length:", 15) == 0) {
            const char *p = headers + i + key_len;
            while (p < headers + headers_len && (*p == ' ' || *p == '\t')) p++;
            return (size_t)strtoull(p, NULL, 10);
        }
    }
    return 0;
}

/* 尝试解析并处理一个完整 HTTP 请求；返回 1=已处理, 0=数据不足, -1=错误 */
static int try_handle_request(mcp_client_t *c)
{
    const char *hdr_end = find_header_end(c->recv_buf, c->recv_len);
    if (!hdr_end) {
        /* 用实际 cap 而非 VBMCP_RECV_BUFSIZE 常量：recv_buf_ensure 会动态扩张 cap，
         * 旧常量上限会误拒超 64KB 的合法 headers。 */
        if (c->recv_len >= c->recv_cap - 1) return -1;
        return 0;
    }
    size_t headers_len = (size_t)(hdr_end - c->recv_buf) + 4;
    size_t content_length = parse_content_length(c->recv_buf, headers_len);
    if (content_length > VBMCP_MAX_BODY_SIZE) return -1;
    size_t total = headers_len + content_length;
    if (c->recv_len < total) return 0;

    /* 解析 method/path */
    char method[16] = {0};
    char path[1024] = {0};
    extract_method_path(c->recv_buf, headers_len, method, sizeof(method), path, sizeof(path));

    /* 取 body（如果存在） */
    const char *body = c->recv_buf + headers_len;
    size_t body_len = content_length;

    /* 复制 body 以保证 NUL 终止 */
    char *body_copy = NULL;
    if (body_len > 0) {
        body_copy = (char *)vbmcp_alloc(body_len + 1);
        if (!body_copy) return -1;
        memcpy(body_copy, body, body_len);
        body_copy[body_len] = '\0';
    } else {
        body_copy = vbmcp_strdup("");
    }

    /* 路由 */
    int handled_status = 200;
    const char *handled_resp = NULL;
    size_t handled_resp_len = 0;
    if (vb_streq(method, "OPTIONS")) {
        /* CORS 预检 - 返回 204 */
        char hdr[256];
        int n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, Accept, MCP-Protocol-Version, Mcp-Session-Id\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Content-Length: 0\r\n"
            "\r\n");
        if (n > 0) send_buf_append(c, hdr, (size_t)n);
        c->close_after_send = 1;
        client_flush(c);
        handled_status = 204;
    }
    else if (vb_streq(path, "/health")) {
        /* /health 不是 MCP 协议要求端点，但几乎所有客户端都会探测它。
         * 故意模仿 iida-mcp 的 "instance 列表" 风格：
         *   - status: 顶层 ok/ready
         *   - server: {name, version, protocolVersion}
         *   - instances: 当前 MCP 服务实例（VB Decompiler 进程）状态详情
         *   - ready: 1 表示服务完全就绪（监听 + tools 注册完成）
         * 与 iida 不同的是 vbmcp 通常只有 1 个实例（每个 VB Decompiler 进程）。 */
        DWORD pid = GetCurrentProcessId();
        char exepath[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, exepath, MAX_PATH);
        const char *fname = strrchr(exepath, '\\');
        fname = fname ? fname + 1 : exepath;
        int tools_n = mcp_local_tool_count();

        strbuf_t hb; sb_init(&hb);
        sb_printf(&hb,
            "{\"status\":\"ready\","
             "\"server\":{\"name\":\"%s\",\"version\":\"%s\",\"protocolVersion\":\"%s\"},"
             "\"instances\":[{"
                 "\"name\":\"%s\","
                 "\"arch\":\"Win32\",\"bits\":32,"
                 "\"path\":\"%s\","
                 "\"pid\":%lu,"
                 "\"local\":true,"
                 "\"state\":\"ready\","
                 "\"tools_count\":%d"
             "}],"
             "\"ready\":1}",
            VBMCP_PLUGIN_NAME, VBMCP_VERSION, VBMCP_PROTOCOL,
            fname, exepath, (unsigned long)pid, tools_n);
        sb_finalize(&hb);

        http_send(c, 200, "OK", "application/json", hb.buf, hb.len);
        handled_resp = hb.buf; handled_resp_len = hb.len;
        sb_free(&hb);
    }
    else if ((vb_streq(path, "/") || vb_str_empty(path)) && vb_streq(method, "GET")) {
        /* SSE 占位（CodeBuddy/Claude 等客户端 GET / 探测） */
        const char *sse =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
            "event: endpoint\r\ndata: /\r\n\r\n";
        send_buf_append(c, sse, strlen(sse));
        c->close_after_send = 1;
        client_flush(c);
        handled_resp = "event: endpoint\\ndata: /";
        handled_resp_len = strlen(handled_resp);
    }
    else if ((vb_streq(path, "/") || vb_streq(path, "/mcp") || vb_str_empty(path))
              && vb_streq(method, "POST")) {
        /* 主 JSON-RPC 入口
         * 对照 iida-mcp（CodeBuddy 配合良好的实现）：
         *   - Content-Type: application/json（不是 SSE）
         *   - 只带 Content-Type + Content-Length + Connection: close
         *   - 不带 Mcp-Session-Id / MCP-Protocol-Version / CORS 头
         *   - notification 返回 202 Accepted + 空 body
         * 路径同时支持 "/" 和 "/mcp" 兼容不同客户端实现。 */
        char *resp = vbmcp_process_jsonrpc(body_copy ? body_copy : "");
        if (resp && *resp) {
            /* 缓存 strlen：大 body（project_text 10MB+）两次 O(N) 太贵 */
            size_t rlen = strlen(resp);
            http_send_minimal(c, 200, "OK", "application/json", resp, rlen);
            handled_resp = resp; handled_resp_len = rlen;
            vbmcp_free(resp);
        } else if (resp && !*resp) {
            /* notification：202 Accepted + 空 body */
            http_send_minimal(c, 202, "Accepted", "application/json", "", 0);
            handled_resp = ""; handled_resp_len = 0;
            vbmcp_free(resp);
        } else {
            const char *err = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}";
            size_t elen = strlen(err);
            http_send_minimal(c, 200, "OK", "application/json", err, elen);
            handled_resp = err; handled_resp_len = elen;
        }
    }
    else if (vb_streq(path, "/sse")) {
        /* 历史 SSE 端点（同 GET /，发完即关） */
        const char *sse =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n"
            "event: endpoint\r\ndata: /\r\n\r\n";
        send_buf_append(c, sse, strlen(sse));
        c->close_after_send = 1;
        client_flush(c);
        handled_resp = "event: endpoint\\ndata: /";
        handled_resp_len = strlen(handled_resp);
    }
    else if (vb_streq(path, "/debug/schema")) {
        /* 调试端点：直接返回 tools/list 的完整 JSON-RPC 响应，
         * 方便用 curl 验证 schema 输出格式是否合法。
         * 仅 GET，无需 body。 */
        extern char *mcp_build_tools_schema(void);
        char *schema = mcp_build_tools_schema();
        if (schema) {
            /* 直接包成 JSON-RPC 响应（id=null）方便前端解析。
             * 用 strbuf 拼接，避免 sprintf + %s 对 schema 中潜在的 % 字符脆弱。 */
            strbuf_t sb;
            sb_init(&sb);
            sb_append_str(&sb, "{\"jsonrpc\":\"2.0\",\"id\":null,\"result\":");
            sb_append_str(&sb, schema);
            sb_append_char(&sb, '}');
            sb_finalize(&sb);
            http_send(c, 200, "OK", "application/json", sb.buf, sb.len);
            handled_resp = sb.buf; handled_resp_len = sb.len;
            sb_free(&sb);
            vbmcp_free(schema);
        } else {
            const char *err = "{\"error\":\"schema build failed\"}";
            http_send(c, 500, "Internal Error", "application/json", err, strlen(err));
            handled_status = 500;
            handled_resp = err; handled_resp_len = strlen(err);
        }
    }
    else if (vb_streq(path, "/debug/echo")) {
        /* 调试端点：原样回显 POST body（看 Content-Length / JSON 体是否正确） */
        http_send(c, 200, "OK", "application/json",
                  body_copy ? body_copy : "(no body)",
                  body_copy ? strlen(body_copy) : 9);
        handled_resp = body_copy ? body_copy : "(no body)";
        handled_resp_len = body_copy ? strlen(body_copy) : 9;
    }
    else if (vb_streq(path, "/debug/log")) {
        /* 调试端点：返回最近 N 条请求日志，方便看客户端发了什么。
         * 用 strbuf 拼成 JSON 数组。 */
        strbuf_t lb;
        sb_init(&lb);
        sb_append_str(&lb, "{\"count\":");
        sb_printf(&lb, "%d,\"entries\":[", g_vlog_count);
        int n_entries = g_vlog_count;
        int start = (g_vlog_head - n_entries + VLOG_CAP) % VLOG_CAP;
        for (int i = 0; i < n_entries; i++) {
            vlog_entry_t *e = &g_vlog[(start + i) % VLOG_CAP];
            if (i) sb_append_char(&lb, ',');
            sb_printf(&lb,
                "{\"seq\":%lu,\"tick\":%u,\"method\":\"%s\",\"path\":\"%s\","
                "\"status\":%d,\"resp_len\":%zu,"
                "\"body\":\"%s\",\"resp\":\"%s\"}",
                (unsigned long)e->seq, e->tick, e->method, e->path,
                e->status, e->resp_len,
                e->body_preview, e->resp_preview);
        }
        sb_append_str(&lb, "]}");
        sb_finalize(&lb);
        http_send(c, 200, "OK", "application/json", lb.buf, lb.len);
        handled_resp = lb.buf; handled_resp_len = lb.len;
        sb_free(&lb);
    }
    else {
        const char *err = "Not Found";
        http_send(c, 404, "Not Found", "text/plain", err, strlen(err));
        handled_status = 404;
        handled_resp = err; handled_resp_len = strlen(err);
    }

    /* 记录请求到环形缓冲（/debug/log 自身不记录，避免自己读自己） */
    if (!vb_streq(path, "/debug/log")) {
        vlog_record(method, path, body_copy, body_len,
                    handled_status, handled_resp, handled_resp_len);
    }

    if (body_copy) vbmcp_free(body_copy);

    /* 从 recv_buf 移除已处理的数据 */
    size_t remaining = c->recv_len - total;
    if (remaining > 0) memmove(c->recv_buf, c->recv_buf + total, remaining);
    c->recv_len = remaining;
    c->recv_buf[c->recv_len] = '\0';
    return 1;
}

/* 非阻塞读取所有可用数据并尝试解析（FD_READ 时调用） */
static int client_recv(mcp_client_t *c)
{
    for (;;) {
        if (!recv_buf_ensure(c, 2048)) return 0;
        int r = recv(c->sock, c->recv_buf + c->recv_len,
                     (int)(c->recv_cap - c->recv_len - 1), 0);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) break;
            return 0;
        }
        if (r == 0) return 0; /* 对端关闭 */
        c->recv_len += (size_t)r;
        c->recv_buf[c->recv_len] = '\0';

        /* 尝试解析尽可能多的请求 */
        for (;;) {
            int x = try_handle_request(c);
            if (x < 0) return 0;
            if (x == 0) break;
            if (c->close_after_send) break;
        }
        if (c->close_after_send) break;
        /* 接着读 */
    }
    return 1;
}

/* ============================================================
 * 监听 socket 自动重建设置（处理 VB Decompiler / 系统级 Winsock 干扰）
 * ============================================================ */

static SOCKET create_listen_socket(void)
{
    if (!g_bindings) g_bindings = vbmcp_strdup(VBMCP_DEFAULT_BIND);

    /* 解析绑定地址 */
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    char port_str[16];
    _snprintf_s(port_str, sizeof(port_str), _TRUNCATE, "%d", g_port);

    struct addrinfo *ai = NULL;
    if (getaddrinfo(g_bindings, port_str, &hints, &ai) != 0 || !ai)
        return INVALID_SOCKET;

    SOCKET result = INVALID_SOCKET;
    for (struct addrinfo *p = ai; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        /* SO_REUSEADDR 允许 TIME_WAIT 时重新 bind */
        int reuse = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

        if (bind(s, p->ai_addr, (int)p->ai_addrlen) == SOCKET_ERROR) {
            int e = WSAGetLastError();
            vbmcp_log("bind(%s:%d) failed, err=%d", g_bindings, g_port, e);
            closesocket(s);
            continue;
        }
        if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(s);
            continue;
        }
        result = s;
        break;
    }
    freeaddrinfo(ai);
    return result;
}

/* ============================================================
 * 窗口过程：socket 事件派发
 * ============================================================ */

static LRESULT CALLBACK mcp_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        if (wp == VBMCP_TIMER_CHECK) {
            /* 周期性健康检查：监听 socket 可能被外部 WSACleanup 销毁 */
            if (g_listen_sock == INVALID_SOCKET) {
                SOCKET ns = create_listen_socket();
                if (ns != INVALID_SOCKET) {
                    g_listen_sock = ns;
                    WSAAsyncSelect(g_listen_sock, hwnd, VBMCP_WM_LISTEN,
                                   FD_ACCEPT | FD_CLOSE);
                    vbmcp_log("timer: listen socket re-created");
                }
            }
        }
        return 0;

    case VBMCP_WM_LISTEN: {
        int events = LOWORD(lp);
        int err    = HIWORD(lp);

        /* 监听 socket 异常 → 重建 */
        if (err || (events & FD_CLOSE)) {
            vbmcp_log("WM_LISTEN error=%d events=0x%X, recreating", err, events);
            if (g_listen_sock != INVALID_SOCKET) {
                WSAAsyncSelect(g_listen_sock, hwnd, 0, 0);
                closesocket(g_listen_sock);
                g_listen_sock = INVALID_SOCKET;
            }
            SOCKET ns = create_listen_socket();
            if (ns != INVALID_SOCKET) {
                g_listen_sock = ns;
                WSAAsyncSelect(g_listen_sock, hwnd, VBMCP_WM_LISTEN, FD_ACCEPT | FD_CLOSE);
            }
            return 0;
        }

        /* accept 全部挂起连接 */
        for (;;) {
            SOCKET cs = accept(g_listen_sock, NULL, NULL);
            if (cs == INVALID_SOCKET) {
                int e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) break;
                /* accept 失败，可能是监听 socket 已失效 */
                vbmcp_log("accept failed err=%d", e);
                break;
            }
            /* nodelay + 注册事件 */
            int one = 1;
            setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            WSAAsyncSelect(cs, hwnd, VBMCP_WM_CLIENT, FD_READ | FD_WRITE | FD_CLOSE);
            client_create(cs);
        }
        return 0;
    }

    case VBMCP_WM_CLIENT: {
        SOCKET s = (SOCKET)wp;
        mcp_client_t *c = client_find(s);
        if (!c) return 0;

        int events = LOWORD(lp);
        int err    = HIWORD(lp);
        if (err) { client_destroy(c); return 0; }

        if (events & FD_READ) {
            if (!client_recv(c)) { client_destroy(c); return 0; }
        }
        if (events & FD_WRITE) {
            if (!client_flush(c)) { client_destroy(c); return 0; }
        }
        if (events & FD_CLOSE) {
            /* 半关闭：尝试继续读取残留数据 */
            client_recv(c);
            client_destroy(c);
            return 0;
        }
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ============================================================
 * 启动 / 停止
 * ============================================================ */

int vbmcp_server_start(void)
{
    vbmcp_log("vbmcp_server_start, enabled=%d, port=%d", g_enabled, g_port);
    if (g_listen_sock != INVALID_SOCKET) return 1;  /* 已启动，幂等返回 */

    if (!g_enabled) return 1;

    /* WSAStartup 一次，保持进程级 Winsock 引用计数 */
    if (!g_winsock_inited) {
        WSADATA wsadata;
        if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
            vbmcp_log("WSAStartup failed");
            return 0;
        }
        g_winsock_inited = 1;
    }

    /* 创建隐藏窗口（接收 socket 事件） */
    if (!g_mcp_hwnd) {
        WNDCLASS wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = mcp_wnd_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "VBMCP_SERVER_V1";
        if (!RegisterClass(&wc)) {
            /* 可能已注册（重复启动场景） */
            vbmcp_log("RegisterClass maybe-already-exists");
        }
        g_mcp_hwnd = CreateWindowEx(0, "VBMCP_SERVER_V1", "VBMCP_SERVER_V1",
                                     0, 0, 0, 0, 0,
                                     HWND_MESSAGE, NULL, GetModuleHandle(NULL), NULL);
        if (!g_mcp_hwnd) {
            vbmcp_log("CreateWindowEx failed");
            return 0;
        }
        SetTimer(g_mcp_hwnd, VBMCP_TIMER_CHECK, 5000, NULL);
    }

    /* 创建监听 socket */
    SOCKET s = create_listen_socket();
    if (s == INVALID_SOCKET) {
        vbmcp_log("create_listen_socket failed");
        return 0;
    }
    g_listen_sock = s;
    if (WSAAsyncSelect(g_listen_sock, g_mcp_hwnd, VBMCP_WM_LISTEN,
                       FD_ACCEPT | FD_CLOSE) == SOCKET_ERROR) {
        vbmcp_log("WSAAsyncSelect failed");
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
        return 0;
    }

    vbmcp_log("listening on %s:%d", g_bindings ? g_bindings : "0.0.0.0", g_port);
    return 1;
}

void vbmcp_server_stop(void)
{
    vbmcp_log("vbmcp_server_stop, clients=%ld", g_client_count);

    /* 销毁所有客户端 */
    while (g_client_head) client_destroy(g_client_head);

    if (g_listen_sock != INVALID_SOCKET) {
        WSAAsyncSelect(g_listen_sock, g_mcp_hwnd, 0, 0);
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }
    if (g_mcp_hwnd) {
        KillTimer(g_mcp_hwnd, VBMCP_TIMER_CHECK);
        DestroyWindow(g_mcp_hwnd);
        g_mcp_hwnd = NULL;
    }
}

int vbmcp_server_is_running(void)
{
    return g_listen_sock != INVALID_SOCKET ? 1 : 0;
}

/* 根据当前全局 g_enabled / g_port 同步运行时配置（需先更新全局变量再调用）。
 * 由 vbmcp_server_apply 内部使用。直接调用可保留旧行为。 */
static void apply_globals(void)
{
    /* 写入状态栏提示（如果有引擎） */
    if (g_engine) {
        char msg[128];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "MCP: %s on %s:%d",
                    vbmcp_server_is_running() ? "running" : "stopped",
                    g_bindings ? g_bindings : VBMCP_DEFAULT_BIND, g_port);
        vb_set_status(msg);
    }
}

int vbmcp_server_apply(int enable, int port)
{
    if (enable) {
        int port_changed = (port > 0 && port < 65536 && port != g_port);
        if (port_changed) g_port = port;
        g_enabled = 1;
        if (vbmcp_server_is_running()) {
            if (port_changed) {
                /* 已在运行但端口变了 —— 必须重启 server 才能绑新端口 */
                vbmcp_log("port changed to %d, restarting server", g_port);
                vbmcp_server_stop();
                if (!vbmcp_server_start()) {
                    /* 端口被占用：保留新 g_port 但状态切回 running=false */
                    vbmcp_log("server restart failed on new port %d", g_port);
                    apply_globals();
                    return 0;
                }
            }
            apply_globals();
            return 1;
        }
        if (!vbmcp_server_start()) {
            /* 启动失败：可能是端口被占用 */
            return 0;
        }
        apply_globals();
        return 1;
    } else {
        g_enabled = 0;
        vbmcp_server_stop();
        apply_globals();
        return 1;
    }
}
