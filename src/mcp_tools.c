/*
 * mcp_tools.c - MCP 工具实现：把所有 VB Decompiler SDK 操作封装为 MCP 工具
 *
 * 设计原则：
 *  - 工具名采用下划线风格（get_project、list_modules 等），与 MCP 客户端习惯一致。
 *  - 输入用 JSON 对象（arguments），输出也是 JSON。
 *  - 错误返回 {"error":"..."} JSON 字符串。
 *  - 大文本（project_text 等）原样返回，避免缓冲区膨胀。
 *  - 所有工具实现都假定在主线程调用（避开 Callback Engine 线程安全问题）。
 *  - 所有用户可见文本（错误消息 + tools/list 描述）经 vbmcp_t_utf8() 按
 *    VB Decompiler 主程序语言（注册表 CustomLanguage，插件加载时由
 *    VBDecompilerPluginName → vbmcp_lang_refresh() 刷新）返回 EN / ZH 的
 *    UTF-8 字符串，与 HTTP 层 application/json（UTF-8）保持一致。
 */
#include "mcp_tools.h"
#include "vbmcp.h"
#include "lang.h"
#include <stdio.h>

/* ============================================================
 * 错误响应辅助
 * ============================================================ */

static jv_value_t *make_error(const char *msg)
{
    jv_value_t *err = (jv_value_t *)calloc(1, sizeof(*err));
    if (!err) return NULL;
    err->type = JV_OBJECT;
    /* 直接构造 {"error":"..."} */
    jv_value_t *v = (jv_value_t *)calloc(1, sizeof(*v));
    if (!v) { free(err); return NULL; }
    v->type = JV_STRING;
    v->u.s = vbmcp_strdup(msg ? msg : "");

    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    if (!m) { free(v); free(err); return NULL; }
    m->key = vbmcp_strdup("error");
    m->value = v;
    err->u.members = m;
    return err;
}

/* 把 ANSI 字符串塞进返回值对象 {"text": "..."} */
static jv_value_t *wrap_text(const char *s)
{
    jv_value_t *root = (jv_value_t *)calloc(1, sizeof(*root));
    jv_value_t *val = (jv_value_t *)calloc(1, sizeof(*val));
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    if (!root || !val || !m) { free(root); free(val); free(m); return NULL; }
    root->type = JV_OBJECT;
    val->type = JV_STRING;
    val->u.s = vbmcp_strdup(s ? s : "");
    m->key = vbmcp_strdup("text");
    m->value = val;
    root->u.members = m;
    return root;
}

/* 把 ANSI 字符串放进对象 {"field": "..."} */
static jv_value_t *wrap_field(const char *field, const char *s)
{
    jv_value_t *root = (jv_value_t *)calloc(1, sizeof(*root));
    jv_value_t *val = (jv_value_t *)calloc(1, sizeof(*val));
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    if (!root || !val || !m) { free(root); free(val); free(m); return NULL; }
    root->type = JV_OBJECT;
    val->type = JV_STRING;
    val->u.s = vbmcp_strdup(s ? s : "");
    m->key = vbmcp_strdup(field);
    m->value = val;
    root->u.members = m;
    return root;
}

/* Native 项目守护：demo/正式版 VB Decompiler 都会在 rename_* / UpdateAll 等
 * 操作触发 modal "Please fill forms and modules before call UpdateAll"，
 * 阻塞 RPC 调用栈。返回 1=是 native 项目（应拒绝），0=非 native（允许）。 */
static int is_native_project(void)
{
    char *s = vb_get_value(VB_IsNativeCompilation, 0, 0);
    int nat = (s && s[0] == '1') ? 1 : 0;
    if (s) vbmcp_free(s);
    return nat;
}

/* 给对象添加成员。add_str(root, "key", "value") */
static int add_str(jv_value_t *obj, const char *key, const char *s)
{
    if (!obj || obj->type != JV_OBJECT) return 0;
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    jv_value_t *v = (jv_value_t *)calloc(1, sizeof(*v));
    if (!m || !v) { free(m); free(v); return 0; }
    m->key = vbmcp_strdup(key);
    v->type = JV_STRING;
    v->u.s = vbmcp_strdup(s ? s : "");
    m->value = v;
    /* 加到尾部 */
    if (!obj->u.members) obj->u.members = m;
    else {
        jv_member_t *t = obj->u.members;
        while (t->next) t = t->next;
        t->next = m;
    }
    return 1;
}

static int add_int(jv_value_t *obj, const char *key, long long v)
{
    if (!obj || obj->type != JV_OBJECT) return 0;
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    jv_value_t *vv = (jv_value_t *)calloc(1, sizeof(*vv));
    if (!m || !vv) { free(m); free(vv); return 0; }
    m->key = vbmcp_strdup(key);
    vv->type = JV_NUMBER;
    vv->u.n = (double)v;
    m->value = vv;
    if (!obj->u.members) obj->u.members = m;
    else {
        jv_member_t *t = obj->u.members;
        while (t->next) t = t->next;
        t->next = m;
    }
    return 1;
}

static int add_bool(jv_value_t *obj, const char *key, int v)
{
    if (!obj || obj->type != JV_OBJECT) return 0;
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    jv_value_t *vv = (jv_value_t *)calloc(1, sizeof(*vv));
    if (!m || !vv) { free(m); free(vv); return 0; }
    m->key = vbmcp_strdup(key);
    vv->type = JV_BOOL;
    vv->u.b = v ? 1 : 0;
    m->value = vv;
    if (!obj->u.members) obj->u.members = m;
    else {
        jv_member_t *t = obj->u.members;
        while (t->next) t = t->next;
        t->next = m;
    }
    return 1;
}

static int add_null(jv_value_t *obj, const char *key)
{
    if (!obj || obj->type != JV_OBJECT) return 0;
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    jv_value_t *vv = (jv_value_t *)calloc(1, sizeof(*vv));
    if (!m || !vv) { free(m); free(vv); return 0; }
    m->key = vbmcp_strdup(key);
    vv->type = JV_NULL;
    m->value = vv;
    if (!obj->u.members) obj->u.members = m;
    else {
        jv_member_t *t = obj->u.members;
        while (t->next) t = t->next;
        t->next = m;
    }
    return 1;
}

