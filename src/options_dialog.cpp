/*
 * options_dialog.cpp - 设置窗体（双语 EN / ZH，双属性页）
 * options_dialog.cpp - Settings dialog (bilingual EN / ZH, two property pages)
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
 * Layout (modeled after similar MCP server settings dialogs):
 *   page 1 "Server settings": group "Service" (enable / bind IP / port / status /
 *     permission mode / LAN advanced) and group "Permissions and security"
 *     (allow writes + security note + apply hint).
 *   page 2 "Integration info": client preset combo (setup guides for 8 AI
 *     clients), config path, connection URL, config snippet and multi-line
 *     instructions - each read-only.
 *   Bottom: restore defaults / help / OK / cancel (shared by both pages).
 *
 * 实现要点：
 *   - 控件树来自 res/vbdmcp.rc：shell 模板 2000 只有 tab + 底部按钮；两个
 *     属性页各自是独立模板（2001 / 2002），因为 rc 的对话框模板是扁平的，
 *     一个模板表达不了"控件属于哪个属性页"。运行期把页窗口作为子窗口创建
 *     并停靠在 tab 显示区，于是资源编辑器里控件也是按页分组的。
 *   - 业务代码统一用 find_ctl(id) 跨页寻址（先查两页，再查 shell），
 *     不需要知道控件挂在哪个窗口上。
 *   - 页窗口的对话框过程把 WM_COMMAND / WM_NOTIFY 转发给 shell 对话框过程，
 *     并就地处理 WM_CTLCOLOR*（说明文字保持透明底）。
 *   - 全部用户可见文本通过 _W()（UTF-16，本对话框是 Unicode 窗口）/
 *     _()（UTF-8）获取；宿主 ANSI 接口（状态栏等）才用 _A()。
 *   - 本文件的代码只做控件布局与交互，不触碰权限/传输逻辑
 *     （见 mcp_http.cpp / mcp_rpc.cpp）。
 *
 * Implementation notes:
 *   - The control tree comes from res/vbdmcp.rc: shell template 2000 holds only
 *     the tab and the bottom buttons; the two pages are separate templates
 *     (2001 / 2002) because rc dialog templates are flat and cannot express
 *     "which page a control belongs to". At runtime the page windows are created
 *     as child dialogs docked into the tab display area, so the resource editor
 *     still groups controls per page.
 *   - Business code addresses controls with find_ctl(id) across pages (pages
 *     first, then the shell) and never needs to know which window owns them.
 *   - The page dialog proc forwards WM_COMMAND / WM_NOTIFY to the shell dialog
 *     proc and handles WM_CTLCOLOR* locally (note text keeps a transparent
 *     background).
 *   - All user-visible text goes through _W() (UTF-16 - this dialog is a Unicode
 *     window) / _() (UTF-8); only host ANSI surfaces (the status bar, the plugin
 *     name/author fields) use _A().
 *   - This file only lays out controls and handles interaction; it never touches
 *     permission / transport logic (see mcp_http.cpp / mcp_rpc.cpp).
 *
 * 可重入性：单实例，重复点击菜单只前置已开窗体。
 * Reentrancy: single instance - clicking the menu again just brings the open dialog forward.
 */
#include "vbdmcp.h"
#include "vbdmcp_ids.h"      /* 控件 ID（与 res/vbdmcp.rc 共用同一份定义）
                                control IDs (shared with res/vbdmcp.rc) */
#include "i18n.h"
#include "mcp_tools.h"       /* mcp_local_tool_count（帮助文本动态显示工具数）
                                mcp_local_tool_count (help text shows the live tool count) */
#include <commctrl.h>    /* SysTabControl32 / TCITEM / ICC_TAB_CLASSES */
#include <stdio.h>
#include <strsafe.h>
#include <shlobj.h>     /* CF_TEXT 剪贴板 / CF_TEXT clipboard */
#include <string>       /* std::wstring（tab 标题的本地副本，见 setup_tab_pages）
                           std::wstring for local copies of the tab titles */

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

/* 本工程把 NTDDI_VERSION 压得较低，commctrl.h 会隐藏下面两个常量。
 * 它们的取值是稳定的公开 ABI（XP 起就有），此处直接补上，
 * 免得为了两个宏去动全局 _WIN32_WINNT / NTDDI_VERSION。
 *
 * This project pins NTDDI_VERSION low, which makes commctrl.h hide the two
 * constants below. Their values are a stable public ABI (present since XP), so
 * they are defined here instead of raising _WIN32_WINNT / NTDDI_VERSION. */
#ifndef WS_EX_CTRLACTIVE
#define WS_EX_CTRLACTIVE 0x00000020L
#endif
#ifndef TCM_GETCLIENTRECT
#define TCM_GETCLIENTRECT (TCM_FIRST + 2)
#endif

/* ============================================================
 * AI 客户端接入向导（英文原文 + 语言包译文）
 * name 用产品名（始终英文）。
 * path：该客户端的 MCP 配置文件路径（可含 %VAR%，空=建议界面添加；无需翻译）。
 * config / steps：英文原文，含 @url@ 占位（填 MCP 端点）。
 *
 * 本地化方式：guide_text() 先用语言包里的 guide:<client>/config 与
 * guide:<client>/steps 译文（键见 kAiGuideKeys），没有译文才回落这里的英文。
 * zh / zh-TW 与其它语言一样都放在 lang_res/*.json 里 —— 表里不再保留中文副本，
 * 避免两份中文各自漂移。
 *
 * Setup guides for the AI clients (English source + pack translations).
 * name is the product name (always English).
 * path: the client's MCP config file path (may contain %VAR%; empty = add it
 *       through the client UI instead) - never translated.
 * config / steps: English source text with the @url@ placeholder (the MCP endpoint).
 *
 * Localization: guide_text() first looks up guide:<client>/config and
 * guide:<client>/steps in the language pack (keys: kAiGuideKeys) and falls back to
 * the English text here. zh and zh-TW live in lang_res/*.json like every other
 * language - no Chinese copy is kept in this table, so the two cannot drift apart.
 * ============================================================ */
typedef struct {
    const char *name;      /* 产品名（始终英文） */
    const char *path;      /* MCP 配置文件路径（可含 %VAR%；无需翻译） */
    const char *config;    /* 英文原文：配置片段（语言包可覆盖） */
    const char *steps;     /* 英文原文：接入步骤（语言包可覆盖） */
} ai_guide_t;

#define URL_PLACEHOLDER "@url@"

static const ai_guide_t kAiGuides[] = {
    {
        "CodeBuddy",
        "%USERPROFILE%\\.codebuddy\\mcp.json",
        /* config (EN) */
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbdmcp\": {\n"
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
        "4. Confirm \"vbdmcp\" shows as connected in CodeBuddy's MCP panel.\n"
        "\n"
        "Docs: https://www.codebuddy.cn -> help center (MCP section)"
    },
    {
        "VS Code",
        "%APPDATA%\\Code\\User\\mcp.json",
        "{\n"
        "  \"servers\": {\n"
        "    \"vbdmcp\": {\n"
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
        "4. Confirm vbdmcp status is connected in the MCP panel.\n"
        "\n"
        "Docs: https://code.visualstudio.com/docs/copilot/chat/mcp-servers"
    },
    {
        "Cursor",
        "%USERPROFILE%\\.cursor\\mcp.json",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbdmcp\": {\n"
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
        "1. After adding, the tools list in the Cursor chat should include vbdmcp;\n"
        "2. If the list is empty, restart Cursor or reload the window.\n"
        "\n"
        "Docs: https://docs.cursor.com (search \"Model Context Protocol\")"
    },
    {
        "Trae",
        "%APPDATA%\\Trae\\User\\mcp.json",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbdmcp\": {\n"
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
        "Docs: https://www.trae.ai/docs (search \"MCP\")"
    },
    {
        "WorkBuddy",
        "%USERPROFILE%\\.workbuddy\\mcp.json",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbdmcp\": {\n"
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
        "3. Restart WorkBuddy; confirm vbdmcp is connected in the MCP panel.\n"
        "\n"
        "Note: WorkBuddy's MCP config keys and paths can differ between minor\n"
        "versions. If anything fails, check Settings -> MCP for the current schema."
    },
    {
        "Claude Desktop",
        "%APPDATA%\\Claude\\claude_desktop_config.json",
        "{\n"
        "  \"mcpServers\": {\n"
        "    \"vbdmcp\": {\n"
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
        "3. Confirm the tools are available in the chat (or vbdmcp shows up under\n"
        "   Settings -> Installed connectors).\n"
        "\n"
        "Docs: https://support.anthropic.com (search \"Claude Desktop MCP\")"
    },
    {
        "Claude Code",
        "%USERPROFILE%\\.claude.json",
        /* config (EN) */
        "# write to ~/.claude.json:\n"
        "claude mcp add vbdmcp --transport http \"" URL_PLACEHOLDER "\"\n",
        /* steps (EN) */
        "Steps:\n"
        "1. Run one of the commands above in a terminal;\n"
        "2. If `claude mcp add` rejects the syntax, run `claude mcp add --help`\n"
        "   and pick the form that matches your installed version;\n"
        "3. Verify with: claude mcp list;\n"
        "4. Inside a Claude Code session use /mcp to view/manage servers.\n"
        "\n"
        "Docs: https://docs.claude.com/en/docs/claude-code/mcp"
    },
    {
        "OpenAI Codex",
        "%USERPROFILE%\\.codex\\config.toml",
        "[mcp_servers.vbdmcp]\n"
        "type = \"http\"\n"
        "url = \"" URL_PLACEHOLDER "\"",
        "Option 1 (CLI, recommended):\n"
        "  codex mcp add vbdmcp -- " URL_PLACEHOLDER "\n"
        "Option 2 (manual config.toml): append the snippet on the left to the end of\n"
        "  the config file above (or <project-dir>\\.codex\\config.toml).\n"
        "\n"
        "Steps: Save, restart codex; use /mcp in the session to confirm connection status.\n"
        "\n"
        "If `codex mcp add` errors out, run codex mcp add --help to confirm\n"
        "the exact argument order supported by your installed version.\n"
        "\n"
        "Docs: https://github.com/openai/codex (README -> MCP section)"
    }
};

#define AI_GUIDES_COUNT (sizeof(kAiGuides)/sizeof(kAiGuides[0]))

/* 接入向导文案的语言包键：每个客户端两条（config / steps）。
 * 键只是短标识，不是文案；语言包里有译文就用译文，没有就回落到 kAiGuides 里
 * 对应语言的原文（英文/中文）—— 所以缺译文的语言行为与现在完全一致。
 * 用短键而不是把整段英文当 msgid：那些段落多行且含 @url@，当键既难维护又易错。
 * 顺序必须与 kAiGuides 严格一致（下面的 static_assert 会兜住）。
 *
 * Pack keys for the setup guides: two per client (config / steps). A key is an
 * identifier, not text; when the pack has a translation it wins, otherwise the
 * table's own text for that language is used - so a language without translations
 * behaves exactly as before. Short keys are used instead of the multi-line English
 * paragraphs (which contain @url@) because those make fragile keys. The order must
 * match kAiGuides exactly; the static_assert below enforces the count. */
