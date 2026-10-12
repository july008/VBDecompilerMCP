/*
 * mcp_tools.h - MCP 工具实现（VB Decompiler 业务逻辑）
 *
 * 每个工具接收 JSON 参数字符串，返回 JSON 结果字符串（new alloc，调用方 vbmcp_free）。
 * 工具实现通过 vb_get_value / vb_set_value 访问 VB Decompiler 内部数据。
 */
#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include "json.h"

/* 在工具线程中分发：根据 tool_name 解析 args，返回结果 JSON。
 * 返回 NULL 表示工具名未知。 */
jv_value_t *mcp_dispatch_tool(const char *tool_name, const jv_value_t *args);

/* 构造工具 inputSchema 列表（用于 MCP initialize / tools/list）。
 * 返回值为 new alloc 的 JSON 字符串（数组）。调用方 vbmcp_free。 */
char *mcp_build_tools_schema(void);

/* 仅返回本地工具的 JSON 数组字符串（"[{...},{...}]"，new alloc）。
 * 用于 mcp_rpc 层的 tools/list 响应。 */
char *mcp_build_local_tools_array(void);

/* 本地工具总数（g_tools 数组长度，sentinel-终止）。 */
int mcp_local_tool_count(void);

#endif
