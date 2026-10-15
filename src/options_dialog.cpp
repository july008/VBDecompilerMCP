/*
 * options_dialog.cpp - 设置窗体（双语 EN / ZH，双属性页）
 *
 * 布局（参考同类 MCP 服务器设置窗体）：
 *   属性页 1「服务设置」
 *     - 分组框「服务」：启用 MCP 服务器 / 绑定 IP 地址 / 端口号 / 服务器状态
 *                       / 权限模式 / 局域网高级选项
 *     - 分组框「权限与安全」：允许 MCP 客户端写入项目数据 + 安全说明 + 生效提示
 *   属性页 2「接入信息」
 *     - 客户端软件下拉（内置 8 个 AI 客户端的接入向导）
 *     - 写入路径（该客户端的 MCP 配置文件位置，只读 + 复制路径）
 *     - 连接地址（本机 MCP 端点，只读 + 复制地址）
 *     - 配置片段（可直接粘贴的 JSON/TOML，只读 + 复制配置）
 *     - 接入说明（多行、可滚动）
 *   底部：恢复默认设置 / 帮助 / 确定 / 取消（两个属性页共用）
 *
 * 实现要点：
 *   - 控件树来自 res/vbmcp.rc：shell 模板 2000 只有 tab + 底部按钮；两个
 *     属性页各自是独立模板（2001 / 2002），因为 rc 的对话框模板是扁平的，
 *     一个模板表达不了"控件属于哪个属性页"。运行期把页窗口作为子窗口创建
 *     并停靠在 tab 显示区，于是资源编辑器里控件也是按页分组的。
 *   - 业务代码统一用 find_ctl(id) 跨页寻址（先查两页，再查 shell），
 *     不需要知道控件挂在哪个窗口上。
 *   - 页窗口用 SetWindowSubclass 把 WM_COMMAND / WM_NOTIFY 转发给对话框过程，
 *     并在子类里就地处理 WM_CTLCOLOR*（说明文字保持透明底）。
 *   - 全部用户可见文本通过 _A()（GBK）/ _()（UTF-8）获取。
 *   - 一次编辑只改控件布局，不触碰权限/传输逻辑（见 mcp_http.cpp / mcp_rpc.cpp）。
 *
 * 可重入性：单实例，重复点击菜单只前置已开窗体。
 */
#include "vbmcp.h"
#include "vbmcp_ids.h"      /* 控件 ID（与 res/vbmcp.rc 共用同一份定义） */
#include "i18n.h"
#include <commctrl.h>    /* SysTabControl32 / TCITEM / ICC_TAB_CLASSES */
#include <stdio.h>
#include <strsafe.h>
#include <shlobj.h>     /* CF_TEXT 剪贴板 */

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

/* 本工程把 NTDDI_VERSION 压得较低，commctrl.h 会隐藏下面两个常量。
 * 它们的取值是稳定的公开 ABI（XP 起就有），此处直接补上，
 * 免得为了两个宏去动全局 _WIN32_WINNT / NTDDI_VERSION。 */
#ifndef WS_EX_CTRLACTIVE
#define WS_EX_CTRLACTIVE 0x00000020L
#endif
#ifndef TCM_GETCLIENTRECT
#define TCM_GETCLIENTRECT (TCM_FIRST + 2)
#endif

/* ============================================================
 * AI 客户端接入向导（双语）
 * name 用产品名（始终英文）。
 * path_xx：该客户端的 MCP 配置文件路径（可含 %VAR%，空=建议界面添加）。
 * config_xx / steps_xx：当前语言对应版本，含 @url@ 占位（填 MCP 端点）。
 * ============================================================ */
typedef struct {
    const char *name;
    const char *path_en;
    const char *path_zh;
    const char *config_en;
    const char *config_zh;
    const char *steps_en;
    const char *steps_zh;
} ai_guide_t;

#define URL_PLACEHOLDER "@url@"

