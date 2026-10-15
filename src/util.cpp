/*
 * util.cpp - 公共工具函数：内存分配、调试日志、字符串辅助
 * util.cpp - Shared helpers: memory allocation, debug logging, string utilities
 */
#include "vbdmcp.h"

/* ===== 内存 / Memory ===== */

void *vbdmcp_alloc(size_t n)
{
    if (n == 0) n = 1;
    return malloc(n);
}

void *vbdmcp_calloc(size_t n)
{
    if (n == 0) n = 1;
    return calloc(1, n);
}

char *vbdmcp_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)vbdmcp_alloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n + 1);
    return r;
}

void vbdmcp_free(void *p)
{
    if (p) free(p);
}

/* ===== 字符串辅助 / String helpers ===== */

int vbd_streq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* ===== 分级调试日志 / Leveled debug log =====
 * 写入 %TEMP%\vbdmcp.log，设置环境变量 VBDMCP_DEBUG_LOG=1 启用。
 * 阈值由 VBDMCP_LOG_LEVEL 控制（0=ERROR 1=WARN 2=INFO 3=DEBUG，默认 2）。
 * 安全约束：调用方不得把项目源码/反编译文本等内容写入日志。
 *
 * Writes to %TEMP%\vbdmcp.log; enabled by setting VBDMCP_DEBUG_LOG=1.
 * Threshold via VBDMCP_LOG_LEVEL (0=ERROR 1=WARN 2=INFO 3=DEBUG, default 2).
 * Security rule: callers must never log project source or decompiled text.
 *
 * 线程安全：日志同时来自 UI 主线程与后台网络线程，用互斥量串行化写文件，
 * 避免交错写坏日志行。
 * Thread safety: logs come from both the UI main thread and the background
 * network thread; a mutex serializes file writes so lines never interleave.
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
    case VBDMCP_LOG_ERROR: return "ERROR";
    case VBDMCP_LOG_WARN:  return "WARN ";
    case VBDMCP_LOG_DEBUG: return "DEBUG";
    default:              return "INFO ";
    }
}

/* 日志文件轮转：单文件上限 10MB，超过则在进程内首次写入时改名 .old
 * （只保留一代）。只在日志临界区内调用，checked 标志无需原子。
 *
 * Log rotation: the file is capped at 10MB; when exceeded it is renamed to
 * ".old" on the first write of this process (one generation is kept). Only run
 * inside the log critical section, so "checked" needs no atomics. */
static const unsigned long long kLogMaxBytes = 10ull * 1024 * 1024;

static void rotate_log_if_needed(const char *path)
{
    static bool checked = false;
    if (checked) return;
    checked = true;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return;
    const unsigned long long size =
        ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size < kLogMaxBytes) return;

    char old_path[MAX_PATH + 8];
    _snprintf_s(old_path, sizeof(old_path), _TRUNCATE, "%s.old", path);
    DeleteFileA(old_path);
    MoveFileA(path, old_path);
}

/* 日志核心（va_list 版本）：vbdmcp_logf 与 vbdmcp_log 共用同一实现，
 * 统一处理环境变量解析、阈值判断与文件写入。
 *
 * Log core (va_list version): vbdmcp_logf and vbdmcp_log share this
 * implementation for environment parsing, threshold checks and file writes. */
static void log_write_va(int level, const char *fmt, va_list ap)
{
    static int enabled = -1;   /* -1=未初始化 / not initialized yet */
    static int threshold = VBDMCP_LOG_INFO;
    log_lock();
    if (enabled == -1) {
        char envval[8] = "";
        enabled = (GetEnvironmentVariableA("VBDMCP_DEBUG_LOG", envval, sizeof(envval)) > 0) ? 1 : 0;
        envval[0] = '\0';
        if (GetEnvironmentVariableA("VBDMCP_LOG_LEVEL", envval, sizeof(envval)) > 0) {
            int lv = atoi(envval);
            if (lv >= VBDMCP_LOG_ERROR && lv <= VBDMCP_LOG_DEBUG) threshold = lv;
        }
    }
    if (!enabled || level > threshold) { log_unlock(); return; }

    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) strcpy_s(path, MAX_PATH, ".");
    strcat_s(path, MAX_PATH, "\\vbdmcp.log");

    rotate_log_if_needed(path);

    FILE *f = NULL;
    if (fopen_s(&f, path, "ab") != 0 || !f) { log_unlock(); return; }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d][%s] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        log_level_tag(level));

    vfprintf(f, fmt, ap);

    fprintf(f, "\n");
    fclose(f);
    log_unlock();
}

void vbdmcp_logf(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write_va(level, fmt, ap);
    va_end(ap);
}

/* 等价于 vbdmcp_logf(VBDMCP_LOG_INFO, ...)：元信息类日志的统一入口。
 * Equivalent to vbdmcp_logf(VBDMCP_LOG_INFO, ...): the common entry point for
 * informational logs. */
void vbdmcp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write_va(VBDMCP_LOG_INFO, fmt, ap);
    va_end(ap);
}

/* ============================================================
 * 通用 UTF-8 → ANSI 代码页（中文 Windows = GBK）转换。
 *
 * i18n 体系里 `_()` 返回 UTF-8、`_A()` 返回 GBK；本函数用于把
 * "不在语言包里、但源码里是 UTF-8 字面量" 的文本（如设置窗体中
 * 各 AI 客户端的接入说明）转成 GBK 再交给 ANSI 控件。
 *
 * 返回 vbdmcp_alloc 申请的新字符串（调用方 vbdmcp_free）；
 * ASCII 快速路径直接 strdup；失败回退 strdup(src)（乱码但不崩）。
 *
 * Generic UTF-8 -> ANSI code page (GBK on Chinese Windows) conversion.
 *
 * In the i18n scheme `_()` returns UTF-8 and `_A()` returns GBK; this function
 * converts text that is not in the language pack but is a UTF-8 literal in the
 * sources (e.g. the per-client setup guides in the settings dialog) to GBK for
 * ANSI controls.
 *
 * Returns a new alloc'ed string (caller frees with vbdmcp_free). The ASCII fast
 * path strdups directly; failures fall back to strdup(src) (garbled but stable).
 * ============================================================ */
