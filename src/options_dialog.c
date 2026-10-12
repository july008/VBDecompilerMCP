/*
 * options_dialog.c - 设置窗体（双语 EN / ZH）
 *
 * 仿照 HardCodedStringMCPServer 的 SettingsForm：
 *   - 启用 MCP 服务器复选框
 *   - 绑定 IP 地址（只读）
 *   - 端口号微调按钮 1-65535
 *   - 服务器状态文本（实时反映 vbmcp_server_is_running）
 *   - AI 客户端下拉 + 复制配置 + 右侧说明多行文本框
 *   - 底部：恢复默认设置 / 帮助 / 确定 / 取消
 *
 * 不使用 RC/Dialog 模板，纯 CreateWindowEx 编码。
 * 全部用户可见文本通过 vbmcp_t() 获取当前语言版本；AI 客户端的
 * config / steps 段落每个客户端都有 en + zh 两份。
 *
 * 可重入性：单实例，重复点击菜单只前置已开窗体。
 */
#include "vbmcp.h"
#include "strbuf.h"
#include "lang.h"
#include <stdio.h>
#include <strsafe.h>
#include <shlobj.h>     /* CF_TEXT 剪贴板 */

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

/* ============================================================
 * AI 客户端接入向导（双语）
 * name 用产品名（始终英文）。
 * config_xx / steps_xx：当前语言对应版本，含 @url@ 占位。
 * ============================================================ */
typedef struct {
    const char *name;
    const char *config_en;
    const char *config_zh;
    const char *steps_en;
    const char *steps_zh;
} ai_guide_t;

#define URL_PLACEHOLDER "@url@"