static const ai_guide_t kAiGuides[] = {
    {
        "CodeBuddy",
        "%USERPROFILE%\\.codebuddy\\mcp.json",
        "%USERPROFILE%\\.codebuddy\\mcp.json",
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
        "Merge the snippet above into the config file (create it if missing).\n"
        "If the file already contains other mcpServers entries, do not overwrite them.\n"
        "\n"
        "Note:\n"
        "- \"type\": \"http\" means Streamable HTTP, which is what this server provides.\n"
        "- Older CodeBuddy builds may only recognize \"streamable-http\"; replace \"http\"\n"
        "  with that if your build rejects it.\n"
        "\n"
        "Steps:\n"
        "1. Quit CodeBuddy first (so the file is not overwritten);\n"
        "2. Open mcp.json in a text editor and merge the snippet;\n"
        "3. Save and restart CodeBuddy;\n"
        "4. Confirm \"vbmcp\" shows as connected in CodeBuddy's MCP panel.\n"
        "\n"
        "Docs: https://www.codebuddy.cn -> help center (MCP section)",
        /* steps (ZH) */
        "把上方配置片段合并进配置文件（不存在则新建）。\n"
        "若文件已含其他 mcpServers 条目，请勿覆盖它们。\n"
        "\n"
        "注意：\n"
        "- \"type\": \"http\" 即 Streamable HTTP，本服务器正是该类型。\n"
        "- 较旧版本可能只识别 \"streamable-http\"；如安装版本报错，请改回 streamable-http。\n"
        "\n"
        "操作步骤：\n"
        "1. 先关闭 CodeBuddy（避免配置被覆盖）；\n"
        "2. 用文本编辑器打开 mcp.json，合并上方片段；\n"
        "3. 保存后重启 CodeBuddy；\n"
        "4. 在 CodeBuddy 的 MCP 管理面板中确认 vbmcp 显示已连接。\n"
        "\n"
        "文档：https://www.codebuddy.cn -> 帮助中心（MCP 章节）"
    },
    {
        "VS Code",
        "%APPDATA%\\Code\\User\\mcp.json",
        "%APPDATA%\\Code\\User\\mcp.json",
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
        "The snippet above is the user-level config. For a project-shared setting use\n"
        "<project-root>\\.vscode\\mcp.json instead (same content, escape backslashes as \\\\).\n"
        "\n"
        "Note:\n"
        "- The top-level key in VS Code's mcp.json is \"servers\" (NOT \"mcpServers\");\n"
        "- type \"http\" already means Streamable HTTP, which is what this server provides.\n"
        "\n"
        "Steps:\n"
        "1. Make sure your VS Code supports MCP (Copilot Chat / MCP since 2025);\n"
        "2. Paste the snippet into mcp.json;\n"
        "3. Run \"MCP: Reload Servers\" from the command palette, or reload the window;\n"
        "4. Confirm vbmcp status is connected in the MCP panel.\n"
        "\n"
        "Docs: https://code.visualstudio.com/docs/copilot/chat/mcp-servers",
        "上方片段为用户级配置。若希望随项目共享，请改用\n"
        "<项目根目录>\\.vscode\\mcp.json（内容相同，反斜杠需转义为 \\\\）。\n"
        "\n"
        "注意：\n"
        "- VS Code 的 mcp.json 顶层键是 \"servers\"（不是 \"mcpServers\"）；\n"
        "- type 填 \"http\" 即 Streamable HTTP，本服务器正是该类型。\n"
        "\n"
        "操作步骤：\n"
        "1. 确保当前 VS Code 支持 MCP（2025 版起的 Copilot Chat / MCP 工具支持）；\n"
        "2. 把上方片段粘贴进 mcp.json；\n"
        "3. 命令面板执行 \"MCP: Reload Servers\" 或重启窗口；\n"
        "4. 在 MCP 面板确认 vbmcp 状态为已连接。\n"
        "\n"
        "文档：https://code.visualstudio.com/docs/copilot/chat/mcp-servers"
    },
    {
        "Cursor",
        "%USERPROFILE%\\.cursor\\mcp.json",
        "%USERPROFILE%\\.cursor\\mcp.json",
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
        "Option 2 (file): paste the snippet into mcp.json above; after saving,\n"
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
        "方式二（文件）：把上方片段粘贴进 mcp.json，保存后\n"
        "  在 Cursor 设置的 MCP 列表里手动 Enable 一次。\n"
        "\n"
        "操作步骤：\n"
        "1. 添加后回到 Cursor 对话框，工具列表中应出现 vbmcp；\n"
        "2. 若列表空白，尝试重启 Cursor 或重新加载窗口。\n"
        "\n"
        "文档：https://docs.cursor.com （搜索 \"Model Context Protocol\"）"
    },
    {
        "Trae",
        "%APPDATA%\\Trae\\User\\mcp.json",
        "%APPDATA%\\Trae\\User\\mcp.json",
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
        "Option 2 (file): paste the snippet into the config file above.\n"
        "  Trae ships several product variants (Trae / Trae CN), so the folder name in\n"
        "  %APPDATA% may differ; if the file is not found, use the UI option instead.\n"
        "\n"
        "Steps: After adding, confirm the status is \"connected\", then call the tools in chat.\n"
        "\n"
        "Docs: https://www.trae.ai/docs (search \"MCP\")",
        "方式一（界面添加，推荐）：Trae 设置 -> MCP 服务器 -> 添加服务器\n"
        "  - 传输类型: HTTP(S) (Streamable HTTP)\n"
        "  - 地址: " URL_PLACEHOLDER "\n"
        "方式二（文件）：把上方片段粘贴进上述配置文件。\n"
        "  Trae 有多个版本（Trae / Trae CN），%APPDATA% 下的目录名可能不同；\n"
        "  找不到文件时请改用界面方式添加。\n"
        "\n"
        "操作步骤：添加后确认列表中状态为已连接，然后在对话中调用相关工具。\n"
        "\n"
        "文档：https://www.trae.ai/docs （搜索 \"MCP\"）"
    },
    {
        "WorkBuddy",
        "%USERPROFILE%\\.workbuddy\\mcp.json",
        "%USERPROFILE%\\.workbuddy\\mcp.json",
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
        "If the file already has other mcpServers entries, only merge the snippet.\n"
        "\n"
        "Steps:\n"
        "1. Quit WorkBuddy;\n"
        "2. Merge the snippet into mcp.json and save;\n"
        "3. Restart WorkBuddy; confirm vbmcp is connected in the MCP panel.\n"
        "\n"
        "Note: WorkBuddy's MCP config keys and paths can differ between minor\n"
        "versions. If anything fails, check Settings -> MCP for the current schema.",
        "文件位置：\n"
        "- 用户级：%USERPROFILE%\\.workbuddy\\mcp.json（所有项目复用）\n"
        "- 项目级：<项目目录>\\.workbuddy\\mcp.json（仅当前项目）\n"
        "若文件已含其他 mcpServers 条目，请只合并本片段。\n"
        "\n"
        "操作步骤：\n"
        "1. 关闭 WorkBuddy；\n"
        "2. 打开上述 mcp.json，合并本片段并保存；\n"
        "3. 重启 WorkBuddy，在 MCP 面板中确认已连接。\n"
        "\n"
        "提示：WorkBuddy 不同小版本的 MCP 配置键名/路径可能不同，若配置未生效，\n"
        "请以 设置 -> MCP 中当前显示的字段为准。"
    },
    {
        "Claude Desktop",
        "%APPDATA%\\Claude\\claude_desktop_config.json",
        "%APPDATA%\\Claude\\claude_desktop_config.json",
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
        "You can also open this file via Claude -> Settings -> Developer -> Edit Config.\n"
        "\n"
        "Note: This server is Streamable HTTP, so type \"http\" + url is enough\n"
        "(no command/args required).\n"
        "\n"
        "Steps:\n"
        "1. Edit the config file above and merge the snippet;\n"
        "2. Fully quit and restart Claude Desktop;\n"
        "3. Confirm the tools are available in the chat (or vbmcp shows up under\n"
        "   Settings -> Installed connectors).\n"
        "\n"
        "Docs: https://support.anthropic.com (search \"Claude Desktop MCP\")",
        "也可通过 Claude -> 设置 -> 开发者 -> 编辑配置 打开该文件。\n"
        "\n"
        "注意：本服务器为 Streamable HTTP，type 填 \"http\" 并给 url 即可\n"
        "（无需 command/args）。\n"
        "\n"
        "操作步骤：\n"
        "1. 编辑上述配置文件，合并本片段；\n"
        "2. 完全退出并重启 Claude Desktop；\n"
        "3. 在对话中确认工具可用（或 设置 -> 已安装连接器 里看到 vbmcp）。\n"
        "\n"
        "文档：https://support.anthropic.com （搜索 \"Claude Desktop MCP\"）"
    },
    {
        "Claude Code",
        "%USERPROFILE%\\.claude.json",
        "%USERPROFILE%\\.claude.json",
        /* config (EN) */
        "# write to ~/.claude.json:\n"
        "claude mcp add vbmcp --transport http \"" URL_PLACEHOLDER "\"\n",
        /* config (ZH) */
        "# 推荐用 CLI 添加（写入 ~/.claude.json，自动生效）：\n"
        "claude mcp add vbmcp --transport http \"" URL_PLACEHOLDER "\"\n",
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
        "%USERPROFILE%\\.codex\\config.toml",
        "%USERPROFILE%\\.codex\\config.toml",
        "[mcp_servers.vbmcp]\n"
        "type = \"http\"\n"
        "url = \"" URL_PLACEHOLDER "\"",
        "[mcp_servers.vbmcp]\n"
        "type = \"http\"\n"
        "url = \"" URL_PLACEHOLDER "\"",
        "Option 1 (CLI, recommended):\n"
        "  codex mcp add vbmcp -- " URL_PLACEHOLDER "\n"
        "Option 2 (manual config.toml): append the snippet on the left to the end of\n"
        "  the config file above (or <project-dir>\\.codex\\config.toml).\n"
        "\n"
        "Steps: Save, restart codex; use /mcp in the session to confirm connection status.\n"
        "\n"
        "If `codex mcp add` errors out, run codex mcp add --help to confirm\n"
        "the exact argument order supported by your installed version.\n"
        "\n"
        "Docs: https://github.com/openai/codex (README -> MCP section)",
        "方式一（CLI，推荐）：\n"
        "  codex mcp add vbmcp -- " URL_PLACEHOLDER "\n"
        "方式二（手写 config.toml）：把左侧片段追加到上述配置文件末尾\n"
        "  （或 <项目目录>\\.codex\\config.toml）。\n"
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
 * 全局状态
 *
 *   g_shell   : shell 对话框窗口（模板 2000：tab + 底部按钮）
 *   g_page[2] : 两个属性页窗口（模板 2001 / 2002），g_shell 的子窗口
 * ============================================================ */

/* 单实例：避免重复打开 */
static HWND g_options_hwnd = NULL;
static HWND g_shell        = NULL;
static HWND g_page[2]      = { NULL, NULL };
static HWND g_tab_hwnd     = NULL;
static int  g_cur_tab      = 0;   /* 0=服务设置 1=接入信息 */

#define SERVER_PAGE 0
#define ACCESS_PAGE 1

/* 字体（DPI 感知的 12pt 字体；底色刷另见"统一背景色"小节） */
static HFONT g_hUiFont    = NULL;

/* ============================================================
 * DPI 与布局
 *
 * - 字号 12pt 并随系统 DPI 缩放（S()）；
 * - 控件树来自 res/vbmcp.rc 的 IDD_VBMCP_OPTIONS 模板；
 * - 几何由 layout_dialog() 按当前客户区实时计算，WM_SIZE 重排，
 *   所以窗体可自由缩放（模板已带 WS_THICKFRAME）。
 * ============================================================ */
static const int kFontPt = 12;                        /* 字号（pt） */
/* 默认/最小客户区尺寸，单位是 96dpi 逻辑像素，运行期按 S() 缩放。
 * 注意别把设计尺寸定得太大：宿主若不是 DPI aware，Windows 会把整个窗口按
 * 150%/200% 位图放大，设计尺寸 720x636 在 150% 下就会变成约 1080x954 物理像素，
 * 接近 1080p 屏幕的整高（150% 下用户反馈"窗口显得太大"）。720x560 在 150% 下
 * 约 1080x840，并且两页内容都还有富余。 */
static const int kDefaultClientW = 720;               /* 默认客户区宽（96dpi） */
static const int kDefaultClientH = 560;               /* 默认客户区高 */
static const int kMinClientW     = 600;               /* 最小客户区宽 */
static const int kMinClientH     = 460;               /* 最小客户区高 */
/* 默认尺寸占工作区的最大比例：小屏 / 高缩放下不至于占满屏幕 */
static const double kMaxWorkAreaRatioW = 0.92;
static const double kMaxWorkAreaRatioH = 0.88;

static UINT g_dpi = 96;

static UINT GetDpiForWindowSafe(HWND hwnd)
{
    UINT dpi = 0;
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow pGetDpi = NULL;
    static BOOL resolved = FALSE;
    if (!resolved) {
        resolved = TRUE;
        HMODULE h = GetModuleHandleA("user32.dll");
        if (h) pGetDpi = (PFN_GetDpiForWindow)GetProcAddress(h, "GetDpiForWindow");
    }
    if (pGetDpi && hwnd) dpi = pGetDpi(hwnd);
    if (dpi == 0 && hwnd) {
        /* GetDC 拿到的 DC 必须 ReleaseDC，否则每次 WM_SIZE 漏一个 DC */
        HDC dc = GetDC(hwnd);
        if (dc) {
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(hwnd, dc);
        }
    }
    if (dpi == 0) dpi = 96;
    return dpi;
}

/* 逻辑像素 → 物理像素 */
static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static void create_ui_font(UINT dpi)
{
    if (g_hUiFont) DeleteObject(g_hUiFont);
    g_hUiFont = CreateFontA(
        -MulDiv(kFontPt, (int)dpi, 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FF_SWISS | DEFAULT_PITCH,
        "Microsoft YaHei UI");     /* 中英文都清晰；缺失时系统自动回退 */
}

/* ============================================================
 * 跨页控件寻址与操作
 *
 * 控件分属三个窗口（shell + 两个页窗口），业务代码只认 ID：
 * find_ctl 先查页窗口，再查 shell；查不到返回 NULL，调用方需判空。
 * ============================================================ */
static HWND find_ctl(int id)
{
    for (int i = 0; i < 2; i++) {
        if (g_page[i]) {
            HWND h = GetDlgItem(g_page[i], id);
            if (h) return h;
        }
    }
    if (g_shell) return GetDlgItem(g_shell, id);
    return NULL;
}

static void put(int id, int x, int y, int w, int hgt)
{
    HWND h = find_ctl(id);
    if (h) MoveWindow(h, x, y, w, hgt, TRUE);
}

/* ANSI 窗口统一入口（模板文案与运行期文案都已是 GBK） */
static void set_text(int id, const char *s)
{
    HWND h = find_ctl(id);
    if (h) SetWindowTextA(h, s ? s : "");
}

static void get_item_text(int id, char *buf, int cap)
{
    HWND h = find_ctl(id);
    if (buf && cap > 0) buf[0] = '\0';
    if (h && buf && cap > 0) GetWindowTextA(h, buf, cap);
}

static LRESULT send_ctl(int id, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND h = find_ctl(id);
    return h ? SendMessageA(h, msg, wp, lp) : 0;
}

static void set_check(int id, int on)
{
    HWND h = find_ctl(id);
    if (h) SendMessageA(h, BM_SETCHECK, (WPARAM)(on ? BST_CHECKED : BST_UNCHECKED), 0);
}

static int get_check(int id)
{
    HWND h = find_ctl(id);
    return h ? (int)SendMessageA(h, BM_GETCHECK, 0, 0) : 0;
}

static void enable_ctl(int id, BOOL on)
{
    HWND h = find_ctl(id);
    if (h) EnableWindow(h, on);
}

/* EnumChildWindows 回调：把 UI 字体铺到一整棵子树 */
static BOOL CALLBACK apply_font_cb(HWND h, LPARAM font)
{
    char cls[24];
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, "ComboLBox") != 0) {
        SendMessage(h, WM_SETFONT, (WPARAM)font, TRUE);
    }
    return TRUE;
}

/* 下面这些定义在文件后半部分 */
static void apply_font_to_children(HWND hDlg);
static void size_window_to_client(HWND hDlg, int client_w, int client_h);
static void center_window(HWND hDlg);
static HMONITOR monitor_for_dialog(HWND hDlg);
static void create_page_windows(HWND hDlg);
static void setup_tab_pages(HWND hDlg);
static void apply_captions(HWND hDlg);
static void layout_dialog(HWND hDlg);

/* ============================================================
 * 字符串工具
 * ============================================================ */

/* 把 URL 占位符 @url@ 换成真实地址（new alloc） */
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
    for (;;) {
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

/*
 * 写入 ANSI 窗口（EDIT / STATIC）：
 *   UTF-8 源文本 → GBK（Windows ANSI 代码页）+ 多行 EDIT 需要 \r\n。
 * OOM 时显示原文，不崩。
 */
static void set_text_utf8(HWND hCtl, const char *utf8)
{
    if (!hCtl) return;
    char *gbk = vbmcp_str_to_gbk(utf8 ? utf8 : "");
    const char *src = gbk ? gbk : "";

    size_t n_count = 0;
    for (const char *p = src; *p; p++) if (*p == '\n') n_count++;

    if (n_count == 0) {
        SetWindowTextA(hCtl, src);
    } else {
        size_t in_len = strlen(src);
        char *fixed = (char *)vbmcp_alloc(in_len + n_count + 1);
        if (!fixed) {
            SetWindowTextA(hCtl, src);
        } else {
            char *dst = fixed;
            for (const char *p = src; *p; p++) {
                if (*p == '\n') { *dst++ = '\r'; *dst++ = '\n'; }
                else             { *dst++ = *p; }
            }
            *dst = '\0';
            SetWindowTextA(hCtl, fixed);
            vbmcp_free(fixed);
        }
    }
    if (gbk) vbmcp_free(gbk);
}

/* ============================================================
 * URL 构造
 * ============================================================ */

/* 是否回环地址（安全默认）；空串视为回环 */
static int is_loopback_bind(const char *bind)
{
    if (!bind || !*bind) return 1;
    while (*bind == ' ' || *bind == '\t') bind++;
    return _stricmp(bind, "127.0.0.1") == 0 ||
           _stricmp(bind, "localhost") == 0 ||
           _stricmp(bind, "::1") == 0;
}

/* 基地址：http://host:port（"连接地址" 显示用；bind 在服务设置页、port 在同页） */
static char *build_base_url(void)
{
    char bind[64] = {0};
    char port_txt[32] = {0};
    get_item_text(IDC_TXT_BIND, bind, sizeof(bind));
    get_item_text(IDC_EDIT_PORT, port_txt, sizeof(port_txt));

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

/* MCP 端点：http://host:port/mcp（配置片段 / 接入说明使用） */
static char *build_mcp_url(void)
{
    char *base = build_base_url();
    if (!base) return NULL;
    char *mcp = (char *)vbmcp_alloc(160);
    if (!mcp) { vbmcp_free(base); return NULL; }
    _snprintf_s(mcp, 160, _TRUNCATE, "%s/mcp", base);
    vbmcp_free(base);
    return mcp;
}

/* 展开 %VAR%（写入路径展示用；失败返回副本） */
static char *expand_env_path(const char *src)
{
    if (!src || !*src) return NULL;
    char buf[1024] = {0};
    DWORD n = ExpandEnvironmentStringsA(src, buf, (DWORD)sizeof(buf));
    if (n > 0 && n <= sizeof(buf)) return vbmcp_strdup(buf);
    return vbmcp_strdup(src);
}

/* ============================================================
 * 接入信息页刷新（客户端切换 / 端口变化时）
 * ============================================================ */
static void update_access_page(void)
{
    int idx = (int)send_ctl(IDC_CBO_AI, CB_GETCURSEL, 0, 0);
    if (idx < 0 || idx >= (int)AI_GUIDES_COUNT) idx = 0;
    const ai_guide_t *g = &kAiGuides[idx];
    const int zh = (I18n_GetMode() == I18N_MODE_ZH);

    char *mcp_url = build_mcp_url();
    if (!mcp_url) return;
    const char *url = mcp_url;

    char *base = build_base_url();
    set_text_utf8(find_ctl(IDC_TXT_URL), base ? base : url);
    if (base) vbmcp_free(base);

    const char *path_raw = zh ? g->path_zh : g->path_en;
    if (path_raw && *path_raw) {
        char *expanded = expand_env_path(path_raw);
        set_text_utf8(find_ctl(IDC_TXT_PATH), expanded ? expanded : path_raw);
        if (expanded) vbmcp_free(expanded);
    } else {
        set_text_utf8(find_ctl(IDC_TXT_PATH), _("Add it directly in the client UI (no config file needed)"));
    }

    char *config = replace_url_placeholder(zh ? g->config_zh : g->config_en, url);
    set_text_utf8(find_ctl(IDC_TXT_SNIPPET), config ? config : "");
    if (config) vbmcp_free(config);

    char *steps = replace_url_placeholder(zh ? g->steps_zh : g->steps_en, url);
    set_text_utf8(find_ctl(IDC_TXT_STEPS), steps ? steps : "");
    if (steps) vbmcp_free(steps);

    vbmcp_free(mcp_url);
}

/* ============================================================
 * 非回环监听的高危警告（MCP 安全模型硬约束）
 * ============================================================ */
static int confirm_open_bind_warning(HWND hDlg, const char *bind)
{
    char msg[512];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                _A("You are about to make the MCP server listen on \"%s\", which is NOT the loopback address (127.0.0.1).\n\nThis exposes the server to your LAN / public network. The MCP spec does not recommend running without authentication on a non-local interface, and any MCP client (including AI agents) on the network could read - or, if write access is enabled, modify - your project data.\n\nProceed anyway?"),
                bind);
    const int r = MessageBoxA(hDlg, msg, _A("Security Warning - Non-loopback Listen Address"),
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST);
    return (r == IDYES);
}

/* ============================================================
 * 状态 / 权限模式显示
 * ============================================================ */
/* 「权限模式」标签：按界面勾选预览目标模式（点「确定」后才真正生效）。 */
static void refresh_mode_label(void)
{
    const int allow_write = (get_check(IDC_CHK_ALLOW_WRITE) == BST_CHECKED);
    set_text(IDC_LBL_MODE_VAL, allow_write ? _A("read-write") : _A("read-only"));
}

static void refresh_status(void)
{
    const int running = vbmcp_server_is_running();
    set_text(IDC_LBL_STATUS, running ? _A("running") : _A("stopped"));
    refresh_mode_label();
}

/* ============================================================
 * 属性页切换：显示/隐藏两个页窗口
 * ============================================================ */
static void apply_tab(int tab)
{
    if (tab != ACCESS_PAGE) tab = SERVER_PAGE;
    g_cur_tab = tab;
    if (g_tab_hwnd) SendMessageA(g_tab_hwnd, TCM_SETCURSEL, (WPARAM)tab, 0);

    for (int p = 0; p < 2; p++) {
        if (g_page[p]) ShowWindow(g_page[p], (p == tab) ? SW_SHOW : SW_HIDE);
    }
}

/* ============================================================
 * 初始化
 * ============================================================ */
static void populate_ai_combo(void)
{
    send_ctl(IDC_CBO_AI, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < AI_GUIDES_COUNT; i++) {
        send_ctl(IDC_CBO_AI, CB_ADDSTRING, 0, (LPARAM)kAiGuides[i].name);
    }
    send_ctl(IDC_CBO_AI, CB_SETCURSEL, 0, 0);
}

static void init_dialog(void)
{
    int port = g_port.load();
    int allow_write = VBMCP_DEFAULT_ALLOW_WRITE;
    char bind[64];
    vbmcp_config_load(&port, bind, sizeof(bind), &allow_write);

    g_port.store(port);
    g_allow_write.store(allow_write);
    vbmcp_set_bind_address(bind);

    set_check(IDC_CHK_ENABLE, g_enabled.load());
    set_check(IDC_CHK_ALLOW_WRITE, g_allow_write.load());

    /* 局域网高级选项默认隐藏（未勾选）：绑定框只读 + 禁用，强制回环监听 */
    const int lan_on = !is_loopback_bind(bind);
    set_check(IDC_CHK_LAN_ADVANCED, lan_on);
    set_text(IDC_TXT_BIND, lan_on ? bind : VBMCP_DEFAULT_BIND);
    enable_ctl(IDC_TXT_BIND, lan_on ? TRUE : FALSE);

    /* 端口编辑框是端口值的唯一事实来源；旁边的 up-down（msctls_updown32）
     * 不绑 buddy、不用它自带的数值机制：点击 +/- 由 dlg_proc 的 UDN_DELTAPOS
     * 分支直接改本编辑框（原因见该分支的注释）。 */
    char port_txt[16];
    _snprintf_s(port_txt, sizeof(port_txt), _TRUNCATE, "%d", port);
    set_text(IDC_EDIT_PORT, port_txt);

    populate_ai_combo();

    set_text_utf8(find_ctl(IDC_TXT_SEC_TIP),
        _("Read-only is the default: the server only listens on 127.0.0.1, sends no CORS headers, and vb_write_* tools are hidden from tools/list. Enabling write access lets an AI agent modify the decompiled project (including tools that replace the whole project text) - back up your files first. LAN access exposes the server to the whole network without authentication; only enable it on a network you trust."));
    set_text_utf8(find_ctl(IDC_TXT_SRV_HINT),
        _("Changes to the port / bind address take effect right after clicking OK. Authorization is reset when VB Decompiler restarts, so you will be asked again on the first tool call of the next session."));

    refresh_status();
    update_access_page();
    apply_tab(SERVER_PAGE);
}

/* ============================================================
 * 帮助
 * ============================================================ */

/* msgid 必须是**单个**字符串字面量：早先把版本号用 + 拼接在同一个宏调用里，
 * 提取脚本只能抓到第一段，整段将无法翻译；现在版本号改用 %s 占位符传入。
 * 注意：本注释里刻意不写出带引号的宏调用形式，否则提取脚本会把注释内容
 * 当成词条（历史上就产生过一个纯省略号的假 msgid）。 */
static void show_help(void)
{
    MessageBoxA(g_shell,
                _AF("VB Decompiler MCP Server Plugin v%s\n\nThis plugin runs as an MCP (Model Context Protocol) server (Streamable HTTP),\nallowing AI clients to read/write VB Decompiler-decompiled code and internal\ndata via the JSON-RPC 2.0 interface.\n\nTools available: 28, split into two groups: vb_read_* (always available)\nand vb_write_* (only when \"Allow MCP client to write project data\" is\nenabled). The first tool call of each session asks for your confirmation.\n\nDebug log (when VBMCP_DEBUG_LOG=1):\n  %%TEMP%%\\vbmcp.log\n", VBMCP_VERSION),
                _A("VB Decompiler MCP Server Help"), MB_OK | MB_ICONINFORMATION);
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

static void subclass_port_edit(void)
{
    HWND h = find_ctl(IDC_EDIT_PORT);
    if (!h) return;
    WNDPROC orig = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC,
                                              (LONG_PTR)port_edit_subclass_proc);
    if (orig && orig != port_edit_subclass_proc) {
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)orig);
    }
}

