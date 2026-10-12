/*
 * lang.h - 多语言切换（EN / ZH）
 *
 * 决策依据：VB Decompiler 的注册表
 *   HKEY_CURRENT_USER\SOFTWARE\VB and VBA Program Settings\VB Decompiler\Options\CustomLanguage
 *
 *   值 = "ChineseSimplified"   →  zh_CN
 *   值 = 任何其他 / 缺失       →  en
 *
 * 所有用户可见文本（菜单项、设置窗体、MessageBox、help）均通过
 * `vbmcp_t(T_xxx)` 获取当前语言对应的字符串。AI 客户端名（CodeBuddy
 * / Claude Desktop 等产品名）保留英文，不做翻译。
 */
#ifndef VBMCP_LANG_H
#define VBMCP_LANG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VBMCP_LANG_EN = 0,  /* 默认：英文 */
    VBMCP_LANG_ZH = 1,  /* 简体中文 */
} vbmcp_lang_t;

/* 文本资源键。每条在 lang.c 中提供 en + zh 两个变体。 */
enum {
    T_PLUGIN_NAME = 0,       /* 插件名（写到 Plugins 菜单项 + MessageBox 标题） */
    T_PLUGIN_AUTHOR,         /* 作者信息 */
    T_MENU_SETTINGS,         /* 菜单项文字（备用，VB Decompiler 直接读 T_PLUGIN_NAME） */
    T_DLG_TITLE,             /* 设置窗体标题 */
    T_CHK_ENABLE,            /* "启用 MCP 服务器" 复选框 */
    T_LBL_BIND,              /* "绑定 IP 地址:" 标签 */
    T_LBL_PORT,              /* "端口号:" 标签 */
    T_LBL_STATUS_RUNNING,    /* "运行中" */
    T_LBL_STATUS_STOPPED,    /* "已停止" */
    T_LBL_AI,                /* "AI 客户端:" 标签 */
    T_BTN_COPY,              /* "复制配置" 按钮 */
    T_BTN_RESTORE,           /* "恢复默认设置" 按钮 */
    T_BTN_HELP,              /* "帮助" 按钮 */
    T_BTN_OK,                /* "确定" */
    T_BTN_CANCEL,            /* "取消" */
    T_BTN_COPIED,            /* 复制后按钮临时文字 */
    T_GUIDE_BRACKET_OPEN,    /* 客户端名前左括号：英文 [ / 中文 【 */
    T_GUIDE_BRACKET_CLOSE,   /* 客户端名前右括号：英文 ] / 中文 】 */
    T_GUIDE_PREFACE,         /* 顶部提醒：xxx (含 URL 显示) */
    T_GUIDE_NOTE,            /* "注意：" 前缀 */
    T_GUIDE_OPS_HEADER,      /* "操作步骤：" */
    T_GUIDE_CONFIG_HEADER,   /* "配置片段 (可点右侧\"复制配置\"):" */
    T_ERR_PORT_IN_USE,       /* 端口被占用 */
    T_ERR_CLIPBOARD,         /* 无法访问剪贴板 */
    T_ERR_REG_CLASS,         /* 注册窗口类失败 */
    T_ERR_CREATE_DLG,        /* 创建设置窗体失败 */
    T_HELP_TITLE,            /* 帮助窗体标题 */
    T_HELP_BODY,             /* 帮助正文（多行） */
    T_LOG_RESTORED,          /* 日志：已恢复默认 */
    T_LOG_SETTINGS_OK,       /* 日志：设置已应用 */
    T_LOG_LANG,              /* 日志：当前语言 */

    /* ---- MCP 工具文本（mcp_tools.c 使用）----
     * 这些文本经 JSON-RPC / HTTP "application/json"（即 UTF-8）返回给 AI 客户端，
     * 必须用 vbmcp_t_utf8() 取 **UTF-8** 串；vbmcp_t() 的 GBK 输出只适用于
     * ANSI Win32 控件（MessageBoxA / SetDlgItemTextA / AppendMenuA 等）。 */

    /* 工具错误消息（make_error / snprintf 格式化后返回） */
    T_ERR_OOM,                       /* "内存不足" */
    T_ERR_NO_PROJECT,                /* "当前没有打开的项目" */
    T_ERR_MISSING_PROJECT_TEXT,      /* "缺少 project_text 参数" */
    T_ERR_PROJECT_TEXT_TOO_LARGE,    /* project_text 超 16MB 上限（%zu = 实际字节） */
    T_ERR_MISSING_ARGS,              /* "缺少参数对象" */
    T_ERR_BAD_FORM_INDEX,            /* "缺少或非法的 form_index" */
    T_ERR_CANNOT_READ_FORM,          /* "无法读取表单内容" */
    T_ERR_MISSING_FORM_TEXT,         /* "缺少 form_text" */
    T_ERR_MISSING_NEW_NAME,          /* "缺少 new_name" */
    T_ERR_NATIVE_RENAME_FORM,        /* rename_form 拒绝 native 编译项目 */
    T_ERR_BAD_MODULE_INDEX,          /* "缺少或非法的 module_index" */
    T_ERR_MISSING_DECLARATIONS,      /* "缺少 declarations" */
    T_ERR_MISSING_STRING_REFERENCES, /* "缺少 string_references" */
    T_ERR_BAD_MODULE_FUNCTION_INDEX, /* "缺少或非法的 module_index/function_index" */
    T_ERR_MISSING_CODE,              /* "缺少 code" */
    T_ERR_NATIVE_RENAME_FUNCTION,    /* rename_function 拒绝 native 编译项目 */
    T_ERR_MISSING_TEXT,              /* "缺少 text" */
    T_ERR_BAD_LINE,                  /* "缺少或非法的 line" */
    T_ERR_EMPTY_TOOL_NAME,           /* "工具名为空" */
    T_ERR_PROJECT_NOT_LOADED,        /* 项目未加载 + 恢复方法（%s = 工具名） */
    T_ERR_UNKNOWN_TOOL,              /* "未知工具: %s"（%s = 工具名） */

