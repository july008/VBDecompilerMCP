# 改造进度记录（MCP 规范对齐 + C++ 迁移）

> 本文档记录「对齐最新 MCP 官方规范（Streamable HTTP / SSE）+ 项目改造总要求」的
> 逐项完成情况。每完成一项即在此勾选并记录实现位置，便于回溯与验收。
>
> 最后更新：2026-10-14

---

## 0. 总览

| 阶段 | 内容 | 状态 |
|---|---|---|
| A | 修复上一次改造遗留的编译失败（options_dialog 与新 config API 不同步） | ✅ 完成 |
| B | C → C++ 迁移（C++17 / RAII / 原子变量 / 异常保护） | ✅ 完成 |
| C | JSON 统一到 yyjson（删除全部手写 JSON 编解码） | ✅ 完成 |
| D | 网络层重构：Streamable HTTP + SSE + 后台网络线程 + UI 任务队列 | ✅ 完成 |
| E | 权限模型：read_/write_ 工具分组 + 动态 listTools + callTool 二次校验 | ✅ 完成 |
| F | 安全模型：默认 127.0.0.1、CORS 白名单、首次 callTool 授权弹窗 | ✅ 完成 |
| G | 错误处理与稳定性：错误分类、超时/取消、限流、体积上限、分级日志 | ✅ 完成 |
| H | 文档：README / README_EN / SECURITY.md / 开发文档 | ✅ 完成 |
| I | 设置窗体 UI 重构：双属性页（服务设置 / 接入信息） | ✅ 完成 |
| K | 设置窗体 UI 打磨：字号 12pt+DPI、说明文字透明背景、窗体可缩放与自适应布局 | ✅ 完成 |
| J | i18n 改造：删除 `T_*` 双语数组，改为 `_()` + `lang_res/zh.json` + DLL 内嵌资源 | ✅ 完成 |

编译状态：`msbuild vbmcp.sln /p:Configuration=Release /p:Platform=Win32`
→ **0 error / 0 warning**，产物 `Win32\Release\vbmcp.dll`。

---

## 一、网络层 & MCP Transport

| # | 要求 | 状态 | 实现位置 / 说明 |
|---|---|---|---|
| 1.1 | 默认固定监听 `127.0.0.1`，端口可配置，**绝不默认 0.0.0.0** | ✅ | `vbmcp.h: VBMCP_DEFAULT_BIND/VBMCP_LOOPBACK_BIND`；`mcp_http.cpp: create_listen_socket()` 只用 `vbmcp_bind_address()` 快照 |
| 1.2 | 开放局域网需手动修改 + 高危警告，且选项默认隐藏 | ✅ | `options_dialog.cpp`：`IDC_CHK_LAN_ADVANCED` 默认未勾选、绑定框 `EnableWindow(FALSE)`；勾选/确定时 `confirm_open_bind_warning()` 弹高危警告，用户拒绝则强制回环 |
| 2.1 | 移除旧版一次性 HTTP，实现 MCP Streamable HTTP（SSE） | ✅ | `mcp_http.cpp`：`POST /mcp`（JSON-RPC）、`GET /mcp`（SSE 长连接）、`DELETE /mcp`（终止会话）；`/health` 用 yyjson 构造；旧的 `/debug/*` 回显端点已移除 |
| 2.2 | SSE 服务端推送 + 心跳保活 | ✅ | `sse_begin()` / `sse_push_json()` / `sse_heartbeat_all()`（15s `: ping` 注释帧）；权限变化时主动推送 `notifications/tools/list_changed` |
| 2.3 | CORS 禁止 `*`，默认不返回 CORS 头 | ✅ | `origin_is_trusted()`：注册表 `CorsOrigins` 为空 → 一个 CORS 头都不发；命中白名单才回显该 origin；通配 `*` 被显式忽略 |
| 3.1 | `listTools` 等预检**不弹窗** | ✅ | 仅 `handle_tools_call()` 内调用 `vbmcp_session_authorize()`；`initialize/ping/tools/list` 直接放行 |
| 3.2 | 第一条 `callTool` 弹窗展示客户端连接信息 | ✅ | `mcp_rpc.cpp: vbmcp_session_authorize()`，`MessageBoxA` 展示 `client_info`（如 `127.0.0.1:52341`），在 **UI 主线程** 弹出 |
| 3.3 | 允许本次会话 / 拒绝；拒绝后拒绝所有后续请求 | ✅ | `g_session_authorized` / `g_session_denied` 原子标志；拒绝后所有 `tools/call` 返回 `-32001` 权限错误 |
| 3.4 | 服务重启后授权重置，下次新连接再次弹窗 | ✅ | `vbmcp_server_stop()` → `vbmcp_session_reset()`；另外「所有客户端断开」也重置（`g_session_was_used`） |
| 3.5 | 准入授权不替代读写开关 | ✅ | 准入只放行会话；`g_allow_write` 独立控制 `write_*` 工具可见性与可调用性 |