/* ============================================================
 * 统一背景色：直接指定，窗体 / 属性页 / 所有控件都用它
 *
 * 踩坑记录（都是实测）：
 *  1) 硬编码 GetSysColorBrush(COLOR_BTNFACE)：宿主开视觉样式时对话框底色是白，
 *     灰刷上去就出现一块块"带底色的文字"。
 *  2) 向 DefDlgProc(WM_CTLCOLORDLG) 要画刷：主题返回的是 **pattern brush**，
 *     填出来的颜色与窗体实际底色不一致（而且 GetPixel 采样会骗人）。
 *  3) 向父窗口（页）要画刷：两个窗口各自向主题取色，可能不同。
 *
 * 所以：不查主题、不问系统，直接定一个颜色 kUiBgColor，自己建实色刷：
 *   - shell 与两个属性页在 WM_ERASEBKGND 里用它刷满客户区；
 *   - 所有 WM_CTLCOLOR* 返回它（标签用 TRANSPARENT，压根不画自己的底）。
 * 要换配色只改 kUiBgColor 这一处。
 * ============================================================ */
static const int kUiBgColorIndex = COLOR_WINDOW;   /* 系统颜色索引；想换灰改成 COLOR_BTNFACE */
static HBRUSH g_hUiBgBrush = NULL;                /* 自建实色刷（需 DeleteObject） */