static const char *const kAiGuideKeys[][2] = {
    { N_("guide:codebuddy/config"),     N_("guide:codebuddy/steps")     },
    { N_("guide:vscode/config"),        N_("guide:vscode/steps")        },
    { N_("guide:cursor/config"),        N_("guide:cursor/steps")        },
    { N_("guide:trae/config"),          N_("guide:trae/steps")          },
    { N_("guide:workbuddy/config"),     N_("guide:workbuddy/steps")     },
    { N_("guide:claudedesktop/config"), N_("guide:claudedesktop/steps") },
    { N_("guide:claudecode/config"),    N_("guide:claudecode/steps")    },
    { N_("guide:codex/config"),         N_("guide:codex/steps")         },
};

static_assert(sizeof(kAiGuideKeys) / sizeof(kAiGuideKeys[0]) ==
              sizeof(kAiGuides) / sizeof(kAiGuides[0]),
              "kAiGuideKeys must have one row per kAiGuides entry");

/* ============================================================
 * 全局状态 / Global state
 *
 *   g_shell   : shell 对话框窗口（模板 2000：tab + 底部按钮）
 *   g_page[2] : 两个属性页窗口（模板 2001 / 2002），g_shell 的子窗口
 *
 *   g_shell   : the shell dialog (template 2000: tab + bottom buttons)
 *   g_page[2] : the two page windows (templates 2001 / 2002), children of g_shell
 * ============================================================ */

/* 单实例：避免重复打开 / single instance, never opened twice */
static HWND g_options_hwnd = NULL;
static HWND g_shell        = NULL;
static HWND g_page[2]      = { NULL, NULL };
static HWND g_tab_hwnd     = NULL;
static int  g_cur_tab      = 0;   /* 0=服务设置 1=接入信息 / 0 = server settings, 1 = integration info */

#define SERVER_PAGE 0
#define ACCESS_PAGE 1

/* 字体（DPI 感知的 12pt 字体；底色刷另见"统一背景色"小节）
 * UI font (12pt, DPI aware); the background brush is described in the
 * "unified background color" section below. */
static HFONT g_hUiFont    = NULL;

/* ============================================================
 * DPI 与布局 / DPI and layout
 *
 * - 字号 12pt 并随系统 DPI 缩放（S()）；
 * - 控件树来自 res/vbdmcp.rc 的 IDD_VBDMCP_OPTIONS 模板；
 * - 几何由 layout_dialog() 按当前客户区实时计算，WM_SIZE 重排，
 *   所以窗体可自由缩放（模板已带 WS_THICKFRAME）。
 *
 * - the 12pt font scales with the system DPI through S();
 * - the control tree comes from the IDD_VBDMCP_OPTIONS template in res/vbdmcp.rc;
 * - layout_dialog() computes every geometry from the current client area and
 *   WM_SIZE re-lays it out, so the window resizes freely (the template already
 *   carries WS_THICKFRAME).
 * ============================================================ */
static const int kFontPt = 12;                        /* 字号（pt）/ font size in points */
/* 默认/最小客户区尺寸，单位是 96dpi 逻辑像素，运行期按 S() 缩放。
 * 设计尺寸不能太大：宿主若不是 DPI aware，Windows 会把整个窗口按 150%/200%
 * 位图放大，太大的设计尺寸在 150% 下会接近 1080p 屏幕的整高。
 * 720x560 在 150% 下约 1080x840，两页内容都还有富余。
 *
 * Default / minimum client sizes in 96dpi logical pixels, scaled by S() at
 * runtime. Keep the design size modest: when the host is not DPI aware, Windows
 * bitmap-scales the whole window at 150% / 200%, and an oversized design would
 * nearly fill a 1080p screen at 150%. 720x560 becomes about 1080x840 at 150%,
 * still leaving room for both pages.
 * Defaults / minimums below: client width and height at 96dpi. */
static const int kDefaultClientW = 720;               /* 默认客户区宽（96dpi）/ default client width */
static const int kDefaultClientH = 560;               /* 默认客户区高 / default client height */
static const int kMinClientW     = 600;               /* 最小客户区宽 / minimum client width */
static const int kMinClientH     = 460;               /* 最小客户区高 / minimum client height */
/* 默认尺寸占工作区的最大比例：小屏 / 高缩放下不至于占满屏幕
 * Cap on the default size relative to the work area: never fills the screen on
 * small displays or at high scaling. */
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
        /* GetDC 拿到的 DC 必须 ReleaseDC，否则每次 WM_SIZE 漏一个 DC
         * A DC from GetDC must be released with ReleaseDC, or every WM_SIZE leaks one. */
        HDC dc = GetDC(hwnd);
        if (dc) {
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(hwnd, dc);
        }
    }
    if (dpi == 0) dpi = 96;
    return dpi;
}

/* 逻辑像素 → 物理像素 / logical pixels -> physical pixels */
static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

/* 界面字体族按**当前界面语言**选：CJK 用微软雅黑，其余用 Segoe UI。
 * 没有任何单一字体族覆盖全部 23 种语言：雅黑没有阿拉伯字形（波斯语会掉进
 * 回退字体或方块），Segoe UI 没有 CJK 字形。字体缺失时由系统字体回退兜底。
 *
 * The font family follows the **current UI language**: YaHei for CJK, Segoe UI
 * otherwise. No single family covers all 23 languages - Microsoft YaHei UI has
 * no Arabic glyphs (Persian would land on fallback boxes) and Segoe UI has no
 * CJK glyphs. A missing face is handled by the system font fallback. */
static const wchar_t *ui_font_face(void)
{
    switch (I18n_GetMode()) {
    case I18N_LANG_ZH_SIMPLIFIED:
    case I18N_LANG_ZH_TRADITIONAL:
    case I18N_LANG_JA:
        return L"Microsoft YaHei UI";
    default:
        return L"Segoe UI";
    }
}

static void create_ui_font(UINT dpi)
{
    if (g_hUiFont) DeleteObject(g_hUiFont);
    g_hUiFont = CreateFontW(
        -MulDiv(kFontPt, (int)dpi, 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FF_SWISS | DEFAULT_PITCH,
        ui_font_face());
}

/* ============================================================
 * 跨页控件寻址与操作 / Cross-page control addressing
 *
 * 控件分属三个窗口（shell + 两个页窗口），业务代码只认 ID：
 * find_ctl 先查页窗口，再查 shell；查不到返回 NULL，调用方需判空。
 *
 * Controls live in three windows (shell + two pages) while business code only
 * knows IDs: find_ctl checks the pages first, then the shell, and returns NULL
 * when not found (callers must handle NULL).
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

/* Unicode 窗口统一入口（本对话框由 DialogBoxParamW 创建，控件都是 Unicode）：
 * 传入 **UTF-8**，这里一次性转 UTF-16。ANSI 版本会把波斯语等语言压成 '?'。
 *
 * Single entry for the Unicode windows of this dialog (created by
 * DialogBoxParamW, so every control is Unicode): the input is **UTF-8** and is
 * converted to UTF-16 here. The ANSI variants would collapse Persian & co. to
 * '?'. */
static void set_text(int id, const char *s)
{
    HWND h = find_ctl(id);
    if (h) SetWindowTextW(h, I18n_ToWide(s ? s : ""));
}

/* 读回控件文本并转成 UTF-8（cap 是缓冲区字节数，含结尾 NUL）
 * Reads a control back as UTF-8 (cap is the buffer size in bytes, NUL included). */
static void get_item_text(int id, char *buf, int cap)
{
    HWND h = find_ctl(id);
    if (buf && cap > 0) buf[0] = '\0';
    if (!h || !buf || cap <= 0) return;

    /* 先问长度再分配：EDIT 里的内容可能比栈缓冲长（别静默截断用户输入）
     * Ask for the length first: the EDIT may hold more than a stack buffer
     * would, and silently truncating user input is not acceptable. */
    const int wlen = GetWindowTextLengthW(h);
    if (wlen <= 0) return;
    wchar_t *wbuf = (wchar_t *)vbdmcp_alloc(((size_t)wlen + 1) * sizeof(wchar_t));
    if (!wbuf) return;
    GetWindowTextW(h, wbuf, wlen + 1);

    char *utf8 = I18n_WideToUtf8(wbuf);
    if (utf8) {
        _snprintf_s(buf, (size_t)cap, _TRUNCATE, "%s", utf8);
        vbdmcp_free(utf8);
    }
    vbdmcp_free(wbuf);
}

static LRESULT send_ctl(int id, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND h = find_ctl(id);
    return h ? SendMessageW(h, msg, wp, lp) : 0;
}

static void set_check(int id, int on)
{
    HWND h = find_ctl(id);
    if (h) SendMessageW(h, BM_SETCHECK, (WPARAM)(on ? BST_CHECKED : BST_UNCHECKED), 0);
}

static int get_check(int id)
{
    HWND h = find_ctl(id);
    return h ? (int)SendMessageW(h, BM_GETCHECK, 0, 0) : 0;
}

static void enable_ctl(int id, BOOL on)
{
    HWND h = find_ctl(id);
    if (h) EnableWindow(h, on);
}

/* EnumChildWindows 回调：把 UI 字体铺到一整棵子树
 * EnumChildWindows callback: applies the UI font to a whole subtree. */
static BOOL CALLBACK apply_font_cb(HWND h, LPARAM font)
{
    char cls[24];
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, "ComboLBox") != 0) {
        SendMessage(h, WM_SETFONT, (WPARAM)font, TRUE);
    }
    return TRUE;
}

/* 下面这些定义在文件后半部分 / forward declarations (defined later in this file) */
static void apply_font_to_children(HWND hDlg);
static void size_window_to_client(HWND hDlg, int client_w, int client_h);
static void center_window(HWND hDlg);
static HMONITOR monitor_for_dialog(HWND hDlg);
static void create_page_windows(HWND hDlg);
static void setup_tab_pages(HWND hDlg);
static void apply_captions(HWND hDlg);
static void layout_dialog(HWND hDlg);
static int  measure_text_px(HWND h, const wchar_t *text, int len);
static int  control_text_px(int id);

/* ============================================================
 * 字符串工具 / String helpers
 * ============================================================ */

/* 把 URL 占位符 @url@ 换成真实地址（new alloc）
 * Replaces the @url@ placeholder with the real address (new alloc). */
