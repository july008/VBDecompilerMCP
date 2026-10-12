/*
 * lang.c - 多语言资源 + 注册表 CustomLanguage 读取 + UTF-8→GBK 转换缓存
 *
 * 仅 en + zh-CN 两种语言。新增语言时：
 *   1) vbmcp_lang_t 增加枚举值
 *   2) g_strings[] 增加对应列
 *   3) vbmcp_lang_refresh 决定选哪一列
 *
 * 编码策略：
 *   - 源文件统一 UTF-8（编辑器友好，VS / VS Code / Notepad++ 正常显示）
 *   - 所有用户可见字符串在运行时通过 vbmcp_t() 获取，返回 *GBK* 字节序列
 *   - 编译期用 /utf-8，所以 g_strings[] 字面量是 UTF-8 字节
 *   - 运行时调 MultiByteToWideChar(CP_UTF8) + WideCharToMultiByte(CP_ACP) 转换为 GBK
 *   - GBK 结果缓存到 g_gbk[][]，首次访问后零开销
 *   - 调用点（MessageBoxA / AppendMenuA / CreateWindowExA / SetDlgItemTextA 等
 *     ANSI 接口）拿到的就是 Windows ANSI 代码页（中文 = CP936 / GBK）字节
 *
 * 线程安全：DLL 主线程 + UI 消息循环都跑在同一线程，单线程下缓存读写安全。
 */
#include "vbmcp.h"
#include "lang.h"
#include <string.h>

/* 注册表路径：与 1.reg 实际内容一致（用户也确认）。 */
#define VBMCP_LANG_REG_KEY   "SOFTWARE\\VB and VBA Program Settings\\VB Decompiler\\Options"
#define VBMCP_LANG_REG_VALUE "CustomLanguage"
#define VBMCP_LANG_ZH_TAG    "ChineseSimplified"

/* 当前缓存的语言（线程不安全：单线程插件，符合预期）。 */
static vbmcp_lang_t g_lang = VBMCP_LANG_EN;

/* ============================================================
 * 字符串资源表（源文件编码：UTF-8）
 * 顺序与 lang.h 中 enum T_* 严格一致。
 * 任意一侧为 NULL 则退回另一侧。
 * ============================================================ */
