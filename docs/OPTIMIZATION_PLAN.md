# vbdmcp 优化方案（已实施 · 2026-10-15）

> 状态：**已实施（2026-10-15）**。用户决议：全部实施（P0–P4），且工具输出统一 UTF-8。
> 逐项实施结果与验证见文末「决议与实施对照」与「实施记录」；
> 长期历史记录见 [REFACTOR_PROGRESS.md](REFACTOR_PROGRESS.md) 第八节（阶段 M）。
> 评估基准：2026-10-15 工作树（当时 HEAD = `e1dac25`，其上存在大量未提交改动，见 P0-2）。
> 评估方式：静态审查 + 只读校验脚本（未运行 VB Decompiler 宿主；需真机回归的项已标注）。

---

## 1. 评估范围与方法

已完整阅读（约 6100 行源码 + 全部文档）：

| 类型 | 内容 |
|---|---|
| 源码 | `src/*.cpp`（9 个）、`src/*.h/.hpp`（5 个）、`src/vbdmcp.def` |
| 资源 | `res/vbdmcp.rc`、`res/resource.h`、`vbdmcp.vcxproj` |
| 脚本/配套 | `build.bat`、`.gitignore`、`.gitmodules`、`tools/*`、`lang_res/zh.json` |
| 文档 | `README.md`、`README_EN.md`、`SECURITY.md`、`docs/DEVELOPMENT.md`、`docs/REFACTOR_PROGRESS.md` |

已执行的只读校验（均通过）：

| 检查 | 结果 |
|---|---|
| `python tools/extract_i18n.py --check` | 98 条 msgid；zh 已译 95 / 待译 3 / 多余 0 / 缺失 0；16 项单元测试通过 |
| `python tools/gen_ids_header.py --check` | 34 个 ID 与 `src/vbdmcp_ids.h` 同步 |
| `dumpbin /exports Win32\Release\vbdmcp.dll` | 3 个导出名未修饰（`VBDecompilerPluginName/Author/Load`），以别名映射到 C++ 修饰名，宿主 GetProcAddress 正常 |

**未覆盖项**（后续需实测）：无法在真实 VB Decompiler 宿主 + 真实 MCP 客户端下运行，因此涉及「客户端实际收到的报文内容」「引擎编码行为」的结论标注为**待实测**。

**总体结论**：项目基线质量高（A–L 阶段重构完成，Release 构建 0 error / 0 warning，安全模型完整，i18n 工具链健壮）。本次发现的问题集中在五类：**工程卫生（P0）、协议一致性缺陷（P1）、健壮性（P2）、需要拍板的设计决策（D）、文档漂移（P4）**。没有发现会导致崩溃或数据损坏的严重缺陷。

---

## 2. 问题与建议清单

### 2.1 P0 工程卫生（建议最先处理，零功能风险）

#### P0-1 `.gitignore` 把整个 `tools/` 忽略，工具链处于「不入库」状态

- **证据**：`.gitignore` 含 `tools/`（整目录）与重复两行的 `Win32/`；`git status` 显示 `lang_res/`、`docs/`、`SECURITY.md`、`src/i18n.{h,cpp}`、`src/khash.h`、`src/vbdmcp_ids.h`、`src/vbdmcp_mem.hpp`、`.gitignore` 本身均未跟踪。
- **影响**：`tools/extract_i18n.py`（98 词条提取/同步/校验）、`tools/gen_ids_header.py`（ID 生成校验）、`tools/ui_preview`（UI 回归截图）、`verify_i18n_resource.ps1` 只存在于本机磁盘。任何人重新检出仓库后，**无法同步语言包、无法校验 ID、无法做 UI 回归**，且 README/DEVELOPMENT 中描述的流程全部失效。
- **建议**：
  1. 改为「忽略产物、保留脚本」：
     ```gitignore
     Win32/
     *.obj
     *.pdb
     *.exp
     *.lib
     *.aps
     .vs/
     # 工具链中间产物（脚本/工程必须入库）
     tools/**/obj/
     tools/**/Debug/
     tools/**/Release/
     lang_res/untranslated*.txt
     ```
  2. `git rm --cached -r Win32`（`Win32/Release/vbdmcp.dll` 目前仍被跟踪，`Win32/` 已写入 ignore 但对已跟踪文件无效；构建产物入库无意义）。
  3. `tools/` 下的 28 个 `.tlog/.obj/.pdb` 等中间产物保留在磁盘但不入库。
