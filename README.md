# VB Decompiler MCP Server Plugin
(https://www.vb-decompiler.org/plugins_sdk.htm)

[简体中文](README.md) | [English](README_EN.md)

C++17 实现的 [VB Decompiler](https://www.vb-decompiler.org) 插件，作为 [MCP（Model Context Protocol）](https://modelcontextprotocol.io) 服务器运行，使 AI 客户端能够通过 **Streamable HTTP（SSE）** + JSON-RPC 2.0 接口读取、修改由 VB Decompiler 反编译生成的代码与内部数据结构。

<img src="pic/pic2.png" width="350" alt="截图">

## 特性

- **MCP Streamable HTTP 传输**：客户端 `POST` 发送请求，服务端通过 **SSE 长连接**推送消息与保活；协议版本在 `initialize` 时按客户端请求协商（支持 `2025-06-18` / `2025-03-26` / `2024-11-05`，默认回 `2025-03-26`）。
- **28 个 MCP 工具，严格分两组**：`vb_read_*`（13 个，只读，始终可见）与 `vb_write_*`（15 个，仅在勾选「允许 MCP 客户端写入项目数据」后出现）。
- **默认只监听 `127.0.0.1`**：绝不默认监听 `0.0.0.0`；开放局域网需在设置窗体手动开启，并弹高危警告。
- **会话准入授权**：客户端首次真正调用工具（`tools/call`）时弹窗确认，展示客户端连接信息；`listTools` 等预检不弹窗。
- **默认只读**：`listTools` 动态过滤，写权限需用户在 UI 显式开启；`callTool` 入口二次校验权限。
- **后台网络线程 + UI 任务队列**：网络 IO 在独立后台线程，工具调用一律投递到 UI 主线程执行，带超时、取消令牌与并发上限，绝不阻塞/带崩界面。
- **全量 yyjson**：无手写 JSON 序列化代码；协议层错误一律使用 MCP 标准错误对象。
- **UTF-8 文本契约**：工具默认在工程 ANSI/GBK 字节与 UTF-8 之间双向转换（读侧转出、写侧转回），可用 `encoding:"gbk"` 回退旧字节透传行为。
- **中英双语界面**：菜单、设置窗体、AI 客户端接入向导、帮助文本，以及 MCP `tools/list` 描述与工具错误消息，均跟随 VB Decompiler 主程序语言。

## 安全模型（务必阅读）

| 机制 | 默认行为 | 说明 |
|---|---|---|
| 监听地址 | `127.0.0.1:3023` | 仅本机可访问；开放局域网需手动开启 + 高危警告 |
| CORS | 不返回任何 CORS 头 | 禁止 `Access-Control-Allow-Origin: *`；如需浏览器客户端，需手动配置可信 origin 白名单（注册表 `CorsOrigins`） |
| 写入权限 | 关闭（只读） | 设置窗体勾选「允许 MCP 客户端写入项目数据」后才出现 `vb_write_*` |
| 会话准入 | 首次 `tools/call` 弹窗 | 允许本次会话 / 拒绝；服务重启或全部客户端断开后重置 |
| 体积/限流 | 64MB body、200 req/s、8 个并发任务 | 超限返回 `413` / `429` + MCP 标准错误 |

详见 [SECURITY.md](SECURITY.md)。

## MCP 工具列表
<img src="pic/pic1.png" width="500" alt="截图">

所有工具统一加 `vb_` 前缀，避免与其他 MCP 服务器的同名工具冲突；后缀标识读写属性（详见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)）。

### 只读组 `vb_read_*`（只读模式下即可见、可调用）

| 工具 | 说明 |
|---|---|
| `vb_read_project_info` | 项目元信息（路径、native/pcode、编译器类型、打包状态） |
| `vb_read_project_text` | 完整项目文本 |
| `vb_read_list_forms` / `vb_read_form` | Form 列表 / 读取 |
| `vb_read_list_modules` / `vb_read_module` | Module 列表 / 读取 |
| `vb_read_list_module_functions` / `vb_read_function` | Function 列表 / 读取（含反汇编视图） |
| `vb_read_sub_main` | 入口点 Sub Main |
| `vb_read_active_text` / `vb_read_active_disasm_text` | 当前激活窗口 / 反汇编窗口文本 |
| `vb_read_active_coords` | 激活视图坐标 |
| `vb_read_list_frx_icons` | FRX 图标资源枚举 |

### 写入组 `vb_write_*`（仅勾选写入权限后出现）

| 工具 | 说明 |
|---|---|
| `vb_write_project_text` | 覆盖整个项目文本 |
| `vb_write_clear_buffers` | 清空 VB Decompiler 内部缓冲 |
| `vb_write_form` / `vb_write_rename_form` | Form 修改 / 重命名 |
| `vb_write_module` / `vb_write_module_string_references` | Module 声明 / 字符串引用修改 |
| `vb_write_function` / `vb_write_rename_function` | Function 代码修改 / 重命名 |
| `vb_write_sub_main` | 修改入口点 |
| `vb_write_active_text` / `vb_write_active_disasm_text` | 修改当前窗口 / 反汇编窗口文本 |
| `vb_write_goto_line` | 跳转到指定行 |
| `vb_write_status` | 设置状态栏文字 |
| `vb_write_options` | 控制「解析栈参数」「过程分析器」复选框 |
| `vb_write_update_all` | 提交所有修改并刷新 GUI |

分页：`vb_read_list_forms` / `vb_read_list_modules` / `vb_read_list_module_functions` / `vb_read_list_frx_icons` 接受 `limit`（默认 100，最大 10000）与 `offset`（默认 0），返回 `{count, total_count, offset, limit, has_more, next_offset, ...}`。

数据编码：全部工具接受可选参数 `encoding`（`"utf-8"` 默认 / `"gbk"` 旧行为）。默认模式下服务端把工程的 ANSI/GBK 字节转成 UTF-8 输出、把客户端 UTF-8 参数转回 ANSI/GBK 写入，保证 MCP 报文始终是合法 UTF-8；含代码/声明/项目文本的结果附带 `{"encoding": "utf-8"}` 字段，便于客户端判断。仅在客户端仍按 ANSI 字节处理数据时才需要传 `encoding:"gbk"`。

## 快速开始

### 构建

需要：Visual Studio 2019/2022（含 C++ 生成工具，v142）+ Windows 10+。

```cmd
build.bat
```

输出：`Win32\Release\vbmcp.dll`。工程为 **C++17**（`/std:c++17`、`/EHa`）。

### 安装

把 `vbmcp.dll` 复制到 VB Decompiler 安装目录下的 `Plugins` 文件夹（首次安装需创建）：

```
C:\Program Files\VB Decompiler\Plugins\vbmcp.dll
```

### 启动

插件**不会自动启动 MCP 服务器**——这是刻意设计：保证服务器启动那一刻 `VBDecompilerPluginLoad` 重入已完成、Callback Engine 一定有效。

1. 菜单 *Plugins → VB Decompiler MCP Server* 打开设置窗体；
2. 勾选 **启用 MCP 服务器**（可选：勾选 **允许 MCP 客户端写入项目数据**），点 *确定*。

未启用时无任何监听端口。

### 配置

设置窗体采用**双属性页**布局：

| 属性页 | 内容 |
|---|---|
| **服务设置** | 分组框「服务」：启用 MCP 服务器、绑定 IP 地址、端口号、服务器状态、权限模式（只读/读写）、局域网访问（高级，默认隐藏）；分组框「权限与安全」：允许 MCP 客户端写入项目数据 + 安全说明 + 生效提示 |
| **接入信息** | 客户端软件下拉（8 个 AI 客户端）→ 自动给出**写入路径**（该客户端的 MCP 配置文件位置）、**连接地址**、可直接粘贴的**配置片段**，以及**接入说明**；每项右侧均有「复制路径 / 复制地址 / 复制配置」按钮 |
| 底部 | 恢复默认设置 / 帮助 / 确定 / 取消（两页共用） |

> 窗体标题栏可拖拽缩放（最小 640×560，默认 720×636，随 96dpi 设计）；
> 字号 12pt 并按系统 DPI 缩放；说明类文字使用透明背景，不会出现"带底色的文字块"。

配置可通过设置窗体修改，或直接编辑注册表：

```
HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\MCP
```

| 名称 | 类型 | 说明 | 默认 |
|---|---|---|---|
| `Port` | `REG_DWORD` | 监听端口（1–65535） | `3023` |
| `BindAddress` | `REG_SZ` | 绑定 IP（**默认且推荐 `127.0.0.1`**） | `127.0.0.1` |
| `AllowWrite` | `REG_DWORD` | 允许 MCP 客户端写入项目数据（0/1） | `0`（只读） |
| `CorsOrigins` | `REG_SZ` | 可信 origin 白名单（逗号分隔；空=不发任何 CORS 头；`*` 被忽略） | 空 |

- 字段缺失或值越界时，插件用默认值补齐并**自动写回注册表**。
- 设置窗体改完点 *确定* → 写注册表并立即启动/停止/重启服务器。
- **重启 VB Decompiler 后授权状态重置**（服务不自动启用，需重新勾选）。

### 界面语言

UI 语言跟随 VB Decompiler 主程序设置（注册表 `...\VB Decompiler\Options\CustomLanguage`）：
`ChineseSimplified` → 简体中文；其他值/缺失 → 英文。覆盖菜单、设置窗体、MessageBox、帮助、AI 客户端接入向导，以及 MCP `tools/list` 描述与工具错误消息（UTF-8 JSON）。

**国际化实现**：gettext 风格，源码里统一写 `_("English text")`，中文译文放在
`lang_res/zh.json`，构建时以 `RCDATA("JSON_ZH")` **嵌入 vbmcp.dll**
（不依赖任何外部 langpack）。新增/修改文案后运行 `python tools/extract_i18n.py`
自动同步语言包并生成待翻译清单；未翻译的词条运行时自动回退英文。详见
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#多语言i18n注意事项)。

### 调试日志

```cmd
set VBMCP_LANG=en          :: 临时强制界面语言（zh/en），优先于注册表设置
set VBMCP_DEBUG_LOG=1     :: 写 %TEMP%\vbmcp.log
set VBMCP_LOG_LEVEL=3     :: 0=ERROR 1=WARN 2=INFO(默认) 3=DEBUG
set VBMCP_TASK_TIMEOUT_MS=30000   :: 单个 MCP 任务超时（毫秒，1000~600000）
```

日志只记录连接、工具名、权限判定、错误分类与耗时，**不记录任何项目源码/反编译文本**；
单文件超过 10MB 时自动轮转为 `%TEMP%\vbmcp.log.old`（只保留一代）。

### 健康检查

```cmd
curl http://127.0.0.1:3023/health
```

```json
{"status":"ready","server":{"name":"VB Decompiler MCP Server","version":"2.0.0","protocolVersion":"2025-03-26"},"instances":[{"name":"VB Decompiler.exe","arch":"Win32","bits":32,"path":"C:\\...","pid":1234,"local":true,"state":"ready","readOnly":true,"tools_count":13}],"ready":1}
```

`readOnly:true` + `tools_count:13` 表示处于只读模式（勾选写入权限后为 `false` + `28`）。

### 自测（无需 VB Decompiler 宿主）

提交前一键跑全部检查（资源 ID 一致 + i18n 词条一致 + 纯函数自测 + Release 构建）：

```cmd
tools\check_all.bat
```

也可单独运行：

- `tools\selftest`（编码桥 / i18n / 工具 schema，26 项断言）；
- `tools\e2e_test.ps1`（端到端 MCP 协议：health / initialize 版本协商 /
  tools-list / ping / SSE（含"不带 Content-Length"与心跳回归）/ 错误面，16 项断言；在本机把真实服务器代码
  跑起来测试，**无需 VB Decompiler**）。

详见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) 第 5 节。