**Streamable HTTP 细节实现**

- `initialize` 响应附带 `Mcp-Session-Id`（GUID）；后续请求带该头且不匹配 → `404` + 标准错误。
- `GET` 的 `Accept` 必须含 `text/event-stream`，否则 `406`。
- `POST` 的 `Accept` 必须含 `application/json`（否则 `406`），`Content-Type` 非 JSON → `415`。
- `MCP-Protocol-Version` 头非法 → `400`（兼容 `2025-03-26` 与 `2024-11-05`）。
- JSON-RPC 批量（数组）请求明确回绝 `400`（本实现单请求单响应）。
- notification（无 `id`）→ `202 Accepted` 空 body。

---

## 二、工具权限模型（只读 / 写入分离）

| # | 要求 | 状态 | 实现位置 / 说明 |
|---|---|---|---|
| 1 | 设置窗体新增「允许 MCP 客户端写入项目数据」，默认未勾选 | ✅ | `options_dialog.cpp: IDC_CHK_ALLOW_WRITE`；`VBMCP_DEFAULT_ALLOW_WRITE=0`；持久化到注册表 `AllowWrite` |
| 2 | 工具严格分 `vb_read_*` / `vb_write_*` 两组 | ✅ | `mcp_tools.cpp: g_tools[]`（13 只读 + 15 写入 = 28）；命名规范见 `docs/DEVELOPMENT.md` |
| 3 | `listTools` 按权限动态生成清单 | ✅ | `mcp_build_tools_into(doc, arr, allow_write)`：只读模式只输出 `vb_read_*` |
| 4 | `callTool` 入口二次权限校验（防绕过） | ✅ | 三道闸：① `mcp_rpc.cpp` `handle_tools_call()` 显式校验；② `mcp_tools.cpp: mcp_dispatch_tool()` 校验；③ 写入工具实现只在 UI 线程执行 |
| 5 | 权限变化自动通知客户端 | ✅ | 设置窗体确定且写入权限变化 → `vbmcp_notify_tools_list_changed()` → SSE 广播 `notifications/tools/list_changed` |
| 6 | 权限模式状态可见 | ✅ | 设置窗体「服务设置」页的「权限模式」字段显示「只读」/「读写」 |

---

## 三、代码语言与 JSON 库重构

