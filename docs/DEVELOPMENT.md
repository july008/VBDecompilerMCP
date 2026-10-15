# 开发文档（vbmcp）

面向二次开发的内部文档：工具命名规范、模块划分、线程模型、新增工具与新增端点的流程。

---

## 1. 工具命名规范（强制）

所有 MCP 工具名必须严格分为两组，**不得新增第三种命名风格**：

| 前缀 | 语义 | 判定标准 | 在 `listTools` 中的可见性 | 可调用条件 |
|---|---|---|---|---|
| `vb_read_*` | 只读 | 只读取 VB Decompiler 内部数据，不改变任何状态 | 始终可见 | 会话已授权 |
| `vb_write_*` | 写入 | 会调用 `VB_Set*` / `VB_UpdateAll` 等改变工程或 UI 状态 | 仅 `AllowWrite=1` 时可见 | 会话已授权 **且** `AllowWrite=1` |

补充规则：

1. **统一 `vb_` 前缀**：避免与同进程/同客户端上的其他 MCP 服务器工具重名。
2. **权限由工具表决定，不靠命名判断**：读写属性写在 `mcp_tools.cpp: g_tools[]` 的
   `is_write` 字段，`mcp_build_tools_into()` 与 `mcp_dispatch_tool()` 都读该字段，
   两者必须同时生效（动态过滤 + 入口二次校验）。
3. **只读工具不得有副作用**：包括不得调用 `VB_Set*`、不得触发 `VB_UpdateAll`、
   不得改变状态栏/光标位置。UI 相关的只读查询（如 `vb_read_active_coords`）允许读取，
   但不允许跳转。
4. **annotations 必须如实标注**：`readOnlyHint = !is_write`、`destructiveHint`（不可逆/清空类
   为 1）、`idempotentHint`、`openWorldHint = false`（本地工具恒为 false）。
5. 新增工具时同步更新：`README.md` / `README_EN.md` 的工具表格、工具表中的
   `desc` 英文描述（`_("...")`，缺失时以工具名兜底），并运行
   `python tools/extract_i18n.py` 同步语言包；如涉及结构变化再更新
   `docs/REFACTOR_PROGRESS.md`。

### 当前工具清单（28 = 13 只读 + 15 写入；完整说明与参数见 README.md 的工具表格）

只读：`vb_read_project_info`、`vb_read_project_text`、`vb_read_list_forms`、`vb_read_form`、
`vb_read_list_modules`、`vb_read_module`、`vb_read_list_module_functions`、`vb_read_function`、
`vb_read_sub_main`、`vb_read_active_text`、`vb_read_active_disasm_text`、`vb_read_active_coords`、
`vb_read_list_frx_icons`

写入：`vb_write_project_text`、`vb_write_clear_buffers`、`vb_write_form`、`vb_write_rename_form`、
`vb_write_module`、`vb_write_module_string_references`、`vb_write_function`、`vb_write_rename_function`、
`vb_write_sub_main`、`vb_write_active_text`、`vb_write_active_disasm_text`、`vb_write_goto_line`、
`vb_write_status`、`vb_write_options`、`vb_write_update_all`

---

## 2. 模块划分

| 文件 | 职责 | 允许触碰 UI / 引擎？ |
|---|---|---|
| `mcp_http.cpp` | Streamable HTTP / SSE、后台网络线程、`UiTask` 队列、超时/取消、限流 | **绝对不允许**（只做传输层） |
| `mcp_rpc.cpp` | JSON-RPC 2.0 分发、协议层错误、会话准入授权弹窗 | 允许（本身就在 UI 主线程执行） |
| `mcp_tools.cpp` | 工具表 + 28 个工具实现 + 权限二次校验 | 允许（必须在 UI 主线程执行） |
| `vbdec_sdk.cpp` | Callback Engine 封装（`vb_get_value` / `vb_set_value` / `vb_set_status`） | 允许，且必须**只在 UI 主线程**调用 |
| `options_dialog.cpp` | 设置窗体（`SysTabControl32` 双属性页：服务设置 / 接入信息；写入权限、局域网警告、AI 客户端向导） | 允许（UI 主线程） |
| `i18n.h / i18n.cpp` | gettext 风格翻译：`_()` / `_A()` / `_F()` / `_AF()`，加载 zh.json 资源、khash 查表、GBK 轮转缓冲 | 只读取文本（UI 主线程） |
| `khash.h` | 字符串哈希表（i18n 翻译表专用，API 兼容 khash 子集） | — |
| `util.cpp` | 内存助手、分级日志（临界区串行化）、编码转换（UTF-8 ↔ ANSI/GBK） | 无 |
| `config.cpp` | 注册表读写 | 无（启动/设置窗体时调用） |

### 多语言（i18n）注意事项

- 词条以**英文原文**作为 msgid，直接写在调用点：`_("Enable MCP Server")`；
  中文译文在 `lang_res/zh.json`，构建时由 `res/vbmcp.rc` 以 `RCDATA("JSON_ZH")`
  嵌入 `vbmcp.dll`，运行时 `i18n.cpp` 用
  `GetModuleHandleExA(FROM_ADDRESS)` 取本模块句柄后 `FindResourceA` 读取。