- **工作量**：S（约 20 分钟）。

#### P0-2 大量工作未提交，工作树与仓库严重脱节

- **证据**：`git status` 显示 C→C++ 迁移（`RM src/*.c -> *.cpp`）、`src/json.{c,h}` 与 `src/lang.{c,h}` 删除、`src/strbuf.*` 删除、i18n 全套新增、三份文档、`SECURITY.md` 全部未提交。
- **影响**：任何一次误操作（`git checkout .`、清理工具）都会丢失数天工作；无法回滚、无法二分定位。
- **建议**：按主题拆分 4 个提交（顺序执行，每步构建一次）：
  1. `chore: 修正 .gitignore（保留 tools 脚本；忽略产物与 .vs）` + `git rm --cached` 产物；
  2. `refactor: C → C++17 迁移（RAII/原子/yyjson；删除 json/lang/strbuf）`；
  3. `feat(i18n): gettext 风格语言包（_/_A/_F/_AF + extract_i18n.py + zh.json RCDATA）`；
  4. `feat: Streamable HTTP/SSE + UI 任务队列 + read/write 权限 + 安全模型 + docs/SECURITY.md`。
- **工作量**：S（约 30 分钟，含逐提交构建确认）。

### 2.2 P1 正确性 / 协议一致性（小改动，建议本批修复）

#### P1-1 `initialize` 能力声明与实际行为不符

- **证据**：
  - `src/mcp_rpc.cpp:169` 声明 `capabilities.tools.listChanged = 0`，但 `src/mcp_http.cpp:1143` 在写入权限变化时会广播 `notifications/tools/list_changed`。按 MCP 规范，客户端**只会在 listChanged=true 时监听该通知**；当前实现会让「权限变化自动通知客户端」形同虚设（REFACTOR_PROGRESS 4.5 声称已实现）。
  - `src/mcp_rpc.cpp:171-177` 声明了 `prompts` 与 `resources` 能力（含 `subscribe/listChanged`），但服务端**没有实现** `prompts/*`、`resources/*` 方法，客户端探测会得到 `-32601`。
- **建议**：`listChanged` 改 `true`；删除 `prompts`/`resources` 声明（只保留 `tools`，规范允许）；如希望保留 resources 声明，则同时实现 `resources/list` 返回空数组（二选一，决议见第 6 节 T-2）。
- **工作量**：S。

#### P1-2 工具 schema 描述残留「手写 JSON 时代」的转义

- **证据**：`src/mcp_tools.cpp:952` 与 `lang_res/zh.json:66` 中的词条是 `\\\"0\\\"=off \\\"1\\\"=on`（字符串实际内容含反斜杠）。当时的目的是手写 JSON 转义；现在 yyjson 会自动转义，导致客户端解析后看到 `\"0\"=off \"1\"=on`（多出反斜杠和引号）。
- **建议**：msgid 改成 `"0"=off, "1"=on`，跑 `python tools/extract_i18n.py` 同步语言包并补中文（`"0"=关, "1"=开`）。
- **工作量**：S。

#### P1-3 未知 notification 会被当成请求回应

- **证据**：`src/mcp_rpc.cpp:308-314` 只白名单了 4 个 `notifications/*`；其余无 `id` 的方法（MCP 未来扩展的通知、`notifications/message` 等）会落入方法分发末尾，返回 `Method not found` 错误文档。JSON-RPC 2.0 规定：**无 id 的请求（notification）不产生响应**。
- **建议**：在方法分发之前统一处理 `if (!idv) return ""（HTTP 202）;`，白名单可删除。
- **工作量**：S。

#### P1-4 i18n 表被网络线程读取，存在数据竞争

- **证据**：`src/i18n.cpp:50-52` 的 `g_trans/g_mode/g_source` 均为非原子普通变量；`mcp_http.cpp` 的**网络线程**会在 3 处调用 `_F()`（队列满提示、同连接并发提示、任务超时提示），即跨线程读表。i18n 已经用「旧表只退休不释放」保证了内存安全，但 C++ 层面仍是 data race（注释见 `src/i18n.h:176-179` 声称的线程安全尚未完全落实）。
- **建议**：`g_trans`、`g_mode` 改 `std::atomic`；`g_source` 改为固定 `const char*` 或加互斥保护；在 `i18n.h` 注释中写明真实约束。
- **工作量**：S。

