// claude_chat.rc で使うリソース ID
#pragma once

#define IDS_MENU_TEXT           100
#define IDS_STATUS_MESSAGE      101
#define IDS_VERSION             102
#define IDS_SURE_TO_UNINSTALL   103

#define IDB_BITMAP              201

// チャットパネル
#define IDD_CHAT                300
#define IDC_HISTORY             301
#define IDC_INPUT               302
#define IDC_SEND                303
#define IDC_NEW_CHAT            304
#define IDC_INSERT              305
#define IDC_ACTIVE_DOC          306
#define IDC_LOGIN               307
#define IDC_MODEL               308
#define IDC_EFFORT              309
#define IDC_WEB                 310
#define IDC_SETTINGS            311

// 履歴表示（WebView2）に埋め込む HTML / CSS / JS（RCDATA）
#define IDR_CHAT_HTML           500
#define IDR_MARKDOWN_CSS        501
#define IDR_MARKED_JS           502

// 設定画面
#define IDD_SETTINGS            400
#define IDC_DEF_MODEL           401
#define IDC_DEF_EFFORT          402
#define IDC_DEF_WEB             403
#define IDC_MCP_LIST            404
#define IDC_MCP_NOTE            405