static char *replace_url_placeholder(const char *src, const char *url)
{
    if (!src) return NULL;
    if (!url) url = "";
    size_t src_len = strlen(src);
    size_t ph_len  = strlen(URL_PLACEHOLDER);
    if (ph_len == 0 || src_len < ph_len) {
        char *r = (char *)vbdmcp_alloc(src_len + 1);
        if (!r) return NULL;
        memcpy(r, src, src_len + 1);
        return r;
    }
    size_t count = 0;
    const char *p = src;
    while ((p = strstr(p, URL_PLACEHOLDER)) != NULL) { count++; p += ph_len; }
    size_t url_len = strlen(url);
    size_t out_len = src_len + count * url_len - count * ph_len;
    char *out = (char *)vbdmcp_alloc(out_len + 1);
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
 * 写入 Unicode 窗口（EDIT / STATIC）：
 *   UTF-8 源文本 → UTF-16 + 多行 EDIT 需要 \r\n。
 * OOM 时显示原文，不崩。
 *
 * Writes to a Unicode window (EDIT / STATIC): UTF-8 source -> UTF-16, and
 * multi-line EDITs need \r\n. On OOM the original text is shown; never crashes.
 */
static void set_text_utf8(HWND hCtl, const char *utf8)
{
    if (!hCtl) return;

    /* 直接转 UTF-16（不再先转 GBK）：本对话框是 Unicode 窗口，
     * 多行 EDIT 需要 \r\n —— 换行规范化改在宽字符上做。
     *
     * Convert straight to UTF-16 (no GBK step): this dialog is a Unicode window.
     * Multi-line EDITs need \r\n, so newline normalization happens in UTF-16. */
    const wchar_t *src = I18n_ToWide(utf8 ? utf8 : "");

    size_t n_count = 0;
    for (const wchar_t *p = src; *p; p++) if (*p == L'\n') n_count++;

    if (n_count == 0) {
        SetWindowTextW(hCtl, src);
        return;
    }

    const size_t in_len = wcslen(src);
    wchar_t *fixed = (wchar_t *)vbdmcp_alloc((in_len + n_count + 1) * sizeof(wchar_t));
    if (!fixed) {
        SetWindowTextW(hCtl, src);          /* 内存不足：原样显示，绝不崩 */
        return;
    }
    wchar_t *dst = fixed;
    for (const wchar_t *p = src; *p; p++) {
        if (*p == L'\n') { *dst++ = L'\r'; *dst++ = L'\n'; }
        else             { *dst++ = *p; }
    }
    *dst = L'\0';
    SetWindowTextW(hCtl, fixed);
    vbdmcp_free(fixed);
}

/* ============================================================
 * URL 构造 / URL builders
 * ============================================================ */

/* 是否回环地址（安全默认）；空串视为回环
 * Whether the address is loopback (the safe default); empty counts as loopback. */
static int is_loopback_bind(const char *bind)
{
    if (!bind || !*bind) return 1;
    while (*bind == ' ' || *bind == '\t') bind++;
    return _stricmp(bind, "127.0.0.1") == 0 ||
           _stricmp(bind, "localhost") == 0 ||
           _stricmp(bind, "::1") == 0;
}

/* 基地址：http://host:port（"连接地址" 显示用；bind 在服务设置页、port 在同页）
 * Base URL http://host:port for the "connection URL" field (bind and port live on
 * the server settings page). */
static char *build_base_url(void)
{
    char bind[64] = {0};
    char port_txt[32] = {0};
    get_item_text(IDC_TXT_BIND, bind, sizeof(bind));
    get_item_text(IDC_EDIT_PORT, port_txt, sizeof(port_txt));

    char *b = bind;
    while (*b == ' ' || *b == '\t') b++;
    if (!*b) _snprintf_s(bind, sizeof(bind), _TRUNCATE, "%s", VBDMCP_DEFAULT_BIND);

    long long port = atoi(port_txt);
    if (port <= 0 || port > 65535) port = VBDMCP_DEFAULT_PORT;
    char *url = (char *)vbdmcp_alloc(128);
    if (!url) return NULL;
    _snprintf_s(url, 128, _TRUNCATE, "http://%s:%lld", bind, port);
    return url;
}

/* MCP 端点：http://host:port/mcp（配置片段 / 接入说明使用）
 * MCP endpoint http://host:port/mcp (used by the config snippet / instructions). */
static char *build_mcp_url(void)
{
    char *base = build_base_url();
    if (!base) return NULL;
    char *mcp = (char *)vbdmcp_alloc(160);
    if (!mcp) { vbdmcp_free(base); return NULL; }
    _snprintf_s(mcp, 160, _TRUNCATE, "%s/mcp", base);
    vbdmcp_free(base);
    return mcp;
}

/* 展开 %VAR%（写入路径展示用；失败返回副本）
 * Expands %VAR% for the config-path display; returns a copy on failure. */
static char *expand_env_path(const char *src)
{
    if (!src || !*src) return NULL;
    char buf[1024] = {0};
    DWORD n = ExpandEnvironmentStringsA(src, buf, (DWORD)sizeof(buf));
    if (n > 0 && n <= sizeof(buf)) return vbdmcp_strdup(buf);
    return vbdmcp_strdup(src);
}

/* ============================================================
 * 接入信息页刷新（客户端切换 / 端口变化时）
 * Refresh of the integration page (client switched / port changed)
 * ============================================================ */
/* 接入向导文案：语言包有译文用译文，否则回落到表内对应语言的原文
 * Guide text: the pack's translation when present, else the table's own text. */
static const char *guide_text(int idx, int is_steps, const char *fallback)
{
    if (idx < 0 || idx >= (int)AI_GUIDES_COUNT) return fallback;
    const char *key = kAiGuideKeys[idx][is_steps ? 1 : 0];
    const char *t = _(key);
    /* 未翻译时 _() 原样返回键本身，据此判断"有没有译文"
     * _() returns the key itself when untranslated, which is how we tell. */
    return (t && *t && strcmp(t, key) != 0) ? t : fallback;
}

static void update_access_page(void)
{
    int idx = (int)send_ctl(IDC_CBO_AI, CB_GETCURSEL, 0, 0);
    if (idx < 0 || idx >= (int)AI_GUIDES_COUNT) idx = 0;
    const ai_guide_t *g = &kAiGuides[idx];
    /* 接入向导文案：表里只剩英文原文（回落用），**所有**语言（含 zh / zh-TW）
     * 的本地化文案都在语言包里，走 guide_text() 覆盖：
     *   - 语言包有 guide:<client>/{config,steps} 译文 → 用译文；
     *   - 没有 → 回落表内英文（见 kAiGuides 上方的说明）。
     * 这样每种语言的文案都只有一个来源（lang_res/*.json），不会出现"表里一份、
     * 包里一份"的漂移。
     *
     * Guide text: the table now holds English only (the fallback). Every language -
     * including zh and zh-TW - gets its localized text from the language pack via
     * guide_text(): a translated guide:<client>/{config,steps} wins, otherwise the
     * English text in the table is used. One source of truth per language, no drift
     * between table and packs. */

    char *mcp_url = build_mcp_url();
    if (!mcp_url) return;
    const char *url = mcp_url;

    char *base = build_base_url();
    set_text_utf8(find_ctl(IDC_TXT_URL), base ? base : url);
    if (base) vbdmcp_free(base);

    const char *path_raw = g->path;   /* 路径含 %VAR%，不需翻译 */
    if (path_raw && *path_raw) {
        char *expanded = expand_env_path(path_raw);
        set_text_utf8(find_ctl(IDC_TXT_PATH), expanded ? expanded : path_raw);
        if (expanded) vbdmcp_free(expanded);
    } else {
        set_text_utf8(find_ctl(IDC_TXT_PATH), _("Add it directly in the client UI (no config file needed)"));
    }

    /* 先取"当前语言的表内原文"，再用语言包译文覆盖（有译文才覆盖）
     * Take the table's own text for this language, then let the pack override it. */
    const char *cfg_txt = guide_text(idx, 0, g->config);
    const char *step_txt = guide_text(idx, 1, g->steps);

    char *config = replace_url_placeholder(cfg_txt, url);
    set_text_utf8(find_ctl(IDC_TXT_SNIPPET), config ? config : "");
    if (config) vbdmcp_free(config);

    char *steps = replace_url_placeholder(step_txt, url);
    set_text_utf8(find_ctl(IDC_TXT_STEPS), steps ? steps : "");
    if (steps) vbdmcp_free(steps);

    vbdmcp_free(mcp_url);
}

/* ============================================================
 * 非回环监听的高危警告（MCP 安全模型硬约束）
 * High-risk warning for non-loopback listening (hard security constraint)
 * ============================================================ */
static int confirm_open_bind_warning(HWND hDlg, const char *bind)
{
    /* 宽字符缓冲：本对话框是 Unicode 窗口，MessageBoxW 只接受 UTF-16。
     * 容量按**字符**计（sizeof/sizeof），告警正文约 380 字符，1024 足够。
     *
     * Wide buffer: this dialog is a Unicode window and MessageBoxW takes UTF-16
     * only. The capacity is in characters (sizeof/sizeof); the warning body is
     * ~380 characters, so 1024 is plenty. */
    wchar_t msg[1024];
    if (bind) {
        _snwprintf_s(msg, sizeof(msg) / sizeof(msg[0]), _TRUNCATE,
                     _W("You are about to make the MCP server listen on \"%s\", which is NOT the loopback address (127.0.0.1).\n\nThis exposes the server to your LAN / public network. The MCP spec does not recommend running without authentication on a non-local interface, and any MCP client (including AI agents) on the network could read - or, if write access is enabled, modify - your project data.\n\nProceed anyway?"),
                     I18n_ToWide(bind));
    } else {
        /* bind == NULL：刚勾选"局域网高级选项"、具体地址还没选 —— 用不含地址的
         * 另一句文案，避免把英文占位词（如 "non-loopback address"）塞进译文里。
         * 用 _snwprintf_s 而不是 wcscpy_s：超长时截断，绝不触发无效参数处理。
         *
         * bind == NULL: the LAN advanced option was just enabled and no address is
         * chosen yet - use a separate message without an address instead of
         * embedding an English placeholder (e.g. "non-loopback address") into a
         * translated sentence. _snwprintf_s is used instead of wcscpy_s so an
         * over-long translation truncates instead of hitting the invalid-parameter
         * handler. */
        _snwprintf_s(msg, sizeof(msg) / sizeof(msg[0]), _TRUNCATE, L"%s",
                     _W("LAN access is about to be enabled: the MCP server will stop listening on the loopback address (127.0.0.1) only and will become reachable from your LAN / public network.\n\nThe MCP spec does not recommend running without authentication on a non-local interface, and any MCP client (including AI agents) on the network could read - or, if write access is enabled, modify - your project data.\n\nProceed anyway?"));
    }
    /* owner 是本对话框（模态链已保证前台），不需要 MB_TOPMOST ——
     * 它与模态 owner 组合反而会导致"置顶但无法正常激活/拖动"（见
     * mcp_rpc.cpp 授权弹窗处的同款说明）。
     *
     * The owner is this dialog (the modal chain already keeps it foreground), so
     * MB_TOPMOST is not needed - combined with a modal owner it causes
     * "on top but not activatable/draggable" (same note at the authorization
     * dialog in mcp_rpc.cpp). */
    const int r = MessageBoxW(hDlg, msg, _W("Security Warning - Non-loopback Listen Address"),
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    return (r == IDYES);
}

/* ============================================================
 * 状态 / 权限模式显示
 * Status / permission-mode display
 * ============================================================ */
/* 「权限模式」标签：按界面勾选预览目标模式（点「确定」后才真正生效）。
 * Permission-mode label: previews the target mode from the checkbox state (it
 * takes effect on OK). */
static void refresh_mode_label(void)
{
    const int allow_write = (get_check(IDC_CHK_ALLOW_WRITE) == BST_CHECKED);
    set_text(IDC_LBL_MODE_VAL, allow_write ? _("read-write") : _("read-only"));
}

static void refresh_status(void)
{
    const int running = vbdmcp_server_is_running();
    set_text(IDC_LBL_STATUS, running ? _("running") : _("stopped"));
    refresh_mode_label();
}

/* ============================================================
 * 属性页切换：显示/隐藏两个页窗口
 * Page switching: shows/hides the two page windows
 * ============================================================ */
static void apply_tab(int tab)
{
    if (tab != ACCESS_PAGE) tab = SERVER_PAGE;
    g_cur_tab = tab;
    if (g_tab_hwnd) SendMessageW(g_tab_hwnd, TCM_SETCURSEL, (WPARAM)tab, 0);

    for (int p = 0; p < 2; p++) {
        if (g_page[p]) ShowWindow(g_page[p], (p == tab) ? SW_SHOW : SW_HIDE);
    }
}

/* ============================================================
 * 初始化 / Initialization
 * ============================================================ */
static void populate_ai_combo(void)
{
    send_ctl(IDC_CBO_AI, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < AI_GUIDES_COUNT; i++) {
        /* Unicode 组合框要 UTF-16 字符串：客户端名是 ASCII，但走 W 消息就必须给
         * 宽串（给窄串会按 UTF-16 解释成乱码）。ComboBox 会立即拷贝内容，
         * 所以轮转池指针在这里传一次即可。
         *
         * A Unicode combo box wants UTF-16: client names are ASCII, but the W
         * message still needs a wide string (a narrow pointer would be read as
         * UTF-16 and come out as garbage). The ComboBox copies the text right
         * away, so passing a rotating-pool pointer here is safe. */
        send_ctl(IDC_CBO_AI, CB_ADDSTRING, 0, (LPARAM)I18n_ToWide(kAiGuides[i].name));
    }
    send_ctl(IDC_CBO_AI, CB_SETCURSEL, 0, 0);
}

static void init_dialog(void)
{
    int port = g_port.load();
    int allow_write = VBDMCP_DEFAULT_ALLOW_WRITE;
    char bind[64];
    vbdmcp_config_load(&port, bind, sizeof(bind), &allow_write);

    g_port.store(port);
    g_allow_write.store(allow_write);
    vbdmcp_set_bind_address(bind);

    set_check(IDC_CHK_ENABLE, g_enabled.load());
    set_check(IDC_CHK_ALLOW_WRITE, g_allow_write.load());

    /* 局域网高级选项默认隐藏（未勾选）：绑定框只读 + 禁用，强制回环监听
     * The LAN advanced option is off by default: the bind box is read-only and
     * disabled, forcing a loopback listen. */
    const int lan_on = !is_loopback_bind(bind);
    set_check(IDC_CHK_LAN_ADVANCED, lan_on);
    set_text(IDC_TXT_BIND, lan_on ? bind : VBDMCP_DEFAULT_BIND);
    enable_ctl(IDC_TXT_BIND, lan_on ? TRUE : FALSE);

    /* 端口编辑框是端口值的唯一事实来源；旁边的 up-down（msctls_updown32）
     * 不绑 buddy、不用它自带的数值机制：点击 +/- 由 dlg_proc 的 UDN_DELTAPOS
     * 分支直接改本编辑框（原因见该分支的注释）。
     *
     * The port edit box is the single source of truth for the port; the adjacent
     * up-down (msctls_updown32) has no buddy and none of its value mechanics:
     * clicks are handled in the UDN_DELTAPOS branch of dlg_proc, which edits this
     * box directly (see that branch). */
    char port_txt[16];
    _snprintf_s(port_txt, sizeof(port_txt), _TRUNCATE, "%d", port);
    set_text(IDC_EDIT_PORT, port_txt);

    populate_ai_combo();

    set_text_utf8(find_ctl(IDC_TXT_SEC_TIP),
        _("Read-only is the default: the server only listens on 127.0.0.1, sends no CORS headers, and vbd_write_* tools are hidden from tools/list. Enabling write access lets an AI agent modify the decompiled project (including tools that replace the whole project text) - back up your files first. LAN access exposes the server to the whole network without authentication; only enable it on a network you trust."));
    set_text_utf8(find_ctl(IDC_TXT_SRV_HINT),
        _("Changes to the port / bind address take effect right after clicking OK. Authorization is reset when VB Decompiler restarts, so you will be asked again on the first tool call of the next session."));

    refresh_status();
    update_access_page();
    apply_tab(SERVER_PAGE);
}

/* ============================================================
 * 帮助 / Help
 * ============================================================ */

/* 约定：msgid 必须是**单个**字符串字面量（用 + 拼接时提取脚本只能抓到第一段，
 * 整段将无法翻译）—— 版本号等动态内容用 %s 占位符传入。
 * 另外不要在注释里写出带引号的宏调用形式，否则提取脚本会把注释内容当成词条。 */
static void show_help(void)
{
    MessageBoxW(g_shell,
                _FW("VB Decompiler MCP Server Plugin v%s\n\nThis plugin runs as an MCP (Model Context Protocol) server (Streamable HTTP),\nallowing AI clients to read/write VB Decompiler-decompiled code and internal\ndata via the JSON-RPC 2.0 interface.\n\nTools available: %d, split into two groups: vbd_read_* (always available)\nand vbd_write_* (only when \"Allow MCP client to write project data\" is\nenabled). The first tool call of each session asks for your confirmation.\n\nDebug log (when VBDMCP_DEBUG_LOG=1):\n  %%TEMP%%\\vbdmcp.log\n", VBDMCP_VERSION, mcp_local_tool_count(1)),
                _W("VB Decompiler MCP Server Help"), MB_OK | MB_ICONINFORMATION);
}

/* ============================================================
 * 端口 EditBox 子类：仅数字输入
 * Port edit box subclass: digits only
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
 * 这样做的原因（均为实测结论）：
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
 *
 * Unified background color: chosen explicitly and used by the shell, the pages
 * and every control.
 *
 * Rationale (all measured):
 *  1) Hard-coding GetSysColorBrush(COLOR_BTNFACE): with visual styles the dialog
 *     background is white, so a gray brush produces patches of "text with a
 *     background".
 *  2) Asking DefDlgProc (WM_CTLCOLORDLG) for a brush: the theme returns a
 *     **pattern brush** whose fill does not match the real background (and
 *     GetPixel sampling lies).
 *  3) Asking the parent (page) window for a brush: each window samples the theme
 *     separately and may differ.
 *
 * Therefore: no theme, no system query - one hard color kUiBgColor and a solid
 * brush built from it:
 *   - the shell and both pages fill their client area with it in WM_ERASEBKGND;
 *   - every WM_CTLCOLOR* returns it (labels use TRANSPARENT and paint no
 *     background of their own).
 * Changing the palette means changing kUiBgColor only.
 * ============================================================ */
static const int kUiBgColorIndex = COLOR_WINDOW;   /* 系统颜色索引；想换灰改成 COLOR_BTNFACE
                                                      system color index; use COLOR_BTNFACE for gray */
static HBRUSH g_hUiBgBrush = NULL;                /* 自建实色刷（需 DeleteObject）/ own solid brush (needs DeleteObject) */

/* 注意：COLOR_WINDOW 之类是**系统颜色索引**，不是 RGB 值，
 * 必须经过 GetSysColor() 才是颜色（直接 CreateSolidBrush(COLOR_WINDOW) 会画出近黑色）。
 *
 * Note: COLOR_WINDOW and friends are **system color indices**, not RGB values;
 * only GetSysColor() yields a color (CreateSolidBrush(COLOR_WINDOW) paints nearly
 * black). */
static COLORREF ui_bg_color(void)
{
    return GetSysColor(kUiBgColorIndex);
}

static void create_ui_bg_brush(void)
{
    if (g_hUiBgBrush) { DeleteObject(g_hUiBgBrush); g_hUiBgBrush = NULL; }
    g_hUiBgBrush = CreateSolidBrush(ui_bg_color());
}

/* 统一背景色（拿不到就退回同色系统刷）
 * Unified background brush (falls back to the same system color brush). */
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
 *
 * Compensating for "the first button click after switching away does nothing".
 *
 * Symptom: when the settings dialog is not foreground, the first click only
 * activates the window and a second click is needed on the button. Cause: that
 * activating WM_LBUTTONDOWN is swallowed by the system (dialogs answer
 * WM_MOUSEACTIVATE with MA_ACTIVATEANDEAT by default). Both common "standard
 * fixes" fail here:
 *   - returning MA_ACTIVATE from the dialog proc: dlgmanager does not forward
 *     WM_MOUSEACTIVATE to it (measured: SendMessage returns 0), so the click is
 *     still eaten;
 *   - waiting for an "orphan WM_LBUTTONUP" in a button subclass: during a cold
 *     click the button only receives WM_MOUSEMOVE, no down/up at all.
 *
 * What works and can be verified: at the moment the window is activated
 * (WM_ACTIVATE + WA_ACTIVE), if the left button is still held and the cursor sits
 * on a control inside the window, post one BM_CLICK to that control. The real
 * mouse-up produces no click (its down was eaten), so nothing fires twice.
 * ============================================================ */
static void replay_click_after_activate(HWND hDlg)
{
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;   /* 不是鼠标激活的 / not activated by the mouse */

    POINT pt;
    GetCursorPos(&pt);
    HWND h = WindowFromPoint(pt);
    if (!h) return;
    if (GetAncestor(h, GA_ROOT) != hDlg) return;   /* 光标不在设置窗体上，不管 / cursor not on the settings dialog: ignore */
    if (h == hDlg) return;                         /* 停在空白/分组框上：不补发 / on empty space or a group box: do not re-post */

    PostMessageA(h, BM_CLICK, 0, 0);
}



/* ============================================================
 * 属性页窗口（模板 2001 / 2002）
 *
 * rc 的对话框模板是扁平的：一个模板表达不了"控件属于哪个属性页"，所以
 * 每页一个模板，运行期用 CreateDialogParamA 把页建成 shell 的子窗口，
 * 停靠在 tab 显示区。
 *
 * 页的对话框过程做两件事：
 *   - 把 WM_COMMAND / WM_NOTIFY 转发给 shell 对话框过程（控件的父窗口是
 *     页而不是对话框，不转发就收不到点击、下拉与输入变化通知）；
 *   - 就地处理 WM_CTLCOLOR*，说明文字保持透明底、输入框白底。
 *
 * Page windows (templates 2001 / 2002).
 *
 * rc dialog templates are flat and cannot express "which page a control belongs
 * to", so each page has its own template and is created at runtime with
 * CreateDialogParamA as a child of the shell, docked into the tab display area.
 *
 * The page dialog proc does two things:
 *   - forwards WM_COMMAND / WM_NOTIFY to the shell dialog proc (the controls'
 *     parent is the page, not the dialog, so without forwarding clicks, combo and
 *     edit notifications would be lost);
 *   - handles WM_CTLCOLOR* locally: note text keeps a transparent background and
 *     edit fields stay white.
 * ============================================================ */
static INT_PTR CALLBACK page_dlg_proc(HWND hPage, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        /* 控件由模板创建，这里只统一字体（模板里的 12pt 只是初值）
         * Controls come from the template; only the font is unified here (the
         * template's 12pt is just a starting value). */
        apply_font_to_children(hPage);
        return TRUE;

    case WM_COMMAND:
    case WM_NOTIFY:
        /* 页 → shell 的同进程转发：用 W 变体与本对话框（Unicode）保持同族。
         * 通知消息不带字符串，但 W/A 混用会让 shell 收到不同族的消息。
         *
         * Page -> shell forwarding inside one process: use the W variant to stay
         * in the same family as this Unicode dialog. The notifications carry no
         * strings, but mixing W/A would hand the shell the other family. */
        if (g_shell && IsWindow(g_shell)) SendMessageW(g_shell, msg, wp, lp);
        return TRUE;

    case WM_ERASEBKGND: {
        /* 属性页底色由我们自己刷（不依赖主题/对话框类刷）
         * The page background is painted by us (no theme / dialog-class brush). */
        RECT rc;
        GetClientRect(hPage, &rc);
        FillRect((HDC)wp, &rc, ui_bg_brush());
        return 1;
    }

    case WM_CTLCOLORDLG: {
        /* 页也是对话框：同样统一成窗体那把刷，否则子窗口对话框可能与 shell 不同色
         * A page is a dialog too: use the same brush, or it may differ from the shell. */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)ui_bg_brush();
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        /* 标签 / 复选框 / 分组框：与窗体同一把刷（不会出现"带底色的文字块"）
         * Labels / checkboxes / group boxes share the window brush (no "text with
         * a background" patches). */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)ui_bg_brush();

    case WM_CTLCOLOREDIT: {
        /* 输入框（只读字段也是 EDIT）：同一个背景色 + 不透明，避免又出现色差
         * Edit fields (read-only ones too): same background, opaque, no color mismatch. */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor((HDC)wp, ui_bg_color());
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)ui_bg_brush();
    }
    }
    return FALSE;   /* 对话框过程：返回 FALSE 交给默认对话框过程
                       dialog proc: FALSE hands the message to the default dialog proc */
}