static const ai_guide_t kAiGuides[] = {
    {
        "CodeBuddy",
        /* config (EN) */
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\",\n"
        "      \"disabled\": false\n"
        "    }\n"
        "  }\n"
        "}",
        /* config (ZH) */
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\",\n"
        "      \"disabled\": false\n"
        "    }\n"
        "  }\n"
        "}",
        /* steps (EN) */
        "File location: %USERPROFILE%\\.codebuddy\\mcp.json (user-level; create if missing)\n"
        "If the file already contains other mcpServers entries, merge this snippet; do not overwrite the whole file.\n"
        "\n"
        "Note:\n"
        "- \"type\": \"http\" means Streamable HTTP, which is what this server provides.\n"
        "- Older CodeBuddy builds may only recognize \"streamable-http\"; replace \"http\" with that if your build rejects it.\n"
        "\n"
        "Steps:\n"
        "1. Quit CodeBuddy first (so the file is not overwritten);\n"
        "2. Open the mcp.json in a text editor and merge this snippet;\n"
        "3. Save and restart CodeBuddy;\n"
        "4. Confirm \"vbmcp\" shows as connected in CodeBuddy's MCP panel.\n"
        "\n"
        "Docs: https://www.codebuddy.cn -> help center (MCP section)",
        /* steps (ZH) */
        "文件位置：%USERPROFILE%\\.codebuddy\\mcp.json（用户级，不存在则新建）\n"
        "若文件已含其他 mcpServers 条目，请只合并本片段，切勿整篇覆盖。\n"
        "\n"
        "注意：\n"
        "- \"type\": \"http\" 即 Streamable HTTP，本服务器正是该类型。\n"
        "- 较旧版本可能只识别 \"streamable-http\"；如安装版本报错，请改回 streamable-http。\n"
        "\n"
        "操作步骤：\n"
        "1. 先关闭 CodeBuddy（避免配置被覆盖）；\n"
        "2. 用文本编辑器打开上述 mcp.json，合并本片段；\n"
        "3. 保存后重启 CodeBuddy；\n"
        "4. 在 CodeBuddy 的 MCP 管理面板中确认 vbmcp 显示已连接。\n"
        "\n"
        "文档：https://www.codebuddy.cn → 帮助中心（MCP 章节）"
    },
    {
        "VS Code",
        "{\n"
        "  \"servers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "{\n"
        "  \"servers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "File location: <project-root>\\.vscode\\mcp.json (workspace-level, shared with the project; recommended)\n"
        "\n"
        "Note:\n"
        "- The top-level key in VS Code's mcp.json is \"servers\" (NOT \"mcpServers\");\n"
        "- type \"http\" already means Streamable HTTP, which is what this server provides.\n"
        "\n"
        "Steps:\n"
        "1. Make sure your VS Code supports MCP (Copilot Chat / MCP since 2025);\n"
        "2. Create .vscode\\mcp.json under the project root and paste this snippet (escape backslashes as \\\\);\n"
        "3. Run \"MCP: Reload Servers\" from the command palette, or reload the window;\n"
        "4. Confirm vbmcp status is connected in the MCP panel.\n"
        "\n"
        "Docs: https://code.visualstudio.com/docs/copilot/chat/mcp-servers",
        "文件位置：项目根目录\\.vscode\\mcp.json（工作区级，随项目共享；推荐）\n"
        "\n"
        "注意：\n"
        "- VS Code 的 mcp.json 顶层键是 \"servers\"（不是 \"mcpServers\"）；\n"
        "- type 填 \"http\" 即 Streamable HTTP，本服务器正是该类型。\n"
        "\n"
        "操作步骤：\n"
        "1. 确保当前 VS Code 支持 MCP（2025 版起的 Copilot Chat / MCP 工具支持）；\n"
        "2. 在项目根目录创建 .vscode\\mcp.json，粘贴本片段（注意转义反斜杠为 \\\\）；\n"
        "3. 命令面板执行 \"MCP: Reload Servers\" 或重启窗口；\n"
        "4. 在 MCP 面板确认 vbmcp 状态为已连接。\n"
        "\n"
        "文档：https://code.visualstudio.com/docs/copilot/chat/mcp-servers"
    },
    {
        "Cursor",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "Option 1 (UI, recommended): Settings -> MCP -> + New Server\n"
        "  - Type: HTTP(S) / Streamable HTTP\n"
        "  - URL: " URL_PLACEHOLDER "\n"
        "Option 2 (file): <project-root>\\.cursor\\mcp.json (same snippet). After saving,\n"
        "  manually Enable it once in Cursor's MCP list.\n"
        "\n"
        "Steps:\n"
        "1. After adding, the tools list in the Cursor chat should include vbmcp;\n"
        "2. If the list is empty, restart Cursor or reload the window.\n"
        "\n"
        "Docs: https://docs.cursor.com (search \"Model Context Protocol\")",
        "方式一（界面添加，推荐）：Settings -> MCP -> + New Server\n"
        "  - Type: 选 HTTP(S) / Streamable HTTP\n"
        "  - URL: 填 " URL_PLACEHOLDER "\n"
        "方式二（文件）：项目根目录\\.cursor\\mcp.json（同上片段），保存后\n"
        "  在 Cursor 设置中 MCP 列表里手动 Enable 一次。\n"
        "\n"
        "操作步骤：\n"
        "1. 添加后回到 Cursor 对话框，工具列表中应出现 vbmcp；\n"
        "2. 若列表空白，尝试重启 Cursor 或重新加载窗口。\n"
        "\n"
        "文档：https://docs.cursor.com （搜索 \"Model Context Protocol\"）"
    },
    {
        "Trae",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "Option 1 (UI, recommended): Trae Settings -> MCP Servers -> Add Server\n"
        "  - Transport: HTTP(S) (Streamable HTTP)\n"
        "  - URL: " URL_PLACEHOLDER "\n"
        "Option 2 (file): project-level .trae\\mcp.json or user-level config.\n"
        "  Use the key name documented by your Trae version (usually compatible with mcpServers).\n"
        "\n"
        "Steps: After adding, confirm the status is \"connected\", then call the tools directly in chat.\n"
        "\n"
        "Docs: https://www.trae.ai/docs (search \"MCP\" / \"MCP server\")",
        "方式一（界面添加，推荐）：Trae 设置 -> MCP 服务器 -> 添加服务器\n"
        "  - 传输类型: HTTP(S) (Streamable HTTP)\n"
        "  - 地址: " URL_PLACEHOLDER "\n"
        "方式二（文件）：项目级 .trae\\mcp.json 或用户级配置，键名以 Trae\n"
        "  当前版本说明为准（一般与 mcpServers 兼容）。\n"
        "\n"
        "操作步骤：添加后确认列表中状态为已连接，然后在对话中直接调用相关工具。\n"
        "\n"
        "文档：https://www.trae.ai/docs （搜索 \"MCP\" / \"MCP server\"）"
    },
    {
        "WorkBuddy",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "File location:\n"
        "- User-level: %USERPROFILE%\\.workbuddy\\mcp.json (shared by all projects)\n"
        "- Project-level: <project-dir>\\.workbuddy\\mcp.json (current project only)\n"
        "If the file already has other mcpServers entries, only merge this snippet.\n"
        "\n"
        "Steps:\n"
        "1. Quit WorkBuddy;\n"
        "2. Open the mcp.json above, merge this snippet and save;\n"
        "3. Restart WorkBuddy; confirm vbmcp is connected in the MCP panel.\n"
        "\n"
        "Note: WorkBuddy's MCP config keys and file paths can differ between\n"
        "minor versions. If anything fails, check WorkBuddy's official docs\n"
        "or Settings -> MCP for the current schema.",
        "文件位置：\n"
        "- 用户级：%USERPROFILE%\\.workbuddy\\mcp.json（所有项目复用）\n"
        "- 项目级：<项目目录>\\.workbuddy\\mcp.json（仅当前项目）\n"
        "若文件已含其他 mcpServers 条目，请只合并本片段。\n"
        "\n"
        "操作步骤：\n"
        "1. 关闭 WorkBuddy；\n"
        "2. 打开上述 mcp.json，合并本片段并保存；\n"
        "3. 重启 WorkBuddy，在 MCP 连接器/服务器面板中确认已连接。\n"
        "\n"
        "提示：WorkBuddy 不同小版本的 MCP 配置键名/文件路径可能不同，若\n"
        "配置未生效，请以 WorkBuddy 当前版本官方文档或 设置 -> MCP 中\n"
        "显示的字段为准。"
    },
    {
        "Claude Desktop",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbmcp\": {\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"" URL_PLACEHOLDER "\"\n"
        "    }\n"
        "  }\n"
        "}",
        "File location: %APPDATA%\\Claude\\claude_desktop_config.json\n"
        "You can also open it via Claude -> Settings -> Developer -> Edit Config.\n"
        "\n"
        "Note: This server is Streamable HTTP, so type \"http\" + url is enough (no command/args required).\n"
        "\n"
        "Steps:\n"
        "1. Edit the config file above and merge this snippet;\n"
        "2. Fully quit and restart Claude Desktop;\n"
        "3. Confirm the tools are available in the chat (or vbmcp shows up under Settings -> Installed connectors).\n"
        "\n"
        "Docs: https://support.anthropic.com (search \"Claude Desktop MCP\")",
        "文件位置：%APPDATA%\\Claude\\claude_desktop_config.json\n"
        "也可通过 Claude -> Settings -> Developer -> Edit Config 打开。\n"
        "\n"
        "注意：本服务器为 Streamable HTTP，type 填 \"http\" 并给 url 即可（无需 command/args）。\n"
        "\n"
        "操作步骤：\n"
        "1. 编辑上述配置文件，合并本片段；\n"
        "2. 完全退出并重启 Claude Desktop；\n"
        "3. 在对话中确认工具可用（或 Settings -> 已安装连接器里看到 vbmcp）。\n"
        "\n"
        "文档：https://support.anthropic.com （搜索 \"Claude Desktop MCP\"）"
    },
    {
        "Claude Code",
        /* config (EN) */
        "# Modern syntax (Claude Code 2025+):\n"
        "claude mcp add vbmcp --transport http \"" URL_PLACEHOLDER "\"\n"
        "\n"
        "# Legacy syntax (Claude Code 2024):\n"
        "claude mcp add vbmcp \"" URL_PLACEHOLDER "\"",
        /* config (ZH) */
        "# 推荐用 CLI 添加（写入 ~/.claude.json，自动生效）：\n"
        "# 现代语法（Claude Code 2025+）：\n"
        "claude mcp add vbmcp --transport http \"" URL_PLACEHOLDER "\"\n"
        "# 旧语法（Claude Code 2024）：\n"
        "claude mcp add vbmcp \"" URL_PLACEHOLDER "\"",
        /* steps (EN) */
        "Steps:\n"
        "1. Run one of the commands above in a terminal;\n"
        "2. If `claude mcp add` rejects the syntax, run `claude mcp add --help`\n"
        "   and pick the form that matches your installed version;\n"
        "3. Verify with: claude mcp list;\n"
        "4. Inside a Claude Code session use /mcp to view/manage servers.\n"
        "\n"
        "Docs: https://docs.claude.com/en/docs/claude-code/mcp",
        /* steps (ZH) */
        "操作步骤：\n"
        "1. 在终端执行上面任一命令；\n"
        "2. 若报错不接受 --transport 等参数，请先执行 claude mcp add --help\n"
        "   确认本机版本的参数名/顺序，再选用对应语法；\n"
        "3. 用 claude mcp list 查看是否已注册；\n"
        "4. 在 Claude Code 会话内用 /mcp 可查看/管理服务器。\n"
        "\n"
        "文档：https://docs.claude.com/en/docs/claude-code/mcp"
    },
    {
        "OpenAI Codex",
        "[mcp_servers.vbmcp]\n"
        "type = \"http\"\n"
        "url = \"" URL_PLACEHOLDER "\"",
        "[mcp_servers.vbmcp]\n"
        "type = \"http\"\n"
        "url = \"" URL_PLACEHOLDER "\"",
        "Option 1 (CLI, recommended):\n"
        "  codex mcp add vbmcp -- " URL_PLACEHOLDER "\n"
        "Option 2 (manual config.toml):\n"
        "  File location: %USERPROFILE%\\.codex\\config.toml (user-level)\n"
        "  or <project-dir>\\.codex\\config.toml (project-level)\n"
        "  Append the TOML snippet on the left to the end of the file.\n"
        "\n"
        "Steps: Save, restart codex; use /mcp in the session to confirm connection status.\n"
        "\n"
        "If `codex mcp add` errors out, run codex mcp add --help to confirm\n"
        "the exact argument order supported by your installed version.\n"
        "\n"
        "Docs: https://github.com/openai/codex (README -> MCP section)",
        "方式一（CLI，推荐）：\n"
        "  codex mcp add vbmcp -- " URL_PLACEHOLDER "\n"
        "方式二（手写 config.toml）：\n"
        "  文件位置：%USERPROFILE%\\.codex\\config.toml（用户级）\n"
        "  或 <项目目录>\\.codex\\config.toml（项目级）\n"
        "  在文件末尾追加左侧 TOML 片段。\n"
        "\n"
        "操作步骤：保存后重启 codex，会话内可用 /mcp 确认连接状态。\n"
        "\n"
        "若 codex mcp add 报错，请先执行 codex mcp add --help 确认本机版本\n"
        "支持的参数顺序，再选用对应语法。\n"
        "\n"
        "文档：https://github.com/openai/codex （README -> MCP 章节）"
    }
};

#define AI_GUIDES_COUNT (sizeof(kAiGuides)/sizeof(kAiGuides[0]))

/* ============================================================
 * 控件 ID
 * ============================================================ */
enum {
    IDC_CHK_ENABLE     = 1001,
    IDC_TXT_BIND       = 1002,
    IDC_SPIN_PORT      = 1003,
    IDC_EDIT_PORT      = 1004,
    IDC_LBL_STATUS     = 1005,
    IDC_CBO_AI         = 1006,
    IDC_BTN_COPY       = 1007,
    IDC_TXT_GUIDE      = 1008,
    IDC_BTN_RESTORE    = 1009,
    IDC_BTN_HELP       = 1010,
    IDC_BTN_OK         = 1011,
    IDC_BTN_CANCEL     = 1012,
};

/* 单实例：避免重复打开 */
static HWND g_options_hwnd = NULL;

/* 自定义字体：Segoe UI 9pt（Windows Vista+ 默认）。比 "MS Shell Dlg"
 * 细一些，更适合现代 UI。FW_NORMAL / FW_REGULAR 不用粗体。 */
static HFONT g_hUiFont = NULL;

/* 窗体背景刷：COLOR_BTNFACE（标准灰色），与系统对话框一致；
 * 同时给 STATIC/EDIT 提供背景色，让所有控件背景统一。 */
static HBRUSH g_hBgBrush = NULL;

/* ============================================================
 * 字符串工具：替换
 * ============================================================ */

static char *replace_url_placeholder(const char *src, const char *url)
{
    if (!src) return NULL;
    if (!url) url = "";
    size_t src_len = strlen(src);
    size_t ph_len  = strlen(URL_PLACEHOLDER);
    if (ph_len == 0 || src_len < ph_len) {
        char *r = (char *)vbmcp_alloc(src_len + 1);
        if (!r) return NULL;
        memcpy(r, src, src_len + 1);
        return r;
    }
    size_t count = 0;
    const char *p = src;
    while ((p = strstr(p, URL_PLACEHOLDER)) != NULL) { count++; p += ph_len; }
    size_t url_len = strlen(url);
    size_t out_len = src_len + count * url_len - count * ph_len;
    char *out = (char *)vbmcp_alloc(out_len + 1);
    if (!out) return NULL;
    char *dst = out;
    const char *s = src;
    while (1) {
        const char *hit = strstr(s, URL_PLACEHOLDER);
        if (!hit) {
            strcpy_s(dst, (size_t)(out + out_len + 1 - dst), s);
            break;
        }
        size_t head = (size_t)(hit - s);
        memcpy(dst, s, head);
        dst += head;
        memcpy(dst, url, url_len);
        dst += url_len;
        s = hit + ph_len;
        if (!*s) { *dst = '\0'; break; }
    }
    return out;
}

/* ============================================================
 * 渲染向导文本（按当前语言选择 steps/config 版本）
 * ============================================================ */

static char *render_guide(int index, const char *url)
{
    if (index < 0 || index >= (int)AI_GUIDES_COUNT) return NULL;
    const ai_guide_t *g = &kAiGuides[index];

    vbmcp_lang_t lang = vbmcp_lang_current();
    const char *steps_raw  = (lang == VBMCP_LANG_ZH) ? g->steps_zh : g->steps_en;
    const char *config_raw = (lang == VBMCP_LANG_ZH) ? g->config_zh : g->config_en;

    /* kAiGuides 的中文段是源码 UTF-8，与 vbmcp_t() 的 GBK 输出必须分别转码后
     * 再拼接，否则 SetDlgItemTextA 显示会乱码（混 GBK + UTF-8 字节序列）。 */
    char *steps_utf8  = replace_url_placeholder(steps_raw, url);
    char *config_utf8 = replace_url_placeholder(config_raw, url);
    char *steps  = vbmcp_str_to_gbk(steps_utf8  ? steps_utf8  : "");
    char *config = vbmcp_str_to_gbk(config_utf8 ? config_utf8 : "");
    if (steps_utf8)  vbmcp_free(steps_utf8);
    if (config_utf8) vbmcp_free(config_utf8);

    strbuf_t b; sb_init(&b);
    /* 客户端名两侧括号用 lang 键（英文 [ ] / 英文 [ ]），避免 UTF-8 字面量
     * 与 GBK 拼接产生乱码。客户端名本身始终是 ASCII 产品名，无需转换。 */
    sb_printf(&b, "%s for %s%s\n",
        vbmcp_t(T_GUIDE_BRACKET_OPEN), g->name, vbmcp_t(T_GUIDE_BRACKET_CLOSE));
    sb_printf(&b, "%s", vbmcp_t(T_GUIDE_PREFACE));
    sb_printf(&b, "\n");
    if (steps) { sb_printf(&b, "%s", steps); vbmcp_free(steps); }
    sb_printf(&b, "\n");
    sb_printf(&b, "%s", vbmcp_t(T_GUIDE_CONFIG_HEADER));
    if (config) { sb_printf(&b, "%s", config); vbmcp_free(config); }
    sb_printf(&b, "\n");
    sb_finalize(&b);
    return b.buf;
}

/* ============================================================
 * BuildUrl：从窗体 bind + port 框构造 http://host:port
 * ============================================================ */

static char *build_url_from_hwnd(HWND hDlg)
{
    char bind[64] = {0};
    char port_txt[32] = {0};
    GetDlgItemTextA(hDlg, IDC_TXT_BIND, bind, sizeof(bind));
    GetDlgItemTextA(hDlg, IDC_EDIT_PORT, port_txt, sizeof(port_txt));

    char *b = bind;
    while (*b == ' ' || *b == '\t') b++;
    if (!*b) _snprintf_s(bind, sizeof(bind), _TRUNCATE, "%s", VBMCP_DEFAULT_BIND);

    long long port = atoi(port_txt);
    if (port <= 0 || port > 65535) port = VBMCP_DEFAULT_PORT;
    char *url = (char *)vbmcp_alloc(128);
    if (!url) return NULL;
    _snprintf_s(url, 128, _TRUNCATE, "http://%s:%lld", bind, port);
    return url;
}

/* ============================================================
 * 渲染向导（控件变化时）
 * ============================================================ */

static void show_guide(HWND hDlg)
{
    int idx = (int)SendDlgItemMessageA(hDlg, IDC_CBO_AI, CB_GETCURSEL, 0, 0);
    if (idx < 0) idx = 0;
    char *url = build_url_from_hwnd(hDlg);
    char *text = render_guide(idx, url ? url : "http://127.0.0.1:3023");
    if (text) {
        /* Win32 EDIT 控件 ES_MULTILINE 显示时需要 \r\n 才换行；
         * 仅 \n 显示为单行（甚至变成软空格），但 SetDlgItemTextA 写入的字节
         * 本身是正确的（"复制出去是正常的"）——所以问题在显示层。
         * 把所有 \n 替换为 \r\n：原地计数再分配。 */
        size_t n_count = 0;
        for (const char *p = text; *p; p++) if (*p == '\n') n_count++;
        if (n_count > 0) {
            size_t in_len = strlen(text);
            char *fixed = (char *)vbmcp_alloc(in_len + n_count + 1);
            if (!fixed) {
                /* OOM：直接显示原文（不替换 \n → \r\n），不 crash */
                SetDlgItemTextA(hDlg, IDC_TXT_GUIDE, text);
            } else {
                char *dst = fixed;
                for (const char *p = text; *p; p++) {
                    if (*p == '\n') {
                        *dst++ = '\r';
                        *dst++ = '\n';
                    } else {
                        *dst++ = *p;
                    }
                }
                *dst = '\0';
                SetDlgItemTextA(hDlg, IDC_TXT_GUIDE, fixed);
                vbmcp_free(fixed);
            }
        } else {
            SetDlgItemTextA(hDlg, IDC_TXT_GUIDE, text);
        }
        vbmcp_free(text);
    }
    if (url) vbmcp_free(url);
}

/* ============================================================
 * 更新服务器状态显示
 * ============================================================ */

static void refresh_status(HWND hDlg)
{
    int running = vbmcp_server_is_running();
    const char *state = running ? vbmcp_t(T_LBL_STATUS_RUNNING)
                                : vbmcp_t(T_LBL_STATUS_STOPPED);
    char url[160];
    _snprintf_s(url, sizeof(url), _TRUNCATE,
        "%s: %s   (http://%s:%d%s)",
        /* 简化标签：用 running/stopped 作为整个 status 标签 */
        state, state,
        (g_bindings && *g_bindings) ? g_bindings : VBMCP_DEFAULT_BIND,
        g_port ? g_port : VBMCP_DEFAULT_PORT,
        running ? "/" : "");
    SetDlgItemTextA(hDlg, IDC_LBL_STATUS, url);
}

/* ============================================================
 * 设置窗体初始化时显示向导 + 设置端口范围
 * ============================================================ */

static void populate_ai_combo(HWND hDlg)
{
    SendDlgItemMessageA(hDlg, IDC_CBO_AI, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < AI_GUIDES_COUNT; i++) {
        SendDlgItemMessageA(hDlg, IDC_CBO_AI, CB_ADDSTRING, 0, (LPARAM)kAiGuides[i].name);
    }
    SendDlgItemMessageA(hDlg, IDC_CBO_AI, CB_SETCURSEL, 0, 0);
}

static void init_dialog(HWND hDlg)
{
    int port = g_port;
    char bind[64];
    vbmcp_config_load(&port, bind, sizeof(bind));

    g_port = port;
    if (g_bindings) vbmcp_free(g_bindings);
    g_bindings = vbmcp_strdup(bind);

    CheckDlgButton(hDlg, IDC_CHK_ENABLE, g_enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextA(hDlg, IDC_TXT_BIND, bind);
    char port_txt[16];
    _snprintf_s(port_txt, sizeof(port_txt), _TRUNCATE, "%d", port);
    SetDlgItemTextA(hDlg, IDC_EDIT_PORT, port_txt);

    SendDlgItemMessageA(hDlg, IDC_SPIN_PORT, UDM_SETRANGE, 0, MAKELPARAM(65535, 1));

    populate_ai_combo(hDlg);

    show_guide(hDlg);
    refresh_status(hDlg);
}

/* ============================================================
 * 剪贴板复制
 * ============================================================ */

static int copy_to_clipboard(HWND hDlg, const char *text)
{
    if (!text) return 0;
    size_t len = strlen(text);
    if (len == 0) return 0;
    if (!OpenClipboard(hDlg)) return 0;

    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (!h) { CloseClipboard(); return 0; }
    char *p = (char *)GlobalLock(h);
    if (!p) { GlobalFree(h); CloseClipboard(); return 0; }
    memcpy(p, text, len);
    p[len] = '\0';
    GlobalUnlock(h);
    SetClipboardData(CF_TEXT, h);
    CloseClipboard();
    return 1;
}

/* ============================================================
 * 帮助按钮
 * ============================================================ */

static void show_help(HWND hDlg)
{
    MessageBoxA(hDlg, vbmcp_t(T_HELP_BODY), vbmcp_t(T_HELP_TITLE),
                MB_OK | MB_ICONINFORMATION);
}

/* ============================================================
 * 端口 EditBox 子类：仅数字输入
 * ============================================================ */

static LRESULT CALLBACK port_edit_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC orig = (WNDPROC)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    if (!orig) orig = DefWindowProcA;

    if (msg == WM_CHAR) {
        TCHAR ch = (TCHAR)wp;
        if (ch >= '0' && ch <= '9') goto pass;
        if (ch < 32) goto pass;
        return 0;
    }
pass:
    return CallWindowProcA(orig, hwnd, msg, wp, lp);
}

static void subclass_port_edit(HWND hDlg)
{
    HWND h = GetDlgItem(hDlg, IDC_EDIT_PORT);
    WNDPROC orig = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC,
                                              (LONG_PTR)port_edit_subclass_proc);
    if (orig && orig != port_edit_subclass_proc) {
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)orig);
    }
}

