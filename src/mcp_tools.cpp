/*
 * mcp_tools.cpp - MCP 工具实现（yyjson 版）
 * mcp_tools.cpp - MCP tool implementations (yyjson)
 *
 * 设计原则：
 *  - 工具名严格分为两组：
 *      vbd_read_*   只读操作（读取项目结构 / 源码 / 反编译信息 / 元数据）
 *      vbd_write_*  修改操作（保存、修改工程、覆盖导出、删除/清空项目资源）
 *    只读模式下 listTools 只返回 vbd_read_*；开启"允许写入"后才追加 vbd_write_*。
 *  - 输入为 JSON 对象（arguments），输出为 JSON：**全部使用 yyjson** 构建/解析。
 *  - 数据编码（见下方"数据编码"小节）：工具默认把引擎的 ANSI/GBK 字节转成
 *    UTF-8 输出、把客户端 UTF-8 参数转回 ANSI/GBK 写入；可选参数 encoding:"gbk"
 *    改为字节透传的兼容模式。含自由文本的结果附带 {"encoding": "..."} 字段。
 *  - 业务错误返回 {"error":"..."}（isError:true 的工具结果，占用 MCP 内容通道，
 *    不占用 JSON-RPC 错误通道）；权限/参数类错误经 out_err 由 dispatcher 分类。
 *  - 错误分类由每个 impl 通过 `int *out_err` 显式回填 VBDMCP_ERR_*，
 *    再由 mcp_dispatch_tool 上报给 rpc 层。
 *  - 所有工具实现都假定在主线程调用（避开 Callback Engine 线程安全问题）。
 *
 * Design rules:
 *  - Tool names fall into exactly two groups:
 *      vbd_read_*   read-only (project structure / source / decompilation data)
 *      vbd_write_*  mutating (save, edit project, overwrite exports, clear buffers)
 *    In read-only mode listTools returns vbd_read_* only; enabling write access
 *    appends vbd_write_*.
 *  - Input is a JSON object (arguments), output is JSON, all built/parsed with
 *    yyjson.
 *  - Data encoding (see the "data encoding" section below): by default the tools
 *    convert the engine's ANSI/GBK bytes to UTF-8 on output and client UTF-8 back
 *    to ANSI/GBK on input; encoding:"gbk" selects the compatibility pass-through
 *    mode. Results with free text carry an {"encoding": "..."} field.
 *  - Business errors return {"error":"..."} (an isError:true tool result on the
 *    MCP content channel, not the JSON-RPC error channel); permission/parameter
 *    errors are classified by the dispatcher from out_err.
 *  - Error classes are filled in explicitly by each impl via `int *out_err`
 *    (VBDMCP_ERR_*) and reported to the rpc layer by mcp_dispatch_tool.
 *  - Every tool implementation assumes it runs on the main thread (callback
 *    engine thread safety).
 */
#include "mcp_tools.h"
#include "vbdmcp.h"
#include "i18n.h"
#include "marks.h"      /* 函数标记（分析进度跟踪，vbdmcp 自有数据）
                              function marks (progress tracking, vbdmcp's own data) */
#include <ctype.h>
#include <stdio.h>
#include <string>
#include <vector>

/* ============================================================
 * 结果构造辅助（yyjson）/ Result builders (yyjson)
 * ============================================================ */

/* 序列化并释放 mutable doc。失败时回退到一个合法的空对象。
 * Serializes and frees a mutable doc; falls back to a valid empty object. */
static char *doc_to_string(yyjson_mut_doc *doc)
{
    char *s = NULL;
    if (doc) {
        s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
        yyjson_mut_doc_free(doc);
    }
    if (!s) s = vbdmcp_strdup("{}");
    return s;
}

/* 新建 doc + 顶层 object（已 set_root）。失败返回 NULL。
 * Creates a doc plus a top-level object (set as root); NULL on failure. */
static yyjson_mut_doc *new_obj_doc(yyjson_mut_val **root_out)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) { yyjson_mut_doc_free(doc); return NULL; }
    yyjson_mut_doc_set_root(doc, root);
    *root_out = root;
    return doc;
}

/* 业务/权限错误负载：{"error":"..."}；同时把错误分类写回 out_err
 * （由 mcp_dispatch_tool 读取后上报给 rpc 层）。
 *
 * Business / permission error payload {"error":"..."}; also writes the error
 * class to out_err, which mcp_dispatch_tool reports to the rpc layer. */
static char *err_json(int *out_err, int code, const char *msg)
{
    if (out_err) *out_err = code;
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return vbdmcp_strdup("{\"error\":\"out of memory\"}");
    yyjson_mut_obj_add_strcpy(doc, root, "error", msg ? msg : "");
    return doc_to_string(doc);
}

/* ============================================================
 * 数据编码（MCP JSON ↔ VB Decompiler 引擎）
 *
 * MCP 报文（JSON）必须是 UTF-8，而引擎的文本字段是 ANSI（中文 Windows = GBK）
 * 字节。每个工具都接受可选参数 encoding：
 *   "utf-8"（默认）：读侧 GBK→UTF-8、写侧 UTF-8→GBK（双向转换）；
 *   "gbk"          ：兼容模式——字节原样透传（供按 ANSI 处理数据的客户端）。
 * 含自由文本的结果额外带 {"encoding": "..."} 字段，声明该结果的实际编码。
 *
 * Data encoding (MCP JSON <-> VB Decompiler engine).
 *
 * MCP messages (JSON) must be UTF-8 while the engine's text fields are ANSI bytes
 * (GBK on Chinese Windows). Every tool accepts the optional encoding parameter:
 *   "utf-8" (default): GBK->UTF-8 on read, UTF-8->GBK on write (bidirectional);
 *   "gbk"            : compatibility mode - raw byte pass-through (for clients
 *                      that treat the data as ANSI).
 * Results with free text carry {"encoding": "..."} stating the encoding used.
 * ============================================================ */
enum { ENC_UTF8 = 0, ENC_GBK = 1 };

/* 引擎文本（ANSI/GBK）→ 结果文本：返回 new alloc（vbdmcp_free）。
 * Engine text (ANSI/GBK) -> result text: new alloc (frees with vbdmcp_free). */
static char *enc_out(const char *engine_text, int enc)
{
    if (!engine_text) return NULL;
    return (enc == ENC_GBK) ? vbdmcp_strdup(engine_text)
                            : vbdmcp_acp_to_utf8(engine_text);
}

/* 客户端文本（UTF-8 JSON）→ 引擎文本（ANSI/GBK）：返回 new alloc（vbdmcp_free）。
 * Client text (UTF-8 JSON) -> engine text (ANSI/GBK): new alloc (vbdmcp_free). */
static char *enc_in(const char *json_text, int enc)
{
    if (!json_text) return NULL;
    return (enc == ENC_GBK) ? vbdmcp_strdup(json_text)
                            : vbdmcp_str_to_gbk(json_text);
}

/* 把引擎文本按编码转换后写入对象的字符串字段。
 * Converts engine text per encoding and stores it in an object string field. */
static void add_text(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                     const char *key, const char *engine_text, int enc)
{
    char *t = enc_out(engine_text, enc);
    yyjson_mut_obj_add_strcpy(doc, obj, key, t ? t : "");
    if (t) vbdmcp_free(t);
}

/* 数组版本（表单名 / 模块名列表等）。
 * Array variant (form names / module names ...). */
static void arr_add_text(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                         const char *engine_text, int enc)
{
    char *t = enc_out(engine_text, enc);
    yyjson_mut_arr_add_strcpy(doc, arr, t ? t : "");
    if (t) vbdmcp_free(t);
}

/* 含自由文本的结果：声明实际使用的编码（utf-8 / gbk），便于客户端判断。
 * Results with free text declare the encoding actually used (utf-8 / gbk). */
static void add_encoding_field(yyjson_mut_doc *doc, yyjson_mut_val *root, int enc)
{
    yyjson_mut_obj_add_strcpy(doc, root, "encoding", enc == ENC_GBK ? "gbk" : "utf-8");
}

/* 写工具：把客户端文本转成引擎字节并调用 vbd_set_value（Set 语义见 vbdec_sdk.cpp）。
 * Write helper: converts client text to engine bytes and calls vbd_set_value
 * (Set semantics: see vbdec_sdk.cpp). */
static void set_text_value(int vlType, const char *json_text, int enc, int idx, int fn)
{
    char *ansi = enc_in(json_text, enc);
    vbd_set_value(vlType, ansi ? ansi : (json_text ? json_text : ""), idx, fn);
    if (ansi) vbdmcp_free(ansi);
}

/* ============================================================
 * 参数读取辅助（读侧 yyjson API）/ Argument readers (yyjson, read side)
 * ============================================================ */

/* 读取整型参数。兼容 integer / real / 数字字符串。缺失或非法返回 def。
 * Reads an integer argument (integer / real / numeric string); def when missing or invalid. */
static long long arg_int(yyjson_val *args, const char *key, long long def)
{
    yyjson_val *v = args ? yyjson_obj_get(args, key) : NULL;
    if (!v) return def;
    if (yyjson_is_num(v)) return (long long)yyjson_get_sint(v);
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        if (!s || !*s) return def;
        char *end = NULL;
        long long r = strtoll(s, &end, 10);
        return (end == s) ? def : r;
    }
    return def;
}

/* 读取字符串参数；缺失或非字符串返回 NULL。
 * Reads a string argument; NULL when missing or not a string. */
