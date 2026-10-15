/*
 * marks.h - 函数标记（分析进度跟踪）
 *
 * vbdmcp **自有数据，不写入工程** —— 因此在 native 编译的项目上也能安全使用
 * （与 vbd_write_rename_function 的 native 限制无关）。
 *
 * 存储位置：与被分析程序同目录，文件名 `vbdmcp_marks_<exe 文件名>.json`：
 *     分析 Y:\9\vb\exe\VR_ONln.exe
 *       → Y:\9\vb\exe\vbdmcp_marks_VR_ONln.exe.json
 * 若该目录不可写（典型：Program Files 下的程序），自动回退到
 *     %TEMP%\vbdmcp_marks\ 下的同名文件（marks_file_path 会返回实际使用路径）。
 *
 * 文件格式（UTF-8 JSON；索引键 "module_index,function_index"）：
 *   {
 *     "app": "VR_ONln.exe",
 *     "app_path": "Y:\\9\\vb\\exe\\VR_ONln.exe",
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
 * 线程模型：只在 UI 主线程调用（全部 MCP 工具都在 UI 线程执行），无需加锁；
 * 文件 mtime 变化时会自动重载（支持外部编辑/多窗口场景）。
 */
#ifndef VBDMCP_MARKS_H
#define VBDMCP_MARKS_H

/* 当前项目的标记文件路径（实际使用的那一个）；无项目时返回 ""。
 * 返回内部静态缓冲，立即使用，不要长期持有。 */
const char *marks_file_path(void);

/* 查询某函数的标记；未标记返回 ""（内部静态缓冲，立即使用）。 */
const char *marks_get(int module_index, int function_index);

/* 设置 / 清除标记：mark 为 NULL 或 "" → 删除该条。
 * name / address 为 **UTF-8** 字符串（可为 NULL，仅作附加信息）。
 * 返回 1 = 成功，0 = 失败（文件不可写等）。
 * 会同步更新内存缓存并原子写盘（临时文件 + MoveFileEx 替换）。 */
int marks_set(int module_index, int function_index,
              const char *mark, const char *name, const char *address);

/* 枚举全部标记（按 module_index,function_index 数值升序）。
 * 回调返回 0 可提前结束；返回实际枚举条数。 */
typedef int (*marks_enum_fn)(int module_index, int function_index,
                             const char *mark, const char *name,
                             const char *address, void *user);
int marks_enum(marks_enum_fn fn, void *user);

/* 丢弃内存缓存（下次访问重新读盘）。 */
void marks_reset_cache(void);

#endif /* VBDMCP_MARKS_H */