/* 创建两个页窗口（不显示：显示/隐藏由 apply_tab 决定）
 * Creates the two page windows hidden; apply_tab controls their visibility. */
static void create_page_windows(HWND hDlg)
{
    HINSTANCE hInst = vbdmcp_self_module();
    if (!hInst) hInst = (HINSTANCE)GetModuleHandleA(NULL);

    const int ids[2] = { IDD_PAGE_SERVER, IDD_PAGE_ACCESS };
    for (int i = 0; i < 2; i++) {
        /* 必须用 CreateDialogParamW：CreateWindowEx("#32770", 模板ID, ...)
         * 在 32 位 USER32 上必定访问冲突（模板查找成功也一样崩），
         * CreateDialogParamW 才是"用模板建子窗口对话框"的官方入口。
         * 用 W 版：DLL 里的模板是 Unicode（rc.exe 生成的就是 UTF-16 模板，
         * 见 tools/verify_dialog_templates.ps1），A 版会把模板字符串压成 ANSI。
         *
         * CreateDialogParamW is mandatory: CreateWindowEx("#32770", templateId,
         * ...) always raises an access violation on 32-bit USER32 (even when the
         * template is found); CreateDialogParamW is the official way to create a
         * child dialog from a template. The W variant is used because the
         * templates in the DLL are Unicode (rc.exe emits UTF-16 templates - see
         * tools/verify_dialog_templates.ps1), while the A variant would collapse
         * their strings to ANSI. */
        HWND page = CreateDialogParamW(hInst, MAKEINTRESOURCEW(ids[i]), hDlg,
                                       page_dlg_proc, (LPARAM)i);
        if (!page) continue;
        g_page[i] = page;
        ShowWindow(page, SW_HIDE);
    }
}

