/*
 * json.h - 极简 JSON 解析器（够 MCP 请求/响应使用）
 *
 * 仅支持解析与构建。不做深拷贝，引用原始字符串。
 */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "strbuf.h"

/* JSON 值类型 */
typedef enum {
    JV_NULL,
    JV_BOOL,
    JV_NUMBER,
    JV_STRING,
    JV_ARRAY,
    JV_OBJECT,
    JV_INVALID
} jv_type;

typedef struct jv_value jv_value_t;
typedef struct jv_member {
    char *key;             /* 复制出来的键名 */
    jv_value_t *value;
    struct jv_member *next;
} jv_member_t;

struct jv_value {
    jv_type type;
    union {
        int b;
        double n;
        char *s;          /* 复制出来的字符串 */
        struct {
            jv_value_t **items;
            size_t count;
            size_t cap;
        } a;
        jv_member_t *members;  /* 单链表（保持插入顺序） */
    } u;
};

/* 解析（失败返回 NULL） */
jv_value_t *jv_parse(const char *text);
void jv_free(jv_value_t *v);

/* 深拷贝：递归复制节点（含字符串 + 数组元素 + 对象成员）。
 * 返回的树需用 jv_free 释放。用于避免 dump+parse 的 O(N) 序列化反序列化。
 * 失败返回 NULL（OOM）。 */
jv_value_t *jv_deep_copy(const jv_value_t *v);

/* 访问 */
jv_type jv_typeof(const jv_value_t *v);
const char *jv_get_string(const jv_value_t *v);  /* JV_STRING only */
double jv_get_number(const jv_value_t *v, double def);
int jv_get_bool(const jv_value_t *v, int def);

/* 对象：取成员 */
jv_value_t *jv_obj_get(const jv_value_t *v, const char *key);
const char *jv_obj_get_string(const jv_value_t *v, const char *key);
double jv_obj_get_number(const jv_value_t *v, const char *key, double def);
int jv_obj_get_bool(const jv_value_t *v, const char *key, int def);

/* 数组遍历 */
size_t jv_array_len(const jv_value_t *v);
jv_value_t *jv_array_at(const jv_value_t *v, size_t i);

/* 序列化（写入 strbuf_t） */
void jv_dump(const jv_value_t *v, strbuf_t *out);

/* 高层辅助：构造常见 JSON 结构（追加到 strbuf） */
void sb_json_null(strbuf_t *b);
void sb_json_bool(strbuf_t *b, int v);
void sb_json_int(strbuf_t *b, long long v);
void sb_json_str(strbuf_t *b, const char *s);

#endif /* JSON_H */
