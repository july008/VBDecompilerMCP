/*
 * strbuf.h - 自实现动态字符串缓冲（C99）
 * 用于组装JSON响应、HTTP头部、调试日志等场景。
 * 与 std::string 的 std::strbuf 不同，这是纯C手写。
 */
#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>
#include <stdarg.h>

typedef struct {
    char *buf;     /* 分配的缓冲区（始终 NUL 终止） */
    size_t len;    /* 已使用的字节数（不含 NUL） */
    size_t cap;    /* 总容量（含 NUL 位置） */
} strbuf_t;

#define STRBUF_INIT { NULL, 0, 0 }

/* 初始化 / 释放 / 重置 */
void sb_init(strbuf_t *b);
void sb_free(strbuf_t *b);
void sb_reset(strbuf_t *b);

/* 保留 capacity 至少为 len+extra+1 */
void sb_reserve(strbuf_t *b, size_t extra);

/* 追加 */
void sb_append(strbuf_t *b, const char *s, size_t n);
void sb_append_str(strbuf_t *b, const char *s);
void sb_append_char(strbuf_t *b, char c);
void sb_append_int(strbuf_t *b, long long v);
void sb_append_uint(strbuf_t *b, unsigned long long v);
void sb_append_double(strbuf_t *b, double v);

/* 追加指定长度的字符串（含二进制） */
void sb_append_raw(strbuf_t *b, const void *data, size_t n);

/* 字符串末尾添加 '\\0'（一般在最后取出指针前调用） */
void sb_finalize(strbuf_t *b);

/* 直接取 c 字符串（确保已 final） */
const char *sb_cstr(const strbuf_t *b);

/* 追加 JSON 字符串：转义 " \ / 控制字符 */
void sb_append_json_string(strbuf_t *b, const char *s);

/* printf 风格 */
void sb_printf(strbuf_t *b, const char *fmt, ...);
void sb_vprintf(strbuf_t *b, const char *fmt, va_list ap);

#endif /* STRBUF_H */