static const struct { const char *en; const char *zh; } g_strings[T_COUNT] = {
    [T_PLUGIN_NAME]      = { "VB Decompiler MCP Server",
                            "VB Decompiler MCP 服务器" },

    [T_PLUGIN_AUTHOR]    = { "vbmcp project, admin@vb-decompiler.org",
                            "vbmcp 项目, admin@vb-decompiler.org" },

    [T_MENU_SETTINGS]   = { "VB Decompiler MCP Server Settings...",
                            "VB Decompiler MCP 服务器设置..." },

    [T_DLG_TITLE]       = { "VB Decompiler MCP Server Settings",
                            "VB Decompiler MCP 服务器设置" },

    [T_CHK_ENABLE]      = { "Enable MCP Server",
                            "启用 MCP 服务器" },

    [T_LBL_BIND]        = { "Bind IP address:",
                            "绑定 IP 地址:" },

    [T_LBL_PORT]        = { "Port:",
                            "端口号:" },

    [T_LBL_STATUS_RUNNING]  = { "running",  "运行中" },
    [T_LBL_STATUS_STOPPED]  = { "stopped",  "已停止" },

    [T_LBL_AI]          = { "AI Client:",
                            "AI 客户端:" },

    [T_BTN_COPY]        = { "Copy config",   "复制配置" },
    [T_BTN_RESTORE]     = { "Restore defaults", "恢复默认设置" },
    [T_BTN_HELP]        = { "Help",          "帮助" },
    [T_BTN_OK]          = { "OK",            "确定" },
    [T_BTN_CANCEL]      = { "Cancel",        "取消" },
    [T_BTN_COPIED]      = { "Copied",        "已复制" },

    [T_GUIDE_BRACKET_OPEN]  = { "[",  "["},
    [T_GUIDE_BRACKET_CLOSE] = { "]",  "]"},

    [T_GUIDE_PREFACE]   = {
        "Before configuring: make sure \"Enable MCP Server\" is checked and click OK above so the service is running.\n",
        "配置前：请确保上方\"启用 MCP 服务器\"已勾选并点\"确定\"，服务处于运行中。\n" },

    [T_GUIDE_NOTE]      = { "Note:\n",        "注意：\n" },
    [T_GUIDE_OPS_HEADER]= { "Steps:\n",       "操作步骤：\n" },
    [T_GUIDE_CONFIG_HEADER] = {
        "Config snippet (click \"Copy config\" on the right):\n",
        "配置片段 (可点右侧\"复制配置\")：\n" },

    [T_ERR_PORT_IN_USE] = {
        "MCP Server failed to start on port %d: port may be in use. Try another port.",
        "MCP 服务器无法在端口 %d 启动：端口可能被占用。请换一个端口。" },

    [T_ERR_CLIPBOARD]   = { "Cannot access clipboard.",
                            "无法访问剪贴板。" },

    [T_ERR_REG_CLASS]   = { "Failed to register window class.",
                            "注册窗口类失败。" },

    [T_ERR_CREATE_DLG]  = { "Failed to create settings window.",
                            "创建设置窗体失败。" },

    [T_HELP_TITLE]      = { VBMCP_PLUGIN_NAME " Help",
                            VBMCP_PLUGIN_NAME " 帮助" },

    [T_HELP_BODY]       = {
        "VB Decompiler MCP Server Plugin v" VBMCP_VERSION "\n\n"
        "This plugin runs as an MCP (Model Context Protocol) server (Streamable HTTP),\n"
        "allowing AI clients to read/write VB Decompiler-decompiled code and internal\n"
        "data via the JSON-RPC 2.0 interface.\n\n"
        "Tools available: 28 (including get_project, set_function, list_modules, etc.).\n\n"
        "Debug log (when VBMCP_DEBUG_LOG=1):\n"
        "  %TEMP%\\vbmcp.log\n",

        "VB Decompiler MCP Server 插件 v" VBMCP_VERSION "\n\n"
        "本插件作为 MCP（Model Context Protocol）服务器（Streamable HTTP）运行，\n"
        "使 AI 客户端能通过 JSON-RPC 2.0 接口读取 / 修改由 VB Decompiler 反编译\n"
        "生成的代码与内部数据结构。\n\n"
        "可用工具数：28（含 get_project、set_function、list_modules 等）。\n\n"
        "调试日志（启用 VBMCP_DEBUG_LOG=1 后）：\n"
        "  %TEMP%\\vbmcp.log\n" },

    [T_LOG_RESTORED]    = { "settings restored to defaults",
                            "设置已恢复为默认" },

    [T_LOG_SETTINGS_OK] = { "apply settings ok: enable=%d port=%d bind=%s",
                            "设置已应用：启用=%d 端口=%d 绑定=%s" },

    [T_LOG_LANG]        = { "current language: %s",
                            "当前语言：%s" },

    /* ---- MCP 工具文本（mcp_tools.c；经 JSON-RPC/HTTP UTF-8 返回）---- */
    [T_ERR_OOM] = { "Out of memory", "内存不足" },
    [T_ERR_NO_PROJECT] = { "No project is currently open",
                           "当前没有打开的项目" },
    [T_ERR_MISSING_PROJECT_TEXT] = { "Missing required parameter: project_text",
                                     "缺少 project_text 参数" },
    [T_ERR_PROJECT_TEXT_TOO_LARGE] = {
        "project_text exceeds the 16MB limit (actual %zu bytes); please write it in chunks",
        "project_text 超过 16MB 上限（实际 %zu 字节），请分段写入" },
    [T_ERR_MISSING_ARGS] = { "Missing arguments object", "缺少参数对象" },
    [T_ERR_BAD_FORM_INDEX] = { "Missing or invalid form_index",
                               "缺少或非法的 form_index" },
    [T_ERR_CANNOT_READ_FORM] = { "Cannot read form content",
                                 "无法读取表单内容" },
    [T_ERR_MISSING_FORM_TEXT] = { "Missing form_text", "缺少 form_text" },
    [T_ERR_MISSING_NEW_NAME] = { "Missing new_name", "缺少 new_name" },
    [T_ERR_NATIVE_RENAME_FORM] = {
        "rename_form is not supported on native-compiled projects "
        "(it would trigger a modal dialog that blocks the RPC call). "
        "Use it on a source-form VB project.",
        "rename_form 不支持 native 编译项目（会触发 modal 弹窗阻塞 RPC）。请在源码形式 VB 项目上使用。" },
    [T_ERR_BAD_MODULE_INDEX] = { "Missing or invalid module_index",
                                 "缺少或非法的 module_index" },
    [T_ERR_MISSING_DECLARATIONS] = { "Missing declarations", "缺少 declarations" },
    [T_ERR_MISSING_STRING_REFERENCES] = { "Missing string_references",
                                          "缺少 string_references" },
    [T_ERR_BAD_MODULE_FUNCTION_INDEX] = { "Missing or invalid module_index/function_index",
                                          "缺少或非法的 module_index/function_index" },
    [T_ERR_MISSING_CODE] = { "Missing code", "缺少 code" },
    [T_ERR_NATIVE_RENAME_FUNCTION] = {
        "rename_function is not supported on native-compiled projects "
        "(it would trigger a modal dialog that blocks the RPC call). "
        "Use it on a source-form VB project.",
        "rename_function 不支持 native 编译项目（会触发 modal 弹窗阻塞 RPC）。请在源码形式 VB 项目上使用。" },
    [T_ERR_MISSING_TEXT] = { "Missing text", "缺少 text" },
    [T_ERR_BAD_LINE] = { "Missing or invalid line", "缺少或非法的 line" },
    [T_ERR_EMPTY_TOOL_NAME] = { "Tool name is empty", "工具名为空" },
    [T_ERR_PROJECT_NOT_LOADED] = {
        "Project not loaded. The VB SDK provides no ReloadFile/Refresh API, "
        "so vbmcp cannot re-initialize the engine from the DLL side. "
        "Please open the vbmcp settings window from the Plugins menu in the "
        "VB Decompiler main window and click OK "
        "(this triggers a VBDecompilerPluginLoad re-entry). Tool called: %s",
        "项目未加载。VB SDK 不提供 ReloadFile/Refresh API，"
        "vbmcp 无法从 DLL 侧自动重新初始化 engine。"
        "请在 VB Decompiler 主窗口的 Plugins 菜单里打开 vbmcp "
        "配置窗口并点确定（会触发 VBDecompilerPluginLoad 重入）。"
        "调用工具: %s" },
    [T_ERR_UNKNOWN_TOOL] = { "Unknown tool: %s", "未知工具: %s" },

    [T_DESC_LIMIT] = { "Maximum number of items to return", "最多返回多少条" },
    [T_DESC_OFFSET] = { "Start offset", "起始偏移" },
    [T_DESC_PROJECT_INFO] = {
        "Get metadata of the project currently open in VB Decompiler: "
        "source file path, whether it is natively compiled, compiler type "
        "(VB/.NET/Delphi/Unknown), whether it is packed, and the VB Decompiler "
        "installation path. No parameters.",
        "获取当前VB Decompiler打开的项目元信息：源文件路径、是否Native编译、编译器类型(VB/.NET/Delphi/Unknown)、是否打包、VB Decompiler安装路径。无参数。" },
    [T_DESC_PROJECT_TEXT] = {
        "Get the complete decompiled text of the entire project. Returns a "
        "large string (possibly very long, containing all Forms/Modules/Functions).",
        "获取整个反编译项目的完整文本内容。返回大字符串（可能非常长，含所有Form/Module/Function）。" },
    [T_DESC_SET_PROJECT_TEXT] = {
        "Completely replace the project text with new text. Automatically "
        "calls UpdateAll to commit changes and refresh the GUI.",
        "用新文本完全替换项目文本。会自动 UpdateAll 提交修改并刷新GUI。" },
    [T_DESC_PARAM_PROJECT_TEXT] = { "Full replacement project text (required)",
                                    "替换后的完整项目文本（必填）" },
    [T_DESC_CLEAR_BUFFERS] = {
        "Clear VB Decompiler's internal decompilation buffers. Use only when "
        "implementing a custom full decompilation workflow; discards all "
        "existing decompilation results.",
        "清空VB Decompiler的内部反编译缓冲。仅在实现自定义完整反编译工作流时使用，会丢弃所有现有反编译结果。" },
    [T_DESC_LIST_FORMS] = {
        "List all VB Forms in the project. Supports pagination (limit/offset). "
        "Returns {count, forms:[...]}.",
        "列出项目中的所有 VB Form。支持分页（limit/offset）。返回 {count, forms:[...]}。" },
    [T_DESC_GET_FORM] = {
        "Get the decompiled property text of the specified Form. "
        "Returns {form_index, form_name, form_text}.",
        "获取指定 Form 的反编译属性文本。返回 {form_index, form_name, form_text}。" },
    [T_DESC_PARAM_FORM_INDEX] = { "Form index (0-based)", "Form 索引（从 0 开始）" },
    [T_DESC_SET_FORM] = {
        "Replace the property text of the specified Form. "
        "Automatically calls UpdateAll.",
        "替换指定 Form 的属性文本。会自动 UpdateAll。" },
    [T_DESC_RENAME_FORM] = { "Rename a Form.", "重命名 Form。" },
    [T_DESC_LIST_MODULES] = {
        "List all Modules in the project (standard .bas, class .cls, "
        "control .ctl). Supports pagination. Returns {count, modules:[...]}.",
        "列出项目中的所有 Module（标准模块.bas、类模块.cls、控件.ctl）。支持分页。返回 {count, modules:[...]}。" },
    [T_DESC_GET_MODULE] = {
        "Get the module's declarations header (all function/constant/type "
        "declarations) and string references.",
        "获取模块的 declarations 头部（含所有函数/常量/类型声明）和字符串引用。" },
    [T_DESC_SET_MODULE] = { "Replace the module's declarations header.",
                            "替换模块 declarations 头部。" },
    [T_DESC_LIST_MODULE_FUNCTIONS] = {
        "List all functions in the specified module. Supports pagination. "
        "Returns {module_index, count, functions:[{name,address,index}...]}.",
        "列出指定模块的所有函数。支持分页。返回 {module_index, count, functions:[{name,address,index}...]}。" },
    [T_DESC_GET_FUNCTION] = {
        "Get a function's decompiled code plus multiple views "
        "(fast/disassembly/string references). Returns {module_index, "
        "function_index, name, address, code, code_fast, disassembly, string_references}.",
        "获取函数反编译代码 + 多种视图（fast/反汇编/字符串引用）。返回 {module_index, function_index, name, address, code, code_fast, disassembly, string_references}。" },
    [T_DESC_SET_FUNCTION] = { "Replace a function's code.", "替换函数代码。" },
    [T_DESC_RENAME_FUNCTION] = { "Rename a function.", "重命名函数。" },
    [T_DESC_GET_SUB_MAIN] = {
        "Get the code of the program entry point Sub Main. No parameters.",
        "获取程序入口 Sub Main 的代码。无参数。" },
    [T_DESC_SET_SUB_MAIN] = { "Set the Sub Main code.", "设置 Sub Main 代码。" },
    [T_DESC_GET_ACTIVE_TEXT] = {
        "Get the text of the currently active code window (editor).",
        "获取当前激活代码窗口（编辑器）的文本。" },
    [T_DESC_SET_ACTIVE_TEXT] = {
        "Set the text of the currently active code window. "
        "Automatically calls UpdateAll.",
        "设置当前激活代码窗口的文本。会自动 UpdateAll。" },
    [T_DESC_GET_ACTIVE_DISASM_TEXT] = {
        "Get the raw text of the active disassembly window.",
        "获取激活反汇编窗口的原始文本。" },
    [T_DESC_SET_ACTIVE_DISASM_TEXT] = {
        "Set the text of the active disassembly window.",
        "设置激活反汇编窗口文本。" },
    [T_DESC_GOTO_LINE] = {
        "Jump to the specified line in the code window "
        "(UI only, does not modify text).",
        "跳到代码窗口的指定行号（仅 UI，不改文本）。" },
    [T_DESC_PARAM_LINE] = { "Target line number (0-based)", "目标行号（从 0 开始）" },
    [T_DESC_GET_ACTIVE_COORDS] = {
        "Get the module/function coordinates of the currently active view "
        "(e.g. \"ModuleIndex,FunctionIndex\").",
        "获取当前激活视图的模块/函数坐标（如 \"ModuleIndex,FunctionIndex\"）。" },
    [T_DESC_SET_STATUS] = {
        "Set the text of VB Decompiler's bottom status bar "
        "(used to show MCP operation progress).",
        "设置VB Decompiler底栏状态栏文本（用于显示MCP操作进度）。" },
    [T_DESC_SET_OPTIONS] = {
        "Control decompilation options: the \"Parse stack parameters\" and "
        "\"Procedure analyzer and optimizer\" checkboxes. Omitting either "
        "parameter leaves the corresponding option unchanged.",
        "控制反编译选项：\"Parse stack parameters\"和\"Procedure analyzer and optimizer\"复选框。任一参数省略 → 不改变对应选项。" },
    /* 注意：实际内容为 \"0\"=关 \"1\"=开（JSON 转义引号），与旧源码逐字节一致 */
    [T_DESC_PARAM_ONOFF] = { "\\\"0\\\"=off \\\"1\\\"=on",
                             "\\\"0\\\"=关 \\\"1\\\"=开" },
    [T_DESC_LIST_FRX_ICONS] = {
        "List icons/images in the Form's FRX file: "
        "{count, icons:[{index,offset,size}]}. Supports pagination.",
        "列出 Form FRX 文件中的图标/图像：{count, icons:[{index,offset,size}]}。支持分页。" },
    [T_DESC_UPDATE_ALL] = {
        "Commit all Set modifications and refresh the GUI (tools normally "
        "call this automatically; call manually only after modifying large "
        "amounts of data externally).",
        "提交所有 Set 修改并刷新 GUI（一般工具内部已自动调用，仅在外部修改大量数据后需要时手动调用）。" },
};