- 宏一览：

  | 宏 | 用途 | 返回 |
  |---|---|---|
  | `_("msg")` | 通用 / MCP JSON 报文 / 日志 | UTF-8，生命周期为静态字符串 |
  | `_A("msg")` | Win32 **ANSI** 控件（`MessageBoxA`/`SetDlgItemTextA`/`CreateWindowExA`） | GBK，轮转缓冲，立即使用 |
  | `_F("fmt %d", x)` | 先翻译再格式化（UTF-8），用于 MCP 错误消息；支持 `%s %d %zu %.2f %%` 等占位符 | 轮转缓冲，立即使用 |
  | `_AF("fmt %d", x)` | 同上但输出 GBK，用于 ANSI 弹窗；占位符支持同上 | 轮转缓冲，立即使用 |
  | `N_("msg")` | 仅用于提取脚本标记静态字符串，本身不翻译 | 原字符串 |

- **禁止翻译**：MCP 工具名（`vb_read_*` / `vb_write_*`）、JSON 键名、
  注册表键值、协议常量。
- 新增/修改展示字符串的流程：
  1. 源码里写 `_("English text")`（Windows ANSI 控件用 `_A("English text")`）；
  2. 运行 `python tools/extract_i18n.py`——语言包与源码自动对齐：
     新增字符串 → **追加到文件末尾**、值为 `null`；源码已删除的字符串 → **从语言包移除**；
     文件不存在则创建；**已有译文绝不改动、绝不重复输出**（键顺序也不变）。
     同时生成各语言的待翻译清单（`lang_res/untranslated.txt` 与
     `lang_res/untranslated-<code>.txt`）；
  3. 填译文（**可以只填一部分**，例如法语先翻一条）→ 重新 build；
  4. `python tools/extract_i18n.py --check` 在 CI 里校验结构（不写文件）：
     有新增/失效词条或语言包缺失都算失败；`--strict` 时"还有 `null` 未翻译"也算失败。
  - 这形成**递进式翻译**：代码加一条字符串，语言包尾部多一条 `null`；
    代码删一条，对应条目随即消失，不会留下无主的旧译文。
  - 注意：`_()` 只能写在**被扫描的源文件**里。写在 `src/i18n.h`、`khash.h`
    这类被排除的文件（脚本里的 `EXCLUDE_FILES`）中，会被当成"源码已删除"而清掉。
- 未填写译文的词条在运行时**自动回退英文原文**（msgid 本身），不会显示空白；
  某语言整包都没有嵌入资源时同样回退英文。
- **语言包取值语义（改脚本必须同时改 `src/i18n.cpp`）**：

  | 语言包里写的值 | 含义 | 运行时结果 |
  |---|---|---|
  | 键缺失 / `null` | 未翻译 | 返回 msgid（英文原文） |
  | `""` | **故意留空** | 返回空串（该处就显示空白） |
  | `"文本"` | 译文 | 返回译文 |

  - `extract_i18n.py` 给新词条写 `null` 而不是 `""`：早期版本用 `""` 当"未翻译"
    占位，与"故意留空"语义冲突，脚本已把这批占位一次性迁成 `null`
    （`zh.json` 的 97 条译文、`fr.json` 已填的那 1 条均不受影响）；
  - 唯一例外：msgid **自带 `%` 占位符**时 `""` 会被拒绝并回退原文——留空等于丢参数，
    会让 `vsnprintf` 错参甚至崩溃，这种"留空"几乎一定是误操作；
  - 占位符与 msgid 不一致的译文同样整条拒绝并回退原文。
- **调试覆盖**：环境变量 `VBMCP_LANG=zh|en` 优先于注册表（不改注册表即可验证双语）。
- 语言来源：注册表 `HKCU\...\VB Decompiler\Options\CustomLanguage`，
  值就是宿主里的语言名（`ChineseSimplified`、`French (native tongue v3)`、
  `Brazilian Portuguese`、`Kurdish (sorani)` …），`I18n_LangFromName()` 原样匹配
  （大小写不敏感、去两端空白、另接受 `zh`/`ja`/`pt-BR` 两字母代码）；
  设置窗体每次打开前 `I18n_Refresh()` 重新读取。
- **多语言已占位**：语言枚举见 `i18n.h` 的 `I18nLang`（24 种），映射表在
  `i18n.cpp` 的 `kLangs`。目前**只有简体中文嵌了语言包**（`JSON_ZH`），其余语言
  `I18n_LangHasPack()` 返回 0，`_()` 直接返回 msgid（英文原文），不会显示空白。
  新增语言三步：① 加 `lang_res/<code>.json`；② `res/vbmcp.rc` 加一条
  `JSON_XX RCDATA`；③ `kLangs` 对应行填 `resource` 名（并跑
  `extract_i18n.py` 同步词条）。
- 语言包只从**资源**加载，**不做磁盘回退**：资源缺失直接回英文 msgid。
  （曾有 `load_from_file("lang_res\\zh.json")` 回退，会让开发机上的磁盘副本掩盖
  "RCDATA 没链进 DLL" 的打包问题——踩过：rc 漏了 `JSON_ZH` 一行，界面却仍是中文。）
  验证嵌入：`tools/verify_i18n_resource.ps1`（需 32 位宿主）。
- `tools/selftest`（**需先构建** `tools\selftest\selftest.vcxproj`）直接链接
  `src/util.cpp` / `src/i18n.cpp` / `src/mcp_tools.cpp` / `src/vbdec_sdk.cpp`，
  在真实产品代码上覆盖：编码桥往返、语言名映射、未翻译回退、`_F()` 占位符校验
  （`I18n_FormatArity` / `I18n_FormatCompatible`）、工具 schema（工具数 /
  encoding 参数 / 描述无残留转义）。注意它**不链** `res/vbmcp.rc`，
  "中文语言包确实嵌进 DLL"仍由 `tools/verify_i18n_resource.ps1` 验证。
