/*
 * strbuf.c - 自实现动态字符串缓冲
 */
#include "strbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SB_INIT_CAP 64

void sb_init(strbuf_t *b)
{
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_free(strbuf_t *b)
{
    if (!b) return;
    if (b->buf) free(b->buf);
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

void sb_reset(strbuf_t *b)
{
    if (b->buf && b->cap) b->buf[0] = '\0';
    b->len = 0;
}

void sb_reserve(strbuf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap && b->buf) return;
    size_t cap = b->cap ? b->cap : SB_INIT_CAP;
    while (cap < need) cap *= 2;
    char *nb = (char *)malloc(cap);
    if (!nb) return;
    if (b->buf && b->len) memcpy(nb, b->buf, b->len);
    if (b->buf) free(b->buf);
    b->buf = nb;
    b->cap = cap;
    b->buf[b->len] = '\0';
}

void sb_append(strbuf_t *b, const char *s, size_t n)
{
    if (!s || !n) return;
    sb_reserve(b, n);
    if (!b->buf) return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

void sb_append_str(strbuf_t *b, const char *s)
{
    if (!s) return;
    sb_append(b, s, strlen(s));
}

void sb_append_char(strbuf_t *b, char c)
{
    sb_append(b, &c, 1);
}

void sb_append_raw(strbuf_t *b, const void *data, size_t n)
{
    sb_append(b, (const char *)data, n);
}

static void sb_append_unsigned_internal(strbuf_t *b, unsigned long long v, int is_neg, unsigned long long abs_v)
{
    char tmp[32];
    int n = 0;
    if (v == 0) { tmp[n++] = '0'; }
    else {
        while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    }
    /* 反转 */
    if (is_neg) sb_append_char(b, '-');
    for (int i = n - 1; i >= 0; i--) sb_append_char(b, tmp[i]);
    (void)abs_v;
}

void sb_append_int(strbuf_t *b, long long v)
{
    unsigned long long uv;
    int neg = 0;
    if (v < 0) { neg = 1; uv = (unsigned long long)(-(v + 1)) + 1ULL; }
    else uv = (unsigned long long)v;
    sb_append_unsigned_internal(b, uv, neg, 0);
}

void sb_append_uint(strbuf_t *b, unsigned long long v)
{
    sb_append_unsigned_internal(b, v, 0, 0);
}

void sb_append_double(strbuf_t *b, double v)
{
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "%.6f", v);
    if (n > 0) sb_append(b, tmp, (size_t)n);
    /* 去掉末尾无意义的 0 和可能的 '.' */
    char *p = b->buf + b->len - (size_t)n;
    if (strchr(p, '.')) {
        char *end = b->buf + b->len - 1;
        while (end > p && *end == '0') { *end-- = '\0'; b->len--; }
        if (end > p && *end == '.') { *end-- = '\0'; b->len--; }
        b->buf[b->len] = '\0';
    }
}

void sb_finalize(strbuf_t *b)
{
    sb_reserve(b, 0);
    if (b->buf) b->buf[b->len] = '\0';
}

const char *sb_cstr(const strbuf_t *b)
{
    return b->buf ? b->buf : "";
}

void sb_append_json_string(strbuf_t *b, const char *s)
{
    sb_append_char(b, '"');
    if (!s) { sb_append_char(b, '"'); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  sb_append(b, "\\\"", 2); break;
        case '\\': sb_append(b, "\\\\", 2); break;
        case '\b': sb_append(b, "\\b", 2); break;
        case '\f': sb_append(b, "\\f", 2); break;
        case '\n': sb_append(b, "\\n", 2); break;
        case '\r': sb_append(b, "\\r", 2); break;
        case '\t': sb_append(b, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char esc[8];
                int n = snprintf(esc, sizeof(esc), "\\u%04x", c);
                sb_append(b, esc, (size_t)n);
            } else {
                sb_append_char(b, (char)c);
            }
            break;
        }
    }
    sb_append_char(b, '"');
}

void sb_vprintf(strbuf_t *b, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = _vscprintf(fmt, ap2);
    va_end(ap2);
    if (n <= 0) return;
    sb_reserve(b, (size_t)n);
    if (!b->buf) return;
    vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    b->len += (size_t)n;
    b->buf[b->len] = '\0';
}

void sb_printf(strbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(b, fmt, ap);
    va_end(ap);
}
