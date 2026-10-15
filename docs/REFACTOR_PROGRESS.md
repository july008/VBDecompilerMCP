# 改造进度记录（MCP 规范对齐 + C++ 迁移）

> 本文档记录「对齐最新 MCP 官方规范（Streamable HTTP / SSE）+ 项目改造总要求」的
> 逐项完成情况。每完成一项即在此勾选并记录实现位置，便于回溯与验收。
>
> 最后更新：2026-10-15

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
| M | 优化批次（P0–P4）：工程卫生 / 协议一致性 / 健壮性 / UTF-8 编码桥 / 测试工具 | ✅ 完成（详见第八节） |

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
- `MCP-Protocol-Version` 头非法 → `400`（支持集合 `2025-06-18` / `2025-03-26` / `2024-11-05`，见第八节 8.5）。
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
| 词条载体 | `lang.h` 的 `T_*` 枚举 + `lang.cpp: g_strings[T_COUNT]` 双语数组（107 条） | `lang_res/zh.json`（msgid→msgstr，当前 98 条），随源码提交 |
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
| `tools/verify_i18n_resource.ps1`（32 位宿主 LoadLibrary + FindResourceA） | `JSON_ZH` 资源存在（阶段 J 时 14881 字节，随后续词条增删变化），内容为 UTF-8 JSON；FreeLibrary 正常 |
| `tools/i18n_selftest`（20 项断言，阶段 J 时存在；阶段 M 起由 `tools/selftest` 承接，见第八节） | ALL PASS：EN 回退 / ZH 加载 / 查表 / 未收录回退 / `_F` 参数替换 / `_A` GBK 输出 / `_AF` / `I18n_Clear` |
| `python tools/extract_i18n.py --check` | 98 词条（阶段 J 时为 104），新增 0 / 待翻译 0 / 失效 0 |
| `_F()`/`_AF()` 占位符支持与两道安全网 | 支持 `%s %d %zu %.2f %%`；自测覆盖「实参个数核对」与「译文占位符核对」，并因此修掉 2 个真实缺陷（漏参 → 访问冲突；安全退化路径仍走 `vsnprintf`） |
| `%zu` / `%s` 类词条审计 | 10 条含占位符的词条全部经 `_F`/`_AF`/snprintf 使用，无字面量 `%` 泄漏 |

## 五之四、设置窗体 UI 打磨（阶段 K）

用户反馈三个问题，逐项修复并**用 `tools/ui_preview` 截图验证**（无需 VB Decompiler）：

| 问题 | 修法 |
|---|---|
| 字体偏小 | 字号由 `-14px(≈10.5pt)` 提到 **12pt**，且随系统 DPI 缩放（`GetDpiForWindow` + `S()`） |
| 文字有背景色 | 安全说明/生效提示由白底 `EDIT` 改为 `STATIC`（`WM_CTLCOLORSTATIC` 透明），只有真正的"字段框/内容框"保留白底 |
| 窗体太小、不能调 | 默认客户区 425×510 → **720×636**；可拖拽缩放 + `WM_GETMINMAXINFO` 限制最小 640×560（`WS_THICKFRAME` 写进 rc 后会被资源编辑器保存时丢掉，阶段 M 改为运行期补样式，见第八节） |

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
3. **会话粒度**：会话 id 按连接分配，且只在"本连接已持有 id"时校验（新连接携带
   旧 id 不会 404，也不会关联旧会话）——按宽容策略处理规范允许的恢复场景，
   README 的表述已同步为此语义。若要严格支持「MCP-Session-Id → 连接组」映射，
   需后续实现映射表（本批未做，见待办）。
4. **批量请求**：按规范允许服务端拒绝，本实现显式返回 `400`，不做批量合并。
5. ~~`src/i18n.c` 未完成的历史实验~~ → **已解决（阶段 J）**：`src/i18n.cpp` +
   `src/khash.h` 正式实现并参与编译，`lang_res/zh.json` 以 `RCDATA("JSON_ZH")`
   嵌入 `vbmcp.dll`，`lang.cpp/lang.h` 的 `T_*` 双语数组已删除。
6. ~~`strbuf.cpp`~~ → **已删除（阶段 M）**：HTTP 头拼装改用 `std::string`，
   编码转换归入 `util.cpp`（`vbmcp_str_to_gbk` / `vbmcp_acp_to_utf8`），
   仓库中不再存在该模块。
7. **待办（本批未做）**：会话 id 跨连接映射表；工具描述精简（当前最长约 260 字符，
   占用 AI 上下文）；协议版本向 2025-06-18 演进评估；`/health` 的 `pid` 字段在
   局域网场景可考虑一并隐藏。

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
- [ ] 中文项目「读 → 修改 → 写 → 回读」全链路一致（默认 `utf-8`；`encoding:"gbk"` 回退旧行为）
- [ ] 只读/读写切换后，已连接客户端收到 `notifications/tools/list_changed` 并自动重拉清单（能力声明已为 true）
- [ ] 设置窗体可拖拽缩放；按 Esc 可关闭；「恢复默认设置」只改界面、点「取消」可放弃
- [ ] 慢客户端不读 SSE 时，待发缓冲超过 4MB 被断开且日志有记录