- 内存：`I18n_Clear()` 释放哈希表内 strdup 的 key/value 与 yyjson 文档；
  `DllMain(DLL_PROCESS_DETACH)` 调用 `I18n_PrepareShutdown()`
  （先关闭查表再释放，避免其它线程正在查表）。

（旧实现说明：早期版本用 `lang.h` 的 `T_*` 枚举 + `g_strings[T_COUNT]` 双语数组，
已于 i18n 改造中移除，`T_*` 键与 `vbmcp_t()/vbmcp_t_utf8()` 一并删除。）

- 所有 JSON 文案一律用 `yyjson_mut_obj_add_strcpy(doc, obj, key, _(msgid))` 写入，
  不做手工转义；MCP 报文必须是 **UTF-8**，因此 JSON 里只用 `_()`，不用 `_A()`。
- 线程安全：语言包表用**原子指针发布**（`g_trans` / `g_mode` 为 `std::atomic`），
  旧表"只退休不释放"，因此 MCP **网络线程**（`mcp_http.cpp` 里的 `_F()` 错误消息）
  与 UI 线程可以并发查表；真正释放只发生在 `I18n_Clear()`（DLL 卸载前）。

---

### 数据编码（MCP JSON ↔ 引擎 ANSI/GBK）

MCP 报文（JSON）必须是 UTF-8，而 VB Decompiler 引擎的文本字段是 ANSI
（中文 Windows = GBK）字节。两侧转换由 `mcp_tools.cpp` 统一处理：

| 方向 | 辅助函数 | 说明 |
|---|---|---|
| 读（引擎 → JSON） | `add_text()` / `arr_add_text()` | 默认 `vbmcp_acp_to_utf8()` 转 UTF-8；`encoding:"gbk"` 时原样输出 |
| 写（JSON → 引擎） | `set_text_value()` | 默认 `vbmcp_str_to_gbk()` 转 ANSI；`encoding:"gbk"` 时原样透传 |
| 声明 | `add_encoding_field()` | 含自由文本的结果附加 `{"encoding":"utf-8"}` / `{"encoding":"gbk"}` |

规则：

1. **每个工具都接受可选参数 `encoding`**（`"utf-8"` 默认 / `"gbk"` 旧行为），
   由 `arg_encoding()` 统一读取，schema 里由 `add_params()` 无条件加入该参数；
2. 含**自由文本**的结果（项目文本 / 表单 / 模块声明 / 函数代码 / 窗口文本）
   必须用 `add_text*` 并调用 `add_encoding_field()`；短标量（索引、地址、
   `"0"/"1"` 标志）也走 `add_text*`（ASCII 快速路径零开销），但不加 encoding 字段；
3. 写工具的响应回显**客户端原文**（UTF-8），不回显转换后的引擎字节；
4. `initialize` 的 `instructions` 向客户端声明该契约；
5. 转换失败（非法字节序列）回退原字节，绝不崩溃——见
   `vbmcp_acp_to_utf8()` / `vbmcp_str_to_gbk()` 的实现注释；
6. 回归保障：`tools/selftest` 覆盖往返转换与 schema 字段（含每个工具都有
   `encoding` 参数），改这块代码后必须跑绿。

---

### 设置窗体布局规范（`options_dialog.cpp` + `res/vbmcp.rc`）

- 字号常量 `kFontPt = 12`，用 `S(v) = MulDiv(v, dpi, 96)` 做 DPI 缩放；
  跨屏移动导致 DPI 变化时 `layout_dialog()` 会重建字体。
- **高 DPI 下的窗口尺寸**：宿主（VB Decompiler）不保证是 DPI aware，此时 Windows 会把整个
  窗口按 150%/200% 位图放大。所以设计尺寸（`kDefaultClientW/H`，单位 96dpi 逻辑像素）
  要留富余：`720x560` 在 150% 下约 1080x840 物理像素，而不是把高度做到 636（那是整屏高度）。
  `size_window_to_client()` 还会按 `kMaxWorkAreaRatioW/H`（0.92 / 0.88）把窗口压到工作区内，
  `WM_GETMINMAXINFO` 同时给出 `ptMaxTrackSize`，避免手动拖拽拖出屏幕。
  调小高度后必须同步看**接入信息页**（它的高度需求最大）：行距 36、配置片段框 `S(140)`，
  "接入说明"贴底吃剩余空间。
- **控件底色必须与窗体一致**：不要用 `GetSysColorBrush(COLOR_BTNFACE)` 给标签/复选框/
  group box 刷底色（宿主开视觉样式时对话框底色是白的，灰刷上去就是一块块"带底色的文字"）；
  也**不要**向 `DefDlgProc(WM_CTLCOLORDLG)` 要画刷（主题返回的是 pattern brush，用它填控件
  颜色与对话框实际绘制出来的底色不一致，而且 `GetPixel` 采样会骗人）。
  现在的做法（`options_dialog.cpp` 的"统一底色"小节）：
  1. `create_ui_bg_brush()` 按 `IsThemeActive()` 选 `COLOR_WINDOW`（主题，白）或
     `COLOR_BTNFACE`（经典，灰），建**一把自建实色刷** `g_hUiBgBrush`；
  2. shell 与两个属性页都在 `WM_ERASEBKGND` 里 `FillRect` 整块客户区 → 底色由我们说了算；
  3. 所有 `WM_CTLCOLOR*` 返回同一把刷，标签用 `TRANSPARENT` 背景模式（不再画自己的底）；
  4. 输入框仍是 `COLOR_WINDOW` 白底 + `OPAQUE`（它本来就是"输入框"，白底是对的）；
  5. 自建刷在 `vbmcp_options_dialog_cleanup()` 里 `DeleteObject`（系统刷不能删）。
  诊断日志会打印 `theme=` 与 `uiBg=0xRRGGBB`，改配色相关代码时先核对这两个值。