static const char *arg_str(yyjson_val *args, const char *key)
{
    yyjson_val *v = args ? yyjson_obj_get(args, key) : NULL;
    return (v && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}

/* 读取 encoding 参数：缺省 / "utf-8" → UTF-8 双向转换；"gbk" → 字节透传兼容模式。
 * Reads the encoding argument: missing / "utf-8" -> bidirectional UTF-8;
 * "gbk" -> raw byte pass-through compatibility mode. */
static int arg_encoding(yyjson_val *args)
{
    const char *e = arg_str(args, "encoding");
    if (e && _stricmp(e, "gbk") == 0) return ENC_GBK;
    return ENC_UTF8;
}

/* Native 项目守护：rename_* / UpdateAll 在 native 项目上会触发 modal
 * "Please fill forms and modules before call UpdateAll"，阻塞 RPC 调用栈。
 * 返回 1=是 native 项目（应拒绝），0=非 native（允许）。
 *
 * Native-project guard: rename_* / UpdateAll trigger the modal dialog "Please
 * fill forms and modules before call UpdateAll" on native projects, blocking the
 * RPC call stack. Returns 1 = native project (must be refused), 0 = safe. */
static int is_native_project(void)
{
    char *s = vbd_get_value(VBD_IsNativeCompilation, 0, 0);
    int nat = (s && s[0] == '1') ? 1 : 0;
    if (s) vbdmcp_free(s);
    return nat;
}

/* 安全地把整数字符串解析为 long long（容错）。
 * Tolerant long long parser for integer strings. */
static long long safe_parse_int(const char *s, long long def)
{
    if (!s || !*s) return def;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    return (end == s) ? def : v;
}

/* 宿主引擎的索引上界（模块/表单/函数数量级，留足余量）。
 * 客户端可以传任意大的整数：arg_int 返回 long long，直接 (int) 截断会把
 * 2^40 变成 0 或负数，再交给 VB Decompiler 引擎就是越界访问 → 宿主崩溃。
 * 所以所有索引一律过这个函数校验。
 *
 * Upper bound for host engine indices (module/form/function scale, generous).
 * Clients may send arbitrarily large integers: arg_int yields long long, and a
 * plain (int) truncation would turn 2^40 into 0 or a negative number, which the
 * engine would treat as an out-of-range access and crash the host. All indices
 * therefore pass through this check. */
static const long long kMaxEngineIndex = 1000000;

static int arg_index(yyjson_val *args, const char *name, int def)
{
    const long long v = arg_int(args, name, def);
    if (v < 0 || v > kMaxEngineIndex) return -1;
    return (int)v;      /* 校验后再截断 / truncate only after the range check */
}

/* ---------------- 索引越界预检 / Index range pre-check ----------------
 * 引擎对超界索引不报错，而是返回哨兵值（实测 form_index=999999 时
 * form_name / form_text 都是 "Error"），直接透传会被 AI 当成真实数据；
 * 越界的写操作更不应该执行。因此所有带索引的工具在进入业务逻辑前
 * 先做一次上限校验，越界直接返回参数错误。
 *
 * The engine does not report an error for out-of-range indices; it returns
 * sentinel values (measured: form_index=999999 yields "Error" for both form_name
 * and form_text) that an AI would mistake for real data, and out-of-range writes
 * must never run at all. Every indexed tool therefore checks the upper bound
 * before entering business logic and returns a parameter error when out of range. */

/* 窗体总数（引擎直接提供，调用很便宜）
 * Total form count (provided by the engine; very cheap to call). */
static long long form_total(void)
{
    char *s = vbd_get_value(VBD_GetVBFormCount, 0, 0);
    const long long n = safe_parse_int(s, 0);
    if (s) vbdmcp_free(s);
    return n;
}

/* 指定模块的函数数 / Function count of one module */
static long long module_function_total(int midx)
{
    char *s = vbd_get_value(VBD_GetModuleFunctionCount, midx, 0);
    const long long n = safe_parse_int(s, 0);
    if (s) vbdmcp_free(s);
    return n;
}

/* 模块总数：复用模块名探测缓存（定义见下方 get_module_names）
 * Module count: reuses the module-name probe cache (see get_module_names below). */
static long long module_total(void);

/* 按指定进制解析无符号整数（base=16 时允许 0x 前缀）。非法字符/超 32 位返回 -1。
 * 用于 vbd_find_function 的地址匹配："89FB20"（十六进制）与 "9042720"（十进制）
 * 在字面上无法区分，调用方按两种进制各解析一次、任一命中即可。
 *
 * Parses an unsigned integer in the given base (0x prefix allowed for 16).
 * -1 on invalid characters or > 32 bits. Used for address matching in
 * vbd_find_function: "89FB20" (hex) and "9042720" (decimal) are
 * indistinguishable as text, so callers parse both ways and accept either. */
static long long parse_uint_base(const char *s, int base)
{
    if (!s || !*s) return -1;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    long long v = 0;
    int digits = 0;
    for (; *s; s++) {
        const unsigned char ch = (unsigned char)*s;
        int d;
        if (ch >= '0' && ch <= '9')                        d = ch - '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f')     d = ch - 'a' + 10;
        else if (base == 16 && ch >= 'A' && ch <= 'F')     d = ch - 'A' + 10;
        else return -1;
        if (d >= base) return -1;
        v = v * base + d;
        if (v > 0xFFFFFFFFLL) return -1;
        digits++;
    }
    return digits > 0 ? v : -1;
}

/* ---------------- 写入辅助 / Write helpers ---------------- */

/* 读取 "0"/"1" 开关参数（缺省 def）。
 * Reads a "0"/"1" on/off argument (def when absent). */
static int arg_onoff(yyjson_val *args, const char *name, int def)
{
    const char *s = arg_str(args, name);
    if (!s || !*s) return def;
    return (s[0] == '1') ? 1 : 0;
}

/* ---- include_previous：写入类工具回传"写入前的原值"，便于出错回滚 ----
 * 用法：
 *     char *prev = NULL;
 *     want_previous(args, VBD_GetVBForm, (int)idx, 0, &prev);   // 写入前
 *     ... 写入 ...
 *     emit_previous(doc, root, prev, enc);                        // 响应中输出 + 释放
 * 未请求 include_previous 时 prev 保持 NULL，两步都是零开销。
 *
 * include_previous: write tools can return the pre-write value for rollback.
 * Usage: [same snippet]
 * When include_previous is not requested, prev stays NULL and both steps are
 * free. */
static void want_previous(yyjson_val *args, int vl, int a, int b, char **prev)
{
    *prev = NULL;
    if (arg_onoff(args, "include_previous", 0)) *prev = vbd_get_value(vl, a, b);
}

static void emit_previous(yyjson_mut_doc *doc, yyjson_mut_val *root, char *prev, int enc)
{
    if (!prev) return;
    add_text(doc, root, "previous", prev, enc);
    vbdmcp_free(prev);
}

/* ---------------- vbd_write_function 的写入规范化 ----------------
 * 引擎的 SetModuleFunction 期望的是**函数体**（不含首行
 * "Private Sub xxx(...) 'addr"，也不含末尾 "End Sub"），它会自己补上这层
 * 框架。若把 vbd_read_function 的 code_fast（**含**签名与 End Sub）原样
 * 写回，会得到重复的签名与 End Sub（实测：写回后 code_fast 出现两行签名
 * + 两个 End Sub，代码被破坏）。因此这里做幂等处理：识别并剥离首行签名
 * 与末尾 End * 行，让"写完整函数"与"写函数体"两种用法都安全。
 *
 * Write normalization for vbd_write_function.
 * SetModuleFunction expects the function BODY (without the leading
 * "Private Sub xxx(...) 'addr" line and without the trailing "End Sub"); the
 * engine adds that wrapper itself. Writing back vbd_read_function's code_fast
 * (which INCLUDES the wrapper) would duplicate the signature and End Sub
 * (measured: the written code_fast then holds two signatures and two End Subs,
 * corrupting the code). The normalization is idempotent: it strips a leading
 * signature and a trailing End * line, making both "write the full function" and
 * "write only the body" safe. */

/* 首行（去掉可选修饰符后）的首关键字是否是 Sub / Function / Property
 * Whether the first keyword of the line (after optional modifiers) is Sub / Function / Property. */
static int looks_like_vb_signature(const std::string& line)
{
    size_t i = 0;
    while (i < line.size() && isspace((unsigned char)line[i])) i++;
    static const char *mods[] = { "Private", "Public", "Friend", "Global", "Static", NULL };
    for (int m = 0; mods[m]; m++) {
        const size_t ml = strlen(mods[m]);
        if (i + ml <= line.size() && line.compare(i, ml, mods[m]) == 0) {
            size_t j = i + ml;
            while (j < line.size() && isspace((unsigned char)line[j])) j++;
            if (j == i + ml) continue;          /* 修饰符后必须有空白 / whitespace must follow the modifier */
            i = j;
            break;
        }
    }
    static const char *kws[] = { "Sub", "Function", "Property", NULL };
    for (int k = 0; kws[k]; k++) {
        const size_t kl = strlen(kws[k]);
        if (i + kl <= line.size() && line.compare(i, kl, kws[k]) == 0) {
            const size_t m = i + kl;
            return (m >= line.size()) || isspace((unsigned char)line[m]) ||
                   line[m] == '(' || line[m] == '=';
        }
    }
    return 0;
}

/* trim 后是否等于 End Sub / End Function / ...
 * Whether the trimmed line equals End Sub / End Function / ... */
static int is_vb_end_line(const std::string& line)
{
    size_t b = 0, e = line.size();
    while (b < e && isspace((unsigned char)line[b])) b++;
    while (e > b && isspace((unsigned char)line[e - 1])) e--;
    const std::string t = line.substr(b, e - b);
    static const char *ends[] = { "End Sub", "End Function", "End Property",
                                  "End Enum", "End Type", "End Declare",
                                  "End Structure", NULL };
    for (int k = 0; ends[k]; k++) if (t == ends[k]) return 1;
    return 0;
}

static int is_blank_line(const std::string& s)
{
    return s.find_first_not_of(" \t\r") == std::string::npos;
}

/* 把"完整函数"规范化为引擎期望的函数体。返回被剥离的行数（0 = 未剥离）。
 * Normalizes a "full function" into the body the engine expects; returns the
 * number of stripped lines (0 = nothing stripped). */
static int strip_function_wrapper(std::string *io)
{
    if (!io) return 0;
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= io->size(); i++) {
        if (i == io->size() || (*io)[i] == '\n') {
            lines.push_back(io->substr(start, i - start));
            start = i + 1;
        }
    }
    size_t last = lines.size();
    while (last > 0 && is_blank_line(lines[last - 1])) last--;
    size_t first = 0;
    while (first < last && is_blank_line(lines[first])) first++;
    if (first >= last) return 0;

    int stripped = 0;
    if (looks_like_vb_signature(lines[first])) { first++; stripped++; }
    if (last > first && is_vb_end_line(lines[last - 1])) { last--; stripped++; }
    if (!stripped) return 0;

    std::string out;
    for (size_t i = first; i < last; i++) {
        out += lines[i];
        if (i + 1 < last) out += "\n";
    }
    *io = out;
    return stripped;
}

/* 分页元数据 + 把 items 数组挂到 root。
 * 遵守 MCP Best Practices 分页约定：
 *   {count, total_count, offset, limit, has_more[, next_offset], <items_key>: [...]}
 *
 * Pagination metadata; attaches the items array to root, following the MCP Best
 * Practices pagination convention (same shape as above). */
static void apply_pagination(yyjson_mut_doc *doc, yyjson_mut_val *root,
                             const char *items_key, yyjson_mut_val *arr,
                             long long total_count, long long offset, long long limit)
{
    long long returned = arr ? (long long)yyjson_mut_arr_size(arr) : 0;
    long long next_offset = -1;
    if (returned >= limit && (offset + returned) < total_count) {
        next_offset = offset + returned;
    }
    yyjson_mut_obj_add_int(doc, root, "count", returned);
    yyjson_mut_obj_add_int(doc, root, "total_count", total_count);
    yyjson_mut_obj_add_int(doc, root, "offset", offset);
    yyjson_mut_obj_add_int(doc, root, "limit", limit);
    yyjson_mut_obj_add_bool(doc, root, "has_more", next_offset >= 0);
    if (next_offset >= 0) yyjson_mut_obj_add_int(doc, root, "next_offset", next_offset);
    if (arr) yyjson_mut_obj_add_val(doc, root, items_key, arr);
}

/* 读分页参数（limit 默认 100 / 最大 10000，offset 默认 0）。
 * Reads paging arguments (limit default 100 / max 10000, offset default 0). */
static void read_paging(yyjson_val *args, long long *limit, long long *offset)
{
    long long l = arg_int(args, "limit", 100);
    long long o = arg_int(args, "offset", 0);
    if (l < 1) l = 1;
    if (l > 10000) l = 10000;
    if (o < 0) o = 0;
    *limit = l;
    *offset = o;
}

/* 单文本字段结果：{"<key>": "<text>"[, "encoding": "..."]}。
 * Single-text-field result: {"<key>": "<text>"[, "encoding": "..."]}. */
static char *one_text_result(const char *key, const char *engine_text,
                             int enc, int with_encoding)
{
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return vbdmcp_strdup("{}");
    add_text(doc, root, key, engine_text, enc);
    if (with_encoding) add_encoding_field(doc, root, enc);
    return doc_to_string(doc);
}

/* ============================================================
 * 只读工具实现 / Read-only tools
 * ============================================================ */

/* -------- vbd_read_project_info -------- */
static char *tool_read_project_info(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));

    char *file_name = vbd_get_value(VBD_GetFileName, 0, 0);
    char *dec_path  = vbd_get_value(VBD_GetVBDecompilerPath, 0, 0);
    char *native    = vbd_get_value(VBD_IsNativeCompilation, 0, 0);
    char *compiler  = vbd_get_value(VBD_GetCompiler, 0, 0);
    char *packed    = vbd_get_value(VBD_IsPacked, 0, 0);

    add_text(doc, root, "file_name", file_name ? file_name : "", enc);
    add_text(doc, root, "vbdmcp_path", dec_path ? dec_path : "", enc);
    add_text(doc, root, "is_native", native ? native : "", enc);
    add_text(doc, root, "compiler", compiler ? compiler : "", enc);
    add_text(doc, root, "is_packed", packed ? packed : "", enc);

    vbdmcp_free(file_name);
    vbdmcp_free(dec_path);
    vbdmcp_free(native);
    vbdmcp_free(compiler);
    vbdmcp_free(packed);
    return doc_to_string(doc);
}

