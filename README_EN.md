# VB Decompiler MCP Server Plugin
(https://www.vb-decompiler.org/plugins_sdk.htm)

[English](README_EN.md) | [简体中文](README.md)

A plugin for [VB Decompiler](https://www.vb-decompiler.org) written in C, running as an [MCP (Model Context Protocol)](https://modelcontextprotocol.io) server. It allows AI clients to read and modify the code and internal data structures produced by VB Decompiler through a JSON-RPC 2.0 interface.

## Features

- **27 MCP tools**: covering project structure (Forms, Modules, Functions), code read/write, and metadata queries. `list_*` tools support `limit`/`offset` pagination.
- **Bilingual UI (EN/ZH)**: menus, settings dialog, AI client setup guides, help text, and even the MCP `tools/list` descriptions and tool error messages all follow the VB Decompiler host language (registry `CustomLanguage` = `ChineseSimplified` → Simplified Chinese; any other value / missing → English).
- **Pure C**: single-threaded, event-driven, built on Winsock. No C++/STL/RapidJSON dependencies.
- **Zero third-party libraries**: self-contained minimal JSON parser, dynamic string buffer, and HTTP server.
- **MCP protocol compatible**: Streamable HTTP + JSON-RPC 2.0 + SSE placeholder. Protocol version 2025-03-26 (2024-11-05 also supported).

## MCP Tool List

All tools share a `vb_` prefix to avoid name collisions with other MCP servers (e.g. iida).

| Tool | Description |
|---|---|
| `vb_get_project_info` | Project metadata (path, native/pcode, compiler type, packed state) |
| `vb_get_project_text` / `vb_set_project_text` | Full project text read/write |
| `vb_clear_buffers` | Clear VB Decompiler internal buffers |
| `vb_list_forms` / `vb_get_form` / `vb_set_form` / `vb_rename_form` | Form CRUD |
| `vb_list_modules` / `vb_get_module` / `vb_set_module` | Module CRUD |
| `vb_list_module_functions` / `vb_get_function` / `vb_set_function` / `vb_rename_function` | Function CRUD (incl. disassembly view) |
| `vb_get_sub_main` / `vb_set_sub_main` | Sub Main entry point |
| `vb_get_active_text` / `vb_set_active_text` / `vb_goto_line` | Active window editing / jump to line |
| `vb_get_active_disasm_text` / `vb_set_active_disasm_text` | Active disassembly window editing |
| `vb_get_active_coords` | Active view coordinates |
| `vb_set_status` | Status bar control |
| `vb_set_options` | Control the "Parse stack parameters" and "Procedure analyzer and optimizer" checkboxes |
| `vb_list_frx_icons` | FRX icon resource enumeration |
| `vb_update_all` | Commit all changes and refresh the GUI |

Pagination: `vb_list_forms` / `vb_list_modules` / `vb_list_module_functions` / `vb_list_frx_icons` accept `limit` (default 100, max 10000) and `offset` (default 0), and return `{count, total_count, offset, limit, has_more, next_offset, ...}`.

## Quick Start

### Build

Requires: Visual Studio 2019/2022 + Windows 10+.

```cmd
build.bat
```

Output: `Win32\Release\vbmcp.dll`. The script also attempts to deploy it automatically to VB Decompiler's `Plugins` folder (requires administrator privileges).

### Install

Copy `vbmcp.dll` into the `Plugins` folder under the VB Decompiler installation directory (create the folder on first install):

```
C:\Program Files\VB Decompiler\Plugins\vbmcp.dll
```

### Running

After opening VB Decompiler, the plugin does **not** start the MCP server automatically — this is a deliberate design choice: it guarantees that at the moment the server starts, the `VBDecompilerPluginLoad` re-entry has completed and the Callback Engine (`g_engine`) is valid. To enable it manually:

1. Open the settings dialog from the *Plugins → VB Decompiler MCP Server* menu;
2. Check **Enable MCP Server** and click *OK*.

Listens on `127.0.0.1:3023` by default. While disabled, no listening socket exists.

### Configuration

Change settings via the settings dialog (*Plugins → VB Decompiler MCP Server* menu), or edit the registry directly:

```
HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\MCP
```

| Name | Type | Description | Default |
|---|---|---|---|
| `Port` | `REG_DWORD` | Listen port (1–65535) | `3023` |
| `BindAddress` | `REG_SZ` | Bind IP | `127.0.0.1` |

- Missing or out-of-range fields are filled with defaults and **written back to the registry automatically**, simplifying first-time setup.
- Changing settings in the dialog and clicking *OK* writes the registry and starts/stops/restarts the MCP server immediately (no VB Decompiler restart needed).
- `EnableMcp` is a **legacy field**: older versions used it to persist the enabled state; the current version no longer reads or writes it (old values are ignored). Whether the server runs is determined solely by the settings dialog checkbox, and it defaults to disabled on every VB Decompiler start.
- If you edit the registry directly, open the settings dialog once from the menu (or restart VB Decompiler) for the plugin to re-read the values.

### UI Language

The UI language follows the VB Decompiler host setting, read from:

```
HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\Options\CustomLanguage
```

`ChineseSimplified` → Simplified Chinese; any other value / missing → English. The value is re-read every time the settings dialog opens.

Coverage:

- Plugins menu item text, all settings dialog controls, MessageBoxes, and help text;
- AI client setup guides (config snippets + step-by-step instructions, EN and ZH variants for 8 clients);
- MCP protocol layer: `tools/list` descriptions and tool error messages (UTF-8 JSON, consistent with HTTP `application/json`).

### Debug Logging

Set the environment variable `VBMCP_DEBUG_LOG=1` and all plugin logs are written to `%TEMP%\vbmcp.log`:

```cmd
set VBMCP_DEBUG_LOG=1
"%ProgramFiles%\VB Decompiler\VB Decompiler.exe"
```

Unset the variable (or set it to 0) to disable logging.

### Testing

```cmd
curl http://127.0.0.1:3023/health
```

Returns:
```json
{"status":"ready","server":{"name":"VB Decompiler MCP Server","version":"1.0.0","protocolVersion":"2025-03-26"},"instances":[{"name":"VB Decompiler.exe","arch":"Win32","bits":32,"path":"C:\\...","pid":1234,"local":true,"state":"ready","tools_count":27}],"ready":1}
```

### MCP Client Configuration

Example for CodeBuddy / Claude Desktop (Streamable HTTP, key name `type`):

```json
{
  "mcpServers": {
    "vbmcp": {
      "type": "http",
      "url": "http://127.0.0.1:3023"
    }
  }
}
```

The AI client dropdown on the right side of the settings dialog ships with complete setup guides for CodeBuddy, VS Code, Cursor, Trae, WorkBuddy, Claude Desktop, Claude Code, and OpenAI Codex (switching along with the UI language). Click *Copy config* to place a config snippet with the current URL on the clipboard.

## HTTP Endpoints

| Method | Path | Description |
|---|---|---|
| POST | `/` or `/mcp` | JSON-RPC 2.0 entry point (MCP `tools/call`, etc.) |
| GET  | `/` | SSE placeholder (`event: endpoint` + `data: /`) |
| GET  | `/sse` | Same as above (legacy client compatibility) |
| GET  | `/health` | Health check (incl. tool count, protocol version) |
| GET  | `/debug/schema` | Returns the full tools/list schema (debug) |
| GET  | `/debug/log` | Recent request log ring buffer (debug) |
| POST | `/debug/echo` | Echoes the POST body verbatim (debug) |
| OPTIONS | `*` | CORS preflight, returns 204 |

## Architecture

```
┌───────────────────────────────────────────────┐
│VB Decompiler host process (GUI)               │
│ ├─ VBDecompilerPluginLoad entry point         │
│ ├─ Message loop (WSAAsyncSelect dispatch)     │
│     ├─ WSAStartup (process refcount >= 1)     │
│     ├─ Hidden window VBMCP_SERVER_V1          │
│     │   ├─ WM_LISTEN -> accept                │
│     │   ├─ WM_CLIENT -> recv/flush/close      │
│     │   └─ 5s health-check timer              │
│     ├─ HTTP request parsing (recv_buf)        │
│     ├─ JSON-RPC dispatch                      │
│     └─ mcp_dispatch_tool                      │
│          └─ g_engine(...) -> VB Decompiler    │
└───────────────────────────────────────────────┘
```

**Threading model**: fully single-threaded. All socket IO, HTTP parsing, JSON parsing, and VB Decompiler Callback Engine calls run on the VB Decompiler main thread (the message-loop thread), avoiding Callback Engine thread-safety issues.

### Key Design Decisions

1. **Synchronous HTTP handling**: VB Decompiler's Callback Engine can only be called from the main thread, so requests are processed synchronously to avoid concurrency issues. MCP requests are request-response in nature, with no concurrency requirements.
2. **No server auto-start**: on VB Decompiler startup the plugin only reads config and opens the settings dialog — it never starts the server. The user must check "Enable" in the settings dialog and click "OK".
3. **WSAStartup once**: the plugin keeps the Winsock process-level reference count at ≥ 1, preventing other VB Decompiler modules from zeroing the count via WSACleanup and closing all sockets.
4. **Health-check timer**: every 5s it checks whether the listening socket was closed by an external WSACleanup, and rebuilds it if so (avoiding ports that "vanish").
5. **SO_REUSEADDR**: allows re-binding the same port while it is in TIME_WAIT.
6. **Dual-encoding string lookup**: `vbmcp_t()` returns GBK (for ANSI controls such as `MessageBoxA` / `SetDlgItemTextA`), while `vbmcp_t_utf8()` returns UTF-8 (for MCP JSON-RPC / HTTP `application/json` responses). Both use parallel caches with zero overhead after the first access.
7. **Memory**: all memory uses CRT malloc/free, reclaimed uniformly when the VB Decompiler process exits.

## Third-Party Dependencies

None. All code is self-contained:

- **HTTP parsing**: hand-written (~200 lines)
- **JSON parsing & serialization**: hand-written (~350 lines, C99)
- **Dynamic strings**: hand-written strbuf_t (~120 lines)
- **Winsock wrapper**: standard ws2_32.dll

## Limitations

- Only one vbmcp instance per process (hidden window class name `VBMCP_SERVER_V1`, avoiding multi-instance conflicts within the same process)
- Port / bind IP changes take effect immediately after clicking *OK* in the settings dialog; if you edit the registry directly, click the menu once to trigger a `PluginLoad` re-read (or restart VB Decompiler)
- The enabled state is not persisted: it defaults to disabled after every VB Decompiler restart and must be re-checked in the settings dialog
- Not all commands prior to v3.9 are implemented (some commands may behave differently in modern versions of VB Decompiler)

## File Structure

```
vbmcp/
├── vbmcp.sln / vbmcp.vcxproj    Solution + project
├── schema_dump.vcxproj          Debug tool: offline tools/list schema dump
├── build.bat                    Build + deploy script
├── README.md                    Documentation (Chinese)
├── README_EN.md                 Documentation (English)
├── src/
│   ├── vbmcp.h                  Common header (config/constants/global state)
│   ├── vbmcp.def                Export definitions
│   ├── strbuf.h / strbuf.c      Dynamic string buffer
│   ├── json.h / json.c          Minimal JSON parser
│   ├── util.c                   Memory/log/string utilities
│   ├── config.c                 Registry config read/write (Port/BindAddress)
│   ├── lang.h / lang.c          Localization resource table (EN/ZH) + CustomLanguage reader
│   ├── vbdec_sdk.c              VB Decompiler Callback Engine wrapper
│   ├── mcp_tools.h / mcp_tools.c  27 MCP tool implementations
│   ├── mcp_rpc.c                JSON-RPC 2.0 dispatch
│   ├── mcp_http.c               HTTP server + event loop
│   ├── options_dialog.c         Settings dialog (bilingual UI + AI client guides)
│   └── plugin_main.c            Plugin entry points (exports + DllMain)
└── res/
    ├── vbmcp.rc                 Windows resources (version info)
    └── resource.h                Resource header
```

## License

This project is released under the MIT License.

The VB Decompiler Plugin SDK is provided by DotFix Software and is used under its original license (see the plugins sdk directory).