/* ============================================================
 * 对话框过程（窗体本体来自 res/vbdmcp.rc 的 IDD_VBDMCP_OPTIONS 模板，
 * 两个属性页来自 IDD_PAGE_SERVER / IDD_PAGE_ACCESS 两个页模板）
 *
 * 模板负责"有哪些控件"（样式、Tab 顺序、WS_THICKFRAME 等），本过程负责：
 *   - WM_INITDIALOG：字体(12pt/DPI) → 建页窗口 → 客户区尺寸 → 居中
 *                    → 插属性页标题 → 填双语文案 → 读配置 → 首次排版
 *   - WM_COMMAND / WM_NOTIFY：交互（页窗口的对话框过程会转发到这里）
 *   - WM_SIZE：按新尺寸重排（窗体可缩放）；WM_GETMINMAXINFO：最小尺寸
 *
 * Dialog procedure (the shell itself comes from the IDD_VBDMCP_OPTIONS template
 * in res/vbdmcp.rc; the pages from IDD_PAGE_SERVER / IDD_PAGE_ACCESS).
 *
 * The templates define which controls exist (styles, tab order, WS_THICKFRAME);
 * this proc handles:
 *   - WM_INITDIALOG: font (12pt/DPI) -> page windows -> client size -> centering
 *     -> tab titles -> bilingual captions -> read config -> first layout;
 *   - WM_COMMAND / WM_NOTIFY: interaction (the page dialog procs forward here);
 *   - WM_SIZE: re-layout for the new size (the window resizes);
 *     WM_GETMINMAXINFO: minimum size.
 * ============================================================ */