/* -------- vbd_read_project_text -------- */
static char *tool_read_project_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    char *text = vbd_get_value(VBD_GetVBProject, 0, 0);
    if (!text) return err_json(out_err, VBDMCP_ERR_BUSINESS, _("No project is currently open"));
    char *r = one_text_result("project_text", text, enc, 1);
    vbdmcp_free(text);
    return r;
}

/* -------- vbd_read_list_forms -------- */
static char *tool_read_list_forms(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    long long limit, offset;
    read_paging(args, &limit, &offset);

    char *count_str = vbd_get_value(VBD_GetVBFormCount, 0, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbdmcp_free(count_str);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *name = vbd_get_value(VBD_GetVBFormName, (int)i, 0);
        arr_add_text(doc, arr, name ? name : "", enc);
        if (name) vbdmcp_free(name);
    }
    apply_pagination(doc, root, "forms", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* -------- vbd_read_form -------- */
static char *tool_read_form(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "form_index", -1);
    if (idx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid form_index"));
    if ((long long)idx >= form_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("form_index %d is out of range (the project has %lld forms)", idx, form_total()));
    }

    char *content = vbd_get_value(VBD_GetVBForm, (int)idx, 0);
    if (!content) return err_json(out_err, VBDMCP_ERR_BUSINESS, _("Cannot read form content"));

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) { vbdmcp_free(content); return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory")); }

    char *name = vbd_get_value(VBD_GetVBFormName, (int)idx, 0);
    yyjson_mut_obj_add_int(doc, root, "form_index", idx);
    add_text(doc, root, "form_name", name ? name : "", enc);
    add_text(doc, root, "form_text", content, enc);
    add_encoding_field(doc, root, enc);
    vbdmcp_free(name);
    vbdmcp_free(content);
    return doc_to_string(doc);
}

/* -------- vbd_read_list_modules --------
 * SDK 26 中 value=36 语义冲突（.h=SetModuleCount / .inc=GetModuleCount），
 * 官方 demo 均不使用。改用 GetModuleName(0..N) 探测到 NULL/空串确定总数。
 * 探测结果加进程内缓存（见 get_module_names），避免每次 list 都全量调用引擎。
 *
 * In SDK 26 the value 36 has conflicting semantics (.h says SetModuleCount,
 * .inc says GetModuleCount) and the official demos never use it. The total is
 * therefore probed with GetModuleName(0..N) until NULL/empty. The probe result is
 * cached in-process (see get_module_names) so listing does not hammer the engine. */
#define VBDMCP_MAX_MODULES 10000

struct ModuleNameCache {
    std::string              key;      /* 缓存键：项目文件名 + 写入世代 / cache key: project file name + write generation */
    std::vector<std::string> names;
    bool                     valid = false;
};

static ModuleNameCache g_module_cache;

/* 每次写工具执行后自增：使模块名缓存失效（改名/写工程都可能改变列表）
 * Bumped after every write tool so the module-name cache is invalidated
 * (renaming or writing the project may change the list). */
static unsigned g_write_generation = 0;

/* VB 标识符规则：字母或下划线开头，仅含字母 / 数字 / 下划线。
 * 用于过滤 GetModuleName 遍历到列表末尾时返回的 GUI 装饰项 ——
 * 实测在列表尾部会返回 "End of forms :)"（含空格/冒号/括号），
 * 若不过滤会被当成一个模块（28 → 实际 27）。
 *
 * VB identifier rule: starts with a letter or underscore, then letters / digits /
 * underscores. Filters the GUI decoration returned by GetModuleName at the end of
 * the list - measured: "End of forms :)" (spaces/colon/parenthesis), which would
 * otherwise count as a module (28 vs. the real 27). */
static int is_vbd_identifier(const char *s)
{
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char *p = s + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    }
    return 1;
}

/* 探测并缓存模块名列表。返回的指针在下次失效前有效。
 * Probes and caches the module-name list; the returned pointer is valid until the
 * cache is invalidated. */
static const std::vector<std::string> *get_module_names(void)
{
    char *fn = vbd_get_value(VBD_GetFileName, 0, 0);
    std::string key = fn ? fn : "";
    if (fn) vbdmcp_free(fn);
    key += '#';
    key += std::to_string(g_write_generation);

    if (!g_module_cache.valid || g_module_cache.key != key) {
        g_module_cache.names.clear();
        for (int i = 0; i < VBDMCP_MAX_MODULES; i++) {
            char *name = vbd_get_value(VBD_GetModuleName, i, 0);
            if (!name || !*name) { if (name) vbdmcp_free(name); break; }
            /* 列表末尾是 GUI 装饰项（如 "End of forms :)"）：它标志列表结束，
             * 遇到即停止探测。这样既过滤掉装饰项，又**不再继续访问真正越界的
             * 索引** —— 引擎对越界索引的访问可能有宿主副作用（实测
             * GetVBForm(999999) 会让当前视图切到空位置）。
             *
             * The trailing GUI decoration (e.g. "End of forms :)") marks the end of
             * the list and stops the probe. This filters it out AND avoids touching
             * truly out-of-range indices: such engine calls may have host side
             * effects (measured: GetVBForm(999999) moves the current view to an
             * empty location). */
            if (!is_vbd_identifier(name)) { vbdmcp_free(name); break; }
            g_module_cache.names.emplace_back(name);
            vbdmcp_free(name);
        }
        g_module_cache.key = key;
        g_module_cache.valid = true;
    }
    return &g_module_cache.names;
}

/* 模块总数（复用上面的探测缓存；见文件上方的前置声明）
 * Module count (reuses the probe cache above; see the forward declaration). */
static long long module_total(void)
{
    return (long long)get_module_names()->size();
}

