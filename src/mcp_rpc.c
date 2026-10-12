/*
 * mcp_rpc.c - JSON-RPC 2.0 分发 + MCP 协议层
 *
 * 处理以下 JSON-RPC 方法（MCP 协议要求的）：
 *   - initialize           → 返回 serverInfo + capabilities
 *   - ping                 → 返回 {}
 *   - tools/list           → 返回工具 schema 列表
 *   - tools/call           → 调用工具
 *   - notifications/*      → 不返回响应（HTTP 返回 200 空）
 *
 * 错误：返回 JSON-RPC 2.0 error 对象 {code, message}。
 *
 * 实现原则：
 *  - 全部同步处理（在 VB Decompiler 主线程中），避免 Callback Engine 线程安全问题。
 *  - 每个响应都会立即构造成字符串并返回给 HTTP 层。
 */
#include "vbmcp.h"
#include "json.h"
#include "mcp_tools.h"
#include <stdio.h>

/* ============================================================
 * JSON-RPC 响应构造辅助
 * ============================================================ */

static char *make_rpc_result(const char *id_raw, const char *result_json)
{
    strbuf_t b;
    sb_init(&b);
    /* 用 sb_append_str 自动 strlen，避免手算字节数错（C 里 \" 是 1 字节）。 */
    sb_append_str(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && *id_raw) sb_append_str(&b, id_raw);
    else sb_append_str(&b, "null");
    sb_append_str(&b, ",\"result\":");
    if (result_json) sb_append_str(&b, result_json);
    else sb_append_str(&b, "null");
    sb_append_char(&b, '}');
    sb_finalize(&b);
    return b.buf;
}

static char *make_rpc_error(const char *id_raw, int code, const char *msg)
{
    strbuf_t b;
    sb_init(&b);
    sb_append_str(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id_raw && *id_raw) sb_append_str(&b, id_raw);
    else sb_append_str(&b, "null");
    sb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    sb_append_json_string(&b, msg ? msg : "error");
    sb_append_str(&b, "}}");
    sb_finalize(&b);
    return b.buf;
}

/* ============================================================
 * 通知请求体解析（method, params, id）
 * 这些函数接收**已解析**的 jv_value_t*（由 vbmcp_process_jsonrpc
 * 入口处 jv_parse 一次得到），避免每个字段都重复解析整段 body。
 * 调用方负责 jv_free 返回的字符串/对象。
 * ============================================================ */

static char *extract_id_raw(const jv_value_t *req)
{
    if (!req || req->type != JV_OBJECT) return NULL;
    jv_value_t *id = jv_obj_get(req, "id");
    if (!id) return NULL;
    if (id->type == JV_NULL)  return vbmcp_strdup("null");
    if (id->type == JV_STRING) return vbmcp_strdup(id->u.s ? id->u.s : "");
    if (id->type == JV_NUMBER) {
        char tmp[64];
        int n = _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%g", id->u.n);
        if (n < 0) return NULL;
        return vbmcp_strndup(tmp, (size_t)n);
    }
    /* id 为对象/数组（罕见）：完整 dump 保形 */
    strbuf_t b;
    sb_init(&b);
    jv_dump(id, &b);
    sb_finalize(&b);
    return b.buf;
}

static char *extract_method(const jv_value_t *req)
{
    if (!req || req->type != JV_OBJECT) return NULL;
    const char *m = jv_obj_get_string(req, "method");
    return m ? vbmcp_strdup(m) : NULL;
}

/* 从 params.arguments 解出 tool name（MCP tools/call 规范）。 */
static char *extract_tool_name(const jv_value_t *req)
{
    if (!req || req->type != JV_OBJECT) return NULL;
    jv_value_t *params = jv_obj_get(req, "params");
    const char *n = (params && params->type == JV_OBJECT)
                    ? jv_obj_get_string(params, "name") : NULL;
    return n ? vbmcp_strdup(n) : NULL;
}

/* 从请求中取出 arguments 对象（深拷贝为新jv_value_t）。
 * 调用方负责 jv_free。改用 jv_deep_copy 替代 dump+parse（O(N) 节点遍历）。 */
static jv_value_t *extract_arguments(const jv_value_t *req)
{
    if (!req || req->type != JV_OBJECT) return NULL;
    jv_value_t *params = jv_obj_get(req, "params");
    if (!params || params->type != JV_OBJECT) return NULL;
    jv_value_t *args = jv_obj_get(params, "arguments");
    if (!args) return NULL;
    return jv_deep_copy(args);
}

/* ============================================================
 * tools/call 响应包装
 * tools/call 的 result 必须是 {"content":[{"type":"text","text":"<JSON>"}], "isError":bool}
 * ============================================================ */

static char *wrap_tool_result(const char *tool_result_json, int is_error)
{
    strbuf_t b;
    sb_init(&b);
    /* 用 sb_append_str 自动取 strlen——之前用 sb_append 写死 32 字节但字面量
     * "{\"content\":[{\"type\":\"text\",\"text\":\" 实际是 34 字节（C 里 \" 是 1 字节），
     * 漏 2 字节导致 tools/call 响应永远是截断的非法 JSON，CodeBuddy 严格
     * 解析失败 → 后续所有工具调用都丢 → 即便 list 显示 0 个也算了，反正 0 个
     * 调不出来。这里改成 sb_append_str 杜绝手算字节数。 */
    sb_append_str(&b, "{\"content\":[{\"type\":\"text\",\"text\":");
    sb_append_json_string(&b, tool_result_json ? tool_result_json : "");
    sb_printf(&b, "}],\"isError\":%s}", is_error ? "true" : "false");
    sb_finalize(&b);
    return b.buf;
}