#### P1-5 设置窗体「可拖拽缩放」实际失效（rc 丢失 `WS_THICKFRAME`）

- **证据**：`res/vbdmcp.rc:25` 模板 2000 的 STYLE 为 `DS_SETFONT | DS_MODALFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_POPUP | WS_CAPTION | WS_SYSMENU`——**没有 `WS_THICKFRAME`**，窗口边框不可拖动缩放。而 `docs/DEVELOPMENT.md`（168-171 行、147-155 行）与 `REFACTOR_PROGRESS.md`（阶段 K/L）均宣称「加 WS_THICKFRAME 可拖拽缩放」「template 已带 WS_THICKFRAME」，`layout_dialog()`/`WM_GETMINMAXINFO` 也都是按可缩放写的。
- **风险背景**：`.rc` 归 VS 资源编辑器管，保存时会把未勾选 "Resizing" 的样式重写掉——这正是文档里反复记录过的坑。
- **建议（推荐 a）**：
  - a. 在 `dlg_proc` 的 `WM_INITDIALOG` 运行期 `SetWindowLongPtr(GWL_STYLE, style | WS_THICKFRAME)` + `SetWindowPos(..., SWP_FRAMECHANGED)`；对资源编辑器免疫，无需重新踩坑；
  - b. 在 rc 里补回 WS_THICKFRAME，并在 `tools/gen_ids_header.py --check`（或 check_rc_health 脚本）中增加 STYLE 断言，防止再次被编辑器清掉。
- **工作量**：S（a）/ M（b，含校验脚本）。

#### P1-6 设置窗体交互两处不一致

- **证据 1（Esc 无效）**：`src/options_dialog.cpp:1093-1201` 的 `WM_COMMAND` 对未匹配 id 一律 `return TRUE`，`IDCANCEL`（Esc 键，id=2）被吞掉，按 Esc 无法关闭窗体（点 X 正常）。
- **证据 2（恢复默认立即生效）**：`src/options_dialog.cpp:1150-1168` 的「恢复默认设置」直接 `vbdmcp_server_stop()` + 写注册表 + 重置全局变量，点「取消」无法撤销；与同窗体「确定/取消」语义冲突。
- **建议**：① `if (id == IDCANCEL) { EndDialog(IDCANCEL); return TRUE; }`；② 「恢复默认设置」只重置控件状态（不落盘、不停服务），统一由「确定」生效；如需保留立即生效语义，则弹确认框并在文档中写明。
- **工作量**：S。

### 2.3 P2 健壮性 / 性能

#### P2-1 SSE 发送缓冲无上限，慢客户端可撑爆内存

- **证据**：`src/mcp_http.cpp:343-351` `sse_push_json()` 直接 `send_buf += ...`，心跳（`: ping`）与 `tools/list_changed` 广播同样追加；`vbdmcp_flush()` 只在可写时消化。若客户端不读（连接僵死），缓冲无界增长。
- **建议**：为 `Connection::send_buf` 设上限（例如 4MB），超限记录日志并断开该连接（与 `VBDMCP_MAX_CONNECTIONS` 同层防御）。
- **工作量**：S。

#### P2-2 HTTP 头部长度限制未真正生效

- **证据**：`src/mcp_http.cpp:421-427` 只在**未找到** `\r\n\r\n` 时检查 `buf.size() > VBDMCP_MAX_HEADER_SIZE`；一旦分隔符出现，`headers_len` 不再受 16KB 限制（`connection_on_readable` 只按 `VBDMCP_MAX_BODY_SIZE + VBDMCP_MAX_HEADER_SIZE` 兜底，即单连接可缓存 64MB+16KB；64 个连接理论峰值 4GB）。
- **建议**：`parse_request` 中在定位 `hdr_end` 后校验 `headers_len <= VBDMCP_MAX_HEADER_SIZE`，否则返回 -1；`recv_buf` 超限时先校验能否组成完整请求再拒绝。
- **工作量**：S。

#### P2-3 `initialize` 靠 body 字符串搜索识别

- **证据**：`src/mcp_http.cpp:671` `req.body.find("\"initialize\"") != npos`。任何 arguments/文本里含 `"initialize"` 字样的请求（例如 `vbd_write_function` 写入含该词的代码）都会被分配 `Mcp-Session-Id`。
- **建议**：让 UI 线程在任务完成时把「该请求是 initialize」写回 `UiTask`（加一个 `bool is_initialize`），网络线程据此分配会话 id 并回带响应头——零解析假设。次选：网络线程按 `"method"`+空白+`"initialize"` 模式做有界扫描（前 512 字节）。
- **工作量**：M（推荐方案）。