- **控件树在资源里，几何在代码里**：控件由 `res/vbmcp.rc` 的对话框模板创建
  （ID / 样式 / Tab 顺序），`layout_dialog(HWND)` 只算坐标；`WM_SIZE` 会再次调用，
  因此窗体可自由缩放（`WM_GETMINMAXINFO` 限最小客户区）。
  **`WS_THICKFRAME` 由运行期补，不在 rc 里**：资源编辑器保存 .rc 时会把它丢掉
  （"Resizing" 属性未勾选），`dlg_proc` 的 `WM_INITDIALOG` 检测到样式缺失时用
  `SetWindowLongPtr(GWL_STYLE, |WS_THICKFRAME)` + `SWP_FRAMECHANGED` 补上，
  对编辑器重写免疫；判断"能否拖拽缩放"不要只看 rc 文本。
- **一个属性页一个模板**：rc 的对话框模板是扁平的，同一个模板里的控件都是兄弟，
  表达不了"这个控件属于第 2 个属性页"，所以资源编辑器里无法分组。现在是：
  | 模板 | ID | 内容 |
  |---|---|---|
  | `IDD_VBMCP_OPTIONS` | 2000 | shell：`IDC_TAB` + 底部 4 个按钮 |
  | `IDD_PAGE_SERVER` | 2001 | 属性页 1「服务设置」的全部控件 |
  | `IDD_PAGE_ACCESS` | 2002 | 属性页 2「接入信息」的全部控件 |
  运行期 `create_page_windows()` 用 `CreateDialogParamA` 把两个页建成 shell 的
  子窗口，`layout_dialog()` 把它们摆到 tab 显示区（`tab_display_rect()`）。
- **跨页控件一律用 ID 访问**：控件分属三个窗口，业务代码用 `find_ctl(id)` /
  `set_text(id, s)` / `get_check(id)` / `send_ctl(id, ...)`，不要写
  `GetDlgItem(hDlg, id)`（hDlg 是 shell 时找不到页内控件）。
  页的对话框过程 `page_dlg_proc()` 把 `WM_COMMAND` / `WM_NOTIFY` 转发给
  shell 的 `dlg_proc()`，并就地处理 `WM_CTLCOLOR*`。
- 排版规则：左侧标签列宽 `labelW = S(150)`，输入/下拉列起点
  `colX = px + S(16) + labelW + S(10)`；右侧状态区靠右边缘
  （`rxLabel = px + pw - S(288)`）；底部按钮栏固定在客户区底部。
  两页各自按**页客户区**坐标排（`layout_page_server()` / `layout_page_access()`）。
- **文案禁忌**：分组框/按钮标题不要带 `&`（会被当作助记符吞掉，实测
  "Permissions & security" 会显示成 "Permissions _security"）；
  过长英文文案务必在 12pt 下验证是否被裁剪（用 ui_preview 截图）。
- 页模板**不要写 CAPTION**：子窗口对话框仍会在左上角把标题文字画出来，
  会压在 tab 标签上。

### rc.exe / Win32 踩坑（实测，32 位 x86 + rc.exe 10.0）

