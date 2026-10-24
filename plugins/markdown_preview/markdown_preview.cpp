// Markdown プレビュー プラグイン
//
// カスタムバー（サイドパネル）に WebView2 を載せ、アクティブな Markdown 文書を marked（GFM）で描画する。
//   - 編集したら 0.3 秒後に描き直す（打鍵のたびには描かない）。タブの切り替えではすぐ描き直す
//   - エディタのスクロールにプレビューを追従させる（表示の一番上の論理行 → プレビューの該当ブロック）
//   - プレビューのブロックをダブルクリックすると、エディタのその行へ移動する
//   - 相対パスの画像は、文書のフォルダを https://emeditor-doc.local/ に割り当てて表示する
//
// 描画はページ側（web/preview.html）。ネイティブは文書の全文と行番号を送るだけ。
// カスタムバーの作法は claude_chat と同じ（docs/custom-bar.md）。WebView2 まわりは docs/webview2.md。

#include "eecommon.h"
#include "resource.h"
#include "process_runner.h"
#include "editor_doc.h"
#include "webview_host.h"
#include <nlohmann/json.hpp>
#include <set>

#define ETL_FRAME_CLASS_NAME CMyFrame
#include "etlframe.h"

#ifdef _DEBUG
#define new _DEBUG_NEW
#else
#define new _RELEASE_NEW
#endif

namespace {

using json = nlohmann::json;

constexpr LPCWSTR kProfileOpenStartup = L"OpenStartup";
constexpr LPCWSTR kDocHost = L"emeditor-doc.local";        // 文書のフォルダを見せる仮想ホスト名
constexpr UINT_PTR kRenderTimer = 1;                      // 編集後、少し待ってから描き直す
constexpr UINT kRenderDelayMs = 300;
constexpr UINT_PTR kScrollTimer = 2;                      // スクロール連動を間引く
constexpr UINT kScrollDelayMs = 30;

// Markdown として扱う拡張子
constexpr LPCWSTR kMarkdownExts[] = { L".md", L".markdown", L".mdown", L".mkd", L".mkdn", L".mdx", L".mdwn" };

std::wstring Lower( std::wstring s )
{
	CharLowerBuffW( s.data(), (DWORD)s.size() );
	return s;
}

// 拡張子、または EmEditor の設定（構成）名が Markdown なら Markdown 文書とみなす
bool IsMarkdownDoc( HWND hwndView, const std::wstring& path )
{
	const std::wstring ext = Lower( PathFindExtensionW( path.c_str() ) );
	for( LPCWSTR e : kMarkdownExts )  if( ext == e )  return true;
	WCHAR config[MAX_PATH] = {};
	Editor_GetConfigW( hwndView, config );
	return Lower( config ).find( L"markdown" ) != std::wstring::npos;
}

std::wstring FolderOf( const std::wstring& path )
{
	std::wstring dir = path;
	PathRemoveFileSpecW( dir.data() );
	dir.resize( wcslen( dir.c_str() ) );
	return dir;
}

} // namespace

// EmEditor のウィンドウ（フレーム）ごとに 1 インスタンス作られる。
class CMyFrame : public CETLFrame<CMyFrame>
{
public:
	// ---- etlframe.h が参照する定数。1 つでも欠けるとコンパイルエラーになる ----

	enum { _USE_LOC_DLL			= LOC_USE_PLUGIN_DLL	};

	enum { _IDS_MENU			= IDS_MENU_TEXT			};
	enum { _IDS_STATUS			= IDS_STATUS_MESSAGE	};
	enum { _IDS_NAME			= IDS_MENU_TEXT			};
	enum { _IDS_VER				= IDS_VERSION			};

	enum { _IDB_BITMAP			= IDB_BITMAP			};
	enum { _IDB_16C_24			= 0						};
	enum { _IDB_TRUE_16_DEFAULT = 0						};
	enum { _IDB_TRUE_16_HOT		= 0						};
	enum { _IDB_TRUE_16_BW		= 0						};
	enum { _IDB_TRUE_24_DEFAULT = 0						};
	enum { _IDB_TRUE_24_HOT		= 0						};
	enum { _IDB_TRUE_24_BW		= 0						};
	enum { _MASK_TRUE_COLOR		= RGB( 192, 192, 192 )	};

	enum { _ALLOW_OPEN_SAME_GROUP		= TRUE	};
	enum { _ALLOW_MULTIPLE_INSTANCES	= TRUE	};
	enum { _SUPPORT_EE_PRO				= TRUE	};
	enum { _USE_CUSTOM_BAR				= TRUE	};

	// ---- コマンド: パネルの開閉 ----

