// Claude チャット プラグイン
//
// カスタムバー（サイドパネル）に会話 UI を出し、`claude -p` と会話する。
// 1 回目の応答の session_id を覚えておき、2 回目以降は --resume で同じ会話を続ける。
//
// Claude は EmEditor の文書をツールで読み書きできる:
//   パネルを開くとプラグイン内で MCP サーバー（127.0.0.1・ランダムポート・トークン認証）を起動し、
//   claude -p に --mcp-config で渡す。ツールの中身は shared/editor_tools.h。
//   ツール呼び出しは MCP サーバーのスレッドに届くので、ダイアログへ SendMessage して UI スレッドで実行する。
//
// カスタムバーの作法（公式 Search / OutlineText プラグインのソースより）:
//   - クライアント（ここでは子ダイアログ）をフレームを仮の親にして作り、Editor_CustomBarOpen に渡す。
//   - EVENT_CUSTOM_BAR_CLOSING / CLOSED は「他のプラグインのバー」でも届く。nID で自分のものか確かめる。
//   - パネル内のキー入力は PreTranslateMessage で IsDialogMessage に回す。
//
// スレッドとデッドロック:
//   UI スレッド ──(std::thread)──> ワーカー: claude -p の終了を待つ
//   claude ──(HTTP)──> MCP サーバーのスレッド ──(SendMessageTimeout)──> UI スレッド: Editor_* を実行
//   UI スレッドがワーカーや MCP サーバーの終了を待つときは、ee::mcp::JoinPumpingSentMessages で
//   送られてきたメッセージを処理しながら待つ（ふつうの join だと、ツール呼び出しと待ち合ってデッドロックする）。

#include "eecommon.h"
#include "resource.h"
#include "process_runner.h"
#include "json_lite.h"
#include "mcp_server.h"
#include "editor_tools.h"
#include "chat_view.h"
#include "templates.h"
#include <shellapi.h>

#define ETL_FRAME_CLASS_NAME CMyFrame
#include "etlframe.h"

#ifdef _DEBUG
#define new _DEBUG_NEW
#else
#define new _RELEASE_NEW
#endif

namespace {

constexpr UINT WM_APP_CHAT_DONE = WM_APP + 1;
constexpr UINT WM_APP_TOOL_CALL = WM_APP + 2;
constexpr UINT WM_APP_AUTH_DONE = WM_APP + 3;    // claude auth status の結果が出た
constexpr UINT WM_APP_LOGIN_EXIT = WM_APP + 4;   // claude auth login のコンソールが閉じた
constexpr UINT_PTR kThinkingTimer = 1;           // 「考え中」表示を進めるタイマー
constexpr UINT_PTR kTemplateTimer = 2;           // テンプレートのファイルが保存されたかを見るタイマー
constexpr LPCWSTR kProfileOpenStartup = L"OpenStartup";
constexpr LPCWSTR kProfileModel = L"DefaultModel";
constexpr LPCWSTR kProfileEffort = L"DefaultEffort";
constexpr LPCWSTR kProfileWeb = L"DefaultWeb";
constexpr LPCWSTR kProfileMcp = L"McpServers";   // 有効にするユーザー MCP サーバー名（カンマ区切り）
constexpr char kMcpServerName[] = "emeditor";

// --model / --effort に渡す値と表示名
struct Choice { LPCWSTR value; LPCWSTR label; };
constexpr Choice kModels[] = {
	{ L"opus",   L"Opus" },
	{ L"sonnet", L"Sonnet" },
	{ L"haiku",  L"Haiku" },
	{ L"fable",  L"Fable" },
};
constexpr Choice kEfforts[] = {
	{ L"low",    L"low" },
	{ L"medium", L"medium" },
	{ L"high",   L"high" },
	{ L"xhigh",  L"xhigh" },
	{ L"max",    L"max" },
};

struct ChatSettings
{
	std::wstring              model = L"opus";
	std::wstring              effort = L"medium";
	bool                      web = false;
	std::vector<std::wstring> mcp;   // 有効にするユーザー MCP サーバー名
};

template <size_t N>
void FillCombo( HWND hCombo, const Choice ( &choices )[N], const std::wstring& selected )
{
	SendMessageW( hCombo, CB_RESETCONTENT, 0, 0 );
	int sel = 0;
	for( size_t i = 0; i < N; i++ ){
		SendMessageW( hCombo, CB_ADDSTRING, 0, (LPARAM)choices[i].label );
		if( selected == choices[i].value )  sel = (int)i;
	}
	SendMessageW( hCombo, CB_SETCURSEL, sel, 0 );
}

template <size_t N>
std::wstring ComboValue( HWND hCombo, const Choice ( &choices )[N] )
{
	const LRESULT i = SendMessageW( hCombo, CB_GETCURSEL, 0, 0 );
	return ( i >= 0 && (size_t)i < N ) ? choices[i].value : choices[0].value;
}

template <size_t N>
bool IsChoice( const Choice ( &choices )[N], const std::wstring& value )
{
	for( const Choice& c : choices )  if( value == c.value )  return true;
	return false;
}

template <size_t N>
std::wstring ChoiceLabel( const Choice ( &choices )[N], const std::wstring& value )
{
	for( const Choice& c : choices )  if( value == c.value )  return c.label;
	return value;
}

std::vector<std::wstring> SplitComma( const std::wstring& s )
{
	std::vector<std::wstring> r;
	size_t pos = 0;
	while( pos <= s.size() ){
		const size_t c = s.find( L',', pos );
		std::wstring item = s.substr( pos, c == std::wstring::npos ? std::wstring::npos : c - pos );
		if( !item.empty() )  r.push_back( item );
		if( c == std::wstring::npos )  break;
		pos = c + 1;
	}
	return r;
}

std::wstring JoinComma( const std::vector<std::wstring>& v )
{
	std::wstring r;
	for( const auto& s : v ){
		if( !r.empty() )  r += L",";
		r += s;
	}
	return r;
}

// Claude Code のユーザー設定ファイル。CLAUDE_CONFIG_DIR があればその下、なければ %USERPROFILE%\.claude.json
std::wstring ClaudeUserConfigPath()
{
	WCHAR sz[MAX_PATH];
	if( GetEnvironmentVariableW( L"CLAUDE_CONFIG_DIR", sz, MAX_PATH ) ){
		return std::wstring( sz ) + L"\\.claude.json";
	}
	if( GetEnvironmentVariableW( L"USERPROFILE", sz, MAX_PATH ) ){
		return std::wstring( sz ) + L"\\.claude.json";
	}
	return {};
}

std::string ReadFileUtf8( const std::wstring& path )
{
	std::string data;
	HANDLE h = CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr );
	if( h == INVALID_HANDLE_VALUE )  return data;
	LARGE_INTEGER size;
	if( GetFileSizeEx( h, &size ) && size.QuadPart < 64 * 1024 * 1024 ){
		data.resize( (size_t)size.QuadPart );
		DWORD read = 0;
		ReadFile( h, data.data(), (DWORD)data.size(), &read, nullptr );
		data.resize( read );
	}
	CloseHandle( h );
	return data;
}

