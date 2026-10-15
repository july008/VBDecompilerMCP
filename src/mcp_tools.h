/*
 * mcp_tools.h - MCP 工具实现（VB Decompiler 业务逻辑）
 *
 * 全部 JSON 构建/解析使用 yyjson（third_party/yyjson）。
 * 工具分两组，命名规范：
 *   vbd_read_*   只读操作（只读模式下可见、可调用）
 *   vbd_write_*  修改操作（仅在"允许写入"开启后出现在 tools/list 且可调用）
 *
 * 数据编码：所有工具接受可选参数 encoding（默认 "utf-8"，可传 "gbk"）。
 *   utf-8：读侧把引擎的 ANSI/GBK 字节转成 UTF-8 输出，写侧把客户端 UTF-8
 *          参数转回 ANSI/GBK 再交给引擎（MCP 报文必须是合法 UTF-8）；
 *   gbk  ：旧行为，字节原样透传（兼容按 ANSI 处理数据的客户端）。
 * 含自由文本的读结果附带 {"encoding": "..."} 字段声明实际编码。
 */
#ifndef MCP_TOOLS_H
#define MCP_TOOLS_H

#include "yyjson.h"

/* 分发一个工具调用，返回结果 JSON 字符串（new alloc，调用方 vbdmcp_free）。
 *
 * 参数：
 *   tool_name        工具名（vbd_read_* / vbd_write_*）
 *   args             已解析的 arguments 对象（yyyyjson 只读视图，可为 NULL）
 *   allow_write      当前是否允许写入（0=只读模式）
 *   out_error_code   [out] 错误分类码。0=成功；负值为 MCP 错误码（见
 *                    vbdmcp.h 的 VBDMCP_ERR_*）。可为 NULL。
 *
 * 返回 NULL 表示内存不足。工具业务错误（如项目未加载）以
 * VBDMCP_ERR_BUSINESS 返回，由 rpc 层作为 tools/call isError 结果包装。 */
char *mcp_dispatch_tool(const char *tool_name, yyjson_val *args,
                        int allow_write, int *out_error_code);

/* 把当前权限下可见的工具追加到调用方已有的 yyjson 数组（不创建 doc）。
 * 供 rpc 层在构造完整 JSON-RPC 响应时直接内联工具数组，避免字符串往返。 */
void mcp_build_tools_into(yyjson_mut_doc *doc, yyjson_mut_val *arr, int allow_write);

/* 工具是否为写入类（vbd_write_*）。未知工具返回 0。 */
int mcp_tool_is_write(const char *tool_name);

/* 当前可用工具数（随 allow_write 变化）。 */
int mcp_local_tool_count(int allow_write);

#endif /* MCP_TOOLS_H */