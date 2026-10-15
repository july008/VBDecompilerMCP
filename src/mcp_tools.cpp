/*
 * mcp_tools.cpp - MCP 工具实现（yyjson 版）
 *
 * 设计原则：
 *  - 工具名严格分为两组：
 *      vb_read_*   只读操作（读取项目结构 / 源码 / 反编译信息 / 元数据）
 *      vb_write_*  修改操作（保存、修改工程、覆盖导出、删除/清空项目资源）
 *    只读模式下 listTools 只返回 vb_read_*；开启"允许写入"后才追加 vb_write_*。
 *  - 输入为 JSON 对象（arguments），输出为 JSON：**全部使用 yyjson** 构建/解析，
 *    删除全部手写 JSON 序列化代码。
 *  - 数据编码（见下方"数据编码"小节）：工具默认把引擎的 ANSI/GBK 字节转成
 *    UTF-8 输出、把客户端 UTF-8 参数转回 ANSI/GBK 写入；可选参数 encoding:"gbk"
 *    回退为旧的字节透传行为。含自由文本的结果附带 {"encoding": "..."} 字段。
 *  - 业务错误返回 {"error":"..."}（isError:true 的工具结果，占用 MCP 内容通道，
 *    不占用 JSON-RPC 错误通道）；权限/参数类错误经 out_err 由 dispatcher 分类。
 *  - 错误分类不再走全局变量：每个 impl 通过 `int *out_err` 显式回填
 *    VBMCP_ERR_*，由 mcp_dispatch_tool 上报给 rpc 层。
 *  - 所有工具实现都假定在主线程调用（避开 Callback Engine 线程安全问题）。
 */
#include "mcp_tools.h"
#include "vbmcp.h"
#include "i18n.h"
#include <ctype.h>
#include <stdio.h>
#include <string>
#include <vector>

/* ============================================================
 * 结果构造辅助（yyjson）
 * ============================================================ */

