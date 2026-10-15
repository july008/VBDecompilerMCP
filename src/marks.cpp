/*
 * marks.cpp - 函数标记实现（路径 / 文件格式 / 线程约定见 marks.h）
 * marks.cpp - Function marks implementation (paths / format / threading: see marks.h)
 *
 * 所有文本在内存与文件中一律是 **UTF-8**（vbdmcp 自有数据，与工程的
 * ANSI/GBK 字节无关）；调用方负责在传入前把引擎文本转成 UTF-8。
 * 文件路径仍用 ANSI（与引擎返回的路径、Win32 A 系列 API 保持一致）。
 *
 * All text is **UTF-8** in memory and in the file (vbdmcp's own data, unrelated
 * to the project's ANSI/GBK bytes); callers convert engine text to UTF-8 before
 * passing it in. File paths stay ANSI (consistent with engine-returned paths and
 * with Win32 A APIs).
 */
#include "marks.h"
#include "vbdmcp.h"
#include "yyjson.h"

#include <algorithm>
#include <map>
#include <string>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <windows.h>

namespace {

struct MarkEntry {
    std::string mark;      /* UTF-8 */
    std::string name;      /* UTF-8，附加信息（可选）/ UTF-8, extra info (optional) */
    std::string address;   /* ASCII，如 "0x89CB90"（可选）/ ASCII, e.g. "0x89CB90" (optional) */
    std::string updated;   /* 本地时间戳 / local timestamp */
};

std::map<std::string, MarkEntry> g_marks;   /* key = "module_index,function_index" */

/* 已加载状态（用于 mtime 感知的缓存失效）
 * Loaded state (used for mtime-aware cache invalidation) */
std::string g_loaded_app;                   /* 被分析程序路径（ANSI）/ analyzed program path (ANSI) */
std::string g_loaded_file;                  /* 实际使用的标记文件（ANSI）/ marks file in use (ANSI) */
FILETIME    g_loaded_mtime = { 0, 0 };
int         g_loaded_valid = 0;

/* marks_file_path / marks_get 的返回缓冲：轮转 4 个槽位，避免相邻调用互相覆盖。
 * Return buffers for marks_file_path / marks_get: 4 rotating slots so adjacent
 * calls do not overwrite each other. */
const char *rot(const std::string& s, int slot)
{
    static std::string bufs[4];
    if (slot < 0 || slot > 3) slot = 0;
    bufs[slot] = s;
    return bufs[slot].c_str();
}

std::string key_of(int m, int f)
{
    char b[32];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%d,%d", m, f);
    return std::string(b);
}

void stamp_now(std::string *out)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[32];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    *out = b;
}

/* 被分析程序路径（引擎返回 ANSI/GBK 字节）
 * Path of the analyzed program (the engine returns ANSI/GBK bytes). */
std::string app_path_ansi(void)
{
    char *fn = vbd_get_value(VBD_GetFileName, 0, 0);
    std::string s = fn ? fn : "";
    if (fn) vbdmcp_free(fn);
    return s;
}

std::string dir_of(const std::string& p)
{
    const size_t i = p.find_last_of("\\/");
    return (i == std::string::npos) ? std::string() : p.substr(0, i);
}

std::string base_of(const std::string& p)
{
    const size_t i = p.find_last_of("\\/");
    return (i == std::string::npos) ? p : p.substr(i + 1);
}

bool file_exists_ansi(const std::string& p)
{
    return !p.empty() && GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool file_mtime(const std::string& p, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (p.empty() || !GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad)) return false;
    *ft = fad.ftLastWriteTime;
    return true;
}

std::string temp_dir(void)
{
    char tmp[MAX_PATH] = { 0 };
    const DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string d = (n > 0 && n < MAX_PATH) ? tmp : "C:\\Windows\\Temp\\";
    while (!d.empty() && (d[d.size() - 1] == '\\' || d[d.size() - 1] == '/')) d.erase(d.size() - 1);
    return d;
}

std::string fallback_path(const std::string& app)
{
    const std::string d = temp_dir() + "\\vbdmcp_marks";
    CreateDirectoryA(d.c_str(), NULL);      /* 已存在时返回失败，忽略 / fails when it already exists, ignored */
    return d + "\\vbdmcp_marks_" + base_of(app) + ".json";
}

std::string primary_path(const std::string& app)
{
    const std::string dir = dir_of(app);
    if (dir.empty()) return std::string();
    return dir + "\\vbdmcp_marks_" + base_of(app) + ".json";
}

/* 决定当前使用哪个文件：
 *   1) 首选存在     → 首选；
 *   2) 回退存在     → 回退（说明之前写过、但首选目录不可写）；
 *   3) 都不存在     → 首选（首次保存失败时会自动改用回退并记住）。
 *
 * Chooses which file to use:
 *   1) primary exists  -> primary;
 *   2) fallback exists -> fallback (written before while the primary folder was
 *      not writable);
 *   3) neither exists  -> primary (a failed first save switches to the fallback
 *      and remembers it). */