---

## 八、阶段 M：优化批次（P0–P4）实施记录

方案、问题证据与用户决议见 [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md)；本节只记录
**实施结果与验证**（2026-10-15）。

### 8.1 改动清单

| 项 | 内容 | 主要位置 |
|---|---|---|
| P0-1 | `.gitignore` 修正：`tools/` 脚本与测试工程入库，产物 / `.vs` / `__pycache__` 忽略；`Win32/Release/vbmcp.dll` 停止跟踪 | `.gitignore` |
| P0-2 | 阶段 A–L 成果分批提交（2 个提交），工作树归零 | git 历史 |
| P1-1 | `initialize` 只声明实际实现的 tools 能力，`listChanged=1`（与广播行为对齐）；删除未实现的 prompts/resources 声明 | `mcp_rpc.cpp` |
| P1-2 | 工具描述去掉残留的手写 JSON 转义（`\"0\"=off` → `"0"=off, "1"=on`） | `mcp_tools.cpp` + `lang_res/zh.json` |
| P1-3 | 无 `id` 的请求（notification）统一 202 空响应，不再按方法名白名单 | `mcp_rpc.cpp` |
| P1-4 | i18n 表指针原子化（`g_mode`/`g_trans`），让网络线程查表成为定义良好的并发 | `i18n.cpp` |
| P1-5 | `WS_THICKFRAME` 改为运行期补样式（资源编辑器保存会丢），窗体恢复可拖拽缩放 | `options_dialog.cpp` |
| P1-6 | Esc 可关闭窗体；「恢复默认设置」只重置界面、由「确定」统一生效 | `options_dialog.cpp` |
| P2-1 | SSE 单连接待发缓冲上限 4MB，超限断开该连接 | `mcp_http.cpp` |
| P2-2 | HTTP 头长度独立校验（16KB 上限真正生效） | `mcp_http.cpp` |
| P2-3 | 会话 id 由 UI 线程在 initialize 完成后回标（`task->is_initialize`），取消 body 字符串搜索 | `mcp_http.cpp` + `mcp_rpc.cpp` |
| P2-4 | 服务未运行时不再残留 `list_changed` 广播标志 | `mcp_http.cpp` |
| P2-5 | CORS 白名单进程内缓存（服务 stop/start 失效） | `mcp_http.cpp` |
| P2-6 | 模块名列表进程内缓存（写操作 / 项目文件名变化失效） | `mcp_tools.cpp` |
| P2-7 | 日志合并为单实现 + 单文件 10MB 自动轮转（`.old`） | `util.cpp` |
| P2-8 | 非回环监听时 `/health` 只返回文件名，不暴露宿主绝对路径 | `mcp_http.cpp` |
| P4-3a | `vbmcp_server_start` 的 bind 等待由 `Sleep` 忙等改为条件变量（最多 3 秒） | `mcp_http.cpp` |
| D-1 | **UTF-8 编码桥（用户决议）**：读侧 ANSI→UTF-8、写侧 UTF-8→ANSI；全部工具新增可选参数 `encoding`（默认 `utf-8`，`gbk` 回退字节透传）；含自由文本的结果带 `encoding` 字段；`initialize.instructions` 声明契约 | `util.cpp` + `mcp_tools.cpp/h` + `mcp_rpc.cpp` |
| D-3 | 工具错误码改为显式 `int *out_err` 回传，删除全局 `g_tool_err` | `mcp_tools.cpp/h` |
| D-4 | （既有能力）引擎 Set 泄漏分级告警；SECURITY.md 按实际泄漏量级如实描述 | `vbdec_sdk.cpp` / `SECURITY.md` |
| P4-1 | 全部 md 去噪：strbuf / 旧 API / i18n_selftest / 词条数 / 样式表述 / 文件结构等 | 各文档 |
| P4-2 | 新增 `tools/selftest`（26 项断言）+ `tools/check_all.bat`；`ui_preview` 随 `tools/` 入库 | `tools/` |
| P4-3b | `build.bat` 支持 `VBDEC_DIR` 环境变量覆盖宿主路径 | `build.bat` |

### 8.2 验证结果

| 验证项 | 结果 |
|---|---|
| Release 构建（`msbuild vbmcp.sln /p:Configuration=Release /p:Platform=Win32`） | 0 error / 0 warning |
| `tools/selftest`（编码桥往返 / i18n 纯函数 / 工具 schema） | 26 checks, 0 failure |
| `python tools/extract_i18n.py --check` | 98 词条，新增 0 / 待翻译 0 / 失效 0（zh 全译） |
| `python tools/gen_ids_header.py --check` | 34 个 ID 同步 |
| `tools/check_all.bat` | 全绿（以上检查的一键入口） |

### 8.3 待人工回归（需 VB Decompiler 宿主，已并入第七节清单）

