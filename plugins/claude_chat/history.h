// チャット履歴の表示の共通インターフェース。
//   WebHistory（WebView2 + marked で HTML 描画）… 通常はこちら
//   chatview::View（リッチエディット）           … WebView2 Runtime が無い環境などでの代替
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "process_runner.h"
#include "webview_host.h"

class IHistory
{
public:
	virtual ~IHistory() = default;
	virtual void Notice( std::wstring_view text ) = 0;
	virtual void User( std::wstring_view text, std::wstring_view meta ) = 0;   // meta: 使ったモデル等
	virtual void Claude( double seconds, std::wstring_view markdown ) = 0;
	virtual void Tool( std::wstring_view text ) = 0;
	virtual void Error( std::wstring_view text ) = 0;
	virtual void StartThinking() = 0;
	virtual void TickThinking() = 0;   // 400ms ごと（リッチエディット版がマークを回すのに使う）
	virtual void StopThinking() = 0;
	virtual void Clear() = 0;

	// お願いテンプレートのボタン列（WebView2 版のみ。リッチエディット版では出さない）
	struct TemplateButton { std::wstring icon, name, prompt; };
	virtual void SetTemplates( const std::vector<TemplateButton>& /*items*/, const std::wstring& /*error*/ ) {}
};

// WebView2 版。ページの準備ができるまでのメッセージは溜めておき、準備ができたら流す
class WebHistory : public IHistory
{
public:
	using json = nlohmann::json;

	struct Callbacks
	{
		std::function<void( const std::wstring& text )> onCopy;
		std::function<void( const std::wstring& text )> onInsert;
		std::function<void()>                          onEscape;
		std::function<void( HRESULT )>                 onError;   // WebView2 を使えない（Runtime が無いなど）
		std::function<void( int index )>               onTemplate;        // テンプレートのボタンが押された
		std::function<void()>                          onEditTemplates;   // 「＋ 編集」が押された
	};

	// libraryJs: ページより先に読み込むライブラリ（marked）
	void Create( HWND hwndParent, const std::wstring& html, const std::wstring& libraryJs, Callbacks cb )
	{
		m_cb = std::move( cb );
		ee::web::Host::Callbacks hcb;
		hcb.onMessage = [this]( const std::wstring& j ){ OnMessage( j ); };
		hcb.onError = [this]( HRESULT hr ){ if( m_cb.onError )  m_cb.onError( hr ); };
		hcb.onKey = [this]( UINT vkey ){
			if( vkey == VK_ESCAPE && m_cb.onEscape ){ m_cb.onEscape(); return true; }
			return false;
		};
		m_host.Create( hwndParent, std::move( hcb ) );
		m_host.AddScriptOnDocumentCreated( libraryJs );
		m_host.NavigateToString( html );
	}

	void Close(){ m_host.Close(); m_ready = false; m_pending.clear(); }
	void SetBounds( const RECT& rc ){ m_host.SetBounds( rc ); }

	void Notice( std::wstring_view text ) override { Send( { { "type", "notice" }, { "text", U8( text ) } } ); }
	void User( std::wstring_view text, std::wstring_view meta ) override { Send( { { "type", "user" }, { "text", U8( text ) }, { "meta", U8( meta ) } } ); }
	void Claude( double seconds, std::wstring_view markdown ) override { Send( { { "type", "claude" }, { "markdown", U8( markdown ) }, { "seconds", seconds } } ); }
	void Tool( std::wstring_view text ) override { Send( { { "type", "tool" }, { "text", U8( text ) } } ); }
	void Error( std::wstring_view text ) override { Send( { { "type", "error" }, { "text", U8( text ) } } ); }
	void StartThinking() override { Send( { { "type", "thinking" }, { "on", true } } ); }
	void TickThinking() override {}   // 経過秒数はページ側で数える
	void StopThinking() override { Send( { { "type", "thinking" }, { "on", false } } ); }
	void Clear() override { Send( { { "type", "clear" } } ); }

	void SetTemplates( const std::vector<TemplateButton>& items, const std::wstring& error ) override
	{
		json list = json::array();
		for( const TemplateButton& t : items ){
			list.push_back( { { "icon", U8( t.icon ) }, { "name", U8( t.name ) }, { "prompt", U8( t.prompt ) } } );
		}
		Send( { { "type", "templates" }, { "items", list }, { "error", U8( error ) } } );
	}

private:
	ee::web::Host     m_host;
	Callbacks         m_cb;
	bool              m_ready = false;
	std::vector<json> m_pending;

	static std::string U8( std::wstring_view s ){ return ee::WideToUtf8( s ); }

	void Send( json msg )
	{
		if( !m_ready ){
			m_pending.push_back( std::move( msg ) );
			return;
		}
		m_host.PostJson( ee::Utf8ToWide( msg.dump() ) );
	}

	void OnMessage( const std::wstring& j )
	{
		const json msg = json::parse( ee::WideToUtf8( j ), nullptr, false );
		if( !msg.is_object() )  return;
		const std::string type = msg.value( "type", "" );
		if( type == "ready" ){
			m_ready = true;
			for( json& p : m_pending )  m_host.PostJson( ee::Utf8ToWide( p.dump() ) );
			m_pending.clear();
		}
		else if( type == "copy" && m_cb.onCopy ){
			m_cb.onCopy( ee::Utf8ToWide( msg.value( "text", "" ) ) );
		}
		else if( type == "insert" && m_cb.onInsert ){
			m_cb.onInsert( ee::Utf8ToWide( msg.value( "text", "" ) ) );
		}
		else if( type == "template" && m_cb.onTemplate ){
			m_cb.onTemplate( msg.value( "index", -1 ) );
		}
		else if( type == "editTemplates" && m_cb.onEditTemplates ){
			m_cb.onEditTemplates();
		}
		else if( type == "open" ){
			ee::web::OpenExternal( ee::Utf8ToWide( msg.value( "href", "" ) ) );   // http(s) だけ開く
		}
	}
};

// HTML テンプレートに CSS を埋め込む。nonce は読み込みごとにランダム
inline std::wstring BuildChatHtml( HINSTANCE hinst, int idHtml, int idCss )
{
	// nonce を先に置き換える（CSS の中身に {{...}} があっても触らないように、埋め込みは最後）
	return ee::Utf8ToWide( ee::web::FillTemplate( ee::web::LoadResourceUtf8( hinst, idHtml ), {
		{ "NONCE", ee::web::RandomHex() },
		{ "MARKDOWN_CSS", ee::web::LoadResourceUtf8( hinst, idCss ) },
	} ) );
}