/* 序列化并释放 mutable doc。失败时回退到一个合法的空对象。 */
static char *doc_to_string(yyjson_mut_doc *doc)
{
    char *s = NULL;
    if (doc) {
        s = yyjson_mut_write(doc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
        yyjson_mut_doc_free(doc);
    }
    if (!s) s = vbmcp_strdup("{}");
    return s;
}

/* 新建 doc + 顶层 object（已 set_root）。失败返回 NULL。 */
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
 * （由 mcp_dispatch_tool 读取后上报给 rpc 层，取代旧的 g_tool_err 全局变量）。 */
static char *err_json(int *out_err, int code, const char *msg)
{
    if (out_err) *out_err = code;
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return vbmcp_strdup("{\"error\":\"out of memory\"}");
    yyjson_mut_obj_add_strcpy(doc, root, "error", msg ? msg : "");
    return doc_to_string(doc);
}

/* ============================================================
 * 数据编码（MCP JSON ↔ VB Decompiler 引擎）
 *
 * MCP 报文（JSON）必须是 UTF-8，而引擎的文本字段是 ANSI（中文 Windows = GBK）
 * 字节。每个工具都接受可选参数 encoding：
 *   "utf-8"（默认）：读侧 GBK→UTF-8、写侧 UTF-8→GBK（双向转换）；
 *   "gbk"          ：旧行为——字节原样透传（兼容按 ANSI 处理数据的客户端）。
 * 含自由文本的结果额外带 {"encoding": "..."} 字段，声明该结果的实际编码。
 * ============================================================ */
enum { ENC_UTF8 = 0, ENC_GBK = 1 };

/* 引擎文本（ANSI/GBK）→ 结果文本：返回 new alloc（vbmcp_free）。 */
static char *enc_out(const char *engine_text, int enc)
{
    if (!engine_text) return NULL;
    return (enc == ENC_GBK) ? vbmcp_strdup(engine_text)
                            : vbmcp_acp_to_utf8(engine_text);
}

/* 客户端文本（UTF-8 JSON）→ 引擎文本（ANSI/GBK）：返回 new alloc（vbmcp_free）。 */
static char *enc_in(const char *json_text, int enc)
{
    if (!json_text) return NULL;
    return (enc == ENC_GBK) ? vbmcp_strdup(json_text)
                            : vbmcp_str_to_gbk(json_text);
}

/* 把引擎文本按编码转换后写入对象的字符串字段。 */
static void add_text(yyjson_mut_doc *doc, yyjson_mut_val *obj,
                     const char *key, const char *engine_text, int enc)
{
    char *t = enc_out(engine_text, enc);
    yyjson_mut_obj_add_strcpy(doc, obj, key, t ? t : "");
    if (t) vbmcp_free(t);
}

/* 数组版本（表单名 / 模块名列表等）。 */
static void arr_add_text(yyjson_mut_doc *doc, yyjson_mut_val *arr,
                         const char *engine_text, int enc)
{
    char *t = enc_out(engine_text, enc);
    yyjson_mut_arr_add_strcpy(doc, arr, t ? t : "");
    if (t) vbmcp_free(t);
}

/* 含自由文本的结果：声明实际使用的编码（utf-8 / gbk），便于客户端判断。 */
static void add_encoding_field(yyjson_mut_doc *doc, yyjson_mut_val *root, int enc)
{
    yyjson_mut_obj_add_strcpy(doc, root, "encoding", enc == ENC_GBK ? "gbk" : "utf-8");
}

/* 写工具：把客户端文本转成引擎字节并调用 vb_set_value（Set 语义见 vbdec_sdk.cpp）。 */
static void set_text_value(int vlType, const char *json_text, int enc, int idx, int fn)
{
    char *ansi = enc_in(json_text, enc);
    vb_set_value(vlType, ansi ? ansi : (json_text ? json_text : ""), idx, fn);
    if (ansi) vbmcp_free(ansi);
}

/* ============================================================
 * 参数读取辅助（读侧 yyjson API）
 * ============================================================ */

/* 读取整型参数。兼容 integer / real / 数字字符串。缺失或非法返回 def。 */
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

/* 读取字符串参数；缺失或非字符串返回 NULL。 */
static const char *arg_str(yyjson_val *args, const char *key)
{
    yyjson_val *v = args ? yyjson_obj_get(args, key) : NULL;
    return (v && yyjson_is_str(v)) ? yyjson_get_str(v) : NULL;
}

/* 读取 encoding 参数：缺省 / "utf-8" → UTF-8 双向转换；"gbk" → 旧字节透传。 */
static int arg_encoding(yyjson_val *args)
{
    const char *e = arg_str(args, "encoding");
    if (e && _stricmp(e, "gbk") == 0) return ENC_GBK;
    return ENC_UTF8;
}

/* Native 项目守护：rename_* / UpdateAll 在 native 项目上会触发 modal
 * "Please fill forms and modules before call UpdateAll"，阻塞 RPC 调用栈。
 * 返回 1=是 native 项目（应拒绝），0=非 native（允许）。 */
static int is_native_project(void)
{
    char *s = vb_get_value(VB_IsNativeCompilation, 0, 0);
    int nat = (s && s[0] == '1') ? 1 : 0;
    if (s) vbmcp_free(s);
    return nat;
}

/* 安全地把整数字符串解析为 long long（容错）。 */
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
 * 所以所有索引一律过这个函数校验。 */
static const long long kMaxEngineIndex = 1000000;

static int arg_index(yyjson_val *args, const char *name, int def)
{
    const long long v = arg_int(args, name, def);
    if (v < 0 || v > kMaxEngineIndex) return -1;
    return (int)v;      /* 校验后再截断 */
}

/* ---------------- 索引越界预检 ----------------
 * 引擎对超界索引不报错，而是返回哨兵值（实测 form_index=999999 时
 * form_name / form_text 都是 "Error"），直接透传会被 AI 当成真实数据；
 * 越界的写操作更不应该执行。因此所有带索引的工具在进入业务逻辑前
 * 先做一次上限校验，越界直接返回参数错误。 */

/* 窗体总数（引擎直接提供，调用很便宜） */
static long long form_total(void)
{
    char *s = vb_get_value(VB_GetVBFormCount, 0, 0);
    const long long n = safe_parse_int(s, 0);
    if (s) vbmcp_free(s);
    return n;
}

/* 指定模块的函数数 */
static long long module_function_total(int midx)
{
    char *s = vb_get_value(VB_GetModuleFunctionCount, midx, 0);
    const long long n = safe_parse_int(s, 0);
    if (s) vbmcp_free(s);
    return n;
}

/* 模块总数：复用模块名探测缓存（定义见下方 get_module_names） */
static long long module_total(void);

/* 分页元数据 + 把 items 数组挂到 root。
 * 遵守 MCP Best Practices 分页约定：
 *   {count, total_count, offset, limit, has_more[, next_offset], <items_key>: [...]} */
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

/* 读分页参数（limit 默认 100 / 最大 10000，offset 默认 0）。 */
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

/* 单文本字段结果：{"<key>": "<text>"[, "encoding": "..."]}。 */
static char *one_text_result(const char *key, const char *engine_text,
                             int enc, int with_encoding)
{
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return vbmcp_strdup("{}");
    add_text(doc, root, key, engine_text, enc);
    if (with_encoding) add_encoding_field(doc, root, enc);
    return doc_to_string(doc);
}

/* ============================================================
 * 只读工具实现
 * ============================================================ */

/* -------- vb_read_project_info -------- */
static char *tool_read_project_info(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));

    char *file_name = vb_get_value(VB_GetFileName, 0, 0);
    char *dec_path  = vb_get_value(VB_GetVBDecompilerPath, 0, 0);
    char *native    = vb_get_value(VB_IsNativeCompilation, 0, 0);
    char *compiler  = vb_get_value(VB_GetCompiler, 0, 0);
    char *packed    = vb_get_value(VB_IsPacked, 0, 0);

    add_text(doc, root, "file_name", file_name ? file_name : "", enc);
    add_text(doc, root, "vbmcp_path", dec_path ? dec_path : "", enc);
    add_text(doc, root, "is_native", native ? native : "", enc);
    add_text(doc, root, "compiler", compiler ? compiler : "", enc);
    add_text(doc, root, "is_packed", packed ? packed : "", enc);

    vbmcp_free(file_name);
    vbmcp_free(dec_path);
    vbmcp_free(native);
    vbmcp_free(compiler);
    vbmcp_free(packed);
    return doc_to_string(doc);
}

