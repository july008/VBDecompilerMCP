/*
 * json.c - 极简 JSON 解析器与序列化
 *
 * 递归下降解析。支持 null/true/false/数字/字符串/数组/对象。
 * 字符串/键名会被复制，其他引用源文本（解析失败/类型错误返回 NULL）。
 */
#include "json.h"
#include "vbmcp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 序列化 ---------- */

static void dump_value(const jv_value_t *v, strbuf_t *out);

static void dump_string(strbuf_t *out, const char *s)
{
    sb_append_json_string(out, s ? s : "");
}

static void dump_member(const jv_member_t *m, strbuf_t *out)
{
    dump_string(out, m->key);
    sb_append_char(out, ':');
    dump_value(m->value, out);
}

static void dump_value(const jv_value_t *v, strbuf_t *out)
{
    if (!v) { sb_append(out, "null", 4); return; }
    switch (v->type) {
    case JV_NULL: sb_append(out, "null", 4); break;
    case JV_BOOL: sb_append(out, v->u.b ? "true" : "false", v->u.b ? 4 : 5); break;
    case JV_NUMBER: {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%g", v->u.n);
        sb_append(out, tmp, (size_t)n);
        break;
    }
    case JV_STRING:
        dump_string(out, v->u.s);
        break;
    case JV_ARRAY:
        sb_append_char(out, '[');
        for (size_t i = 0; i < v->u.a.count; i++) {
            if (i > 0) sb_append_char(out, ',');
            dump_value(v->u.a.items[i], out);
        }
        sb_append_char(out, ']');
        break;
    case JV_OBJECT:
        sb_append_char(out, '{');
        {
            int first = 1;
            for (jv_member_t *m = v->u.members; m; m = m->next) {
                if (!first) sb_append_char(out, ',');
                first = 0;
                dump_member(m, out);
            }
        }
        sb_append_char(out, '}');
        break;
    case JV_INVALID:
    default:
        sb_append(out, "null", 4);
        break;
    }
}

void jv_dump(const jv_value_t *v, strbuf_t *out)
{
    dump_value(v, out);
}

void sb_json_null(strbuf_t *b) { sb_append(b, "null", 4); }
void sb_json_bool(strbuf_t *b, int v) { sb_append(b, v ? "true" : "false", v ? 4 : 5); }
void sb_json_int(strbuf_t *b, long long v) { sb_append_int(b, v); }
void sb_json_str(strbuf_t *b, const char *s) { sb_append_json_string(b, s); }

/* ---------- 释放 ---------- */

static void free_value(jv_value_t *v)
{
    if (!v) return;
    switch (v->type) {
    case JV_STRING:
        free(v->u.s);
        break;
    case JV_ARRAY:
        for (size_t i = 0; i < v->u.a.count; i++) free_value(v->u.a.items[i]);
        free(v->u.a.items);
        break;
    case JV_OBJECT: {
        jv_member_t *m = v->u.members;
        while (m) {
            jv_member_t *next = m->next;
            free(m->key);
            free_value(m->value);
            free(m);
            m = next;
        }
        break;
    }
    default:
        break;
    }
    free(v);
}

void jv_free(jv_value_t *v)
{
    free_value(v);
}

/* ============================================================
 * 深拷贝：节点遍历复制，O(N) 但常数项远小于 dump+parse
 * （dump 是字符串拼接 + 编码转义，parse 是字符扫描 + 状态机）
 * ============================================================ */