/* 把协商后的协议版本写入 HTTP 层的全局变量，使后续响应头带
 * MCP-Protocol-Version。MCP 2025-03 规范要求 initialize 之后必带。
 * 由 vbmcp_process_jsonrpc("initialize") 调用一次。 */
void vbmcp_set_active_protocol_version(const char *ver)
{
    vbmcp_http_set_protocol_version(ver);
}

/* ============================================================
 * 主要：处理 JSON-RPC body，返回响应字符串（new alloc）
 * 返回 ""（空串）表示 notification（HTTP 仍 200 但无 body）
 * 返回 NULL 表示解析错误 / 不支持的方法（应返回 4xx 或 -32601）
 * ============================================================ */

char *vbmcp_process_jsonrpc(const char *body)
{
    if (!body || !*body) return make_rpc_error(NULL, -32700, "Parse error: empty body");

    /* 整个 RPC 请求只解析一次 JSON。所有 extract_* 都复用同一棵解析树，
     * 之前每个字段都 jv_parse(body) → 3 次解析；10MB project_text 等大 body
     * 影响最明显。 */
    jv_value_t *req = jv_parse(body);
    if (!req || req->type != JV_OBJECT) {
        jv_free(req);
        return make_rpc_error(NULL, -32700, "Parse error: invalid JSON-RPC request");
    }

    char *method = extract_method(req);
    char *id_raw = extract_id_raw(req);
    jv_value_t *args = extract_arguments(req);

    if (!method) {
        char *err = make_rpc_error(id_raw, -32600, "Missing method");
        if (args) jv_free(args);
        if (id_raw) vbmcp_free(id_raw);
        jv_free(req);
        return err;
    }

    char *resp = NULL;

    if (vb_streq(method, "initialize")) {
        /* 对照 iida-mcp：服务端硬编码回 "2025-03-26"，不做协商，
         * 与 iida 行为一致。CodeBuddy 客户端无论发哪个版本，都能接受
         * 服务端回 "2025-03-26"。 */
        vbmcp_set_active_protocol_version(VBMCP_PROTOCOL);

        strbuf_t b;
        sb_init(&b);
        sb_printf(&b,
            "{\"protocolVersion\":\"%s\","
            "\"capabilities\":{"
              "\"tools\":{\"listChanged\":false},"
              "\"prompts\":{\"listChanged\":false},"
              "\"resources\":{\"subscribe\":false,\"listChanged\":false}"
            "},"
            "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"},"
            "\"instructions\":\"VB Decompiler MCP server. Tools operate on the currently opened decompiled VB project. Get info first, then list/get/set modules, forms, functions.\"}",
            VBMCP_PROTOCOL,
            VBMCP_PLUGIN_NAME,
            VBMCP_VERSION);
        sb_finalize(&b);
        resp = make_rpc_result(id_raw, b.buf);
        sb_free(&b);
    }
    else if (vb_streq(method, "ping")) {
        resp = make_rpc_result(id_raw, "{}");
    }
    else if (vb_streq(method, "tools/list")) {
        /* MCP 规范要求 result.tools 是数组。
         * mcp_build_local_tools_array() 只返回 "[{...},{...}]"（将来合并上游数组用），
         * 这里必须用 mcp_build_tools_schema()，已包 {"tools":[...]}。 */
        char *schema = mcp_build_tools_schema();
        resp = make_rpc_result(id_raw, schema ? schema : "{\"tools\":[]}");
        if (schema) vbmcp_free(schema);
    }
    else if (vb_streq(method, "tools/call")) {
        char *tool_name = extract_tool_name(req);
        jv_value_t *result = mcp_dispatch_tool(tool_name ? tool_name : "", args);
        strbuf_t b;
        sb_init(&b);
        jv_dump(result, &b);
        sb_finalize(&b);
        int is_err = (result && jv_obj_get(result, "error") != NULL) ? 1 : 0;
        char *wrapped = wrap_tool_result(b.buf, is_err);
        resp = make_rpc_result(id_raw, wrapped);
        vbmcp_free(wrapped);
        sb_free(&b);
        if (result) jv_free(result);
        if (tool_name) vbmcp_free(tool_name);
    }
    else if (vb_streq(method, "notifications/initialized") ||
             vb_streq(method, "notifications/cancelled") ||
             vb_streq(method, "notifications/progress")) {
        /* notification: 不返回 JSON-RPC 响应，但 HTTP 必须 200 空 body */
        resp = vbmcp_strdup("");
    }
    else {
        char buf[256];
        snprintf(buf, sizeof(buf), "Method not found: %s", method);
        resp = make_rpc_error(id_raw, -32601, buf);
    }

    if (args) jv_free(args);
    if (id_raw) vbmcp_free(id_raw);
    if (method) vbmcp_free(method);
    jv_free(req);
    return resp;
}
