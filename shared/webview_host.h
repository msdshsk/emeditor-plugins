// WebView2 をプラグインのウィンドウの中に埋め込むための部品。チャット履歴・Markdown プレビューで使う。
//
// 使い方:
//   1. Create( 親 HWND, コールバック ) … 非同期で初期化される。準備ができたら onReady
//   2. NavigateToString( html )        … HTML を表示（2MB まで）。外部ファイルは読まない
//   3. PostJson( json )                … ページの window.chrome.webview の 'message' イベントへ送る
//   4. ページからの window.chrome.webview.postMessage(obj) は onMessage( JSON 文字列 ) に届く
//   5. SetBounds( rect ) で位置合わせ、Close() で破棄
//
// 安全策:
//   - 最初の NavigateToString 以外の遷移はすべて取り消す。http(s) のリンクは既定のブラウザで開く
//   - 新しいウィンドウ（target=_blank 等）も同様に既定のブラウザへ
//   - 開発者ツール（Release のみ無効）・ホストオブジェクト・既定のスクリプトダイアログは無効
//   - ページ側の HTML には CSP を書くこと（このヘッダーでは強制できない）
//
// 注意:
//   - UI スレッドで使う（COM の STA。EmEditor の UI スレッドは初期化済み）
//   - ユーザーデータフォルダは %LOCALAPPDATA%\EmEditorPlugins\WebView2。同じフォルダを使う環境は、作成オプションを揃える必要がある
//   - WebView2 の中にフォーカスがあるときのキー入力は、別プロセスで処理されるので PreTranslateMessage には来ない。
//     必要なキー（Esc など）は AcceleratorKeyPressed で拾う（onKey）
#pragma once

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <wrl.h>
#include <WebView2.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "WebView2LoaderStatic.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")   // WebView2LoaderStatic.lib がレジストリ・ETW の API を使う
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace ee::web {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

inline bool IsHttpUrl( const std::wstring& url )
{
	return url.rfind( L"http://", 0 ) == 0 || url.rfind( L"https://", 0 ) == 0;
}

inline void OpenExternal( const std::wstring& url )
{
	if( IsHttpUrl( url ) )  ShellExecuteW( nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL );
}

// リソース（RCDATA）を UTF-8 文字列として読む
inline std::string LoadResourceUtf8( HINSTANCE hinst, int id )
{
	HRSRC hrsrc = FindResourceW( hinst, MAKEINTRESOURCEW( id ), MAKEINTRESOURCEW( 10 ) );   // 10 = RT_RCDATA（UNICODE 未定義でも W 版で渡す）
	if( !hrsrc )  return {};
	HGLOBAL h = LoadResource( hinst, hrsrc );
	const char* p = h ? static_cast<const char*>( LockResource( h ) ) : nullptr;
	if( !p )  return {};
	std::string s( p, SizeofResource( hinst, hrsrc ) );
	if( s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF )  s.erase( 0, 3 );   // BOM
	return s;
}

// CSP の nonce などに使うランダムな 16 進文字列
inline std::string RandomHex( size_t bytes = 16 )
{
	std::string r;
	std::vector<unsigned char> buf( bytes );
	if( BCryptGenRandom( nullptr, buf.data(), (ULONG)buf.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG ) != 0 )  return r;
	static const char hex[] = "0123456789abcdef";
	for( unsigned char b : buf ){ r += hex[b >> 4]; r += hex[b & 15]; }
	return r;
}

// HTML テンプレートの {{KEY}} を置き換える。値の中に {{...}} があっても再置換しないよう、前から順に 1 回ずつ
inline std::string FillTemplate( std::string html, const std::vector<std::pair<std::string, std::string>>& values )
{
	for( const auto& [key, value] : values ){
		const std::string k = "{{" + key + "}}";
		for( size_t p = html.find( k ); p != std::string::npos; p = html.find( k, p + value.size() ) ){
			html.replace( p, k.size(), value );
		}
	}
	return html;
}

inline std::wstring UserDataFolder()
{
	PWSTR local = nullptr;
	std::wstring dir;
	if( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_LocalAppData, 0, nullptr, &local ) ) ){
		dir = std::wstring( local ) + L"\\EmEditorPlugins";
		CreateDirectoryW( dir.c_str(), nullptr );
		dir += L"\\WebView2";
		CreateDirectoryW( dir.c_str(), nullptr );
	}
	CoTaskMemFree( local );
	return dir;
}

class Host
{
public:
	struct Callbacks
	{
		std::function<void()>                       onReady;     // 最初の NavigateToString の読み込み完了後
		std::function<void( const std::wstring& )>  onMessage;   // ページからの postMessage（JSON 文字列）
		std::function<void( HRESULT )>              onError;     // 初期化に失敗（Runtime が無いなど）
		std::function<bool( UINT vkey )>            onKey;       // キーが押された。true を返すとページには渡さない
	};

	~Host(){ Close(); }

