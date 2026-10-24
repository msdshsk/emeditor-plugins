// EmEditor の文書を操作する MCP ツール群。Claude から呼ばれる。
// すべて UI スレッドで呼ぶこと（中身は Editor_* = SendMessage）。
//
// 編集系ツールの方針（Claude Code の Edit ツールに合わせる）:
//   - replace_text は old_string が文書中に「ちょうど 1 か所」あるときだけ置き換える。0 か所・複数は失敗を返す
//   - 編集は 1 回の操作ごとに Ctrl+Z 1 回で戻せる（EmEditor に Undo をまとめる API は無い）
//   - 対象は常にアクティブな文書
#pragma once

#include "plugin.h"
#include "editor_doc.h"
#include "mcp_server.h"
#include "process_runner.h"
#include <shlwapi.h>
#include <string>
#include <vector>

namespace ee::tools {

using namespace ee::doc;   // GetDocPath / GetDocumentText / ToLf など（ee::tools::GetDocName のようにも呼べる）
using json = mcp::json;

constexpr size_t kMaxReadChars = 200000;

inline std::vector<size_t> FindAll( std::wstring_view hay, std::wstring_view needle )
{
	std::vector<size_t> r;
	if( needle.empty() )  return r;
	for( size_t p = hay.find( needle ); p != std::wstring_view::npos; p = hay.find( needle, p + needle.size() ) ){
		r.push_back( p );
	}
	return r;
}

inline std::string U8( std::wstring_view s ){ return WideToUtf8( s ); }
inline std::wstring W( const std::string& s ){ return Utf8ToWide( s ); }

inline std::vector<mcp::Tool> Definitions()
{
	const json noArgs = { { "type", "object" }, { "properties", json::object() } };
	return {
		{ "get_active_document",
		  "EmEditor で現在アクティブな（ユーザーが見ている）文書の名前・パス・行数・全文を返す。"
		  "文書の内容について答える前や、編集する前に必ず呼ぶこと。",
		  { { "type", "object" }, { "properties", {
			  { "with_line_numbers", { { "type", "boolean" }, { "description", "true なら各行の先頭に「行番号<TAB>」を付ける（既定 false）" } } },
		  } } } },
		{ "get_selection",
		  "アクティブな文書で選択中のテキストと、その開始・終了位置（1 始まりの行・桁）を返す。選択が無ければ空文字列。",
		  noArgs },
		{ "list_documents",
		  "EmEditor で開いているタブ（文書）の一覧を返す。編集できるのはアクティブな文書だけ。",
		  noArgs },
		{ "replace_text",
		  "アクティブな文書の中の old_string を new_string に置き換える。old_string は文書中に 1 か所だけ一致する必要がある"
		  "（一致しない・複数一致する場合は失敗する。その場合は前後の文脈を含めて一意にする）。"
		  "replace_all=true なら一致をすべて置き換える。改行は \\n で書く。",
		  { { "type", "object" }, { "properties", {
			  { "old_string", { { "type", "string" } } },
			  { "new_string", { { "type", "string" } } },
			  { "replace_all", { { "type", "boolean" } } },
		  } }, { "required", { "old_string", "new_string" } } } },
		{ "insert_text",
		  "アクティブな文書にテキストを挿入する。position は cursor（カーソル位置。選択中なら選択範囲を置き換える）/ start（先頭）/ end（末尾）。",
		  { { "type", "object" }, { "properties", {
			  { "text", { { "type", "string" } } },
			  { "position", { { "type", "string" }, { "enum", { "cursor", "start", "end" } } } },
		  } }, { "required", { "text" } } } },
		{ "set_document_text",
		  "アクティブな文書の内容全体を text で置き換える。メモ全体を整理し直すなど、全体を書き換えるときに使う。部分的な修正には replace_text を使う。",
		  { { "type", "object" }, { "properties", {
			  { "text", { { "type", "string" } } },
		  } }, { "required", { "text" } } } },
		{ "new_document",
		  "新しいタブを開いて text を書き込む。元の文書を残したまま結果を別に出したいときに使う。新しいタブがアクティブになる。",
		  { { "type", "object" }, { "properties", {
			  { "text", { { "type", "string" } } },
		  } }, { "required", { "text" } } } },
	};
}

// ツールを実行する。log には履歴欄に出す 1 行（ユーザー向け）を入れる
inline mcp::ToolResult Execute( HWND hwndView, const std::string& name, const json& args, std::wstring& log )
{
	using mcp::ToolResult;
	if( !hwndView || !IsWindow( hwndView ) )  return ToolResult::Error( "EmEditor の文書ウィンドウが見つかりません" );
	const std::wstring docName = GetDocName( hwndView );

	if( name == "get_active_document" ){
		std::wstring text = ToLf( GetDocumentText( hwndView ) );
		const size_t total = text.size();
		const bool truncated = total > kMaxReadChars;
		if( truncated )  text.resize( kMaxReadChars );
		if( args.value( "with_line_numbers", false ) ){
			std::wstring numbered;
			size_t line = 1, pos = 0;
			while( pos <= text.size() ){
				const size_t nl = text.find( L'\n', pos );
				numbered += std::to_wstring( line++ ) + L"\t" + text.substr( pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos + 1 );
				if( nl == std::wstring::npos )  break;
				pos = nl + 1;
			}
			text = numbered;
		}
		log = L"📖 読み取り: " + docName;
		return ToolResult::Ok( json{
			{ "name", U8( docName ) },
			{ "path", U8( GetDocPath( hwndView ) ) },
			{ "modified", Editor_GetModified( hwndView ) != FALSE },
			{ "line_count", (unsigned long long)Editor_GetLines( hwndView, POS_LOGICAL_W ) },
			{ "chars", total },
			{ "truncated", truncated },
			{ "text", U8( text ) },
		}.dump() );
	}

	if( name == "get_selection" ){
		POINT_PTR s = {}, e = {};
		Editor_GetSelStart( hwndView, POS_LOGICAL_W, &s );
		Editor_GetSelEnd( hwndView, POS_LOGICAL_W, &e );
		log = L"📖 選択範囲: " + docName;
		return ToolResult::Ok( json{
			{ "name", U8( docName ) },
			{ "text", U8( ToLf( GetSelectedText( hwndView ) ) ) },
			{ "start", { { "line", s.y + 1 }, { "column", s.x + 1 } } },
			{ "end", { { "line", e.y + 1 }, { "column", e.x + 1 } } },
		}.dump() );
	}

	if( name == "list_documents" ){
		const int count = (int)Editor_Info( hwndView, EI_GET_DOC_COUNT, 0 );
		const int active = (int)Editor_Info( hwndView, EI_GET_ACTIVE_INDEX, 0 );
		json docs = json::array();
		for( int i = 0; i < count; i++ ){
			WCHAR sz[MAX_PATH] = {};
			Editor_DocInfo( hwndView, i, EI_GET_FILE_NAMEW, (LPARAM)sz );
			docs.push_back( { { "index", i }, { "name", U8( PathFindFileNameW( sz ) ) }, { "path", U8( sz ) }, { "active", i == active } } );
		}
		log = L"📖 タブ一覧（" + std::to_wstring( count ) + L" 件）";
		return ToolResult::Ok( json{ { "documents", docs } }.dump() );
	}

	if( name == "replace_text" ){
		const std::wstring oldStr = ToLf( W( args.value( "old_string", "" ) ) );
		const std::wstring newStr = ToLf( W( args.value( "new_string", "" ) ) );
		const bool all = args.value( "replace_all", false );
		if( oldStr.empty() )  return ToolResult::Error( "old_string が空です" );
		if( oldStr == newStr )  return ToolResult::Error( "old_string と new_string が同じです" );

		const std::wstring doc = ToLf( GetDocumentText( hwndView ) );
		const std::vector<size_t> hits = FindAll( doc, oldStr );
		if( hits.empty() )  return ToolResult::Error( "old_string が文書中に見つかりません。get_active_document で最新の内容を確認してください" );
		if( hits.size() > 1 && !all ){
			return ToolResult::Error( "old_string が " + std::to_string( hits.size() ) + " か所に一致します。前後の文脈を含めて一意にするか、replace_all=true を指定してください" );
		}
		// 後ろから置き換えれば、前の一致の位置はずれない
		for( auto it = hits.rbegin(); it != hits.rend(); ++it ){
			SelectRange( hwndView, OffsetToPos( doc, *it ), OffsetToPos( doc, *it + oldStr.size() ) );
			Editor_InsertW( hwndView, newStr.c_str(), true );
		}
		log = L"✏️ 置換: " + docName + L"（" + std::to_wstring( hits.size() ) + L" か所）";
		return ToolResult::Ok( std::to_string( hits.size() ) + " か所を置き換えました" );
	}

	if( name == "insert_text" ){
		const std::wstring text = ToLf( W( args.value( "text", "" ) ) );
		const std::string position = args.value( "position", "cursor" );
		if( position == "start" || position == "end" ){
			const std::wstring doc = ToLf( GetDocumentText( hwndView ) );
			POINT_PTR pt = position == "start" ? POINT_PTR{} : OffsetToPos( doc, doc.size() );
			Editor_SetCaretPos( hwndView, POS_LOGICAL_W, &pt );
		}
		else if( position != "cursor" ){
			return ToolResult::Error( "position は cursor / start / end のいずれかです" );
		}
		Editor_InsertW( hwndView, text.c_str(), true );
		log = L"✏️ 挿入: " + docName;
		return ToolResult::Ok( "挿入しました" );
	}

	if( name == "set_document_text" ){
		const std::wstring text = ToLf( W( args.value( "text", "" ) ) );
		Editor_ExecCommand( hwndView, EEID_EDIT_SELECT_ALL );
		Editor_InsertW( hwndView, text.c_str(), true );
		log = L"✏️ 全体を書き換え: " + docName;
		return ToolResult::Ok( "文書全体を置き換えました" );
	}

	if( name == "new_document" ){
		const std::wstring text = ToLf( W( args.value( "text", "" ) ) );
		Editor_ExecCommand( hwndView, EEID_FILE_NEW );
		Editor_InsertW( hwndView, text.c_str(), true );
		log = L"📄 新しいタブに出力: " + GetDocName( hwndView );
		return ToolResult::Ok( "新しいタブ「" + U8( GetDocName( hwndView ) ) + "」に書き込みました" );
	}

	return ToolResult::Error( "unknown tool: " + name );
}

} // namespace ee::tools