#### P2-4 服务未运行时广播标记会残留到下次启动

- **证据**：`src/mcp_http.cpp:1143-1151` `vbdmcp_notify_tools_list_changed()` 只置位 `g_list_changed_pending`；若此时服务器未运行（设置窗体在服务关闭状态下改权限），标志会留到下一次启动，首个 SSE 连接收到一次无意义的 `list_changed`。
- **建议**：加 `if (!vbdmcp_server_is_running()) return;`；或启动时清空标志。
- **工作量**：S。

#### P2-5 CORS 白名单每次请求读注册表

- **证据**：`src/mcp_http.cpp:141-161` `origin_is_trusted()` 每个 POST/OPTIONS 都 `vbdmcp_config_get_cors_origins()`（RegOpenKey + 读 1KB）。默认配置下等于每请求一次无谓注册表访问。
- **建议**：改为进程级缓存 + 设置窗体保存时刷新（或在 `vbdmcp_config_save` 后失效）。
- **工作量**：S。

#### P2-6 模块列表每次全量探测，无缓存

- **证据**：`src/mcp_tools.cpp:263-298` `vbd_read_list_modules` 每次调用都从 0 开始逐个调 `VBD_GetModuleName`（每个索引一次引擎调用 + WideCharToMultiByte 分配），上限 10000 次。大项目下每次 list 都是 O(N) 引擎调用。`vbd_read_list_forms` 用 `VBD_GetVBFormCount` 无此问题。
- **建议**：加短生命周期缓存（如项目文本哈希/`VBD_GetFileName` + 写入操作失效），或首次探测后缓存「模块数」；同时在文档写明 SDK 的 `VBD_GetModuleCount` 语义冲突背景。
- **工作量**：M。

#### P2-7 日志实现重复且无上限

- **证据**：`src/util.cpp:81-153` `vbdmcp_logf` 与 `vbdmcp_log` 是两份几乎相同的实现（仅阈值/标签差异）；日志文件 `%TEMP%\vbdmcp.log` 只追加，无大小/日期轮转。
- **建议**：`vbdmcp_log` 改为转调 `vbdmcp_logf(VBDMCP_LOG_INFO, ...)`；首次写入时按大小（如 10MB）截断或轮转。
- **工作量**：S。

#### P2-8 `/health` 在开放局域网时暴露宿主绝对路径

- **证据**：`src/mcp_http.cpp:560-599` 无条件返回 `path`（`GetModuleFileNameA` 绝对路径）与 `pid`。回环绑定下无碍；LAN 场景属信息泄露。
- **建议**：非回环绑定时省略 `path`（或仅返回文件名）。
- **工作量**：S。

### 2.4 D 设计决策项（需要你拍板，不属于「顺手修」）

#### D-1（重点）JSON 与引擎之间的编码桥缺失——中文读写双向乱码风险

- **证据链**：
  - 读：`src/vbdec_sdk.cpp:53-80` 把引擎返回的 UTF-16 转成 **ANSI（GBK）**；工具层 `src/mcp_tools.cpp` 直接 `yyjson_mut_obj_add_strcpy` 写进 JSON，序列化用 `YYJSON_WRITE_ALLOW_INVALID_UNICODE`（`doc_to_string`）。即**工具结果里含 GBK 原始字节，不是合法 UTF-8 的 JSON**。
  - 写：客户端按 JSON 规范发送 **UTF-8**；`arg_str()` 直取字节后 `vbd_set_value()` 原样透传给按 ANSI 解析的引擎（`engine_set_from_ansi`），中文写入会乱码。
  - 对照组：UI 路径已有正确做法（`vbdmcp_str_to_gbk`，`src/util.cpp:165`），但只被 `options_dialog.cpp:632` 使用；工具层没有任何转换。
