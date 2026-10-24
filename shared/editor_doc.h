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

// アクティブな文書の全文。Editor_GetTextW を試し、取れなければ 1 行ずつ集める
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
