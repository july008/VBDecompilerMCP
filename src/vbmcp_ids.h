// vbmcp_ids.h - GENERATED FILE, do not edit by hand.
//
// Source of truth: the IDC_/IDD_ defines in the resource script
//   res/vbmcp.rc       (hand-written layout)
//   res/resource.h     (generated when vbmcp.rc is edited in the VS resource editor)
// Regenerate after changing an ID:  python tools/gen_ids_header.py
// Verify consistency:               python tools/gen_ids_header.py --check
//
// The .rc does not include this header on purpose: rc.exe reports RC1004 when
// these macros come from an included header and are used inside a DIALOGEX
// block. Defining them in the resource script itself is stable, so they are
// mirrored here.
//
// Keep the resource script ASCII-only and use line comments only: the rc.exe
// lexer is not DBCS-safe and treats apostrophes inside comments as string
// delimiters.

#ifndef VBMCP_IDS_H
#define VBMCP_IDS_H
#define IDC_CHK_ENABLE           1001
#define IDC_TXT_BIND             1002
#define IDC_SPIN_PORT            1003
#define IDC_EDIT_PORT            1004
#define IDC_LBL_STATUS           1005
#define IDC_CBO_AI               1006
#define IDC_BTN_RESTORE          1009
#define IDC_BTN_HELP             1010
#define IDC_BTN_OK               1011
#define IDC_BTN_CANCEL           1012
#define IDC_CHK_ALLOW_WRITE      1013
#define IDC_CHK_LAN_ADVANCED     1014
#define IDC_LBL_BIND             1015
#define IDC_LBL_PORT             1016
#define IDC_LBL_AI               1017
#define IDC_TAB                  1020
#define IDC_GRP_SERVICE          1021
#define IDC_GRP_SECURITY         1022
#define IDC_LBL_SRV_STATUS       1023
#define IDC_LBL_MODE             1024
#define IDC_LBL_MODE_VAL         1025
#define IDC_TXT_SEC_TIP          1026
#define IDC_TXT_SRV_HINT         1027
#define IDC_LBL_PATH             1028
#define IDC_TXT_PATH             1029
#define IDC_LBL_URL              1031
#define IDC_TXT_URL              1032
#define IDC_LBL_SNIPPET          1034
#define IDC_TXT_SNIPPET          1035
#define IDC_LBL_STEPS            1036
#define IDC_TXT_STEPS            1037
#define IDD_VBMCP_OPTIONS        2000
#define IDD_PAGE_SERVER          2001
#define IDD_PAGE_ACCESS          2002

#endif // VBMCP_IDS_H