| 现象 | 原因 | 现在的做法 |
|---|---|---|
| `RC1004 unexpected end of file` | `#include` 进来的头文件带 `#define`，在 `DIALOGEX` 里用宏；或注释里混用 `/* */` 与 `//`、注释里有撇号 | ID 宏直接写在资源脚本里，C++ 侧头文件由 `tools/gen_ids_header.py` 生成；只用 `//` 注释，注释不写撇号 |
| `RC2108 expected numerical dialog constant` | ① 资源名用宏（`IDD_X DIALOGEX`）；② `EDITTEXT/PUSHBUTTON` 后面跟 exstyle | 资源名写字面量 `2001 DIALOGEX`；控件一律用 `CONTROL` 语句 |
| `RC2104 undefined keyword DS_SETFONT` | **VS 资源编辑器保存后**把 `.rc` 开头的 `#include <windows.h>` 换成了 `#include "resource.h"`，而那个文件只有 ID 常量，没有模板关键字（`DS_SETFONT` / `WS_*` / `ES_*` 都在 `winuser.h`） | 两个 `.vcxproj` 里的 `VbmcpPrepRc` 目标会生成 `$(IntDir)rc\vbmcp_build.rc` 包装文件（先 `#include <windows.h>` 再 `#include` 真正的 rc），并把它替换进 `@(ResourceCompile)`——**不要去改 `resource.h` 加 include，编辑器每次保存都会重写它** |
| 保存后 `.rc` 里找不到 ID 定义 | 编辑器把 ID 列表搬进了 `res/resource.h` | `tools/gen_ids_header.py` 同时扫 `res/vbmcp.rc` 与 `res/resource.h`；改完 ID 跑一次生成 + `--check` |
| `RC4005 HTERROR / PWR_FAIL / HTTRANSPARENT` 重定义警告 | VS 生成的 `resource.h` 里这三个宏与 `windows.h` 撞了（rc.exe 对"值相同"的重定义也报警），而 `resource.h` 不能加 include guard（会被重写） | 在**我们自己的包装 rc** 里 `#include <windows.h>` 之后 `#undef HTERROR / PWR_FAIL / HTTRANSPARENT`，让 `resource.h` 的定义成为首次定义（模板不用这三个宏）。包装里必须用 `<windows.h>` 而非更轻的 `<winresrc.h>`：后者宏集合不同，冲突会变成 `WM_KEYLAST`/`WM_MOUSELAST`/`_WIN32_WINNT`/`_WIN32_IE` 四条 |
| `RC2104 undefined keyword WS_EX_CTRLACTIVE` | rc 预处理没有 `_WIN32_WINNT`，`winuser.h` 把该宏隐藏了 | 写 `0x00000020` 并加注释（C++ 侧同理，本地 `#define` 兜底） |
| `CreateWindowExA("#32770", 模板ID, ...)` 必崩（AV） | 32 位 USER32 这条路径不可用（模板查找成功也一样崩） | 建页一律用 `CreateDialogParamA(hInst, MAKEINTRESOURCEA(id), 父, proc, 0)` |
| `TCM_GETCLIENTRECT` 返回 0 | Win32 版 `SysTabControl32` 不实现该消息 | `tab_display_rect()` 用固定内缩兜底：顶部让开 `S(26)` 标签条，四周 `S(6)` |
| 页窗口跑到屏幕左上角（坐标为负） | 把 tab **客户区**坐标当屏幕坐标调了一次 `ScreenToClient` | 必须两次换算：tab 客户区 → `ClientToScreen` → `ScreenToClient(shell)` |
| 端口微调钮（spin）点击 +/- 无反应；绑 buddy 后文本又变成首字符（`3023`→`3`） | ① rc 未写 `UDS_AUTOBUDDY`、代码也未发 `UDM_SETBUDDY`，buddy 未绑定时点击不更新任何控件；② 绑上 buddy 后 `UDS_SETBUDDYINT` 的自动写回在本环境把宽字符串按 ANSI 写入编辑框（只剩首字符）；③ 16 位 `UDM_SETRANGE` 装不下 65535（按有符号 short 解释成 -1） | **不依赖 up-down 自带的 buddy/数值机制**：`dlg_proc` 处理 `UDN_DELTAPOS`，按通知的 `iDelta` 直接读/写端口编辑框（文本是唯一事实来源）。注意符号实测为 **up = -1 / down = +1**，要取负。回归：`ui_preview` 的 `probe_port_spin`（合成鼠标点击箭头区域，与真实点击同路径） |

### 用 VS 资源编辑器改界面

`res/vbmcp.rc` 归 VS 资源编辑器管，保存后它会重写整个文件，**不要手工重排它的格式、也不要往里加 `#include`**：

- 文件会被套上 `#if !defined(AFX_RESOURCE_DLL) || defined(AFX_TARG_CHS)` + `LANGUAGE` +
  `#pragma code_page(936)`，并追加 `TEXTINCLUDE` / `DESIGNINFO` / `AFX_DIALOG_LAYOUT` 段；
- ID 列表被搬到 `res/resource.h`，`.rc` 里改用 `#include "resource.h"`；
- 自定义注释与 `#include <windows.h>` 会被**直接删掉**（实测），所以编译时不能直接用它：
  两个 `.vcxproj`（`vbmcp.vcxproj`、`tools/ui_preview/ui_preview.vcxproj`）的
  `VbmcpPrepRc` 目标会生成 `$(IntDir)rc\vbmcp_build.rc`：

  ```
  #include <windows.h>
  #include "<绝对路径>\res\vbmcp.rc"
  ```

  并用 `@(ResourceCompile) Remove/Include` 把包装文件换进编译列表。
  同一目标还把 `lang_res/zh.json` 复制到 `$(IntDir)lang_res\`——因为 rc.exe 是按
  **被编译文件**所在目录解析 RCDATA 路径的，`..\lang_res\zh.json` 才仍然成立。
  产物 `$(IntDir)rc\vbmcp_build.rc` 不要手改，每次构建都会重写；
- 三个对话框（shell + 两个属性页）在编辑器里是三个独立条目，可以各自编辑——
  这正是按属性页拆模板的目的；
- `res/vbmcp.aps` 是编辑器的图形缓存，会随时重建，已在 `.gitignore` 里；
- **改完控件 ID 后**跑一次 `python tools/gen_ids_header.py`（再 `--check` 确认），
  否则 `src/vbmcp_ids.h` 与资源脚本会脱节；
- 布局仍由 `layout_dialog()` / `layout_page_*()` 在运行期计算，模板里的坐标只是
  给编辑器看的初值；改完用 `tools/ui_preview` 截图核对实际效果。

## 3. 线程模型

```
[网络线程]                        [UI 主线程]
accept/select/HTTP/SSE            消息循环
      │  PostMessage(VBMCP_WM_TASK)  ▲
      ├──── UiTask 入队 ──────────────┤ task_pump_on_ui_thread()
      │                                └─ mcp_process_jsonrpc → 工具 → g_engine()
      ◄── notify(done) / 超时 / 断开取消 ┘