/* 注意：COLOR_WINDOW 之类是**系统颜色索引**，不是 RGB 值，
 * 必须经过 GetSysColor() 才是颜色（直接 CreateSolidBrush(COLOR_WINDOW) 会画出近黑色）。 */
static COLORREF ui_bg_color(void)
{
    return GetSysColor(kUiBgColorIndex);
}

static void create_ui_bg_brush(void)
{
    if (g_hUiBgBrush) { DeleteObject(g_hUiBgBrush); g_hUiBgBrush = NULL; }
    g_hUiBgBrush = CreateSolidBrush(ui_bg_color());
}

/* 统一背景色（拿不到就退回同色系统刷） */
static HBRUSH ui_bg_brush(void)
{
    return g_hUiBgBrush ? g_hUiBgBrush : (HBRUSH)GetSysColorBrush(kUiBgColorIndex);
}

/* ============================================================
 * "切到别的程序后，第一次点击按钮没反应"的补偿
 *
 * 现象：设置窗体不是前台时，第一次点击只用来激活窗口，按钮要点第二次才有反应。
 * 原因：那次"激活点击"的 WM_LBUTTONDOWN 被系统吃掉（对话框默认对
 * WM_MOUSEACTIVATE 返回 MA_ACTIVATEANDEAT）。两个常见的"标准修法"对本项目都不成立：
 *   - 在对话框过程里返回 MA_ACTIVATE：dlgmanager 不把 WM_MOUSEACTIVATE 透传给
 *     对话框过程（实测 SendMessage 进去返回 0），第一次点击仍被吃掉；
 *   - 在按钮子类里等"孤立的 WM_LBUTTONUP"：实测冷点击期间按钮只收到
 *     WM_MOUSEMOVE，down/up 一个都没有。
 *
 * 可行且能验证的做法：在**窗口被激活的那一刻**（WM_ACTIVATE + WA_ACTIVE），
 * 若左键仍按着且光标停在窗体内某个控件上，就给那个控件补发一次 BM_CLICK。
 * 真实的那次 mouse-up 不会再产生点击（down 已被吃掉），所以不会重复触发。
 * ============================================================ */