| # | 要求 | 状态 | 实现位置 / 说明 |
|---|---|---|---|
| 1 | 项目代码从 C 迁移为 C++，保持业务逻辑，做好内存安全 | ✅ | `src/*.c` → `src/*.cpp`（git mv 保留历史）；工程改为 `stdcpp17` + `/EHa`；新增 `src/vbmcp_mem.hpp`（`SocketHandle`/`unique_ptr`/`ScopeGuard` RAII） |
| 2 | 删除全部自研 JSON 编解码，完整替换为 yyjson | ✅ | `third_party/yyjson` 以 `CompileAsC` 参与编译；`src/json.c/.h` 已删除；`mcp_http.cpp` 的 `/health`、错误文档全部走 `yyjson_mut_*` |
| 2b | 说明：需求文档第三节写 "RapidJSON"，第七节硬性约束写 "yyjson（源码在 third_party\yyjson）" | ✅ 按第七节执行 | 采用 **yyjson**（仓库内已内置，零新增依赖），并已删除全部手写 JSON 代码 |
| 3 | 所有 MCP 请求入口 try-catch，单请求失败不崩 UI | ✅ | 双层保护：`mcp_http.cpp: task_pump_on_ui_thread()`（`catch (const std::exception&) / catch(...)`）+ `mcp_rpc.cpp: vbmcp_process_jsonrpc()`；引擎调用另保留原有 SEH `__try` 保护 |
| 4.1 | 服务运行在后台独立线程，禁止后台线程操作 UI | ✅ | `mcp_http.cpp: network_thread_main()` 仅做 socket/HTTP；无任何 UI/引擎调用 |
| 4.2 | 需要 UI/工程数据的请求经任务队列投递到 UI 主线程 | ✅ | `UiTask` + 隐藏消息窗口 `VBMCP_TASK_V1`（`VBMCP_WM_TASK`）+ `task_pump_on_ui_thread()` |
| 4.3 | 支持任务超时、取消令牌 | ✅ | 超时 `VBMCP_TASK_TIMEOUT_MS`（默认 30s，可用环境变量覆盖）；`cancelled` 原子令牌在「客户端断开 / 超时 / 服务停止」时置位，未执行任务直接丢弃 |
| 4.4 | 限制并发任务数量，防止 UI 卡死/竞态 | ✅ | `VBMCP_MAX_PENDING_TASKS=8`，队列满立即 `429` + `-32003` 繁忙错误 |
| 4.5 | 跨线程共享状态安全 | ✅ | `g_enabled/g_port/g_allow_write` 改 `std::atomic<int>`；绑定地址走互斥量 + `vbmcp_bind_address()`；日志写文件用临界区串行化 |

---

## 四、错误处理与稳定性

| # | 要求 | 状态 | 实现位置 / 说明 |
|---|---|---|---|
| 1 | callTool 返回 MCP 标准错误对象，区分权限/参数/超时/内部错误 | ✅ | `vbmcp.h: VBMCP_ERR_PERMISSION(-32001) / TIMEOUT(-32002) / BUSY(-32003) / INTERNAL(-32603) / INVALID_PARAMS(-32602)`；业务错误走 `isError:true` 内容通道 |
| 2 | transport 异常与 MCP 应用层错误分离 | ✅ | `http_send_error()`：HTTP 状态码（400/404/405/406/413/415/429）表达传输语义，body 内 `{code,message}` 表达协议语义 |
| 3 | SSE 连接断开时终止后台任务、释放资源 | ✅ | `client_gone()` 探测 FIN；断开即 `cancelled=true` + 清空 `send_buf`；`SocketHandle` RAII 自动 `closesocket`；`UiTask` 用 `shared_ptr` 生命周期托管 |
| 4 | 最大请求体限制 + 基础限流 + 防畸形/超大 payload | ✅ | `VBMCP_MAX_BODY_SIZE`(64MB) + `VBMCP_MAX_HEADER_SIZE`(16KB) + `Content-Length` 数字校验 + 全局 200 req/s 限流 + 单连接上限 64 |
| 5 | 分级日志，不泄露源码敏感信息 | ✅ | `VBMCP_LOG_ERROR/WARN/INFO/DEBUG`；仅记录连接、工具名、权限判定、错误分类与耗时，**不记录任何项目文本/源码内容**；`VBMCP_DEBUG_LOG=1` 开启，`VBMCP_LOG_LEVEL` 控阈值 |

---

## 五、文档与仓库配套

| # | 要求 | 状态 | 产物 |
|---|---|---|---|
| 1 | README 说明 Streamable HTTP、默认 127.0.0.1、首次弹窗、读写开关 | ✅ | `README.md` / `README_EN.md` 已重写相关章节 |
| 2 | SECURITY.md 写明风险与权限模型 | ✅ | 新增 `SECURITY.md` |
| 3 | 开发文档说明工具命名规范 | ✅ | 新增 `docs/DEVELOPMENT.md`（命名规范 / 架构 / 新增工具流程 / 错误码表） |
| 4 | 改造进度记录 | ✅ | 本文档 `docs/REFACTOR_PROGRESS.md` |

---

## 五之二、设置窗体 UI 重构（双属性页）

参考同类 MCP 服务器的设置界面，把原来"一屏堆放"的窗体拆成 `SysTabControl32`
双属性页，窗体尺寸 425x510 → 620x548：

