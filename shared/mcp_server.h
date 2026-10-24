// プラグイン内で動かす最小の MCP サーバー（Streamable HTTP, JSON レスポンスのみ）。
// claude -p に --mcp-config で渡し、Claude からツールとして呼んでもらうためのもの。EmEditor には依存しない。
//
// 安全策:
//   - 127.0.0.1 にだけ bind し、ポートは OS に任せる（毎回変わる）
//   - 起動ごとにランダムなトークンを作り、Authorization: Bearer <token> が一致しない要求は 401
//   - Origin ヘッダー付き（ブラウザからの要求）は 403（DNS リバインディング対策）
//
// 簡略化している点:
//   - 1 接続ずつ順番に処理し、毎回 Connection: close で切る
//   - GET（サーバーからの SSE ストリーム）は 405。claude はそれでも POST だけで動く（2026-10-24 確認）
//   - セッション ID（Mcp-Session-Id）は使わない
//
// ツールのコールバックはサーバーのスレッドで呼ばれる。UI スレッドで動かす必要があるなら、呼び出し側で受け渡すこと。
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")

namespace ee::mcp {

using json = nlohmann::json;

// スレッドの終了を待つ間、他スレッドからの SendMessage だけは処理する。
// UI スレッドが join で止まっている間にツール呼び出しが UI へ SendMessage してくると、通常の join ではデッドロックする。
inline void JoinPumpingSentMessages( std::thread& t )
{
	if( !t.joinable() )  return;
	HANDLE h = t.native_handle();
	while( MsgWaitForMultipleObjects( 1, &h, FALSE, INFINITE, QS_SENDMESSAGE ) == WAIT_OBJECT_0 + 1 ){
		MSG msg;
		PeekMessageW( &msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE );   // 送られてきたメッセージを処理する
	}
	t.join();
}

struct ToolResult
{
	std::string text;        // UTF-8
	bool        isError = false;

	static ToolResult Ok( std::string s ){ return { std::move( s ), false }; }
	static ToolResult Error( std::string s ){ return { std::move( s ), true }; }
};

struct Tool
{
	std::string name;
	std::string description;
	json        inputSchema;
};

// name と arguments を受け取ってツールを実行する
using ToolHandler = std::function<ToolResult( const std::string& name, const json& arguments )>;

class Server
{
public:
	Server() = default;
	~Server(){ Stop(); }
	Server( const Server& ) = delete;
	Server& operator=( const Server& ) = delete;

	bool Start( std::string serverName, std::vector<Tool> tools, ToolHandler handler )
	{
		Stop();
		m_serverName = std::move( serverName );
		m_tools = std::move( tools );
		m_handler = std::move( handler );

		WSADATA wsa;
		if( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 )  return false;
		m_wsaStarted = true;

		const SOCKET sock = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
		if( sock == INVALID_SOCKET ){ Stop(); return false; }
		m_listen = sock;
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
		addr.sin_port = 0;
		if( bind( sock, (sockaddr*)&addr, sizeof( addr ) ) != 0 || listen( sock, 4 ) != 0 ){ Stop(); return false; }
		int len = sizeof( addr );
		getsockname( sock, (sockaddr*)&addr, &len );
		m_port = ntohs( addr.sin_port );

		unsigned char rnd[24];
		if( BCryptGenRandom( nullptr, rnd, sizeof( rnd ), BCRYPT_USE_SYSTEM_PREFERRED_RNG ) != 0 ){ Stop(); return false; }
		static const char hex[] = "0123456789abcdef";
		m_token.clear();
		for( unsigned char b : rnd ){ m_token += hex[b >> 4]; m_token += hex[b & 15]; }

		m_thread = std::thread( [this]{ AcceptLoop(); } );
		return true;
	}

