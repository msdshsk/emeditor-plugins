// Claude でメモ整理 プラグイン
//
// 選択範囲（無ければ文書全体）の走り書きメモを `claude -p` に stdin で渡し、
// MECE に分類・構造化した結果で選択範囲を置き換える。
// claude の応答には数秒かかるので、ワーカースレッドで実行して EmEditor の UI を止めない。
// 実行中にもう一度コマンドを実行するとキャンセルする。
//
// スレッドの約束:
//   - Editor_* は UI スレッド（OnCommand / OnEvents / 通知ウィンドウの WndProc）からだけ呼ぶ。
//   - ワーカーは結果を m_result に書き、メッセージ専用ウィンドウに PostMessage して UI スレッドへ戻す。
//
// 置き換えの安全策:
//   応答を待つ間にユーザーが編集・タブ切り替えをしているかもしれない。完了時に
//   「同じ文書か」「元の範囲に元のテキストがそのまま残っているか」を確かめ、違えば置き換えずにアウトプット バーに出す。

#include "eecommon.h"
#include "resource.h"
#include "process_runner.h"

#define ETL_FRAME_CLASS_NAME CMyFrame
#include "etlframe.h"

#ifdef _DEBUG
#define new _DEBUG_NEW
#else
#define new _RELEASE_NEW
#endif