| 属性页 | 控件 | 说明 |
|---|---|---|
| **服务设置** | 分组框「服务」 | 启用 MCP 服务器 / 绑定 IP 地址 / 端口号（含微调按钮）/ 服务器状态 / 权限模式 / 局域网访问（高级） |
| | 分组框「权限与安全」 | 允许 MCP 客户端写入项目数据 + 安全说明（只读默认、LAN 风险、AI 修改工程风险）+ 生效提示 |
| **接入信息** | 客户端软件下拉 | 8 个 AI 客户端（CodeBuddy / VS Code / Cursor / Trae / WorkBuddy / Claude Desktop / Claude Code / OpenAI Codex），随界面语言切换 |
| | 写入路径 | 该客户端的 MCP 配置文件位置（`%APPDATA%` 等变量已展开为实际路径），只读 + 「复制路径」 |
| | 连接地址 | `http://<bind>:<port>`（基地址），只读 + 「复制地址」 |
| | 配置片段 | 可直接粘贴的 JSON / TOML（`url` 已填 MCP 端点 `.../mcp`），只读多行 + 滚动条 + 「复制配置」 |
| | 接入说明 | 该客户端的步骤与注意事项，只读多行 + 滚动条 |
| 底部（两页共用） | 恢复默认设置 / 帮助 / 确定 / 取消 | 逻辑与改造前完全一致 |

实现要点：

- 所有控件仍是对话框的**直接子控件**（`GetDlgItem` / `SetDlgItemTextA` 可用），
  切换属性页时按 ID 列表批量 `ShowWindow`，不引入父子层级查找。
- 文本统一经 `set_text_utf8()`：UTF-8 → GBK（ANSI 窗口）+ `\n` → `\r\n`（多行 EDIT）。
- 输入框背景改为 `COLOR_WINDOW`（白底），标签/复选框/分组框保持 `COLOR_BTNFACE`，
  视觉上区分"可编辑区"与"说明区"。
- 端口变化 / 客户端切换 → `update_access_page()` 实时刷新地址、路径、片段、说明。
- 复制按钮统一走 `copy_from_control()` + `flash_button()`（1.5 秒"已复制"反馈）。
- 兼容性修复：该 SDK 未定义 `TCS_TOP`（`TCS_TABS` 即默认值），初始化类名是
  `ICC_TAB_CLASSES`，`TCM_INSERTITEM` 使用本地 ANSI 结构（避免依赖 SDK 的
  `TCITEM`/`TCITAMA` TCHAR 映射宏在不同 SDK 版本下可见性不一致）。

## 五之三、i18n 国际化改造（双语数组 → 语言包 + 资源嵌入）

### 动因与结果

| 项 | 改造前 | 改造后 |
|---|---|---|
| 词条载体 | `lang.h` 的 `T_*` 枚举 + `lang.cpp: g_strings[T_COUNT]` 双语数组（107 条） | `lang_res/zh.json`（104 条 msgid→msgstr），随源码提交 |
| 取串API | `vbmcp_t(T_X)`（GBK）/ `vbmcp_t_utf8(T_X)`（UTF-8） | `_()`（UTF-8）/ `_A()`（GBK）/ `_F()` / `_AF()` / `N_()` |
| 加载方式 | 进程内静态表 | `RCDATA("JSON_ZH")` **嵌入 vbmcp.dll** → `GetModuleHandleExA(FROM_ADDRESS)` 取本模块句柄 → `FindResourceA`/`LoadResource` → yyjson 解析 → khash 查表 |
| 外部依赖 | 无 | **无外部 langpack.dll**（资源就在本 DLL 内） |
| 内存管理 | `g_gbk[][]` 缓存（进程退出才回收） | `I18n_Clear()` 释放表内 strdup 的 key/value + yyjson 文档；`DllMain(DLL_PROCESS_DETACH)` 调 `I18n_PrepareShutdown()`（先关闭查表再释放） |
| 词条同步 | 手工维护 C 数组 | `python tools/extract_i18n.py` 扫描 `_()/N_()/_A()/_F()/_AF()`，合并语言包 + 生成待翻译清单 + `--check` CI 校验 + `--prune` 清理失效词条 |
| 语言来源 | 注册表 `CustomLanguage` | 同左（`i18n.cpp: I18n_Refresh()`，设置窗体每次打开前重读） |

### 关键设计决定

1. **msgid = 英文原文**：源码里 `_("Enable MCP Server")` 直接可读，翻译集中在 JSON，
   新增语言不影响代码。