static void replay_click_after_activate(HWND hDlg)
{
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;   /* 不是鼠标激活的 */

    POINT pt;
    GetCursorPos(&pt);
    HWND h = WindowFromPoint(pt);
    if (!h) return;
    if (GetAncestor(h, GA_ROOT) != hDlg) return;   /* 光标不在设置窗体上，不管 */
    if (h == hDlg) return;                         /* 停在空白/分组框上：不补发 */

    PostMessageA(h, BM_CLICK, 0, 0);
}



/* 给所有按钮（含属性页里的）装上"冷点击补偿" */
/* ============================================================
 * 属性页窗口（模板 2001 / 2002）
 *
 * rc 的对话框模板是扁平的：一个模板表达不了"控件属于哪个属性页"，所以
 *
 * rc 的对话框模板是扁平的：一个模板表达不了"控件属于哪个属性页"，所以
 * 每页一个模板，运行期用 CreateDialogParamA 把页建成 shell 的子窗口，
 * 停靠在 tab 显示区。
 *
 * 页的对话框过程做两件事：
 *   - 把 WM_COMMAND / WM_NOTIFY 转发给 shell 对话框过程（控件的父窗口是
 *     页而不是对话框，不转发就收不到点击、下拉与输入变化通知）；
 *   - 就地处理 WM_CTLCOLOR*，说明文字保持透明底、输入框白底。
 * ============================================================ */
static INT_PTR CALLBACK page_dlg_proc(HWND hPage, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        /* 控件由模板创建，这里只统一字体（模板里的 12pt 只是初值） */
        apply_font_to_children(hPage);
        return TRUE;

    case WM_COMMAND:
    case WM_NOTIFY:
        if (g_shell && IsWindow(g_shell)) SendMessageA(g_shell, msg, wp, lp);
        return TRUE;

    case WM_ERASEBKGND: {
        /* 属性页底色由我们自己刷（不依赖主题/对话框类刷） */
        RECT rc;
        GetClientRect(hPage, &rc);
        FillRect((HDC)wp, &rc, ui_bg_brush());
        return 1;
    }

    case WM_CTLCOLORDLG: {
        /* 页也是对话框：同样统一成窗体那把刷，否则子窗口对话框可能与 shell 不同色 */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)ui_bg_brush();
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        /* 标签 / 复选框 / 分组框：与窗体同一把刷（不会出现"带底色的文字块"） */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)ui_bg_brush();

    case WM_CTLCOLOREDIT: {
        /* 输入框（只读字段也是 EDIT）：同一个背景色 + 不透明，避免又出现色差 */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor((HDC)wp, ui_bg_color());
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)ui_bg_brush();
    }
    }
    return FALSE;   /* 对话框过程：返回 FALSE 交给默认对话框过程 */
}

/* 创建两个页窗口（不显示：显示/隐藏由 apply_tab 决定） */
static void create_page_windows(HWND hDlg)
{
    HINSTANCE hInst = vbmcp_self_module();
    if (!hInst) hInst = (HINSTANCE)GetModuleHandleA(NULL);

    const int ids[2] = { IDD_PAGE_SERVER, IDD_PAGE_ACCESS };
    for (int i = 0; i < 2; i++) {
        /* 必须用 CreateDialogParamA：CreateWindowExA("#32770", 模板ID, ...)
         * 在 32 位 USER32 上必定访问冲突（模板查找成功也一样崩），
         * CreateDialogParamA 才是"用模板建子窗口对话框"的官方入口。 */
        HWND page = CreateDialogParamA(hInst, MAKEINTRESOURCEA(ids[i]), hDlg,
                                       page_dlg_proc, (LPARAM)i);
        if (!page) continue;
        g_page[i] = page;
        ShowWindow(page, SW_HIDE);
    }
}

/* ============================================================
 * 对话框过程（窗体本体来自 res/vbmcp.rc 的 IDD_VBMCP_OPTIONS 模板，
 * 两个属性页来自 IDD_PAGE_SERVER / IDD_PAGE_ACCESS 两个页模板）
 *
 * 模板负责"有哪些控件"（样式、Tab 顺序、WS_THICKFRAME 等），本过程负责：
 *   - WM_INITDIALOG：字体(12pt/DPI) → 建页窗口 → 客户区尺寸 → 居中
 *                    → 插属性页标题 → 填双语文案 → 读配置 → 首次排版
 *   - WM_COMMAND / WM_NOTIFY：交互（页窗口的子类会转发到这里）
 *   - WM_SIZE：按新尺寸重排（窗体可缩放）；WM_GETMINMAXINFO：最小尺寸
 * ============================================================ */
