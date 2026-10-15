@echo off
REM ============================================
REM  VB Decompiler MCP Server Plugin - Build & Deploy
REM  Run as Administrator when deploying to plugins folder
REM ============================================
setlocal enabledelayedexpansion

set "PROJECT_DIR=%~dp0"
set "SOLUTION=%PROJECT_DIR%vbmcp.sln"

REM VB Decompiler 安装目录（安装位置不同时请修改）
set "VBDEC_DIR=E:\Ollydbg\VB Decompiler Pro v11.5"
set "PLUGIN_DIR=%VBDEC_DIR%\Plugins"

set "CONFIG=Release"
set "PLATFORM=Win32"
set "DLL_NAME=vbmcp.dll"

REM MSBuild 路径（VS 2019/2022，默认安装位置）
set "MSBUILD_VS2019=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe"
set "MSBUILD_VS2022=C:\Program Files (x86)\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\amd64\MSBuild.exe"

if exist "%MSBUILD_VS2022%" (
    set "MSBUILD=%MSBUILD_VS2022%"
) else if exist "%MSBUILD_VS2019%" (
    set "MSBUILD=%MSBUILD_VS2019%"
) else (
    echo [ERROR] Cannot find MSBuild. Install Visual Studio 2019/2022 first.
    pause
    exit /b 1
)

echo ===========================================
echo   VB Decompiler MCP Server Plugin
echo   %CONFIG% ^| %PLATFORM%
echo ===========================================
echo.

echo [1/2] Building...
"%MSBUILD%" "%SOLUTION%" /p:Configuration=%CONFIG% /p:Platform=%PLATFORM% /t:Build /nologo /v:minimal
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b %ERRORLEVEL%
)
echo [OK] Build succeeded
echo.

set "SRC_DLL=%PROJECT_DIR%%PLATFORM%\%CONFIG%\%DLL_NAME%"
if not exist "%SRC_DLL%" (
    echo [ERROR] Build output not found: %SRC_DLL%
    pause
    exit /b 1
)
echo Built: %SRC_DLL%
copy /Y "%SRC_DLL%" "%PLUGIN_DIR%\"
echo.

echo.
echo [2/2] Usage:
echo    POST   http://127.0.0.1:3023/mcp   JSON-RPC 2.0 (MCP Streamable HTTP)
echo    GET    http://127.0.0.1:3023/mcp   SSE stream (Accept: text/event-stream)
echo    DELETE http://127.0.0.1:3023/mcp   terminate session
echo    GET    http://127.0.0.1:3023/health
echo.
echo  Security model:
echo    - listens on 127.0.0.1 only (LAN exposure is opt-in + high risk warning)
echo    - read-only by default; vb_write_* need "Allow MCP clients to write"
echo    - first tools/call of a session asks for user confirmation
echo.
echo  Configure via Settings dialog (Plugins -^> VB Decompiler MCP Server)
echo  or registry key:
echo    HKCU\SOFTWARE\VB and VBA Program Settings\VB Decompiler\MCP
echo      Port          REG_DWORD
echo      BindAddress   REG_SZ
echo      AllowWrite    REG_DWORD
echo      CorsOrigins   REG_SZ
echo.
echo  Debug log (optional):
echo    set VBMCP_DEBUG_LOG=1           (write to %%TEMP%%\vbmcp.log)
echo    set VBMCP_LOG_LEVEL=3           (0=ERROR 1=WARN 2=INFO 3=DEBUG)
echo    set VBMCP_TASK_TIMEOUT_MS=30000 (per MCP task timeout)
echo.
echo  Docs: README.md / SECURITY.md / docs\DEVELOPMENT.md / docs\REFACTOR_PROGRESS.md
echo.

endlocal
pause
