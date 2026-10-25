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
#include <string>
#include <vector>

namespace ee::tools {

using namespace ee::doc;   // GetDocPath / GetDocumentText / ToLf など（ee::tools::GetDocName のようにも呼べる）
using json = mcp::json;

constexpr size_t   kMaxReadChars = 200000;        // 1 回に返す最大文字数
constexpr UINT_PTR kDefaultReadLines = 2000;      // get_active_document の既定の行数
constexpr UINT_PTR kMaxReadLines = 5000;
constexpr UINT_PTR kMaxEditRangeLines = 5000;     // 大きな文書で replace_text に指定できる範囲の上限
constexpr DWORD    kNativeSearchBudgetMs = 5000;  // search_document 1 回あたりの上限時間（EmEditor を長く固めない）
constexpr size_t   kResultLineChars = 300;        // 検索結果に載せる 1 行の最大文字数
constexpr int      kFilterWholeLine = -1;         // EE_FILTER の iColumn で「行全体」（マクロの 0 とは違う。filter_view の説明を参照）

// EmEditor の検索フラグ（EE_FIND / EE_REPLACE は 32 ビットの UINT で受け取る）。
// FLAG_FIND_NO_PROMPT: 見つからないときなどにダイアログを出さない
inline UINT FindFlags( const json& args )
{
	UINT flags = (UINT)FLAG_FIND_NO_PROMPT;
	if( args.value( "case_sensitive", false ) )  flags |= (UINT)FLAG_FIND_CASE;
	if( args.value( "regex", false ) )           flags |= (UINT)FLAG_FIND_REG_EXP;
	return flags;
}

// 検索で動いたカーソル・選択範囲・スクロール位置を元に戻すための保存
struct ViewState
{
	POINT_PTR caret = {};
	POINT_PTR anchor = {};
	POINT_PTR scroll = {};
};

inline ViewState SaveViewState( HWND hwndView )
{
	ViewState s;
	Editor_GetCaretPos( hwndView, POS_LOGICAL_W, &s.caret );
	Editor_GetAnchorPos( hwndView, POS_LOGICAL_W, &s.anchor );
	Editor_GetScrollPos( hwndView, &s.scroll );
	return s;
}

inline void RestoreViewState( HWND hwndView, ViewState s )
{
	Editor_SetCaretPos( hwndView, POS_LOGICAL_W, &s.anchor );       // 選択の起点
	Editor_SetCaretPosEx( hwndView, POS_LOGICAL_W, &s.caret, TRUE ); // 起点からカーソルまで選択（起点と同じなら選択なし）
	Editor_SetScrollPos( hwndView, &s.scroll );
	Editor_ExecCommand( hwndView, EEID_ERASE_FIND_HILITE );         // 検索文字列の強調表示を消す
}

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
		  "EmEditor の検索機能でアクティブな文書を検索し、一致の総数と、一致した行の番号・内容を返す（大きなログから目的の箇所を探すのに使う）。"
		  "数 GB の文書でも速い。結果が多いときは max_results 件で止まり、next_start_line から続きを探せる。"
		  "一致した行をまとめて見たいときは extract_lines、ユーザーの画面で絞り込みたいときは filter_view を使う。",
		  { { "type", "object" }, { "properties", {
			  { "pattern", { { "type", "string" }, { "description", "探す文字列（regex=true なら EmEditor の正規表現）" } } },
			  { "regex", { { "type", "boolean" } } },
			  { "case_sensitive", { { "type", "boolean" }, { "description", "既定 false" } } },
			  { "start_line", { { "type", "integer" }, { "description", "探し始める行（1 始まり、既定 1）" } } },
			  { "max_results", { { "type", "integer" }, { "description", "既定 50、最大 500" } } },
		  } }, { "required", { "pattern" } } } },
		{ "extract_lines",
		  "EmEditor の「抽出」で、パターンに一致する行だけを新しいタブに抜き出す。元の文書は変わらない。新しいタブがアクティブになるので、"
		  "続けて get_active_document で読める。巨大なログから ERROR の行だけを集める、などに使う。",
		  { { "type", "object" }, { "properties", {
			  { "pattern", { { "type", "string" } } },
			  { "regex", { { "type", "boolean" } } },
			  { "case_sensitive", { { "type", "boolean" } } },
		  } }, { "required", { "pattern" } } } },
		{ "replace_all",
		  "EmEditor の「すべて置換」で、アクティブな文書のパターンに一致する箇所をすべて置き換える。数 GB の文書でも速い。"
		  "正規表現の置換文字列では \\1 などで後方参照できる。置き換えた件数を返す。1 か所だけを正確に直すなら replace_text を使う。",
		  { { "type", "object" }, { "properties", {
			  { "pattern", { { "type", "string" } } },
			  { "replacement", { { "type", "string" } } },
			  { "regex", { { "type", "boolean" } } },
			  { "case_sensitive", { { "type", "boolean" } } },
		  } }, { "required", { "pattern", "replacement" } } } },
		{ "get_filter",
		  "アクティブな文書に今かかっているフィルターの設定（段ごとの検索語・列・フラグなど）を返す。フィルターが無ければ空の一覧。",
		  noArgs },
		{ "filter_view",
		  "EmEditor のフィルターで、ユーザーの画面にパターンに一致する行だけを表示する（前後の行も表示できる）。文書の内容は変わらない。"
		  "pattern を空文字列にするとフィルターを解除する。ユーザーに該当箇所を見せたいときに使う。",
		  { { "type", "object" }, { "properties", {
			  { "pattern", { { "type", "string" }, { "description", "空文字列で解除" } } },
			  { "regex", { { "type", "boolean" } } },
			  { "case_sensitive", { { "type", "boolean" } } },
			  { "lines_before", { { "type", "integer" }, { "description", "一致した行の前に表示する行数（既定 0）" } } },
			  { "lines_after", { { "type", "integer" }, { "description", "一致した行の後に表示する行数（既定 0）" } } },
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

	// ---- ここから EmEditor 自身の検索・抽出・置換・フィルターを使うツール（巨大なファイルでも EmEditor の速度で動く） ----

	if( name == "search_document" ){
		const std::wstring pattern = W( args.value( "pattern", "" ) );
		if( pattern.empty() )  return ToolResult::Error( "pattern が空です" );
		const UINT flags = FindFlags( args );
		const size_t maxResults = (size_t)std::clamp<long long>( args.value( "max_results", 50LL ), 1, 500 );
		const UINT_PTR total = GetLineCount( hwndView );
		const UINT_PTR start = std::min( LineArg( args, "start_line", 0 ), total );

		// 検索はカーソル・選択範囲・スクロール位置を動かすので、終わったら元に戻す
		ViewState saved = SaveViewState( hwndView );
		const int count = Editor_FindW( hwndView, flags | (UINT)FLAG_FIND_COUNT, pattern.c_str() );   // 文書全体の一致数

		json matches = json::array();
		POINT_PTR pos = { 0, (LONG_PTR)start };
		Editor_SetCaretPos( hwndView, POS_LOGICAL_W, &pos );
		LONG_PTR lastLine = -1;
		bool more = false;
		const ULONGLONG deadline = GetTickCount64() + kNativeSearchBudgetMs;
		while( true ){
			if( matches.size() >= maxResults || GetTickCount64() > deadline ){ more = true; break; }
			if( !Editor_FindW( hwndView, flags | (UINT)FLAG_FIND_NEXT, pattern.c_str() ) )  break;   // もう無い
			POINT_PTR s = {};
			Editor_GetSelStart( hwndView, POS_LOGICAL_W, &s );
			if( s.y <= lastLine )  break;   // 末尾から先頭へ戻った（念のため）
			lastLine = s.y;
			std::wstring line = GetLineText( hwndView, (UINT_PTR)s.y, kResultLineChars );
			if( line.size() >= kResultLineChars )  line += L"…";
			matches.push_back( { { "line", (unsigned long long)s.y + 1 }, { "text", U8( line ) } } );
			// 同じ行で何度も止まらないよう、次の行の先頭から探す
			POINT_PTR nextPos = { 0, s.y + 1 };
			if( (UINT_PTR)nextPos.y >= total )  break;
			Editor_SetCaretPos( hwndView, POS_LOGICAL_W, &nextPos );
		}
		RestoreViewState( hwndView, saved );

		log = L"🔎 検索: " + docName + L"（" + ( count > 0 ? std::to_wstring( count ) + L" 件中 " : L"" ) + std::to_wstring( matches.size() ) + L" 行）";
		json r = { { "matches", matches }, { "line_count", (unsigned long long)total }, { "finished", !more } };
		if( count > 0 )  r["total_matches"] = count;
		if( more && lastLine >= 0 )  r["next_start_line"] = (unsigned long long)lastLine + 2;
		return ToolResult::Ok( r.dump() );
	}

	if( name == "extract_lines" ){
		const std::wstring pattern = W( args.value( "pattern", "" ) );
		if( pattern.empty() )  return ToolResult::Error( "pattern が空です" );
		const std::wstring from = docName;
		const int count = Editor_FindW( hwndView, FindFlags( args ) | (UINT)FLAG_FIND_EXTRACT, pattern.c_str() );
		if( count <= 0 )  return ToolResult::Ok( json{ { "extracted", 0 }, { "message", "一致する行がありませんでした（新しいタブは作られていません）" } }.dump() );
		const std::wstring to = GetDocName( hwndView );
		log = L"📄 抽出: " + from + L" → " + to + L"（" + std::to_wstring( count ) + L" 件）";
		return ToolResult::Ok( json{
			{ "extracted", count },
			{ "new_document", U8( to ) },
			{ "line_count", (unsigned long long)GetLineCount( hwndView ) },
			{ "message", "一致した行を新しいタブに抜き出しました。新しいタブがアクティブです" },
		}.dump() );
	}

	if( name == "replace_all" ){
		const std::wstring pattern = W( args.value( "pattern", "" ) );
		const std::wstring replacement = ToLf( W( args.value( "replacement", "" ) ) );
		if( pattern.empty() )  return ToolResult::Error( "pattern が空です" );
		// 検索文字列と置換文字列を \0 で区切り、末尾を \0\0 にして渡す
		std::wstring findReplace = pattern;
		findReplace += L'\0';
		findReplace += replacement;
		findReplace += L'\0';
		findReplace += L'\0';
		const int count = Editor_ReplaceW( hwndView, FindFlags( args ) | (UINT)FLAG_REPLACE_ALL, findReplace.c_str() );
		Editor_ExecCommand( hwndView, EEID_ERASE_FIND_HILITE );
		log = L"✏️ すべて置換: " + docName + L"（" + std::to_wstring( std::max( count, 0 ) ) + L" か所）";
		return ToolResult::Ok( json{ { "replaced", std::max( count, 0 ) } }.dump() );
	}

	if( name == "get_filter" ){
		// iFilter = -1 で段の数、0 以上でその段の設定が返る。前後の表示行数も見たいので EE_GET_FILTER を直接送る
		json levels = json::array();
		for( int i = 0; i < 16; i++ ){
			WCHAR buf[1024] = {};
			FILTER_INFO_EX fi = {};
			fi.cbSize = sizeof( fi );
			fi.pszFilter = buf;
			fi.cchFilter = _countof( buf );
			const int n = (int)SendMessageW( hwndView, EE_GET_FILTER, (WPARAM)&fi, (LPARAM)i );
			if( n <= 0 )  break;
			WCHAR flagsHex[32];
			StringCchPrintfW( flagsHex, _countof( flagsHex ), L"0x%llX", (unsigned long long)fi.flags );
			levels.push_back( {
				{ "filter", U8( buf ) }, { "column", fi.iColumn }, { "flags", U8( flagsHex ) },
				{ "x_begin", (long long)fi.xBegin }, { "x_end", (long long)fi.xEnd },
				{ "lines_above", fi.nVisibleLinesAbove }, { "lines_below", fi.nVisibleLinesBelow },
			} );
		}
		log = L"🔍 フィルターの設定: " + docName + L"（" + std::to_wstring( levels.size() ) + L" 段）"
			+ ( levels.empty() ? L"" : L" " + W( levels[0].dump() ) );
		return ToolResult::Ok( json{ { "levels", levels } }.dump() );
	}

	if( name == "filter_view" ){
		const std::wstring pattern = W( args.value( "pattern", "" ) );
		const int before = (int)std::clamp<long long>( args.value( "lines_before", 0LL ), 0, 1000 );
		const int after = (int)std::clamp<long long>( args.value( "lines_after", 0LL ), 0, 1000 );
		// フィルターのフラグは大文字小文字・正規表現だけ（FLAG_FIND_NO_PROMPT はフィルターのフラグとしては定義されていない）
		UINT64 filterFlags = 0;
		if( args.value( "case_sensitive", false ) )  filterFlags |= FLAG_FIND_CASE;
		if( args.value( "regex", false ) )           filterFlags |= FLAG_FIND_REG_EXP;
		if( pattern.empty() ){
			Editor_Filter( hwndView, L"", 0, 0, 0, 0, 0, 0 );   // 空文字列でフィルター解除
			log = L"🧹 フィルター解除: " + docName;
			return ToolResult::Ok( "フィルターを解除しました" );
		}
		// 先に、フィルターなしの状態で一致数を数えておく（フィルター中は隠れた行が検索対象から外れるため）
		Editor_Filter( hwndView, L"", 0, 0, 0, 0, 0, 0 );
		ViewState saved = SaveViewState( hwndView );
		const int expected = Editor_FindW( hwndView, FindFlags( args ) | (UINT)FLAG_FIND_COUNT, pattern.c_str() );
		RestoreViewState( hwndView, saved );
		// 行全体を対象にする。マクロの document.Filter では iColumn = 0 が「行全体」だが、プラグイン API（EE_FILTER）で 0 を渡すと
		// フィルターバーの列が「カスタム (1...0)」になり、空の範囲を探して 0 行になった（2026-10-25 / EmEditor で確認）。
		// プラグイン API の列は 0 始まりで、行全体は -1 と考えられる
		const int count = Editor_Filter( hwndView, pattern.c_str(), kFilterWholeLine, filterFlags, 0, 0, before, after );
		log = L"🔍 フィルター: " + docName + L"（" + std::to_wstring( std::max( count, 0 ) ) + L" 行" + ( expected > 0 ? L"・検索では " + std::to_wstring( expected ) + L" 件" : L"" ) + L"）";
		json r = { { "matched_lines", count }, { "search_count", expected } };
		if( count <= 0 && expected > 0 ){
			r["message"] = "フィルターは 0 行と返しましたが、検索では一致があります。大きな文書ではフィルターの反映が遅れて表示されることがあります。"
			               "ユーザーに画面を確認してもらってください。解除するには pattern を空にして呼ぶ";
		}
		else {
			r["message"] = "ユーザーの画面に一致した行だけを表示しました。解除するには pattern を空にして呼ぶ";
		}
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