static INT_PTR CALLBACK dlg_proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        /* 统一背景色：直接指定一个颜色（见"统一背景色"小节） */
        create_ui_bg_brush();

        /* 资源编辑器保存 .rc 时会丢掉 WS_THICKFRAME（"Resizing" 属性未勾选），
         * 使窗体变成不可拖拽缩放。这里在运行期补上，对编辑器重写免疫。 */
        {
            LONG_PTR style = GetWindowLongPtrA(hDlg, GWL_STYLE);
            if (!(style & WS_THICKFRAME)) {
                SetWindowLongPtrA(hDlg, GWL_STYLE, style | WS_THICKFRAME);
                SetWindowPos(hDlg, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOACTIVATE | SWP_FRAMECHANGED);
            }
        }

        g_options_hwnd = hDlg;
        g_shell        = hDlg;
        g_dpi = GetDpiForWindowSafe(hDlg);
        create_ui_font(g_dpi);
        apply_font_to_children(hDlg);

        /* tab 句柄要先拿到：下面的 WM_SIZE 会走 layout_dialog */
        g_tab_hwnd = GetDlgItem(hDlg, IDC_TAB);

        /* 先建页窗口，后面 WM_SIZE 重排时才有页可摆 */
        create_page_windows(hDlg);

        /* 模板尺寸是 DLU（不精确）：统一改成设计尺寸并居中 */
        size_window_to_client(hDlg, S(kDefaultClientW), S(kDefaultClientH));
        center_window(hDlg);

        setup_tab_pages(hDlg);      /* 插入两个属性页标题（双语文案） */
        apply_captions(hDlg);       /* 控件文案（模板里是英文，仅供查看） */
        init_dialog();              /* 读配置填控件 + 首次排版 */
        subclass_port_edit();
        layout_dialog(hDlg);        /* 兜底排一次：模板 DLU 尺寸必须被覆盖 */

        /* 诊断：确认宿主加载的是哪个 DLL、DPI、以及窗体/标签统一后的底色。
         * 打开日志：set VBMCP_DEBUG_LOG=1（默认 %TEMP%\vbmcp.log） */
        {
            char selfPath[MAX_PATH] = { 0 };
            char hostPath[MAX_PATH] = { 0 };
            HMODULE hSelf = vbmcp_self_module();
            if (hSelf) GetModuleFileNameA(hSelf, selfPath, MAX_PATH);
            GetModuleFileNameA(NULL, hostPath, MAX_PATH);   /* 宿主 exe */

            RECT cr;
            GetClientRect(hDlg, &cr);
            vbmcp_logf(VBMCP_LOG_INFO,
                       "options dialog: dll=%s host=%s dpi=%u client=%dx%d "
                       "uiBg=0x%06X",
                       selfPath, hostPath, (unsigned)g_dpi,
                       (int)(cr.right - cr.left), (int)(cr.bottom - cr.top),
                       (unsigned)(ui_bg_color() & 0xFFFFFF));
        }
        return TRUE;
    }

    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        WORD code = HIWORD(wp);

        if (id == IDC_BTN_OK) {
            const int enable        = (get_check(IDC_CHK_ENABLE) == BST_CHECKED);
            const int allow_write   = (get_check(IDC_CHK_ALLOW_WRITE) == BST_CHECKED);
            const int lan_adv       = (get_check(IDC_CHK_LAN_ADVANCED) == BST_CHECKED);
            char port_txt[16];
            get_item_text(IDC_EDIT_PORT, port_txt, sizeof(port_txt));
            int port = atoi(port_txt);
            if (port <= 0 || port > 65535) port = VBMCP_DEFAULT_PORT;

            char bind[64] = {0};
            if (lan_adv) {
                get_item_text(IDC_TXT_BIND, bind, sizeof(bind));
                char *b = bind;
                while (*b == ' ' || *b == '\t') b++;
                if (!*b) strcpy_s(bind, sizeof(bind), VBMCP_DEFAULT_BIND);
            } else {
                /* 未显式开启局域网高级选项 → 强制回环，绝不监听 0.0.0.0 */
                strcpy_s(bind, sizeof(bind), VBMCP_DEFAULT_BIND);
            }

            /* 安全硬约束：非回环监听必须弹高危警告并二次确认 */
            if (!is_loopback_bind(bind) && !confirm_open_bind_warning(hDlg, bind)) {
                return TRUE;
            }

            const int write_changed = (allow_write != g_allow_write.load());

            g_enabled.store(enable);
            g_port.store(port);
            g_allow_write.store(allow_write);
            vbmcp_set_bind_address(bind);
            vbmcp_config_save(port, bind, allow_write);

            if (!vbmcp_server_apply(enable, port)) {
                MessageBoxA(hDlg,
                            _AF("MCP Server failed to start on port %d: port may be in use. Try another port.",
                                port),
                            _("VB Decompiler MCP Server"), MB_OK | MB_ICONWARNING);
                refresh_status();
                return TRUE;              /* 保持窗体打开，让用户改端口 */
            }
            /* 读写工具集变化 → 广播 tools/list_changed */
            if (write_changed) vbmcp_notify_tools_list_changed();
            vbmcp_log(_F("apply settings ok: enable=%d port=%d bind=%s", enable, port, bind));
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        if (id == IDC_BTN_CANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        /* 键盘 Esc 以 IDCANCEL(id=2) 发 WM_COMMAND；不处理会被下方兜底
         * return TRUE 吞掉，导致 Esc 无法关窗（点标题栏 X 走 WM_CLOSE 正常）。 */
        if (id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        if (id == IDC_BTN_RESTORE) {
            /* 只把界面重置为默认值（含权限模式标签预览），实际生效由
             * 「确定」统一完成 —— 这样点「取消」仍可放弃本次修改，与窗体
             * 其余控件语义一致（历史上这里会立即停服并写注册表）。 */
            set_check(IDC_CHK_ENABLE, 0);
            set_check(IDC_CHK_ALLOW_WRITE, 0);
            set_check(IDC_CHK_LAN_ADVANCED, 0);
            enable_ctl(IDC_TXT_BIND, FALSE);
            char port_txt[16];
            _snprintf_s(port_txt, sizeof(port_txt), _TRUNCATE, "%d", VBMCP_DEFAULT_PORT);
            set_text(IDC_EDIT_PORT, port_txt);
            set_text(IDC_TXT_BIND, VBMCP_DEFAULT_BIND);
            refresh_mode_label();
            update_access_page();
            return TRUE;
        }

        if (id == IDC_BTN_HELP) {
            show_help();
            return TRUE;
        }

        if (id == IDC_CBO_AI) {
            if (code == CBN_SELCHANGE) update_access_page();
            return TRUE;
        }
        if (id == IDC_EDIT_PORT) {
            if (code == EN_CHANGE) update_access_page();
            return TRUE;
        }

        if (id == IDC_CHK_ALLOW_WRITE) {
            /* 「权限模式」显示的是界面勾选对应的目标模式（确定后才生效） */
            refresh_mode_label();
            return TRUE;
        }

        if (id == IDC_CHK_LAN_ADVANCED) {
            const int on = (get_check(IDC_CHK_LAN_ADVANCED) == BST_CHECKED);
            if (on) {
                if (!confirm_open_bind_warning(hDlg, "non-loopback address")) {
                    set_check(IDC_CHK_LAN_ADVANCED, 0);
                    return TRUE;
                }
                enable_ctl(IDC_TXT_BIND, TRUE);
                set_text(IDC_TXT_BIND, "0.0.0.0");
            } else {
                enable_ctl(IDC_TXT_BIND, FALSE);
                set_text(IDC_TXT_BIND, VBMCP_DEFAULT_BIND);
            }
            update_access_page();
            return TRUE;
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        if (lp) {
            LPNMHDR nh = (LPNMHDR)lp;
            if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE) {
                apply_tab((int)SendMessageA(g_tab_hwnd, TCM_GETCURSEL, 0, 0));
                return TRUE;
            }
            /* 端口微调钮：点击 + / - 时把"端口编辑框"的值 ±1 —— 编辑框文本是
             * 端口值的唯一事实来源。不依赖 up-down 自带的 buddy 机制：
             * UDS_SETBUDDYINT 在本环境写回时会把文本写成首字符（实测），
             * 且它的 16 位范围接口装不下 65535。 */
            if (nh->idFrom == IDC_SPIN_PORT && nh->code == UDN_DELTAPOS) {
                LPNMUPDOWN ud = (LPNMUPDOWN)lp;
                char buf[16] = {0};
                get_item_text(IDC_EDIT_PORT, buf, sizeof(buf));
                int v = atoi(buf);
                if (v < 1) v = VBMCP_DEFAULT_PORT;   /* 文本为空/非法：从默认端口起算 */
                /* 符号实测（ui_preview 的合成点击探针）：up 箭头 iDelta = -1、
                 * down 箭头 = +1（位置按"向下增长"约定），故取负后 + 为 +1。 */
                v -= ud->iDelta;
                if (v < 1)     v = 1;
                if (v > 65535) v = 65535;
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", v);
                set_text(IDC_EDIT_PORT, buf);
                update_access_page();                /* 端口变了：刷新接入页地址/片段 */
                return TRUE;                         /* 阻止控件的默认处理 */
            }
        }
        return TRUE;
    }

    case WM_ACTIVATE:
        /* 后台被鼠标点中时，系统把"激活窗口"的那一次点击吃掉了；这里在激活的
         * 那一刻补发一次点击，让"切走后再回来第一次点按钮"就生效。 */
        if (LOWORD(wp) == WA_ACTIVE) replay_click_after_activate(hDlg);
        return TRUE;

    case WM_SIZE:
        layout_dialog(hDlg);
        return TRUE;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        const int frameW = GetSystemMetrics(SM_CXFRAME) * 2;
        const int frameH = GetSystemMetrics(SM_CYFRAME) * 2 + GetSystemMetrics(SM_CYCAPTION);
        mmi->ptMinTrackSize.x = S(kMinClientW) + frameW;
        mmi->ptMinTrackSize.y = S(kMinClientH) + frameH;
        /* 上限＝工作区比例：150%/200% 下别让用户拖出屏幕外 */
        HMONITOR mon = monitor_for_dialog(hDlg);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoA(mon, &mi)) {
            mmi->ptMaxTrackSize.x = (int)((mi.rcWork.right  - mi.rcWork.left) * kMaxWorkAreaRatioW);
            mmi->ptMaxTrackSize.y = (int)((mi.rcWork.bottom - mi.rcWork.top)  * kMaxWorkAreaRatioH);
        }
        return TRUE;
    }

    case WM_TIMER:
        return TRUE;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* 与窗体同一把刷（页内控件由 page_dlg_proc 处理，这里只管 shell 里的按钮） */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)ui_bg_brush();
    }

    case WM_ERASEBKGND: {
        /* 窗体底色由我们自己刷（不依赖主题/对话框类刷） */
        RECT rc;
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wp, &rc, ui_bg_brush());
        return 1;
    }

    case WM_CTLCOLORDLG: {
        /* shell 与两个属性页的底色统一成同一把刷：属性页是子窗口对话框，
         * 主题可能给它和 shell 不同颜色，这里强制一致。 */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)ui_bg_brush();
    }

    case WM_CTLCOLOREDIT: {
        /* 输入框（只读字段也是 EDIT）：同一个背景色 + 不透明 */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor((HDC)wp, ui_bg_color());
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)ui_bg_brush();
    }

    case WM_DESTROY:
        g_options_hwnd = NULL;
        g_shell = NULL;
        g_tab_hwnd = NULL;
        g_page[0] = NULL;
        g_page[1] = NULL;
        return TRUE;
    }
    return FALSE;   /* 对话框过程：未处理的消息返回 FALSE */
}