static jv_value_t *copy_value(const jv_value_t *v)
{
    if (!v) return NULL;
    jv_value_t *o = (jv_value_t *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->type = v->type;
    switch (v->type) {
    case JV_NULL:
    case JV_BOOL:
    case JV_NUMBER:
        /* 标量直接位拷贝（calloc 已 0，BOOL/NUMBER union 同一槽位） */
        o->u = v->u;
        break;
    case JV_STRING:
        o->u.s = vbmcp_strdup(v->u.s ? v->u.s : "");
        if (!o->u.s) { free(o); return NULL; }
        break;
    case JV_ARRAY: {
        size_t n_items = v->u.a.count;
        if (n_items > 0) {
            o->u.a.items = (jv_value_t **)calloc(n_items, sizeof(jv_value_t *));
            if (!o->u.a.items) { free(o); return NULL; }
            o->u.a.cap = n_items;
            for (size_t i = 0; i < n_items; i++) {
                o->u.a.items[i] = copy_value(v->u.a.items[i]);
                if (!o->u.a.items[i]) {
                    /* 失败回滚：之前已分配的 items 不算泄漏（jv_free 会清空 a.count） */
                    o->u.a.count = i;
                    jv_free(o);
                    return NULL;
                }
            }
        }
        o->u.a.count = n_items;
        break;
    }
    case JV_OBJECT: {
        /* 复制整条成员链表（保持插入顺序） */
        jv_member_t **tail = &o->u.members;
        for (jv_member_t *m = v->u.members; m; m = m->next) {
            jv_member_t *nm = (jv_member_t *)calloc(1, sizeof(*nm));
            if (!nm) { jv_free(o); return NULL; }
            nm->key = vbmcp_strdup(m->key ? m->key : "");
            if (!nm->key) { free(nm); jv_free(o); return NULL; }
            nm->value = copy_value(m->value);
            if (!nm->value) { free(nm->key); free(nm); jv_free(o); return NULL; }
            *tail = nm;
            tail = &nm->next;
        }
        break;
    }
    default:
        /* JV_INVALID 或未知：保留类型字段为空 union */
        break;
    }
    return o;
}

jv_value_t *jv_deep_copy(const jv_value_t *v)
{
    return copy_value(v);
}

/* ---------- 访问 ---------- */

jv_type jv_typeof(const jv_value_t *v) { return v ? v->type : JV_NULL; }

const char *jv_get_string(const jv_value_t *v)
{
    return (v && v->type == JV_STRING) ? v->u.s : NULL;
}

double jv_get_number(const jv_value_t *v, double def)
{
    if (!v || v->type != JV_NUMBER) return def;
    return v->u.n;
}

int jv_get_bool(const jv_value_t *v, int def)
{
    if (!v || v->type != JV_BOOL) return def;
    return v->u.b ? 1 : 0;
}

jv_value_t *jv_obj_get(const jv_value_t *v, const char *key)
{
    if (!v || v->type != JV_OBJECT || !key) return NULL;
    for (jv_member_t *m = v->u.members; m; m = m->next) {
        if (m->key && strcmp(m->key, key) == 0) return m->value;
    }
    return NULL;
}

const char *jv_obj_get_string(const jv_value_t *v, const char *key)
{
    jv_value_t *m = jv_obj_get(v, key);
    return jv_get_string(m);
}

double jv_obj_get_number(const jv_value_t *v, const char *key, double def)
{
    jv_value_t *m = jv_obj_get(v, key);
    return jv_get_number(m, def);
}

int jv_obj_get_bool(const jv_value_t *v, const char *key, int def)
{
    jv_value_t *m = jv_obj_get(v, key);
    return jv_get_bool(m, def);
}

size_t jv_array_len(const jv_value_t *v)
{
    return (v && v->type == JV_ARRAY) ? v->u.a.count : 0;
}

jv_value_t *jv_array_at(const jv_value_t *v, size_t i)
{
    if (!v || v->type != JV_ARRAY || i >= v->u.a.count) return NULL;
    return v->u.a.items[i];
}

/* ---------- 解析 ---------- */

typedef struct {
    const char *src;
    const char *p;
    const char *end;
} parser_t;

static void skip_ws(parser_t *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->p++;
        else break;
    }
}

static jv_value_t *parse_value(parser_t *ps);