	// userDataFolder: 省略時は %LOCALAPPDATA%\EmEditorPlugins\WebView2。
	// 別プロセス（テスト用 exe など）が EmEditor と同じフォルダを使うと、起動が極端に遅くなったり進まなくなったりした
	void Create( HWND hwndParent, Callbacks cb, const std::wstring& userDataFolder = {} )
	{
		m_hwndParent = hwndParent;
		m_cb = std::move( cb );
		m_alive = std::make_shared<bool>( true );
		std::weak_ptr<bool> alive = m_alive;

		const std::wstring dataFolder = userDataFolder.empty() ? UserDataFolder() : userDataFolder;
		const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions( nullptr, dataFolder.c_str(), nullptr,
			Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[this, alive]( HRESULT result, ICoreWebView2Environment* env ) -> HRESULT {
					if( alive.expired() )  return S_OK;   // 初期化中に Close された
					if( FAILED( result ) || !env ){ Fail( result ); return S_OK; }
					return env->CreateCoreWebView2Controller( m_hwndParent,
						Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
							[this, alive]( HRESULT result, ICoreWebView2Controller* controller ) -> HRESULT {
								if( alive.expired() )  return S_OK;
								if( FAILED( result ) || !controller ){ Fail( result ); return S_OK; }
								OnControllerCreated( controller );
								return S_OK;
							} ).Get() );
				} ).Get() );
		if( FAILED( hr ) )  Fail( hr );
	}

	void Close()
	{
		m_alive.reset();
		m_scriptsAdded = false;
		m_appliedHost.clear();
		if( m_controller )  m_controller->Close();
		m_webview.Reset();
		m_controller.Reset();
		m_ready = false;
	}

	bool IsReady() const { return m_ready; }

	void SetBounds( const RECT& rc )
	{
		m_bounds = rc;
		if( m_controller )  m_controller->put_Bounds( rc );
	}

	void SetVisible( bool visible )
	{
		if( m_controller )  m_controller->put_IsVisible( visible ? TRUE : FALSE );
	}

	// ページが作られるたびに、ページのスクリプトより先に実行する JS（ライブラリの読み込み用）。Create の直後に呼ぶこと。
	// ライブラリを HTML の <script> に直接埋め込むと、ソース中の "<!--" "<script" などで HTML の解析が狂い、
	// スクリプトが文字として表示されてしまった（marked で実際に踏んだ）。この方法なら HTML の解析を通らない
	void AddScriptOnDocumentCreated( const std::wstring& js )
	{
		m_scripts.push_back( js );
	}

	void NavigateToString( const std::wstring& html )
	{
		m_pendingHtml = html;
		if( m_webview && m_scriptsAdded ){
			m_navigationAllowed = true;   // この 1 回だけ許可する
			m_webview->NavigateToString( html.c_str() );
		}
	}

	// ページが受け取れる状態かは呼び出し側で管理する（ページのスクリプトから「準備完了」を postMessage してもらう）。
	// NavigationCompleted を待つと、ページのスクリプトの「準備完了」より後に来るので、その間に送ったものが消える（実際に踏んだ）
	void PostJson( const std::wstring& json )
	{
		if( m_webview )  m_webview->PostWebMessageAsJson( json.c_str() );
	}

	// 仮想ホスト名（https://<hostName>/）でローカルフォルダを見せる。Markdown の相対パスの画像などに使う。
	// folder が空なら割り当てを外す。WebView2 の準備前に呼んでもよい（準備ができたら反映）
	void MapFolder( const std::wstring& hostName, const std::wstring& folder )
	{
		m_mapHost = hostName;
		m_mapFolder = folder;
		ApplyFolderMapping();
	}

	// ページ内で JS を実行し、結果（JSON 文字列）を受け取る
	void ExecuteScript( const std::wstring& js, std::function<void( const std::wstring& )> done = {} )
	{
		if( !m_webview )  return;
		m_webview->ExecuteScript( js.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
			[done]( HRESULT hr, LPCWSTR result ) -> HRESULT {
				if( done )  done( SUCCEEDED( hr ) && result ? result : L"" );
				return S_OK;
			} ).Get() );
	}

	void Focus()
	{
		if( m_controller )  m_controller->MoveFocus( COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC );
	}