- **影响**：AI 客户端读取含中文的反编译代码/字符串时可能显示乱码；AI 写入含中文的代码时工程数据被写坏。**这是需要实测确认影响面的最高价值改进**（若你的项目主要是英文标识符，影响面小；但字符串字面量/注释中文很常见）。
- **方案选项**：
  - A. **维持现状**，在 README/SECURITY 明确「工具数据为 ANSI/GBK 字节」，由用户自行处理；
  - B. **只修读侧**：所有 engine 文本在入 JSON 前 GBK→UTF-8（输出合法 UTF-8 JSON），写入仍要求客户端提供 GBK 字节（文档说明）；
  - C. **双向转换（推荐）**：读侧 GBK→UTF-8；写侧 UTF-8→GBK；新增可选参数 `encoding: "utf8"（默认）|"gbk"（回退旧行为）`，保证兼容；
  - D. C 的基础上再加环境变量/注册表开关 `DataEncoding`，便于排障。
- **风险与验证**：转换会改变现有客户端观察到的字节（老客户端如依赖旧行为需 `encoding:"gbk"` 回退）；需在真实宿主 + 至少一个 MCP 客户端（CodeBuddy/VS Code）实测含中文项目的读/写/回读一致性。
- **工作量**：L（4–8 小时，含实测）。

#### D-2 会话 id 跨连接语义与文档不一致

- **证据**：`src/mcp_http.cpp:622-626` 仅在「请求头带 session id 且**当前连接已持有** session id」时校验；新连接携带 session id（规范允许的恢复场景）不会 404，也不会关联到旧会话；README 表述为「后续请求带该头且不匹配 → 404」。REFACTOR_PROGRESS「已知限制 #3」已承认。
- **选项**：A. 保持宽松实现，修正 README 表述（S）；B. 实现 `session_id → 连接组` 映射表（M，需处理断线清理）。建议 A。

#### D-3 工具错误码经全局变量 `g_tool_err` 传递

- **证据**：`src/mcp_tools.cpp:28` 全局 `g_tool_err`，`err_json()` 写、`mcp_dispatch_tool()` 读回填。当前串行执行下安全，但任何「忘记调用 err_json 的路径」或未来并发化都会踩坑。
- **选项**：A. 保持（注释说明约束）；B. 重构为 `impl(args, int* out_err)` 显式返回（改动 28 个函数签名 + 表结构，M）。建议 B（在 D-1 之后做，二者都动工具层，合并一次改动成本更低）。

#### D-4 引擎 Set 泄漏的可观测性

- **证据**：`src/vbdec_sdk.cpp:93-116` 引擎持有 Set 指针（设计如此），累计泄漏量已有阈值告警日志；但 `/health` 与设置窗体看不到。
- **选项**：把 `set_leak_bytes` 暴露到 `/health`（如 `engineSetLeakBytes`），便于用户在客户端反复写大项目时自查（S）。建议做。

### 2.5 P4 文档与测试工具

#### P4-1 文档漂移清单（逐条核对结果）

| # | 位置 | 现状 | 应更新为 |
|---|---|---|---|
| 1 | `README.md` 文件结构、`README_EN.md`、`docs/DEVELOPMENT.md:59` | 描述 `src/strbuf.h` / `strbuf.cpp` 模块 | 该模块已删除（git 显示 `RD src/strbuf.*`），条目移除 |
| 2 | `docs/DEVELOPMENT.md:270,273` 新增工具示例 | 使用已删除的 `vbdmcp_t_utf8(T_ERR_*)` API | 改为 `_()` / `_F()` 示例 |
| 3 | `docs/DEVELOPMENT.md:125,351`、`REFACTOR_PROGRESS.md:175` | 引用 `tools/i18n_selftest`（20 项断言） | 该目录不存在；要么恢复该测试工程（见 P4-3），要么改为说明「自测已移除」 |
| 4 | `docs/DEVELOPMENT.md`（i18n 章节）/`REFACTOR_PROGRESS.md:144,176` | 「104 条词条」 | 实测 98 条（zh 已译 95 / 待译 3） |
| 5 | `REFACTOR_PROGRESS.md` 阶段 K/L、`DEVELOPMENT.md:168-171` | 「rc 带 WS_THICKFRAME，窗体可拖拽缩放」 | 实际 rc 无 WS_THICKFRAME（见 P1-5），修复后再恢复表述 |
| 6 | `README.md:244-276` 文件结构 | 未列 `docs/OPTIMIZATION_PLAN.md`、`lang_res/` 新文件 | 随 P0-2 提交时顺带更新 |
| 7 | `src/config.cpp:20` | 注释指向已删除的 `lang.cpp` | 改为 `i18n.cpp` |
| 8 | `src/vbdmcp_mem.hpp:56-58` | 注释描述 `ScopeGuard`（未实现） | 实现或删除注释 |
| 9 | `src/mcp_tools.h:30`、`src/mcp_tools.cpp:1029` | `mcp_build_tools_schema()` 无调用点（死代码） | 删除，或接入 `/health`/调试端点 |
| 10 | `src/vbdmcp.h:178`、`util.cpp:121` | `vbdmcp_log` 标注「兼容旧调用点」 | 合并到 `vbdmcp_logf`（见 P2-7） |
| 11 | `res/vbdmcp.rc:114` | 版本信息 `Translation 0x409, 1200`（英文）但资源语言是中文 | 改 `0x804, 1200` 或保持（无功能影响，最低优先级） |
| 12 | `zh.json` 3 条待译 | `mcp_http.cpp` 网络线程的 3 条 `_F()` 消息未翻译 | 补中文 |