std::string resolve_path(const std::string& app)
{
    if (app.empty()) return std::string();
    const std::string pri = primary_path(app);
    const std::string fb  = fallback_path(app);
    if (file_exists_ansi(pri)) return pri;
    if (file_exists_ansi(fb))  return fb;
    return pri.empty() ? fb : pri;
}

void load_from(const std::string& path)
{
    g_marks.clear();
    if (path.empty()) return;

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return;
    std::string buf;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) buf.append(chunk, n);
    fclose(fp);
    if (buf.empty()) return;

    const char *p = buf.c_str();
    size_t len = buf.size();
    if (len >= 3 && (unsigned char)p[0] == 0xEF &&
        (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3; len -= 3;                      /* 去 UTF-8 BOM / strip the UTF-8 BOM */
    }

    yyjson_doc *doc = yyjson_read(p, len, YYJSON_READ_NOFLAG);
    if (!doc) return;
    yyjson_val *root  = yyjson_doc_get_root(doc);
    yyjson_val *marks = root ? yyjson_obj_get(root, "marks") : NULL;
    if (marks && yyjson_is_obj(marks)) {
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(marks, idx, max, k, v) {
            if (!yyjson_is_obj(v)) continue;
            MarkEntry e;
            yyjson_val *mk = yyjson_obj_get(v, "mark");
            yyjson_val *nm = yyjson_obj_get(v, "name");
            yyjson_val *ad = yyjson_obj_get(v, "address");
            yyjson_val *up = yyjson_obj_get(v, "updated");
            e.mark    = (mk && yyjson_is_str(mk)) ? yyjson_get_str(mk) : "";
            e.name    = (nm && yyjson_is_str(nm)) ? yyjson_get_str(nm) : "";
            e.address = (ad && yyjson_is_str(ad)) ? yyjson_get_str(ad) : "";
            e.updated = (up && yyjson_is_str(up)) ? yyjson_get_str(up) : "";
            if (!e.mark.empty()) g_marks[yyjson_get_str(k)] = e;
        }
    }
    yyjson_doc_free(doc);
}

/* 缓存失效检查：项目变化 / 文件位置变化 / mtime 变化 → 重新加载
 * Cache invalidation: project, file location or mtime changed -> reload. */
void ensure_loaded(void)
{
    const std::string app  = app_path_ansi();
    const std::string path = resolve_path(app);

    FILETIME ft = { 0, 0 };
    const bool exists = file_mtime(path, &ft);

    if (g_loaded_valid && g_loaded_app == app && g_loaded_file == path) {
        const bool same = exists
            ? (CompareFileTime(&ft, &g_loaded_mtime) == 0)
            : (g_loaded_mtime.dwLowDateTime == 0 && g_loaded_mtime.dwHighDateTime == 0);
        if (same) return;
    }

    g_loaded_app   = app;
    g_loaded_file  = path;
    g_loaded_mtime = exists ? ft : FILETIME{ 0, 0 };
    g_loaded_valid = 1;
    load_from(path);
}

/* 序列化全部标记为 UTF-8 JSON（new alloc，调用方 vbdmcp_free）。
 * Serializes all marks as UTF-8 JSON (new alloc, caller frees with vbdmcp_free). */
char *serialize_all(void)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) return NULL;
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    char *app_u8  = vbdmcp_acp_to_utf8(g_loaded_app.c_str());   /* 路径是 ANSI，文件内转 UTF-8
                                                                   paths are ANSI; convert to UTF-8 for the file */
    char *file_u8 = vbdmcp_acp_to_utf8(g_loaded_file.c_str());
    yyjson_mut_obj_add_strcpy(doc, root, "app", base_of(g_loaded_app).c_str());
    yyjson_mut_obj_add_strcpy(doc, root, "app_path", app_u8 ? app_u8 : "");
    yyjson_mut_obj_add_strcpy(doc, root, "marks_file", file_u8 ? file_u8 : "");
    std::string updated;
    stamp_now(&updated);
    yyjson_mut_obj_add_strcpy(doc, root, "updated", updated.c_str());
    if (app_u8)  vbdmcp_free(app_u8);
    if (file_u8) vbdmcp_free(file_u8);

    yyjson_mut_val *marks = yyjson_mut_obj(doc);
    for (std::map<std::string, MarkEntry>::const_iterator it = g_marks.begin();
         it != g_marks.end(); ++it) {
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, o, "mark", it->second.mark.c_str());
        if (!it->second.name.empty())
            yyjson_mut_obj_add_strcpy(doc, o, "name", it->second.name.c_str());
        if (!it->second.address.empty())
            yyjson_mut_obj_add_strcpy(doc, o, "address", it->second.address.c_str());
        if (!it->second.updated.empty())
            yyjson_mut_obj_add_strcpy(doc, o, "updated", it->second.updated.c_str());
        yyjson_mut_obj_add_val(doc, marks, it->first.c_str(), o);
    }
    yyjson_mut_obj_add_val(doc, root, "marks", marks);

    char *s = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
    yyjson_mut_doc_free(doc);
    return s;
}