    /* tools/list schema 描述（append_local_tools） */
    T_DESC_LIMIT,                    /* limit 参数："最多返回多少条" */
    T_DESC_OFFSET,                   /* offset 参数："起始偏移" */
    T_DESC_PROJECT_INFO,             /* vb_get_project_info 描述 */
    T_DESC_PROJECT_TEXT,             /* vb_get_project_text 描述 */
    T_DESC_SET_PROJECT_TEXT,         /* vb_set_project_text 描述 */
    T_DESC_PARAM_PROJECT_TEXT,       /* project_text 参数描述 */
    T_DESC_CLEAR_BUFFERS,            /* vb_clear_buffers 描述 */
    T_DESC_LIST_FORMS,               /* vb_list_forms 描述 */
    T_DESC_GET_FORM,                 /* vb_get_form 描述 */
    T_DESC_PARAM_FORM_INDEX,         /* form_index 参数描述 */
    T_DESC_SET_FORM,                 /* vb_set_form 描述 */
    T_DESC_RENAME_FORM,              /* vb_rename_form 描述 */
    T_DESC_LIST_MODULES,             /* vb_list_modules 描述 */
    T_DESC_GET_MODULE,               /* vb_get_module 描述 */
    T_DESC_SET_MODULE,               /* vb_set_module 描述 */
    T_DESC_LIST_MODULE_FUNCTIONS,    /* vb_list_module_functions 描述 */
    T_DESC_GET_FUNCTION,             /* vb_get_function 描述 */
    T_DESC_SET_FUNCTION,             /* vb_set_function 描述 */
    T_DESC_RENAME_FUNCTION,          /* vb_rename_function 描述 */
    T_DESC_GET_SUB_MAIN,             /* vb_get_sub_main 描述 */
    T_DESC_SET_SUB_MAIN,             /* vb_set_sub_main 描述 */
    T_DESC_GET_ACTIVE_TEXT,          /* vb_get_active_text 描述 */
    T_DESC_SET_ACTIVE_TEXT,          /* vb_set_active_text 描述 */
    T_DESC_GET_ACTIVE_DISASM_TEXT,   /* vb_get_active_disasm_text 描述 */
    T_DESC_SET_ACTIVE_DISASM_TEXT,   /* vb_set_active_disasm_text 描述 */
    T_DESC_GOTO_LINE,                /* vb_goto_line 描述 */
    T_DESC_PARAM_LINE,               /* line 参数描述 */
    T_DESC_GET_ACTIVE_COORDS,        /* vb_get_active_coords 描述 */
    T_DESC_SET_STATUS,               /* vb_set_status 描述 */
    T_DESC_SET_OPTIONS,              /* vb_set_options 描述 */
    T_DESC_PARAM_ONOFF,              /* stack_check/analyzer_check 参数描述 */
    T_DESC_LIST_FRX_ICONS,           /* vb_list_frx_icons 描述 */
    T_DESC_UPDATE_ALL,               /* vb_update_all 描述 */

    T_COUNT                  /* 哨兵 */
};

/* 重新从注册表读取语言（用户在设置窗体打开前可能改了 CustomLanguage）。
 * 同时缓存到模块级静态变量。 */
vbmcp_lang_t vbmcp_lang_refresh(void);

/* 取当前缓存的语言，不读注册表。 */
vbmcp_lang_t vbmcp_lang_current(void);

/* 返回键 key 在当前语言下的字符串。空键返回空串。
 * 生命周期：返回的指针指向 lang.c 内部静态表，无须 free。
 * 注意：返回 **GBK** 字节序列，只适用于 ANSI Win32 控件
 *（MessageBoxA / SetDlgItemTextA / AppendMenuA 等）。 */
const char *vbmcp_t(int key);

/* 返回键 key 在当前语言下的 **UTF-8** 字符串（不做 GBK 转换）。
 * 用于 MCP JSON-RPC 响应（HTTP 层 application/json 即 UTF-8）等
 * 非 ANSI 控件场景。空键返回空串，缺失时退回另一种语言。
 * 生命周期：返回的指针指向 lang.c 内部静态表，无须 free。 */
const char *vbmcp_t_utf8(int key);

/* 通用 UTF-8 → ANSI 代码页（中文 Windows = GBK）转换。
 * 用于把源码中的 UTF-8 字符串（不在 lang.c 资源表里的）
 * 转成 GBK，再与 vbmcp_t() 输出拼接给 ANSI 控件。
 * 返回 vbmcp_alloc 申请的新字符串（调用方 vbmcp_free）；
 * ASCII 快速路径直接 strdup；失败回退到 vbmcp_strdup(src)。 */
char *vbmcp_str_to_gbk(const char *src);

#ifdef __cplusplus
}
#endif

#endif /* VBMCP_LANG_H */