/* -------- vb_read_project_text -------- */
static char *tool_read_project_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    char *text = vb_get_value(VB_GetVBProject, 0, 0);
    if (!text) return err_json(out_err, VBMCP_ERR_BUSINESS, _("No project is currently open"));
    char *r = one_text_result("project_text", text, enc, 1);
    vbmcp_free(text);
    return r;
}

/* -------- vb_read_list_forms -------- */
static char *tool_read_list_forms(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    long long limit, offset;
    read_paging(args, &limit, &offset);

    char *count_str = vb_get_value(VB_GetVBFormCount, 0, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbmcp_free(count_str);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *name = vb_get_value(VB_GetVBFormName, (int)i, 0);
        arr_add_text(doc, arr, name ? name : "", enc);
        if (name) vbmcp_free(name);
    }
    apply_pagination(doc, root, "forms", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* -------- vb_read_form -------- */
static char *tool_read_form(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "form_index", -1);
    if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid form_index"));
    if ((long long)idx >= form_total()) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("form_index %d is out of range (the project has %lld forms)", idx, form_total()));
    }

    char *content = vb_get_value(VB_GetVBForm, (int)idx, 0);
    if (!content) return err_json(out_err, VBMCP_ERR_BUSINESS, _("Cannot read form content"));

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) { vbmcp_free(content); return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory")); }

    char *name = vb_get_value(VB_GetVBFormName, (int)idx, 0);
    yyjson_mut_obj_add_int(doc, root, "form_index", idx);
    add_text(doc, root, "form_name", name ? name : "", enc);
    add_text(doc, root, "form_text", content, enc);
    add_encoding_field(doc, root, enc);
    vbmcp_free(name);
    vbmcp_free(content);
    return doc_to_string(doc);
}

/* -------- vb_read_list_modules --------
 * SDK 26 中 value=36 语义冲突（.h=SetModuleCount / .inc=GetModuleCount），
 * demo 全不使用。改用 GetModuleName(0..N) 探测到 NULL/空串确定总数。
 * 探测结果加进程内缓存（见 get_module_names），避免每次 list 都全量调用引擎。 */
#define VBMCP_MAX_MODULES 10000

struct ModuleNameCache {
    std::string              key;      /* 缓存键：项目文件名 + 写入世代 */
    std::vector<std::string> names;
    bool                     valid = false;
};

static ModuleNameCache g_module_cache;

/* 每次写工具执行后自增：使模块名缓存失效（改名/写工程都可能改变列表） */
static unsigned g_write_generation = 0;

/* VB 标识符规则：字母或下划线开头，仅含字母 / 数字 / 下划线。
 * 用于过滤 GetModuleName 遍历到列表末尾时返回的 GUI 装饰项 ——
 * 实测在列表尾部会返回 "End of forms :)"（含空格/冒号/括号），
 * 若不过滤会被当成一个模块（28 → 实际 27）。 */
static int is_vb_identifier(const char *s)
{
    if (!s || !*s) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (const char *p = s + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    }
    return 1;
}

/* 探测并缓存模块名列表。返回的指针在下次失效前有效。 */
static const std::vector<std::string> *get_module_names(void)
{
    char *fn = vb_get_value(VB_GetFileName, 0, 0);
    std::string key = fn ? fn : "";
    if (fn) vbmcp_free(fn);
    key += '#';
    key += std::to_string(g_write_generation);

    if (!g_module_cache.valid || g_module_cache.key != key) {
        g_module_cache.names.clear();
        for (int i = 0; i < VBMCP_MAX_MODULES; i++) {
            char *name = vb_get_value(VB_GetModuleName, i, 0);
            if (!name || !*name) { if (name) vbmcp_free(name); break; }
            /* 列表末尾是 GUI 装饰项（如 "End of forms :)"）：它标志列表结束，
             * 遇到即停止探测。这样既过滤掉装饰项，又**不再继续访问真正越界的
             * 索引** —— 引擎对越界索引的访问可能有宿主副作用（实测
             * GetVBForm(999999) 会让当前视图切到空位置）。 */
            if (!is_vb_identifier(name)) { vbmcp_free(name); break; }
            g_module_cache.names.emplace_back(name);
            vbmcp_free(name);
        }
        g_module_cache.key = key;
        g_module_cache.valid = true;
    }
    return &g_module_cache.names;
}