	void OnCommand( HWND hwndView )
	{
		m_hwndView = hwndView;
		if( m_hDlg ){
			CloseBar();
			WriteProfileInt( kProfileOpenStartup, FALSE );
		}
		else {
			OpenBar();
		}
	}

	BOOL QueryStatus( HWND /*hwndView*/, LPBOOL pbChecked )
	{
		*pbChecked = m_hDlg != nullptr;
		return TRUE;
	}

	void OnEvents( HWND hwndView, UINT nEvent, LPARAM lParam )
	{
		if( hwndView )  m_hwndView = hwndView;

		if( m_hDlg && m_ready ){
			if( nEvent & ( EVENT_DOC_SEL_CHANGED | EVENT_FILE_OPENED | EVENT_CONFIG_CHANGED ) ){
				KillTimer( m_hDlg, kRenderTimer );
				Render();
			}
			else if( nEvent & EVENT_CHANGE ){
				SetTimer( m_hDlg, kRenderTimer, kRenderDelayMs, nullptr );   // 打鍵が止まってから描き直す
			}
			if( nEvent & ( EVENT_SCROLL | EVENT_CARET_MOVED ) ){
				SetTimer( m_hDlg, kScrollTimer, kScrollDelayMs, nullptr );
			}
		}

		if( nEvent & EVENT_CREATE_FRAME ){
			if( GetProfileInt( kProfileOpenStartup, FALSE ) )  OpenBar();
		}
		if( nEvent & EVENT_CLOSE_FRAME ){
			if( m_hDlg ){
				WriteProfileInt( kProfileOpenStartup, TRUE );
				CloseBar();
			}
		}
		if( nEvent & EVENT_CUSTOM_BAR_CLOSED ){
			// 他のプラグインのバーでも届くので、自分のものか確かめる
			auto* pInfo = reinterpret_cast<CUSTOM_BAR_CLOSE_INFO*>( lParam );
			if( m_hDlg && pInfo && pInfo->nID == m_nClientID ){
				BarClosed();
				if( !m_bClosingBySelf ){
					WriteProfileInt( kProfileOpenStartup, ( pInfo->dwFlags & CLOSED_FRAME_WINDOW ) ? TRUE : FALSE );
				}
			}
		}
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
		return FALSE;
	}

	BOOL SetProperties( HWND /*hDlg*/ )
	{
		return FALSE;
	}

	// ---- その他のフック ----

