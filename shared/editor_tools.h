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
#include <algorithm>
#include <regex>
#include <string>
#include <vector>

namespace ee::tools {

using namespace ee::doc;   // GetDocPath / GetDocumentText / ToLf など（ee::tools::GetDocName のようにも呼べる）
using json = mcp::json;

constexpr size_t   kMaxReadChars = 200000;        // 1 回に返す最大文字数
constexpr UINT_PTR kDefaultReadLines = 2000;      // get_active_document の既定の行数
constexpr UINT_PTR kMaxReadLines = 5000;
constexpr UINT_PTR kMaxEditRangeLines = 5000;     // 大きな文書で replace_text に指定できる範囲の上限
constexpr DWORD    kSearchBudgetMs = 2000;        // search_document 1 回あたりの時間（EmEditor を長く固めない）
constexpr size_t   kSearchLineChars = 100000;     // 検索で 1 行あたり見る最大文字数
constexpr size_t   kResultLineChars = 300;        // 検索結果に載せる 1 行の最大文字数

inline std::vector<size_t> FindAll( std::wstring_view hay, std::wstring_view needle )
{
	std::vector<size_t> r;
	if( needle.empty() )  return r;
	for( size_t p = hay.find( needle ); p != std::wstring_view::npos; p = hay.find( needle, p + needle.size() ) ){
		r.push_back( p );
	}
	return r;
}

inline std::wstring Lower( std::wstring s )
{
	if( !s.empty() )  CharLowerBuffW( s.data(), (DWORD)s.size() );
	return s;
}

inline std::string U8( std::wstring_view s ){ return WideToUtf8( s ); }
inline std::wstring W( const std::string& s ){ return Utf8ToWide( s ); }

// 1 始まりの行番号の引数を 0 始まりにする（省略・0 以下は def）
inline UINT_PTR LineArg( const json& args, const char* key, UINT_PTR def )
{
	if( !args.contains( key ) || !args[key].is_number_integer() )  return def;
	const long long v = args[key].get<long long>();
	return v >= 1 ? (UINT_PTR)( v - 1 ) : def;
}

inline std::vector<mcp::Tool> Definitions()
{
	const json noArgs = { { "type", "object" }, { "properties", json::object() } };
	return {
		{ "get_active_document",
		  "EmEditor で現在アクティブな（ユーザーが見ている）文書の情報と本文を、行の範囲を指定して返す（Claude Code の Read と同じ考え方）。"
		  "既定は先頭から 2000 行・20 万字まで。続きがあれば has_more=true と next_start_line を返すので、必要なら続きを読む。"
		  "EmEditor は数 GB のログも開けるため、large=true の文書は必要な範囲だけを読み、目的の箇所は search_document で探すこと。"
		  "文書の内容について答える前や、編集する前に呼ぶこと。",
		  { { "type", "object" }, { "properties", {
			  { "start_line", { { "type", "integer" }, { "description", "読み始める行（1 始まり、既定 1）" } } },
			  { "max_lines", { { "type", "integer" }, { "description", "読む最大行数（既定 2000、最大 5000）" } } },
			  { "with_line_numbers", { { "type", "boolean" }, { "description", "true なら各行の先頭に「行番号<TAB>」を付ける（既定 false）" } } },
		  } } } },
		{ "search_document",
		  "アクティブな文書を 1 行ずつ検索し、一致した行の番号と内容を返す（大きなログから目的の箇所を探すのに使う）。"
		  "1 回あたり約 2 秒で打ち切り、続きがあれば finished=false と next_start_line を返す。",
		  { { "type", "object" }, { "properties", {
			  { "pattern", { { "type", "string" }, { "description", "探す文字列（regex=true なら ECMAScript の正規表現）" } } },
			  { "regex", { { "type", "boolean" } } },
			  { "case_sensitive", { { "type", "boolean" }, { "description", "既定 false" } } },
			  { "start_line", { { "type", "integer" }, { "description", "探し始める行（1 始まり、既定 1）" } } },
			  { "max_results", { { "type", "integer" }, { "description", "既定 50、最大 500" } } },
		  } }, { "required", { "pattern" } } } },
		{ "get_selection",
		  "アクティブな文書で選択中のテキストと、その開始・終了位置（1 始まりの行・桁）を返す。選択が無ければ空文字列。20 万字を超える部分は返さない。",
		  noArgs },
		{ "list_documents",
		  "EmEditor で開いているタブ（文書）の一覧を返す。編集できるのはアクティブな文書だけ。",
		  noArgs },
		{ "replace_text",
		  "アクティブな文書の中の old_string を new_string に置き換える。old_string は対象範囲に 1 か所だけ一致する必要がある"
		  "（一致しない・複数一致する場合は失敗する。その場合は前後の文脈を含めて一意にする）。replace_all=true なら一致をすべて置き換える。改行は \\n で書く。"
		  "大きな文書（get_active_document の large=true）では start_line と end_line で範囲（最大 5000 行）を必ず指定する。",
		  { { "type", "object" }, { "properties", {
			  { "old_string", { { "type", "string" } } },
			  { "new_string", { { "type", "string" } } },
			  { "replace_all", { { "type", "boolean" } } },
			  { "start_line", { { "type", "integer" }, { "description", "探す範囲の最初の行（1 始まり）" } } },
			  { "end_line", { { "type", "integer" }, { "description", "探す範囲の最後の行（1 始まり、この行を含む）" } } },
		  } }, { "required", { "old_string", "new_string" } } } },
		{ "insert_text",
		  "アクティブな文書にテキストを挿入する。position は cursor（カーソル位置。選択中なら選択範囲を置き換える）/ start（先頭）/ end（末尾）。",
		  { { "type", "object" }, { "properties", {
			  { "text", { { "type", "string" } } },
			  { "position", { { "type", "string" }, { "enum", { "cursor", "start", "end" } } } },
		  } }, { "required", { "text" } } } },
		{ "set_document_text",
		  "アクティブな文書の内容全体を text で置き換える。メモ全体を整理し直すなど、全体を書き換えるときに使う。部分的な修正には replace_text を使う。"
		  "大きな文書（large=true）では使えない。",
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
		// 全文を一度に取り出さない。指定の範囲を 1 行ずつ読む（数 GB のログでも、読むのは返す分だけ）
		const UINT_PTR total = GetLineCount( hwndView );
		const UINT_PTR start = std::min( LineArg( args, "start_line", 0 ), total );
		UINT_PTR maxLines = kDefaultReadLines;
		if( args.contains( "max_lines" ) && args["max_lines"].is_number_integer() ){
			maxLines = (UINT_PTR)std::clamp<long long>( args["max_lines"].get<long long>(), 1, kMaxReadLines );
		}
		UINT_PTR next = start;
		bool lineCut = false;
		std::wstring text = ReadLines( hwndView, start, maxLines, kMaxReadChars, next, &lineCut );
		if( args.value( "with_line_numbers", false ) ){
			std::wstring numbered;
			UINT_PTR line = start + 1;
			size_t pos = 0;
			for( ;; ){
				const size_t nl = text.find( L'\n', pos );
				numbered += std::to_wstring( line++ ) + L"\t" + text.substr( pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos + 1 );
				if( nl == std::wstring::npos )  break;
				pos = nl + 1;
			}
			text = std::move( numbered );
		}
		const bool large = IsLargeDoc( hwndView );
		log = L"📖 読み取り: " + docName + ( start > 0 || next < total ? L"（" + std::to_wstring( start + 1 ) + L"〜" + std::to_wstring( next ) + L" 行目）" : L"" );
		json r = {
			{ "name", U8( docName ) },
			{ "path", U8( GetDocPath( hwndView ) ) },
			{ "modified", Editor_GetModified( hwndView ) != FALSE },
			{ "line_count", (unsigned long long)total },
			{ "large", large },
			{ "start_line", (unsigned long long)start + 1 },
			{ "end_line", (unsigned long long)next },
			{ "has_more", next < total },
			{ "text", U8( text ) },
		};
		if( next < total )  r["next_start_line"] = (unsigned long long)next + 1;
		if( lineCut )  r["note"] = "長すぎる行があり、途中までしか読んでいません";
		return ToolResult::Ok( r.dump() );
	}

	if( name == "search_document" ){
		const std::wstring pattern = W( args.value( "pattern", "" ) );
		if( pattern.empty() )  return ToolResult::Error( "pattern が空です" );
		const bool useRegex = args.value( "regex", false );
		const bool caseSensitive = args.value( "case_sensitive", false );
		const size_t maxResults = (size_t)std::clamp<long long>( args.value( "max_results", 50LL ), 1, 500 );

		std::wregex re;
		if( useRegex ){
			try {
				re = std::wregex( pattern, caseSensitive ? std::regex::ECMAScript : ( std::regex::ECMAScript | std::regex::icase ) );
			}
			catch( const std::regex_error& e ){
				return ToolResult::Error( std::string( "正規表現が不正です: " ) + e.what() );
			}
		}
		const std::wstring needle = caseSensitive ? pattern : Lower( pattern );

		const UINT_PTR total = GetLineCount( hwndView );
		UINT_PTR y = std::min( LineArg( args, "start_line", 0 ), total );
		const ULONGLONG deadline = GetTickCount64() + kSearchBudgetMs;
		json matches = json::array();
		for( ; y < total && matches.size() < maxResults; y++ ){
			if( ( y & 0xFF ) == 0 && GetTickCount64() > deadline )  break;   // 時間切れ。続きは next_start_line から
			std::wstring line = GetLineText( hwndView, y, kSearchLineChars );
			const bool hit = useRegex ? std::regex_search( line, re )
			                          : ( caseSensitive ? line : Lower( line ) ).find( needle ) != std::wstring::npos;
			if( hit ){
				if( line.size() > kResultLineChars )  line = line.substr( 0, kResultLineChars ) + L"…";
				matches.push_back( { { "line", (unsigned long long)y + 1 }, { "text", U8( line ) } } );
			}
		}
		const bool finished = y >= total;
		log = L"🔎 検索: " + docName + L"（" + std::to_wstring( matches.size() ) + L" 件" + ( finished ? L"" : L"・途中まで" ) + L"）";
		json r = { { "matches", matches }, { "finished", finished }, { "line_count", (unsigned long long)total } };
		if( !finished )  r["next_start_line"] = (unsigned long long)y + 1;
		return ToolResult::Ok( r.dump() );
	}

	if( name == "get_selection" ){
		POINT_PTR s = {}, e = {};
		Editor_GetSelStart( hwndView, POS_LOGICAL_W, &s );
		Editor_GetSelEnd( hwndView, POS_LOGICAL_W, &e );
		// 選択範囲が巨大でも、確保・転送するのは上限まで
		const UINT_PTR need = Editor_GetSelTextW( hwndView, 0, nullptr );
		const bool truncated = need > kMaxReadChars + 1;
		std::wstring text;
		if( need > 1 ){
			text.assign( truncated ? kMaxReadChars + 1 : need, L'\0' );
			Editor_GetSelTextW( hwndView, text.size(), text.data() );
			text.resize( wcslen( text.c_str() ) );
		}
		log = L"📖 選択範囲: " + docName;
		json r = {
			{ "name", U8( docName ) },
			{ "text", U8( ToLf( text ) ) },
			{ "start", { { "line", s.y + 1 }, { "column", s.x + 1 } } },
			{ "end", { { "line", e.y + 1 }, { "column", e.x + 1 } } },
		};
		if( truncated )  r["note"] = "選択範囲が大きいため、先頭 20 万字だけを返しています";
		return ToolResult::Ok( r.dump() );
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

		// 探す範囲。大きな文書では範囲の指定を必須にする（全文を取り出さない）
		const UINT_PTR total = GetLineCount( hwndView );
		const bool hasRange = args.contains( "start_line" ) || args.contains( "end_line" );
		if( !hasRange && IsLargeDoc( hwndView ) ){
			return ToolResult::Error( "大きな文書なので start_line と end_line で範囲（最大 5000 行）を指定してください。search_document で場所を探せます" );
		}
		const UINT_PTR first = std::min( LineArg( args, "start_line", 0 ), total );
		const UINT_PTR last = std::min( LineArg( args, "end_line", total ? total - 1 : 0 ), total ? total - 1 : 0 );
		if( last < first )  return ToolResult::Error( "end_line が start_line より前です" );
		if( hasRange && last - first + 1 > kMaxEditRangeLines )  return ToolResult::Error( "範囲は 5000 行までにしてください" );

		UINT_PTR next = first;
		bool lineCut = false;
		const std::wstring range = ReadLines( hwndView, first, last - first + 1, SIZE_MAX / 2, next, &lineCut );
		const std::vector<size_t> hits = FindAll( range, oldStr );
		if( hits.empty() )  return ToolResult::Error( "old_string が見つかりません。get_active_document か search_document で最新の内容を確認してください" );
		if( hits.size() > 1 && !all ){
			return ToolResult::Error( "old_string が " + std::to_string( hits.size() ) + " か所に一致します。前後の文脈を含めて一意にするか、範囲を狭めるか、replace_all=true を指定してください" );
		}
		// 後ろから置き換えれば、前の一致の位置はずれない。位置は範囲の先頭行からの相対なので first を足す
		for( auto it = hits.rbegin(); it != hits.rend(); ++it ){
			POINT_PTR a = OffsetToPos( range, *it ), b = OffsetToPos( range, *it + oldStr.size() );
			a.y += first;
			b.y += first;
			SelectRange( hwndView, a, b );
			Editor_InsertW( hwndView, newStr.c_str(), true );
		}
		log = L"✏️ 置換: " + docName + L"（" + std::to_wstring( hits.size() ) + L" か所）";
		return ToolResult::Ok( std::to_string( hits.size() ) + " か所を置き換えました" );
	}

	if( name == "insert_text" ){
		const std::wstring text = ToLf( W( args.value( "text", "" ) ) );
		const std::string position = args.value( "position", "cursor" );
		if( position == "start" ){
			POINT_PTR pt = {};
			Editor_SetCaretPos( hwndView, POS_LOGICAL_W, &pt );
		}
		else if( position == "end" ){
			// 末尾 = 最終行の行末。全文を読まずに求める
			const UINT_PTR total = GetLineCount( hwndView );
			POINT_PTR pt = {};
			if( total > 0 ){
				pt.y = (LONG_PTR)( total - 1 );
				pt.x = (LONG_PTR)GetLineText( hwndView, total - 1, SIZE_MAX / 2 ).size();
			}
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
		if( IsLargeDoc( hwndView ) ){
			return ToolResult::Error( "大きな文書の全体置き換えはできません。replace_text で範囲を指定して直すか、new_document で別のタブに出してください" );
		}
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