namespace {

constexpr UINT WM_APP_TIDY_DONE = WM_APP + 1;
constexpr LPCWSTR kNotifyClassName = L"EmEditorPlugin_MemoTidy_Notify";
constexpr LPCWSTR kProfileKeepOriginal = L"KeepOriginal";
constexpr LPCWSTR kSeparator = L"↓↓↓";

constexpr LPCWSTR kSystemPrompt =
	L"あなたはメモの整理を手伝うアシスタントです。入力は、話を聞きながら取った走り書きのメモです。次の方針で整理してください。\n"
	L"- 内容をトピックごとに MECE（漏れなく・重複なく）に分類し、見出しと箇条書きで構造化する。同じ内容を複数の節に重複して書かない\n"
	L"- Markdown で出力する（見出しは ## から、箇条書きは - ）\n"
	L"- 決定事項・TODO・未決事項（疑問点）に当たる項目は、行頭に【決定】【TODO】【未決】を付けて区別する。TODO は担当や期限が書かれていれば併記する\n"
	L"- メモに書かれていない情報を推測で足さない。意味が曖昧な箇所は無理に解釈せず、元の表現を残す\n"
	L"- 誤字脱字や表記ゆれは直す\n"
	L"- 整理した本文だけを出力する。前置き・後書き・コードブロックの囲みは付けない";
constexpr LPCWSTR kInstruction = L"次のメモを整理してください。";

// claude.exe の場所。EmEditor 起動後に PATH が変わっていても見つかるよう、既定のインストール先も見る。
std::wstring FindClaudeExe()
{
	WCHAR sz[MAX_PATH];
	if( SearchPathW( nullptr, L"claude.exe", nullptr, MAX_PATH, sz, nullptr ) ){
		return sz;
	}
	WCHAR szProfile[MAX_PATH];
	if( GetEnvironmentVariableW( L"USERPROFILE", szProfile, MAX_PATH ) ){
		std::wstring path = std::wstring( szProfile ) + L"\\.local\\bin\\claude.exe";
		if( GetFileAttributesW( path.c_str() ) != INVALID_FILE_ATTRIBUTES ){
			return path;
		}
	}
	return {};
}

// claude の作業フォルダ。claude はカレントの CLAUDE.md や .claude/ を読むので、空のフォルダで動かす。
std::wstring GetWorkDir()
{
	WCHAR sz[MAX_PATH];
	GetTempPathW( MAX_PATH, sz );
	std::wstring dir = std::wstring( sz ) + L"emeditor-memo-tidy";
	CreateDirectoryW( dir.c_str(), nullptr );
	return dir;
}

std::wstring GetSelectedText( HWND hwndView )
{
	const UINT_PTR cch = Editor_GetSelTextW( hwndView, 0, nullptr );   // 終端 NUL を含む必要サイズ
	if( cch <= 1 )  return {};
	std::wstring s( cch, L'\0' );
	Editor_GetSelTextW( hwndView, cch, s.data() );
	s.resize( wcslen( s.c_str() ) );
	return s;
}

std::wstring GetFileName( HWND hwndView )
{
	WCHAR sz[MAX_PATH] = {};
	Editor_Info( hwndView, EI_GET_FILE_NAMEW, (LPARAM)sz );   // 無題の文書は「無題-1」のような名前が返る
	return sz;
}

// 改行を \n に揃える（置き換え時に KEEP_DEST_RETURN_TYPE で文書の改行コードに変換される）
std::wstring NormalizeNewlines( std::wstring_view s )
{
	std::wstring r;
	r.reserve( s.size() );
	for( size_t i = 0; i < s.size(); i++ ){
		if( s[i] == L'\r' ){
			if( i + 1 < s.size() && s[i + 1] == L'\n' )  continue;
			r += L'\n';
		}
		else {
			r += s[i];
		}
	}
	return r;
}

std::wstring TrimTrailingNewlines( std::wstring s )
{
	while( !s.empty() && ( s.back() == L'\n' || s.back() == L'\r' ) )  s.pop_back();
	return s;
}

INT_PTR CALLBACK PropertiesDlgProc( HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam )
{
	switch( msg ){
	case WM_INITDIALOG:
		SetWindowLongPtrW( hDlg, DWLP_USER, lParam );
		CheckDlgButton( hDlg, IDC_KEEP_ORIGINAL, *reinterpret_cast<BOOL*>( lParam ) ? BST_CHECKED : BST_UNCHECKED );
		return TRUE;
	case WM_COMMAND:
		if( LOWORD( wParam ) == IDOK ){
			*reinterpret_cast<BOOL*>( GetWindowLongPtrW( hDlg, DWLP_USER ) ) = IsDlgButtonChecked( hDlg, IDC_KEEP_ORIGINAL ) == BST_CHECKED;
			EndDialog( hDlg, IDOK );
			return TRUE;
		}
		if( LOWORD( wParam ) == IDCANCEL ){
			EndDialog( hDlg, IDCANCEL );
			return TRUE;
		}
		break;
	}
	return FALSE;
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
	enum { _USE_CUSTOM_BAR				= FALSE	};

	// ---- コマンド ----

	void OnCommand( HWND hwndView )
	{
		if( m_running ){
			m_runner.Cancel();
			Editor_SetStatusW( hwndView, L"メモ整理をキャンセルしています..." );
			return;
		}

		const std::wstring exe = FindClaudeExe();
		if( exe.empty() ){
			Editor_OutputString( hwndView, L"[メモ整理] claude.exe が見つかりません。Claude Code をインストールしてください。\n", FLAG_OPEN_OUTPUT );
			return;
		}

		const int selType = Editor_GetSelType( hwndView ) & SEL_TYPE_MASK;
		if( selType == SEL_TYPE_BOX ){
			Editor_OutputString( hwndView, L"[メモ整理] 矩形選択には対応していません。\n", FLAG_OPEN_OUTPUT );
			return;
		}
		if( selType == SEL_TYPE_NONE ){
			Editor_ExecCommand( hwndView, EEID_EDIT_SELECT_ALL );
		}
		const std::wstring text = GetSelectedText( hwndView );
		if( text.empty() ){
			Editor_OutputString( hwndView, L"[メモ整理] 対象のテキストがありません。\n", FLAG_OPEN_OUTPUT );
			return;
		}

		if( !EnsureNotifyWindow() ){
			Editor_OutputString( hwndView, L"[メモ整理] 内部エラー: 通知ウィンドウを作成できません。\n", FLAG_OPEN_OUTPUT );
			return;
		}

		std::wstring cmd = ee::QuoteArg( exe );
		// --strict-mcp-config: ユーザー設定の MCP サーバーを読み込まない（--tools "" では MCP のツールは消えない）
		cmd += L" -p --tools \"\" --strict-mcp-config --no-session-persistence --system-prompt ";
		cmd += ee::QuoteArg( kSystemPrompt );
		cmd += L" ";
		cmd += ee::QuoteArg( kInstruction );

		// 完了時に同じ範囲を置き換えるため、対象を記録しておく
		m_hwndView = hwndView;
		m_fileName = GetFileName( hwndView );
		m_original = text;
		Editor_GetSelStart( hwndView, POS_LOGICAL_W, &m_selStart );
		Editor_GetSelEnd( hwndView, POS_LOGICAL_W, &m_selEnd );

		if( m_worker.joinable() )  m_worker.join();
		m_running = true;
		m_startTick = GetTickCount64();
		Editor_SetStatusW( hwndView, L"Claude でメモを整理中...（もう一度実行するとキャンセル）" );

		m_worker = std::thread( [this, cmd, input = ee::WideToUtf8( text ), dir = GetWorkDir()]{
			m_result = m_runner.Run( cmd, dir, input );
			PostMessageW( m_hwndNotify, WM_APP_TIDY_DONE, 0, 0 );
		} );
	}

	// 実行中はメニュー/ボタンをチェック状態にする
	BOOL QueryStatus( HWND /*hwndView*/, LPBOOL pbChecked )
	{
		*pbChecked = m_running ? TRUE : FALSE;
		return TRUE;
	}

	void OnEvents( HWND /*hwndView*/, UINT nEvent, LPARAM /*lParam*/ )
	{
		if( nEvent & EVENT_CLOSE_FRAME ){
			Shutdown();
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
		return TRUE;   // 「プロパティ」ボタンを有効にする
	}

	BOOL SetProperties( HWND hDlg )
	{
		BOOL bKeepOriginal = GetProfileInt( kProfileKeepOriginal, FALSE );
		if( DialogBoxParamW( EEGetInstanceHandle(), MAKEINTRESOURCEW( IDD_PROPERTIES ), hDlg, PropertiesDlgProc, (LPARAM)&bKeepOriginal ) != IDOK ){
			return FALSE;
		}
		WriteProfileInt( kProfileKeepOriginal, bKeepOriginal );
		return TRUE;
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

	~CMyFrame()
	{
		Shutdown();
	}

private:
	ee::ProcessRunner	m_runner;
	std::thread			m_worker;
	ee::ProcessResult	m_result;            // ワーカーが書き、join 後に UI スレッドが読む
	std::atomic<bool>	m_running = false;
	HWND				m_hwndNotify = nullptr;
	ULONGLONG			m_startTick = 0;

	// 置き換え対象（OnCommand で記録し、OnDone で照合する）
	HWND				m_hwndView = nullptr;
	std::wstring		m_fileName;
	std::wstring		m_original;
	POINT_PTR			m_selStart = {};
	POINT_PTR			m_selEnd = {};

	// フレームが閉じる/プラグインが解放されるとき。子プロセスを止め、スレッドを回収する。
	void Shutdown()
	{
		if( m_worker.joinable() ){
			m_runner.Cancel();
			m_worker.join();
		}
		m_running = false;
		if( m_hwndNotify ){
			DestroyWindow( m_hwndNotify );
			m_hwndNotify = nullptr;
		}
	}

	// ワーカーからの完了通知を UI スレッドで受けるためのメッセージ専用ウィンドウ
	bool EnsureNotifyWindow()
	{
		if( m_hwndNotify )  return true;
		WNDCLASSEXW wc = { sizeof( wc ) };
		if( !GetClassInfoExW( EEGetInstanceHandle(), kNotifyClassName, &wc ) ){
			wc.lpfnWndProc = NotifyWndProc;
			wc.hInstance = EEGetInstanceHandle();
			wc.lpszClassName = kNotifyClassName;
			if( !RegisterClassExW( &wc ) )  return false;
		}
		m_hwndNotify = CreateWindowExW( 0, kNotifyClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, EEGetInstanceHandle(), nullptr );
		if( !m_hwndNotify )  return false;
		SetWindowLongPtrW( m_hwndNotify, GWLP_USERDATA, (LONG_PTR)this );
		return true;
	}

	static LRESULT CALLBACK NotifyWndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
	{
		if( msg == WM_APP_TIDY_DONE ){
			auto* pThis = reinterpret_cast<CMyFrame*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
			if( pThis )  pThis->OnDone();
			return 0;
		}
		return DefWindowProcW( hwnd, msg, wParam, lParam );
	}

	void OnDone()
	{
		if( m_worker.joinable() )  m_worker.join();
		m_running = false;

		HWND hwndView = m_hwndView;
		if( !hwndView || !IsWindow( hwndView ) )  return;

		const ee::ProcessResult& r = m_result;
		const double sec = ( GetTickCount64() - m_startTick ) / 1000.0;
		WCHAR sz[160];
		if( !r.started ){
			StringCchPrintfW( sz, _countof( sz ), L"[メモ整理] claude を起動できません (error %lu)\n", r.error );
			Editor_OutputString( hwndView, sz, FLAG_OPEN_OUTPUT );
			Editor_SetStatusW( hwndView, L"メモ整理に失敗しました" );
			return;
		}
		if( r.canceled ){
			Editor_SetStatusW( hwndView, L"メモ整理をキャンセルしました" );
			return;
		}
		if( r.exitCode != 0 ){
			StringCchPrintfW( sz, _countof( sz ), L"[メモ整理] claude が失敗しました (exit %lu, %.1f 秒)\n", r.exitCode, sec );
			std::wstring detail = ee::Utf8ToWide( r.err ) + ee::Utf8ToWide( r.out );
			// 未ログインなら "Not logged in · Please run /login" が返る
			if( detail.find( L"Not logged in" ) != std::wstring::npos ){
				detail += L"\n→ Claude Code にログインしていません。Claude チャットのパネルの「ログイン」ボタンか、コマンドプロンプトで claude auth login を実行してください。";
			}
			Editor_OutputString( hwndView, ( sz + detail + L"\n" ).c_str(), FLAG_OPEN_OUTPUT );
			Editor_SetStatusW( hwndView, L"メモ整理に失敗しました" );
			return;
		}

		const std::wstring tidy = TrimTrailingNewlines( NormalizeNewlines( ee::Utf8ToWide( r.out ) ) );
		if( tidy.empty() ){
			Editor_OutputString( hwndView, L"[メモ整理] claude の応答が空でした。\n", FLAG_OPEN_OUTPUT );
			Editor_SetStatusW( hwndView, L"メモ整理に失敗しました" );
			return;
		}

		std::wstring replacement;
		if( GetProfileInt( kProfileKeepOriginal, FALSE ) ){
			replacement = TrimTrailingNewlines( NormalizeNewlines( m_original ) ) + L"\n" + kSeparator + L"\n" + tidy;
		}
		else {
			replacement = tidy;
		}
		const wchar_t last = m_original.empty() ? 0 : m_original.back();
		if( last == L'\n' || last == L'\r' )  replacement += L'\n';   // 元の選択が改行で終わっていれば合わせる

		if( !RestoreSelection( hwndView ) ){
			StringCchPrintfW( sz, _countof( sz ), L"[メモ整理] 待っている間に文書が変更されたため、置き換えずにここに出力します (%.1f 秒)\n", sec );
			Editor_OutputString( hwndView, ( sz + tidy + L"\n" ).c_str(), FLAG_OPEN_OUTPUT );
			Editor_SetStatusW( hwndView, L"メモ整理の結果をアウトプット バーに出力しました" );
			return;
		}
		Editor_InsertW( hwndView, replacement.c_str(), true );   // 選択範囲を置き換え。Ctrl+Z で戻せる
		StringCchPrintfW( sz, _countof( sz ), L"メモ整理が完了しました (%.1f 秒)", sec );
		Editor_SetStatusW( hwndView, sz );
	}

	// 記録しておいた範囲を選択し直し、元のテキストのままなら true
	bool RestoreSelection( HWND hwndView )
	{
		if( GetFileName( hwndView ) != m_fileName )  return false;   // 別のタブに切り替わっている
		POINT_PTR start = m_selStart;
		POINT_PTR end = m_selEnd;
		Editor_SetCaretPos( hwndView, POS_LOGICAL_W, &start );
		Editor_SetCaretPosEx( hwndView, POS_LOGICAL_W, &end, TRUE );   // TRUE = 選択を伸ばす
		return GetSelectedText( hwndView ) == m_original;
	}
};

// CMyFrame の定義の後に必須
_ETL_IMPLEMENT