2. **`_()` 给 UTF-8、`_A()` 给 GBK**：MCP JSON 报文必须是 UTF-8，而 Win32 A 系列接口
   （`MessageBoxA`/`SetDlgItemTextA`/`CreateWindowExA`）只能吃 ANSI 字节，因此提供两个
   宏而不是让 `_()` 隐式转换（否则 JSON 里会出现 GBK 乱码）。
3. **轮转缓冲池**（16 槽 × 4KB）：`_A()`/`_F()`/`_AF()` 的结果放在池子里，
   语义是"立即消费"（控件 API 会立刻拷贝），避免嵌套调用互相覆盖，也避免每次
   都 malloc。
4. **未翻译词条回退英文原文**（而不是空串），并由 `I18n_GetText()` 统一兜底。
5. **工具名不翻译**：`vb_read_*` / `vb_write_*` 是 MCP 协议标识符，
   `g_tools[]` 的 `name` 字段保持字面量，只有 `desc` 走 `_()`。
6. **khash 子集**（`src/khash.h`）：只需"字符串键 → 指针值"，实现 FNV-1a + 开放寻址，
   约 200 行；与上游 khash 语义一致（不拥有 key/value 内存，`kh_put` 的 `ret`
   表示 0/1/2）。

### 验证

| 验证项 | 结果 |
|---|---|
| Release 构建 | 0 error / 0 warning |
| `tools/verify_i18n_resource.ps1`（32 位宿主 LoadLibrary + FindResourceA） | `JSON_ZH` 资源存在，14881 字节，内容为 UTF-8 JSON；FreeLibrary 正常 |
| `tools/i18n_selftest`（20 项断言） | ALL PASS：EN 回退 / ZH 加载 / 查表 / 未收录回退 / `_F` 参数替换 / `_A` GBK 输出 / `_AF` / `I18n_Clear` |
| `python tools/extract_i18n.py --check` | 104 词条，新增 0 / 待翻译 0 / 失效 0 |
| `_F()`/`_AF()` 占位符支持与两道安全网 | 支持 `%s %d %zu %.2f %%`；自测覆盖「实参个数核对」与「译文占位符核对」，并因此修掉 2 个真实缺陷（漏参 → 访问冲突；安全退化路径仍走 `vsnprintf`） |
| `%zu` / `%s` 类词条审计 | 10 条含占位符的词条全部经 `_F`/`_AF`/snprintf 使用，无字面量 `%` 泄漏 |

## 五之四、设置窗体 UI 打磨（阶段 K）

用户反馈三个问题，逐项修复并**用 `tools/ui_preview` 截图验证**（无需 VB Decompiler）：

| 问题 | 修法 |
|---|---|
| 字体偏小 | 字号由 `-14px(≈10.5pt)` 提到 **12pt**，且随系统 DPI 缩放（`GetDpiForWindow` + `S()`） |
| 文字有背景色 | 安全说明/生效提示由白底 `EDIT` 改为 `STATIC`（`WM_CTLCOLORSTATIC` 透明），只有真正的"字段框/内容框"保留白底 |
| 窗体太小、不能调 | 默认客户区 425×510 → **720×636**；加 `WS_THICKFRAME` + `WS_MAXIMIZEBOX` 可拖拽缩放；`WM_GETMINMAXINFO` 限制最小 640×560 |

顺带修掉的布局缺陷（截图暴露）：

1. `colX` 未计入页面内边距 → 标签文字被输入框盖住（"绑定 IP 地址:" 只剩"绑定 IP 地"）；
2. 启用勾选框宽度过大 → 压住右侧"服务器状态:"；
3. 右侧标签列 120px 不够 → 英文 "Permission mode:" 被裁成 "ermission mode:"；
4. 分组框标题含 `&` 被 BS_GROUPBOX 当助记符吞掉 → 改为 "Permissions and security"；
5. 两段说明按 2/3 比例分配导致中间大片空白 → 改为按内容固定高度。

新增 `VBMCP_LANG=zh|en` 环境变量（优先于注册表），可在不修改 VB Decompiler 语言设置的前提下验证双语界面。

### 阶段 K 补记：150% 缩放下的两个问题