/* ============================================================
 * UTF-8 → GBK 转换缓存（按需动态分配）
 *   - g_gbk[key][0]=GBK 英文 / g_gbk[key][1]=GBK 中文（动态 malloc，长度按需）
 *   - g_gbk_ready[key][lang] = 是否已转换
 *
 * 之前用静态 512B 固定 buffer：拼接 steps+config+headers 时被截断，
 * 文本框里的向导内容"成一行"。现改为按字符串实际长度 malloc，永不截断。
 *
 * 转换失败回退到原 UTF-8 字节（虽然错，但不崩溃；用户能看见乱码便于排查）。
 * 进程退出时 CRT 统一回收，DllMain 不必主动清理。
 * ============================================================ */
static char *g_gbk[T_COUNT][2] = { NULL };
static BOOL g_gbk_ready[T_COUNT][2] = { FALSE };

/* 转换一次：UTF-8 字符串 → GBK。结果缓存到 g_gbk[key][idx]（vbmcp_alloc 申请）。 */
static const char *ensure_gbk(int key, int idx)
{
    if (g_gbk_ready[key][idx]) return g_gbk[key][idx];

    const char *src = (idx == 1) ? g_strings[key].zh : g_strings[key].en;
    if (!src) return NULL;

    /* ASCII fast path：所有字节都 < 0x80，GBK 与 ASCII 兼容。 */
    int all_ascii = 1;
    for (const char *p = src; *p; p++) {
        if ((unsigned char)*p >= 0x80) { all_ascii = 0; break; }
    }
    if (all_ascii) {
        g_gbk[key][idx] = vbmcp_strdup(src);
        g_gbk_ready[key][idx] = TRUE;
        return g_gbk[key][idx];
    }

    /* UTF-8 → UTF-16 → ANSI 代码页（中文 Windows = CP936 / GBK） */
    size_t src_len = strlen(src);
    wchar_t *wbuf = (wchar_t *)vbmcp_alloc((src_len + 1) * sizeof(wchar_t));
    if (!wbuf) return src;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, src, -1, wbuf, (int)(src_len + 1));
    if (wlen <= 0) {
        vbmcp_free(wbuf);
        return src;
    }

    /* UTF-16 → GBK：UTF-16 字符数 × 2 + NUL 字节足够（实际 GBK 不会超过） */
    size_t need = (size_t)(wlen - 1) * 2 + 16;
    char *out = (char *)vbmcp_alloc(need);
    if (!out) { vbmcp_free(wbuf); return src; }
    int glen = WideCharToMultiByte(CP_ACP, 0, wbuf, wlen - 1,
                                   out, (int)(need - 1), NULL, NULL);
    vbmcp_free(wbuf);
    if (glen <= 0) {
        vbmcp_free(out);
        return src;
    }
    out[glen] = '\0';

    g_gbk[key][idx] = out;
    g_gbk_ready[key][idx] = TRUE;
    return out;
}