/* 模块总数（复用上面的探测缓存；见文件上方的前置声明） */
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
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        arr_add_text(doc, arr, (*names)[(size_t)i].c_str(), enc);
    }
    apply_pagination(doc, root, "modules", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* -------- vb_read_module -------- */
static char *tool_read_module(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    char *content = vb_get_value(VB_GetModule, (int)idx, 0);
    char *name    = vb_get_value(VB_GetModuleName, (int)idx, 0);
    char *str_ref = vb_get_value(VB_GetModuleStringReferences, (int)idx, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        vbmcp_free(content); vbmcp_free(name); vbmcp_free(str_ref);
        return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);
    add_text(doc, root, "module_name", name ? name : "", enc);
    add_text(doc, root, "declarations", content ? content : "", enc);
    add_text(doc, root, "string_references", str_ref ? str_ref : "", enc);
    add_encoding_field(doc, root, enc);

    vbmcp_free(content);
    vbmcp_free(name);
    vbmcp_free(str_ref);
    return doc_to_string(doc);
}

/* -------- vb_read_list_module_functions -------- */
static char *tool_read_list_module_functions(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    long long limit, offset;
    read_paging(args, &limit, &offset);

    char *count_str = vb_get_value(VB_GetModuleFunctionCount, (int)idx, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbmcp_free(count_str);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *name = vb_get_value(VB_GetModuleFunctionName, (int)idx, (int)i);
        char *addr = vb_get_value(VB_GetModuleFunctionAddress, (int)idx, (int)i);
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, arr);
        if (item) {
            add_text(doc, item, "name", name ? name : "", enc);
            add_text(doc, item, "address", addr ? addr : "", enc);
            yyjson_mut_obj_add_int(doc, item, "index", i);
        }
        if (name) vbmcp_free(name);
        if (addr) vbmcp_free(addr);
    }
    apply_pagination(doc, root, "functions", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* -------- vb_read_function -------- */
static char *tool_read_function(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int midx = arg_index(args, "module_index", -1);
    int fidx = arg_index(args, "function_index", -1);
    if (midx < 0 || fidx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index/function_index"));
    if ((long long)midx >= module_total() ||
        (long long)fidx >= module_function_total(midx)) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("function_index %d is out of range (module %d has %lld functions)", fidx, midx,
               module_function_total(midx)));
    }

    char *name      = vb_get_value(VB_GetModuleFunctionName, (int)midx, (int)fidx);
    char *addr      = vb_get_value(VB_GetModuleFunctionAddress, (int)midx, (int)fidx);
    char *code      = vb_get_value(VB_GetModuleFunction, (int)midx, (int)fidx);
    char *code_fast = vb_get_value(VB_GetModuleFunctionCode, (int)midx, (int)fidx);
    char *str_ref   = vb_get_value(VB_GetModuleFunctionStrRef, (int)midx, (int)fidx);
    char *disasm    = vb_get_value(VB_GetModuleFunctionDisasm, (int)midx, (int)fidx);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) {
        vbmcp_free(name); vbmcp_free(addr); vbmcp_free(code);
        vbmcp_free(code_fast); vbmcp_free(str_ref); vbmcp_free(disasm);
        return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    }
    yyjson_mut_obj_add_int(doc, root, "module_index", midx);
    yyjson_mut_obj_add_int(doc, root, "function_index", fidx);
    add_text(doc, root, "name", name ? name : "", enc);
    add_text(doc, root, "address", addr ? addr : "", enc);
    add_text(doc, root, "code", code ? code : "", enc);
    add_text(doc, root, "code_fast", code_fast ? code_fast : "", enc);
    add_text(doc, root, "string_references", str_ref ? str_ref : "", enc);
    add_text(doc, root, "disassembly", disasm ? disasm : "", enc);
    add_encoding_field(doc, root, enc);

    vbmcp_free(name);
    vbmcp_free(addr);
    vbmcp_free(code);
    vbmcp_free(code_fast);
    vbmcp_free(str_ref);
    vbmcp_free(disasm);
    return doc_to_string(doc);
}

/* -------- vb_read_sub_main -------- */
static char *tool_read_sub_main(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *code = vb_get_value(VB_GetSubMain, 0, 0);
    char *r = one_text_result("code", code ? code : "", enc, 1);
    if (code) vbmcp_free(code);
    return r;
}

/* -------- vb_read_active_text -------- */
static char *tool_read_active_text(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *text = vb_get_value(VB_GetActiveText, 0, 0);
    char *r = one_text_result("text", text ? text : "", enc, 1);
    if (text) vbmcp_free(text);
    return r;
}

/* -------- vb_read_active_disasm_text -------- */
static char *tool_read_active_disasm_text(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *text = vb_get_value(VB_GetActiveDisasmText, 0, 0);
    char *r = one_text_result("text", text ? text : "", enc, 1);
    if (text) vbmcp_free(text);
    return r;
}

/* -------- vb_read_active_coords -------- */
static char *tool_read_active_coords(yyjson_val *args, int *out_err)
{
    (void)out_err;
    const int enc = arg_encoding(args);
    char *s = vb_get_value(VB_GetActiveModuleCoordinats, 0, 0);
    char *r = one_text_result("coordinates", s ? s : "", enc, 0);
    if (s) vbmcp_free(s);
    return r;
}

/* -------- vb_read_list_frx_icons -------- */
static char *tool_read_list_frx_icons(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    long long limit, offset;
    read_paging(args, &limit, &offset);

    char *count_str = vb_get_value(VB_GetFrxIconCount, 0, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbmcp_free(count_str);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));

    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *off  = vb_get_value(VB_GetFrxIconOffset, (int)i, 0);
        char *size = vb_get_value(VB_GetFrxIconSize, (int)i, 0);
        yyjson_mut_val *item = yyjson_mut_arr_add_obj(doc, arr);
        if (item) {
            yyjson_mut_obj_add_int(doc, item, "index", i);
            add_text(doc, item, "offset", off ? off : "", enc);
            add_text(doc, item, "size", size ? size : "", enc);
        }
        if (off) vbmcp_free(off);
        if (size) vbmcp_free(size);
    }
    apply_pagination(doc, root, "icons", arr, total, offset, limit);
    return doc_to_string(doc);
}