/* ============================================================
 * 主对话框过程
 * 注意：使用 CreateWindowEx（非 DialogBox），WM_CREATE 触发初始化。
 * ============================================================ */

/* 子控件创建（前向声明，定义在文件下方） */
static void create_child_controls(HWND hDlg);

static LRESULT CALLBACK dlg_wnd_proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        /* 创建一次字体/背景刷；用 +1 偏移保留 Color 值（见 petzold） */
        if (!g_hBgBrush) g_hBgBrush = GetSysColorBrush(COLOR_BTNFACE);
        if (!g_hUiFont) {
            /* nHeight 为负 → 字符高度（像素），绝对值越大字越大 */
            g_hUiFont = CreateFontA(
                -14, 0, 0, 0,
                FW_NORMAL,            /* 不要粗体 */
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                FF_SWISS | DEFAULT_PITCH,
                "Segoe UI");           /* Win Vista+ 默认 UI 字体 */
        }

        create_child_controls(hDlg);
        init_dialog(hDlg);
        subclass_port_edit(hDlg);
        return 0;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        WORD code = HIWORD(wp);
        if (id == IDC_BTN_OK) {
            int enable = (IsDlgButtonChecked(hDlg, IDC_CHK_ENABLE) == BST_CHECKED);
            char port_txt[16];
            GetDlgItemTextA(hDlg, IDC_EDIT_PORT, port_txt, sizeof(port_txt));
            int port = atoi(port_txt);
            if (port <= 0 || port > 65535) port = VBMCP_DEFAULT_PORT;

            g_enabled = enable;
            g_port = port;
            if (g_bindings) vbmcp_free(g_bindings);
            char bind[64];
            GetDlgItemTextA(hDlg, IDC_TXT_BIND, bind, sizeof(bind));
            char *b = bind;
            while (*b == ' ' || *b == '\t') b++;
            if (!*b) strcpy_s(bind, sizeof(bind), VBMCP_DEFAULT_BIND);
            g_bindings = vbmcp_strdup(bind);
            vbmcp_config_save(g_port, g_bindings);

            int ok = vbmcp_server_apply(g_enabled, g_port);
            if (!ok) {
                char msg[320];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                            vbmcp_t(T_ERR_PORT_IN_USE), g_port);
                MessageBoxA(hDlg, msg, vbmcp_t(T_PLUGIN_NAME), MB_OK | MB_ICONWARNING);
                refresh_status(hDlg);
                return 0;
            }
            vbmcp_log(vbmcp_t(T_LOG_SETTINGS_OK), g_enabled, g_port, g_bindings);
            DestroyWindow(hDlg);
            g_options_hwnd = NULL;
            return 0;
        }
        if (id == IDC_BTN_CANCEL) {
            DestroyWindow(hDlg);
            g_options_hwnd = NULL;
            return 0;
        }
        if (id == IDC_BTN_RESTORE) {
            CheckDlgButton(hDlg, IDC_CHK_ENABLE, BST_UNCHECKED);
            char port_txt[16];
            _snprintf_s(port_txt, sizeof(port_txt), _TRUNCATE, "%d", VBMCP_DEFAULT_PORT);
            SetDlgItemTextA(hDlg, IDC_EDIT_PORT, port_txt);
            SetDlgItemTextA(hDlg, IDC_TXT_BIND, VBMCP_DEFAULT_BIND);
            g_enabled = 0;
            g_port = VBMCP_DEFAULT_PORT;
            vbmcp_server_stop();
            vbmcp_config_save(g_port, VBMCP_DEFAULT_BIND);
            vbmcp_log("%s", vbmcp_t(T_LOG_RESTORED));
            refresh_status(hDlg);
            show_guide(hDlg);
            return 0;
        }
        if (id == IDC_BTN_HELP) {
            show_help(hDlg);
            return 0;
        }
        if (id == IDC_BTN_COPY) {
            int idx = (int)SendDlgItemMessageA(hDlg, IDC_CBO_AI, CB_GETCURSEL, 0, 0);
            if (idx < 0 || idx >= (int)AI_GUIDES_COUNT) return 0;
            char *url = build_url_from_hwnd(hDlg);
            vbmcp_lang_t lang = vbmcp_lang_current();
            const char *raw = (lang == VBMCP_LANG_ZH)
                              ? kAiGuides[idx].config_zh
                              : kAiGuides[idx].config_en;
            char *json = replace_url_placeholder(raw, url ? url : "http://127.0.0.1:3023");
            if (json) {
                if (copy_to_clipboard(hDlg, json)) {
                    SetDlgItemTextA(hDlg, IDC_BTN_COPY, vbmcp_t(T_BTN_COPIED));
                    SetTimer(hDlg, 0xC07, 1500, NULL);
                } else {
                    MessageBoxA(hDlg, vbmcp_t(T_ERR_CLIPBOARD),
                                vbmcp_t(T_PLUGIN_NAME), MB_OK | MB_ICONWARNING);
                }
                vbmcp_free(json);
            }
            if (url) vbmcp_free(url);
            return 0;
        }
        if (id == IDC_CBO_AI) {
            if (code == CBN_SELCHANGE) show_guide(hDlg);
            return 0;
        }
        if (id == IDC_EDIT_PORT) {
            if (code == EN_CHANGE) show_guide(hDlg);
            return 0;
        }
        if (id == IDC_CHK_ENABLE) {
            return 0;
        }
        return 0;
    }

    case WM_TIMER: {
        if (wp == 0xC07) {
            KillTimer(hDlg, 0xC07);
            SetDlgItemTextA(hDlg, IDC_BTN_COPY, vbmcp_t(T_BTN_COPY));
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hDlg);
        g_options_hwnd = NULL;
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        /* 让 STATIC / EDIT / 复选框背景与窗体一致（COLOR_BTNFACE）；
         * 文本颜色用系统对话框文本色，避免过深或过浅。 */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)g_hBgBrush;
    }

    case WM_DESTROY:
        g_options_hwnd = NULL;
        /* 注意：g_hUiFont / g_hBgBrush 是进程级共享，多次打开只创建一次。
         * 真正需要清理的是 DllMain 的 DLL_PROCESS_DETACH（这里略，OS 回收）。
         * 若担心泄漏，下次 vbmcp_options_dialog 入口处引用前先判定 not-NULL 即可。 */
        return 0;
    }
    return DefWindowProcA(hDlg, msg, wp, lp);
}