static char *tool_read_list_modules(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    long long limit, offset;
    read_paging(args, &limit, &offset);

    const std::vector<std::string> *names = get_module_names();
    const long long total = (long long)names->size();

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        arr_add_text(doc, arr, (*names)[(size_t)i].c_str(), enc);
    }
    apply_pagination(doc, root, "modules", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* -------- vbd_read_module -------- */
static char *tool_read_module(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    if (idx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    char *content = vbd_get_value(VBD_GetModule, (int)idx, 0);
    char *name    = vbd_get_value(VBD_GetModuleName, (int)idx, 0);
    char *str_ref = vbd_get_value(VBD_GetModuleStringReferences, (int)idx, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        vbdmcp_free(content); vbdmcp_free(name); vbdmcp_free(str_ref);
        return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);
    add_text(doc, root, "module_name", name ? name : "", enc);
    add_text(doc, root, "declarations", content ? content : "", enc);
    add_text(doc, root, "string_references", str_ref ? str_ref : "", enc);
    add_encoding_field(doc, root, enc);

    vbdmcp_free(content);
    vbdmcp_free(name);
    vbdmcp_free(str_ref);
    return doc_to_string(doc);
}

/* -------- vbd_read_list_module_functions -------- */
static char *tool_read_list_module_functions(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    if (idx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    long long limit, offset;
    read_paging(args, &limit, &offset);

    char *count_str = vbd_get_value(VBD_GetModuleFunctionCount, (int)idx, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbdmcp_free(count_str);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *name = vbd_get_value(VBD_GetModuleFunctionName, (int)idx, (int)i);
        char *addr = vbd_get_value(VBD_GetModuleFunctionAddress, (int)idx, (int)i);
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, arr);
        if (item) {
            add_text(doc, item, "name", name ? name : "", enc);
            add_text(doc, item, "address", addr ? addr : "", enc);
            yyjson_mut_obj_add_int(doc, item, "index", i);
        }
        if (name) vbdmcp_free(name);
        if (addr) vbdmcp_free(addr);
    }
    apply_pagination(doc, root, "functions", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* 读取一个函数的全部视图（name / address / code / code_fast / str_ref / disasm）。
 * 抽成函数是为了让"恢复宿主定位"能按**完全相同**的调用序列再执行一遍：
 * 宿主定位的切换是由这组视图调用里的某一个触发的，只读单个视图不足以切回
 * （实测：恢复时只读 code 视图无效 —— 读完后宿主 active_coords 仍停在目标函数）。
 * 因此恢复必须复用同一序列，且顺序保持一致。
 *
 * Reads every view of one function (name / address / code / code_fast / str_ref /
 * disasm). Extracted so "restore the host position" can replay the EXACT same
 * call sequence: the host position is moved by one of these view calls and a
 * single view is not enough to move it back (measured: reading only the code view
 * leaves active_coords on the target function). The restore path must therefore
 * reuse this sequence in the same order. */
static void read_all_function_views(int midx, int fidx,
                                    char **name, char **addr, char **code,
                                    char **code_fast, char **str_ref, char **disasm)
{
    *name      = vbd_get_value(VBD_GetModuleFunctionName, midx, fidx);
    *addr      = vbd_get_value(VBD_GetModuleFunctionAddress, midx, fidx);
    *code      = vbd_get_value(VBD_GetModuleFunction, midx, fidx);
    *code_fast = vbd_get_value(VBD_GetModuleFunctionCode, midx, fidx);
    *str_ref   = vbd_get_value(VBD_GetModuleFunctionStrRef, midx, fidx);
    *disasm    = vbd_get_value(VBD_GetModuleFunctionDisasm, midx, fidx);
}

/* -------- vbd_read_function -------- */
static char *tool_read_function(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int midx = arg_index(args, "module_index", -1);
    int fidx = arg_index(args, "function_index", -1);
    if (midx < 0 || fidx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index/function_index"));
    if ((long long)midx >= module_total() ||
        (long long)fidx >= module_function_total(midx)) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("function_index %d is out of range (module %d has %lld functions)", fidx, midx,
               module_function_total(midx)));
    }

    /* 记录读取前的宿主"当前定位"。引擎为生成 code / code_fast / disassembly
     * 等视图，会在内部把宿主的当前定位切到被读取的函数；若不恢复，用户的
     * 界面会被切走 —— native 项目下目标位置的内容不会随之重新加载，表现就是
     * "左侧面板被清空"（实测：读 {26,7} 后 coords 由 2,8 变为 26,7 且文本为空）。
     * 恢复办法：读完再读一次原位置的函数视图，读取本身会把定位切回去。
     *
     * Records the host's "current position" before reading. To build the code /
     * code_fast / disassembly views the engine internally moves the host position
     * to the function being read; without a restore the user's view is hijacked -
     * on native projects the target location is not reloaded, which looks like
     * "the left panel went blank" (measured: reading {26,7} moved coords from 2,8
     * to 26,7 with empty text). Remedy: after reading, read the original position's
     * function views again - reading itself moves the position back. */
    char *prev_coords = vbd_get_value(VBD_GetActiveModuleCoordinats, 0, 0);

    char *name = NULL, *addr = NULL, *code = NULL;
    char *code_fast = NULL, *str_ref = NULL, *disasm = NULL;
    read_all_function_views(midx, fidx, &name, &addr, &code, &code_fast, &str_ref, &disasm);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        vbdmcp_free(name); vbdmcp_free(addr); vbdmcp_free(code);
        vbdmcp_free(code_fast); vbdmcp_free(str_ref); vbdmcp_free(disasm);
        return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_int(doc, root, "module_index", midx);
    yyjson_mut_obj_add_int(doc, root, "function_index", fidx);
    add_text(doc, root, "name", name ? name : "", enc);
    add_text(doc, root, "address", addr ? addr : "", enc);
    add_text(doc, root, "code", code ? code : "", enc);
    add_text(doc, root, "code_fast", code_fast ? code_fast : "", enc);
    add_text(doc, root, "string_references", str_ref ? str_ref : "", enc);
    add_text(doc, root, "disassembly", disasm ? disasm : "", enc);
    /* 已标记时带出 mark（vbdmcp 自有数据，本身是 UTF-8，不经编码桥）
     * Includes "mark" when set (vbdmcp's own data, UTF-8 already, no encoding bridge) */
    {
        const char *mk = marks_get(midx, fidx);
        if (mk && *mk) yyjson_mut_obj_add_strcpy(doc, root, "mark", mk);
    }
    add_encoding_field(doc, root, enc);

    vbdmcp_free(name);
    vbdmcp_free(addr);
    vbdmcp_free(code);
    vbdmcp_free(code_fast);
    vbdmcp_free(str_ref);
    vbdmcp_free(disasm);

    /* 把宿主定位恢复到读取之前的位置：按**同样的视图序列**读一遍原位置，
     * 确保触发与上面相同的内部定位逻辑（原位置即目标位置时无需动作）。
     *
     * Restores the host position by replaying the same view sequence on the
     * original location, ensuring the same internal positioning logic runs
     * (nothing to do when the original position is the target). */
    if (prev_coords && prev_coords[0]) {
        int pm = -1, pf = -1;
        if (sscanf(prev_coords, "%d , %d", &pm, &pf) == 2 && pm >= 0 && pf >= 0 &&
            !(pm == midx && pf == fidx)) {
            char *rn = NULL, *ra = NULL, *rc = NULL;
            char *rcf = NULL, *rsr = NULL, *rd = NULL;
            read_all_function_views(pm, pf, &rn, &ra, &rc, &rcf, &rsr, &rd);
            vbdmcp_free(rn);  vbdmcp_free(ra); vbdmcp_free(rc);
            vbdmcp_free(rcf); vbdmcp_free(rsr); vbdmcp_free(rd);
            vbdmcp_logf(VBDMCP_LOG_DEBUG,
                        "read_function: host position restored to %s (target was %d,%d)",
                        prev_coords, midx, fidx);
        }
    }
    vbdmcp_free(prev_coords);

    return doc_to_string(doc);
}

/* -------- vbd_find_function --------
 * 按"名字片段"或"地址"查找函数，覆盖 VB Decompiler 界面上的各种显示形式：
 *   - "Proc_4_51_89CB90"（名字_地址后缀）→ 剥掉后缀后与 name 匹配；
 *   - "Command5"（名字子串，忽略大小写）；
 *   - "0x89CB90" / "89CB90" / "9030544"（地址：十六进制与十进制各试一次 ——
 *     两者在字面上无法区分，"89FB20" 是十六进制、"9042720" 是十进制）。
 * 可选 module_index 限定单模块；limit/offset 分页（默认 50，上限 1000）。
 * 结果里的 module_index + function_index 可直接传给 vbd_read_function。
 *
 * Finds functions by name fragment or address, covering the display forms used by
 * the VB Decompiler UI:
 *   - "Proc_4_51_89CB90" (name + address suffix) -> strip the suffix, match name;
 *   - "Command5" (case-insensitive name substring);
 *   - "0x89CB90" / "89CB90" / "9030544" (address: try hex and decimal, since the
 *     two are indistinguishable as text).
 * The optional module_index limits the search to one module; limit/offset paging
 * (default 50, max 1000). The returned module_index + function_index feed
 * vbd_read_function directly. */
static char *tool_find_function(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    const char *query = arg_str(args, "query");
    if (!query || !*query) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid query"));

    long long limit = arg_int(args, "limit", 50);
    long long offset = arg_int(args, "offset", 0);
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;
    if (offset < 0) offset = 0;
    const int only_module = (int)arg_int(args, "module_index", -1);

    /* 名字查询串：剥掉结尾的 "_<hex>"（VBD 显示名带地址后缀，引擎的 name 不带）
     * Name query string: strip a trailing "_<hex>" (the UI shows an address suffix, the engine name has none). */
    std::string q(query);
    std::string name_q = q;
    {
        const size_t us = q.find_last_of('_');
        if (us != std::string::npos && us + 1 < q.size()) {
            const std::string suf = q.substr(us + 1);
            bool hexish = (suf.size() >= 4 && suf.size() <= 16);
            for (size_t i = 0; hexish && i < suf.size(); i++) {
                if (!isxdigit((unsigned char)suf[i])) hexish = false;
            }
            if (hexish) name_q = q.substr(0, us);
        }
    }
    std::string q_lower, name_q_lower;
    for (size_t i = 0; i < q.size(); i++)      q_lower      += (char)tolower((unsigned char)q[i]);
    for (size_t i = 0; i < name_q.size(); i++) name_q_lower += (char)tolower((unsigned char)name_q[i]);

    const long long q_hex = parse_uint_base(query, 16);
    const long long q_dec = parse_uint_base(query, 10);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    /* 扫描上限：超大项目（函数上万）时仍保证秒级返回
     * Scan cap: keeps the answer fast even on huge projects (tens of thousands of functions). */
    const long long kMaxScan = 20000;
    long long scanned = 0, total = 0, emitted = 0;
    bool truncated = false;

    const std::vector<std::string> *mods = get_module_names();
    for (int mi = 0; mi < (int)mods->size() && !truncated; mi++) {
        if (only_module >= 0 && mi != only_module) continue;
        const long long fn_total = module_function_total(mi);
        for (long long fi = 0; fi < fn_total; fi++) {
            if (++scanned > kMaxScan) { truncated = true; break; }

            char *nm = vbd_get_value(VBD_GetModuleFunctionName, mi, (int)fi);
            char *ad = vbd_get_value(VBD_GetModuleFunctionAddress, mi, (int)fi);
            const char *name = nm ? nm : "";
            const long long addr = parse_uint_base(ad ? ad : "", 10);   /* 引擎返回十进制 / the engine returns decimal */

            bool hit = false;
            if (addr >= 0 && (addr == q_hex || addr == q_dec)) hit = true;
            if (!hit) {
                std::string nlower;
                for (const char *p = name; *p; p++) nlower += (char)tolower((unsigned char)*p);
                if (nlower.find(q_lower) != std::string::npos) hit = true;
                else if (!name_q_lower.empty() && nlower.find(name_q_lower) != std::string::npos) hit = true;
            }

            if (hit) {
                total++;
                if (total > offset && emitted < limit) {
                    yyjson_mut_val *o = yyjson_mut_obj(doc);
                    yyjson_mut_obj_add_int(doc, o, "module_index", mi);
                    add_text(doc, o, "module_name", (*mods)[mi].c_str(), enc);
                    yyjson_mut_obj_add_int(doc, o, "function_index", fi);
                    add_text(doc, o, "name", name, enc);
                    yyjson_mut_obj_add_int(doc, o, "address", addr >= 0 ? addr : 0);
                    if (addr >= 0) {
                        char hex[32];
                        _snprintf_s(hex, sizeof(hex), _TRUNCATE, "0x%llX", (unsigned long long)addr);
                        yyjson_mut_obj_add_strcpy(doc, o, "address_hex", hex);
                    }
                    {
                        const char *mk = marks_get(mi, (int)fi);
                        if (mk && *mk) yyjson_mut_obj_add_strcpy(doc, o, "mark", mk);
                    }
                    yyjson_mut_arr_add_val(arr, o);
                    emitted++;
                }
            }
            if (nm) vbdmcp_free(nm);
            if (ad) vbdmcp_free(ad);
        }
    }

    apply_pagination(doc, root, "matches", arr, total, offset, limit);
    if (truncated) yyjson_mut_obj_add_bool(doc, root, "truncated", 1);
    return doc_to_string(doc);
}

/* -------- vbd_read_marks --------
 * 列出当前项目已标记的函数（vbdmcp 自有数据，见 marks.h）。
 * mark / name 本身是 UTF-8，**不经编码桥**（add_text 是给引擎 ANSI 数据用的）。
 *
 * Lists the functions marked in this project (vbdmcp's own data, see marks.h).
 * mark / name are UTF-8 already and bypass the encoding bridge (add_text is for
 * engine ANSI data). */
struct MarkCollect {
    yyjson_mut_doc *doc;
    yyjson_mut_val *arr;
    long long limit;
    long long offset;
    long long seen;
    long long emitted;
};

/* marks_enum 回调：跳过前 offset 条，收集 limit 条。
 * 始终返回 1（继续枚举）—— 为了让 marks_enum 返回准确的总数。
 *
 * marks_enum callback: skips the first offset entries, collects limit entries.
 * Always returns 1 (keep enumerating) so marks_enum can return the exact total. */
static int marks_collect_cb(int m, int f, const char *mark, const char *name,
                            const char *addr, void *user)
{
    MarkCollect *c = (MarkCollect *)user;
    c->seen++;
    if (c->seen > c->offset && c->emitted < c->limit) {
        yyjson_mut_val *o = yyjson_mut_obj(c->doc);
        yyjson_mut_obj_add_int(c->doc, o, "module_index", m);
        yyjson_mut_obj_add_int(c->doc, o, "function_index", f);
        if (mark && *mark) yyjson_mut_obj_add_strcpy(c->doc, o, "mark", mark);
        if (name && *name) yyjson_mut_obj_add_strcpy(c->doc, o, "name", name);
        if (addr && *addr) yyjson_mut_obj_add_strcpy(c->doc, o, "address", addr);
        yyjson_mut_arr_add_val(c->arr, o);
        c->emitted++;
    }
    return 1;
}

static char *tool_read_marks(yyjson_val *args, int *out_err)
{
    (void)arg_encoding(args);   /* 标记数据本身就是 UTF-8，无编码选项影响
                                   marks are UTF-8 already; the encoding option has no effect */
    long long limit = arg_int(args, "limit", 50);
    long long offset = arg_int(args, "offset", 0);
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;
    if (offset < 0) offset = 0;

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_val *arr = yyjson_mut_arr(doc);

    MarkCollect collect;
    collect.doc = doc;
    collect.arr = arr;
    collect.limit = limit;
    collect.offset = offset;
    collect.seen = 0;
    collect.emitted = 0;

    const long long total = marks_enum(marks_collect_cb, &collect);

    apply_pagination(doc, root, "marks", arr, total, offset, limit);

    const char *file = marks_file_path();
    char *file_u8 = vbdmcp_acp_to_utf8(file ? file : "");
    if (file_u8) {
        yyjson_mut_obj_add_strcpy(doc, root, "marks_file", file_u8);
        vbdmcp_free(file_u8);
    }
    return doc_to_string(doc);
}

/* -------- vbd_write_mark_function --------
 * 设置 / 清除函数标记，三种粒度（**不写入工程** —— vbdmcp 自有数据，
 * native 项目同样可用，与 vbd_write_rename_function 的限制不同）：
 *   1) function_index        → 单个函数；
 *   2) function_indices[]    → 同一模块内一次多个；
 *   3) 两者都不给（仅 module_index）→ **该模块的全部函数**。
 * mark 传空串 = 清除。批量路径改完内存后只写盘一次（marks_set_many）。
 *
 * Sets / clears function marks at three granularities (NOT written into the
 * project - vbdmcp's own data, so native projects work too, unlike
 * vbd_write_rename_function): single function / several in one module via
 * function_indices[] / every function of the module when both are omitted.
 * An empty mark clears; the batch path writes the file once (marks_set_many). */
static char *tool_write_mark_function(yyjson_val *args, int *out_err)
{
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    const int midx = arg_index(args, "module_index", -1);
    if (midx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if ((long long)midx >= module_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", midx, module_total()));
    }
    const long long fn_total = module_function_total(midx);
    const char *mark = arg_str(args, "mark");      /* 缺省 / 空串 = 清除 / absent / empty = clear */

    /* ---- 目标函数集合（三种粒度）/ target function set (three granularities) ---- */
    std::vector<int> targets;
    yyjson_val *indices = yyjson_obj_get(args, "function_indices");
    if (indices && yyjson_is_arr(indices)) {
        size_t i, n;
        yyjson_val *v;
        yyjson_arr_foreach(indices, i, n, v) {
            if (!yyjson_is_int(v)) {
                return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
                    _("function_indices must be an array of non-negative integers"));
            }
            const long long x = yyjson_get_sint(v);
            if (x < 0 || x > kMaxEngineIndex) {
                return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
                    _("function_indices must be an array of non-negative integers"));
            }
            targets.push_back((int)x);
        }
        if (targets.empty()) {
            return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("function_indices must not be empty"));
        }
    } else {
        const int fidx = arg_index(args, "function_index", -1);
        if (fidx >= 0) {
            targets.push_back(fidx);
        } else {
            /* 省略索引 = 整个模块 / both indices omitted = the whole module */
            for (long long i = 0; i < fn_total; i++) targets.push_back((int)i);
            if (targets.empty()) {
                return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
                    _F("module %d has no functions", midx));
            }
        }
    }

    /* ---- 越界检查 + 收集附加信息（name/address，引擎 ANSI → UTF-8） ----
     * range check + gather extras (name/address, engine ANSI -> UTF-8) */
    std::vector<marks_item>   items;
    std::vector<std::string>  names_u8(targets.size());
    std::vector<std::string>  hexes(targets.size());
    items.reserve(targets.size());
    for (size_t i = 0; i < targets.size(); i++) {
        if ((long long)targets[i] >= fn_total) {
            return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
                _F("function_index %d is out of range (module %d has %lld functions)",
                   targets[i], midx, fn_total));
        }
        char *nm = vbd_get_value(VBD_GetModuleFunctionName, midx, targets[i]);
        char *ad = vbd_get_value(VBD_GetModuleFunctionAddress, midx, targets[i]);
        char *nm_u8 = vbdmcp_acp_to_utf8(nm ? nm : "");
        if (nm_u8) { names_u8[i] = nm_u8; vbdmcp_free(nm_u8); }
        if (ad) {
            const long long a = parse_uint_base(ad, 10);
            if (a >= 0) {
                char hex[32];
                _snprintf_s(hex, sizeof(hex), _TRUNCATE, "0x%llX", (unsigned long long)a);
                hexes[i] = hex;
            }
        }
        if (nm) vbdmcp_free(nm);
        if (ad) vbdmcp_free(ad);

        marks_item it;
        it.module_index   = midx;
        it.function_index = targets[i];
        it.mark           = mark ? mark : "";
        it.name           = names_u8[i].c_str();
        it.address        = hexes[i].empty() ? NULL : hexes[i].c_str();
        items.push_back(it);
    }

    if (!marks_set_many(&items[0], (int)items.size())) {
        return err_json(out_err, VBDMCP_ERR_BUSINESS,
            _("Cannot write the marks file; check that the program's folder (or %TEMP%) is writable"));
    }

    /* ---- 响应：单个保留 function_index（向后兼容）；批量给 count + function_indices ----
     * response: a single target keeps function_index (backward compatible); a batch returns count + function_indices */
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", midx);
    yyjson_mut_obj_add_int(doc, root, "count", (long long)targets.size());
    if (targets.size() == 1) {
        yyjson_mut_obj_add_int(doc, root, "function_index", targets[0]);
    } else {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        for (size_t i = 0; i < targets.size(); i++) yyjson_mut_arr_add_int(doc, arr, targets[i]);
        yyjson_mut_obj_add_val(doc, root, "function_indices", arr);
    }
    yyjson_mut_obj_add_strcpy(doc, root, "mark", mark ? mark : "");
    const char *file = marks_file_path();
    char *file_u8 = vbdmcp_acp_to_utf8(file ? file : "");
    if (file_u8) {
        yyjson_mut_obj_add_strcpy(doc, root, "marks_file", file_u8);
        vbdmcp_free(file_u8);
    }
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vbd_read_sub_main -------- */
static char *tool_read_sub_main(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *code = vbd_get_value(VBD_GetSubMain, 0, 0);
    char *r = one_text_result("code", code ? code : "", enc, 1);
    if (code) vbdmcp_free(code);
    return r;
}

