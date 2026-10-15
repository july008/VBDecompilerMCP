/*
 * mcp_tools.h - MCP 工具实现（VB Decompiler 业务逻辑）
 * mcp_tools.h - MCP tool implementations (VB Decompiler business logic)
 *
 * 全部 JSON 构建/解析使用 yyjson（third_party/yyjson）。
 * 工具分两组，命名规范：
 *   vbd_read_*   只读操作（只读模式下可见、可调用）
 *   vbd_write_*  修改操作（仅在"允许写入"开启后出现在 tools/list 且可调用）
 *
 * All JSON building/parsing goes through yyjson (third_party/yyjson).
 * Tools are split into two groups by naming convention:
 *   vbd_read_*   read-only (visible and callable in read-only mode)
 *   vbd_write_*  mutating operations (listed and callable only when write
 *                access is enabled)
 *
 * 数据编码：所有工具接受可选参数 encoding（默认 "utf-8"，可传 "gbk"）。
 *   utf-8：读侧把引擎的 ANSI/GBK 字节转成 UTF-8 输出，写侧把客户端 UTF-8
 *          参数转回 ANSI/GBK 再交给引擎（MCP 报文必须是合法 UTF-8）；
 *   gbk  ：兼容模式，字节原样透传（供按 ANSI 处理数据的客户端）。
 * 含自由文本的读结果附带 {"encoding": "..."} 字段声明实际编码。
 *
 * Data encoding: every tool accepts the optional parameter encoding (default
 * "utf-8"; "gbk" is also accepted).
 *   utf-8: the read side converts the engine's ANSI/GBK bytes to UTF-8, the
 *          write side converts client UTF-8 back to ANSI/GBK (MCP messages must
 *          be valid UTF-8);
 *   gbk  : compatibility mode, raw byte pass-through (for clients that handle
 *          the data as ANSI).
 * Read results containing free text carry an {"encoding": "..."} field stating
 * the actual encoding.
 */
#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include "yyjson.h"

/* 分发一个工具调用，返回结果 JSON 字符串（new alloc，调用方 vbdmcp_free）。
 *
 * 参数：
 *   tool_name        工具名（vbd_read_* / vbd_write_*）
 *   args             已解析的 arguments 对象（yyjson 只读视图，可为 NULL）
 *   allow_write      当前是否允许写入（0=只读模式）
 *   out_error_code   [out] 错误分类码。0=成功；负值为 MCP 错误码（见
 *                    vbdmcp.h 的 VBDMCP_ERR_*）。可为 NULL。
 *
 * 返回 NULL 表示内存不足。工具业务错误（如项目未加载）以
 * VBDMCP_ERR_BUSINESS 返回，由 rpc 层作为 tools/call isError 结果包装。
 *
 * Dispatches one tool call and returns the result JSON string (new alloc,
 * caller frees with vbdmcp_free).
 *
 * Parameters:
 *   tool_name        tool name (vbd_read_* / vbd_write_*)
 *   args             parsed arguments object (yyjson read view, may be NULL)
 *   allow_write      1 = write access enabled (0 = read-only mode)
 *   out_error_code   [out] error class; 0 = success, negative = MCP error code
 *                    (see VBDMCP_ERR_* in vbdmcp.h). May be NULL.
 *
 * NULL means out of memory. Tool business errors (e.g. project not loaded) are
 * returned as VBDMCP_ERR_BUSINESS and wrapped by the rpc layer as tools/call
 * isError results. */
char *mcp_dispatch_tool(const char *tool_name, yyjson_val *args,
                        int allow_write, int *out_error_code);

/* 把当前权限下可见的工具追加到调用方已有的 yyjson 数组（不创建 doc）。
 * 供 rpc 层在构造完整 JSON-RPC 响应时直接内联工具数组，避免字符串往返。
 *
 * Appends the tools visible under the current permission to a caller-provided
 * yyjson array (does not create a doc). Lets the rpc layer inline the tool array
 * directly into the JSON-RPC response, avoiding a string round-trip. */
void mcp_build_tools_into(yyjson_mut_doc *doc, yyjson_mut_val *arr, int allow_write);

/* 工具是否为写入类（vbd_write_*）。未知工具返回 0。
 * Whether a tool is a write tool (vbd_write_*). Unknown tools return 0. */
int mcp_tool_is_write(const char *tool_name);

/* 当前可用工具数（随 allow_write 变化）。
 * Number of currently available tools (varies with allow_write). */
int mcp_local_tool_count(int allow_write);

#endif /* MCP_TOOLS_H */