/* ============================================================
 * 写入工具实现
 * ============================================================ */

/* -------- vb_write_project_text -------- */
static char *tool_write_project_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "project_text");
    if (!text) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing required parameter: project_text"));

    size_t text_len = strlen(text);
    if (text_len > VBMCP_MAX_PROJECT_TEXT) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("project_text exceeds the 16MB limit (actual %zu bytes); please write it in chunks",
               text_len));
    }

    set_text_value(VB_SetVBProject, text, enc, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    yyjson_mut_obj_add_int(doc, root, "length", (long long)text_len);
    return doc_to_string(doc);
}

/* -------- vb_write_clear_buffers -------- */
static char *tool_write_clear_buffers(yyjson_val *args, int *out_err)
{
    (void)args;
    vb_set_value(VB_ClearAllBuffers, "", 0, 0);
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_form -------- */
static char *tool_write_form(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "form_index", -1);
    const char *text = arg_str(args, "form_text");
    if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid form_index"));
    if (!text) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing form_text"));
    if ((long long)idx >= form_total()) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("form_index %d is out of range (the project has %lld forms)", idx, form_total()));
    }

    set_text_value(VB_SetVBForm, text, enc, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "form_index", idx);
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(text));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_rename_form -------- */
static char *tool_write_rename_form(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "form_index", -1);
    const char *new_name = arg_str(args, "new_name");
    if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid form_index"));
    if (!new_name) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing new_name"));
    if ((long long)idx >= form_total()) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("form_index %d is out of range (the project has %lld forms)", idx, form_total()));
    }
    if (is_native_project()) return err_json(out_err, VBMCP_ERR_BUSINESS, _("rename_form is not supported on native-compiled projects (it would trigger a modal dialog that blocks the RPC call). Use it on a source-form VB project."));

    set_text_value(VB_SetVBFormName, new_name, enc, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "form_index", idx);
    /* 回显客户端原文（UTF-8），不回显引擎侧转换结果 */
    yyjson_mut_obj_add_strcpy(doc, root, "new_name", new_name);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_module -------- */
static char *tool_write_module(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    const char *decl = arg_str(args, "declarations");
    if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if (!decl) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing declarations"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    set_text_value(VB_SetModule, decl, enc, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(decl));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_module_string_references -------- */
static char *tool_write_module_string_references(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int idx = arg_index(args, "module_index", -1);
    const char *refs = arg_str(args, "string_references");
    if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index"));
    if (!refs) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing string_references"));
    if ((long long)idx >= module_total()) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("module_index %d is out of range (the project has %lld modules)", idx, module_total()));
    }

    set_text_value(VB_SetModuleStringReferences, refs, enc, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", idx);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_function -------- */
static char *tool_write_function(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int midx = arg_index(args, "module_index", -1);
    int fidx = arg_index(args, "function_index", -1);
    const char *code = arg_str(args, "code");
    if (midx < 0 || fidx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index/function_index"));
    if (!code) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing code"));
    if ((long long)midx >= module_total() ||
        (long long)fidx >= module_function_total(midx)) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("function_index %d is out of range (module %d has %lld functions)", fidx, midx,
               module_function_total(midx)));
    }

    set_text_value(VB_SetModuleFunction, code, enc, (int)midx, (int)fidx);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", midx);
    yyjson_mut_obj_add_int(doc, root, "function_index", fidx);
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(code));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_rename_function -------- */
static char *tool_write_rename_function(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int midx = arg_index(args, "module_index", -1);
    int fidx = arg_index(args, "function_index", -1);
    const char *new_name = arg_str(args, "new_name");
    if (midx < 0 || fidx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid module_index/function_index"));
    if (!new_name) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing new_name"));
    if ((long long)midx >= module_total() ||
        (long long)fidx >= module_function_total(midx)) {
        return err_json(out_err, VBMCP_ERR_INVALID_PARAMS,
            _F("function_index %d is out of range (module %d has %lld functions)", fidx, midx,
               module_function_total(midx)));
    }
    if (is_native_project()) return err_json(out_err, VBMCP_ERR_BUSINESS, _("rename_function is not supported on native-compiled projects (it would trigger a modal dialog that blocks the RPC call). Use it on a source-form VB project."));

    set_text_value(VB_SetModuleFunctionName, new_name, enc, (int)midx, (int)fidx);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "module_index", midx);
    yyjson_mut_obj_add_int(doc, root, "function_index", fidx);
    /* 回显客户端原文（UTF-8），不回显引擎侧转换结果 */
    yyjson_mut_obj_add_strcpy(doc, root, "new_name", new_name);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_sub_main -------- */
static char *tool_write_sub_main(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *code = arg_str(args, "code");
    if (!code) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing code"));

    set_text_value(VB_SetSubMain, code, enc, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(code));
    return doc_to_string(doc);
}

/* -------- vb_write_active_text -------- */
static char *tool_write_active_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "text");
    if (!text) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing text"));
    set_text_value(VB_SetActiveText, text, enc, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    yyjson_mut_obj_add_int(doc, root, "length", (long long)strlen(text));
    return doc_to_string(doc);
}