bool WriteFileUtf8( const std::wstring& path, const std::string& data )
{
	HANDLE h = CreateFileW( path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr );
	if( h == INVALID_HANDLE_VALUE )  return false;
	DWORD written = 0;
	const BOOL ok = WriteFile( h, data.data(), (DWORD)data.size(), &written, nullptr );
	CloseHandle( h );
	return ok && written == data.size();
}

// ~/.claude.json のユーザースコープの MCP サーバー（"mcpServers"）。名前 → 設定
ee::mcp::json LoadUserMcpServers()
{
	const ee::mcp::json root = ee::mcp::json::parse( ReadFileUtf8( ClaudeUserConfigPath() ), nullptr, false );
	if( root.is_object() && root.contains( "mcpServers" ) && root["mcpServers"].is_object() ){
		return root["mcpServers"];
	}
	return ee::mcp::json::object();
}

struct SettingsDlgData
{
	ChatSettings*              settings;
	std::vector<std::wstring>  available;   // ~/.claude.json にある MCP サーバー名
};

INT_PTR CALLBACK SettingsDlgProc( HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam )
{
	switch( msg ){
	case WM_INITDIALOG: {
		SetWindowLongPtrW( hDlg, DWLP_USER, lParam );
		auto* d = reinterpret_cast<SettingsDlgData*>( lParam );
		FillCombo( GetDlgItem( hDlg, IDC_DEF_MODEL ), kModels, d->settings->model );
		FillCombo( GetDlgItem( hDlg, IDC_DEF_EFFORT ), kEfforts, d->settings->effort );
		CheckDlgButton( hDlg, IDC_DEF_WEB, d->settings->web ? BST_CHECKED : BST_UNCHECKED );

		HWND hList = GetDlgItem( hDlg, IDC_MCP_LIST );
		ListView_SetExtendedListViewStyle( hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT );
		RECT rc;
		GetClientRect( hList, &rc );
		LVCOLUMNW col = {};
		col.mask = LVCF_WIDTH;
		col.cx = rc.right - GetSystemMetrics( SM_CXVSCROLL );
		ListView_InsertColumn( hList, 0, &col );
		for( size_t i = 0; i < d->available.size(); i++ ){
			LVITEMW item = {};
			item.mask = LVIF_TEXT;
			item.iItem = (int)i;
			item.pszText = const_cast<LPWSTR>( d->available[i].c_str() );
			ListView_InsertItem( hList, &item );
			const bool on = std::find( d->settings->mcp.begin(), d->settings->mcp.end(), d->available[i] ) != d->settings->mcp.end();
			ListView_SetCheckState( hList, (int)i, on );
		}
		if( d->available.empty() ){
			SetDlgItemTextW( hDlg, IDC_MCP_NOTE, L"~/.claude.json にユーザー設定の MCP サーバーがありません。EmEditor 操作用のツールは常に有効です。" );
		}
		return TRUE;
	}
	case WM_COMMAND:
		if( LOWORD( wParam ) == IDOK ){
			auto* d = reinterpret_cast<SettingsDlgData*>( GetWindowLongPtrW( hDlg, DWLP_USER ) );
			d->settings->model = ComboValue( GetDlgItem( hDlg, IDC_DEF_MODEL ), kModels );
			d->settings->effort = ComboValue( GetDlgItem( hDlg, IDC_DEF_EFFORT ), kEfforts );
			d->settings->web = IsDlgButtonChecked( hDlg, IDC_DEF_WEB ) == BST_CHECKED;
			d->settings->mcp.clear();
			HWND hList = GetDlgItem( hDlg, IDC_MCP_LIST );
			for( size_t i = 0; i < d->available.size(); i++ ){
				if( ListView_GetCheckState( hList, (int)i ) )  d->settings->mcp.push_back( d->available[i] );
			}
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

constexpr LPCWSTR kSystemPrompt =
	L"あなたはテキストエディタ EmEditor に組み込まれたアシスタントです。"
	L"ユーザーは文書の執筆・メモの整理・文章の調整をしています。日本語で簡潔に答えてください。\n\n"
	L"emeditor の MCP ツールで、ユーザーが開いている文書を読み書きできます。\n"
	L"- ユーザーの各メッセージの先頭の <editor_state active_document=\"...\"> は、送信時点でアクティブだった文書の名前です。"
	L"本文は含まれないので、内容が必要なら get_active_document で読んでください。ユーザーはタブを切り替えながら作業するので、文書は会話の途中で変わります。\n"
	L"- 「この文書」「これ」「ここ」は、そのメッセージ時点のアクティブな文書（選択範囲があれば get_selection）を指すと解釈してください。\n"
	L"- 文書の修正を頼まれたら、回答に書くのではなくツールで文書を直接編集してください。"
	L"部分的な修正は replace_text、全体の整理し直しは set_document_text、元を残したい・別に出したいときは new_document を使います。\n"
	L"- 編集の前に必ず最新の内容を読み、変更は依頼された範囲に留めてください。編集はユーザーが Ctrl+Z で元に戻せます。\n"
	L"- 編集した後の返答は、何を変えたかを 1〜3 行で伝えるだけで十分です。変更後の全文を返答に書き写さないでください。\n"
	L"- ファイルの保存・読み込み、コマンドの実行はできません。";

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

// claude の作業フォルダ。--resume はカレントフォルダ単位でセッションを探すので、固定のフォルダにする。
std::wstring GetWorkDir()
{
	WCHAR sz[MAX_PATH];
	GetTempPathW( MAX_PATH, sz );
	std::wstring dir = std::wstring( sz ) + L"emeditor-claude-chat";
	CreateDirectoryW( dir.c_str(), nullptr );
	return dir;
}

std::wstring GetDlgItemString( HWND hDlg, int id )
{
	HWND hwnd = GetDlgItem( hDlg, id );
	const int len = GetWindowTextLengthW( hwnd );
	std::wstring s( len + 1, L'\0' );
	GetWindowTextW( hwnd, s.data(), len + 1 );
	s.resize( len );
	return s;
}

std::wstring Trim( std::wstring s )
{
	const auto isSpace = []( wchar_t c ){ return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\x3000'; };
	while( !s.empty() && isSpace( s.back() ) )  s.pop_back();
	size_t i = 0;
	while( i < s.size() && isSpace( s[i] ) )  i++;
	return s.substr( i );
}

std::wstring EscapeAttr( std::wstring_view s )
{
	std::wstring r;
	for( wchar_t c : s ){
		switch( c ){
		case L'&': r += L"&amp;";  break;
		case L'"': r += L"&quot;"; break;
		case L'<': r += L"&lt;";   break;
		case L'>': r += L"&gt;";   break;
		default:   r += c;
		}
	}
	return r;
}

// MCP サーバーのスレッドから UI スレッドへツール呼び出しを渡すための入れ物
struct ToolCall
{
	const std::string*      name;
	const ee::mcp::json*    args;
	ee::mcp::ToolResult     result;
};

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
	enum { _USE_CUSTOM_BAR				= TRUE	};   // カスタムバーを使う

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

		// 「アクティブ: ○○」の表示を更新する。タブ切り替え等ではすぐ、編集中はアイドル時にまとめて
		if( nEvent & ( EVENT_DOC_SEL_CHANGED | EVENT_FILE_OPENED | EVENT_DOC_CLOSE ) ){
			UpdateActiveLabel();
		}
		else if( nEvent & EVENT_CHANGE ){
			m_bLabelDirty = true;
		}
		// 新しい文書を作ったときなどは EVENT_DOC_SEL_CHANGED が来ないので、アイドル時に文書が変わっていないかも見る
		if( ( nEvent & EVENT_IDLE ) && ( m_bLabelDirty || ( HasView() && ee::doc::GetDocPath( m_hwndView ) != m_labelDocPath ) ) ){
			UpdateActiveLabel();
		}

		if( nEvent & EVENT_CREATE_FRAME ){
			// 前回、パネルを開いたままウィンドウを閉じていたら開き直す
			if( GetProfileInt( kProfileOpenStartup, FALSE ) ){
				OpenBar();
			}
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
				// 自分で閉じた場合は呼び出し元が保存する。ここで書くと上書きしてしまう
				if( !m_bClosingBySelf ){
					WriteProfileInt( kProfileOpenStartup, ( pInfo->dwFlags & CLOSED_FRAME_WINDOW ) ? TRUE : FALSE );
				}
			}
		}
	}

	// パネル内にフォーカスがあるとき、キー入力を EmEditor より先に受け取る
	BOOL PreTranslateMessage( HWND /*hwndView*/, MSG* pMsg )
	{
		if( !m_hDlg )  return FALSE;
		HWND hwndFocus = GetFocus();
		if( !hwndFocus || !IsChild( m_hDlg, hwndFocus ) )  return FALSE;

		if( pMsg->message == WM_KEYDOWN ){
			const bool bCtrl = GetKeyState( VK_CONTROL ) < 0;
			if( pMsg->wParam == VK_RETURN && bCtrl ){
				OnSend();
				return TRUE;
			}
			if( pMsg->wParam == VK_ESCAPE && !bCtrl ){
				Editor_ExecCommand( m_hWnd, EEID_ACTIVE_PANE );   // エディタにフォーカスを戻す
				return TRUE;
			}
		}
		return IsDialogMessageW( m_hDlg, pMsg );
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
		return TRUE;   // 「プロパティ」ボタンで設定画面を開く
	}

	BOOL SetProperties( HWND hDlg )
	{
		return ShowSettings( hDlg ) ? TRUE : FALSE;
	}

	// ---- その他のフック ----

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
		StopWorker();
		m_mcp.Stop();
		StopAuthThreads();
	}

private:
	// パネル
	HWND				m_hDlg = nullptr;
	UINT				m_nClientID = 0;
	bool				m_bClosingBySelf = false;
	bool				m_bShuttingDown = false;
	bool				m_bLabelDirty = false;
	std::wstring		m_labelDocPath;          // 「アクティブ: ○○」に出している文書
	HWND				m_hwndView = nullptr;    // 最後にイベントが来たビュー（ツールの操作対象）

	// 会話
	std::wstring		m_sessionId;             // 空なら次の送信で新しい会話を始める
	std::wstring		m_lastReply;
	ee::ProcessRunner	m_runner;
	std::thread			m_worker;
	ee::ProcessResult	m_result;                // ワーカーが書き、join 後に UI スレッドが読む
	std::atomic<bool>	m_running = false;
	ULONGLONG			m_startTick = 0;

	// EmEditor を操作するツールを提供する MCP サーバー（パネルが開いている間だけ動かす）
	ee::mcp::Server		m_mcp;
	std::wstring		m_mcpConfigFile;         // 送信ごとに作る --mcp-config の一時ファイル（完了時に削除）

	// 履歴欄。通常は WebView2（m_web）、使えなければリッチエディット（m_view）。m_hist が今使っている方
	WebHistory			m_web;
	chatview::View		m_view;
	IHistory*			m_hist = &m_view;

	TemplateStore		m_templates;             // お願いテンプレート（templates.json）

	// ログイン状態（claude auth status）とログイン（claude auth login）
	ee::ProcessRunner	m_authRunner;
	std::thread			m_authThread;
	ee::ProcessResult	m_authResult;
	std::thread			m_loginThread;           // ログイン用コンソールの終了を待つ
	HANDLE				m_hLoginStop = nullptr;   // パネルを閉じるときに m_loginThread を起こす
	bool				m_bLoginVisible = false;

	// ---- パネルの開閉 ----

	void OpenBar()
	{
		if( m_hDlg )  return;
		chatview::LoadRichEdit();   // ダイアログの "RICHEDIT50W" を作れるようにする
		// 仮の親はフレーム。Editor_CustomBarOpen がカスタムバーの子に付け替える
		m_hDlg = CreateDialogParamW( EEGetInstanceHandle(), MAKEINTRESOURCEW( IDD_CHAT ), m_hWnd, ChatDlgProc, (LPARAM)this );
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

		// ツール呼び出しはサーバーのスレッドに届く。ダイアログへ SendMessage して UI スレッドで実行する
		HWND hDlg = m_hDlg;
		const bool started = m_mcp.Start( kMcpServerName, ee::tools::Definitions(),
			[hDlg]( const std::string& name, const ee::mcp::json& args ){
				ToolCall call{ &name, &args, ee::mcp::ToolResult::Error( "EmEditor が応答しません" ) };
				DWORD_PTR ret = 0;
				if( !SendMessageTimeoutW( hDlg, WM_APP_TOOL_CALL, 0, (LPARAM)&call, SMTO_NORMAL, 15000, &ret ) ){
					return ee::mcp::ToolResult::Error( "EmEditor が応答しません（タイムアウト）" );
				}
				return call.result;
			} );
		if( !started ){
			AppendHistory( L"[警告] MCP サーバーを起動できませんでした。Claude は文書を読み書きできません。\n\n" );
		}

		ShowWindow( m_hDlg, SW_SHOW );
		UpdateActiveLabel();
		SetFocus( GetDlgItem( m_hDlg, IDC_INPUT ) );
		CheckAuthAsync();
	}

	void CloseBar()
	{
		if( !m_hDlg )  return;
		m_bClosingBySelf = true;
		Editor_CustomBarClose( m_hWnd, m_nClientID );
		m_bClosingBySelf = false;
		BarClosed();   // 念のため（通常は EVENT_CUSTOM_BAR_CLOSED 経由でも呼ばれる）
	}

	void BarClosed()
	{
		// ダイアログが消える前に、ワーカー（claude）と MCP サーバーを止める。
		// 止めている間に届いたツール呼び出しは実行しない（claude はもう結果を受け取らない）
		m_bShuttingDown = true;
		StopWorker();
		m_mcp.Stop();
		StopAuthThreads();
		m_bShuttingDown = false;
		if( m_hDlg ){
			m_web.Close();
			m_hist = &m_view;
			if( IsWindow( m_hDlg ) )  DestroyWindow( m_hDlg );
			m_hDlg = nullptr;
		}
		m_nClientID = 0;
	}

	void StopWorker()
	{
		if( m_worker.joinable() ){
			m_runner.Cancel();
			ee::mcp::JoinPumpingSentMessages( m_worker );
		}
		m_running = false;
	}

	// ---- 設定 ----

	ChatSettings LoadSettings()
	{
		ChatSettings s;
		WCHAR sz[1024];
		GetProfileString( kProfileModel, sz, _countof( sz ), s.model.c_str() );
		s.model = sz;
		GetProfileString( kProfileEffort, sz, _countof( sz ), s.effort.c_str() );
		s.effort = sz;
		s.web = GetProfileInt( kProfileWeb, FALSE ) != 0;
		GetProfileString( kProfileMcp, sz, _countof( sz ), L"" );
		s.mcp = SplitComma( sz );
		return s;
	}

	void SaveSettings( const ChatSettings& s )
	{
		WriteProfileString( kProfileModel, s.model.c_str() );
		WriteProfileString( kProfileEffort, s.effort.c_str() );
		WriteProfileInt( kProfileWeb, s.web ? TRUE : FALSE );
		WriteProfileString( kProfileMcp, JoinComma( s.mcp ).c_str() );
	}

	// 設定画面。パネルの表示中の値（モデル等）は変えない（既定値は次にパネルを開いたときから）
	bool ShowSettings( HWND hwndOwner )
	{
		ChatSettings s = LoadSettings();
		SettingsDlgData data{ &s, {} };
		try {
			// 注意: `for( … : LoadUserMcpServers().items() )` と書くと、items() が参照する一時オブジェクトが
			// ループ開始前に破棄され、EmEditor ごと落ちる（C++17 の range-for は一時オブジェクトの寿命を延ばさない）
			const ee::mcp::json servers = LoadUserMcpServers();
			for( auto& [name, cfg] : servers.items() ){
				if( name != kMcpServerName )  data.available.push_back( ee::Utf8ToWide( name ) );
			}
		}
		catch( const std::exception& ){
			// 一覧が作れなくても設定画面は開く
		}
		if( DialogBoxParamW( EEGetInstanceHandle(), MAKEINTRESOURCEW( IDD_SETTINGS ), hwndOwner, SettingsDlgProc, (LPARAM)&data ) != IDOK ){
			return false;
		}
		SaveSettings( s );
		return true;
	}

	// claude -p のコマンドラインを組み立てる。MCP の設定は一時ファイルに書く
	// （ユーザーの MCP 設定には API キーなどが含まれうるので、コマンドラインには載せない）
	std::wstring BuildCommand( const std::wstring& exe, const std::wstring& model, const std::wstring& effort, bool web, std::wstring& mcpNote )
	{
		std::wstring cmd = ee::QuoteArg( exe );
		// --strict-mcp-config: ユーザー設定の MCP サーバーは読み込まず、--mcp-config で渡したものだけを使う
		cmd += L" -p --strict-mcp-config --output-format json";
		cmd += L" --model " + ee::QuoteArg( model ) + L" --effort " + ee::QuoteArg( effort );
		// 組み込みツールは Web 検索・取得だけ（ファイル操作・コマンド実行は無効のまま）
		cmd += web ? L" --tools \"WebSearch,WebFetch\"" : L" --tools \"\"";
		cmd += L" --system-prompt " + ee::QuoteArg( kSystemPrompt );

		std::vector<std::wstring> allowed;   // -p では確認できないので、使ってよいツールを明示する
		if( web ){
			allowed.push_back( L"WebSearch" );
			allowed.push_back( L"WebFetch" );
		}

		ee::mcp::json servers = ee::mcp::json::object();
		if( m_mcp.IsRunning() ){
			servers[m_mcp.Name()] = m_mcp.ConfigEntry();
			allowed.push_back( L"mcp__" + ee::Utf8ToWide( m_mcp.Name() ) );
		}
		const ChatSettings settings = LoadSettings();
		if( !settings.mcp.empty() ){
			const ee::mcp::json user = LoadUserMcpServers();
			for( const std::wstring& nameW : settings.mcp ){
				const std::string name = ee::WideToUtf8( nameW );
				if( name == kMcpServerName || !user.contains( name ) )  continue;
				servers[name] = user[name];
				allowed.push_back( L"mcp__" + nameW );
				mcpNote += ( mcpNote.empty() ? L" / MCP: " : L", " ) + nameW;
			}
		}
		if( !servers.empty() ){
			const std::wstring file = GetWorkDir() + L"\\mcp-" + std::to_wstring( GetCurrentProcessId() ) + L"-" + std::to_wstring( GetTickCount64() ) + L".json";
			if( WriteFileUtf8( file, ee::mcp::json{ { "mcpServers", servers } }.dump() ) ){
				m_mcpConfigFile = file;
				cmd += L" --mcp-config " + ee::QuoteArg( file );
			}
		}
		if( !allowed.empty() ){
			cmd += L" --allowedTools " + ee::QuoteArg( JoinComma( allowed ) );
		}
		if( !m_sessionId.empty() ){
			cmd += L" --resume " + ee::QuoteArg( m_sessionId );
		}
		return cmd;
	}

	// ---- ログイン ----

	// claude auth status --json を裏で実行し、結果を WM_APP_AUTH_DONE で受け取る
	void CheckAuthAsync()
	{
		if( !m_hDlg || m_authThread.joinable() )  return;
		const std::wstring exe = FindClaudeExe();
		if( exe.empty() )  return;
		const std::wstring cmd = ee::QuoteArg( exe ) + L" auth status --json";
		m_authThread = std::thread( [this, cmd, hDlg = m_hDlg]{
			m_authResult = m_authRunner.Run( cmd, GetWorkDir(), {} );
			PostMessageW( hDlg, WM_APP_AUTH_DONE, 0, 0 );
		} );
	}

	void OnAuthDone()
	{
		if( m_authThread.joinable() )  m_authThread.join();
		const ee::ProcessResult& r = m_authResult;
		if( !r.started || r.canceled )  return;
		bool loggedIn = true;
		if( !ee::json::GetBool( r.out, "loggedIn", loggedIn ) )  return;   // 判定できなければ何もしない
		const bool wasVisible = m_bLoginVisible;
		ShowLoginButton( !loggedIn );
		if( !loggedIn ){
			AppendHistory( L"[ログイン] Claude Code にログインしていません。「ログイン」ボタンからログインしてください。\n\n" );
		}
		else if( wasVisible ){
			AppendHistory( L"[ログイン] ログインしました。\n\n" );
		}
	}

	void ShowLoginButton( bool show )
	{
		if( !m_hDlg )  return;
		m_bLoginVisible = show;
		ShowWindow( GetDlgItem( m_hDlg, IDC_LOGIN ), show ? SW_SHOW : SW_HIDE );
		RECT rc;
		GetClientRect( m_hDlg, &rc );
		Layout( m_hDlg, rc.right, rc.bottom );
	}

	// claude auth login を新しいコンソール窓で起動する（ブラウザでの OAuth と、必要ならコードの貼り付けをユーザーが行う）。
	// コンソールが閉じたら、ログイン状態を確認し直す
	void OnLogin()
	{
		if( m_loginThread.joinable() ){
			MessageBeep( MB_ICONINFORMATION );   // すでにログイン用の窓が開いている
			return;
		}
		const std::wstring exe = FindClaudeExe();
		if( exe.empty() ){
			AppendHistory( L"[エラー] claude.exe が見つかりません。Claude Code をインストールしてください。\n\n" );
			return;
		}
		std::wstring cmd = ee::QuoteArg( exe ) + L" auth login";
		STARTUPINFOW si = { sizeof( si ) };
		PROCESS_INFORMATION pi = {};
		if( !CreateProcessW( nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
				nullptr, GetWorkDir().c_str(), &si, &pi ) ){
			AppendHistory( L"[エラー] claude auth login を起動できません (error " + std::to_wstring( GetLastError() ) + L")\n\n" );
			return;
		}
		CloseHandle( pi.hThread );
		AppendHistory( L"[ログイン] ログイン用の窓を開きました。ブラウザでログインを済ませ、窓が閉じるのを待ってください。\n\n" );

		if( !m_hLoginStop )  m_hLoginStop = CreateEventW( nullptr, TRUE, FALSE, nullptr );
		ResetEvent( m_hLoginStop );
		m_loginThread = std::thread( [hProcess = pi.hProcess, hStop = m_hLoginStop, hDlg = m_hDlg]{
			HANDLE handles[] = { hProcess, hStop };
			const DWORD w = WaitForMultipleObjects( 2, handles, FALSE, INFINITE );
			CloseHandle( hProcess );
			if( w == WAIT_OBJECT_0 )  PostMessageW( hDlg, WM_APP_LOGIN_EXIT, 0, 0 );
		} );
	}

	void OnLoginExit()
	{
		if( m_loginThread.joinable() )  m_loginThread.join();
		CheckAuthAsync();
	}

	// パネルを閉じるとき。ログイン用の窓そのものは閉じない（ユーザーがログイン中かもしれない）
	void StopAuthThreads()
	{
		if( m_authThread.joinable() ){
			m_authRunner.Cancel();
			ee::mcp::JoinPumpingSentMessages( m_authThread );
		}
		if( m_loginThread.joinable() ){
			SetEvent( m_hLoginStop );
			m_loginThread.join();
		}
		if( m_hLoginStop ){
			CloseHandle( m_hLoginStop );
			m_hLoginStop = nullptr;
		}
		m_bLoginVisible = false;
	}

	bool HasView() const
	{
		return m_hwndView && IsWindow( m_hwndView );
	}

	void UpdateActiveLabel()
	{
		m_bLabelDirty = false;
		if( !m_hDlg || !HasView() )  return;
		m_labelDocPath = ee::doc::GetDocPath( m_hwndView );
		const UINT_PTR lines = Editor_GetLines( m_hwndView, POS_LOGICAL_W );
		const std::wstring label = L"アクティブ: " + ee::tools::GetDocName( m_hwndView ) + L"（" + std::to_wstring( lines ) + L" 行）";
		SetDlgItemTextW( m_hDlg, IDC_ACTIVE_DOC, label.c_str() );
	}

	// ---- ダイアログ ----

	static INT_PTR CALLBACK ChatDlgProc( HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam )
	{
		if( msg == WM_INITDIALOG ){
			SetWindowLongPtrW( hDlg, DWLP_USER, lParam );
			reinterpret_cast<CMyFrame*>( lParam )->OnInitDialog( hDlg );
			return FALSE;   // フォーカスは OpenBar で設定する
		}
		auto* pThis = reinterpret_cast<CMyFrame*>( GetWindowLongPtrW( hDlg, DWLP_USER ) );
		if( !pThis )  return FALSE;

		switch( msg ){
		case WM_SIZE:
			pThis->Layout( hDlg, LOWORD( lParam ), HIWORD( lParam ) );
			return TRUE;
		case WM_COMMAND:
			switch( LOWORD( wParam ) ){
			case IDC_SEND:     pThis->OnSend();    return TRUE;
			case IDC_NEW_CHAT: pThis->OnNewChat(); return TRUE;
			case IDC_INSERT:   pThis->OnInsert();  return TRUE;
			case IDC_LOGIN:    pThis->OnLogin();   return TRUE;
			case IDC_SETTINGS: pThis->ShowSettings( hDlg ); return TRUE;
			}
			break;
		case WM_TIMER:
			if( wParam == kThinkingTimer )  pThis->OnThinkingTick();
			if( wParam == kTemplateTimer )  pThis->RefreshTemplates( false );
			return TRUE;
		case WM_NOTIFY: {
			const NMHDR* hdr = reinterpret_cast<const NMHDR*>( lParam );
			if( hdr->idFrom == IDC_HISTORY && hdr->code == EN_LINK ){
				const ENLINK* link = reinterpret_cast<const ENLINK*>( lParam );
				if( link->msg == WM_LBUTTONUP )  pThis->OnLinkClicked( *link );
				return TRUE;
			}
			break;
		}
		case WM_APP_AUTH_DONE:
			pThis->OnAuthDone();
			return TRUE;
		case WM_APP_LOGIN_EXIT:
			pThis->OnLoginExit();
			return TRUE;
		case WM_APP_CHAT_DONE:
			pThis->OnDone();
			return TRUE;
		case WM_APP_TOOL_CALL:
			pThis->OnToolCall( *reinterpret_cast<ToolCall*>( lParam ) );
			return TRUE;
		}
		return FALSE;
	}

	void OnInitDialog( HWND hDlg )
	{
		SendDlgItemMessageW( hDlg, IDC_INPUT, EM_SETLIMITTEXT, 0, 0 );   // 既定の 32K 文字制限を外す
		m_view.Attach( GetDlgItem( hDlg, IDC_HISTORY ) );

		// 履歴は WebView2 で描く。使えなければ（Runtime が無いなど）リッチエディットに切り替える
		ShowWindow( GetDlgItem( hDlg, IDC_HISTORY ), SW_HIDE );
		m_hist = &m_web;
		WebHistory::Callbacks cb;
		cb.onCopy = [this]( const std::wstring& text ){ CopyToClipboard( text ); };
		cb.onInsert = [this]( const std::wstring& text ){ InsertToEditor( text ); };
		cb.onEscape = [this]{ Editor_ExecCommand( m_hWnd, EEID_ACTIVE_PANE ); };
		cb.onError = [this]( HRESULT hr ){ FallbackToRichEdit( hr ); };
		cb.onTemplate = [this]( int index ){ OnRunTemplate( index ); };
		cb.onEditTemplates = [this]{ OpenTemplatesFile(); };
		m_web.Create( hDlg, BuildChatHtml( EEGetInstanceHandle(), IDR_CHAT_HTML, IDR_MARKDOWN_CSS ),
			ee::Utf8ToWide( ee::web::LoadResourceUtf8( EEGetInstanceHandle(), IDR_MARKED_JS ) ), std::move( cb ) );

		m_hist->Notice( L"Ctrl+Enter で送信 / Esc でエディタに戻る\nClaude はアクティブな文書を読み書きできます（編集は Ctrl+Z で戻せます）" );

		// お願いテンプレート。ファイルが保存されたら読み込み直す（2 秒ごとに更新日時を見る）
		RefreshTemplates( true );
		SetTimer( hDlg, kTemplateTimer, 2000, nullptr );

		// モデル・Effort・Web は、設定画面の既定値で始める
		const ChatSettings def = LoadSettings();
		FillCombo( GetDlgItem( hDlg, IDC_MODEL ), kModels, def.model );
		FillCombo( GetDlgItem( hDlg, IDC_EFFORT ), kEfforts, def.effort );
		CheckDlgButton( hDlg, IDC_WEB, def.web ? BST_CHECKED : BST_UNCHECKED );
	}

	void Layout( HWND hDlg, int cx, int cy )
	{
		const UINT dpi = GetDpiForWindow( hDlg );
		auto px = [dpi]( int v ){ return MulDiv( v, dpi, 96 ); };
		const int m = px( 4 ), btnH = px( 26 ), labelH = px( 20 ), inputH = px( 96 ), topH = px( 24 );
		const int btnW = std::max( px( 40 ), std::min( px( 90 ), ( cx - m * 4 ) / 3 ) );

		const int yHistory = m + topH + m;
		const int yBtn = cy - m - btnH;
		const int yLabel = yBtn - m - labelH;
		const int yInput = yLabel - m - inputH;
		const int hHistory = std::max( 0, yInput - m - yHistory );

		HDWP hdwp = BeginDeferWindowPos( 11 );
		auto place = [&]( int id, int x, int y, int w, int h ){
			hdwp = DeferWindowPos( hdwp, GetDlgItem( hDlg, id ), nullptr, x, y, std::max( 0, w ), std::max( 0, h ), SWP_NOZORDER | SWP_NOACTIVATE );
		};
		// 上部の 1 行: [モデル][Effort][Web][設定]。コンボボックスの高さはドロップダウン部分を含む
		const int webW = px( 52 ), setW = px( 44 );
		const int comboW = std::max( px( 50 ), ( cx - m * 5 - webW - setW ) / 2 );
		place( IDC_MODEL, m, m, comboW, px( 240 ) );
		place( IDC_EFFORT, m * 2 + comboW, m, comboW, px( 240 ) );
		place( IDC_WEB, m * 3 + comboW * 2, m, webW, topH );
		place( IDC_SETTINGS, cx - m - setW, m, setW, topH );
		place( IDC_HISTORY, m, yHistory, cx - m * 2, hHistory );
		m_web.SetBounds( RECT{ m, yHistory, cx - m, yHistory + hHistory } );
		place( IDC_INPUT, m, yInput, cx - m * 2, inputH );
		// ログインボタンは表示中だけ、ラベルの行の右端に置く
		const int loginW = m_bLoginVisible ? btnW : 0;
		place( IDC_ACTIVE_DOC, m, yLabel, cx - m * 2 - ( loginW ? loginW + m : 0 ), labelH );
		place( IDC_LOGIN, cx - m - btnW, yLabel, btnW, labelH );
		place( IDC_SEND, cx - m - btnW, yBtn, btnW, btnH );
		place( IDC_INSERT, cx - ( m + btnW ) * 2, yBtn, btnW, btnH );
		place( IDC_NEW_CHAT, cx - ( m + btnW ) * 3, yBtn, btnW, btnH );
		EndDeferWindowPos( hdwp );
	}

	// お知らせ・エラーを履歴に出す（[エラー] / [警告] で始まるものは赤字）
	void AppendHistory( std::wstring_view text )
	{
		if( !m_hDlg )  return;
		const std::wstring t = Trim( std::wstring( text ) );
		if( t.rfind( L"[エラー]", 0 ) == 0 || t.rfind( L"[警告]", 0 ) == 0 )  m_hist->Error( t );
		else                                                                  m_hist->Notice( t );
	}

	void RefreshTemplates( bool force )
	{
		if( !m_templates.Reload( force ) )  return;
		std::vector<IHistory::TemplateButton> buttons;
		for( const ChatTemplate& t : m_templates.Items() ){
			buttons.push_back( { t.icon, t.name, t.prompt } );
		}
		m_hist->SetTemplates( buttons, m_templates.LastError() );
	}

	// 「＋ 編集」: テンプレートの JSON を EmEditor で開く
	void OpenTemplatesFile()
	{
		m_templates.Reload( false );   // 無ければ既定の内容で作る
		if( !HasView() )  return;
		LOAD_FILE_INFO_EX lfi = { sizeof( lfi ) };
		lfi.nCP = CP_UTF8;
		lfi.bDetectUTF8 = TRUE;
		lfi.nFlags = LFI_ALLOW_NEW_WINDOW;
		if( !Editor_LoadFileW( m_hwndView, &lfi, m_templates.Path().c_str() ) ){
			AppendHistory( L"[エラー] テンプレートのファイルを開けません: " + m_templates.Path() );
		}
	}

	void OnThinkingTick()
	{
		if( m_running )  m_hist->TickThinking();
	}

	// WebView2 が使えないとき。以降の履歴はリッチエディットに出す
	void FallbackToRichEdit( HRESULT hr )
	{
		if( !m_hDlg || m_hist == &m_view )  return;
		m_web.Close();
		m_hist = &m_view;
		ShowWindow( GetDlgItem( m_hDlg, IDC_HISTORY ), SW_SHOW );
		WCHAR sz[160];
		StringCchPrintfW( sz, _countof( sz ), L"WebView2 を使えないため、簡易表示に切り替えました (0x%08lX)。WebView2 Runtime の有無を確認してください。", (unsigned long)hr );
		m_view.Notice( sz );
	}

	void CopyToClipboard( const std::wstring& text )
	{
		if( !OpenClipboard( m_hDlg ) )  return;
		EmptyClipboard();
		const size_t bytes = ( text.size() + 1 ) * sizeof( wchar_t );
		if( HGLOBAL h = GlobalAlloc( GMEM_MOVEABLE, bytes ) ){
			memcpy( GlobalLock( h ), text.c_str(), bytes );
			GlobalUnlock( h );
			if( !SetClipboardData( CF_UNICODETEXT, h ) )  GlobalFree( h );
		}
		CloseClipboard();
	}

	// 履歴の「エディタに挿入」。カーソル位置（選択中なら置き換え）に入れる。Ctrl+Z で戻せる
	void InsertToEditor( const std::wstring& text )
	{
		if( !HasView() )  return;
		Editor_InsertW( m_hwndView, ee::tools::ToLf( text ).c_str(), true );
		Editor_ExecCommand( m_hWnd, EEID_ACTIVE_PANE );
	}

	// 履歴のリンクがクリックされた。http(s) だけをブラウザで開く
	void OnLinkClicked( const ENLINK& link )
	{
		const std::wstring url = m_view.LinkAt( link.chrg );
		if( url.rfind( L"http://", 0 ) == 0 || url.rfind( L"https://", 0 ) == 0 ){
			ShellExecuteW( m_hDlg, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
		}
	}

	void SetBusy( bool busy )
	{
		SetDlgItemTextW( m_hDlg, IDC_SEND, busy ? L"キャンセル" : L"送信" );
		EnableWindow( GetDlgItem( m_hDlg, IDC_NEW_CHAT ), !busy );
	}

	// ---- 操作 ----

	// 送信ボタン / Ctrl+Enter。応答待ちならキャンセル
	void OnSend()
	{
		if( !m_hDlg )  return;
		if( m_running ){
			m_runner.Cancel();
			return;
		}
		const std::wstring text = Trim( GetDlgItemString( m_hDlg, IDC_INPUT ) );
		if( text.empty() )  return;
		SendOptions opt = PanelOptions();
		Send( text, text, opt );
	}

	// お願いテンプレートのボタン。入力欄に書いてあれば「補足」として付ける
	void OnRunTemplate( int index )
	{
		if( !m_hDlg || m_running )  return;
		const auto& items = m_templates.Items();
		if( index < 0 || (size_t)index >= items.size() )  return;
		const ChatTemplate& t = items[index];

		const std::wstring supplement = Trim( GetDlgItemString( m_hDlg, IDC_INPUT ) );
		std::wstring text = t.prompt;
		std::wstring display = ( t.icon.empty() ? L"" : t.icon + L" " ) + t.name;
		if( !supplement.empty() ){
			text += L"\n\n補足: " + supplement;
			display += L"\n" + supplement;
		}

		SendOptions opt = PanelOptions();
		if( IsChoice( kModels, t.model ) )    opt.model = t.model;
		if( IsChoice( kEfforts, t.effort ) )  opt.effort = t.effort;
		if( t.web )                           opt.web = *t.web;

		if( t.newChat && !m_sessionId.empty() ){
			m_sessionId.clear();   // これまでの会話を引きずらない（履歴の表示は残す）
			m_hist->Notice( L"── 新しい会話 ──" );
		}
		Send( display, text, opt );
	}

	struct SendOptions
	{
		std::wstring model;
		std::wstring effort;
		bool         web = false;
	};

	SendOptions PanelOptions() const
	{
		SendOptions opt;
		opt.model = ComboValue( GetDlgItem( m_hDlg, IDC_MODEL ), kModels );
		opt.effort = ComboValue( GetDlgItem( m_hDlg, IDC_EFFORT ), kEfforts );
		opt.web = IsDlgButtonChecked( m_hDlg, IDC_WEB ) == BST_CHECKED;
		return opt;
	}

	// display: 履歴に出す発言 / text: Claude に送る依頼文
	void Send( const std::wstring& display, const std::wstring& text, const SendOptions& opt )
	{
		const std::wstring exe = FindClaudeExe();
		if( exe.empty() ){
			AppendHistory( L"[エラー] claude.exe が見つかりません。Claude Code をインストールしてください。\n\n" );
			return;
		}

		// 本文は送らず、アクティブな文書の名前だけを伝える（内容は Claude がツールで取りに来る）
		std::wstring message = text;
		if( HasView() ){
			message = L"<editor_state active_document=\"" + EscapeAttr( ee::tools::GetDocName( m_hwndView ) ) + L"\" />\n\n" + text;
		}

		std::wstring mcpNote;
		const std::wstring cmd = BuildCommand( exe, opt.model, opt.effort, opt.web, mcpNote );

		const std::wstring meta = ChoiceLabel( kModels, opt.model ) + L" / " + opt.effort + ( opt.web ? L" / Web" : L"" ) + mcpNote;
		m_hist->User( display, meta );
		SetDlgItemTextW( m_hDlg, IDC_INPUT, L"" );
		SetBusy( true );
		Editor_SetStatusW( m_hWnd, L"Claude が考え中..." );

		if( m_worker.joinable() )  m_worker.join();
		m_running = true;
		m_startTick = GetTickCount64();
		m_hist->StartThinking();
		SetTimer( m_hDlg, kThinkingTimer, 400, nullptr );
		// 本文は stdin で渡す（コマンドライン長の制限を避ける）
		m_worker = std::thread( [this, cmd, input = ee::WideToUtf8( message ), dir = GetWorkDir(), hDlg = m_hDlg]{
			m_result = m_runner.Run( cmd, dir, input );
			PostMessageW( hDlg, WM_APP_CHAT_DONE, 0, 0 );
		} );
	}

	// MCP サーバーのスレッドから SendMessage で届く。UI スレッドで EmEditor を操作する
	void OnToolCall( ToolCall& call )
	{
		if( m_bShuttingDown || !m_running ){
			call.result = ee::mcp::ToolResult::Error( "パネルが閉じられました" );
			return;
		}
		try {
			std::wstring log;
			call.result = ee::tools::Execute( m_hwndView, *call.name, *call.args, log );
			if( call.result.isError ){
				m_hist->Tool( L"⚠ " + ee::Utf8ToWide( *call.name ) + L": " + ee::Utf8ToWide( call.result.text ) );
			}
			else if( !log.empty() ){
				m_hist->Tool( log );
			}
			UpdateActiveLabel();
		}
		catch( const std::exception& e ){
			call.result = ee::mcp::ToolResult::Error( std::string( "内部エラー: " ) + e.what() );
		}
	}

	void OnDone()
	{
		if( m_worker.joinable() )  m_worker.join();
		m_running = false;
		KillTimer( m_hDlg, kThinkingTimer );
		m_hist->StopThinking();
		SetBusy( false );
		if( !m_mcpConfigFile.empty() ){
			DeleteFileW( m_mcpConfigFile.c_str() );   // ユーザーの MCP 設定（秘密情報を含みうる）は残さない
			m_mcpConfigFile.clear();
		}

		const ee::ProcessResult& r = m_result;
		const double sec = ( GetTickCount64() - m_startTick ) / 1000.0;
		WCHAR sz[128];

		if( !r.started ){
			StringCchPrintfW( sz, _countof( sz ), L"[エラー] claude を起動できません (error %lu)\n\n", r.error );
			AppendHistory( sz );
		}
		else if( r.canceled ){
			AppendHistory( L"（キャンセルしました）\n\n" );
		}
		else {
			std::string result, sessionId;
			bool isError = false;
			const bool parsed = ee::json::GetString( r.out, "result", result );
			ee::json::GetString( r.out, "session_id", sessionId );
			ee::json::GetBool( r.out, "is_error", isError );

			if( !parsed || isError || r.exitCode != 0 ){
				StringCchPrintfW( sz, _countof( sz ), L"[エラー] claude が失敗しました (exit %lu)\n", r.exitCode );
				std::wstring detail = ee::Utf8ToWide( parsed ? result : r.out ) + ee::Utf8ToWide( r.err );
				AppendHistory( sz + Trim( detail ) + L"\n\n" );
				// 未ログインなら "Not logged in · Please run /login" が返る（2026-10-24 / 2.1.302 で確認）
				if( detail.find( L"Not logged in" ) != std::wstring::npos ){
					ShowLoginButton( true );
					AppendHistory( L"[ログイン] 「ログイン」ボタンからログインしてください。\n\n" );
				}
			}
			else {
				if( !sessionId.empty() )  m_sessionId = ee::Utf8ToWide( sessionId );
				m_lastReply = Trim( ee::Utf8ToWide( result ) );
				m_hist->Claude( sec, m_lastReply );
			}
		}
		Editor_SetStatusW( m_hWnd, L"" );
		SetFocus( GetDlgItem( m_hDlg, IDC_INPUT ) );
	}

	void OnNewChat()
	{
		if( m_running )  return;
		m_sessionId.clear();
		m_lastReply.clear();
		m_hist->Clear();
		m_hist->Notice( L"（新しい会話）" );
		SetFocus( GetDlgItem( m_hDlg, IDC_INPUT ) );
	}

	// 最後の応答を、エディタのカーソル位置（選択中なら置き換え）に入れる。Ctrl+Z で戻せる
	void OnInsert()
	{
		if( m_lastReply.empty() || !HasView() ){
			MessageBeep( MB_ICONWARNING );
			return;
		}
		Editor_InsertW( m_hwndView, m_lastReply.c_str(), true );
		Editor_ExecCommand( m_hWnd, EEID_ACTIVE_PANE );
	}
};

// CMyFrame の定義の後に必須
_ETL_IMPLEMENT