- **工作量**：M（2–3 小时）。

#### P4-2 测试与校验基础设施

- **现状**：`extract_i18n.py --check`、`gen_ids_header.py --check`、`ui_preview`、`verify_i18n_resource.ps1` 都在，但 `i18n_selftest` 工程消失、`ui_preview` 未入库、没有统一入口。
- **建议**：
  1. `tools/selftest/`（新）：纯 C++ 控制台工程，覆盖 i18n（回退/占位符校验/GBK 转换）、HTTP 头解析（`parse_request` 边界）、工具 schema 构建（yyjson 输出断言），可在无宿主环境运行；
  2. `tools/check_all.bat`：一键跑 `gen_ids_header --check` + `extract_i18n --check` + `selftest` + Release 构建；
  3. `ui_preview` 工程与源码入库（配合 P0-1）。
- **工作量**：M–L（半天到一天）。

#### P4-3 其他可选改进（低优先级，建议先记录）

| # | 项 | 说明 |
|---|---|---|
| 1 | `build.bat` 硬编码宿主路径 `E:\Ollydbg\VB Decompiler Pro v11.5` | 改为 `if defined VBDEC_DIR (...)` 可用环境变量覆盖 |
| 2 | `vbdmcp_server_start()` 用 `Sleep(10)×300` 忙等 bind 完成（`mcp_http.cpp:1069`） | 理论上 UI 线程最长阻塞 3 秒；可改为事件/条件变量等待，通常 <10ms 无感 |
| 3 | `tools/list` 工具描述超长（最长约 260 字符） | 影响 AI 上下文占用；可选精简，或按需拆「详细描述」到 `instructions` |
| 4 | 结果字段类型 | `vbd_read_project_info` 的 `is_native`/`is_packed` 返回 `"1"/"0"` 字符串，可加布尔字段（保留旧字段兼容） |
| 5 | 协议版本 | ~~后续评估向 2025-06-18 演进~~ → 已实施（阶段 M 8.5）：支持集合 {2025-06-18, 2025-03-26, 2024-11-05}，`initialize` 按客户端请求协商 |
| 6 | 提交前检查 | 可在 `tools/check_all.bat` 之外补 CI（GitHub Actions 或本地 pre-commit），至少跑两个 `--check` |

---

## 3. 建议实施批次

> 原则：先做零风险工程卫生 → 再做小而确定的正确性修复 → 再做健壮性 → 最后做需要实测/决策的大改动。

| 批次 | 内容 | 预计工作量 | 验收标准 |
|---|---|---|---|
| **批次 1（建议立即）** | P0-1、P0-2（gitignore + 分批提交） | 1 小时 | `git status` 干净；重新检出后 `tools/` 脚本存在；`vbdmcp.dll` 不再被跟踪 |
| **批次 2（低成本修复）** | P1-1 ~ P1-6、P2-4、P2-7、P4-1 文档批量同步、`zh.json` 3 条补译 | 半天 | Release 0 error/0 warning；`extract_i18n --check` 与 `gen_ids_header --check` 通过；`tools/list` 描述无 `\"`；Esc 可关窗；窗体可拖拽缩放 |
| **批次 3（健壮性）** | P2-1、P2-2、P2-3、P2-5、P2-6、D-4、P2-8 | 1 天 | 新增针对 `parse_request`/SSE 上限的 selftest 用例；大项目下列表工具耗时下降（可日志计时对比） |
| **批次 4（决策后执行）** | D-1（按你选的方案）+ D-3 合并重构 + 实测 | 1–2 天 | 中文项目「读 → AI 修改 → 写 → 回读」全链路一致；旧行为可通过参数/开关回退 |
| **批次 5（可选）** | P4-2 测试基础设施 + P4-3 各项 | 1 天 | `tools/check_all.bat` 一键全绿 |