static INT_PTR CALLBACK dlg_proc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        /* 统一背景色：直接指定一个颜色（见"统一背景色"小节）
         * Unified background: one explicit color (see the section above). */
        create_ui_bg_brush();

        /* 资源编辑器保存 .rc 时会丢掉 WS_THICKFRAME（"Resizing" 属性未勾选），
         * 使窗体变成不可拖拽缩放。这里在运行期补上，对编辑器重写免疫。
         *
         * Saving the .rc from the resource editor drops WS_THICKFRAME (the
         * "Resizing" property is unchecked), leaving the window non-resizable.
         * Adding it at runtime is immune to editor rewrites. */
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

        /* tab 句柄要先拿到：下面的 WM_SIZE 会走 layout_dialog
         * The tab handle must be fetched first: the WM_SIZE below runs layout_dialog. */
        g_tab_hwnd = GetDlgItem(hDlg, IDC_TAB);

        /* 先建页窗口，后面 WM_SIZE 重排时才有页可摆
         * Create the page windows first so the later WM_SIZE has pages to place. */
        create_page_windows(hDlg);

        /* 模板尺寸是 DLU（不精确）：统一改成设计尺寸并居中
         * Template sizes are DLU (imprecise): force the design size and center. */
        size_window_to_client(hDlg, S(kDefaultClientW), S(kDefaultClientH));
        center_window(hDlg);

        setup_tab_pages(hDlg);      /* 插入两个属性页标题（双语文案）/ insert the two page titles (bilingual) */
        apply_captions(hDlg);       /* 控件文案（模板里是英文，仅供查看）/ control captions (the template text is English only) */
        init_dialog();              /* 读配置填控件 + 首次排版 / fill controls from config + first layout */
        subclass_port_edit();
        layout_dialog(hDlg);        /* 兜底排一次：模板 DLU 尺寸必须被覆盖 / final layout pass overrides the DLU sizes */

        /* 诊断：确认宿主加载的是哪个 DLL、DPI、以及窗体/标签统一后的底色。
         * 打开日志：set VBDMCP_DEBUG_LOG=1（默认 %TEMP%\vbdmcp.log）
         *
         * Diagnostics: which DLL the host loaded, the DPI, and the unified
         * background color. Enable the log with set VBDMCP_DEBUG_LOG=1
         * (%TEMP%\vbdmcp.log by default). */
        {
            char selfPath[MAX_PATH] = { 0 };
            char hostPath[MAX_PATH] = { 0 };
            HMODULE hSelf = vbdmcp_self_module();
            if (hSelf) GetModuleFileNameA(hSelf, selfPath, MAX_PATH);
            GetModuleFileNameA(NULL, hostPath, MAX_PATH);   /* 宿主 exe / host exe */

            RECT cr;
            GetClientRect(hDlg, &cr);
            vbdmcp_logf(VBDMCP_LOG_INFO,
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
            if (port <= 0 || port > 65535) port = VBDMCP_DEFAULT_PORT;

            char bind[64] = {0};
            if (lan_adv) {
                get_item_text(IDC_TXT_BIND, bind, sizeof(bind));
                char *b = bind;
                while (*b == ' ' || *b == '\t') b++;
                if (!*b) strcpy_s(bind, sizeof(bind), VBDMCP_DEFAULT_BIND);
            } else {
                /* 未显式开启局域网高级选项 → 强制回环，绝不监听 0.0.0.0
                 * Without the LAN advanced option, force loopback - never 0.0.0.0. */
                strcpy_s(bind, sizeof(bind), VBDMCP_DEFAULT_BIND);
            }

            /* 安全硬约束：非回环监听必须弹高危警告并二次确认
             * Hard security rule: non-loopback listening requires the high-risk
             * warning and a second confirmation. */
            if (!is_loopback_bind(bind) && !confirm_open_bind_warning(hDlg, bind)) {
                return TRUE;
            }

            const int write_changed = (allow_write != g_allow_write.load());

            g_enabled.store(enable);
            g_port.store(port);
            g_allow_write.store(allow_write);
            vbdmcp_set_bind_address(bind);
            vbdmcp_config_save(port, bind, allow_write);

            if (!vbdmcp_server_apply(enable, port)) {
                MessageBoxW(hDlg,
                            _FW("MCP Server failed to start on port %d: port may be in use. Try another port.",
                                port),
                            _W("VB Decompiler MCP Server"), MB_OK | MB_ICONWARNING);
                refresh_status();
                return TRUE;              /* 保持窗体打开，让用户改端口 / keep the dialog open so the user can change the port */
            }
            /* 读写工具集变化 → 广播 tools/list_changed
             * The read/write tool set changed -> broadcast tools/list_changed */
            if (write_changed) vbdmcp_notify_tools_list_changed();
            vbdmcp_log(_F("apply settings ok: enable=%d port=%d bind=%s", enable, port, bind));
            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        if (id == IDC_BTN_CANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        /* 键盘 Esc 以 IDCANCEL(id=2) 发 WM_COMMAND；不处理会被下方兜底
         * return TRUE 吞掉，导致 Esc 无法关窗（点标题栏 X 走 WM_CLOSE 正常）。
         *
         * Esc arrives as WM_COMMAND with IDCANCEL (id=2); without this branch the
         * final "return TRUE" would swallow it and Esc could not close the dialog
         * (the title-bar X goes through WM_CLOSE and works). */
        if (id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        if (id == IDC_BTN_RESTORE) {
            /* 只把界面重置为默认值（含权限模式标签预览），实际生效由
             * 「确定」统一完成 —— 这样点「取消」仍可放弃本次修改，与窗体
             * 其余控件语义一致。
             *
             * Only resets the UI to the defaults (including the permission-mode
             * preview); nothing takes effect until OK, so Cancel still discards the
             * change - consistent with every other control in the dialog. */
            set_check(IDC_CHK_ENABLE, 0);
            set_check(IDC_CHK_ALLOW_WRITE, 0);
            set_check(IDC_CHK_LAN_ADVANCED, 0);
            enable_ctl(IDC_TXT_BIND, FALSE);
            char port_txt[16];
            _snprintf_s(port_txt, sizeof(port_txt), _TRUNCATE, "%d", VBDMCP_DEFAULT_PORT);
            set_text(IDC_EDIT_PORT, port_txt);
            set_text(IDC_TXT_BIND, VBDMCP_DEFAULT_BIND);
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
            /* 「权限模式」显示的是界面勾选对应的目标模式（确定后才生效）
             * The permission-mode label previews the mode of the current checkbox (applied on OK) */
            refresh_mode_label();
            return TRUE;
        }

        if (id == IDC_CHK_LAN_ADVANCED) {
            const int on = (get_check(IDC_CHK_LAN_ADVANCED) == BST_CHECKED);
            if (on) {
                if (!confirm_open_bind_warning(hDlg, NULL)) {
                    set_check(IDC_CHK_LAN_ADVANCED, 0);
                    return TRUE;
                }
                enable_ctl(IDC_TXT_BIND, TRUE);
                set_text(IDC_TXT_BIND, "0.0.0.0");
            } else {
                enable_ctl(IDC_TXT_BIND, FALSE);
                set_text(IDC_TXT_BIND, VBDMCP_DEFAULT_BIND);
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
                apply_tab((int)SendMessageW(g_tab_hwnd, TCM_GETCURSEL, 0, 0));
                return TRUE;
            }
            /* 端口微调钮：点击 + / - 时把"端口编辑框"的值 ±1 —— 编辑框文本是
             * 端口值的唯一事实来源。不依赖 up-down 自带的 buddy 机制：
             * UDS_SETBUDDYINT 在本环境写回时会把文本写成首字符（实测），
             * 且它的 16 位范围接口装不下 65535。
             *
             * Port spinner: + / - adjust the port edit box by 1; the box text is
             * the single source of truth. The up-down's own buddy mechanism is not
             * used: UDS_SETBUDDYINT writes back only the first character here
             * (measured) and its 16-bit range API cannot hold 65535. */
            if (nh->idFrom == IDC_SPIN_PORT && nh->code == UDN_DELTAPOS) {
                LPNMUPDOWN ud = (LPNMUPDOWN)lp;
                char buf[16] = {0};
                get_item_text(IDC_EDIT_PORT, buf, sizeof(buf));
                int v = atoi(buf);
                if (v < 1) v = VBDMCP_DEFAULT_PORT;   /* 文本为空/非法：从默认端口起算 / empty or invalid text starts from the default port */
                /* 符号实测（ui_preview 的合成点击探针）：up 箭头 iDelta = -1、
                 * down 箭头 = +1（位置按"向下增长"约定），故取负后 + 为 +1。
                 *
                 * Measured signs (ui_preview synthetic click probe): the up arrow
                 * sends iDelta = -1, the down arrow +1 ("grows downwards"), hence
                 * the negation. */
                v -= ud->iDelta;
                if (v < 1)     v = 1;
                if (v > 65535) v = 65535;
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", v);
                set_text(IDC_EDIT_PORT, buf);
                update_access_page();                /* 端口变了：刷新接入页地址/片段 / port changed: refresh the URL/snippet */
                return TRUE;                         /* 阻止控件的默认处理 / suppress the default handling */
            }
        }
        return TRUE;
    }

    case WM_ACTIVATE:
        /* 后台被鼠标点中时，系统把"激活窗口"的那一次点击吃掉了；这里在激活的
         * 那一刻补发一次点击，让"切走后再回来后第一次点按钮"就生效。
         *
         * A mouse click on a background window is swallowed while activating it;
         * a click is re-posted at activation time so the first click after
         * switching back works (see replay_click_after_activate). */
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
        /* 上限＝工作区比例：150%/200% 下别让用户拖出屏幕外
         * Max track size = a fraction of the work area, so nothing is dragged off
         * screen at 150% / 200%. */
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
        /* 与窗体同一把刷（页内控件由 page_dlg_proc 处理，这里只管 shell 里的按钮）
         * Same brush as the window (page controls are handled by page_dlg_proc;
         * this covers the shell's buttons only). */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)ui_bg_brush();
    }

    case WM_ERASEBKGND: {
        /* 窗体底色由我们自己刷（不依赖主题/对话框类刷）
         * The window background is painted by us (no theme / dialog-class brush). */
        RECT rc;
        GetClientRect(hDlg, &rc);
        FillRect((HDC)wp, &rc, ui_bg_brush());
        return 1;
    }

    case WM_CTLCOLORDLG: {
        /* shell 与两个属性页的底色统一成同一把刷：属性页是子窗口对话框，
         * 主题可能给它和 shell 不同颜色，这里强制一致。
         *
         * The shell and both pages share one brush: a page is a child dialog and
         * the theme might color it differently, so consistency is enforced. */
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        SetBkMode((HDC)wp, OPAQUE);
        return (LRESULT)ui_bg_brush();
    }

    case WM_CTLCOLOREDIT: {
        /* 输入框（只读字段也是 EDIT）：同一个背景色 + 不透明
         * Edit fields (read-only ones too): same background, opaque. */
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
    return FALSE;   /* 对话框过程：未处理的消息返回 FALSE / dialog proc: unhandled messages return FALSE */
}

/* ============================================================
 * 对话框模板初始化助手 / Dialog template helpers
 *
 * 注意：控件分属 shell 与两个页窗口三棵子窗口树，所以一律用 find_ctl /
 * EnumChildWindows 这类"跨页"写法，不要直接 GetDlgItem(hDlg, id)。
 *
 * Note: controls live in three child-window trees (shell + two pages), so always
 * use the cross-page helpers find_ctl / EnumChildWindows instead of a direct
 * GetDlgItem(hDlg, id).
 * ============================================================ */

/* 把自定义字体应用到所有子控件（EnumChildWindows 会遍历整棵子树，
 * 正好覆盖页窗口里的控件；ComboLBox 由 ComboBox 自己处理）
 *
 * Applies the custom font to every child control (EnumChildWindows walks the whole
 * subtree, covering the page windows; ComboLBox is managed by the ComboBox). */
static void apply_font_to_children(HWND hDlg)
{
    if (!g_hUiFont || !hDlg) return;
    EnumChildWindows(hDlg, apply_font_cb, (LPARAM)g_hUiFont);
}

/* 取对话框所在显示器：优先父窗口所在屏，否则主屏
 * Monitor of the dialog: the parent window's monitor first, then the primary. */
static HMONITOR monitor_for_dialog(HWND hDlg)
{
    HWND parent = (hDlg && GetParent(hDlg) && IsWindow(GetParent(hDlg))) ? GetParent(hDlg) : NULL;
    if (parent) return MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
    POINT origin{ 0, 0 };
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

/* 把窗口（含边框）调整到指定客户区尺寸
 * Resizes the window (border included) to the requested client size. */
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
     * 不管宿主是否 DPI aware 都有效：坐标与 GetMonitorInfo 同一套虚拟化空间。
     *
     * Work-area cap: S(design size) can approach a full screen at 150% / 200%, so
     * it is scaled back. Works whether or not the host is DPI aware, because the
     * coordinates share the same virtualization space as GetMonitorInfo. */
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
    /* 别被压到比边框还小 / never clamped below the frame size */
    if (want_w < frame_w + 80) want_w = frame_w + 80;
    if (want_h < frame_h + 80) want_h = frame_h + 80;

    SetWindowPos(hDlg, NULL, 0, 0, want_w, want_h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 居中：优先相对父窗口，其次主显示器工作区
 * Centering: relative to the parent window, otherwise the primary work area. */
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

/* 插入两个属性页标题（双语；模板里没有，运行时插）
 * Inserts the two page titles (bilingual; not in the template, added at runtime). */
static void setup_tab_pages(HWND hDlg)
{
    HWND tab = GetDlgItem(hDlg, IDC_TAB);
    g_tab_hwnd = tab;
    if (!tab) return;

    /* 对话框是 Unicode（DialogBoxParamW），所以 tab 也是 Unicode 控件：
     * 必须用 TCITEMW + TCM_INSERTITEMW。本工程不定义 UNICODE，
     * TCM_INSERTITEM/TCITEM 会映射到 A 版，因此这里写全名。
     * 标题先取成 std::wstring 本地副本：_W() 的指针来自轮转池，只保证立即有效。
     *
     * The dialog is Unicode (DialogBoxParamW), hence so is the tab control:
     * TCITEMW + TCM_INSERTITEMW are required. This project does not define
     * UNICODE, so TCM_INSERTITEM/TCITEM would map to the A variants - spell the
     * W names out. Titles are copied into local std::wstring: _W() hands out a
     * rotating-pool pointer that is only valid right away. */
    TCITEMW tci;
    const std::wstring titles[2] = { I18n_ToWide(_("Server settings")),
                                     I18n_ToWide(_("Integration info")) };

    for (int i = 0; i < 2; i++) {
        ZeroMemory(&tci, sizeof(tci));
        tci.mask = TCIF_TEXT;
        tci.pszText = (LPWSTR)titles[i].c_str();
        SendMessageW(tab, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&tci);
    }
}

/* 模板里写英文（方便用资源编辑器查看），这里统一覆盖成当前语言。
 * 页内控件走 set_text()（跨页寻址），shell 标题直接设窗口文本。
 *
 * The template holds English (handy in the resource editor); this overrides every
 * caption with the current language. Page controls use set_text() (cross-page
 * addressing); the shell title is set directly. */
static void apply_captions(HWND hDlg)
{
    SetWindowTextW(hDlg, _W("VB Decompiler MCP Server Settings"));
    set_text(IDC_GRP_SERVICE,      _("Service"));
    set_text(IDC_GRP_SECURITY,     _("Permissions and security"));
    set_text(IDC_CHK_ENABLE,       _("Enable MCP Server"));
    set_text(IDC_CHK_ALLOW_WRITE,  _("Allow MCP clients to write project data"));
    set_text(IDC_CHK_LAN_ADVANCED, _("Allow LAN access (advanced)"));
    set_text(IDC_LBL_SRV_STATUS,   _("Server status:"));
    set_text(IDC_LBL_BIND,         _("Bind IP address:"));
    set_text(IDC_LBL_PORT,         _("Port:"));
    set_text(IDC_LBL_MODE,         _("Permission mode:"));
    set_text(IDC_LBL_AI,           _("Client app:"));
    set_text(IDC_LBL_PATH,         _("Config path:"));
    set_text(IDC_LBL_URL,          _("Connection URL:"));
    set_text(IDC_LBL_SNIPPET,      _("Config snippet:"));
    set_text(IDC_LBL_STEPS,        _("How to connect:"));
    set_text(IDC_BTN_RESTORE,      _("Restore defaults"));
    set_text(IDC_BTN_HELP,         _("Help"));
    set_text(IDC_BTN_OK,           _("OK"));
    set_text(IDC_BTN_CANCEL,       _("Cancel"));
}

/* ============================================================
 * 布局：按当前客户区摆放所有控件（WM_SIZE 时也会调用）
 *
 * 两级布局：
 *   1) shell 客户区：底部按钮栏 + tab 控件；
 *   2) 两个页窗口停靠在 tab 显示区，再各自按页客户区排一遍页内控件。
 * 排版规则：左侧标签列 labelW，输入列起点 colX，右侧状态区靠右。
 *
 * Layout: places every control for the current client area (also called on WM_SIZE).
 *
 * Two levels:
 *   1) shell client area: bottom button bar + tab control;
 *   2) both page windows docked into the tab display area, each laying out its own
 *      controls for the page client area.
 * Rules: label column on the left (labelW), input column starts at colX, status
 * area aligned to the right.
 * ============================================================ */

/* tab 控件的显示区，换算成 shell 客户区坐标
 *
 * 注意 1：Win32 版 SysTabControl32 并不实现 TCM_GETCLIENTRECT（实测返回 0），
 *         所以这里用固定内缩兜底：顶部让开标签条（S(26)），四周再收 S(6)。
 * 注意 2：TCM_GETCLIENTRECT 与 GetClientRect 给的都是 **tab 客户区** 坐标，
 *         而页窗口要放在 shell 客户区里，必须做两次换算：
 *         tab 客户区 --ClientToScreen--> 屏幕 --ScreenToClient--> shell 客户区。
 *         少一次换算会把页窗口放到屏幕原点附近（实测 -926,-312）。
 *
 * The tab control's display area converted into shell client coordinates.
 *
 * Note 1: the Win32 SysTabControl32 does not implement TCM_GETCLIENTRECT
 * (measured: returns 0), so a fixed inset is used as the fallback: leave the tab
 * strip at the top (S(26)) and inset S(6) on the other sides.
 * Note 2: TCM_GETCLIENTRECT and GetClientRect both yield TAB client coordinates
 * while the pages live in the shell client area, so two conversions are required:
 * tab client --ClientToScreen--> screen --ScreenToClient--> shell client. Skipping
 * one places the page near the screen origin (measured: -926,-312). */
static void tab_display_rect(HWND hShell, HWND hTab, RECT *out)
{
    RECT tc, disp;
    ZeroMemory(out, sizeof(*out));
    GetClientRect(hTab, &tc);

    const BOOL ok = SendMessageW(hTab, TCM_GETCLIENTRECT, 0, (LPARAM)&disp);
    /* 顶部标签条至少有 S(16) 高，否则 TCM_GETCLIENTRECT 的结果不可信，
     * 退回"客户端区去掉上下内缩"的保守算法。
     *
     * The top tab strip is at least S(16) tall; a smaller result from
     * TCM_GETCLIENTRECT is not trustworthy, so the conservative
     * "client area minus insets" fallback is used. */
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

/* 页 1：服务设置（页客户区坐标）/ page 1: server settings (page client coordinates) */
static void layout_page_server(int W, int H)
{
    const int px = S(14), py = S(12);
    const int pw = W - 2 * px;
    const int bottom = H - py;
    const int rowH = S(26), editH = S(28);
    /* 标签列宽 = max(设计值 S(150), 当前语言最长标签 + 内边距)，最多加宽 S(70)
     * （给右侧"LAN 高级选项"勾选框留位置）。英文/中文正好落在设计值上。
     * Label column = max(design S(150), longest label in this language + padding),
     * capped at +S(70) so the LAN checkbox keeps its room; English/Chinese land
     * exactly on the design value. */
    int labelW = S(150);
    {
        const int lbls[] = { IDC_LBL_BIND, IDC_LBL_PORT };
        for (size_t i = 0; i < sizeof(lbls) / sizeof(lbls[0]); i++) {
            const int w = control_text_px(lbls[i]) + S(14);
            if (w > labelW) labelW = w;
        }
        if (labelW > S(150) + S(70)) labelW = S(150) + S(70);
    }
    const int colX = px + S(16) + labelW + S(10);
    const int rxLabel = px + pw - S(288);

    const int grp1H = S(146);   /* 三行：启用/状态、绑定IP/LAN、端口 / three rows: enable/status, bind IP/LAN, port */
    put(IDC_GRP_SERVICE, px, py, pw, grp1H);
    put(IDC_CHK_ENABLE, px + S(16), py + S(24), S(210), rowH);

    put(IDC_LBL_SRV_STATUS, rxLabel, py + S(24), S(150), rowH);
    put(IDC_LBL_STATUS, rxLabel + S(156), py + S(24), S(124), rowH);

    /* "绑定 IP 地址"输入框只需放得下 IPv4/IPv6 地址，收窄到 S(140) 后把右侧的
     * "局域网高级选项"勾选框左移并加宽 —— 它的译文（希腊语/日语/俄语…）比英文长
     * 得多，原来挤在右边必然超出窗体。
     *
     * The bind-IP field only has to fit an address, so it shrinks to S(140) and the
     * "LAN access (advanced)" checkbox moves left and grows: its translation is far
     * longer than English (Greek/Japanese/Russian...) and used to run off the form. */
    const int bindW = S(140);
    put(IDC_LBL_BIND, px + S(16), py + S(64), labelW, rowH);
    put(IDC_TXT_BIND, colX, py + S(62), bindW, editH);
    put(IDC_CHK_LAN_ADVANCED, colX + bindW + S(12), py + S(64),
        px + pw - (colX + bindW + S(12)) - S(16), rowH);

    put(IDC_LBL_PORT, px + S(16), py + S(104), labelW, rowH);
    put(IDC_EDIT_PORT, colX, py + S(102), S(110), editH);
    put(IDC_SPIN_PORT, colX + S(112), py + S(102), S(24), editH);

    const int grp2Y = py + grp1H + S(12);
    const int grp2H = bottom - grp2Y;
    put(IDC_GRP_SECURITY, px, grp2Y, pw, grp2H);

    /* 「权限模式」放在**权限与安全**分组里，与「允许 MCP 客户端写入项目数据」
     * 同一行（它描述的正是这个勾选框的当前效果，语义上也应放在一起）；
     * 勾选框宽度收窄，给右侧的「权限模式」腾出位置。
     *
     * The permission-mode label sits in the "Permissions and security" group on the
     * same row as "Allow MCP clients to write project data" (it describes exactly
     * that checkbox); the checkbox is narrowed to make room for it. */
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

/* 页 2：接入信息（页客户区坐标）/ page 2: integration info (page client coordinates) */
static void layout_page_access(int W, int H)
{
    const int px = S(14), py = S(12);
    const int pw = W - 2 * px;
    const int bottom = H - py;
    const int rowH = S(26), editH = S(28);
    /* 标签列宽 = max(设计值 S(150), 当前语言最长标签 + 内边距)：接入页的标签
     * （Config path: / Config snippet: / How to connect: …）在希腊语、法语等语言里
     * 明显更长，固定列宽会把它们裁掉。加宽后输入框相应右移、变窄。
     * Label column = max(design S(150), longest label in this language + padding):
     * the access-page labels are much longer in Greek, French, etc., and a fixed
     * column clipped them. The fields shift right and get narrower accordingly. */
    int labelW = S(150);
    {
        const int lbls[] = { IDC_LBL_AI, IDC_LBL_PATH, IDC_LBL_URL,
                             IDC_LBL_SNIPPET, IDC_LBL_STEPS };
        for (size_t i = 0; i < sizeof(lbls) / sizeof(lbls[0]); i++) {
            const int w = control_text_px(lbls[i]) + S(14);
            if (w > labelW) labelW = w;
        }
        if (labelW > S(150) + S(70)) labelW = S(150) + S(70);
    }
    const int colX = px + S(16) + labelW + S(10);
    /* 输入框铺到右边（无复制按钮）；下拉框与"绑定 IP 地址"输入框同宽（S(200)）
     * Edit fields stretch to the right (no copy buttons); the combo matches the
     * width of the "bind IP" field (S(200)). */
    const int fieldW = pw - (colX - px) - S(8);
    const int comboW = S(200);

    /* 行距取 36：默认客户区高度在 150% 下要更紧凑，省下的高度还给下面的
     * "接入说明"，否则说明文字的可见行数会变少。 */
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

/* ============================================================
 * 译文自适应宽度 / translation-aware width fitting
 *
 * 译文普遍比英文长，而控件宽度是照英文调的固定值 —— 装不下的按钮/标签会被裁字。
 * 2026-10 用 tools/audit_languages.ps1 全语言审计的实测：en / zh / zh-TW 为 0，
 * 其余语言 2~16 个控件被裁（el 16、fr 14、it 12、ru/uk 11 …）。
 *
 * 这里在排版完成后逐控件量一次文本宽度，装不下的朝"有空隙的一侧"撑开：
 *   - 右对齐标签（SS_RIGHT 的表头式标签）朝**左**撑，保持与右侧输入框的间距；
 *   - 其余（按钮 / 复选框 / 左对齐标签）朝**右**撑；
 *   - 最多撑到距最近同层控件的 S(6) 处；没有空隙就保持原样（宁可裁一点，也不重叠）。
 * 只处理单行控件：多行 EDIT、会折行的 Static（SS_LEFT / SS_LEFTNOWORDWRAP）、
 * 输入与容器类控件（Edit / ComboBox / ScrollBar / Tab / up-down）一律不动。
 * 英文与中文本来就不缺空间，所以这条路径对它们没有任何副作用（可回归比对）。
 *
 * Translated strings are usually longer than English while the widths are fixed
 * values tuned for English, so buttons/labels end up clipped (the 2026-10
 * all-language audit: 0 for en / zh / zh-TW, 2~16 controls elsewhere). After the
 * layout pass this measures each control's text and grows the ones that do not
 * fit towards the side that has room: right-aligned labels grow left (keeping
 * their gap to the input field), everything else grows right, never closer than
 * S(6) to the nearest sibling - and if there is no room it stays as is (clipping
 * a couple of characters beats overlapping controls). Only single-line controls
 * are touched; English and Chinese never need this, so they are unaffected.
 * ============================================================ */
static int measure_text_px(HWND h, const wchar_t *text, int len)
{
    HFONT font = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    HDC hdc = GetDC(h);
    HGDIOBJ old = font ? SelectObject(hdc, font) : NULL;
    SIZE sz;
    ZeroMemory(&sz, sizeof(sz));
    GetTextExtentPoint32W(hdc, text, len, &sz);
    if (old) SelectObject(hdc, old);
    ReleaseDC(h, hdc);
    return sz.cx;
}

/* 取某个控件当前文本的像素宽度（按语言自适应排版用；控件不存在或无文本返回 0）
 * Pixel width of a control's current text (used by the language-adaptive layout);
 * returns 0 when the control does not exist or holds no text. */
static int control_text_px(int id)
{
    HWND h = find_ctl(id);
    if (!h) return 0;

    wchar_t text[512];
    text[0] = L'\0';
    GetWindowTextW(h, text, (int)(sizeof(text) / sizeof(text[0])) - 1);
    const int len = (int)wcslen(text);
    if (len <= 0) return 0;
    return measure_text_px(h, text, len);
}

/* 同层里与 h 垂直重叠的邻居之间剩下多少空隙（像素）
 * Free space next to h, measured against vertically overlapping siblings only. */
static int free_space_px(HWND h, int to_left)
{
    HWND parent = GetParent(h);
    if (!parent) return 0;

    RECT me;
    GetWindowRect(h, &me);

    RECT pr;
    GetClientRect(parent, &pr);
    POINT org = { pr.left, pr.top };
    ClientToScreen(parent, &org);

    int room = to_left ? (me.left - org.x) : ((org.x + pr.right) - me.right);

    for (HWND s = GetWindow(parent, GW_CHILD); s; s = GetWindow(s, GW_HWNDNEXT)) {
        if (s == h) continue;   /* 隐藏的同层控件也算邻居（另一页的布局同样占用空间） */
        RECT sr;
        GetWindowRect(s, &sr);
        if (sr.bottom <= me.top || sr.top >= me.bottom) continue;   /* 不相邻 / not adjacent */
        if (to_left) {
            if (sr.right <= me.left && me.left - sr.right < room) room = me.left - sr.right;
        } else {
            if (sr.left >= me.right && sr.left - me.right < room) room = sr.left - me.right;
        }
    }
    return room;
}

static void fit_one_control(HWND h)
{
    /* 不按可见性过滤：另一个属性页在首次排版时是隐藏的，跳过它就等于那一页的
     * 控件永远得不到补偿（接入页的四个标签最初就是这么漏掉的）。
     * No visibility filter: the other tab page is hidden during the first layout, so
     * skipping it would leave that page unfitted forever (that is how the four
     * access-page labels were missed at first). */
    if (!h) return;

    char cls[40] = { 0 };
    GetClassNameA(h, cls, (int)sizeof(cls) - 1);
    const LONG style = GetWindowLongW(h, GWL_STYLE);

    /* 输入/容器类控件不参与撑宽（撑了也没意义，还会破坏布局）
     * Input and container classes are left alone. */
    if (_stricmp(cls, "Edit") == 0 || _stricmp(cls, "ComboBox") == 0 ||
        _stricmp(cls, "ScrollBar") == 0 || _stricmp(cls, "SysTabControl32") == 0 ||
        _stricmp(cls, "msctls_updown32") == 0) {
        return;
    }

    const int is_static = (_stricmp(cls, "Static") == 0);
    if (is_static) {
        const LONG t = style & 0x001FL;                 /* SS_TYPEMASK */
        if (t == 0x0000L || t == 0x000CL) return;       /* SS_LEFT / SS_LEFTNOWORDWRAP：折行 */
    } else if ((style & 0x000FL) == 0x0007L) {
        return;                                         /* BS_GROUPBOX：容器 */
    }

    wchar_t text[512];
    text[0] = L'\0';
    GetWindowTextW(h, text, (int)(sizeof(text) / sizeof(text[0])) - 1);
    int len = (int)wcslen(text);
    for (int i = 0; i < len; i++) {                     /* 只量第一行 */
        if (text[i] == L'\r' || text[i] == L'\n') { len = i; break; }
    }
    if (len <= 0) return;

    /* 复选框/单选框的文本宽度不含左边的方框，额外留出方框+内边距
     * Check/radio buttons need room for the box itself. */
    int extra = S(8);
    if (!is_static) {
        const LONG t = style & 0x000FL;
        if (t == 0x0002L || t == 0x0003L || t == 0x0004L ||
            t == 0x0005L || t == 0x0006L || t == 0x0009L) {
            extra += S(20);
        }
    }

    RECT r;
    GetWindowRect(h, &r);
    const int have = r.right - r.left;
    const int need = measure_text_px(h, text, len) + extra;
    if (need <= have) return;

    const int to_left = is_static && ((style & 0x001FL) == 0x0002L);   /* SS_RIGHT */
    int room = free_space_px(h, to_left) - S(6);
    if (room <= 0) return;

    int add = need - have;
    if (add > room) add = room;
    if (add <= 0) return;

    HWND parent = GetParent(h);
    POINT tl = { r.left, r.top };
    ScreenToClient(parent, &tl);
    SetWindowPos(h, NULL, to_left ? tl.x - add : tl.x, tl.y,
                 have + add, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

static BOOL CALLBACK fit_cb(HWND h, LPARAM lp)
{
    (void)lp;
    fit_one_control(h);
    return TRUE;
}

/* 排版收尾：把两个页与 shell 上的单行控件都过一遍
 * Layout epilogue: run the fit pass over both pages and the shell. */
static void fit_widths(HWND hDlg)
{
    if (!hDlg) return;
    for (int i = 0; i < 2; i++) {
        if (g_page[i]) EnumChildWindows(g_page[i], fit_cb, 0);
    }
    EnumChildWindows(hDlg, fit_cb, 0);
}

static void layout_dialog(HWND hDlg)
{
    HWND tab = find_ctl(IDC_TAB);
    if (!hDlg || !tab) return;

    /* 跨屏导致 DPI 变化时重建字体 / rebuild the font when the DPI changes (moving across monitors) */
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

    /* ---- 底部按钮栏（shell）/ bottom button bar (shell) ----
     * 宽度按当前语言的按钮文本取（英文/中文 = 设计值 S(140)/S(100)，长语言自动
     * 变宽；"恢复默认设置"在德语里比英文长 60%，固定 S(140) 必然裁字）。
     *
     * Button widths follow the current language's text: the design values
     * S(140)/S(100) hold for English and Chinese, while longer languages grow
     * ("Restore defaults" is 60% longer in German and was clipped at S(140)). */
    const int wRestore = (control_text_px(IDC_BTN_RESTORE) + S(24) > S(140))
                       ? control_text_px(IDC_BTN_RESTORE) + S(24) : S(140);
    const int wHelp    = (control_text_px(IDC_BTN_HELP)    + S(28) > S(100))
                       ? control_text_px(IDC_BTN_HELP)    + S(28) : S(100);
    const int wCancel  = (control_text_px(IDC_BTN_CANCEL)  + S(28) > S(100))
                       ? control_text_px(IDC_BTN_CANCEL)  + S(28) : S(100);
    const int wOk      = (control_text_px(IDC_BTN_OK)      + S(28) > S(100))
                       ? control_text_px(IDC_BTN_OK)      + S(28) : S(100);

    const int btnY = H - m - btnH;
    put(IDC_BTN_RESTORE, m, btnY, wRestore, btnH);
    put(IDC_BTN_HELP, m + wRestore + S(10), btnY, wHelp, btnH);
    put(IDC_BTN_CANCEL, W - m - wCancel, btnY, wCancel, btnH);
    put(IDC_BTN_OK, W - m - wCancel - S(10) - wOk, btnY, wOk, btnH);

    /* ---- tab（shell） ---- */
    const int tabY = m;
    const int tabH = btnY - m - tabY;
    put(IDC_TAB, m, tabY, W - 2 * m, tabH);

    /* ---- 两个页窗口停靠在 tab 显示区，再排页内控件 ----
     * dock both page windows into the tab display area, then lay out their controls ---- */
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

    /* 收尾：按当前语言把装不下的单行控件撑开（译文比英文长时才有动作）
     * Epilogue: grow single-line controls that do not fit in this language. */
    fit_widths(hDlg);
}

/* ============================================================
 * 公共入口：用对话框模板创建模态设置窗体
 * Public entry: creates the modal settings dialog from the template
 *
 * 注意 hInstance 必须是**本 DLL**：插件由 VB Decompiler 加载，
 * 宿主 hInstance 不是我们，DialogBoxParam 会去错模块找模板。
 *
 * Note: hInstance must be THIS DLL - VB Decompiler loads the plugin, so the host
 * hInstance is not ours and DialogBoxParam would look in the wrong module.
 * ============================================================ */
void vbdmcp_options_dialog(HWND hParent)
{
    if (g_options_hwnd && IsWindow(g_options_hwnd)) {
        SetForegroundWindow(g_options_hwnd);
        return;
    }

    /* 每次开窗先重读语言，让界面跟用户最近改的语言一致
     * Re-read the language on every open so the UI matches the user's latest choice. */
    I18n_Refresh();

    HINSTANCE hInst = vbdmcp_self_module();
    if (!hInst) hInst = (HINSTANCE)GetModuleHandleA(NULL);

    /* DialogBoxParam 自带模态循环：禁用父窗、IsDialogMessage、
     * 结束自动回到这里，无需手写消息循环。
     * 用 W 版：整个界面（含 MessageBoxW）都要能显示非 ANSI 语言（波斯语、
     * 阿拉伯语、希伯来语…）；模板本身是 Unicode（rc.exe 生成的是 UTF-16 模板，
     * 由 tools/verify_dialog_templates.ps1 在构建后校验）。
     *
     * DialogBoxParam runs the modal loop itself (disables the owner, does
     * IsDialogMessage, returns here when done) - no hand-written loop needed.
     * The W variant is used so the whole surface (MessageBoxW included) can show
     * languages that are not representable in ANSI (Persian, Arabic, Hebrew...).
     * The templates themselves are Unicode (rc.exe emits UTF-16 templates -
     * verified after each build by tools/verify_dialog_templates.ps1). */
    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_VBDMCP_OPTIONS), hParent, dlg_proc, 0);

    g_options_hwnd = NULL;
    g_shell = NULL;
    g_page[0] = NULL;
    g_page[1] = NULL;
    g_tab_hwnd = NULL;
}

/* 进程退出时释放字体/背景刷（GDI 对象必须在进程退出前释放）
 * Releases the font / background brush at process exit (GDI objects must be freed). */
void vbdmcp_options_dialog_cleanup(void)
{
    if (g_hUiFont) { DeleteObject(g_hUiFont); g_hUiFont = NULL; }
    /* g_hUiBgBrush 是自建的实色刷，必须删（系统刷不能删）
     * g_hUiBgBrush is our own solid brush and must be deleted (system brushes must not). */
    if (g_hUiBgBrush) { DeleteObject(g_hUiBgBrush); g_hUiBgBrush = NULL; }
}
