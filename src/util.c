/*
 * util.c - 公共工具函数：内存分配、调试日志、字符串辅助
 */
#include "vbmcp.h"

/* ===== 内存 ===== */

void *vbmcp_alloc(size_t n)
{
    if (n == 0) n = 1;
    return malloc(n);
}

void *vbmcp_calloc(size_t n)
{
    if (n == 0) n = 1;
    return calloc(1, n);
}

char *vbmcp_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)vbmcp_alloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n + 1);
    return r;
}

char *vbmcp_strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    /* 计算实际长度（不超过 n） */
    size_t real = 0;
    while (real < n && s[real]) real++;
    char *r = (char *)vbmcp_alloc(real + 1);
    if (!r) return NULL;
    memcpy(r, s, real);
    r[real] = '\0';
    return r;
}

void vbmcp_free(void *p)
{
    if (p) free(p);
}

/* ===== 字符串辅助 ===== */

int vb_streq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

int vb_str_empty(const char *s)
{
    return !s || !s[0];
}

/* 修剪并复制（去除两端空白） */
char *vb_str_trim_dup(const char *s, size_t n)
{
    if (!s) return NULL;
    /* 找首 */
    size_t start = 0;
    while (start < n && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r'))
        start++;
    /* 找尾 */
    size_t end = n;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r'))
        end--;
    return vbmcp_strndup(s + start, end - start);
}

/* ===== 调试日志 =====
 * 写入 %TEMP%\vbmcp.log，需要设置环境变量 VBMCP_DEBUG_LOG=1 启用
 */
void vbmcp_log(const char *fmt, ...)
{
    static int enabled = -1;
    if (enabled == -1) {
        char envval[2] = "";
        enabled = (GetEnvironmentVariableA("VBMCP_DEBUG_LOG", envval, 2) > 0) ? 1 : 0;
    }
    if (!enabled) return;

    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) strcpy_s(path, MAX_PATH, ".");
    strcat_s(path, MAX_PATH, "\\vbmcp.log");

    FILE *f = NULL;
    fopen_s(&f, path, "ab");
    if (!f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}