/* 原子写入：先写 <path>.tmp，再 MoveFileEx 替换目标。返回 1 = 成功。
 * Atomic write: write <path>.tmp first, then MoveFileEx onto the target.
 * Returns 1 on success. */
int write_atomic(const std::string& path, const char *data)
{
    if (path.empty() || !data) return 0;
    const std::string tmp = path + ".tmp";
    FILE *fp = fopen(tmp.c_str(), "wb");
    if (!fp) return 0;
    const size_t len = strlen(data);
    const int written = (fwrite(data, 1, len, fp) == len);
    fclose(fp);
    if (!written) { DeleteFileA(tmp.c_str()); return 0; }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return 0;
    }
    return 1;
}

int save_current(void)
{
    if (g_loaded_file.empty()) return 0;
    char *data = serialize_all();
    if (!data) return 0;

    int ok = write_atomic(g_loaded_file, data);
    if (!ok) {
        /* 首选目录不可写（典型：Program Files 下的程序）→ 回退 %TEMP% 并记住
         * Primary folder not writable (typical: a program under Program Files)
         * -> fall back to %TEMP% and remember it. */
        const std::string fb = fallback_path(g_loaded_app);
        if (!fb.empty() && fb != g_loaded_file && write_atomic(fb, data)) {
            g_loaded_file = fb;
            ok = 1;
        }
    }
    vbdmcp_free(data);

    if (ok) {
        FILETIME ft = { 0, 0 };
        file_mtime(g_loaded_file, &ft);
        g_loaded_mtime = ft;      /* 更新缓存基准，避免自己的写入触发重载
                                     refresh the cache baseline so our own write does not trigger a reload */
    }
    return ok;
}

/* marks_enum 的排序项（按 module, function 数值序）
 * Sort item for marks_enum (numeric order by module, function). */
struct EnumItem {
    int m;
    int f;
    const MarkEntry *e;
};

bool enum_less(const EnumItem& a, const EnumItem& b)
{
    return (a.m != b.m) ? (a.m < b.m) : (a.f < b.f);
}

} // namespace

/* ---------------- 对外接口 / Public API ---------------- */

const char *marks_file_path(void)
{
    ensure_loaded();
    return rot(g_loaded_file, 0);
}

const char *marks_get(int module_index, int function_index)
{
    ensure_loaded();
    const std::string k = key_of(module_index, function_index);
    std::map<std::string, MarkEntry>::const_iterator it = g_marks.find(k);
    return rot(it == g_marks.end() ? std::string() : it->second.mark, 1);
}

int marks_set(int module_index, int function_index,
              const char *mark, const char *name, const char *address)
{
    marks_item it;
    it.module_index   = module_index;
    it.function_index = function_index;
    it.mark           = mark;
    it.name           = name;
    it.address        = address;
    return marks_set_many(&it, 1);
}

int marks_set_many(const marks_item *items, int count)
{
    if (!items || count <= 0) return 1;         /* 空批 = 无事可做 / empty batch = nothing to do */
    ensure_loaded();

    std::string now;
    stamp_now(&now);
    for (int i = 0; i < count; i++) {
        const std::string k = key_of(items[i].module_index, items[i].function_index);
        if (!items[i].mark || !*items[i].mark) {
            g_marks.erase(k);                   /* 空串 = 取消标记 / empty string = clear the mark */
        } else {
            MarkEntry e;
            e.mark = items[i].mark;
            if (items[i].name)    e.name = items[i].name;
            if (items[i].address) e.address = items[i].address;
            e.updated = now;
            g_marks[k] = e;
        }
    }
    return save_current();                      /* 只写盘一次 / write the file once */
}

int marks_enum(marks_enum_fn fn, void *user)
{
    ensure_loaded();
    if (!fn) return 0;

    std::vector<EnumItem> items;
    items.reserve(g_marks.size());
    for (std::map<std::string, MarkEntry>::const_iterator it = g_marks.begin();
         it != g_marks.end(); ++it) {
        int m = -1, f = -1;
        if (sscanf(it->first.c_str(), "%d,%d", &m, &f) == 2) {
            EnumItem x;
            x.m = m;
            x.f = f;
            x.e = &it->second;
            items.push_back(x);
        }
    }
    std::sort(items.begin(), items.end(), enum_less);

    int n = 0;
    for (size_t i = 0; i < items.size(); i++) {
        if (!fn(items[i].m, items[i].f, items[i].e->mark.c_str(),
                items[i].e->name.c_str(), items[i].e->address.c_str(), user)) {
            break;
        }
        n++;
    }
    return n;
}

void marks_reset_cache(void)
{
    g_loaded_valid = 0;
    g_loaded_app.clear();
    g_loaded_file.clear();
    g_loaded_mtime = FILETIME{ 0, 0 };
    g_marks.clear();
}