---

## 4. 每批次的统一验证方式

1. `msbuild vbdmcp.sln /p:Configuration=Release /p:Platform=Win32` → 0 error / 0 warning；
2. `python tools/extract_i18n.py --check`、`python tools/gen_ids_header.py --check` 通过；
3. UI 相关改动：`ui_preview` 截图（中/英 × 默认/放大）人工比对 + `%TEMP%\vbdmcp_ui_probe.txt` 坐标抽查；
4. 协议相关改动：`curl` 验证 `/health`、`tools/list` 数量、`initialize` 能力字段、`notifications/*` 返回 202、Esc/缩放行为；
5. 涉及宿主的改动（D-1 等）：在装有 VB Decompiler 的机器上按 `REFACTOR_PROGRESS.md` 第七节手工回归清单执行，并补充「中文项目读写一致性」用例。

## 5. 不改变的部分（明确边界）

- 安全模型（默认回环、无 CORS 通配、写入开关、会话准入）不动；
- 线程模型（网络线程只做传输、工具全量投递 UI 线程）不动；
- 28 个工具的名称与读写分组不动（D-1 只增加可选参数，不破坏现有参数）；
- i18n 的 msgid=英文原文、`_()/_A()` 双轨、语言包只从 RCDATA 加载等约定不动。

---

## 6. 决议与实施对照

> 用户回复：**「工具输出结果采用 utf-8 …… 最后实施 P0-P4」**（即全部条目不裁剪）。

| 编号 | 事项 | 决议 | 实施情况 |
|---|---|---|---|
| T-1 | 批次 1（gitignore + 整理提交） | 做 | 已完成：`.gitignore` 修正 + 2 个整理提交 |
| T-2 | 能力声明 | 只声明 tools | 已完成：`listChanged=true`，删除 prompts/resources |
| T-3 | 缩放修复方式 | 运行期补 `WS_THICKFRAME` | 已完成（`options_dialog.cpp`，对编辑器重写免疫） |
| T-4 | 「恢复默认设置」语义 | 只改 UI、确定后生效 | 已完成（含「权限模式」标签按勾选预览） |
| T-5 | **编码桥** | **工具输出统一 UTF-8**（双向转换 + 参数回退） | 已完成：默认 `utf-8`（读转出 / 写转回），`encoding:"gbk"` 回退旧行为 |
| T-6 | 会话 id | 保持宽松实现、修正文档 | 已完成（README/REFACTOR 表述同步） |
| T-7 | 错误码传递 | 显式 out 参数 | 已完成（删除全局 `g_tool_err`） |
| T-8 | 测试基础设施 | 做 | 已完成：`tools/selftest`（26 项）+ `tools/check_all.bat` |
| T-9 | 其他痛点 | 无补充 | — |

**关于"结果是否要携带编码信息"的落地建议**（已按此实施）：

1. **契约级**：默认 UTF-8 写进 `initialize.instructions`，客户端可读到；
2. **结果级**：只给 7 个"含自由文本"的读工具结果加一个顶层 `encoding` 字段
   （project_text / form / module / function / sub_main / active_text / active_disasm_text）；
   纯数字 / 列表 / 名称类结果不加（避免冗余与 AI 上下文浪费）；
3. **参数级**：全部 28 个工具的 schema 增加可选 `encoding` 参数（默认 `utf-8`，
   `gbk` 回退），客户端显式可查、可按需切换。

---

## 7. 实施记录（2026-10-15）

- 逐项改动清单、验证结果与待人工回归项见
  [REFACTOR_PROGRESS.md 第八节](REFACTOR_PROGRESS.md)（阶段 M）；
- 验证结论：Release 构建 0 error / 0 warning；`tools/selftest` 26 项断言全过；
  `python tools/extract_i18n.py --check`、`python tools/gen_ids_header.py --check` 全绿；
  `tools/check_all.bat` 可一键复现全部检查；
- 明确留作后续（未做）：会话 id 跨连接映射表、工具描述精简、协议版本向 2025-06-18
  的演进评估、`/health` 的 `pid` 字段在局域网场景的取舍。