| 问题 | 根因 | 修法 |
|---|---|---|
| 窗口显得太大 | 宿主不是 DPI aware 时 Windows 会整体位图放大；设计高度 636 在 150% 下≈整屏高度 | 设计尺寸降到 `720x560`（两页都还有富余）；接入信息页行距 40→36、配置片段框 160→140，省下的高度给"接入说明"；`size_window_to_client()` 按工作区 0.92/0.88 钳制，`WM_GETMINMAXINFO` 补 `ptMaxTrackSize` |
| 标签/说明文字有一块块底色 | 硬编码 `GetSysColorBrush(COLOR_BTNFACE)`，而宿主启用视觉样式后对话框底色由主题决定（偏白），两者不一致 | 直接指定统一背景色 `kUiBgColorIndex`（默认 `COLOR_WINDOW`）：shell 与两个属性页在 `WM_ERASEBKGND` 里自己 `FillRect`，所有 `WM_CTLCOLOR*` 返回同一把自建实色刷，标签用 `TRANSPARENT`；输入框同色 `OPAQUE` |
| 150% 下窗口占满整屏 | 设计高度 636（96dpi 逻辑像素）在 DPI aware 宿主里 ×1.5 ≈ 整屏 | 设计尺寸降到 `720x560`；接入信息页行距 40→36、配置片段框 160→140；`size_window_to_client()` 按工作区 0.92/0.88 钳制，`WM_GETMINMAXINFO` 补 `ptMaxTrackSize` |
| 「权限模式」与「允许写入」语义分散 | 权限模式原本在"服务"分组第三行右侧 | 移到"权限与安全"分组第一行右侧，与「允许 MCP 客户端写入项目数据」同行对齐，勾选框宽度收窄让出右侧列 |


---

## 五之五、设置窗体改为资源模板（阶段 L）

设置窗体原来是纯代码创建窗口（`create_child_controls()` + 自定义 WndProc），
先改成 `DialogBoxParamA` + `res/vbmcp.rc` 的 `DIALOGEX` 模板；随后按"资源编辑器里
控件应按属性页分组"的要求，进一步拆成**一个属性页一个模板**。

| 项 | 之前 | 现在 |
|---|---|---|
| 控件树 | 代码 `CreateWindowExA` | `res/vbmcp.rc` 三个 `DIALOGEX`：`IDD_VBMCP_OPTIONS`(2000, shell) / `IDD_PAGE_SERVER`(2001) / `IDD_PAGE_ACCESS`(2002) |
| 窗体窗口过程 | 自定义 `WndProc` + 手写模态循环 | `DialogBoxParamA` + 对话框过程 `dlg_proc()`（父窗自动禁用、`EndDialog` 收尾） |
| 控件寻址 | `GetDlgItem(hDlg, id)` | `find_ctl(id)` / `set_text` / `get_check` / `send_ctl`（跨三个窗口按 ID 寻址） |
| 属性页切换 | 按 ID 数组批量 `ShowWindow` | `apply_tab()` 显示/隐藏两个页窗口 |
| 页内消息 | 控件父窗就是对话框 | `page_dlg_proc()` 转发 `WM_COMMAND`/`WM_NOTIFY` 给 shell，并就地处理 `WM_CTLCOLOR*` |
| ID 单一来源 | 散落各处字面量 | `res/vbmcp.rc` 顶部 `#define` → `tools/gen_ids_header.py` 生成 `src/vbmcp_ids.h`（`--check` 校验） |

**为什么必须一页一个模板**：rc 的对话框模板是扁平的，一个模板内所有控件都是兄弟，
表达不了"属于哪个属性页"，资源编辑器自然无法分组。改成每页一个模板后，
运行期把页窗口（`CreateDialogParamA` 建的**子窗口**对话框）停靠在 tab 显示区。

实现中踩到的三个坑（均已写入 `docs/DEVELOPMENT.md` 的踩坑表）：

1. `CreateWindowExA("#32770", 模板ID, …)` 在 32 位 USER32 上**必定访问冲突**，
   即使 `FindResourceA` 能查到模板；建子窗口对话框必须用 `CreateDialogParamA`。
2. Win32 版 `SysTabControl32` **不实现** `TCM_GETCLIENTRECT`（返回 0），
   页窗口停靠位置只能用固定内缩（顶部让开 `S(26)` 标签条）推算。