/* ============================================================
 * 注册窗口类（首次）
 * ============================================================ */

static const char *kOptClassName = "VBMCP_OPTIONS_V1";

/* 控件创建 */
static void create_child_controls(HWND hDlg)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(hDlg, GWLP_HINSTANCE);
    /* chkEnable */
    CreateWindowExA(0, "BUTTON", vbmcp_t(T_CHK_ENABLE),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        18, 18, 196, 26, hDlg, (HMENU)IDC_CHK_ENABLE, hInst, NULL);

    /* lblBindAddress */
    CreateWindowExA(0, "STATIC", vbmcp_t(T_LBL_BIND),
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
        18, 56, 110, 24, hDlg, (HMENU)-1, hInst, NULL);
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
        140, 53, 200, 27, hDlg, (HMENU)IDC_TXT_BIND, hInst, NULL);

    /* lblPort */
    CreateWindowExA(0, "STATIC", vbmcp_t(T_LBL_PORT),
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
        18, 96, 110, 24, hDlg, (HMENU)-1, hInst, NULL);
    HWND hPort = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
        140, 93, 140, 27, hDlg, (HMENU)IDC_EDIT_PORT, hInst, NULL);
    CreateWindowExA(0, "msctls_updown32", "",
        WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_HOTTRACK,
        270, 93, 22, 27, hDlg, (HMENU)IDC_SPIN_PORT, hInst, NULL);
    SendDlgItemMessageA(hDlg, IDC_SPIN_PORT, UDM_SETBUDDY, (WPARAM)hPort, 0);

    /* lblStatus */
    CreateWindowExA(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        18, 140, 380, 24, hDlg, (HMENU)IDC_LBL_STATUS, hInst, NULL);

    /* lblAiTool + cboAiTool + btnCopyConfig */
    CreateWindowExA(0, "STATIC", vbmcp_t(T_LBL_AI),
        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE,
        18, 176, 110, 24, hDlg, (HMENU)-1, hInst, NULL);
    CreateWindowExA(0, "COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        140, 173, 150, 200, hDlg, (HMENU)IDC_CBO_AI, hInst, NULL);
    CreateWindowExA(0, "BUTTON", vbmcp_t(T_BTN_COPY),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        300, 172, 96, 28, hDlg, (HMENU)IDC_BTN_COPY, hInst, NULL);

    /* txtGuide */
    CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
        WS_VSCROLL | WS_HSCROLL | ES_READONLY,
        18, 214, 378, 210, hDlg, (HMENU)IDC_TXT_GUIDE, hInst, NULL);

    /* 底部 4 个按钮 */
    CreateWindowExA(0, "BUTTON", vbmcp_t(T_BTN_RESTORE),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        18, 440, 120, 32, hDlg, (HMENU)IDC_BTN_RESTORE, hInst, NULL);
    CreateWindowExA(0, "BUTTON", vbmcp_t(T_BTN_HELP),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        150, 440, 80, 32, hDlg, (HMENU)IDC_BTN_HELP, hInst, NULL);
    CreateWindowExA(0, "BUTTON", vbmcp_t(T_BTN_OK),
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
        240, 440, 90, 32, hDlg, (HMENU)IDC_BTN_OK, hInst, NULL);
    CreateWindowExA(0, "BUTTON", vbmcp_t(T_BTN_CANCEL),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        336, 440, 60, 32, hDlg, (HMENU)IDC_BTN_CANCEL, hInst, NULL);

    /* 统一应用自定义字体（Segoe UI 9pt, FW_NORMAL）。
     * 用 GetWindow(GW_CHILD/GW_HWNDNEXT) 遍历全部顶级子控件。
     * ComboBox 自身要发 WM_SETFONT：会同时把字体传给 ComboLBox（下拉列表项）。
     * 只跳过 ComboLBox（ComboBox 的子窗，由 ComboBox 自己管理字体）。 */
    if (g_hUiFont) {
        HWND child = GetWindow(hDlg, GW_CHILD);
        while (child) {
            char cls[16];
            GetClassNameA(child, cls, sizeof(cls));
            if (_stricmp(cls, "ComboLBox") != 0) {
                SendMessage(child, WM_SETFONT, (WPARAM)g_hUiFont, TRUE);
            }
            child = GetWindow(child, GW_HWNDNEXT);
        }
        /* 主窗体标题栏字体没法改（系统管），但内容区都已是新字体 */
    }
}