/* -------- vb_write_active_disasm_text -------- */
static char *tool_write_active_disasm_text(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "text");
    if (!text) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing text"));
    set_text_value(VB_SetActiveDisasmText, text, enc, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_goto_line -------- */
static char *tool_write_goto_line(yyjson_val *args, int *out_err)
{
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    int line = arg_index(args, "line", -1);
    if (line < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid line"));

    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", line);
    vb_set_value(VB_SetActiveTextLine, buf, 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_int(doc, root, "line", line);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_status -------- */
static char *tool_write_status(yyjson_val *args, int *out_err)
{
    const int enc = arg_encoding(args);
    const char *text = arg_str(args, "text");
    if (!text) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing text"));
    set_text_value(VB_SetStatusBarText, text, enc, 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_options -------- */
static char *tool_write_options(yyjson_val *args, int *out_err)
{
    if (!args || !yyjson_is_obj(args)) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing arguments object"));
    const char *stack    = arg_str(args, "stack_check");
    const char *analyzer = arg_str(args, "analyzer_check");
    if (stack)    vb_set_value(VB_SetStackCheckBoxValue, stack, 0, 0);
    if (analyzer) vb_set_value(VB_SetAnalyzerCheckBoxValue, analyzer, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    if (stack)    yyjson_mut_obj_add_strcpy(doc, root, "stack_check", stack);
    if (analyzer) yyjson_mut_obj_add_strcpy(doc, root, "analyzer_check", analyzer);
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* -------- vb_write_update_all -------- */
static char *tool_write_update_all(yyjson_val *args, int *out_err)
{
    (void)args;
    vb_set_value(VB_UpdateAll, "", 0, 0);
    yyjson_mut_val *root = NULL;
    yyjson_mut_doc *doc = new_obj_doc(&root);
    if (!doc) return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory"));
    yyjson_mut_obj_add_strcpy(doc, root, "status", "ok");
    return doc_to_string(doc);
}

/* ============================================================
 * 工具表 + 分发
 * ============================================================ */

typedef char *(*tool_impl_fn)(yyjson_val *args, int *out_err);

typedef struct {
    const char *name;
    const char *desc;       /* msgid */
    unsigned    params;     /* P_* 位掩码 */
    int         is_write;   /* 1 = vb_write_*（需"允许写入"） */
    int         destructive;
    int         idempotent;
    tool_impl_fn impl;
} tool_entry_t;

/* 参数形状位掩码 */
#define P_PAGING        (1u << 0)
#define P_FORM_INDEX    (1u << 1)
#define P_PROJECT_TEXT  (1u << 2)
#define P_MODULE_INDEX  (1u << 3)
#define P_FUNCTION_INDEX (1u << 4)
#define P_FORM_TEXT     (1u << 5)
#define P_NEW_NAME      (1u << 6)
#define P_TEXT          (1u << 7)
#define P_LINE          (1u << 8)
#define P_CODE          (1u << 9)
#define P_DECLARATIONS  (1u << 10)
#define P_STRING_REFS   (1u << 11)
#define P_OPTIONS       (1u << 12)

static const tool_entry_t g_tools[] = {
    /* ---- 只读 ---- */
    { "vb_read_project_info",        _("Get metadata of the project currently open in VB Decompiler: source file path, whether it is natively compiled, compiler type (VB/.NET/Delphi/Unknown), whether it is packed, and the VB Decompiler installation path. No parameters."),           0,                                 0, 0, 1, tool_read_project_info },
    { "vb_read_project_text",        _("Get the text of the project file (.vbp manifest: project type, references, components, startup object). This is the project descriptor, NOT the concatenated decompiled source of every form/module - to read code, list the forms/modules (vb_read_list_forms / vb_read_list_modules) and read them one by one."),           0,                                 0, 0, 1, tool_read_project_text },
    { "vb_read_list_forms",          _("List all VB Forms in the project. Supports pagination (limit/offset). Returns {count, forms:[...]}."),             P_PAGING,                          0, 0, 1, tool_read_list_forms },
    { "vb_read_form",                _("Get the decompiled property text of the specified Form. Returns {form_index, form_name, form_text}."),               P_FORM_INDEX,                      0, 0, 1, tool_read_form },
    { "vb_read_list_modules",        _("List all Modules in the project (standard .bas, class .cls, control .ctl). Supports pagination. Returns {count, modules:[...]}."),           P_PAGING,                          0, 0, 1, tool_read_list_modules },
    { "vb_read_module",              _("Get the module's declarations header (all function/constant/type declarations) and string references."),             P_MODULE_INDEX,                    0, 0, 1, tool_read_module },
    { "vb_read_list_module_functions", _("List all functions in the specified module. Supports pagination. Returns {module_index, count, functions:[{name,address,index}...]}."), P_MODULE_INDEX | P_PAGING,        0, 0, 1, tool_read_list_module_functions },
    { "vb_read_function",            _("Get a function's decompiled code plus multiple views (fast/disassembly/string references). Returns {module_index, function_index, name, address, code, code_fast, disassembly, string_references}."),           P_MODULE_INDEX | P_FUNCTION_INDEX, 0, 0, 1, tool_read_function },
    { "vb_read_sub_main",            _("Get the code of the program entry point Sub Main. No parameters."),           0,                                 0, 0, 1, tool_read_sub_main },
    { "vb_read_active_text",         _("Get the text of the currently active code window (editor)."),        0,                                 0, 0, 1, tool_read_active_text },
    { "vb_read_active_disasm_text",  _("Get the raw text of the active disassembly window."), 0,                                 0, 0, 1, tool_read_active_disasm_text },
    { "vb_read_active_coords",       _("Get the module/function coordinates of the currently active view (e.g. \"ModuleIndex,FunctionIndex\")."),      0,                                 0, 0, 1, tool_read_active_coords },
    { "vb_read_list_frx_icons",      _("List icons/images in the Form's FRX file: {count, icons:[{index,offset,size}]}. Supports pagination."),         P_PAGING,                          0, 0, 1, tool_read_list_frx_icons },

    /* ---- 写入 ---- */
    { "vb_write_project_text",       _("Completely replace the project text with new text. Automatically calls UpdateAll to commit changes and refresh the GUI."),       P_PROJECT_TEXT,                    1, 1, 1, tool_write_project_text },
    { "vb_write_clear_buffers",      _("Clear VB Decompiler's internal decompilation buffers. Use only when implementing a custom full decompilation workflow; discards all existing decompilation results."),          0,                                 1, 1, 1, tool_write_clear_buffers },
    { "vb_write_form",               _("Replace the property text of the specified Form. Automatically calls UpdateAll."),               P_FORM_INDEX | P_FORM_TEXT,        1, 1, 1, tool_write_form },
    { "vb_write_rename_form",        _("Rename a Form."),            P_FORM_INDEX | P_NEW_NAME,         1, 0, 1, tool_write_rename_form },
    { "vb_write_module",             _("Replace the module's declarations header."),             P_MODULE_INDEX | P_DECLARATIONS,   1, 1, 1, tool_write_module },
    { "vb_write_module_string_references", 0,                       P_MODULE_INDEX | P_STRING_REFS,    1, 1, 1, tool_write_module_string_references },
    { "vb_write_function",           _("Replace a function's code."),           P_MODULE_INDEX | P_FUNCTION_INDEX | P_CODE, 1, 1, 1, tool_write_function },
    { "vb_write_rename_function",    _("Rename a function."),        P_MODULE_INDEX | P_FUNCTION_INDEX | P_NEW_NAME, 1, 0, 1, tool_write_rename_function },
    { "vb_write_sub_main",           _("Set the Sub Main code."),           P_CODE,                            1, 1, 1, tool_write_sub_main },
    { "vb_write_active_text",        _("Set the text of the currently active code window. Automatically calls UpdateAll."),        P_TEXT,                            1, 0, 1, tool_write_active_text },
    { "vb_write_active_disasm_text", _("Set the text of the active disassembly window."), P_TEXT,                            1, 0, 1, tool_write_active_disasm_text },
    { "vb_write_goto_line",          _("Jump to the specified line in the code window (UI only, does not modify text)."),              P_LINE,                            1, 0, 1, tool_write_goto_line },
    { "vb_write_status",             _("Set the text of VB Decompiler's bottom status bar (used to show MCP operation progress)."),             P_TEXT,                            1, 0, 1, tool_write_status },
    { "vb_write_options",            _("Control decompilation options: the \"Parse stack parameters\" and \"Procedure analyzer and optimizer\" checkboxes. Omitting either parameter leaves the corresponding option unchanged."),            P_OPTIONS,                         1, 0, 1, tool_write_options },
    { "vb_write_update_all",         _("Commit all Set modifications and refresh the GUI (tools normally call this automatically; call manually only after modifying large amounts of data externally)."),             0,                                 1, 0, 1, tool_write_update_all },

    { NULL, NULL, 0, 0, 0, 0, NULL }
};

/* 项目加载检查豁免名单：探测/控制类工具，不要求项目已加载。 */
static const char *k_no_project_check[] = {
    "vb_read_project_info",
    "vb_write_status",
    "vb_write_options",
    "vb_write_update_all",
    "vb_write_clear_buffers",
    NULL
};

static const tool_entry_t *find_tool(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; g_tools[i].name; i++) {
        if (vb_streq(name, g_tools[i].name)) return &g_tools[i];
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
 *   allow_write   是否允许写入（只读模式下拒绝 vb_write_*）
 *   out_error_code 返回错误分类（VBMCP_ERR_*）；成功为 VBMCP_ERR_OK
 * 返回工具结果 JSON 字符串（new alloc，调用方 vbmcp_free）。 */
char *mcp_dispatch_tool(const char *tool_name, yyjson_val *args,
                        int allow_write, int *out_error_code)
{
    if (out_error_code) *out_error_code = VBMCP_ERR_OK;

    if (!tool_name || !*tool_name) {
        return err_json(out_error_code, VBMCP_ERR_INVALID_PARAMS, _("Tool name is empty"));
    }

    const tool_entry_t *e = find_tool(tool_name);
    if (!e) {
        return err_json(out_error_code, VBMCP_ERR_INVALID_PARAMS, _F("Unknown tool: %s", tool_name));
    }

    /* 防御兜底：只读模式下即便客户端绕过 listTools 强行调用 vb_write_*，
     * 也在此二次校验权限，直接返回 MCP 权限错误，不执行业务。 */
    if (e->is_write && !allow_write) {
        return err_json(out_error_code, VBMCP_ERR_PERMISSION, _("Write access is disabled. All vb_write_* tools are unavailable in read-only mode. Ask the user to enable \"Allow MCP client to write project data\" in the VB Decompiler MCP settings."));
    }

    /* 集中项目加载检查 */
    int needs_check = 1;
    for (int j = 0; k_no_project_check[j]; j++) {
        if (vb_streq(tool_name, k_no_project_check[j])) { needs_check = 0; break; }
    }
    if (needs_check && !vb_is_project_loaded()) {
        return err_json(out_error_code, VBMCP_ERR_BUSINESS,
            _F("Project not loaded. The VB SDK provides no ReloadFile/Refresh API, so vbmcp cannot re-initialize the engine from the DLL side. Please open the vbmcp settings window from the Plugins menu in the VB Decompiler main window and click OK (this triggers a VBDecompilerPluginLoad re-entry). Tool called: %s",
               tool_name));
    }

    int err = VBMCP_ERR_OK;
    char *payload = e->impl(args, &err);
    /* 写操作可能改变模块/表单/函数列表：使模块名缓存失效 */
    if (e->is_write) g_write_generation++;
    if (out_error_code) *out_error_code = err;
    return payload;
}

/* ============================================================
 * tools/list schema 构造（全部 yyjson）
 *
 * annotations (MCP 2025-03 规范):
 *   readOnlyHint / destructiveHint / idempotentHint / openWorldHint
 * vbmcp 全部为本地工具，openWorldHint 恒为 false。
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

static void param_onoff(yyjson_mut_doc *doc, yyjson_mut_val *props, const char *name)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "type", "string");
    yyjson_mut_val *en = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_strcpy(doc, en, "0");
    yyjson_mut_arr_add_strcpy(doc, en, "1");
    yyjson_mut_obj_add_val(doc, o, "enum", en);
    yyjson_mut_obj_add_strcpy(doc, o, "description", _("\"0\"=off, \"1\"=on"));
    yyjson_mut_obj_add_val(doc, props, name, o);
}

/* 所有工具统一接受的可选 encoding 参数（见文件头"数据编码"小节）。 */
static void param_encoding(yyjson_mut_doc *doc, yyjson_mut_val *props)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "type", "string");
    yyjson_mut_val *en = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_strcpy(doc, en, "utf-8");
    yyjson_mut_arr_add_strcpy(doc, en, "gbk");
    yyjson_mut_obj_add_val(doc, o, "enum", en);
    yyjson_mut_obj_add_strcpy(doc, o, "description",
        _("Encoding of text fields: \"utf-8\" (default) converts between the project's ANSI/GBK bytes and UTF-8; \"gbk\" returns/sends raw ANSI bytes (legacy behavior)."));
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
        param_int(doc, props, "form_index", 0, 0, -1, _("Form index (0-based)"));
    if (flags & P_PROJECT_TEXT)
        param_str(doc, props, "project_text", _("Full replacement project text (required)"));
    if (flags & P_MODULE_INDEX)
        param_int(doc, props, "module_index", 0, 0, -1, NULL);
    if (flags & P_FUNCTION_INDEX)
        param_int(doc, props, "function_index", 0, 0, -1, NULL);
    if (flags & P_FORM_TEXT)
        param_str(doc, props, "form_text", NULL);
    if (flags & P_NEW_NAME)
        param_str(doc, props, "new_name", NULL);
    if (flags & P_TEXT)
        param_str(doc, props, "text", NULL);
    if (flags & P_LINE)
        param_int(doc, props, "line", 0, 0, -1, _("Target line number (0-based)"));
    if (flags & P_CODE)
        param_str(doc, props, "code", NULL);
    if (flags & P_DECLARATIONS)
        param_str(doc, props, "declarations", NULL);
    if (flags & P_STRING_REFS)
        param_str(doc, props, "string_references", NULL);
    if (flags & P_OPTIONS) {
        param_onoff(doc, props, "stack_check");
        param_onoff(doc, props, "analyzer_check");
    }
}

static void build_one_tool(yyjson_mut_doc *doc, yyjson_mut_val *arr, const tool_entry_t *e)
{
    yyjson_mut_val *t = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, t, "name", e->name);
    /* desc == NULL 表示暂无描述（如 module_string_references）：用工具名兜底。
     * 工具名是协议标识符，不翻译；描述走 i18n（MCP 报文必须是 UTF-8）。 */
    yyjson_mut_obj_add_strcpy(doc, t, "description",
        (e->desc && *e->desc) ? _(e->desc) : e->name);

    yyjson_mut_val *schema = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, schema, "type", "object");
    yyjson_mut_val *props = yyjson_mut_obj(doc);
    add_params(doc, props, e->params);
    yyjson_mut_obj_add_val(doc, schema, "properties", props);
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
 * allow_write=0 时只追加 vb_read_*，=1 时追加全部。 */
void mcp_build_tools_into(yyjson_mut_doc *doc, yyjson_mut_val *arr, int allow_write)
{
    if (!doc || !arr) return;
    for (int i = 0; g_tools[i].name; i++) {
        if (g_tools[i].is_write && !allow_write) continue;
        build_one_tool(doc, arr, &g_tools[i]);
    }
}