static char *parse_string_raw(parser_t *ps)
{
    /* ps->p 位置在 '"' */
    if (ps->p >= ps->end || *ps->p != '"') return NULL;
    ps->p++;
    strbuf_t b;
    sb_init(&b);
    while (ps->p < ps->end && *ps->p != '"') {
        char c = *ps->p;
        if (c == '\\') {
            ps->p++;
            if (ps->p >= ps->end) break;
            char esc = *ps->p++;
            switch (esc) {
            case '"': sb_append_char(&b, '"'); break;
            case '\\': sb_append_char(&b, '\\'); break;
            case '/': sb_append_char(&b, '/'); break;
            case 'b': sb_append_char(&b, '\b'); break;
            case 'f': sb_append_char(&b, '\f'); break;
            case 'n': sb_append_char(&b, '\n'); break;
            case 'r': sb_append_char(&b, '\r'); break;
            case 't': sb_append_char(&b, '\t'); break;
            case 'u': {
                /* 简化：不完整支持 \u 转义（按UTF-8原样写入低位字节） */
                if (ps->end - ps->p >= 4) {
                    char hex[5] = {0};
                    memcpy(hex, ps->p, 4);
                    unsigned int cp = 0;
                    sscanf(hex, "%x", &cp);
                    ps->p += 4;
                    if (cp < 0x80) {
                        sb_append_char(&b, (char)cp);
                    } else if (cp < 0x800) {
                        sb_append_char(&b, (char)(0xC0 | (cp >> 6)));
                        sb_append_char(&b, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        sb_append_char(&b, (char)(0xE0 | (cp >> 12)));
                        sb_append_char(&b, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        sb_append_char(&b, (char)(0x80 | (cp & 0x3F)));
                    }
                }
                break;
            }
            default: sb_append_char(&b, esc); break;
            }
        } else {
            sb_append_char(&b, c);
            ps->p++;
        }
    }
    if (ps->p < ps->end && *ps->p == '"') ps->p++;
    sb_finalize(&b);
    return b.buf; /* 所有权转移 */
}

static jv_value_t *parse_string(parser_t *ps)
{
    char *s = parse_string_raw(ps);
    if (!s) return NULL;
    jv_value_t *v = (jv_value_t *)calloc(1, sizeof(*v));
    if (!v) { free(s); return NULL; }
    v->type = JV_STRING;
    v->u.s = s;
    return v;
}

static jv_value_t *parse_number(parser_t *ps)
{
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    if (ps->p == start) return NULL;
    char tmp[64];
    size_t n = (size_t)(ps->p - start);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    double val = strtod(tmp, NULL);
    jv_value_t *v = (jv_value_t *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->type = JV_NUMBER;
    v->u.n = val;
    return v;
}

static jv_value_t *parse_literal(parser_t *ps, const char *kw, jv_type t)
{
    size_t n = strlen(kw);
    if ((size_t)(ps->end - ps->p) < n) return NULL;
    if (memcmp(ps->p, kw, n) != 0) return NULL;
    ps->p += n;
    jv_value_t *v = (jv_value_t *)calloc(1, sizeof(*v));
    if (!v) return NULL;
    v->type = t;
    if (t == JV_BOOL) v->u.b = (kw[0] == 't');
    return v;
}

static int array_push(jv_value_t *arr, jv_value_t *item)
{
    if (arr->u.a.count == arr->u.a.cap) {
        size_t nc = arr->u.a.cap ? arr->u.a.cap * 2 : 4;
        jv_value_t **ni = (jv_value_t **)calloc(nc, sizeof(jv_value_t *));
        if (!ni) return 0;
        if (arr->u.a.items) memcpy(ni, arr->u.a.items, sizeof(jv_value_t *) * arr->u.a.count);
        free(arr->u.a.items);
        arr->u.a.items = ni;
        arr->u.a.cap = nc;
    }
    arr->u.a.items[arr->u.a.count++] = item;
    return 1;
}

static jv_value_t *parse_array(parser_t *ps)
{
    if (ps->p >= ps->end || *ps->p != '[') return NULL;
    ps->p++;
    jv_value_t *arr = (jv_value_t *)calloc(1, sizeof(*arr));
    if (!arr) return NULL;
    arr->type = JV_ARRAY;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
    while (ps->p < ps->end) {
        skip_ws(ps);
        jv_value_t *item = parse_value(ps);
        if (!item) { free_value(arr); return NULL; }
        if (!array_push(arr, item)) { free_value(item); free_value(arr); return NULL; }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        break;
    }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') ps->p++;
    return arr;
}

static int obj_add(jv_value_t *obj, char *key, jv_value_t *value)
{
    jv_member_t *m = (jv_member_t *)calloc(1, sizeof(*m));
    if (!m) { free(key); free_value(value); return 0; }
    m->key = key;
    m->value = value;
    m->next = NULL;
    if (!obj->u.members) {
        obj->u.members = m;
    } else {
        jv_member_t *tail = obj->u.members;
        while (tail->next) tail = tail->next;
        tail->next = m;
    }
    return 1;
}

static jv_value_t *parse_object(parser_t *ps)
{
    if (ps->p >= ps->end || *ps->p != '{') return NULL;
    ps->p++;
    jv_value_t *obj = (jv_value_t *)calloc(1, sizeof(*obj));
    if (!obj) return NULL;
    obj->type = JV_OBJECT;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
    while (ps->p < ps->end) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') { free_value(obj); return NULL; }
        char *key = parse_string_raw(ps);
        if (!key) { free_value(obj); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); free_value(obj); return NULL; }
        ps->p++;
        skip_ws(ps);
        jv_value_t *val = parse_value(ps);
        if (!val) { free(key); free_value(obj); return NULL; }
        if (!obj_add(obj, key, val)) { free_value(obj); return NULL; }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        break;
    }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') ps->p++;
    return obj;
}

static jv_value_t *parse_value(parser_t *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) return NULL;
    char c = *ps->p;
    if (c == '"') return parse_string(ps);
    if (c == '{') return parse_object(ps);
    if (c == '[') return parse_array(ps);
    if (c == 't') return parse_literal(ps, "true", JV_BOOL);
    if (c == 'f') return parse_literal(ps, "false", JV_BOOL);
    if (c == 'n') return parse_literal(ps, "null", JV_NULL);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
    return NULL;
}

jv_value_t *jv_parse(const char *text)
{
    if (!text) return NULL;
    parser_t ps;
    ps.src = text;
    ps.p = text;
    ps.end = text + strlen(text);
    skip_ws(&ps);
    jv_value_t *v = parse_value(&ps);
    return v;
}