/* ============================================================
 * 对话框模板初始化助手
 *
 * 注意：控件分属 shell 与两个页窗口三棵子窗口树，所以一律用 find_ctl /
 * EnumChildWindows 这类"跨页"写法，不要直接 GetDlgItem(hDlg, id)。
 * ============================================================ */

/* 把自定义字体应用到所有子控件（EnumChildWindows 会遍历整棵子树，
 * 正好覆盖页窗口里的控件；ComboLBox 由 ComboBox 自己处理） */
static void apply_font_to_children(HWND hDlg)
{
    if (!g_hUiFont || !hDlg) return;
    EnumChildWindows(hDlg, apply_font_cb, (LPARAM)g_hUiFont);
}

/* 取对话框所在显示器：优先父窗口所在屏，否则主屏 */
static HMONITOR monitor_for_dialog(HWND hDlg)
{
    HWND parent = (hDlg && GetParent(hDlg) && IsWindow(GetParent(hDlg))) ? GetParent(hDlg) : NULL;
    if (parent) return MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
    POINT origin{ 0, 0 };
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

/* 把窗口（含边框）调整到指定客户区尺寸 */
static void size_window_to_client(HWND hDlg, int client_w, int client_h)
{
    RECT rc;
    GetWindowRect(hDlg, &rc);
    const int cur_w = rc.right - rc.left;
    const int cur_h = rc.bottom - rc.top;
    if (cur_w <= 0 || cur_h <= 0) return;

    RECT cr;
    GetClientRect(hDlg, &cr);
    const int frame_w = cur_w - (cr.right - cr.left);
    const int frame_h = cur_h - (cr.bottom - cr.top);

    /* 工作区上限：S(设计尺寸) 在 150%/200% 下可能接近整屏，这里按比例压回来。
     * 不管宿主是否 DPI aware 都有效：坐标与 GetMonitorInfo 同一套虚拟化空间。 */
    int want_w = client_w + frame_w;
    int want_h = client_h + frame_h;
    HMONITOR mon = monitor_for_dialog(hDlg);
    if (mon) {
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        if (GetMonitorInfoA(mon, &mi)) {
            const int work_w = mi.rcWork.right  - mi.rcWork.left;
            const int work_h = mi.rcWork.bottom - mi.rcWork.top;
            const int max_w = (int)(work_w * kMaxWorkAreaRatioW);
            const int max_h = (int)(work_h * kMaxWorkAreaRatioH);
            if (want_w > max_w) want_w = max_w;
            if (want_h > max_h) want_h = max_h;
        }
    }
    /* 别被压到比边框还小 */
    if (want_w < frame_w + 80) want_w = frame_w + 80;
    if (want_h < frame_h + 80) want_h = frame_h + 80;

    SetWindowPos(hDlg, NULL, 0, 0, want_w, want_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 居中：优先相对父窗口，其次主显示器工作区 */
static void center_window(HWND hDlg)
{
    HMONITOR mon = monitor_for_dialog(hDlg);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoA(mon, &mi)) return;

    RECT rc;
    GetWindowRect(hDlg, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 3;
    SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 插入两个属性页标题（双语；模板里没有，运行时插） */
static void setup_tab_pages(HWND hDlg)
{
    HWND tab = GetDlgItem(hDlg, IDC_TAB);
    g_tab_hwnd = tab;
    if (!tab) return;

    /* TCM_INSERTITEM 需要 TCITEM 指针；本工程是 ANSI 构建，
     * 这里用与 TCITEMA 布局一致的本地结构，避开 SDK 的 TCHAR 映射差异。 */
    struct tcm_insert_item_a {
        UINT mask; DWORD dwState, dwStateMask;
        LPSTR pszText; int cchTextMax, iImage; LPARAM lParam;
    } tci;

    const char *titles[2] = { _A("Server settings"), _A("Integration info") };
    for (int i = 0; i < 2; i++) {
        ZeroMemory(&tci, sizeof(tci));
        tci.mask = TCIF_TEXT;
        tci.pszText = (LPSTR)titles[i];
        SendMessageA(tab, TCM_INSERTITEM, (WPARAM)i, (LPARAM)&tci);
    }
}

/* 模板里写英文（方便用资源编辑器查看），这里统一覆盖成当前语言。
 * 页内控件走 set_text()（跨页寻址），shell 标题直接设窗口文本。 */
static void apply_captions(HWND hDlg)
{
    SetWindowTextA(hDlg, _A("VB Decompiler MCP Server Settings"));
    set_text(IDC_GRP_SERVICE,      _A("Service"));
    set_text(IDC_GRP_SECURITY,     _A("Permissions and security"));
    set_text(IDC_CHK_ENABLE,       _A("Enable MCP Server"));
    set_text(IDC_CHK_ALLOW_WRITE,  _A("Allow MCP clients to write project data"));
    set_text(IDC_CHK_LAN_ADVANCED, _A("Allow LAN access (advanced)"));
    set_text(IDC_LBL_SRV_STATUS,   _A("Server status:"));
    set_text(IDC_LBL_BIND,         _A("Bind IP address:"));
    set_text(IDC_LBL_PORT,         _A("Port:"));
    set_text(IDC_LBL_MODE,         _A("Permission mode:"));
    set_text(IDC_LBL_AI,           _A("Client app:"));
    set_text(IDC_LBL_PATH,         _A("Config path:"));
    set_text(IDC_LBL_URL,          _A("Connection URL:"));
    set_text(IDC_LBL_SNIPPET,      _A("Config snippet:"));
    set_text(IDC_LBL_STEPS,        _A("How to connect:"));
    set_text(IDC_BTN_RESTORE,      _A("Restore defaults"));
    set_text(IDC_BTN_HELP,         _A("Help"));
    set_text(IDC_BTN_OK,           _A("OK"));
    set_text(IDC_BTN_CANCEL,       _A("Cancel"));
}

/* ============================================================
 * 布局：按当前客户区摆放所有控件（WM_SIZE 时也会调用）
 *
 * 两级布局：
 *   1) shell 客户区：底部按钮栏 + tab 控件；
 *   2) 两个页窗口停靠在 tab 显示区，再各自按页客户区排一遍页内控件。
 * 排版规则：左侧标签列 labelW，输入列起点 colX，右侧状态区靠右。
 * ============================================================ */

/* tab 控件的显示区，换算成 shell 客户区坐标
 *
 * 注意 1：Win32 版 SysTabControl32 并不实现 TCM_GETCLIENTRECT（实测返回 0），
 *         所以这里用固定内缩兜底：顶部让开标签条（S(26)），四周再收 S(6)。
 * 注意 2：TCM_GETCLIENTRECT 与 GetClientRect 给的都是 **tab 客户区** 坐标，
 *         而页窗口要放在 shell 客户区里，必须做两次换算：
 *         tab 客户区 --ClientToScreen--> 屏幕 --ScreenToClient--> shell 客户区。
 *         少一次换算会把页窗口放到屏幕原点附近（实测 -926,-312）。 */
static void tab_display_rect(HWND hShell, HWND hTab, RECT *out)
{
    RECT tc, disp;
    ZeroMemory(out, sizeof(*out));
    GetClientRect(hTab, &tc);

    const BOOL ok = SendMessageA(hTab, TCM_GETCLIENTRECT, 0, (LPARAM)&disp);
    /* 顶部标签条至少有 S(16) 高，否则 TCM_GETCLIENTRECT 的结果不可信，
     * 退回"客户端区去掉上下内缩"的保守算法。 */
    if (!ok || (disp.top - tc.top) < S(16) || (disp.bottom - disp.top) < S(40)) {
        disp.left   = tc.left + S(6);
        disp.top    = tc.top + S(26);
        disp.right  = tc.right - S(6);
        disp.bottom = tc.bottom - S(6);
    }

    POINT tl = { disp.left,   disp.top    };
    POINT br = { disp.right,  disp.bottom };
    ClientToScreen(hTab, &tl);
    ClientToScreen(hTab, &br);
    ScreenToClient(hShell, &tl);
    ScreenToClient(hShell, &br);
    out->left   = tl.x;
    out->top    = tl.y;
    out->right  = br.x;
    out->bottom = br.y;
}

/* 页 1：服务设置（页客户区坐标） */
static void layout_page_server(int W, int H)
{
    const int px = S(14), py = S(12);
    const int pw = W - 2 * px;
    const int bottom = H - py;
    const int rowH = S(26), editH = S(28), labelW = S(150);
    const int colX = px + S(16) + labelW + S(10);
    const int rxLabel = px + pw - S(288);

    const int grp1H = S(146);   /* 三行：启用/状态、绑定IP/LAN、端口 */
    put(IDC_GRP_SERVICE, px, py, pw, grp1H);
    put(IDC_CHK_ENABLE, px + S(16), py + S(24), S(210), rowH);

    put(IDC_LBL_SRV_STATUS, rxLabel, py + S(24), S(150), rowH);
    put(IDC_LBL_STATUS, rxLabel + S(156), py + S(24), S(124), rowH);

    put(IDC_LBL_BIND, px + S(16), py + S(64), labelW, rowH);
    put(IDC_TXT_BIND, colX, py + S(62), S(200), editH);
    put(IDC_CHK_LAN_ADVANCED, colX + S(212), py + S(64),
        pw - (colX - px) - S(228), rowH);

    put(IDC_LBL_PORT, px + S(16), py + S(104), labelW, rowH);
    put(IDC_EDIT_PORT, colX, py + S(102), S(110), editH);
    put(IDC_SPIN_PORT, colX + S(112), py + S(102), S(24), editH);

    const int grp2Y = py + grp1H + S(12);
    const int grp2H = bottom - grp2Y;
    put(IDC_GRP_SECURITY, px, grp2Y, pw, grp2H);

    /* 「权限模式」放在**权限与安全**分组里，与「允许 MCP 客户端写入项目数据」
     * 同一行（它描述的正是这个勾选框的当前效果，语义上也应放在一起）；
     * 勾选框宽度收窄，给右侧的「权限模式」腾出位置。 */
    const int row1Y = grp2Y + S(24);
    put(IDC_CHK_ALLOW_WRITE, px + S(16), row1Y, rxLabel - px - S(28), rowH);
    put(IDC_LBL_MODE, rxLabel, row1Y, S(150), rowH);
    put(IDC_LBL_MODE_VAL, rxLabel + S(156), row1Y, S(124), rowH);

    const int noteTop = grp2Y + S(58);
    const int noteH = S(112);
    const int gapH = S(14);
    const int note2H = S(76);
    put(IDC_TXT_SEC_TIP, px + S(18), noteTop, pw - S(36), noteH);
    put(IDC_TXT_SRV_HINT, px + S(18), noteTop + noteH + gapH,
        pw - S(36), note2H);
}

/* 页 2：接入信息（页客户区坐标） */
static void layout_page_access(int W, int H)
{
    const int px = S(14), py = S(12);
    const int pw = W - 2 * px;
    const int bottom = H - py;
    const int rowH = S(26), editH = S(28), labelW = S(150);
    const int colX = px + S(16) + labelW + S(10);
    /* 没有复制按钮了：输入框铺到右边；下拉框与"绑定 IP 地址"输入框同宽（S(200)） */
    const int fieldW = pw - (colX - px) - S(8);
    const int comboW = S(200);

    /* 行距压到 36（原来 40）：默认客户区高度在 150% 下要更紧凑，
     * 省下的高度还给下面的"接入说明"，否则说明文字可见行数会变少。 */
    put(IDC_LBL_AI, px + S(16), py + S(22), labelW, rowH);
    put(IDC_CBO_AI, colX, py + S(20), comboW, S(240));

    put(IDC_LBL_PATH, px + S(16), py + S(58), labelW, rowH);
    put(IDC_TXT_PATH, colX, py + S(56), fieldW, editH);

    put(IDC_LBL_URL, px + S(16), py + S(94), labelW, rowH);
    put(IDC_TXT_URL, colX, py + S(92), fieldW, editH);

    put(IDC_LBL_SNIPPET, px + S(16), py + S(130), labelW, rowH);
    put(IDC_TXT_SNIPPET, colX, py + S(128), fieldW, S(140));

    const int stepLblY = py + S(280);
    put(IDC_LBL_STEPS, px + S(16), stepLblY, labelW, rowH);
    put(IDC_TXT_STEPS, px + S(16), stepLblY + rowH + S(6),
        pw - S(32), bottom - (stepLblY + rowH + S(6)) - S(10));
}

static void layout_dialog(HWND hDlg)
{
    HWND tab = find_ctl(IDC_TAB);
    if (!hDlg || !tab) return;

    /* 跨屏导致 DPI 变化时重建字体 */
    const UINT dpi = GetDpiForWindowSafe(hDlg);
    if (dpi != g_dpi) {
        g_dpi = dpi;
        create_ui_font(dpi);
        apply_font_to_children(hDlg);
        for (int i = 0; i < 2; i++) {
            if (g_page[i]) apply_font_to_children(g_page[i]);
        }
    }

    RECT rc;
    GetClientRect(hDlg, &rc);
    const int W = rc.right - rc.left;
    const int H = rc.bottom - rc.top;

    const int m    = S(12);
    const int btnH = S(30);

    /* ---- 底部按钮栏（shell） ---- */
    const int btnY = H - m - btnH;
    put(IDC_BTN_RESTORE, m, btnY, S(140), btnH);
    put(IDC_BTN_HELP, m + S(150), btnY, S(100), btnH);
    put(IDC_BTN_CANCEL, W - m - S(100), btnY, S(100), btnH);
    put(IDC_BTN_OK, W - m - S(210), btnY, S(100), btnH);

    /* ---- tab（shell） ---- */
    const int tabY = m;
    const int tabH = btnY - m - tabY;
    put(IDC_TAB, m, tabY, W - 2 * m, tabH);

    /* ---- 两个页窗口停靠在 tab 显示区，再排页内控件 ---- */
    RECT host;
    tab_display_rect(hDlg, tab, &host);
    const int hostW = host.right - host.left;
    const int hostH = host.bottom - host.top;

    for (int i = 0; i < 2; i++) {
        if (!g_page[i]) continue;
        MoveWindow(g_page[i], host.left, host.top, hostW, hostH, TRUE);
        RECT pr;
        GetClientRect(g_page[i], &pr);
        const int pw = pr.right - pr.left;
        const int ph = pr.bottom - pr.top;
        if (i == SERVER_PAGE) layout_page_server(pw, ph);
        else                  layout_page_access(pw, ph);
    }
}

/* ============================================================
 * 公共入口：用对话框模板创建模态设置窗体
 *
 * 注意 hInstance 必须是**本 DLL**：插件由 VB Decompiler 加载，
 * 宿主 hInstance 不是我们，DialogBoxParam 会去错模块找模板。
 * ============================================================ */
void vbmcp_options_dialog(HWND hParent)
{
    if (g_options_hwnd && IsWindow(g_options_hwnd)) {
        SetForegroundWindow(g_options_hwnd);
        return;
    }

    /* 每次开窗先重读语言，让界面跟用户最近改的语言一致 */
    I18n_Refresh();

    HINSTANCE hInst = vbmcp_self_module();
    if (!hInst) hInst = (HINSTANCE)GetModuleHandleA(NULL);

    /* DialogBoxParam 自带模态循环：禁用父窗、IsDialogMessage、
     * 结束自动回到这里 —— 不再需要手写消息循环。 */
    DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_VBMCP_OPTIONS), hParent, dlg_proc, 0);

    g_options_hwnd = NULL;
    g_shell = NULL;
    g_page[0] = NULL;
    g_page[1] = NULL;
    g_tab_hwnd = NULL;
}

/* 进程退出时释放字体/背景刷（GDI 对象必须在进程退出前释放） */
void vbmcp_options_dialog_cleanup(void)
{
    if (g_hUiFont) { DeleteObject(g_hUiFont); g_hUiFont = NULL; }
    /* g_hUiBgBrush 是自建的实色刷，必须删（系统刷不能删） */
    if (g_hUiBgBrush) { DeleteObject(g_hUiBgBrush); g_hUiBgBrush = NULL; }
}