	// UI スレッドから呼んでよい（ツールのコールバックが UI へ SendMessage していても、処理しながら待つ）。
	void Stop()
	{
		const SOCKET listenSock = m_listen.exchange( INVALID_SOCKET );
		if( listenSock != INVALID_SOCKET ){
			closesocket( listenSock );   // accept を失敗させてループを抜けさせる
		}
		JoinPumpingSentMessages( m_thread );
		if( m_wsaStarted ){
			WSACleanup();
			m_wsaStarted = false;
		}
		m_port = 0;
	}

	std::thread& Thread(){ return m_thread; }
	bool IsRunning() const { return m_port != 0; }
	unsigned short Port() const { return m_port; }
	const std::string& Token() const { return m_token; }

	const std::string& Name() const { return m_serverName; }

	// mcpServers の中の、このサーバーの 1 エントリ
	json ConfigEntry() const
	{
		return {
			{ "type", "http" },
			{ "url", "http://127.0.0.1:" + std::to_string( m_port ) + "/mcp" },
			{ "headers", { { "Authorization", "Bearer " + m_token } } },
		};
	}

	// claude --mcp-config に渡す JSON（1 行）
	std::string McpConfigJson() const
	{
		return json{ { "mcpServers", { { m_serverName, ConfigEntry() } } } }.dump();
	}

private:
	std::atomic<SOCKET> m_listen = INVALID_SOCKET;
	bool                m_wsaStarted = false;
	unsigned short      m_port = 0;
	std::string         m_token;
	std::string         m_serverName;
	std::vector<Tool>   m_tools;
	ToolHandler         m_handler;
	std::thread         m_thread;

	void AcceptLoop()
	{
		for( ;; ){
			SOCKET listenSock = m_listen;
			if( listenSock == INVALID_SOCKET )  break;
			SOCKET s = accept( listenSock, nullptr, nullptr );
			if( s == INVALID_SOCKET )  break;   // Stop で閉じられた
			DWORD timeout = 10000;
			setsockopt( s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof( timeout ) );
			setsockopt( s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof( timeout ) );
			HandleConnection( s );
			shutdown( s, SD_BOTH );
			closesocket( s );
		}
	}

	static bool RecvSome( SOCKET s, std::string& buf )
	{
		char tmp[8192];
		const int n = recv( s, tmp, sizeof( tmp ), 0 );
		if( n <= 0 )  return false;
		buf.append( tmp, n );
		return true;
	}

	static void Send( SOCKET s, const std::string& data )
	{
		size_t pos = 0;
		while( pos < data.size() ){
			const int n = send( s, data.data() + pos, (int)std::min<size_t>( data.size() - pos, 1 << 16 ), 0 );
			if( n <= 0 )  return;
			pos += n;
		}
	}

	static void Respond( SOCKET s, const char* status, const std::string& body = {}, const char* contentType = "application/json" )
	{
		std::string r = std::string( "HTTP/1.1 " ) + status + "\r\n";
		if( !body.empty() )  r += std::string( "Content-Type: " ) + contentType + "\r\n";
		r += "Content-Length: " + std::to_string( body.size() ) + "\r\nConnection: close\r\n\r\n";
		r += body;
		Send( s, r );
	}

	static std::string Lower( std::string s )
	{
		for( char& c : s )  if( c >= 'A' && c <= 'Z' )  c = (char)( c - 'A' + 'a' );
		return s;
	}