private:
	HWND                            m_hwndParent = nullptr;
	Callbacks                       m_cb;
	ComPtr<ICoreWebView2Controller> m_controller;
	ComPtr<ICoreWebView2>           m_webview;
	std::shared_ptr<bool>           m_alive;          // 非同期コールバックが、破棄後の this に触らないための印
	RECT                            m_bounds = {};
	std::wstring                    m_pendingHtml;
	std::vector<std::wstring>       m_scripts;
	std::wstring                    m_mapHost;
	std::wstring                    m_mapFolder;
	std::wstring                    m_appliedHost;    // 実際に割り当て中のホスト名（外すときに使う）
	bool                            m_scriptsAdded = false;
	bool                            m_navigationAllowed = false;
	bool                            m_ready = false;

	void ApplyFolderMapping()
	{
		ComPtr<ICoreWebView2_3> wv3;
		if( !m_webview || FAILED( m_webview.As( &wv3 ) ) )  return;   // 準備前なら OnControllerCreated で反映
		if( !m_appliedHost.empty() ){
			wv3->ClearVirtualHostNameToFolderMapping( m_appliedHost.c_str() );
			m_appliedHost.clear();
		}
		if( !m_mapHost.empty() && !m_mapFolder.empty() ){
			if( SUCCEEDED( wv3->SetVirtualHostNameToFolderMapping( m_mapHost.c_str(), m_mapFolder.c_str(),
					COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW ) ) ){
				m_appliedHost = m_mapHost;
			}
		}
	}

	void Fail( HRESULT hr )
	{
		if( m_cb.onError )  m_cb.onError( hr );
	}

	void OnControllerCreated( ICoreWebView2Controller* controller )
	{
		m_controller = controller;
		m_controller->get_CoreWebView2( &m_webview );
		m_controller->put_Bounds( m_bounds );

		ComPtr<ICoreWebView2Settings> settings;
		if( SUCCEEDED( m_webview->get_Settings( &settings ) ) ){
			settings->put_IsStatusBarEnabled( FALSE );
			settings->put_AreDefaultScriptDialogsEnabled( FALSE );
			settings->put_AreHostObjectsAllowed( FALSE );
			settings->put_IsWebMessageEnabled( TRUE );
#ifndef _DEBUG
			settings->put_AreDevToolsEnabled( FALSE );
#endif
		}

		EventRegistrationToken token;
		// 遷移: NavigateToString の 1 回だけ許可。それ以外は取り消して、http(s) なら既定のブラウザで開く
		m_webview->add_NavigationStarting( Callback<ICoreWebView2NavigationStartingEventHandler>(
			[this]( ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args ) -> HRESULT {
				if( m_navigationAllowed ){
					m_navigationAllowed = false;
					return S_OK;
				}
				LPWSTR uri = nullptr;
				args->get_Uri( &uri );
				if( uri ){ OpenExternal( uri ); CoTaskMemFree( uri ); }
				args->put_Cancel( TRUE );
				return S_OK;
			} ).Get(), &token );

		m_webview->add_NewWindowRequested( Callback<ICoreWebView2NewWindowRequestedEventHandler>(
			[]( ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args ) -> HRESULT {
				LPWSTR uri = nullptr;
				args->get_Uri( &uri );
				if( uri ){ OpenExternal( uri ); CoTaskMemFree( uri ); }
				args->put_Handled( TRUE );
				return S_OK;
			} ).Get(), &token );

		m_webview->add_NavigationCompleted( Callback<ICoreWebView2NavigationCompletedEventHandler>(
			[this]( ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* ) -> HRESULT {
				if( !m_ready ){
					m_ready = true;
					if( m_cb.onReady )  m_cb.onReady();
				}
				return S_OK;
			} ).Get(), &token );

		m_webview->add_WebMessageReceived( Callback<ICoreWebView2WebMessageReceivedEventHandler>(
			[this]( ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args ) -> HRESULT {
				LPWSTR json = nullptr;
				if( SUCCEEDED( args->get_WebMessageAsJson( &json ) ) && json ){
					if( m_cb.onMessage )  m_cb.onMessage( json );
					CoTaskMemFree( json );
				}
				return S_OK;
			} ).Get(), &token );

		m_controller->add_AcceleratorKeyPressed( Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
			[this]( ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args ) -> HRESULT {
				COREWEBVIEW2_KEY_EVENT_KIND kind;
				UINT key = 0;
				args->get_KeyEventKind( &kind );
				args->get_VirtualKey( &key );
				if( kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && m_cb.onKey && m_cb.onKey( key ) ){
					args->put_Handled( TRUE );
				}
				return S_OK;
			} ).Get(), &token );

		ApplyFolderMapping();
		AddScriptsThenNavigate( 0 );
	}

	// スクリプトの登録は非同期なので、全部終わってから最初のページを開く
	void AddScriptsThenNavigate( size_t index )
	{
		if( index >= m_scripts.size() ){
			m_scriptsAdded = true;
			if( !m_pendingHtml.empty() ){
				m_navigationAllowed = true;
				m_webview->NavigateToString( m_pendingHtml.c_str() );
			}
			return;
		}
		std::weak_ptr<bool> alive = m_alive;
		const HRESULT hr = m_webview->AddScriptToExecuteOnDocumentCreated( m_scripts[index].c_str(),
			Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
				[this, alive, index]( HRESULT, LPCWSTR ) -> HRESULT {
					if( !alive.expired() )  AddScriptsThenNavigate( index + 1 );
					return S_OK;
				} ).Get() );
		if( FAILED( hr ) )  Fail( hr );
	}
};

} // namespace ee::web
