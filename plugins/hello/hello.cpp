// Hello World プラグイン。新しいプラグインの雛形も兼ねる（scripts/new-plugin.ps1 がこのフォルダを複製する）。
//
// 構成の決まりごと:
//   - etlframe.h は DllMain とエクスポート関数の「実体」を含むため、include してよいのは 1 つの .cpp だけ。
//     .cpp を分割する場合、他の .cpp では EE_EXTERN_ONLY を定義してから include する。
//   - ETL_FRAME_CLASS_NAME は etlframe.h より前に定義する。
//   - フレームクラスの定義の後に _ETL_IMPLEMENT を置く。

#include "eecommon.h"
#include "resource.h"

#define ETL_FRAME_CLASS_NAME CMyFrame
#include "etlframe.h"

#ifdef _DEBUG
#define new _DEBUG_NEW
#else
#define new _RELEASE_NEW
#endif

// EmEditor のウィンドウ（フレーム）ごとに 1 インスタンス作られる。
class CMyFrame : public CETLFrame<CMyFrame>
{
public:
	// ---- etlframe.h が参照する定数。1 つでも欠けるとコンパイルエラーになる ----

	// リソースの置き場所: LOC_USE_PLUGIN_DLL = この DLL 自身のリソースを使う
	enum { _USE_LOC_DLL			= LOC_USE_PLUGIN_DLL	};

	enum { _IDS_MENU			= IDS_MENU_TEXT			};   // メニュー/ツールバーのコマンド名
	enum { _IDS_STATUS			= IDS_STATUS_MESSAGE	};   // ステータスバーの説明
	enum { _IDS_NAME			= IDS_MENU_TEXT			};   // プラグイン設定ダイアログでの名前
	enum { _IDS_VER				= IDS_VERSION			};   // プラグイン設定ダイアログでのバージョン

	// ツールバーのビットマップ。0 は「用意していない」
	enum { _IDB_BITMAP			= IDB_BITMAP			};   // 16x16 16 色
	enum { _IDB_16C_24			= 0						};   // 24x24 16 色
	enum { _IDB_TRUE_16_DEFAULT = 0						};
	enum { _IDB_TRUE_16_HOT		= 0						};
	enum { _IDB_TRUE_16_BW		= 0						};
	enum { _IDB_TRUE_24_DEFAULT = 0						};
	enum { _IDB_TRUE_24_HOT		= 0						};
	enum { _IDB_TRUE_24_BW		= 0						};
	enum { _MASK_TRUE_COLOR		= RGB( 192, 192, 192 )	};   // 透過色

	enum { _ALLOW_OPEN_SAME_GROUP		= TRUE	};   // プラグイン実行中に同じグループでファイルを開くことを許すか
	enum { _ALLOW_MULTIPLE_INSTANCES	= TRUE	};   // 複数インスタンスを許すか
	enum { _SUPPORT_EE_PRO				= TRUE	};
	enum { _USE_CUSTOM_BAR				= FALSE	};   // カスタムバー（サイドパネル等）を使うか

	// ---- コマンド ----

	// メニューまたはツールバーから実行されたとき
	void OnCommand( HWND hwndView )
	{
		WCHAR szFile[MAX_PATH] = {};
		Editor_Info( hwndView, EI_GET_FILE_NAMEW, (LPARAM)szFile );

		WCHAR sz[MAX_PATH + 64];
		StringCchPrintfW( sz, _countof( sz ), L"Hello World!! (EmEditor %u) file=%s\n",
			Editor_GetVersion( hwndView ), szFile[0] ? szFile : L"(無題)" );
		Editor_OutputString( hwndView, sz, FLAG_OPEN_OUTPUT );
	}

	// メニュー表示時・アイドル時に頻繁に呼ばれる。重い処理は書かない
	BOOL QueryStatus( HWND /*hwndView*/, LPBOOL pbChecked )
	{
		*pbChecked = FALSE;
		return TRUE;   // TRUE = 実行可能
	}

	// イベント通知。nEvent はビットフラグ（EVENT_*）
	void OnEvents( HWND /*hwndView*/, UINT /*nEvent*/, LPARAM /*lParam*/ )
	{
	}

	// ---- プラグイン設定ダイアログ関連 ----

	BOOL QueryUninstall( HWND /*hDlg*/ )
	{
		return TRUE;
	}

	BOOL SetUninstall( HWND hDlg, LPWSTR /*pszUninstallCommand*/, LPWSTR /*pszUninstallParam*/ )
	{
		WCHAR sz[80];
		WCHAR szAppName[80];
		LoadStringW( EEGetLocaleInstanceHandle(), IDS_SURE_TO_UNINSTALL, sz, _countof( sz ) );
		LoadStringW( EEGetLocaleInstanceHandle(), IDS_MENU_TEXT, szAppName, _countof( szAppName ) );
		if( MessageBoxW( hDlg, sz, szAppName, MB_YESNO | MB_ICONEXCLAMATION ) == IDYES ){
			return UNINSTALL_SIMPLE_DELETE;
		}
		return UNINSTALL_FALSE;
	}

	BOOL QueryProperties( HWND /*hDlg*/ )
	{
		return FALSE;   // TRUE にすると「プロパティ」ボタンが有効になり SetProperties が呼ばれる
	}

	BOOL SetProperties( HWND /*hDlg*/ )
	{
		return FALSE;
	}

	// ---- その他のフック ----

	BOOL PreTranslateMessage( HWND /*hwndView*/, MSG* /*pMsg*/ )
	{
		return FALSE;
	}

	BOOL UseDroppedFiles( HWND /*hwnd*/ )
	{
		return FALSE;
	}

	BOOL DisableAutoComplete( HWND /*hwnd*/ )
	{
		return FALSE;
	}

	LRESULT UserMessage( HWND /*hwnd*/, WPARAM /*wParam*/, LPARAM /*lParam*/ )
	{
		return 0;
	}
};

// CMyFrame の定義の後に必須
_ETL_IMPLEMENT