const char *vbmcp_t(int key)
{
    if (key < 0 || key >= T_COUNT) return "";
    vbmcp_lang_t lang = vbmcp_lang_current();
    int idx = (lang == VBMCP_LANG_ZH) ? 1 : 0;
    const char *p = ensure_gbk(key, idx);
    if (!p) {
        /* 当前语言字符串缺失 → 退回另一种语言 */
        int alt = idx ^ 1;
        p = ensure_gbk(key, alt);
    }
    return p ? p : "";
}

/* UTF-8 取串（不转 GBK）：供 MCP JSON-RPC / HTTP application/json 使用。
 * g_strings[] 源文件即 UTF-8，直接返回字面量字节；缺失时退回另一种语言。 */
const char *vbmcp_t_utf8(int key)
{
    if (key < 0 || key >= T_COUNT) return "";
    vbmcp_lang_t lang = vbmcp_lang_current();
    const char *p = (lang == VBMCP_LANG_ZH) ? g_strings[key].zh : g_strings[key].en;
    if (!p) {
        p = (lang == VBMCP_LANG_ZH) ? g_strings[key].en : g_strings[key].zh;
    }
    return p ? p : "";
}

vbmcp_lang_t vbmcp_lang_current(void)
{
    return g_lang;
}

vbmcp_lang_t vbmcp_lang_refresh(void)
{
    char val[64] = {0};
    DWORD cb = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegGetValueA(HKEY_CURRENT_USER, VBMCP_LANG_REG_KEY,
                              VBMCP_LANG_REG_VALUE, RRF_RT_REG_SZ,
                              &type, val, &cb);
    if (st == ERROR_SUCCESS && val[0]) {
        g_lang = (_stricmp(val, VBMCP_LANG_ZH_TAG) == 0)
                 ? VBMCP_LANG_ZH : VBMCP_LANG_EN;
    } else {
        g_lang = VBMCP_LANG_EN;
    }
    vbmcp_log("language refreshed: %s (reg='%s', err=%ld)",
              g_lang == VBMCP_LANG_ZH ? "zh" : "en",
              val, st);
    return g_lang;
}

/* ============================================================
 * 通用 UTF-8 → ANSI 代码页（GBK on 中文 Windows）转换。
 * 用于把 options_dialog.c 中的 UTF-8 源码字符串（kAiGuides 中文段）
 * 转为 ANSI 字节序列，再与 vbmcp_t() 返回的 GBK 拼接给 SetDlgItemTextA。
 *
 * 返回 vbmcp_alloc 申请的新字符串（调用方 vbmcp_free）。
 * 失败回退到 vbmcp_strdup(src)，用户仍能看到原始字节（乱码但不崩）。
 *
 * ASCII fast path：所有字节 < 0x80 → 直接 strdup 复制（GBK 兼容 ASCII）。
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

    /* UTF-16 → GBK：UTF-16 字符数 × 2 + NUL 字节足够 */
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