/* -------- vbd_read_active_text -------- */
static char *tool_read_active_text(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *text = vbd_get_value(VBD_GetActiveText, 0, 0);
    char *r = one_text_result("text", text ? text : "", enc, 1);
    if (text) vbdmcp_free(text);
    return r;
}

/* -------- vbd_read_active_disasm_text -------- */
static char *tool_read_active_disasm_text(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *text = vbd_get_value(VBD_GetActiveDisasmText, 0, 0);
    char *r = one_text_result("text", text ? text : "", enc, 1);
    if (text) vbdmcp_free(text);
    return r;
}

/* -------- vbd_read_active_coords -------- */
static char *tool_read_active_coords(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *s = vbd_get_value(VBD_GetActiveModuleCoordinats, 0, 0);
    char *r = one_text_result("coordinates", s ? s : "", enc, 0);
    if (s) vbdmcp_free(s);
    return r;
}

/* -------- vbd_read_list_frx_icons -------- */
static char *tool_read_list_frx_icons(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    long long limit, offset;
    read_paging(args, &limit, &offset);

    char *count_str = vbd_get_value(VBD_GetFrxIconCount, 0, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbdmcp_free(count_str);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *off  = vbd_get_value(VBD_GetFrxIconOffset, (int)i, 0);
        char *size = vbd_get_value(VBD_GetFrxIconSize, (int)i, 0);
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, arr);
        if (item) {
            yyjson_mut_obj_add_int(doc, item, "index", i);
            add_text(doc, item, "offset", off ? off : "", enc);
            add_text(doc, item, "size", size ? size : "", enc);
        }
        if (off) vbdmcp_free(off);
        if (size) vbdmcp_free(size);
    }
    apply_pagination(doc, root, "icons", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* ============================================================
 * 写入工具实现 / Write tools
 * ============================================================ */

/* -------- vbd_write_project_text -------- */
static char *tool_write_project_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "project_text");
    if (!text) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing required parameter: project_text"));

    size_t text_len = strlen(text);
    if (text_len > VBDMCP_MAX_PROJECT_TEXT) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("project_text exceeds the 16MB limit (actual %zu bytes); please write it in chunks",
               text_len));
    }

    char *prev = NULL;
    want_previous(args, VBD_GetVBProject, 0, 0, &prev);
    set_text_value(VBD_SetVBProject, text, enc, 0, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    yyjson_mut_obj_add_int(doc, root, "length", (long long)text_len);
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_clear_buffers -------- */
static char *tool_write_clear_buffers(yyjson_val *args, int *out_err)
{
    (void)args;
    vbd_set_value(VBD_ClearAllBuffers, "", 0, 0);
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vbd_write_form -------- */
static char *tool_write_form(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "form_index", -1);
    const char *text = arg_str(args, "form_text");
    if (idx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid form_index"));
    if (!text) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing form_text"));
    if ((long long)idx >= form_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("form_index %d is out of range (the project has %lld forms)", idx, form_total()));
    }

    char *prev = NULL;
    want_previous(args, VBD_GetVBForm, (int)idx, 0, &prev);
    set_text_value(VBD_SetVBForm, text, enc, (int)idx, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "form_index", idx);
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(text));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_rename_form -------- */
static char *tool_write_rename_form(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "form_index", -1);
    const char *new_name = arg_str(args, "new_name");
    if (idx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid form_index"));
    if (!new_name) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing new_name"));
    if ((long long)idx >= form_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("form_index %d is out of range (the project has %lld forms)", idx, form_total()));
    }
    if (is_native_project()) return err_json(out_err, VBDMCP_ERR_BUSINESS, _("rename_form is not supported on native-compiled projects (it would trigger a modal dialog that blocks the RPC call). Use it on a source-form VB project."));

    char *prev = NULL;
    want_previous(args, VBD_GetVBFormName, (int)idx, 0, &prev);
    set_text_value(VBD_SetVBFormName, new_name, enc, (int)idx, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "form_index", idx);
    /* 回显客户端原文（UTF-8），不回显引擎侧转换结果
     * Echo the client's own text (UTF-8), not the converted engine result */
    yyjson_mut_obj_add_strcpy(doc, root, "new_name", new_name);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_module -------- */
static char *tool_write_module(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    const char *decl = arg_str(args, "declarations");
    if (idx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if (!decl) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing declarations"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    char *prev = NULL;
    want_previous(args, VBD_GetModule, (int)idx, 0, &prev);
    set_text_value(VBD_SetModule, decl, enc, (int)idx, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(decl));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_module_string_references -------- */
static char *tool_write_module_string_references(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    const char *refs = arg_str(args, "string_references");
    if (idx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if (!refs) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing string_references"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    char *prev = NULL;
    want_previous(args, VBD_GetModuleStringReferences, (int)idx, 0, &prev);
    set_text_value(VBD_SetModuleStringReferences, refs, enc, (int)idx, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_function -------- */
static char *tool_write_function(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int midx = arg_index(args, "module_index", -1);
    int fidx = arg_index(args, "function_index", -1);
    const char *code = arg_str(args, "code");
    if (midx < 0 || fidx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index/function_index"));
    if (!code) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing code"));
    if ((long long)midx >= module_total() ||
        (long long)fidx >= module_function_total(midx)) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("function_index %d is out of range (module %d has %lld functions)", fidx, midx,
               module_function_total(midx)));
    }

    /* include_previous="1" → 回传写入前的 code，便于出错回滚
     * include_previous="1" returns the previous code for rollback */
    const int want_prev = arg_onoff(args, "include_previous", 0);
    char *prev_code = want_prev ? vbd_get_value(VBD_GetModuleFunction, (int)midx, (int)fidx) : NULL;

    /* 幂等规范化：引擎期望**函数体**（它会自己补上签名与 End Sub），
     * 这里剥离调用方可能带上的首行签名 / 末尾 End * 行，使"写完整函数"
     * 与"写函数体"两种用法都安全（否则会出现重复签名 + 重复 End Sub）。
     *
     * Idempotent normalization: the engine expects the body (it adds the wrapper
     * itself), so a leading signature / trailing End * line supplied by the caller
     * is stripped; both "write the full function" and "write the body" stay safe
     * (otherwise the signature and End Sub would be duplicated). */
    std::string body(code);
    const int stripped = strip_function_wrapper(&body);

    set_text_value(VBD_SetModuleFunction, body.c_str(), enc, (int)midx, (int)fidx);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        if (prev_code) vbdmcp_free(prev_code);
        return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_int(doc, root, "module_index", midx);
    yyjson_mut_obj_add_int(doc, root, "function_index", fidx);
    yyjson_mut_obj_add_int(doc, root, "length", (long long)body.size());
    if (stripped) yyjson_mut_obj_add_int(doc, root, "stripped_lines", stripped);
    if (want_prev) add_text(doc, root, "previous", prev_code ? prev_code : "", enc);
    if (prev_code) vbdmcp_free(prev_code);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vbd_write_rename_function -------- */
static char *tool_write_rename_function(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int midx = arg_index(args, "module_index", -1);
    int fidx = arg_index(args, "function_index", -1);
    const char *new_name = arg_str(args, "new_name");
    if (midx < 0 || fidx < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index/function_index"));
    if (!new_name) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing new_name"));
    if ((long long)midx >= module_total() ||
        (long long)fidx >= module_function_total(midx)) {
        return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS,
            _F("function_index %d is out of range (module %d has %lld functions)", fidx, midx,
               module_function_total(midx)));
    }
    if (is_native_project()) return err_json(out_err, VBDMCP_ERR_BUSINESS, _("rename_function is not supported on native-compiled projects (it would trigger a modal dialog that blocks the RPC call). Use it on a source-form VB project."));

    set_text_value(VBD_SetModuleFunctionName, new_name, enc, (int)midx, (int)fidx);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", midx);
    yyjson_mut_obj_add_int(doc, root, "function_index", fidx);
    /* 回显客户端原文（UTF-8），不回显引擎侧转换结果
     * Echo the client's own text (UTF-8), not the converted engine result */
    yyjson_mut_obj_add_strcpy(doc, root, "new_name", new_name);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vbd_write_sub_main -------- */
static char *tool_write_sub_main(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *code = arg_str(args, "code");
    if (!code) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing code"));

    char *prev = NULL;
    want_previous(args, VBD_GetSubMain, 0, 0, &prev);
    set_text_value(VBD_SetSubMain, code, enc, 0, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        if (prev) vbdmcp_free(prev);
        return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(code));
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_active_text -------- */
static char *tool_write_active_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "text");
    if (!text) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing text"));
    char *prev = NULL;
    want_previous(args, VBD_GetActiveText, 0, 0, &prev);
    set_text_value(VBD_SetActiveText, text, enc, 0, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        if (prev) vbdmcp_free(prev);
        return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(text));
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_active_disasm_text -------- */
static char *tool_write_active_disasm_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "text");
    if (!text) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing text"));
    char *prev = NULL;
    want_previous(args, VBD_GetActiveDisasmText, 0, 0, &prev);
    set_text_value(VBD_SetActiveDisasmText, text, enc, 0, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        if (prev) vbdmcp_free(prev);
        return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    emit_previous(doc, root, prev, enc);
    return doc_to_string(doc);
}

/* -------- vbd_write_goto_line -------- */
static char *tool_write_goto_line(yyjson_val *args, int *out_err)
{
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int line = arg_index(args, "line", -1);
    if (line < 0) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing or invalid line"));

    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", line);
    vbd_set_value(VBD_SetActiveTextLine, buf, 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "line", line);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vbd_write_status -------- */
static char *tool_write_status(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "text");
    if (!text) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing text"));
    set_text_value(VBD_SetStatusBarText, text, enc, 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vbd_write_options -------- */
static char *tool_write_options(yyjson_val *args, int *out_err)
{
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBDMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    const char *stack    = arg_str(args, "stack_check");
    const char *analyzer = arg_str(args, "analyzer_check");
    if (stack)    vbd_set_value(VBD_SetStackCheckBoxValue, stack, 0, 0);
    if (analyzer) vbd_set_value(VBD_SetAnalyzerCheckBoxValue, analyzer, 0, 0);
    vbd_set_value(VBD_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    if (stack)    yyjson_mut_obj_add_strcpy(doc, root, "stack_check", stack);
    if (analyzer) yyjson_mut_obj_add_strcpy(doc, root, "analyzer_check", analyzer);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vbd_write_update_all -------- */
static char *tool_write_update_all(yyjson_val *args, int *out_err)
{
    (void)args;
    vbd_set_value(VBD_UpdateAll, "", 0, 0);
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBDMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* ============================================================
 * 工具表 + 分发 / Tool table and dispatch
 * ============================================================ */

typedef char *(*tool_impl_fn)(yyjson_val *args, int *out_err);

typedef struct {
    const char *name;
    const char *desc;       /* msgid */
    unsigned    params;     /* P_* 位掩码 / P_* bitmask */
    int         is_write;   /* 1 = vbd_write_*（需"允许写入"）/ needs write access */
    int         destructive;
    int         idempotent;
    tool_impl_fn impl;
} tool_entry_t;

/* 参数形状位掩码 / Argument shape bitmask */
#define P_PAGING        (1u << 0)
#define P_FORM_INDEX    (1u << 1)
#define P_PROJECT_TEXT  (1u << 2)
#define P_MODULE_INDEX  (1u << 3)
#define P_FUNCTION_INDEX (1u << 4)
#define P_FORM_TEXT     (1u << 5)
#define P_NEW_NAME      (1u << 6)
#define P_TEXT_ACTIVE   (1u << 7)   /* 代码编辑器文本（vbd_write_active_text）/ active code window text */
#define P_TEXT_DISASM   (1u << 18)  /* 反汇编窗口文本（vbd_write_active_disasm_text）/ active disassembly text */
#define P_TEXT_STATUS   (1u << 19)  /* 状态栏文本（vbd_write_status）/ status-bar text */
#define P_LINE          (1u << 8)
#define P_CODE          (1u << 9)
#define P_DECLARATIONS  (1u << 10)
#define P_STRING_REFS   (1u << 11)
#define P_OPTIONS       (1u << 12)
#define P_QUERY         (1u << 13)
/* 查找类工具的分页参数：默认 50 / 上限 1000（比列表工具的 100/10000 更小 ——
 * 搜索命中通常会喂给 vbd_read_function 逐条读取，一次给太多会灌爆上下文）。
 *
 * Paging for find-style tools: default 50 / max 1000 (smaller than the 100/10000
 * of list tools - matches are usually fed to vbd_read_function one by one, and too
 * many at once would flood the client context). */
#define P_FIND_PAGING   (1u << 14)
#define P_MARK          (1u << 15)   /* 函数标记文本（自由文本；空串/缺省 = 清除）/ mark text (free-form; empty/absent = clear) */
#define P_FUNCTION_INDICES (1u << 16)   /* function_indices：一次多个函数（数组）/ several functions at once (array) */
#define P_INCLUDE_PREVIOUS (1u << 17)   /* include_previous="1" → 回传写入前的原值 / return the pre-write value */

static const tool_entry_t g_tools[] = {
    /* ---- 只读 / read-only ---- */
    { "vbd_read_project_info",        _("Get metadata of the project currently open in VB Decompiler: source file path, whether it is natively compiled, compiler type (VB/.NET/Delphi/Unknown), whether it is packed, and the VB Decompiler installation path. All fields are strings as returned by the engine (flag-like fields hold \"1\"/\"0\"). No parameters; can be called without a project loaded."),           0,                                 0, 0, 1, tool_read_project_info },
    { "vbd_read_project_text",        _("Get the text of the project file (.vbp manifest: project type, references, components, startup object). This is the project descriptor, NOT the concatenated decompiled source of every form/module - to read code, list the forms/modules (vbd_read_list_forms / vbd_read_list_modules) and read them one by one. Returns {project_text, encoding}."),           0,                                 0, 0, 1, tool_read_project_text },
    { "vbd_read_list_forms",          _("List all VB Forms in the project. Supports pagination (limit/offset). Returns {count, total_count, offset, limit, has_more[, next_offset], forms:[...]}; while has_more is true, pass next_offset as the next offset."),             P_PAGING,                          0, 0, 1, tool_read_list_forms },
    { "vbd_read_form",                _("Get the decompiled property text of the specified Form (0-based form_index). Returns {form_index, form_name, form_text, encoding}."),               P_FORM_INDEX,                      0, 0, 1, tool_read_form },
    { "vbd_read_list_modules",        _("List all Modules in the project (standard .bas, class .cls, control .ctl). Supports pagination (limit/offset). Returns {count, total_count, offset, limit, has_more[, next_offset], modules:[...]}; feed the index into module_index of the other tools."),           P_PAGING,                          0, 0, 1, tool_read_list_modules },
    { "vbd_read_module",              _("Get the module's declarations header (function/constant/type declarations) and its string references. Returns {module_index, module_name, declarations, string_references, encoding}."),             P_MODULE_INDEX,                    0, 0, 1, tool_read_module },
    { "vbd_read_list_module_functions", _("List all functions in the specified module. Supports pagination. Returns {module_index, count, total_count, offset, limit, has_more[, next_offset], functions:[{index, name, address}]}; feed \"index\" as function_index to vbd_read_function."), P_MODULE_INDEX | P_PAGING,        0, 0, 1, tool_read_list_module_functions },
    { "vbd_read_function",            _("Get a function's decompiled code plus extra views. Returns {module_index, function_index, name, address, code, code_fast, disassembly, string_references[, mark], encoding}. \"code\" is the function body WITHOUT the Sub/End Sub wrapper (write it back with vbd_write_function); \"code_fast\" INCLUDES the wrapper. \"mark\" appears only when the function was marked (see vbd_write_mark_function)."),           P_MODULE_INDEX | P_FUNCTION_INDEX, 0, 0, 1, tool_read_function },
    { "vbd_find_function",            _("Find functions by name substring (case-insensitive) or by address, accepting the display form shown by VB Decompiler (e.g. \"Proc_4_51_89CB90\", \"Command5\", \"0x89CB90\", \"9030544\"). Supports pagination. Returns {count, total_count, offset, limit, has_more[, next_offset], matches:[{module_index, module_name, function_index, name, address, address_hex[, mark]}][, truncated]}; feed module_index + function_index to vbd_read_function. \"truncated\":true means the scan cap (20000 functions) was reached and the remaining functions were not searched."),           P_QUERY | P_MODULE_INDEX | P_FIND_PAGING, 0, 0, 1, tool_find_function },
    { "vbd_read_marks",               _("List the functions marked in this project. Marks are vbdmcp's own data, stored next to the analyzed program (see marks_file) and NOT written into the project, so they also work on native-compiled projects. Returns {count, total_count, offset, limit, has_more[, next_offset], marks:[{module_index, function_index, mark, name, address}], marks_file}."),           P_FIND_PAGING, 0, 0, 1, tool_read_marks },
    { "vbd_read_sub_main",            _("Get the code of the program entry point Sub Main. Returns {code, encoding}. No parameters."),           0,                                 0, 0, 1, tool_read_sub_main },
    { "vbd_read_active_text",         _("Get the text of the currently active code window (editor). Returns {text, encoding}."),        0,                                 0, 0, 1, tool_read_active_text },
    { "vbd_read_active_disasm_text",  _("Get the raw text of the active disassembly window. Returns {text, encoding}."), 0,                                 0, 0, 1, tool_read_active_disasm_text },
    { "vbd_read_active_coords",       _("Get the module/function coordinates of the currently active view exactly as the engine reports them (e.g. \"2 , 8\" = module index 2, function index 8). Returns {coordinates}; empty string when the host reports none."),      0,                                 0, 0, 1, tool_read_active_coords },
    { "vbd_read_list_frx_icons",      _("List icons/images in the Form's FRX resource. Supports pagination. Returns {count, total_count, offset, limit, has_more[, next_offset], icons:[{index, offset, size}]}."),         P_PAGING,                          0, 0, 1, tool_read_list_frx_icons },

    /* ---- 写入 / write ---- */
    { "vbd_write_project_text",       _("Completely replace the project text (.vbp manifest). Automatically calls UpdateAll to commit changes and refresh the GUI. Max 16 MB per call - split larger content into several calls (the error message reports the actual size). Pass include_previous=1 to receive the replaced text back (it can be large; use it only when you need a rollback copy)."),       P_PROJECT_TEXT | P_INCLUDE_PREVIOUS,     1, 1, 1, tool_write_project_text },
    { "vbd_write_clear_buffers",      _("Clear VB Decompiler's internal decompilation buffers. DESTRUCTIVE: this discards every existing decompilation result, so the analyzed file must be re-analyzed (re-opened) before the vbd_read_* tools return data again. Use only when implementing a custom full decompilation workflow. Can be called without a project loaded."),          0,                                 1, 1, 1, tool_write_clear_buffers },
    { "vbd_write_form",               _("Replace the property text of the specified Form (form_index + form_text required). Automatically calls UpdateAll. Pass include_previous=1 to get the previous text back for rollback."),               P_FORM_INDEX | P_FORM_TEXT | P_INCLUDE_PREVIOUS, 1, 1, 1, tool_write_form },
    { "vbd_write_rename_form",        _("Rename a Form (form_index + new_name required). Not supported on native-compiled projects (it would trigger a blocking modal dialog in the host). Automatically calls UpdateAll; include_previous=1 returns the previous name."),            P_FORM_INDEX | P_NEW_NAME | P_INCLUDE_PREVIOUS, 1, 0, 1, tool_write_rename_form },
    { "vbd_write_module",             _("Replace the module's declarations header (module_index + declarations required). Automatically calls UpdateAll; include_previous=1 returns the previous text."),             P_MODULE_INDEX | P_DECLARATIONS | P_INCLUDE_PREVIOUS, 1, 1, 1, tool_write_module },
    { "vbd_write_module_string_references", _("Replace the module's string-reference text (module_index + string_references required). Automatically calls UpdateAll; include_previous=1 returns the previous text."),                       P_MODULE_INDEX | P_STRING_REFS | P_INCLUDE_PREVIOUS, 1, 1, 1, tool_write_module_string_references },
    { "vbd_write_function",           _("Replace a function's code. Pass the function BODY only - without the \"Private Sub ...\" line and without the trailing \"End Sub\"; VB Decompiler adds that wrapper itself. A wrapper present in the input is stripped automatically and the response reports how many lines were removed (stripped_lines). To edit an existing function, read it with vbd_read_function and write back its \"code\" field (NOT \"code_fast\", which contains the wrapper - writing that back would duplicate it). Pass include_previous=1 to get the previous code back for rollback."),           P_MODULE_INDEX | P_FUNCTION_INDEX | P_CODE | P_INCLUDE_PREVIOUS, 1, 1, 1, tool_write_function },
    { "vbd_write_mark_function",      _("Set or clear a mark on one function, on several functions (function_indices), or on EVERY function of a module (omit both index arguments). Free-form text; an empty mark clears. Marks live in vbdmcp's own file next to the analyzed program (see marks_file), so this also works on native-compiled projects; vbd_read_function / vbd_find_function / vbd_read_marks report them."),           P_MODULE_INDEX | P_FUNCTION_INDEX | P_FUNCTION_INDICES | P_MARK, 1, 0, 1, tool_write_mark_function },
    { "vbd_write_rename_function",    _("Rename a function (module_index + function_index + new_name required). Not supported on native-compiled projects (it would trigger a blocking modal dialog in the host). Automatically calls UpdateAll."),        P_MODULE_INDEX | P_FUNCTION_INDEX | P_NEW_NAME, 1, 0, 1, tool_write_rename_function },
    { "vbd_write_sub_main",           _("Set the Sub Main code (program entry point). Automatically calls UpdateAll; include_previous=1 returns the previous code."),           P_CODE | P_INCLUDE_PREVIOUS,         1, 1, 1, tool_write_sub_main },
    { "vbd_write_active_text",        _("Set the text of the currently active code window. Automatically calls UpdateAll. Note: the host may normalize what you write, so read-back is not always byte-identical - keep your own copy of important content. include_previous=1 returns the previous text for rollback."),        P_TEXT_ACTIVE | P_INCLUDE_PREVIOUS,         1, 1, 1, tool_write_active_text },
    { "vbd_write_active_disasm_text", _("Set the text of the active disassembly window. Automatically calls UpdateAll; include_previous=1 returns the previous text for rollback."), P_TEXT_DISASM | P_INCLUDE_PREVIOUS,         1, 1, 1, tool_write_active_disasm_text },
    { "vbd_write_goto_line",          _("Jump to the specified 0-based line in the active code window (UI navigation only; it does not modify any text)."),              P_LINE,                            1, 0, 1, tool_write_goto_line },
    { "vbd_write_status",             _("Set the text of VB Decompiler's bottom status bar (useful to show MCP operation progress). Can be called without a project loaded."),             P_TEXT_STATUS,                            1, 0, 1, tool_write_status },
    { "vbd_write_options",            _("Control decompilation options: the \"Parse stack parameters\" (stack_check) and \"Procedure analyzer and optimizer\" (analyzer_check) checkboxes. Omitting either parameter leaves the corresponding option unchanged. Can be called without a project loaded."),            P_OPTIONS,                         1, 0, 1, tool_write_options },
    { "vbd_write_update_all",         _("Commit all pending Set modifications and refresh the GUI (tools normally call this automatically; call it manually only after modifying large amounts of data externally). Can be called without a project loaded."),             0,                                 1, 0, 1, tool_write_update_all },

    { NULL, NULL, 0, 0, 0, 0, NULL }
};

/* 项目加载检查豁免名单：探测/控制类工具，不要求项目已加载。
 * Tools exempt from the project-loaded check: probing / control tools. */
static const char *k_no_project_check[] = {
    "vbd_read_project_info",
    "vbd_write_status",
    "vbd_write_options",
    "vbd_write_update_all",
    "vbd_write_clear_buffers",
    NULL
};

static const tool_entry_t *find_tool(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; g_tools[i].name; i++) {
        if (vbd_streq(name, g_tools[i].name)) return &g_tools[i];
    }
    return NULL;
}

int mcp_tool_is_write(const char *tool_name)
{
    const tool_entry_t *e = find_tool(tool_name);
    return e ? e->is_write : 0;
}

int mcp_local_tool_count(int allow_write)
{
    int n = 0;
    for (int i = 0; g_tools[i].name; i++) {
        if (g_tools[i].is_write && !allow_write) continue;
        n++;
    }
    return n;
}

/* 分发工具调用。
 *   args          已解析的 arguments 对象（可为 NULL）
 *   allow_write   是否允许写入（只读模式下拒绝 vbd_write_*）
 *   out_error_code 返回错误分类（VBDMCP_ERR_*）；成功为 VBDMCP_ERR_OK
 * 返回工具结果 JSON 字符串（new alloc，调用方 vbdmcp_free）。
 *
 * Dispatches one tool call.
 *   args           parsed arguments object (may be NULL)
 *   allow_write    whether writes are allowed (read-only mode rejects vbd_write_*)
 *   out_error_code error class (VBDMCP_ERR_*); VBDMCP_ERR_OK on success
 * Returns the tool result JSON string (new alloc, caller frees with vbdmcp_free). */
char *mcp_dispatch_tool(const char *tool_name, yyjson_val *args,
                        int allow_write, int *out_error_code)
{
    if (out_error_code) *out_error_code = VBDMCP_ERR_OK;

    if (!tool_name || !*tool_name) {
        return err_json(out_error_code, VBDMCP_ERR_INVALID_PARAMS, _("Tool name is empty"));
    }

    const tool_entry_t *e = find_tool(tool_name);
    if (!e) {
        return err_json(out_error_code, VBDMCP_ERR_INVALID_PARAMS, _F("Unknown tool: %s", tool_name));
    }

    /* 防御兜底：只读模式下即便客户端绕过 listTools 强行调用 vbd_write_*，
     * 也在此二次校验权限，直接返回 MCP 权限错误，不执行业务。
     *
     * Defense in depth: even when a client bypasses listTools and calls
     * vbd_write_* in read-only mode, this re-check returns an MCP permission error
     * without running any business logic. */
    if (e->is_write && !allow_write) {
        return err_json(out_error_code, VBDMCP_ERR_PERMISSION, _("Write access is disabled. All vbd_write_* tools are unavailable in read-only mode. Ask the user to enable \"Allow MCP client to write project data\" in the VB Decompiler MCP settings."));
    }

    /* 集中项目加载检查 / central project-loaded check */
    int needs_check = 1;
    for (int j = 0; k_no_project_check[j]; j++) {
        if (vbd_streq(tool_name, k_no_project_check[j])) { needs_check = 0; break; }
    }
    if (needs_check && !vbd_is_project_loaded()) {
        return err_json(out_error_code, VBDMCP_ERR_BUSINESS,
            _F("Project not loaded. The VB SDK provides no ReloadFile/Refresh API, so vbdmcp cannot re-initialize the engine from the DLL side. Please open the vbdmcp settings window from the Plugins menu in the VB Decompiler main window and click OK (this triggers a VBDecompilerPluginLoad re-entry). Tool called: %s",
               tool_name));
    }

    int err = VBDMCP_ERR_OK;
    char *payload = e->impl(args, &err);
    /* 写操作可能改变模块/表单/函数列表：使模块名缓存失效
     * A write may change the module/form/function lists: invalidate the name cache */
    if (e->is_write) g_write_generation++;
    if (out_error_code) *out_error_code = err;
    return payload;
}

/* ============================================================
 * tools/list schema 构造（全部 yyjson）
 *
 * annotations (MCP 2025-03 规范):
 *   readOnlyHint / destructiveHint / idempotentHint / openWorldHint
 * vbdmcp 全部为本地工具，openWorldHint 恒为 false。
 *
 * tools/list schema construction (all yyjson).
 * annotations (MCP 2025-03 spec): readOnlyHint / destructiveHint /
 * idempotentHint / openWorldHint. Every vbdmcp tool is local, so openWorldHint
 * is always false.
 * ============================================================ */

static void param_int(yyjson_mut_doc *doc, yyjson_mut_val *props, const char *name,
                      long long min, long long max, long long def, const char *desc)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "type", "integer");
    if (min >= 0) yyjson_mut_obj_add_int(doc, o, "minimum", min);
    if (max > 0)  yyjson_mut_obj_add_int(doc, o, "maximum", max);
    if (def >= 0) yyjson_mut_obj_add_int(doc, o, "default", def);
    if (desc)     yyjson_mut_obj_add_strcpy(doc, o, "description", desc);
    yyjson_mut_obj_add_val(doc, props, name, o);
}

static void param_str(yyjson_mut_doc *doc, yyjson_mut_val *props, const char *name,
                      const char *desc)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "type", "string");
    if (desc) yyjson_mut_obj_add_strcpy(doc, o, "description", desc);
    yyjson_mut_obj_add_val(doc, props, name, o);
}

/* 非负整数数组参数（如 function_indices）
 * Array-of-non-negative-integers parameter (e.g. function_indices). */
static void param_int_array(yyjson_mut_doc *doc, yyjson_mut_val *props,
                            const char *name, const char *desc)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "type", "array");
    yyjson_mut_val *items = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, items, "type", "integer");
    yyjson_mut_obj_add_int(doc, items, "minimum", 0);
    yyjson_mut_obj_add_val(doc, o, "items", items);
    if (desc) yyjson_mut_obj_add_strcpy(doc, o, "description", desc);
    yyjson_mut_obj_add_val(doc, props, name, o);
}

/* "0"/"1" 开关参数；desc 为 NULL 时用通用说明。
 * "0"/"1" on-off parameter; a NULL desc falls back to the generic text. */
static void param_onoff(yyjson_mut_doc *doc, yyjson_mut_val *props, const char *name,
                        const char *desc)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "type", "string");
    yyjson_mut_val *en = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_strcpy(doc, en, "0");
    yyjson_mut_arr_add_strcpy(doc, en, "1");
    yyjson_mut_obj_add_val(doc, o, "enum", en);
    yyjson_mut_obj_add_strcpy(doc, o, "description",
                              desc ? desc : _("\"0\" = off, \"1\" = on"));
    yyjson_mut_obj_add_val(doc, props, name, o);
}

/* 所有工具统一接受的可选 encoding 参数（见文件头"数据编码"小节）。
 * Optional encoding parameter accepted by every tool (see "data encoding" above). */
static void param_encoding(yyjson_mut_doc *doc, yyjson_mut_val *props)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "type", "string");
    yyjson_mut_val *en = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_strcpy(doc, en, "utf-8");
    yyjson_mut_arr_add_strcpy(doc, en, "gbk");
    yyjson_mut_obj_add_val(doc, o, "enum", en);
    yyjson_mut_obj_add_strcpy(doc, o, "description",
        _("Encoding of text fields: \"utf-8\" (default) converts between the project's ANSI/GBK bytes and UTF-8; \"gbk\" returns/sends raw ANSI bytes (compatibility mode)."));
    yyjson_mut_obj_add_val(doc, props, "encoding", o);
}