```

规则（违反即引入竞态）：

1. `mcp_http.cpp` 中除 `task_pump_on_ui_thread()` 外，**任何函数都不得调用 VB Decompiler 引擎或 UI API**。
2. 工具实现（`mcp_tools.cpp`）假定自己在 UI 主线程执行，无需再加锁。
3. 跨线程数据一律走 `std::atomic`（`g_enabled` / `g_port` / `g_allow_write` / 会话授权状态 /
   任务取消令牌）或互斥量（绑定地址、任务队列、日志写入）。
4. 任务对象用 `std::shared_ptr<UiTask>` 托管：网络线程与 UI 线程各持一份引用，
   任一侧取消/超时都不会造成悬垂访问。
5. `DllMain(DLL_PROCESS_DETACH)` 中只调用 `vbmcp_server_signal_stop()`，
   **禁止 join 线程**（loader lock 死锁）。

---

## 4. 新增一个工具的步骤

1. **实现函数**（`mcp_tools.cpp`）：

   ```cpp
   /* 每个工具实现：args 为已解析的 arguments；错误分类写入 out_err。 */
   static char *tool_read_something(yyjson_val *args, int *out_err)
   {
       const int enc = arg_encoding(args);                     // 编码契约（见"数据编码"小节）
       long long idx = arg_int(args, "thing_index", -1);
       if (idx < 0) return err_json(out_err, VBMCP_ERR_INVALID_PARAMS, _("Missing or invalid thing_index"));

       char *text = vb_get_value(VB_GetSomething, (int)idx, 0);   // 只读：引擎返回 ANSI/GBK 字节
       if (!text) return err_json(out_err, VBMCP_ERR_BUSINESS, _("No project is currently open"));

       yyjson_mut_val *root = nullptr;
       yyjson_mut_doc *doc = new_obj_doc(&root);
       if (!doc) { vbmcp_free(text); return err_json(out_err, VBMCP_ERR_INTERNAL, _("Out of memory")); }
       add_text(doc, root, "text", text, enc);                 // 入 JSON 前按 encoding 转 UTF-8
       add_encoding_field(doc, root, enc);                     // 含自由文本的结果声明编码
       vbmcp_free(text);
       return doc_to_string(doc);
   }
   ```

   约定：
   - 签名是 `char *impl(yyjson_val *args, int *out_err)`；返回值必须是
     **new alloc 的 JSON 字符串**（调用方 `vbmcp_free`），用 `doc_to_string()` 产出；
   - 错误用 `err_json(out_err, code, utf8_text)`：`VBMCP_ERR_INVALID_PARAMS` /
     `VBMCP_ERR_BUSINESS` / `VBMCP_ERR_INTERNAL` / `VBMCP_ERR_PERMISSION`；
     错误分类经 `out_err` 显式回传（不再有全局变量）；
   - 参数用 `arg_int()` / `arg_str()` 读取，缺失返回默认值；所有索引经
     `arg_index()` 校验上界（防越界调用引擎）；
   - 文本字段：读侧用 `add_text()` / `arr_add_text()`（引擎 ANSI → 结果编码），
     写侧用 `set_text_value()`（客户端编码 → 引擎 ANSI）；**不要**直接把引擎文本
     塞进 JSON（详见"数据编码"小节）。

2. **注册工具表**（`mcp_tools.cpp: g_tools[]`）：

   ```cpp
   { "vb_read_something", _("Read something from the project"), P_THING_INDEX, 0, 0, 1, tool_read_something },
   //  name                desc（英文 msgid，展示时翻译）   params      is_write destructive idempotent impl
   ```
   写入工具把 `is_write` 置 1，并按需设置 `destructive`。`desc` 传 `NULL` 表示
   暂无描述，`build_one_tool()` 会用工具名兜底。

3. **参数 schema**：在 `P_*` 位掩码中新增位，并在 `add_params()` 里补 `param_*()` 调用。

4. **文案**：工具描述写在 `g_tools[]` 的 `desc` 字段（英文原文），业务错误用
   `_F()` / `_("...")` 内联；随后运行 `python tools/extract_i18n.py` 生成
   `lang_res/zh.json` 词条并补中文译文。

5. **（可选）项目加载检查豁免**：控制类工具加入 `k_no_project_check[]`。

6. 验证：`listTools` 数量变化应等于新增数；只读模式下 `tools_count` 不应变化；
   写入工具在只读模式下调用必须返回 `-32001`。

---

### `_F()` / `_AF()`：带格式化的翻译串

**支持全部 printf 占位符**（`%s` `%d` `%i` `%u` `%zu` `%02d` `%.2f` `%c` `%p`
`%lld` … 以及字面量 `%%`），内部就是 `vsnprintf`：

```cpp
_F("MCP Server failed to start on port %d: port may be in use. Try another port.", port);
_AF("An MCP client is trying to access this VB Decompiler project data.\n\nClient: %s\n",
    client_info);
