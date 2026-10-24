// markdown_preview.rc で使うリソース ID
#pragma once

#define IDS_MENU_TEXT           100
#define IDS_STATUS_MESSAGE      101
#define IDS_VERSION             102
#define IDS_SURE_TO_UNINSTALL   103

#define IDB_BITMAP              201

// プレビューパネル（WebView2 を載せるだけの子ダイアログ）
#define IDD_PREVIEW             300
#define IDC_MESSAGE             301   // WebView2 が使えないときのメッセージ

// プレビューに埋め込む HTML / CSS / JS（RCDATA）
#define IDR_PREVIEW_HTML        500
#define IDR_MARKDOWN_CSS        501
#define IDR_MARKED_JS           502
#define IDR_FOOTNOTE_JS         503
#define IDR_HLJS_JS             504
#define IDR_HLJS_LIGHT_CSS      505
#define IDR_HLJS_DARK_CSS       506
#define IDR_KATEX_JS            507
#define IDR_KATEX_CSS           508
#define IDR_MERMAID_JS          509   // 5MB。図が出てきたときだけ読み込む
