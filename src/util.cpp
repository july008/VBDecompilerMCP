/*
 * util.cpp - 公共工具函数：内存分配、调试日志、字符串辅助
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

/* ===== 分级调试日志 =====
 * 写入 %TEMP%\vbmcp.log，需要设置环境变量 VBMCP_DEBUG_LOG=1 启用。
 * 阈值由 VBMCP_LOG_LEVEL 控制（0=ERROR 1=WARN 2=INFO 3=DEBUG，默认 2）。
 * 安全约束：调用方不得把项目源码/反编译文本等内容写入日志。
 *
 * 线程安全：C++ 迁移后日志同时来自 UI 主线程与后台网络线程，
 * 用互斥量串行化写文件，避免交错写坏日志行。
 */
static CRITICAL_SECTION g_log_lock;
static INIT_ONCE       g_log_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_log_lock(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_log_lock);
    return TRUE;
}

static void log_lock(void)
{
    InitOnceExecuteOnce(&g_log_lock_once, init_log_lock, nullptr, nullptr);
    EnterCriticalSection(&g_log_lock);
}

static void log_unlock(void)
{
    LeaveCriticalSection(&g_log_lock);
}

static const char *log_level_tag(int level)
{
    switch (level) {
    case VBMCP_LOG_ERROR: return "ERROR";
    case VBMCP_LOG_WARN:  return "WARN ";
    case VBMCP_LOG_DEBUG: return "DEBUG";
    default:              return "INFO ";
    }
}

void vbmcp_logf(int level, const char *fmt, ...)
{
    static int enabled = -1;   /* -1=未初始化 */
    static int threshold = VBMCP_LOG_INFO;
    log_lock();
    if (enabled == -1) {
        char envval[8] = "";
        enabled = (GetEnvironmentVariableA("VBMCP_DEBUG_LOG", envval, sizeof(envval)) > 0) ? 1 : 0;
        envval[0] = '\0';
        if (GetEnvironmentVariableA("VBMCP_LOG_LEVEL", envval, sizeof(envval)) > 0) {
            int lv = atoi(envval);
            if (lv >= VBMCP_LOG_ERROR && lv <= VBMCP_LOG_DEBUG) threshold = lv;
        }
    }
    if (!enabled || level > threshold) { log_unlock(); return; }

    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) strcpy_s(path, MAX_PATH, ".");
    strcat_s(path, MAX_PATH, "\\vbmcp.log");

    FILE *f = NULL;
    if (fopen_s(&f, path, "ab") != 0 || !f) { log_unlock(); return; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d][%s] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        log_level_tag(level));

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
    log_unlock();
}

/* 兼容旧调用点：等价于 vbmcp_logf(VBMCP_LOG_INFO, ...) */
void vbmcp_log(const char *fmt, ...)
{
    static int enabled = -1;
    log_lock();
    if (enabled == -1) {
        char envval[2] = "";
        enabled = (GetEnvironmentVariableA("VBMCP_DEBUG_LOG", envval, 2) > 0) ? 1 : 0;
    }
    if (!enabled) { log_unlock(); return; }

    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) strcpy_s(path, MAX_PATH, ".");
    strcat_s(path, MAX_PATH, "\\vbmcp.log");

    FILE *f = NULL;
    if (fopen_s(&f, path, "ab") != 0 || !f) { log_unlock(); return; }

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
    log_unlock();
}

/* ============================================================
 * 通用 UTF-8 → ANSI 代码页（中文 Windows = GBK）转换。
 *
 * i18n 体系里 `_()` 返回 UTF-8、`_A()` 返回 GBK；本函数用于把
 * "不在语言包里、但源码里是 UTF-8 字面量" 的文本（如设置窗体中
 * 各 AI 客户端的接入说明）转成 GBK 再交给 ANSI 控件。
 *
 * 返回 vbmcp_alloc 申请的新字符串（调用方 vbmcp_free）；
 * ASCII 快速路径直接 strdup；失败回退 strdup(src)（乱码但不崩）。
 * ============================================================ */
char *vbmcp_str_to_gbk(const char *src)
{
    if (!src) return NULL;
    int all_ascii = 1;
    for (const char *p = src; *p; p++) {
        if ((unsigned char)*p >= 0x80) { all_ascii = 0; break; }
    }
    if (all_ascii) return vbmcp_strdup(src);

    size_t src_len = strlen(src);
    wchar_t *wbuf = (wchar_t *)vbmcp_alloc((src_len + 1) * sizeof(wchar_t));
    if (!wbuf) return vbmcp_strdup(src);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, src, -1, wbuf, (int)(src_len + 1));
    if (wlen <= 0) { vbmcp_free(wbuf); return vbmcp_strdup(src); }

    /* UTF-16 字符数 × 2 + NUL 足够容纳 GBK */
    size_t need = (size_t)(wlen - 1) * 2 + 16;
    char *out = (char *)vbmcp_alloc(need);
    if (!out) { vbmcp_free(wbuf); return vbmcp_strdup(src); }
    int glen = WideCharToMultiByte(CP_ACP, 0, wbuf, wlen - 1,
                                   out, (int)(need - 1), NULL, NULL);
    vbmcp_free(wbuf);
    if (glen <= 0) { vbmcp_free(out); return vbmcp_strdup(src); }
    out[glen] = '\0';
    return out;
}


/* ============================================================
 * 取当前 DLL 自身模块句柄
 *
 * 插件的宿主是 VB Decompiler，GetModuleHandle(NULL) 拿到的是宿主句柄，
 * 资源（语言包 JSON_ZH / 对话框模板 IDD_VBMCP_OPTIONS）必须用本 DLL 的
 * 句柄去找。这里传入本模块函数地址，由 GetModuleHandleExA 反查，
 * UNCHANGED_REFCOUNT 保证不额外增加引用计数（不需要 FreeLibrary）。
 * ============================================================ */
HMODULE vbmcp_self_module(void)
{
    HMODULE h = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(const void *)&vbmcp_self_module, &h)) {
        return NULL;
    }
    return h;
}