/* 中文译文里必须保留同样的占位符，否则不会生效（见下方校验） */
_F("%d files, %zu bytes", 7, (size_t)1024);
```

两道安全网（都由 `tools/selftest` 覆盖）：

1. **实参个数核对（运行期）**：`_F()/_AF()` 是变参模板，格式化前先解析格式串，
   算出"需要几个实参"（`%*.*f` 这种带 `*` 的算 3 个），与实际传入个数比对：
   - 一致 → 正常 `vsnprintf`；
   - 不一致 → 记 `ERROR` 日志并**原样返回格式串**（`%s` 会留在界面上，便于发现）。
   没有这道核对时，漏传参数会让 `vsnprintf` 从栈上取垃圾指针 → 直接访问冲突
   （自测最初就踩到了这个坑）。纯拷贝路径用 `I18n_CopyText/I18n_CopyTextAnsi`，
   **不经过** `vsnprintf`。
2. **译文占位符核对（装载时）**：`zh.json` 载入时比较 msgid 与译文的占位符序列
   （忽略 flags/宽度差异，只比 `%[length]conv` 的顺序与类型）。不一致的译文被
   **拒绝并回退英文原文**，同时记 `WARN` 日志 —— 因为把 `%d` 误译成 `%s`
   会按错误类型取参，轻则错乱、重则崩溃。原文里出现字面量 `%`
   （如帮助文本的 `%TEMP%\vbmcp.log`）时不做校验，避免误伤。

---

## 5. 自测与校验脚本

| 命令 | 作用 |
|---|---|
| `python tools/extract_i18n.py` | 扫描词条 → 同步**全部**语言包 `lang_res/<code>.json`（23 种）→ 生成各语言待翻译清单 `lang_res/untranslated*.txt` |
| `python tools/extract_i18n.py --check` | 只检查结构漂移（词条缺失/新增/失效、语言包缺失），CI 用；`--strict` 时"还有 `null` 未翻译"也算失败 |
| `python tools/extract_i18n.py --migrate-empty` | 一次性把旧脚本留下的 `""` 占位迁成 `null`（会改已有条目，默认不执行） |
| `python tools/extract_i18n.py --lang zh --lang fr` | 只处理指定语言（可重复）；代码必须与 `src/i18n.cpp` 的 `kLangs` 一致 |
| `tools\\extract_i18n.bat` | 双击运行 `extract_i18n.py` 的包装（带 `pause`） |
| `msbuild tools\selftest\selftest.vcxproj /p:Configuration=Debug /p:Platform=Win32` → `tools\selftest\Win32\Debug\selftest\selftest.exe` | 构建并运行**无宿主**纯函数自测（编码桥 / i18n / 工具 schema）；**需在仓库根目录运行**，退出码 0 = 全绿 |
| `msbuild tools\mcp_e2e\mcp_e2e.vcxproj /p:Configuration=Debug /p:Platform=Win32` → `tools\mcp_e2e\Win32\Debug\mcp_e2e\mcp_e2e.exe [port]` | 本机运行**完整 MCP 服务器**（真实传输/RPC/工具代码，无需 VB Decompiler，默认 `127.0.0.1:3024`）；`set VBMCP_E2E_ALLOW_WRITE=1` 以读写模式启动 |
| `powershell -File tools\e2e_test.ps1` | **端到端 MCP 协议测试**（12 项断言：health / initialize 版本协商 / tools-list / ping / SSE / 406·415·404·405）；先构建 mcp_e2e。这类测试捕获"能连上但拿不到数据"的传输层缺陷（见 REFACTOR_PROGRESS 8.6） |
| `tools\check_all.bat` | 一键提交前检查：`gen_ids_header --check` + `extract_i18n --check` + 自测构建/运行 + Release 构建 + 端到端协议测试 |
| `msbuild tools\ui_preview\ui_preview.vcxproj /p:Configuration=Debug /p:Platform=Win32` → `ui_preview.exe [en]` | **不需要 VB Decompiler** 即可渲染真实设置窗体，并截图到 `%TEMP%\vbmcp_ui_page*.bmp`（含"放大窗体后重排"的效果），同时把页窗口/页内控件坐标与消息转发结果写进 `%TEMP%\vbmcp_ui_probe.txt`，改 UI 后用它做回归对比 |
| `python tools/gen_ids_header.py` / `--check` | 由 `res/vbmcp.rc` 的 `#define` 生成 `src/vbmcp_ids.h` / 校验两者一致（头文件是生成产物，不要手改） |
| `powershell -File tools\\verify_i18n_resource.ps1` | 用 32 位宿主验证 `JSON_ZH` 已嵌入 DLL（必须 32 位宿主：64 位 PowerShell 加载 32 位 DLL 会报 193） |
| `powershell -File %USERPROFILE%\\.codebuddy\\skills\\vs2019-rc-editor\\scripts\\check_rc_health.ps1 -RcPath res\\vbmcp.rc -ProjectFile vbmcp.vcxproj` | 资源脚本体检：rc 是否被资源编辑器接管 / 是否缺 `windows.h` / ID 是否同步 / 包装目标是否在位 / rc.exe 工具链自检（有 PROBLEM 时返回非 0）。通用规则见该 skill |

## 6. 新增 HTTP 端点的注意事项

- 端点写在 `mcp_http.cpp: handle_request()`，且只能使用传输层能力：
  不得调用引擎、不得弹窗；
- 响应体若为 JSON，一律用 yyjson 构造（`json_error_document()` 等）；
- 错误必须区分 HTTP 状态码（传输语义）与 JSON-RPC `error.code`（协议语义）；
- 任何解析都要防御超长/畸形输入（参考 `parse_request()` 的长度与数字校验）；
- CORS 仅在 `Origin` 命中 `CorsOrigins` 白名单时输出，禁止 `*`。

**客户端兼容性（实测约定，改动前先读）**：

- **协议版本**：支持集合 = { `2025-06-18`, `2025-03-26`, `2024-11-05` }
  （`vbmcp.h` 的 `VBMCP_PROTOCOL_*`）。两处必须一致——
  `mcp_http.cpp: protocol_version_supported()`（请求头校验）与
  `mcp_rpc.cpp: negotiate_protocol_version()`（initialize 协商）。
  客户端会把协商到的版本放进后续请求的 `MCP-Protocol-Version` 头，
  因此头校验漏掉某个版本会直接表现为"客户端无法连接"；