- 中文项目读写全链路（默认 utf-8 / `encoding:"gbk"` 回退）；
- 权限切换 → 客户端收到 `list_changed` 并刷新清单；
- 窗体缩放 / Esc / 「恢复默认」的交互；
- 各 MCP 客户端对 `encoding` 参数与长描述的兼容性。

### 8.4 后续修复：端口微调钮（用户反馈，2026-10-15）

| 问题 | 根因 | 修法 | 验证 |
|---|---|---|---|
| 点击端口右侧 + / - 无反应 | ① rc 未写 `UDS_AUTOBUDDY`、代码也未发 `UDM_SETBUDDY`，buddy 未绑定，`UDS_SETBUDDYINT` 无写回目标；② 补绑 buddy 与 32 位范围后，该自动写回在本环境把文本写成首字符（`3023`→`3`）；③ 16 位 `UDM_SETRANGE` 装不下 65535 | **弃用 up-down 自带的 buddy/数值机制**：`dlg_proc` 新增 `UDN_DELTAPOS` 分支，按通知的 `iDelta` 直接改端口编辑框文本（±1，clamp 1..65535）并刷新接入页；编辑框文本是端口值的唯一事实来源 | `tools/ui_preview` 新增 `probe_port_spin`：合成鼠标点击箭头区域（与用户点击同一条控件代码路径），探针输出 `click '+' -> 3024 PASS +1` / `click '-' -> 3023 PASS back to initial` |

### 8.5 连接兼容性修复（用户反馈：CodeBuddy 连不上，2026-10-15）

| 项 | 问题 | 修法 |
|---|---|---|
| 协议版本头 | `protocol_version_supported()` 只接受 `2025-03-26` / `2024-11-05`；新 SDK 客户端在后续请求头带 `2025-06-18` 会被 `400` 拒掉（表现为"无法连接"） | 支持集合扩为 { `2025-06-18`, `2025-03-26`, `2024-11-05` } |
| initialize 协商 | 此前固定回 `2025-03-26` | 新增 `negotiate_protocol_version()`：按客户端 `params.protocolVersion` 回同一版本（客户端会据此设置后续请求头），`/health` 展示协商结果 |
| Accept 校验 | POST 要求 `Accept` 必须含 `application/json`，只声明 `text/event-stream` 的客户端会被 `406` | 放宽：命中 `application/json` 或 `text/event-stream` 其一（或未携带）即放行（响应体始终是 JSON） |
| chunked 传输 | `Transfer-Encoding: chunked` 被当成空 body，报误导性的 parse error | 明确回 `400` + `send Content-Length instead`；MCP 客户端均使用 Content-Length，不做 chunked 解码 |
| 诊断日志 | "连不上"时看不到请求头细节 | DEBUG 级请求日志增加 `accept` / `content-type` / 协议版本 / 会话摘要；排查时开 `VBMCP_DEBUG_LOG=1` 看每行请求与 WARN 级 `transport error` |

### 8.6 严重缺陷修复：accept 阻塞导致服务器完全不响应（用户反馈：CodeBuddy 连不上）

**现象**：设置窗体显示 running、TCP 能建立连接（日志有 `client connected`），
但任何 HTTP 请求都得不到响应（`/health` 也超时），且断开后无
`client disconnected` 日志 —— "连上了但拿不到任何数据"。

**根因**（`mcp_http.cpp`）：监听 socket 为**阻塞模式**，而 accept 循环在
`select` 报告可读后连续调用 `accept`；第二次 `accept`（此时已无等待连接）
**永久阻塞整个网络线程**。此后所有 IO（读请求 / 发响应 / 检测断开）全部停摆；
每个新连接只是把卡死的 `accept` 唤醒一次（打印一条 `client connected`）随即再次
卡死。**该缺陷自阶段 D（网络层实现）起即存在**——代码里 recv/send 的
`WSAEWOULDBLOCK` 分支本就齐全（说明原设计就是非阻塞），只是漏了模式设置，
且此前从未做过真正的端到端连接验证（第七节清单一直是未勾选状态）。

**修复**：

1. 监听 socket 设 `ioctlsocket(FIONBIO)`：accept 循环能正常以
   `WSAEWOULDBLOCK` 退出，不再阻塞；
2. accept 出的连接 socket 显式设非阻塞（不依赖继承行为）：recv/send 的
   `WSAEWOULDBLOCK` 分支按设计工作，顺带消除"慢客户端阻塞大响应 send"的隐患；
3. `VBMCP_MAX_CONNECTIONS` 64 → 60：`fd_set` 还要放 listener，且 Windows 的
   `FD_SET` 在满时会**静默忽略**新句柄（那些连接会不被 select 监视、静默挂起）。

**验证**：新增 `tools/mcp_e2e`（本机把真实网络/RPC/工具代码跑成完整服务器，
无需 VB Decompiler）+ `tools/e2e_test.ps1`（12 项断言）。修复后全部通过；
修复前同一测试在 `/health` 一步即超时。已纳入 `tools/check_all.bat` 第 6 步。