	void HandleConnection( SOCKET s )
	{
		constexpr size_t kMaxHeader = 64 * 1024;
		constexpr size_t kMaxBody = 16 * 1024 * 1024;

		std::string buf;
		size_t headerEnd;
		while( ( headerEnd = buf.find( "\r\n\r\n" ) ) == std::string::npos ){
			if( buf.size() > kMaxHeader || !RecvSome( s, buf ) )  return;
		}

		// リクエスト行とヘッダー
		std::string method, auth;
		bool hasOrigin = false;
		size_t contentLength = 0;
		{
			size_t lineStart = 0;
			bool first = true;
			while( lineStart < headerEnd ){
				size_t lineEnd = buf.find( "\r\n", lineStart );
				std::string line = buf.substr( lineStart, lineEnd - lineStart );
				lineStart = lineEnd + 2;
				if( first ){
					method = line.substr( 0, line.find( ' ' ) );
					first = false;
					continue;
				}
				const size_t colon = line.find( ':' );
				if( colon == std::string::npos )  continue;
				const std::string key = Lower( line.substr( 0, colon ) );
				std::string value = line.substr( colon + 1 );
				while( !value.empty() && value.front() == ' ' )  value.erase( 0, 1 );
				if( key == "content-length" )  contentLength = (size_t)_strtoui64( value.c_str(), nullptr, 10 );
				else if( key == "authorization" )  auth = value;
				else if( key == "origin" )  hasOrigin = true;
			}
		}

		if( hasOrigin ){ Respond( s, "403 Forbidden" ); return; }
		if( auth != "Bearer " + m_token ){ Respond( s, "401 Unauthorized" ); return; }
		if( method == "GET" ){ Respond( s, "405 Method Not Allowed" ); return; }
		if( method == "DELETE" ){ Respond( s, "200 OK" ); return; }
		if( method != "POST" ){ Respond( s, "405 Method Not Allowed" ); return; }
		if( contentLength > kMaxBody ){ Respond( s, "413 Payload Too Large" ); return; }

		std::string body = buf.substr( headerEnd + 4 );
		while( body.size() < contentLength ){
			if( !RecvSome( s, body ) )  return;
		}
		body.resize( contentLength );

		const std::optional<std::string> response = HandleJsonRpc( body );
		if( response )  Respond( s, "200 OK", *response );
		else            Respond( s, "202 Accepted" );
	}

	// JSON-RPC を処理し、返すべき JSON を返す。通知（id 無し）なら nullopt
	std::optional<std::string> HandleJsonRpc( const std::string& body )
	{
		json req;
		try {
			req = json::parse( body );
		}
		catch( ... ){
			return json{ { "jsonrpc", "2.0" }, { "id", nullptr }, { "error", { { "code", -32700 }, { "message", "parse error" } } } }.dump();
		}
		if( req.is_array() ){
			// バッチは使われない想定。来たら個別に処理して配列で返す
			json out = json::array();
			for( auto& r : req ){
				if( auto one = HandleOne( r ) )  out.push_back( json::parse( *one ) );
			}
			if( out.empty() )  return std::nullopt;
			return out.dump();
		}
		return HandleOne( req );
	}

	std::optional<std::string> HandleOne( const json& req )
	{
		if( !req.is_object() || !req.contains( "id" ) )  return std::nullopt;   // 通知
		const json id = req["id"];
		const std::string method = req.value( "method", "" );
		const json params = req.value( "params", json::object() );

		auto ok = [&]( json result ){ return json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move( result ) } }.dump(); };
		auto err = [&]( int code, const std::string& msg ){ return json{ { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", msg } } } }.dump(); };

		try {
			if( method == "initialize" ){
				return ok( {
					{ "protocolVersion", params.value( "protocolVersion", "2025-06-18" ) },
					{ "capabilities", { { "tools", json::object() } } },
					{ "serverInfo", { { "name", m_serverName }, { "version", "0.1" } } },
				} );
			}
			if( method == "ping" ){
				return ok( json::object() );
			}
			if( method == "tools/list" ){
				json list = json::array();
				for( const Tool& t : m_tools ){
					list.push_back( { { "name", t.name }, { "description", t.description }, { "inputSchema", t.inputSchema } } );
				}
				return ok( { { "tools", list } } );
			}
			if( method == "tools/call" ){
				const std::string name = params.value( "name", "" );
				const json args = params.value( "arguments", json::object() );
				ToolResult r = m_handler ? m_handler( name, args ) : ToolResult::Error( "no handler" );
				return ok( {
					{ "content", json::array( { { { "type", "text" }, { "text", r.text } } } ) },
					{ "isError", r.isError },
				} );
			}
			return err( -32601, "method not found: " + method );   // server/discover なども未対応で返す
		}
		catch( const std::exception& e ){
			return err( -32603, e.what() );
		}
	}
};

} // namespace ee::mcp
