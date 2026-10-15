/*
* config.cpp - 插件配置读写（Windows 注册表）
*
* 存储位置（与 VB Decompiler 自身的 Options 注册表项同级、互不干扰）：
*   HKEY_CURRENT_USER\SOFTWARE\VB and VBA Program Settings\VB Decompiler\MCP
*
* 值：
 *   Port         REG_DWORD 1..65535             (默认 3023)
 *   BindAddress  REG_SZ                         (默认 127.0.0.1)
 *   AllowWrite   REG_DWORD 0/1                  (默认 0 = 只读模式)
 *   CorsOrigins  REG_SZ 逗号分隔可信 origin 列表 (默认空 = 不返回任何 CORS 头)
 *
*   EnableMcp    REG_SZ  （**历史遗留字段**，新版不再读写 —— 服务启用状态
*                       由用户在 Plugins 设置窗体里点"确定"即时生效，
*                       每次 VB Decompiler 启动默认禁用。旧值保留在注册表
*                       里但被忽略，迁移无需手工清理。）
*
* UI 语言通过单独的子键读取（VB Decompiler 自己写入）：
*   HKEY_CURRENT_USER\SOFTWARE\VB and VBA Program Settings\VB Decompiler\Options\CustomLanguage
* 详见 lang.cpp。
*
* 设计原则：
*   - 不存在 / 字段缺失时返回默认值，不抛错。
*   - RegOpenKey 在缺失子键时 CreateKey 自动建立，简化 "首次保存" 流程。
*   - vbmcp_config_load / save 在 DLL 主线程中调用，无须并发保护。
*/
#include "vbmcp.h"
#include <stdio.h>

/* 主键：与 VB Decompiler 自身的 Options 项同级，作为 MCP 子键 */
#define VBMCP_REG_ROOT  HKEY_CURRENT_USER
#define VBMCP_REG_KEY   "SOFTWARE\\VB and VBA Program Settings\\VB Decompiler\\MCP"
#define VBMCP_VAL_ENABLE  "EnableMcp"
#define VBMCP_VAL_PORT    "Port"
#define VBMCP_VAL_BIND    "BindAddress"
#define VBMCP_VAL_ALLOW_WRITE "AllowWrite"
#define VBMCP_VAL_CORS    "CorsOrigins"

/* 读 REG_SZ 到用户缓冲（不写默认值；仅在类型不符或失败时清空）。 */
static int reg_read_sz(HKEY hKey, const char *value, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    DWORD type = 0, cb = (DWORD)cap;
    LSTATUS st = RegQueryValueExA(hKey, value, NULL, &type, (LPBYTE)out, &cb);
    if (st != ERROR_SUCCESS) return 0;
    if (type != REG_SZ && type != REG_EXPAND_SZ) { out[0] = '\0'; return 0; }
    /* 确保 NUL 终止（RegQueryValueEx 通常会，但保险起见） */
    if (cb > 0 && cb <= (DWORD)cap) out[cb - 1] = '\0';
    else out[cap - 1] = '\0';
    return 1;
}

static int reg_read_dw(HKEY hKey, const char *value, DWORD *out)
{
    if (!out) return 0;
    DWORD type = 0, cb = sizeof(*out);
    LSTATUS st = RegQueryValueExA(hKey, value, NULL, &type, (LPBYTE)out, &cb);
    if (st != ERROR_SUCCESS || type != REG_DWORD || cb != sizeof(*out)) {
        *out = 0;
        return 0;
    }
    return 1;
}

static int reg_write_sz(HKEY hKey, const char *value, const char *s)
{
    if (!s) s = "";
    DWORD cb = (DWORD)strlen(s) + 1;
    return RegSetValueExA(hKey, value, 0, REG_SZ, (const BYTE *)s, cb) == ERROR_SUCCESS;
}

static int reg_write_dw(HKEY hKey, const char *value, DWORD dw)
{
    return RegSetValueExA(hKey, value, 0, REG_DWORD, (const BYTE *)&dw, sizeof(dw)) == ERROR_SUCCESS;
}