static void add_params(yyjson_mut_doc *doc, yyjson_mut_val *props, unsigned flags)
{
    param_encoding(doc, props);
    if (flags & P_PAGING) {
        param_int(doc, props, "limit",  1, 10000, 100, _("Maximum number of items to return"));
        param_int(doc, props, "offset", 0, 0,      0, _("Start offset"));
    }
    if (flags & P_FORM_INDEX)
        param_int(doc, props, "form_index", 0, kMaxEngineIndex, -1,
                  _("Form index (0-based), from vbd_read_list_forms."));
    if (flags & P_PROJECT_TEXT)
        param_str(doc, props, "project_text",
                  _("Full replacement project text (required). Max 16 MB per call - split larger content into several calls."));
    if (flags & P_MODULE_INDEX)
        param_int(doc, props, "module_index", 0, kMaxEngineIndex, -1,
                  _("Module index (0-based), from vbd_read_list_modules or vbd_find_function."));
    if (flags & P_FUNCTION_INDEX)
        param_int(doc, props, "function_index", 0, kMaxEngineIndex, -1,
                  _("Function index inside the module (0-based), from vbd_read_list_module_functions or vbd_find_function."));
    if (flags & P_FORM_TEXT)
        param_str(doc, props, "form_text",
                  _("New property text of the form (complete replacement)."));
    if (flags & P_NEW_NAME)
        param_str(doc, props, "new_name",
                  _("New name (a VB identifier)."));
    if (flags & P_TEXT_ACTIVE)
        param_str(doc, props, "text",
                  _("New text of the active code window (complete replacement). The host may normalize it, so reading back is not always byte-identical."));
    if (flags & P_TEXT_DISASM)
        param_str(doc, props, "text",
                  _("New text of the active disassembly window (complete replacement)."));
    if (flags & P_TEXT_STATUS)
        param_str(doc, props, "text",
                  _("New status-bar text (short, single line)."));
    if (flags & P_LINE)
        param_int(doc, props, "line", 0, 0, -1, _("Target line number (0-based)"));
    if (flags & P_CODE)
        param_str(doc, props, "code",
                  _("Code BODY only: do not include the \"Private Sub ...\" signature line or the trailing \"End Sub\" - VB Decompiler adds that wrapper itself (a wrapper in the input is stripped and reported as stripped_lines). To edit an existing function, read it with vbd_read_function and write back its \"code\" field, not \"code_fast\"."));
    if (flags & P_DECLARATIONS)
        param_str(doc, props, "declarations",
                  _("New declarations header of the module (function/constant/type declarations; complete replacement)."));
    if (flags & P_STRING_REFS)
        param_str(doc, props, "string_references",
                  _("New string-reference text of the module (complete replacement)."));
    if (flags & P_QUERY)
        param_str(doc, props, "query",
                  _("Function name substring (case-insensitive) or an address (decimal or 0x-hex). Accepts the display form shown by VB Decompiler, e.g. \"Proc_4_51_89CB90\"."));
    if (flags & P_FIND_PAGING) {
        param_int(doc, props, "limit",  1, 1000, 50, _("Maximum number of items to return"));
        param_int(doc, props, "offset", 0, 0,     0, _("Start offset"));
    }
    if (flags & P_MARK)
        param_str(doc, props, "mark",
                  _("Free-form mark text for this function (e.g. \"analyzed: crypto entry\"). Pass an empty string to clear the mark. Marks are stored by vbdmcp next to the analyzed program (see marks_file) and are NOT written into the project."));
    if (flags & P_FUNCTION_INDICES)
        param_int_array(doc, props, "function_indices",
                        _("Mark several functions of the module in one call. Omit both function_index and function_indices to mark EVERY function of the module."));
    if (flags & P_INCLUDE_PREVIOUS)
        param_onoff(doc, props, "include_previous",
                    _("\"1\" = include the pre-write value in the response (for rollback); \"0\" (default) = omit it."));
    if (flags & P_OPTIONS) {
        param_onoff(doc, props, "stack_check",
                    _("\"0\" = off, \"1\" = on (parse stack parameters)."));
        param_onoff(doc, props, "analyzer_check",
                    _("\"0\" = off, \"1\" = on (procedure analyzer and optimizer)."));
    }
}