	BOOL PreTranslateMessage( HWND /*hwndView*/, MSG* /*pMsg*/ )
	{
		return FALSE;   // パネルにネイティブの入力部品は無い（WebView2 のキーは onKey で受ける）
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

	~CMyFrame()
	{
		m_web.Close();
	}

private:
	HWND					m_hDlg = nullptr;
	UINT					m_nClientID = 0;
	bool					m_bClosingBySelf = false;
	HWND					m_hwndView = nullptr;
	ee::web::Host			m_web;
	bool					m_ready = false;          // ページが「準備完了」を送ってきた
	bool					m_mermaidRequested = false;
	std::set<std::wstring>	m_forced;                 // Markdown 以外だが「Markdown として表示」を選んだ文書
	std::wstring			m_mappedFolder;           // 仮想ホストに割り当て中のフォルダ
	LONG_PTR				m_lastTop = -1;           // 最後にプレビューへ送った表示行
	LONG_PTR				m_lastCaret = -1;

	bool HasView() const
	{
		return m_hwndView && IsWindow( m_hwndView );
	}

	// ---- パネルの開閉 ----

	void OpenBar()
	{
		if( m_hDlg )  return;
		m_hDlg = CreateDialogParamW( EEGetInstanceHandle(), MAKEINTRESOURCEW( IDD_PREVIEW ), m_hWnd, DlgProc, (LPARAM)this );
		if( !m_hDlg )  return;

		WCHAR szTitle[80];
		LoadStringW( EEGetInstanceHandle(), IDS_MENU_TEXT, szTitle, _countof( szTitle ) );
		CUSTOM_BAR_INFO cbi = {};
		cbi.cbSize = sizeof( cbi );
		cbi.hwndClient = m_hDlg;
		cbi.iPos = CUSTOM_BAR_RIGHT;
		cbi.pszTitle = szTitle;
		m_nClientID = Editor_CustomBarOpen( m_hWnd, &cbi );
		if( !m_nClientID ){
			BarClosed();
			return;
		}
		ShowWindow( m_hDlg, SW_SHOW );
		CreateWebView();
	}

	void CloseBar()
	{
		if( !m_hDlg )  return;
		m_bClosingBySelf = true;
		Editor_CustomBarClose( m_hWnd, m_nClientID );
		m_bClosingBySelf = false;
		BarClosed();
	}

	void BarClosed()
	{
		m_web.Close();
		m_ready = false;
		m_mermaidRequested = false;
		m_mappedFolder.clear();
		m_lastTop = m_lastCaret = -1;
		if( m_hDlg ){
			if( IsWindow( m_hDlg ) )  DestroyWindow( m_hDlg );
			m_hDlg = nullptr;
		}
		m_nClientID = 0;
	}

	void CreateWebView()
	{
		ee::web::Host::Callbacks cb;
		cb.onMessage = [this]( const std::wstring& j ){ OnPageMessage( j ); };
		cb.onError = [this]( HRESULT hr ){
			WCHAR sz[200];
			StringCchPrintfW( sz, _countof( sz ), L"WebView2 を使えないため、プレビューを表示できません (0x%08lX)。\nWebView2 Runtime がインストールされているか確認してください。", (unsigned long)hr );
			SetDlgItemTextW( m_hDlg, IDC_MESSAGE, sz );
			ShowWindow( GetDlgItem( m_hDlg, IDC_MESSAGE ), SW_SHOW );
		};
		cb.onKey = [this]( UINT vkey ){
			if( vkey != VK_ESCAPE )  return false;
			Editor_ExecCommand( m_hWnd, EEID_ACTIVE_PANE );   // Esc でエディタに戻る
			return true;
		};
		m_web.Create( m_hDlg, std::move( cb ) );
		// ページより先に実行するライブラリ（Mermaid は大きいので、図が出てきたときだけ needMermaid で読み込む）
		const HINSTANCE hinst = EEGetInstanceHandle();
		for( int id : { IDR_MARKED_JS, IDR_FOOTNOTE_JS, IDR_HLJS_JS, IDR_KATEX_JS } ){
			m_web.AddScriptOnDocumentCreated( ee::Utf8ToWide( ee::web::LoadResourceUtf8( hinst, id ) ) );
		}
		m_web.NavigateToString( ee::Utf8ToWide( ee::web::FillTemplate( ee::web::LoadResourceUtf8( hinst, IDR_PREVIEW_HTML ), {
			{ "NONCE", ee::web::RandomHex() },
			{ "MARKDOWN_CSS", ee::web::LoadResourceUtf8( hinst, IDR_MARKDOWN_CSS ) },
			{ "HLJS_LIGHT_CSS", ee::web::LoadResourceUtf8( hinst, IDR_HLJS_LIGHT_CSS ) },
			{ "HLJS_DARK_CSS", ee::web::LoadResourceUtf8( hinst, IDR_HLJS_DARK_CSS ) },
			{ "KATEX_CSS", ee::web::LoadResourceUtf8( hinst, IDR_KATEX_CSS ) },
		} ) ) );
		RECT rc;
		GetClientRect( m_hDlg, &rc );
		m_web.SetBounds( rc );
	}

	// ---- 描画 ----

	void Post( const json& msg )
	{
		m_web.PostJson( ee::Utf8ToWide( msg.dump() ) );
	}

	void Render()
	{
		if( !m_ready || !HasView() )  return;
		const std::wstring path = ee::doc::GetDocPath( m_hwndView );

		if( !IsMarkdownDoc( m_hwndView, path ) && !m_forced.count( path ) ){
			Post( { { "type", "message" },
				{ "title", "Markdown の文書ではありません" },
				{ "text", ee::WideToUtf8( ee::doc::GetDocName( m_hwndView ) ) },
				{ "action", "force" }, { "actionLabel", "Markdown として表示" } } );
			return;
		}

		// 相対パスの画像・リンクのために、文書のフォルダを仮想ホストに割り当てる（保存済みの文書だけ）
		std::string baseHref;
		if( ee::doc::IsSavedDoc( path ) ){
			const std::wstring folder = FolderOf( path );
			if( folder != m_mappedFolder ){
				m_web.MapFolder( kDocHost, folder );
				m_mappedFolder = folder;
			}
			baseHref = "https://" + ee::WideToUtf8( kDocHost ) + "/";
		}

		m_lastTop = ee::doc::GetTopLogicalLine( m_hwndView );
		POINT_PTR caret = {};
		Editor_GetCaretPos( m_hwndView, POS_LOGICAL_W, &caret );
		m_lastCaret = caret.y;
		Post( { { "type", "render" },
			{ "markdown", ee::WideToUtf8( ee::doc::GetDocumentText( m_hwndView ) ) },
			{ "docKey", ee::WideToUtf8( path ) },
			{ "baseHref", baseHref },
			{ "line", m_lastTop + 1 },
			{ "caret", m_lastCaret + 1 } } );
	}

	// エディタの表示位置・カーソル行をプレビューへ（変わったときだけ送る）
	void SyncScroll()
	{
		if( !m_ready || !HasView() )  return;
		const LONG_PTR top = ee::doc::GetTopLogicalLine( m_hwndView );
		if( top != m_lastTop ){
			m_lastTop = top;
			Post( { { "type", "scroll" }, { "line", top + 1 } } );
		}
		POINT_PTR caret = {};
		Editor_GetCaretPos( m_hwndView, POS_LOGICAL_W, &caret );
		if( caret.y != m_lastCaret ){
			m_lastCaret = caret.y;
			Post( { { "type", "caret" }, { "line", caret.y + 1 } } );
		}
	}

	// ---- ページからのメッセージ ----

	void OnPageMessage( const std::wstring& j )
	{
		const json msg = json::parse( ee::WideToUtf8( j ), nullptr, false );
		if( !msg.is_object() )  return;
		const std::string type = msg.value( "type", "" );

		if( type == "ready" ){
			m_ready = true;
			Render();
		}
		else if( type == "jump" && HasView() ){
			// プレビューのダブルクリック → エディタのその行の先頭へ
			POINT_PTR pt = { 0, (LONG_PTR)msg.value( "line", 1 ) - 1 };
			if( pt.y < 0 )  pt.y = 0;
			Editor_SetCaretPos( m_hwndView, POS_LOGICAL_W, &pt );
			Editor_ExecCommand( m_hWnd, EEID_ACTIVE_PANE );
		}
		else if( type == "needMermaid" ){
			// Mermaid（5MB）は図が出てきたときに 1 回だけ読み込む。ExecuteScript はページの CSP の対象外
			if( !m_mermaidRequested ){
				m_mermaidRequested = true;
				m_web.ExecuteScript( ee::Utf8ToWide( ee::web::LoadResourceUtf8( EEGetInstanceHandle(), IDR_MERMAID_JS ) ),
					[this]( const std::wstring& ){ Post( { { "type", "mermaidReady" } } ); } );
			}
		}
		else if( type == "openFile" ){
			OpenRelativeFile( ee::Utf8ToWide( msg.value( "path", "" ) ) );
		}
		else if( type == "open" ){
			ee::web::OpenExternal( ee::Utf8ToWide( msg.value( "href", "" ) ) );   // http(s) だけ
		}
		else if( type == "action" && msg.value( "name", "" ) == "force" && HasView() ){
			m_forced.insert( ee::doc::GetDocPath( m_hwndView ) );
			Render();
		}
	}

	// 文書からの相対リンク（/sub/b.md など）を、文書のフォルダ基準で EmEditor に開く
	void OpenRelativeFile( std::wstring rel )
	{
		if( m_mappedFolder.empty() || rel.empty() || !HasView() )  return;
		for( wchar_t& c : rel )  if( c == L'/' )  c = L'\\';
		WCHAR full[MAX_PATH];
		if( FAILED( PathCchCombine( full, MAX_PATH, m_mappedFolder.c_str(), rel.c_str() + ( rel[0] == L'\\' ? 1 : 0 ) ) ) )  return;
		if( GetFileAttributesW( full ) == INVALID_FILE_ATTRIBUTES )  return;
		LOAD_FILE_INFO_EX lfi = { sizeof( lfi ) };
		lfi.nCP = CP_UTF8;
		lfi.bDetectUTF8 = TRUE;
		lfi.bDetectAll = TRUE;
		lfi.nFlags = LFI_ALLOW_NEW_WINDOW;
		Editor_LoadFileW( m_hwndView, &lfi, full );
	}

	// ---- ダイアログ ----

	static INT_PTR CALLBACK DlgProc( HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam )
	{
		if( msg == WM_INITDIALOG ){
			SetWindowLongPtrW( hDlg, DWLP_USER, lParam );
			return FALSE;
		}
		auto* pThis = reinterpret_cast<CMyFrame*>( GetWindowLongPtrW( hDlg, DWLP_USER ) );
		if( !pThis )  return FALSE;

		switch( msg ){
		case WM_SIZE: {
			RECT rc = { 0, 0, LOWORD( lParam ), HIWORD( lParam ) };
			pThis->m_web.SetBounds( rc );
			SetWindowPos( GetDlgItem( hDlg, IDC_MESSAGE ), nullptr, 8, 8, std::max( 0, (int)rc.right - 16 ), std::max( 0, (int)rc.bottom - 16 ), SWP_NOZORDER );
			return TRUE;
		}
		case WM_TIMER:
			KillTimer( hDlg, wParam );
			if( wParam == kRenderTimer )  pThis->Render();
			if( wParam == kScrollTimer )  pThis->SyncScroll();
			return TRUE;
		}
		return FALSE;
	}
};

// CMyFrame の定義の後に必須
_ETL_IMPLEMENT
