/*
 * marks.h - 函数标记（分析进度跟踪）
 * marks.h - Function marks (analysis progress tracking)
 *
 * vbdmcp **自有数据，不写入工程** —— 因此在 native 编译的项目上也能安全使用
 * （与 vbd_write_rename_function 的 native 限制无关）。
 *
 * vbdmcp's own data, NOT written into the project - therefore it is safe to use
 * on natively compiled projects too (unrelated to the native-only restriction of
 * vbd_write_rename_function).
 *
 * 存储位置：与被分析程序同目录，文件名 `vbdmcp_marks_<exe 文件名>.json`：
 *     分析 Y:\9\vb\exe\VR.exe
 *       → Y:\9\vb\exe\vbdmcp_marks_VR.exe.json
 * 若该目录不可写（典型：Program Files 下的程序），自动回退到
 *     %TEMP%\vbdmcp_marks\ 下的同名文件（marks_file_path 返回实际使用路径）。
 *
 * Location: the folder of the analyzed program, file name
 * `vbdmcp_marks_<exe name>.json`:
 *     analyzing Y:\9\vb\exe\VR.exe
 *       -> Y:\9\vb\exe\vbdmcp_marks_VR.exe.json
 * If that folder is not writable (typical for programs under Program Files), the
 * same file name is used under %TEMP%\vbdmcp_marks\ instead (marks_file_path
 * returns the path actually in use).
 *
 * 文件格式（UTF-8 JSON；索引键 "module_index,function_index"）：
 *   {
 *     "app": "VR.exe",
 *     "app_path": "Y:\\9\\vb\\exe\\VR.exe",
 *     "marks_file": "Y:\\9\\vb\\exe\\vbdmcp_marks_VR.exe.json",
 *     "updated": "2026-10-15 22:30:00",
 *     "marks": {
 *       "4,51": { "mark": "已分析：本地化条目分发",
 *                 "name": "Proc_4_51", "address": "0x89CB90",
 *                 "updated": "2026-10-15 22:30:00" }
 *     }
 *   }
 * name / address 只是人类可读的附加信息（索引可能随重新分析变化，便于人工核对）。
 * mark 值为自由文本；空串 = 未标记（写空串即删除该条）。
 *
 * File format (UTF-8 JSON; index key "module_index,function_index"):
 *   [same example as above]
 * name / address are human-readable extras only (indices may change after
 * re-analysis; they help manual verification). mark is free-form text; an empty
 * string means unmarked (writing an empty string deletes the entry).
 *
 * 线程模型：只在 UI 主线程调用（全部 MCP 工具都在 UI 线程执行），无需加锁；
 * 文件 mtime 变化时会自动重载（支持外部编辑/多窗口场景）。
 *
 * Threading: called from the UI main thread only (all MCP tools run there), so no
 * locking is needed. The file is reloaded automatically when its mtime changes
 * (supports external edits / multiple windows).
 */
#ifndef VBDMCP_MARKS_H
#define VBDMCP_MARKS_H

/* 当前项目的标记文件路径（实际使用的那一个）；无项目时返回 ""。
 * 返回内部静态缓冲，立即使用，不要长期持有。
 *
 * Path of the marks file in use for the current project; "" when no project is
 * open. Returns an internal static buffer: use it immediately, do not hold it. */
const char *marks_file_path(void);

/* 查询某函数的标记；未标记返回 ""（内部静态缓冲，立即使用）。
 * Query the mark of one function; "" when unmarked (internal static buffer). */
const char *marks_get(int module_index, int function_index);

/* 设置 / 清除标记：mark 为 NULL 或 "" → 删除该条。
 * name / address 为 **UTF-8** 字符串（可为 NULL，仅作附加信息）。
 * 返回 1 = 成功，0 = 失败（文件不可写等）。
 * 会同步更新内存缓存并原子写盘（临时文件 + MoveFileEx 替换）。
 *
 * Set / clear a mark: mark NULL or "" deletes the entry.
 * name / address are **UTF-8** strings (may be NULL, extra info only).
 * Returns 1 on success, 0 on failure (file not writable etc.).
 * Updates the in-memory cache and writes atomically (temp file + MoveFileEx). */
int marks_set(int module_index, int function_index,
              const char *mark, const char *name, const char *address);

/* 批量项（供 marks_set_many 使用）
 * Batch item (used by marks_set_many). */
typedef struct marks_item {
    int         module_index;
    int         function_index;
    const char *mark;       /* UTF-8；NULL 或 "" = 删除该项 / NULL or "" = delete */
    const char *name;       /* UTF-8，可为 NULL / UTF-8, may be NULL */
    const char *address;    /* ASCII，可为 NULL / ASCII, may be NULL */
} marks_item;

/* 批量设置 / 清除：全部改完内存后**只写盘一次**（逐个 marks_set 会在
 * 每项都触发一次序列化 + 文件替换，批量场景下浪费明显）。
 * 返回 1 = 成功，0 = 失败。count <= 0 视为成功（无事可做）。
 *
 * Batch set / clear: after all in-memory updates, the file is written ONCE
 * (calling marks_set per item would serialize and replace the file every time).
 * Returns 1 on success, 0 on failure. count <= 0 counts as success (nothing to do). */
int marks_set_many(const marks_item *items, int count);

/* 枚举全部标记（按 module_index,function_index 数值升序）。
 * 回调返回 0 可提前结束；返回实际枚举条数。
 *
 * Enumerates all marks (ascending by module_index,function_index).
 * Returning 0 from the callback stops early; the return value is the number
 * of marks visited. */
typedef int (*marks_enum_fn)(int module_index, int function_index,
                             const char *mark, const char *name,
                             const char *address, void *user);
int marks_enum(marks_enum_fn fn, void *user);

/* 丢弃内存缓存（下次访问重新读盘）。
 * Drops the in-memory cache (the file is re-read on next access). */
void marks_reset_cache(void);

#endif /* VBDMCP_MARKS_H */