/* 打开主键（不存在则创建）。返回 RegCloseKey 由调用方执行。 */
static HKEY open_key(BOOL create)
{
    HKEY hKey = NULL;
    if (create) {
        DWORD disp = 0;
        if (RegCreateKeyExA(VBMCP_REG_ROOT, VBMCP_REG_KEY,
                            0, NULL, REG_OPTION_NON_VOLATILE,
                            KEY_READ | KEY_WRITE, NULL, &hKey, &disp) != ERROR_SUCCESS) {
            return NULL;
        }
    } else {
        if (RegOpenKeyExA(VBMCP_REG_ROOT, VBMCP_REG_KEY, 0,
                          KEY_READ, &hKey) != ERROR_SUCCESS) {
            return NULL;
        }
    }
    return hKey;
}

void vbmcp_config_load(int *port, char *bind_out, size_t bind_cap, int *allow_write)
{
    if (port)   *port   = VBMCP_DEFAULT_PORT;
    if (allow_write) *allow_write = VBMCP_DEFAULT_ALLOW_WRITE;
    if (bind_out && bind_cap) {
        _snprintf_s(bind_out, bind_cap, _TRUNCATE, "%s", VBMCP_DEFAULT_BIND);
    }

    HKEY hKey = open_key(FALSE);
    if (!hKey) {
        vbmcp_config_save(port   ? *port   : VBMCP_DEFAULT_PORT,
                          (bind_out && bind_out[0]) ? bind_out : VBMCP_DEFAULT_BIND,
                          allow_write ? *allow_write : VBMCP_DEFAULT_ALLOW_WRITE);
        return;
    }

    char val[256] = {0};
    BOOL need_rewrite = FALSE;

    DWORD dw = 0;
    if (reg_read_dw(hKey, VBMCP_VAL_PORT, &dw) && port) {
        if (dw > 0 && dw < 65536) *port = (int)dw;
        else need_rewrite = TRUE;  /* 越界值 → 写回 */
    } else {
        need_rewrite = TRUE;
    }

    if (reg_read_sz(hKey, VBMCP_VAL_BIND, val, sizeof(val)) && bind_out && bind_cap) {
        _snprintf_s(bind_out, bind_cap, _TRUNCATE, "%s", val);
    } else {
        need_rewrite = TRUE;
    }

    /* AllowWrite：缺失时用默认（只读）；不触发 rewrite，避免老用户被强制写入新键 */
    if (reg_read_dw(hKey, VBMCP_VAL_ALLOW_WRITE, &dw) && allow_write) {
        *allow_write = (dw != 0) ? 1 : 0;
    }

    RegCloseKey(hKey);

    /* 有任何缺失 / 越界 / 不可识别值 → 把当前（已用默认值填补的）结果写回注册表 */
    if (need_rewrite) {
        vbmcp_config_save(port   ? *port   : VBMCP_DEFAULT_PORT,
                          (bind_out && bind_out[0]) ? bind_out : VBMCP_DEFAULT_BIND,
                          allow_write ? *allow_write : VBMCP_DEFAULT_ALLOW_WRITE);
    }

}

void vbmcp_config_save(int port, const char *bind, int allow_write)
{
   
    HKEY hKey = open_key(TRUE);
    if (!hKey) {
        vbmcp_log("config save: create/open key failed");
        return;
    }

    /* 不写 EnableMcp 字段 —— 见上方说明 */
    reg_write_dw(hKey, VBMCP_VAL_PORT, (DWORD)(port > 0 ? port : VBMCP_DEFAULT_PORT));
    reg_write_sz(hKey, VBMCP_VAL_BIND, (bind && *bind) ? bind : VBMCP_DEFAULT_BIND);
    reg_write_dw(hKey, VBMCP_VAL_ALLOW_WRITE, allow_write ? 1u : 0u);

    RegCloseKey(hKey);
    vbmcp_log("config saved: port=%d, bind=%s, allow_write=%d",
              port, bind ? bind : VBMCP_DEFAULT_BIND, allow_write ? 1 : 0);
}

/* 可信 CORS origin 列表（逗号分隔）。空串 = 未配置。允许 "*" 会被忽略。 */
void vbmcp_config_get_cors_origins(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';

    HKEY hKey = open_key(FALSE);
    if (!hKey) return;
    reg_read_sz(hKey, VBMCP_VAL_CORS, out, cap);
    RegCloseKey(hKey);
}