/* 工具的人类可读标题（MCP 2025-06-18 的 title 字段），按工具名查表。
 * N_() 只作提取标记，运行时经 _() 取当前语言译文；未登记的工具不输出 title。
 *
 * Human-readable tool titles (the "title" field of MCP 2025-06-18), looked up by
 * tool name. N_() only marks the strings for the extraction script; _() resolves
 * the translation at runtime. Tools without an entry emit no title. */
static const struct { const char *name; const char *title; } kToolTitles[] = {
    { "vbd_read_project_info",              N_("Project info") },
    { "vbd_read_project_text",              N_("Project text (.vbp)") },
    { "vbd_read_list_forms",                N_("List forms") },
    { "vbd_read_form",                      N_("Read form") },
    { "vbd_read_list_modules",              N_("List modules") },
    { "vbd_read_module",                    N_("Read module") },
    { "vbd_read_list_module_functions",     N_("List module functions") },
    { "vbd_read_function",                  N_("Read function") },
    { "vbd_find_function",                  N_("Find function") },
    { "vbd_read_marks",                     N_("List marks") },
    { "vbd_read_sub_main",                  N_("Sub Main code") },
    { "vbd_read_active_text",               N_("Active code text") },
    { "vbd_read_active_disasm_text",        N_("Active disassembly text") },
    { "vbd_read_active_coords",             N_("Active coordinates") },
    { "vbd_read_list_frx_icons",            N_("List FRX icons") },
    { "vbd_write_project_text",             N_("Write project text") },
    { "vbd_write_clear_buffers",            N_("Clear buffers") },
    { "vbd_write_form",                     N_("Write form") },
    { "vbd_write_rename_form",              N_("Rename form") },
    { "vbd_write_module",                   N_("Write module") },
    { "vbd_write_module_string_references", N_("Write string references") },
    { "vbd_write_function",                 N_("Write function") },
    { "vbd_write_mark_function",            N_("Mark function") },
    { "vbd_write_rename_function",          N_("Rename function") },
    { "vbd_write_sub_main",                 N_("Write Sub Main") },
    { "vbd_write_active_text",              N_("Write active text") },
    { "vbd_write_active_disasm_text",       N_("Write disassembly text") },
    { "vbd_write_goto_line",                N_("Go to line") },
    { "vbd_write_status",                   N_("Set status bar") },
    { "vbd_write_options",                  N_("Set decompile options") },
    { "vbd_write_update_all",               N_("Update all") },
    { NULL, NULL }
};