char *vbdmcp_str_to_gbk(const char *src)
{
    if (!src) return NULL;
    int all_ascii = 1;
    for (const char *p = src; *p; p++) {
        if ((unsigned char)*p >= 0x80) { all_ascii = 0; break; }
    }
    if (all_ascii) return vbdmcp_strdup(src);

    size_t src_len = strlen(src);
    wchar_t *wbuf = (wchar_t *)vbdmcp_alloc((src_len + 1) * sizeof(wchar_t));
    if (!wbuf) return vbdmcp_strdup(src);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, src, -1, wbuf, (int)(src_len + 1));
    if (wlen <= 0) { vbdmcp_free(wbuf); return vbdmcp_strdup(src); }

    /* UTF-16 字符数 × 2 + NUL 足够容纳 GBK
     * UTF-16 char count x 2 + NUL is enough for GBK */
    size_t need = (size_t)(wlen - 1) * 2 + 16;
    char *out = (char *)vbdmcp_alloc(need);
    if (!out) { vbdmcp_free(wbuf); return vbdmcp_strdup(src); }
    int glen = WideCharToMultiByte(CP_ACP, 0, wbuf, wlen - 1,
                                   out, (int)(need - 1), NULL, NULL);
    vbdmcp_free(wbuf);
    if (glen <= 0) { vbdmcp_free(out); return vbdmcp_strdup(src); }
    out[glen] = '\0';
    return out;
}

/* ============================================================
 * 通用 ANSI（GBK / CP_ACP）→ UTF-8 转换。
 *
 * VB Decompiler Callback Engine 返回 ANSI 字节，而 MCP 报文必须是
 * UTF-8；工具层在把引擎文本放进 JSON 之前用本函数转换（默认 utf-8 模式）。
 * 与 vbdmcp_str_to_gbk 互为逆向：写入路径用后者把客户端 UTF-8 转回 ANSI。
 *
 * 返回 vbdmcp_alloc 申请的新字符串（调用方 vbdmcp_free）；
 * ASCII 快速路径直接 strdup；转换失败回退 strdup(src)（乱码但不崩）。
 *
 * Generic ANSI (GBK / CP_ACP) -> UTF-8 conversion.
 *
 * The VB Decompiler callback engine returns ANSI bytes while MCP messages must
 * be UTF-8; the tool layer converts engine text with this function before it
 * enters JSON (default utf-8 mode). It is the inverse of vbdmcp_str_to_gbk,
 * which the write path uses to convert client UTF-8 back to ANSI.
 *
 * Returns a new alloc'ed string (caller frees with vbdmcp_free). The ASCII fast
 * path strdups directly; failures fall back to strdup(src) (garbled but stable).
 * ============================================================ */
char *vbdmcp_acp_to_utf8(const char *src)
{
    if (!src) return NULL;
    int all_ascii = 1;
    for (const char *p = src; *p; p++) {
        if ((unsigned char)*p >= 0x80) { all_ascii = 0; break; }
    }
    if (all_ascii) return vbdmcp_strdup(src);

    int wlen = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
    if (wlen <= 0) return vbdmcp_strdup(src);
    wchar_t *wbuf = (wchar_t *)vbdmcp_alloc((size_t)wlen * sizeof(wchar_t));
    if (!wbuf) return vbdmcp_strdup(src);
    if (MultiByteToWideChar(CP_ACP, 0, src, -1, wbuf, wlen) <= 0) {
        vbdmcp_free(wbuf);
        return vbdmcp_strdup(src);
    }

    int ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen - 1, NULL, 0, NULL, NULL);
    if (ulen <= 0) { vbdmcp_free(wbuf); return vbdmcp_strdup(src); }
    char *out = (char *)vbdmcp_alloc((size_t)ulen + 1);
    if (!out) { vbdmcp_free(wbuf); return vbdmcp_strdup(src); }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen - 1, out, ulen, NULL, NULL);
    out[ulen] = '\0';
    vbdmcp_free(wbuf);
    return out;
}


/* ============================================================
 * 取当前 DLL 自身模块句柄
 *
 * 插件的宿主是 VB Decompiler，GetModuleHandle(NULL) 拿到的是宿主句柄，
 * 资源（语言包 JSON_ZH / 对话框模板 IDD_VBDMCP_OPTIONS）必须用本 DLL 的
 * 句柄去找。这里传入本模块函数地址，由 GetModuleHandleExA 反查，
 * UNCHANGED_REFCOUNT 保证不额外增加引用计数（不需要 FreeLibrary）。
 *
 * Get the module handle of this DLL itself.
 *
 * The host of the plugin is VB Decompiler, so GetModuleHandle(NULL) returns the
 * host handle, while resources (language pack JSON_ZH, dialog template
 * IDD_VBDMCP_OPTIONS) must be looked up with this DLL's handle. Passing the
 * address of a function in this module to GetModuleHandleExA resolves it;
 * UNCHANGED_REFCOUNT avoids increasing the reference count (no FreeLibrary).
 * ============================================================ */
HMODULE vbdmcp_self_module(void)
{
    HMODULE h = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(const void *)&vbdmcp_self_module, &h)) {
        return NULL;
    }
    return h;
}