static BOOL ensure_class_registered(HINSTANCE hInst)
{
    static BOOL registered = FALSE;
    if (registered) return TRUE;

    INITCOMMONCONTROLSEX iccx = { sizeof(iccx), ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&iccx);

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = dlg_wnd_proc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = kOptClassName;
    if (!RegisterClassExA(&wc)) {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) return FALSE;
    }
    registered = TRUE;
    return TRUE;
}

/* ============================================================
 * 公共入口
 * ============================================================ */

void vbmcp_options_dialog(HWND hParent)
{
    if (g_options_hwnd && IsWindow(g_options_hwnd)) {
        SetForegroundWindow(g_options_hwnd);
        return;
    }

    /* 关键：每次开窗先重读 CustomLanguage，让界面跟用户最近改的语言一致 */
    vbmcp_lang_refresh();

    HINSTANCE hInst = (HINSTANCE)GetModuleHandleA("vbmcp.dll");
    if (!hInst) hInst = GetModuleHandleA(NULL);

    if (!ensure_class_registered(hInst)) {
        MessageBoxA(hParent, vbmcp_t(T_ERR_REG_CLASS),
                    vbmcp_t(T_PLUGIN_NAME), MB_OK);
        return;
    }

    /* 计算窗口位置：放在父窗口（或主显示器）工作区正中。\n     * 多显示器环境下 MonitorFromPoint 比 GetSystemMetrics(SM_CXSCREEN)\n     * 更准，避免窗口落到副屏或跨越两块屏幕。 */
    const int kDlgW = 425, kDlgH = 510;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    HMONITOR hMon = NULL;
    if (hParent && IsWindow(hParent)) {
        hMon = MonitorFromWindow(hParent, MONITOR_DEFAULTTONEAREST);
    } else {
        hMon = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    if (hMon) {
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(hMon, &mi)) {
            int workW = mi.rcWork.right  - mi.rcWork.left;
            int workH = mi.rcWork.bottom - mi.rcWork.top;
            x = mi.rcWork.left + (workW - kDlgW) / 2;
            y = mi.rcWork.top  + (workH - kDlgH) / 2;
        }
    }

    HWND hDlg = CreateWindowExA(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                kOptClassName,
                                vbmcp_t(T_DLG_TITLE),
                                WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                x, y, kDlgW, kDlgH,
                                hParent, NULL, hInst, NULL);
    if (!hDlg) {
        MessageBoxA(hParent, vbmcp_t(T_ERR_CREATE_DLG),
                    vbmcp_t(T_PLUGIN_NAME), MB_OK);
        return;
    }
    g_options_hwnd = hDlg;
    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, FALSE);
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageA(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!IsWindow(hDlg)) break;
    }

    if (hParent && IsWindow(hParent)) {
        EnableWindow(hParent, TRUE);
        SetForegroundWindow(hParent);
    }
}

/* 进程退出时释放字体/背景刷（GDI 对象必须在进程退出前释放） */
void vbmcp_options_dialog_cleanup(void)
{
    if (g_hUiFont) { DeleteObject(g_hUiFont); g_hUiFont = NULL; }
    /* g_hBgBrush 是 GetSysColorBrush 返回的共享刷，不能 DeleteObject */
    g_hBgBrush = NULL;
}