static const char *tool_title(const char *name)
{
    for (int i = 0; kToolTitles[i].name; i++) {
        if (vbd_streq(name, kToolTitles[i].name)) return _(kToolTitles[i].title);
    }
    return NULL;
}

/* 每个工具的可选参数集：只列出少量例外，其余在 params 位掩码里的参数一律必填。
 * Optional parameter set of a tool: only a few per-tool exceptions; every other
 * parameter in the params bitmask is required. */
static unsigned optional_params_for(const char *tool_name)
{
    unsigned opt = P_PAGING | P_FIND_PAGING | P_INCLUDE_PREVIOUS |
                   P_MARK | P_FUNCTION_INDICES | P_OPTIONS;
    if (vbd_streq(tool_name, "vbd_find_function")) {
        opt |= P_MODULE_INDEX;      /* 可选：仅用于限定单个模块 / optional: narrows the search to one module */
    } else if (vbd_streq(tool_name, "vbd_write_mark_function")) {
        opt |= P_FUNCTION_INDEX;    /* 可选：省略 = 整个模块 / optional: omitted = the whole module */
    }
    return opt;
}

/* 把必填参数名写进 inputSchema.required（顺序与 add_params 的属性顺序一致）。
 * 没有必填参数时不输出该键。
 *
 * Writes the required parameter names into inputSchema.required (same order as
 * the properties built by add_params). The key is omitted when nothing is
 * required. */
static void add_required(yyjson_mut_doc *doc, yyjson_mut_val *schema,
                         const tool_entry_t *e)
{
    static const struct { unsigned bit; const char *name; } kReq[] = {
        { P_FORM_INDEX,     "form_index" },
        { P_PROJECT_TEXT,   "project_text" },
        { P_MODULE_INDEX,   "module_index" },
        { P_FUNCTION_INDEX, "function_index" },
        { P_FORM_TEXT,      "form_text" },
        { P_NEW_NAME,       "new_name" },
        { P_TEXT_ACTIVE,    "text" },
        { P_TEXT_DISASM,    "text" },
        { P_TEXT_STATUS,    "text" },
        { P_LINE,           "line" },
        { P_CODE,           "code" },
        { P_DECLARATIONS,   "declarations" },
        { P_STRING_REFS,    "string_references" },
        { P_QUERY,          "query" },
    };
    const unsigned req = e->params & ~optional_params_for(e->name);
    if (!req) return;
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (size_t i = 0; i < sizeof(kReq) / sizeof(kReq[0]); i++) {
        if (req & kReq[i].bit) yyjson_mut_arr_add_strcpy(doc, arr, kReq[i].name);
    }
    yyjson_mut_obj_add_val(doc, schema, "required", arr);
}

static void build_one_tool(yyjson_mut_doc *doc, yyjson_mut_val *arr, const tool_entry_t *e)
{
    yyjson_mut_val *t = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, t, "name", e->name);
    /* title：MCP 2025-06-18 的人类可读名称（不认识的客户端会忽略该字段）。
     * name 是协议标识符、不翻译；title 走 i18n（见 kToolTitles）。
     *
     * title: human-readable name from MCP 2025-06-18 (clients that do not know
     * the field ignore it). name is a protocol identifier and is never
     * translated; title goes through i18n (see kToolTitles). */
    const char *title = tool_title(e->name);
    if (title) yyjson_mut_obj_add_strcpy(doc, t, "title", title);
    /* desc == NULL 时用工具名兜底（当前所有工具都有描述，保留兜底以防将来新增遗漏）。
     * 工具名是协议标识符，不翻译；描述走 i18n（MCP 报文必须是 UTF-8）。
     *
     * A NULL desc falls back to the tool name (every tool has a description today;
     * the fallback guards against future omissions). Tool names are protocol
     * identifiers and are never translated; descriptions go through i18n (MCP
     * messages must be UTF-8). */
    yyjson_mut_obj_add_strcpy(doc, t, "description",
        (e->desc && *e->desc) ? _(e->desc) : e->name);

    yyjson_mut_val *schema = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, schema, "type", "object");
    yyjson_mut_val *props = yyjson_mut_obj(doc);
    add_params(doc, props, e->params);
    yyjson_mut_obj_add_val(doc, schema, "properties", props);
    add_required(doc, schema, e);
    yyjson_mut_obj_add_bool(doc, schema, "additionalProperties", 0);
    yyjson_mut_obj_add_val(doc, t, "inputSchema", schema);

    yyjson_mut_val *ann = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, ann, "readOnlyHint", !e->is_write);
    yyjson_mut_obj_add_bool(doc, ann, "destructiveHint", e->destructive);
    yyjson_mut_obj_add_bool(doc, ann, "idempotentHint", e->idempotent);
    yyjson_mut_obj_add_bool(doc, ann, "openWorldHint", 0);
    yyjson_mut_obj_add_val(doc, t, "annotations", ann);

    yyjson_mut_arr_append(arr, t);
}

/* 把当前权限下可见的工具追加到调用方提供的数组（不创建新 doc）。
 * allow_write=0 时只追加 vbd_read_*，=1 时追加全部。
 *
 * Appends the tools visible under the current permission to a caller-provided
 * array (no new doc). allow_write=0 appends vbd_read_* only; =1 appends all. */
void mcp_build_tools_into(yyjson_mut_doc *doc, yyjson_mut_val *arr, int allow_write)
{
    if (!doc || !arr) return;
    for (int i = 0; g_tools[i].name; i++) {
        if (g_tools[i].is_write && !allow_write) continue;
        build_one_tool(doc, arr, &g_tools[i]);
    }
}