3. tab 显示区坐标是 **tab 客户区**坐标，要放到 shell 客户区必须换算两次
   （`ClientToScreen` 再 `ScreenToClient`）；只换算一次会把页窗口丢到屏幕左上角。

验证方式：`tools/ui_preview` 除截图外，新增把页窗口/页内控件坐标、两个页是否独立、
`WM_COMMAND` 转发结果写进 `%TEMP%\vbmcp_ui_probe.txt`；中英文 × 默认/放大四张截图
逐张核对。


---

## 六、已知限制与后续建议

1. **单请求串行执行**：为规避 VB Decompiler Callback Engine 的线程安全问题，UI 侧任务队列
   串行执行（并发上限 8，超出即拒）。若未来 VB Decompiler 提供线程安全的引擎快照 API，
   可改为多 worker 并发。
2. **取消的边界**：已进入引擎调用的任务无法中断（引擎是同步阻塞的），取消令牌只在
   「排队中」和「结果回写前」生效；超时后引擎侧调用仍会跑完，但响应被丢弃。
3. **会话粒度**：当前会话 id 按连接分配，MCP 规范允许跨连接复用同一会话；若客户端在
   SSE 长连接上复用会话，需要后续支持「MCP-Session-Id → 连接」的映射表。
4. **批量请求**：按规范允许服务端拒绝，本实现显式返回 `400`，不做批量合并。
5. ~~`src/i18n.c` 未完成的历史实验~~ → **已解决（阶段 J）**：`src/i18n.cpp` +
   `src/khash.h` 正式实现并参与编译，`lang_res/zh.json` 以 `RCDATA("JSON_ZH")`
   嵌入 `vbmcp.dll`，`lang.cpp/lang.h` 的 `T_*` 双语数组已删除。
6. **`strbuf.cpp`**：仅用于非 JSON 的文本拼装（HTTP 头、GBK 转换），
   所有 JSON 报文已改由 yyjson 生成，保留该模块是为了不再引入第二套字符串抽象。

---

## 七、验收清单（手工回归）

由于插件依赖 VB Decompiler 主进程，以下项目需在装有 VB Decompiler 的机器上实测：

- [ ] 默认配置启动：`/health` 返回 `readOnly:true`、`tools_count=13`
- [ ] `tools/list` 只返回 13 个 `vb_read_*`
- [ ] 设置窗体「服务设置」页勾选「允许写入」→ 「权限模式」显示读写 → 切到「接入信息」页后 `tools/list` 返回 28 个工具
- [ ] 首个 `tools/call` 弹窗：选择「是」后可调用，`选择「否」后所有 callTool 返回 `-32001`
- [ ] 只读模式下强行调用 `vb_write_project_text` → 返回 `-32001`，项目内容未变
- [ ] 属性页切换正常：两页内容互不遮挡，底部按钮在两页都可用
- [ ] 资源编辑器里 `IDD_PAGE_SERVER` / `IDD_PAGE_ACCESS` 各自只含本页控件（无跨页混排）
- [ ] 页内控件交互正常：勾选框/下拉框/端口输入框在对应页内即生效（消息转发链）
- [ ] 接入信息页：切换客户端 → 写入路径 / 连接地址 / 配置片段 / 接入说明 同步刷新
- [ ] 接入信息页：改端口 → 连接地址与配置片段中的 url 同步更新；三个复制按钮均能写入剪贴板
- [ ] 勾选「允许局域网访问」→ 弹高危警告；选择「否」则回到 `127.0.0.1`
- [ ] `GET /mcp`（`Accept: text/event-stream`）可建立 SSE，15s 收到 `: ping`
- [ ] 切换写入权限时，已连接的 SSE 客户端收到 `notifications/tools/list_changed`
- [ ] `DELETE /mcp` 后再带旧 `Mcp-Session-Id` 请求 → `404`
- [ ] 连续并发 10 个大工具调用 → 多余请求返回 `429` + `-32003`，UI 不卡死
- [ ] 单请求超时（`VBMCP_TASK_TIMEOUT_MS=2000` + 大项目读取）→ 返回 `-32002`
- [ ] 关闭 MCP 客户端（不关服务）→ 日志出现 `task cancelled`，无残留任务
- [ ] 发送 100MB body → `413`；发送畸形 HTTP 头 → `400`