static jv_value_t *make_object(void)
{
    jv_value_t *o = (jv_value_t *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->type = JV_OBJECT;
    return o;
}

/* 构造空 JV_ARRAY（new alloc）。失败返回 NULL。 */
static jv_value_t *make_array(void)
{
    jv_value_t *a = (jv_value_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->type = JV_ARRAY;
    return a;
}

/* 把 jv_value_t* push 进 JV_ARRAY。0=ok / -1=err。 */
static int arr_push(jv_value_t *arr, jv_value_t *item)
{
    if (!arr || !item) return -1;
    if (arr->u.a.count == arr->u.a.cap) {
        size_t nc = arr->u.a.cap ? arr->u.a.cap * 2 : 4;
        jv_value_t **ni = (jv_value_t **)calloc(nc, sizeof(jv_value_t *));
        if (!ni) return -1;
        if (arr->u.a.items)
            memcpy(ni, arr->u.a.items, sizeof(jv_value_t *) * arr->u.a.count);
        free(arr->u.a.items);
        arr->u.a.items = ni;
        arr->u.a.cap = nc;
    }
    arr->u.a.items[arr->u.a.count++] = item;
    return 0;
}

/* 把 arr 挂到 root 的指定 key 下（如果该 key 已存在则覆盖）。 */
static void attach_member(jv_value_t *root, const char *key, jv_value_t *arr)
{
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    if (!m) return;
    m->key = vbmcp_strdup(key);
    m->value = arr;
    if (!root->u.members) {
        root->u.members = m;
        return;
    }
    jv_member_t *t = root->u.members;
    while (t->next) t = t->next;
    t->next = m;
}

/* 分页元数据 + 把 arr 挂到 root（key 由调用方决定）。
 * 遵守 MCP Best Practices 分页约定：
 *   - limit/offset 必须被尊重（调用方循环时已跳过 [0, offset) 并提前 break）
 *   - 返回 {count, total_count, offset, limit, has_more, next_offset}
 *     其中 count = 实际返回项数 = arr->u.a.count
 *           total_count = 列表总项数（工具从 VB_Get*Count 拿到）
 *           next_offset  = 是否有更多；>=0 表示下次从 next_offset 开始，-1 表示结束
 * 旧的"forms"/"modules"/"functions"/"icons"键名保留以兼容已有客户端。 */
static void apply_pagination(jv_value_t *root, const char *items_key,
                             jv_value_t *arr,
                             long long total_count,
                             long long offset, long long limit)
{
    long long returned = arr ? (long long)arr->u.a.count : 0;
    long long next_offset = -1;
    if (returned >= limit && (offset + returned) < total_count) {
        next_offset = offset + returned;
    }
    add_int(root, "count", returned);
    add_int(root, "total_count", total_count);
    add_int(root, "offset", offset);
    add_int(root, "limit", limit);
    add_int(root, "has_more", (next_offset >= 0) ? 1 : 0);
    if (next_offset >= 0) add_int(root, "next_offset", next_offset);
    if (arr) attach_member(root, items_key, arr);
}

/* 安全地把整数字符串解析为 int（容错） */
static long long safe_parse_int(const char *s, long long def)
{
    if (!s || !*s) return def;
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s) return def;
    return v;
}

/* ============================================================
 * 工具实现
 * ============================================================ */

/* -------- 工具 1: get_project_info --------
 * 获取当前反编译项目的基本信息：源文件、编译器类型、是否native、是否打包。
 * 无参数。
 */
static jv_value_t *tool_get_project_info(const jv_value_t *args)
{
    (void)args;
    jv_value_t *root = make_object();
    if (!root) return make_error(vbmcp_t_utf8(T_ERR_OOM));

    char *file_name = vb_get_value(VB_GetFileName, 0, 0);
    char *dec_path = vb_get_value(VB_GetVBDecompilerPath, 0, 0);
    char *native = vb_get_value(VB_IsNativeCompilation, 0, 0);
    char *compiler = vb_get_value(VB_GetCompiler, 0, 0);
    char *packed = vb_get_value(VB_IsPacked, 0, 0);

    add_str(root, "file_name", file_name ? file_name : "");
    add_str(root, "vbmcp_path", dec_path ? dec_path : "");
    add_str(root, "is_native", native ? native : "");
    add_str(root, "compiler", compiler ? compiler : "");
    add_str(root, "is_packed", packed ? packed : "");

    vbmcp_free(file_name);
    vbmcp_free(dec_path);
    vbmcp_free(native);
    vbmcp_free(compiler);
    vbmcp_free(packed);
    return root;
}

/* -------- 工具 2: get_project --------
 * 返回完整反编译项目文本（可能很大）。
 */
static jv_value_t *tool_get_project(const jv_value_t *args)
{
    (void)args;
    char *text = vb_get_value(VB_GetVBProject, 0, 0);
    if (!text) return make_error(vbmcp_t_utf8(T_ERR_NO_PROJECT));
    jv_value_t *r = wrap_field("project_text", text);
    vbmcp_free(text);
    return r;
}

/* -------- 工具 3: set_project --------
 * 用新文本替换整个项目内容。
 * 参数：project_text (string, 必填)
 */
static jv_value_t *tool_set_project(const jv_value_t *args)
{
    const char *text = (args && args->type == JV_OBJECT) ? jv_obj_get_string(args, "project_text") : NULL;
    if (!text) return make_error(vbmcp_t_utf8(T_ERR_MISSING_PROJECT_TEXT));

    /* 防止客户端发 GB 级 project_text 直接撑爆 VB Decompiler。
     * HTTP 层 VBMCP_MAX_BODY_SIZE=64MB 是兜底，这里对单个字段再收紧。 */
    size_t text_len = strlen(text);
    if (text_len > VBMCP_MAX_PROJECT_TEXT) {
        char err[128];
        _snprintf_s(err, sizeof(err), _TRUNCATE,
            vbmcp_t_utf8(T_ERR_PROJECT_TEXT_TOO_LARGE), text_len);
        return make_error(err);
    }

    vb_set_value(VB_SetVBProject, text, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    if (!root) return make_error(vbmcp_t_utf8(T_ERR_OOM));
    add_str(root, "status", "ok");
    add_int(root, "length", (long long)text_len);
    return root;
}

/* -------- 工具 4: clear_buffers --------
 * 清空所有VB Decompiler内部缓冲（高级用法：用于完全自定义的反编译工作流）。
 */
static jv_value_t *tool_clear_buffers(const jv_value_t *args)
{
    (void)args;
    vb_set_value(VB_ClearAllBuffers, "", 0, 0);
    jv_value_t *root = make_object();
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 5: list_forms --------
 * 列出所有 VB Form（表单），返回名称数组。
 * 无参数。
 */
static jv_value_t *tool_list_forms(const jv_value_t *args)
{
    /* 分页参数：limit 默认 100，offset 默认 0。limit 最大 10000（schema 已约束）。 */
    long long limit = 100, offset = 0;
    if (args && args->type == JV_OBJECT) {
        limit  = (long long)jv_obj_get_number(args, "limit",  100);
        offset = (long long)jv_obj_get_number(args, "offset",   0);
        if (limit  < 1)   limit  = 1;
        if (limit  > 10000) limit = 10000;
        if (offset < 0)   offset = 0;
    }

    char *count_str = vb_get_value(VB_GetVBFormCount, 0, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbmcp_free(count_str);

    jv_value_t *root = make_object();
    if (!root) return make_error(vbmcp_t_utf8(T_ERR_OOM));

    jv_value_t *arr = make_array();
    if (!arr) { free(root); return make_error(vbmcp_t_utf8(T_ERR_OOM)); }

    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *name = vb_get_value(VB_GetVBFormName, (int)i, 0);
        jv_value_t *item = (jv_value_t *)calloc(1, sizeof(*item));
        if (!item) { if (name) vbmcp_free(name); break; }
        item->type = JV_STRING;
        item->u.s = vbmcp_strdup(name ? name : "");
        if (name) vbmcp_free(name);
        arr_push(arr, item);
    }

    apply_pagination(root, "forms", arr, total, offset, limit);
    return root;
}

/* -------- 工具 6: get_form --------
 * 获取指定表单的反编译内容。
 * 参数：form_index (int, 必填，从 0 开始)
 */
static jv_value_t *tool_get_form(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long idx = (long long)jv_obj_get_number(args, "form_index", -1);
    if (idx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_FORM_INDEX));

    char *content = vb_get_value(VB_GetVBForm, (int)idx, 0);
    if (!content) return make_error(vbmcp_t_utf8(T_ERR_CANNOT_READ_FORM));

    jv_value_t *root = make_object();
    add_int(root, "form_index", idx);
    add_str(root, "form_text", content);
    vbmcp_free(content);

    /* 顺便返回名称 */
    char *name = vb_get_value(VB_GetVBFormName, (int)idx, 0);
    add_str(root, "form_name", name ? name : "");
    vbmcp_free(name);
    return root;
}

/* -------- 工具 7: set_form --------
 * 修改指定表单的属性。
 * 参数：form_index (int), form_text (string)
 */
static jv_value_t *tool_set_form(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long idx = (long long)jv_obj_get_number(args, "form_index", -1);
    const char *text = jv_obj_get_string(args, "form_text");
    if (idx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_FORM_INDEX));
    if (!text) return make_error(vbmcp_t_utf8(T_ERR_MISSING_FORM_TEXT));

    vb_set_value(VB_SetVBForm, text, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_int(root, "form_index", idx);
    add_int(root, "length", (long long)strlen(text));
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 8: rename_form --------
 * 重命名表单。
 */
static jv_value_t *tool_rename_form(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long idx = (long long)jv_obj_get_number(args, "form_index", -1);
    const char *new_name = jv_obj_get_string(args, "new_name");
    if (idx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_FORM_INDEX));
    if (!new_name) return make_error(vbmcp_t_utf8(T_ERR_MISSING_NEW_NAME));

    /* Native guard: 防止 SDK 在 native 项目上弹 modal 卡死 RPC */
    if (is_native_project()) {
        return make_error(vbmcp_t_utf8(T_ERR_NATIVE_RENAME_FORM));
    }

    vb_set_value(VB_SetVBFormName, new_name, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_int(root, "form_index", idx);
    add_str(root, "new_name", new_name);
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 9: list_modules --------
 * 列出所有模块（bas/cls/ctl）。
 *
 * 注：SDK 26 中 value=36 在 .h 是 SetModuleCount、.inc 是 GetModuleCount，
 * 4 个 demo 全部不使用此值，行为未定义且可能误设模块计数。
 * 因此改用 GetModuleName(0..N) 探测到 NULL/空串的方式确定 total。
 */
#define VBMCP_MAX_MODULES 10000  /* 安全上限：项目内模块数硬上限 */
static jv_value_t *tool_list_modules(const jv_value_t *args)
{
    long long limit = 100, offset = 0;
    if (args && args->type == JV_OBJECT) {
        limit  = (long long)jv_obj_get_number(args, "limit",  100);
        offset = (long long)jv_obj_get_number(args, "offset",   0);
        if (limit  < 1)   limit  = 1;
        if (limit  > 10000) limit = 10000;
        if (offset < 0)   offset = 0;
    }

    /* 探测阶段：循环 GetModuleName 直到 NULL/空串，缓存所有名字指针。
     * 用指针数组缓存避免 2N 次 engine 调用。 */
    char **names = (char **)vbmcp_alloc(VBMCP_MAX_MODULES * sizeof(char *));
    if (!names) return make_error(vbmcp_t_utf8(T_ERR_OOM));
    long long total = 0;
    for (long long i = 0; i < VBMCP_MAX_MODULES; i++) {
        char *name = vb_get_value(VB_GetModuleName, (int)i, 0);
        if (!name || !*name) {
            if (name) vbmcp_free(name);
            break;
        }
        names[i] = name;
        total++;
    }

    jv_value_t *root = make_object();
    if (!root) {
        for (long long i = 0; i < total; i++) vbmcp_free(names[i]);
        vbmcp_free(names);
        return make_error(vbmcp_t_utf8(T_ERR_OOM));
    }

    jv_value_t *arr = make_array();
    if (!arr) {
        for (long long i = 0; i < total; i++) vbmcp_free(names[i]);
        vbmcp_free(names);
        free(root);
        return make_error(vbmcp_t_utf8(T_ERR_OOM));
    }

    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        jv_value_t *item = (jv_value_t *)calloc(1, sizeof(*item));
        if (!item) break;
        item->type = JV_STRING;
        item->u.s = vbmcp_strdup(names[i]);
        arr_push(arr, item);
    }

    for (long long i = 0; i < total; i++) vbmcp_free(names[i]);
    vbmcp_free(names);

    apply_pagination(root, "modules", arr, total, offset, limit);
    return root;
}

/* -------- 工具 10: get_module --------
 * 获取模块的声明头（declarations）。
 * 参数：module_index (int)
 */
static jv_value_t *tool_get_module(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long idx = (long long)jv_obj_get_number(args, "module_index", -1);
    if (idx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_MODULE_INDEX));

    char *content = vb_get_value(VB_GetModule, (int)idx, 0);
    char *name = vb_get_value(VB_GetModuleName, (int)idx, 0);
    char *str_ref = vb_get_value(VB_GetModuleStringReferences, (int)idx, 0);

    jv_value_t *root = make_object();
    add_int(root, "module_index", idx);
    add_str(root, "module_name", name ? name : "");
    add_str(root, "declarations", content ? content : "");
    add_str(root, "string_references", str_ref ? str_ref : "");

    vbmcp_free(content);
    vbmcp_free(name);
    vbmcp_free(str_ref);
    return root;
}

/* -------- 工具 11: set_module --------
 * 替换模块声明。
 */
static jv_value_t *tool_set_module(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long idx = (long long)jv_obj_get_number(args, "module_index", -1);
    const char *decl = jv_obj_get_string(args, "declarations");
    if (idx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_MODULE_INDEX));
    if (!decl) return make_error(vbmcp_t_utf8(T_ERR_MISSING_DECLARATIONS));

    vb_set_value(VB_SetModule, decl, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_int(root, "module_index", idx);
    add_int(root, "length", (long long)strlen(decl));
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 12: set_module_string_references --------
 */
static jv_value_t *tool_set_module_string_references(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long idx = (long long)jv_obj_get_number(args, "module_index", -1);
    const char *refs = jv_obj_get_string(args, "string_references");
    if (idx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_MODULE_INDEX));
    if (!refs) return make_error(vbmcp_t_utf8(T_ERR_MISSING_STRING_REFERENCES));

    vb_set_value(VB_SetModuleStringReferences, refs, (int)idx, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_int(root, "module_index", idx);
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 13: list_module_functions --------
 * 列出模块内的所有函数/子例程。
 * 参数：module_index (int)
 * 返回：[{name, address}, ...]
 */
static jv_value_t *tool_list_module_functions(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long idx = (long long)jv_obj_get_number(args, "module_index", -1);
    if (idx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_MODULE_INDEX));

    long long limit = 100, offset = 0;
    limit  = (long long)jv_obj_get_number(args, "limit",  100);
    offset = (long long)jv_obj_get_number(args, "offset",   0);
    if (limit  < 1)   limit  = 1;
    if (limit  > 10000) limit = 10000;
    if (offset < 0)   offset = 0;

    char *count_str = vb_get_value(VB_GetModuleFunctionCount, (int)idx, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbmcp_free(count_str);

    jv_value_t *root = make_object();
    if (!root) return make_error(vbmcp_t_utf8(T_ERR_OOM));
    add_int(root, "module_index", idx);

    jv_value_t *arr = make_array();
    if (!arr) { free(root); return make_error(vbmcp_t_utf8(T_ERR_OOM)); }

    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *name = vb_get_value(VB_GetModuleFunctionName, (int)idx, (int)i);
        char *addr = vb_get_value(VB_GetModuleFunctionAddress, (int)idx, (int)i);

        jv_value_t *item = make_object();
        if (!item) { if (name) vbmcp_free(name); if (addr) vbmcp_free(addr); break; }
        add_str(item, "name", name ? name : "");
        add_str(item, "address", addr ? addr : "");
        add_int(item, "index", i);

        if (name) vbmcp_free(name);
        if (addr) vbmcp_free(addr);

        arr_push(arr, item);
    }

    apply_pagination(root, "functions", arr, total, offset, limit);
    return root;
}

/* -------- 工具 14: get_function --------
 * 获取函数反编译代码。
 * 参数：module_index, function_index
 */
static jv_value_t *tool_get_function(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long midx = (long long)jv_obj_get_number(args, "module_index", -1);
    long long fidx = (long long)jv_obj_get_number(args, "function_index", -1);
    if (midx < 0 || fidx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_MODULE_FUNCTION_INDEX));

    char *name = vb_get_value(VB_GetModuleFunctionName, (int)midx, (int)fidx);
    char *addr = vb_get_value(VB_GetModuleFunctionAddress, (int)midx, (int)fidx);
    char *code = vb_get_value(VB_GetModuleFunction, (int)midx, (int)fidx);
    char *code_fast = vb_get_value(VB_GetModuleFunctionCode, (int)midx, (int)fidx);
    char *str_ref = vb_get_value(VB_GetModuleFunctionStrRef, (int)midx, (int)fidx);
    char *disasm = vb_get_value(VB_GetModuleFunctionDisasm, (int)midx, (int)fidx);

    jv_value_t *root = make_object();
    add_int(root, "module_index", midx);
    add_int(root, "function_index", fidx);
    add_str(root, "name", name ? name : "");
    add_str(root, "address", addr ? addr : "");
    add_str(root, "code", code ? code : "");
    add_str(root, "code_fast", code_fast ? code_fast : "");
    add_str(root, "string_references", str_ref ? str_ref : "");
    add_str(root, "disassembly", disasm ? disasm : "");

    if (name) vbmcp_free(name);
    if (addr) vbmcp_free(addr);
    if (code) vbmcp_free(code);
    if (code_fast) vbmcp_free(code_fast);
    if (str_ref) vbmcp_free(str_ref);
    if (disasm) vbmcp_free(disasm);
    return root;
}

/* -------- 工具 15: set_function --------
 * 替换函数代码。
 */
static jv_value_t *tool_set_function(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long midx = (long long)jv_obj_get_number(args, "module_index", -1);
    long long fidx = (long long)jv_obj_get_number(args, "function_index", -1);
    const char *code = jv_obj_get_string(args, "code");
    if (midx < 0 || fidx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_MODULE_FUNCTION_INDEX));
    if (!code) return make_error(vbmcp_t_utf8(T_ERR_MISSING_CODE));

    vb_set_value(VB_SetModuleFunction, code, (int)midx, (int)fidx);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_int(root, "module_index", midx);
    add_int(root, "function_index", fidx);
    add_int(root, "length", (long long)strlen(code));
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 16: rename_function --------
 */
static jv_value_t *tool_rename_function(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long midx = (long long)jv_obj_get_number(args, "module_index", -1);
    long long fidx = (long long)jv_obj_get_number(args, "function_index", -1);
    const char *new_name = jv_obj_get_string(args, "new_name");
    if (midx < 0 || fidx < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_MODULE_FUNCTION_INDEX));
    if (!new_name) return make_error(vbmcp_t_utf8(T_ERR_MISSING_NEW_NAME));

    /* Native 守护（防止 modal 弹窗阻塞）： */
    if (is_native_project()) {
        return make_error(vbmcp_t_utf8(T_ERR_NATIVE_RENAME_FUNCTION));
    }

    vb_set_value(VB_SetModuleFunctionName, new_name, (int)midx, (int)fidx);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_int(root, "module_index", midx);
    add_int(root, "function_index", fidx);
    add_str(root, "new_name", new_name);
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 17: get_sub_main --------
 * 获取程序的 Sub Main 入口。
 */
static jv_value_t *tool_get_sub_main(const jv_value_t *args)
{
    (void)args;
    char *code = vb_get_value(VB_GetSubMain, 0, 0);
    if (!code) return wrap_field("code", "");
    jv_value_t *r = wrap_field("code", code);
    vbmcp_free(code);
    return r;
}

/* -------- 工具 18: set_sub_main --------
 */
static jv_value_t *tool_set_sub_main(const jv_value_t *args)
{
    const char *code = (args && args->type == JV_OBJECT) ? jv_obj_get_string(args, "code") : NULL;
    if (!code) return make_error(vbmcp_t_utf8(T_ERR_MISSING_CODE));

    vb_set_value(VB_SetSubMain, code, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_str(root, "status", "ok");
    add_int(root, "length", (long long)strlen(code));
    return root;
}

/* -------- 工具 19: get_active_text --------
 * 获取当前激活窗口（代码编辑器）的文本。
 */
static jv_value_t *tool_get_active_text(const jv_value_t *args)
{
    (void)args;
    char *text = vb_get_value(VB_GetActiveText, 0, 0);
    jv_value_t *r = wrap_field("text", text ? text : "");
    if (text) vbmcp_free(text);
    return r;
}

/* -------- 工具 20: set_active_text --------
 * 设置当前激活窗口的文本。
 */
static jv_value_t *tool_set_active_text(const jv_value_t *args)
{
    const char *text = (args && args->type == JV_OBJECT) ? jv_obj_get_string(args, "text") : NULL;
    if (!text) return make_error(vbmcp_t_utf8(T_ERR_MISSING_TEXT));
    vb_set_value(VB_SetActiveText, text, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_str(root, "status", "ok");
    add_int(root, "length", (long long)strlen(text));
    return root;
}

/* -------- 工具 21: get_active_disasm_text --------
 * 获取当前激活反汇编窗口的原始文本。
 */
static jv_value_t *tool_get_active_disasm_text(const jv_value_t *args)
{
    (void)args;
    char *text = vb_get_value(VB_GetActiveDisasmText, 0, 0);
    jv_value_t *r = wrap_field("text", text ? text : "");
    if (text) vbmcp_free(text);
    return r;
}

/* -------- 工具 22: set_active_disasm_text --------
 */
static jv_value_t *tool_set_active_disasm_text(const jv_value_t *args)
{
    const char *text = (args && args->type == JV_OBJECT) ? jv_obj_get_string(args, "text") : NULL;
    if (!text) return make_error(vbmcp_t_utf8(T_ERR_MISSING_TEXT));
    vb_set_value(VB_SetActiveDisasmText, text, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 23: set_active_text_line --------
 * 跳到当前激活窗口的指定行。
 */
static jv_value_t *tool_set_active_text_line(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    long long line = (long long)jv_obj_get_number(args, "line", -1);
    if (line < 0) return make_error(vbmcp_t_utf8(T_ERR_BAD_LINE));

    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", line);
    vb_set_value(VB_SetActiveTextLine, buf, 0, 0);

    jv_value_t *root = make_object();
    add_int(root, "line", line);
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 24: get_active_module_coordinats --------
 * 获取当前激活视图的模块/函数坐标。
 */
static jv_value_t *tool_get_active_module_coordinats(const jv_value_t *args)
{
    (void)args;
    char *s = vb_get_value(VB_GetActiveModuleCoordinats, 0, 0);
    jv_value_t *r = wrap_field("coordinates", s ? s : "");
    if (s) vbmcp_free(s);
    return r;
}

/* -------- 工具 25: set_status_bar_text --------
 * 改变VB Decompiler状态栏文本。
 */
static jv_value_t *tool_set_status_bar_text(const jv_value_t *args)
{
    const char *text = (args && args->type == JV_OBJECT) ? jv_obj_get_string(args, "text") : NULL;
    if (!text) return make_error(vbmcp_t_utf8(T_ERR_MISSING_TEXT));
    vb_set_status(text);

    jv_value_t *root = make_object();
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 26: set_options --------
 * 设置"解析栈参数"和"过程分析器/优化器"复选框状态。
 */
static jv_value_t *tool_set_options(const jv_value_t *args)
{
    if (!args || args->type != JV_OBJECT) return make_error(vbmcp_t_utf8(T_ERR_MISSING_ARGS));
    const char *stack = jv_obj_get_string(args, "stack_check");
    const char *analyzer = jv_obj_get_string(args, "analyzer_check");
    if (stack) vb_set_value(VB_SetStackCheckBoxValue, stack, 0, 0);
    if (analyzer) vb_set_value(VB_SetAnalyzerCheckBoxValue, analyzer, 0, 0);
    vb_set_value(VB_UpdateAll, "", 0, 0);

    jv_value_t *root = make_object();
    if (stack) add_str(root, "stack_check", stack);
    if (analyzer) add_str(root, "analyzer_check", analyzer);
    add_str(root, "status", "ok");
    return root;
}

/* -------- 工具 27: list_frx_icons --------
 * 列出 FRX 中的图标/图像资源。
 */
static jv_value_t *tool_list_frx_icons(const jv_value_t *args)
{
    long long limit = 100, offset = 0;
    if (args && args->type == JV_OBJECT) {
        limit  = (long long)jv_obj_get_number(args, "limit",  100);
        offset = (long long)jv_obj_get_number(args, "offset",   0);
        if (limit  < 1)   limit  = 1;
        if (limit  > 10000) limit = 10000;
        if (offset < 0)   offset = 0;
    }

    char *count_str = vb_get_value(VB_GetFrxIconCount, 0, 0);
    long long total = safe_parse_int(count_str, 0);
    if (count_str) vbmcp_free(count_str);

    jv_value_t *root = make_object();
    if (!root) return make_error(vbmcp_t_utf8(T_ERR_OOM));

    jv_value_t *arr = make_array();
    if (!arr) { free(root); return make_error(vbmcp_t_utf8(T_ERR_OOM)); }

    long long end = offset + limit;
    if (end > total) end = total;
    for (long long i = offset; i < end; i++) {
        char *off = vb_get_value(VB_GetFrxIconOffset, (int)i, 0);
        char *size = vb_get_value(VB_GetFrxIconSize, (int)i, 0);

        jv_value_t *item = make_object();
        if (!item) { if (off) vbmcp_free(off); if (size) vbmcp_free(size); break; }
        add_int(item, "index", i);
        add_str(item, "offset", off ? off : "");
        add_str(item, "size", size ? size : "");
        if (off) vbmcp_free(off);
        if (size) vbmcp_free(size);

        arr_push(arr, item);
    }

    apply_pagination(root, "icons", arr, total, offset, limit);
    return root;
}

/* -------- 工具 28: set_module_string_references (已实现见上，这里导入) --*/

/* -------- 工具 29: update_all --------
 * 等同于内部 UpdateAll 命令：提交所有修改并刷新 GUI。
 */
static jv_value_t *tool_update_all(const jv_value_t *args)
{
    (void)args;
    vb_set_value(VB_UpdateAll, "", 0, 0);
    jv_value_t *root = make_object();
    add_str(root, "status", "ok");
    return root;
}

/* ============================================================
 * 工具分发
 * ============================================================ */

typedef struct {
    const char *name;
    jv_value_t *(*impl)(const jv_value_t *args);
} tool_entry_t;

static const tool_entry_t g_tools[] = {
    { "vb_get_project_info",           tool_get_project_info },
    { "vb_get_project_text",           tool_get_project },
    { "vb_set_project_text",           tool_set_project },
    { "vb_clear_buffers",              tool_clear_buffers },
    { "vb_list_forms",                 tool_list_forms },
    { "vb_get_form",                   tool_get_form },
    { "vb_set_form",                   tool_set_form },
    { "vb_rename_form",                tool_rename_form },
    { "vb_list_modules",               tool_list_modules },
    { "vb_get_module",                 tool_get_module },
    { "vb_set_module",                 tool_set_module },
    { "vb_list_module_functions",      tool_list_module_functions },
    { "vb_get_function",               tool_get_function },
    { "vb_set_function",               tool_set_function },
    { "vb_rename_function",            tool_rename_function },
    { "vb_get_sub_main",               tool_get_sub_main },
    { "vb_set_sub_main",               tool_set_sub_main },
    { "vb_get_active_text",            tool_get_active_text },
    { "vb_set_active_text",            tool_set_active_text },
    { "vb_get_active_disasm_text",     tool_get_active_disasm_text },
    { "vb_set_active_disasm_text",     tool_set_active_disasm_text },
    { "vb_goto_line",                  tool_set_active_text_line },
    { "vb_get_active_coords",          tool_get_active_module_coordinats },
    { "vb_set_status",                 tool_set_status_bar_text },
    { "vb_set_options",                tool_set_options },
    { "vb_list_frx_icons",             tool_list_frx_icons },
    { "vb_update_all",                 tool_update_all },
    { NULL, NULL }
};

jv_value_t *mcp_dispatch_tool(const char *tool_name, const jv_value_t *args)
{
    if (!tool_name) return make_error(vbmcp_t_utf8(T_ERR_EMPTY_TOOL_NAME));
    for (int i = 0; g_tools[i].name; i++) {
        if (vb_streq(tool_name, g_tools[i].name)) {
            /* 集中项目加载检查：除了探测/控制类工具，其他工具调用前
             * 必须确保 VB Decompiler 已加载项目。
             *
             * SDK 不暴露 "ReloadFile / RefreshEngine / ReInitPlugin" 等 API，
             * 触发 VBDecompilerPluginLoad 重入（重新赋值 g_engine）的**唯一**
             * 路径是用户**点 Plugins 菜单项**（VB Decompiler 私有菜单系统，
             * vbmcp 无法模拟）。
             *
             * "Plugins 菜单 → 配置窗口 → 点确定" 这个完整流程中：
             *   - 点菜单项 → PluginLoad 重入（g_engine 重新有效）
             *   - 点确定 → 只启停服务器（不动 g_engine）
             *
             * 返回错误 + 解决方法，让用户立刻知道怎么恢复，而不是面对一片
             * 空数据困惑。 */
            static const char *k_no_project_check[] = {
                "vb_get_project_info",   /* 本身就在探测项目状态 */
                "vb_set_status",
                "vb_set_options",
                "vb_update_all",
                "vb_clear_buffers",
                NULL
            };
            int needs_check = 1;
            for (int j = 0; k_no_project_check[j]; j++) {
                if (vb_streq(tool_name, k_no_project_check[j])) {
                    needs_check = 0;
                    break;
                }
            }
            if (needs_check && !vb_is_project_loaded()) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                    vbmcp_t_utf8(T_ERR_PROJECT_NOT_LOADED),
                    tool_name);
                return make_error(buf);
            }
            return g_tools[i].impl(args);
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), vbmcp_t_utf8(T_ERR_UNKNOWN_TOOL), tool_name);
    return make_error(buf);
}

/* ============================================================
 * tools/list schema（这是 MCP 协议要求客户端获取的工具元数据）
 * ============================================================ */

/* append_schema_tool —— 写单个 tool 的完整 JSON Schema + annotations。
 *
 * properties_json: 必须是合法的 JSON 对象字符串（"{}" 或 "{...}"），不带外层 {}。
 *                  内部会原样拼接进 inputSchema.properties。
 *
 * annotations (MCP 2025-03 规范):
 *   - readOnlyHint:    true  = 工具不会修改任何状态
 *   - destructiveHint: true  = 工具执行不可逆修改（默认；只在 readOnlyHint=false 时相关）
 *   - idempotentHint:  true  = 同名同参多次调用效果一致
 *   - openWorldHint:   true  = 工具与"外部实体"交互（vbmcp 全是本地，无）
 *
 * name/description 走 sb_append_json_string()，因为部分描述（如 vb_set_options
 * 里的 "Parse stack parameters"）内嵌 ASCII 双引号，必须 JSON 转义；
 * 控制字符、UTF-8 高位字符同样由 sb_append_json_string 正确处理。 */
static void append_schema_tool(strbuf_t *out,
                               const char *name, const char *desc,
                               const char *properties_json,
                               int read_only, int destructive,
                               int idempotent, int open_world)
{
    sb_append_char(out, '{');
    sb_append(out, "\"name\":", 7);
    sb_append_json_string(out, name);
    sb_append(out, ",\"description\":", 15);
    sb_append_json_string(out, desc);
    sb_append_str(out, ",\"inputSchema\":{\"type\":\"object\",\"properties\":");
    sb_append_str(out, properties_json);
    sb_append_str(out, ",\"additionalProperties\":false}");
    /* annotations —— 提示客户端（模型/UI）工具的副作用 */
    sb_append_str(out, ",\"annotations\":{");
    sb_append_str(out, "\"readOnlyHint\":");     sb_append_str(out, read_only   ? "true" : "false");
    sb_append_str(out, ",\"destructiveHint\":"); sb_append_str(out, destructive ? "true" : "false");
    sb_append_str(out, ",\"idempotentHint\":");  sb_append_str(out, idempotent  ? "true" : "false");
    sb_append_str(out, ",\"openWorldHint\":");   sb_append_str(out, open_world  ? "true" : "false");
    sb_append_char(out, '}');
    sb_append_char(out, '}');
}

/* ============================================================
 * properties 片段构造（description 按当前语言，UTF-8）
 *
 * append_schema_tool() 把 properties_json 原样拼进 inputSchema.properties
 * （不做 JSON 转义），因此这些函数输出的片段必须是合法 JSON；description
 * 文案经 vbmcp_t_utf8() 取当前语言的 UTF-8 串（与 application/json 一致），
 * 且自身不含 JSON 需转义的字符（引号/反斜杠/控制字符）。
 *
 * 线程安全：tools/list 与工具调用都在 VB Decompiler 主线程
 * （WSAAsyncSelect 消息循环）内同步完成，静态缓冲无并发写问题。
 * ============================================================ */

/* 分页参数片段（不含外层 {}，由调用方包 {}）。limit 默认 100，最大 10000。 */
static const char *json_limit_offset(void)
{
    static char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000,\"default\":100,\"description\":\"%s\"},"
        "\"offset\":{\"type\":\"integer\",\"minimum\":0,\"default\":0,\"description\":\"%s\"}",
        vbmcp_t_utf8(T_DESC_LIMIT), vbmcp_t_utf8(T_DESC_OFFSET));
    return buf;
}

/* "{" + json_limit_offset() + "}"。内层写另一静态缓冲，无别名覆盖。 */
static const char *json_paging(void)
{
    static char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{%s}", json_limit_offset());
    return buf;
}

/* module_index + 分页（vb_list_module_functions 用）。 */
static const char *json_module_index_paging(void)
{
    static char buf[640];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\"module_index\":{\"type\":\"integer\",\"minimum\":0},%s}",
        json_limit_offset());
    return buf;
}

/* form_index 参数（vb_get_form 用）。 */
static const char *json_form_index(void)
{
    static char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\"form_index\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"%s\"}}",
        vbmcp_t_utf8(T_DESC_PARAM_FORM_INDEX));
    return buf;
}

/* project_text 参数（vb_set_project_text 用）。 */
static const char *json_project_text_param(void)
{
    static char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\"project_text\":{\"type\":\"string\",\"description\":\"%s\"}}",
        vbmcp_t_utf8(T_DESC_PARAM_PROJECT_TEXT));
    return buf;
}

/* line 参数（vb_goto_line 用）。 */
static const char *json_line_param(void)
{
    static char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\"line\":{\"type\":\"integer\",\"minimum\":0,\"description\":\"%s\"}}",
        vbmcp_t_utf8(T_DESC_PARAM_LINE));
    return buf;
}

/* stack_check / analyzer_check 参数（vb_set_options 用）。 */
static const char *json_options_params(void)
{
    static char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\"stack_check\":{\"type\":\"string\",\"enum\":[\"0\",\"1\"],\"description\":\"%s\"},"
        "\"analyzer_check\":{\"type\":\"string\",\"enum\":[\"0\",\"1\"],\"description\":\"%s\"}}",
        vbmcp_t_utf8(T_DESC_PARAM_ONOFF), vbmcp_t_utf8(T_DESC_PARAM_ONOFF));
    return buf;
}

