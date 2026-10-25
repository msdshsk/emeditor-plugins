// アクティブな文書を読む・位置を扱うための小さな関数群（EmEditor の Editor_* の薄いラッパ）。
// すべて UI スレッドで呼ぶこと（中身は SendMessage）。
#pragma once

#include "plugin.h"
#include <shlwapi.h>
#include <string>
#include <string_view>

namespace ee::doc {

inline std::wstring GetDocPath( HWND hwndView )
{
	WCHAR sz[MAX_PATH] = {};
	Editor_Info( hwndView, EI_GET_FILE_NAMEW, (LPARAM)sz );   // 無題の文書は「無題-1」のような名前
	return sz;
}

inline std::wstring GetDocName( HWND hwndView )
{
	const std::wstring path = GetDocPath( hwndView );
	return PathFindFileNameW( path.c_str() );
}

// 保存済みの文書か（フルパスを持っているか）
inline bool IsSavedDoc( const std::wstring& path )
{
	return path.size() > 2 && ( path[1] == L':' || ( path[0] == L'\\' && path[1] == L'\\' ) );
}

// 論理行の数
inline UINT_PTR GetLineCount( HWND hwndView )
{
	return Editor_GetLines( hwndView, POS_LOGICAL_W );
}

// 大きな文書か。EmEditor は数 GB のログも開けるので、全文を一度に取り出してはいけない文書を見分ける。
// EmEditor 自身の判定（EI_IS_LARGE_DOC）か、行数で判断する
constexpr UINT_PTR kLargeDocLines = 20000;
inline bool IsLargeDoc( HWND hwndView )
{
	return Editor_Info( hwndView, EI_IS_LARGE_DOC, 0 ) != 0 || GetLineCount( hwndView ) > kLargeDocLines;
}

// 1 つの論理行（0 始まり、改行は含めない）。maxChars を超える長い行は先頭だけ読み、truncated を立てる
inline std::wstring GetLineText( HWND hwndView, UINT_PTR y, size_t maxChars, bool* truncated = nullptr )
{
	GET_LINE_INFO gli = {};
	gli.flags = FLAG_LOGICAL;
	gli.yLine = y;
	const UINT_PTR need = Editor_GetLineW( hwndView, &gli, nullptr );   // 終端 NUL を含む必要サイズ
	if( truncated )  *truncated = false;
	if( need <= 1 )  return {};
	UINT_PTR cch = need;
	if( need - 1 > maxChars ){
		cch = maxChars + 1;
		if( truncated )  *truncated = true;
	}
	std::wstring s( cch, L'\0' );
	gli.cch = cch;
	Editor_GetLineW( hwndView, &gli, s.data() );
	s.resize( wcslen( s.c_str() ) );
	return s;
}

// 行の範囲（0 始まり、最大 maxLines 行・合計 maxChars 文字まで）を \n でつないで読む。
// 読んだ最後の行の次の行番号を nextLine に返す（文書の末尾まで読めたら行数と同じ）
inline std::wstring ReadLines( HWND hwndView, UINT_PTR startLine, UINT_PTR maxLines, size_t maxChars, UINT_PTR& nextLine, bool* anyLineTruncated = nullptr )
{
	const UINT_PTR lines = GetLineCount( hwndView );
	std::wstring text;
	UINT_PTR y = startLine;
	if( anyLineTruncated )  *anyLineTruncated = false;
	for( ; y < lines && y - startLine < maxLines; y++ ){
		if( text.size() >= maxChars )  break;
		bool cut = false;
		std::wstring line = GetLineText( hwndView, y, maxChars - text.size(), &cut );
		if( cut && anyLineTruncated )  *anyLineTruncated = true;
		if( y > startLine )  text += L'\n';
		text += line;
	}
	nextLine = y;
	return text;
}

// アクティブな文書の全文。Editor_GetTextW を試し、取れなければ 1 行ずつ集める。
// 大きな文書では使わないこと（IsLargeDoc で確かめる）
inline std::wstring GetDocumentText( HWND hwndView )
{
	const UINT_PTR cch = Editor_GetTextW( hwndView, 0, nullptr );
	if( cch > 1 ){
		std::wstring s( cch, L'\0' );
		Editor_GetTextW( hwndView, cch, s.data() );
		s.resize( wcslen( s.c_str() ) );
		if( !s.empty() )  return s;
	}
	std::wstring text;
	const UINT_PTR lines = Editor_GetLines( hwndView, POS_LOGICAL_W );
	for( UINT_PTR y = 0; y < lines; y++ ){
		GET_LINE_INFO gli = {};
		gli.flags = FLAG_LOGICAL | FLAG_WITH_CRLF;
		gli.yLine = y;
		const UINT_PTR need = Editor_GetLineW( hwndView, &gli, nullptr );
		if( need <= 1 )  continue;
		std::wstring line( need, L'\0' );
		gli.cch = need;
		Editor_GetLineW( hwndView, &gli, line.data() );
		line.resize( wcslen( line.c_str() ) );
		text += line;
	}
	return text;
}

inline std::wstring GetSelectedText( HWND hwndView )
{
	const UINT_PTR cch = Editor_GetSelTextW( hwndView, 0, nullptr );
	if( cch <= 1 )  return {};
	std::wstring s( cch, L'\0' );
	Editor_GetSelTextW( hwndView, cch, s.data() );
	s.resize( wcslen( s.c_str() ) );
	return s;
}

// 改行を \n に揃える（\r\n と単独の \r を 1 つの改行として扱う）
inline std::wstring ToLf( std::wstring_view s )
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

// \n に揃えたテキスト上のオフセットを、論理行・桁（0 始まり）に変換
inline POINT_PTR OffsetToPos( std::wstring_view lf, size_t offset )
{
	POINT_PTR pt = {};
	size_t lineStart = 0;
	for( size_t i = 0; i < offset && i < lf.size(); i++ ){
		if( lf[i] == L'\n' ){
			pt.y++;
			lineStart = i + 1;
		}
	}
	pt.x = (LONG_PTR)( offset - lineStart );
	return pt;
}

inline void SelectRange( HWND hwndView, POINT_PTR start, POINT_PTR end )
{
	Editor_SetCaretPos( hwndView, POS_LOGICAL_W, &start );
	Editor_SetCaretPosEx( hwndView, POS_LOGICAL_W, &end, TRUE );
}

// 画面の一番上に見えている論理行（0 始まり）。折り返し表示でも論理行に直す
inline LONG_PTR GetTopLogicalLine( HWND hwndView )
{
	POINT_PTR scroll = {};
	Editor_GetScrollPos( hwndView, &scroll );
	POINT_PTR view = { 0, scroll.y };
	POINT_PTR logical = {};
	Editor_ViewToLogical( hwndView, &view, &logical );
	return logical.y;
}

} // namespace ee::doc