- **Accept**：POST 命中 `application/json` 或 `text/event-stream` 之一即放行
  （响应体始终是 JSON）；不要恢复成"必须含 application/json"，
  否则只声明 SSE 的客户端会被 406 拒绝；
- **传输编码**：**不支持** `Transfer-Encoding: chunked`（明确回 400）。
  MCP 客户端均使用 `Content-Length`；如需支持 chunked，要在
  `parse_request()` 里实现分块解码并补自测；
- **排查"连不上"**：`VBMCP_DEBUG_LOG=1` 后：
  1. 日志里**有没有** `client connected` / `http POST /mcp ...` 行——
     没有 → 客户端根本没连上（服务未启用 / 端口不符 / 被别的进程占用，
     先 `curl http://127.0.0.1:3023/health` 确认）；
  2. 有请求行但立刻跟着 `transport error http=4xx`——看该行的
     accept / proto 字段与错误 message（本节的兼容性约定就是为此而设）；
  3. 请求行正常但没有 `tools/call` 后续——通常是 initialize 响应未被接受
     （对照 `initialize` 响应里的 `protocolVersion` 与 capabilities）。

---

## 7. 错误码约定（`vbmcp.h`）

| 常量 | 值 | 用途 |
|---|---|---|
| `VBMCP_ERR_OK` | 0 | 成功 |
| `VBMCP_ERR_PARSE` | -32700 | JSON 解析失败 |
| `VBMCP_ERR_INVALID_REQUEST` | -32600 | 非 JSON-RPC 结构 / 批量请求 |
| `VBMCP_ERR_METHOD_NOT_FOUND` | -32601 | 未知方法 |
| `VBMCP_ERR_INVALID_PARAMS` | -32602 | 参数非法、未知工具 |
| `VBMCP_ERR_INTERNAL` | -32603 | 内部错误、OOM、引擎异常 |
| `VBMCP_ERR_PERMISSION` | -32001 | 会话未授权 / 只读模式下调用 `write_*` |
| `VBMCP_ERR_TIMEOUT` | -32002 | 等待 UI 主线程超时 |
| `VBMCP_ERR_BUSY` | -32003 | UI 任务队列已满 |
| `VBMCP_ERR_BUSINESS` | -32020 | 业务失败（如项目未加载）→ 走 `isError:true` 内容通道 |

区分原则：**权限 / 参数 / 超时 / 内部错误**走 JSON-RPC `error`；
**业务失败**（项目没打开、native 项目不能改名等）走 `tools/call` 的
`result.isError = true` + 文本内容，不占用错误通道。

---

## 8. 关键常量（`vbmcp.h`）

| 常量 | 默认 | 含义 |
|---|---|---|
| `VBMCP_DEFAULT_BIND` | `127.0.0.1` | 默认监听地址（禁止改为 0.0.0.0） |
| `VBMCP_DEFAULT_PORT` | 3023 | 默认端口 |
| `VBMCP_DEFAULT_ALLOW_WRITE` | 0 | 默认只读 |
| `VBMCP_MAX_HEADER_SIZE` | 16KB | 单请求头部长度上限 |
| `VBMCP_MAX_BODY_SIZE` | 64MB | 请求体上限 |
| `VBMCP_MAX_PENDING_TASKS` | 8 | UI 任务队列上限（并发保护） |
| `VBMCP_TASK_TIMEOUT_MS` | 30000 | 单任务超时（`VBMCP_TASK_TIMEOUT_MS` 可覆盖） |
| `VBMCP_MAX_PROJECT_TEXT` | 16MB | `vb_write_project_text` 单次写入上限 |
| `VBMCP_MAX_CONNECTIONS` | 64 | 并发连接上限 |
| `kMaxSendBufferBytes`（`mcp_http.cpp`） | 4MB | SSE 单连接待发缓冲上限（超限断开该连接） |
| `kLogMaxBytes`（`util.cpp`） | 10MB | 日志文件上限（超限轮转 `.old`，只保留一代） |

---

## 9. 构建与提交前检查

```cmd
build.bat
:: 或
msbuild vbmcp.sln /p:Configuration=Release /p:Platform=Win32 /t:Rebuild /nologo /v:minimal
```

提交前检查清单：

- [ ] Release 构建 **0 error / 0 warning**
- [ ] 新增工具已按 `read_*` / `write_*` 命名，并在 `is_write` 中正确标注
- [ ] 只读模式 `tools/list` 只返回 `vb_read_*`；强行调用 `write_*` 返回 `-32001`
- [ ] 未在网络线程新增任何 UI / 引擎调用
- [ ] 未引入任何手写 JSON 序列化（全部 yyjson）
- [ ] README / README_EN / 本文档的工具清单已同步
- [ ] 日志未打印项目文本或敏感内容
- [ ] 新增/修改展示字符串已用 `_()`（ANSI 控件用 `_A()`）包裹，且已运行
      `python tools/extract_i18n.py`（`--check` 无 missing）
- [ ] 工具名 / JSON 键名 / 注册表键值未被 `_()` 包裹
- [ ] `tools\check_all.bat` 全绿（ID 一致 + 词条一致 + 自测 + Release 构建）
- [ ] 新增文本字段已按"数据编码"小节处理（读 `add_text*` / 写 `set_text_value`；
      含自由文本的结果调用 `add_encoding_field`）