/* 把所有本地工具逐项追加到 strbuf（不带外层 {"tools":[...]} 包装）。
 * 用于 mcp_build_tools_schema()（本地单独 tools/list）。
 *
 * 命名规范：所有工具统一加 "vb_" 前缀，避免与其他 MCP 服务器（iida 等）
 * 的同名工具冲突（推荐见 MCP Best Practices）。
 *
 * annotations 规则（MCP 2025-03）：
 *   readOnly=true:    get_*, list_*
 *   destructive=true: 不可逆修改（set_project/set_form/set_module/set_function/set_sub_main/clear_buffers）
 *   idempotent=true:  全部（同名同参多次调用结果一致）
 *   openWorld=false:  全部（vbmcp 是本地插件，无外部交互） */
static void append_local_tools(strbuf_t *b)
{
    append_schema_tool(b, "vb_get_project_info",
        vbmcp_t_utf8(T_DESC_PROJECT_INFO),
        "{}", 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_project_text",
        vbmcp_t_utf8(T_DESC_PROJECT_TEXT),
        "{}", 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_project_text",
        vbmcp_t_utf8(T_DESC_SET_PROJECT_TEXT),
        json_project_text_param(),
        0, 1, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_clear_buffers",
        vbmcp_t_utf8(T_DESC_CLEAR_BUFFERS),
        "{}", 0, 1, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_list_forms",
        vbmcp_t_utf8(T_DESC_LIST_FORMS),
        json_paging(), 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_form",
        vbmcp_t_utf8(T_DESC_GET_FORM),
        json_form_index(),
        1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_form",
        vbmcp_t_utf8(T_DESC_SET_FORM),
        "{\"form_index\":{\"type\":\"integer\",\"minimum\":0},\"form_text\":{\"type\":\"string\"}}",
        0, 1, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_rename_form",
        vbmcp_t_utf8(T_DESC_RENAME_FORM),
        "{\"form_index\":{\"type\":\"integer\",\"minimum\":0},\"new_name\":{\"type\":\"string\",\"minLength\":1}}",
        0, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_list_modules",
        vbmcp_t_utf8(T_DESC_LIST_MODULES),
        json_paging(), 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_module",
        vbmcp_t_utf8(T_DESC_GET_MODULE),
        "{\"module_index\":{\"type\":\"integer\",\"minimum\":0}}",
        1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_module",
        vbmcp_t_utf8(T_DESC_SET_MODULE),
        "{\"module_index\":{\"type\":\"integer\",\"minimum\":0},\"declarations\":{\"type\":\"string\"}}",
        0, 1, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_list_module_functions",
        vbmcp_t_utf8(T_DESC_LIST_MODULE_FUNCTIONS),
        json_module_index_paging(),
        1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_function",
        vbmcp_t_utf8(T_DESC_GET_FUNCTION),
        "{\"module_index\":{\"type\":\"integer\",\"minimum\":0},\"function_index\":{\"type\":\"integer\",\"minimum\":0}}",
        1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_function",
        vbmcp_t_utf8(T_DESC_SET_FUNCTION),
        "{\"module_index\":{\"type\":\"integer\",\"minimum\":0},\"function_index\":{\"type\":\"integer\",\"minimum\":0},\"code\":{\"type\":\"string\"}}",
        0, 1, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_rename_function",
        vbmcp_t_utf8(T_DESC_RENAME_FUNCTION),
        "{\"module_index\":{\"type\":\"integer\",\"minimum\":0},\"function_index\":{\"type\":\"integer\",\"minimum\":0},\"new_name\":{\"type\":\"string\",\"minLength\":1}}",
        0, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_sub_main",
        vbmcp_t_utf8(T_DESC_GET_SUB_MAIN),
        "{}", 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_sub_main",
        vbmcp_t_utf8(T_DESC_SET_SUB_MAIN),
        "{\"code\":{\"type\":\"string\"}}",
        0, 1, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_active_text",
        vbmcp_t_utf8(T_DESC_GET_ACTIVE_TEXT),
        "{}", 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_active_text",
        vbmcp_t_utf8(T_DESC_SET_ACTIVE_TEXT),
        "{\"text\":{\"type\":\"string\"}}",
        0, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_active_disasm_text",
        vbmcp_t_utf8(T_DESC_GET_ACTIVE_DISASM_TEXT),
        "{}", 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_active_disasm_text",
        vbmcp_t_utf8(T_DESC_SET_ACTIVE_DISASM_TEXT),
        "{\"text\":{\"type\":\"string\"}}",
        0, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_goto_line",
        vbmcp_t_utf8(T_DESC_GOTO_LINE),
        json_line_param(),
        0, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_get_active_coords",
        vbmcp_t_utf8(T_DESC_GET_ACTIVE_COORDS),
        "{}", 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_status",
        vbmcp_t_utf8(T_DESC_SET_STATUS),
        "{\"text\":{\"type\":\"string\"}}",
        0, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_set_options",
        vbmcp_t_utf8(T_DESC_SET_OPTIONS),
        json_options_params(),
        0, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_list_frx_icons",
        vbmcp_t_utf8(T_DESC_LIST_FRX_ICONS),
        json_paging(), 1, 0, 1, 0);
    sb_append_char(b, ',');

    append_schema_tool(b, "vb_update_all",
        vbmcp_t_utf8(T_DESC_UPDATE_ALL),
        "{}", 0, 0, 1, 0);
}

/* 仅本地工具的完整 schema（含 {"tools":[...]} 包装）。
 * 保留旧接口名以兼容潜在外部调用方；新代码应优先用 mcp_build_local_tools_array()。 */
char *mcp_build_tools_schema(void)
{
    strbuf_t b;
    sb_init(&b);
    sb_append_str(&b, "{\"tools\":[");
    append_local_tools(&b);
    sb_append_str(&b, "]}");
    sb_finalize(&b);
    return b.buf;
}

/* 仅本地工具的 JSON 数组字符串（"[{...},{...}]"，new alloc）。
 * mcp_rpc 层会把它与上游工具数组合并形成完整 tools/list 响应。
 * 末尾不带 ','，适合直接拼接上游数组。 */
char *mcp_build_local_tools_array(void)
{
    strbuf_t b;
    sb_init(&b);
    sb_append_char(&b, '[');
    append_local_tools(&b);
    sb_append_char(&b, ']');
    sb_finalize(&b);
    return b.buf;
}

/* 本地工具总数（数 g_tools sentinel-终止数组）。用于 /health 端点。 */
int mcp_local_tool_count(void)
{
    int n = 0;
    while (g_tools[n].name) n++;
    return n;
}