### MCP 客户端配置

以 CodeBuddy / Claude Desktop 为例（Streamable HTTP，键名为 `type`）：

```json
{
  "mcpServers": {
    "vbmcp": {
      "type": "http",
      "url": "http://127.0.0.1:3023/mcp"
    }
  }
}
```

设置窗体「接入信息」属性页内置 CodeBuddy、VS Code、Cursor、Trae、WorkBuddy、Claude Desktop、Claude Code、OpenAI Codex 的接入向导（随界面语言切换）：选中客户端即可看到它的配置文件位置、连接地址、可直接粘贴的配置片段与接入说明，并可一键复制。

## HTTP 端点（MCP Streamable HTTP）

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/`、`/mcp` | JSON-RPC 2.0 入口（`Accept: application/json`）；`initialize` 响应返回 `Mcp-Session-Id` |
| `GET` | `/`、`/mcp`、`/sse` | 升级为 SSE 长连接（`Accept: text/event-stream`）；服务端推送 + 15s 心跳 |
| `DELETE` | `/`、`/mcp` | 终止会话（取消在途任务） |
| `OPTIONS` | `*` | 仅当 `Origin` 命中白名单才回 CORS 头，否则无任何 CORS 头 |
| `GET` | `/health` | 健康检查（协议版本、只读状态、可见工具数） |

错误约定：**HTTP 状态码表达传输层语义**（400/404/405/406/413/415/429），**响应体始终是 MCP 标准错误对象** `{"jsonrpc":"2.0","id":null,"error":{"code":...,"message":...}}`。

| code | 含义 |
|---|---|
| `-32700` / `-32600` / `-32601` | 解析错误 / 非法请求 / 方法不存在 |
| `-32602` | 参数非法（含未知工具） |
| `-32603` | 内部错误（含引擎异常、内存不足） |
| `-32001` | 权限拒绝（会话未授权，或只读模式下调用 `vb_write_*`） |
| `-32002` | 任务超时（等待 UI 主线程超时） |
| `-32003` | 服务繁忙（UI 任务队列已满） |

## 架构

```
┌──────────────────────────────────────────────┐
│ [网络线程]  后台独立线程（mcp_http.cpp）        │
│  ├ accept / select / HTTP 解析                 │
│  ├ Streamable HTTP：POST / GET(SSE) / DELETE   │
│  ├ SSE 推送与心跳                              │
│  └ UiTask 投递 ──────────────┐                 │
│            ▲ 响应、超时、取消 │                 │
├──────────────────────────────┼─────────────────┤
│ [UI 主线程] VB Decompiler 消息循环              │
│  ├ 消息窗口 VBMCP_TASK_V1（VBMCP_WM_TASK）     │
│  │   └ task_pump_on_ui_thread()：执行 MCP 工具  │
│  │        └ g_engine(...) → VB Decompiler      │
│  ├ 设置窗体（服务设置 / 接入信息 双属性页）     │
│  └ 授权弹窗（首次 callTool，仅 UI 线程）       │
└──────────────────────────────────────────────┘
```

**线程模型（硬约束）**

1. 网络线程**只做传输层**：socket、HTTP、SSE；**绝不触碰 UI 控件、UI 对象或 Callback Engine**。
2. 所有工具调用经 `UiTask` 投递到 UI 主线程串行执行，规避 Callback Engine 的线程安全问题。
3. 每个任务支持**超时**（默认 30s）与**取消令牌**（客户端断开 / 超时 / 服务停止时置位，排队中的任务直接丢弃）。
4. **并发上限** `VBMCP_MAX_PENDING_TASKS = 8`：队列满立即返回 `429` + `-32003`，避免多个请求同时触发大项目反编译把 UI 卡死。
5. 跨线程状态用 `std::atomic` / 互斥量；日志写文件用临界区串行化。

详细设计与新增工具流程见 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)；改造逐项进度见 [docs/REFACTOR_PROGRESS.md](docs/REFACTOR_PROGRESS.md)。

## 第三方依赖

- **yyjson**（`third_party/yyjson`，MIT）：唯一的 JSON 依赖，所有 MCP 协议报文均由其构建/解析。
- **Winsock**（`ws2_32.lib`）：传输层。
- 其余（HTTP 解析、字符串与编码工具）为自实现，HTTP 解析器只处理 MCP 所需的最小子集。

## 限制

- 同一进程只能运行一个 vbmcp 实例。
- 启用状态不持久化：每次 VB Decompiler 重启后默认不启用。
- 不实现 v3.9 以前的全部命令（部分命令在现代版本的 VB Decompiler 中可能行为不同）。
- UI 侧任务串行执行：已进入引擎调用的任务无法被取消（引擎同步阻塞），取消令牌作用于排队阶段与结果回写前。
- 更完整的已知限制、取消边界与后续建议见 [docs/REFACTOR_PROGRESS.md](docs/REFACTOR_PROGRESS.md) 第六节。

## 文件结构

```
vbmcp/
├── vbmcp.sln / vbmcp.vcxproj     解决方案 + 项目（C++17 / Win32）
├── build.bat                     构建 + 部署脚本
├── README.md / README_EN.md      中英文文档
├── SECURITY.md                   安全模型与风险说明
├── VB Decompiler Plugin API.md   上游 SDK 参考（Plugin API 说明 + vbmcp 差异注记）
├── docs/
│   ├── DEVELOPMENT.md            开发文档（工具命名规范 / 架构 / 新增工具）
│   ├── REFACTOR_PROGRESS.md      改造进度记录
│   └── OPTIMIZATION_PLAN.md      优化方案（P0–P4）与实施记录
├── src/
│   ├── vbmcp.h                   公共头（配置 / 常量 / 全局状态 / SDK 封装）
│   ├── vbmcp_mem.hpp             C++ RAII 助手（SocketHandle / unique_ptr / ScopeGuard）
│   ├── vbmcp_ids.h              控件/资源 ID（由 res/vbmcp.rc 生成，勿手改）
│   ├── vbmcp.def                 导出函数定义
│   ├── util.cpp                  内存 / 分级日志 / 字符串与编码工具（UTF-8 ↔ GBK）
│   ├── config.cpp                注册表配置读写（Port/BindAddress/AllowWrite/CorsOrigins）
│   ├── i18n.h / i18n.cpp         gettext 风格翻译（_/_A/_F/_AF）+ zh.json 资源加载
│   ├── khash.h                   字符串哈希表（i18n 翻译表专用）
│   ├── vbdec_sdk.cpp             VB Decompiler Callback Engine 封装（含 SEH 保护）
│   ├── mcp_tools.h / mcp_tools.cpp  28 个 MCP 工具实现（read/write 分组 + 权限校验）
│   ├── mcp_rpc.cpp               JSON-RPC 2.0 分发 + 会话准入授权
│   ├── mcp_http.cpp              Streamable HTTP / SSE + 后台网络线程 + UI 任务队列
│   ├── options_dialog.cpp        设置窗体（双语 UI + 写入权限 + 局域网警告 + 接入向导）
│   └── plugin_main.cpp           插件入口（导出函数 + DllMain）
├── res/vbmcp.rc                资源脚本：语言包 + **设置窗体 DIALOGEX 模板** + 版本信息
├── lang_res/zh.json             中文语言包（RCDATA 嵌入 DLL，随源码提交）
├── tools/
│   ├── extract_i18n.py          词条提取 / 语言包同步 / 待翻译清单
│   ├── gen_ids_header.py        由 res/vbmcp.rc 生成/校验 src/vbmcp_ids.h
│   ├── check_all.bat            一键校验：ID + i18n + 自测 + 构建
│   ├── selftest/                无宿主单元自测（编码桥 / i18n / 工具 schema）
│   └── ui_preview/              渲染设置窗体并截图（UI 回归）
├── third_party/yyjson/           JSON 库（以 C 编译）
└── res/                          Windows 资源（版本信息）
```

## 许可证

本项目使用 MIT 许可证。

VB Decompiler Plugin SDK 由 DotFix Software 提供，使用其原始许可证（见 plugins sdk 目录）。