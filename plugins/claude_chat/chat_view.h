// チャット履歴の表示（リッチエディット）。
// 発言者ごとに色付きの見出しを付け、Claude の応答は Markdown を簡易的に装飾する。
//
// リッチエディットの注意:
//   - ダイアログテンプレートで "RICHEDIT50W" を使う前に Msftedit.dll を LoadLibrary しておく
//   - SES_EMULATESYSEDIT を付けると「普通の EDIT のふり」をして、文字色・太字・字下げがすべて無視される（実際に踏んだ）
//   - 末尾に追記するときは EM_EXSETSEL で末尾を選択 → EM_SETPARAFORMAT / EM_SETCHARFORMAT(SCF_SELECTION) → EM_REPLACESEL
//   - 改行は \r（\n を渡しても段落区切りになるが、\r に揃えておく）
//   - 読み取り専用でも EM_REPLACESEL でプログラムからは書き込める
#pragma once

#include <windows.h>
#include <richedit.h>
#include <string>
#include <string_view>
#include <vector>
#include "history.h"

namespace chatview {

// 色（白背景前提。EmEditor のテーマとは独立）
constexpr COLORREF kUserBadge   = RGB( 0x2B, 0x6C, 0xB0 );
constexpr COLORREF kUserBack    = RGB( 0xE8, 0xF0, 0xFC );
constexpr COLORREF kClaudeBadge = RGB( 0xC1, 0x5F, 0x3C );
constexpr COLORREF kToolText    = RGB( 0x70, 0x70, 0x70 );
constexpr COLORREF kErrorText   = RGB( 0xC0, 0x20, 0x20 );
constexpr COLORREF kCodeBack    = RGB( 0xF2, 0xF2, 0xF2 );
constexpr COLORREF kQuoteText   = RGB( 0x60, 0x60, 0x60 );
constexpr COLORREF kLinkText    = RGB( 0x1A, 0x5F, 0xB4 );
constexpr COLORREF kWhite       = RGB( 0xFF, 0xFF, 0xFF );
constexpr COLORREF kAuto        = CLR_INVALID;

constexpr LPCWSTR kFont     = L"Yu Gothic UI";
constexpr LPCWSTR kMonoFont = L"BIZ UDGothic";
constexpr int     kSizeTw   = 190;   // 9.5pt（1/20 pt 単位）
constexpr int     kIndentTw = 160;   // 本文の字下げ

struct Style
{
	bool     bold = false;
	bool     italic = false;
	bool     underline = false;
	bool     mono = false;
	int      sizeTw = kSizeTw;
	COLORREF color = kAuto;
	COLORREF back = kAuto;
};

inline void LoadRichEdit()
{
	static HMODULE h = LoadLibraryW( L"Msftedit.dll" );
	(void)h;
}

class View : public IHistory
{
public:
	// ---- IHistory ----

	void User( std::wstring_view text, std::wstring_view meta ) override
	{
		User( text );
		if( !meta.empty() )  Tool( meta );
	}

	void StartThinking() override
	{
		m_thinkingStart = GetTickCount64();
		m_thinkingFrame = 0;
		StartIndicator( ThinkingText() );
	}

	void TickThinking() override
	{
		m_thinkingFrame++;
		UpdateIndicator( ThinkingText() );
	}

	void StopThinking() override { StopIndicator(); }

	// ---- ここから下はリッチエディット固有 ----

	void Attach( HWND hwnd )
	{
		m_hwnd = hwnd;
		SendMessageW( m_hwnd, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor( COLOR_WINDOW ) );
		SendMessageW( m_hwnd, EM_EXLIMITTEXT, 0, (LPARAM)0x7FFFFFFF );
		SendMessageW( m_hwnd, EM_AUTOURLDETECT, AURL_ENABLEURL, 0 );   // 素の URL もリンクにする
		SendMessageW( m_hwnd, EM_SETEVENTMASK, 0, ENM_LINK );           // クリックを EN_LINK で親に通知
	}

	void Clear() override
	{
		m_indicatorStart = -1;
		m_indicatorOn = false;
		m_links.clear();
		SetWindowTextW( m_hwnd, L"" );
	}

	// ---- 発言単位 ----

	void Notice( std::wstring_view text ) override
	{
		Append( [&]{
			Style s;
			s.color = kToolText;
			Para( 0, 0, 60 );
			Run( std::wstring( text ) + L"\r", s );
		} );
	}

	void User( std::wstring_view text )
	{
		Append( [&]{
			Badge( L" あなた ", kUserBadge );
			Style s;
			s.back = kUserBack;
			Para( kIndentTw, 20, 0 );
			for( const std::wstring& line : Lines( text ) ){
				Run( L" " + line + L" \r", s );
			}
			Gap();
		} );
	}

	void Claude( double seconds, std::wstring_view markdown ) override
	{
		Append( [&]{
			WCHAR sz[64];
			swprintf_s( sz, L" Claude（%.1f 秒） ", seconds );
			Badge( sz, kClaudeBadge );
			Markdown( markdown );
			Gap();
		} );
	}

	void Tool( std::wstring_view text ) override
	{
		Append( [&]{
			Style s;
			s.color = kToolText;
			s.sizeTw = kSizeTw - 20;
			Para( kIndentTw, 0, 0 );
			Run( std::wstring( text ) + L"\r", s );
		} );
	}

	void Error( std::wstring_view text ) override
	{
		Append( [&]{
			Style s;
			s.color = kErrorText;
			Para( kIndentTw, 20, 0 );
			Run( std::wstring( text ) + L"\r", s );
			Gap();
		} );
	}

	// ---- 「考え中」の表示（履歴の末尾に 1 行。追記があってもその下に居続ける） ----

	void StartIndicator( std::wstring_view text )
	{
		m_indicatorOn = true;
		m_indicatorText = text;
		Redraw( [&]{ AddIndicator(); } );
	}

	void UpdateIndicator( std::wstring_view text )
	{
		if( !m_indicatorOn )  return;
		m_indicatorText = text;
		Redraw( [&]{ RemoveIndicator(); AddIndicator(); } );
	}

	void StopIndicator()
	{
		if( !m_indicatorOn )  return;
		m_indicatorOn = false;
		Redraw( [&]{ RemoveIndicator(); } );
	}

	// ---- リンク ----

	// EN_LINK のクリック位置から URL を返す（Markdown のリンク、または自動検出された URL）
	std::wstring LinkAt( const CHARRANGE& range ) const
	{
		for( const LinkRange& l : m_links ){
			if( range.cpMin >= l.cpMin && range.cpMin < l.cpMax )  return l.url;
		}
		std::wstring text( (size_t)( range.cpMax - range.cpMin ) + 1, L'\0' );
		TEXTRANGEW tr = { range, text.data() };
		const LRESULT n = SendMessageW( m_hwnd, EM_GETTEXTRANGE, 0, (LPARAM)&tr );
		text.resize( (size_t)n );
		return text;
	}

private:
	struct LinkRange { LONG cpMin; LONG cpMax; std::wstring url; };

	HWND                    m_hwnd = nullptr;
	std::vector<LinkRange>  m_links;
	ULONGLONG               m_thinkingStart = 0;
	int                     m_thinkingFrame = 0;

	std::wstring ThinkingText() const
	{
		static const wchar_t kFrames[] = L"◐◓◑◒";
		const ULONGLONG sec = ( GetTickCount64() - m_thinkingStart ) / 1000;
		return std::wstring( 1, kFrames[m_thinkingFrame % 4] ) + L" Claude が考え中… " + std::to_wstring( sec ) + L" 秒";
	}
	LONG                    m_indicatorStart = -1;
	bool                    m_indicatorOn = false;
	std::wstring            m_indicatorText;

	// 追記の前に「考え中」の行を外し、後で付け直す
	template <class F>
	void Append( F&& body )
	{
		Redraw( [&]{
			RemoveIndicator();
			body();
			if( m_indicatorOn )  AddIndicator();
		} );
	}

	// 書き換え中のちらつきを抑える
	template <class F>
	void Redraw( F&& body )
	{
		SendMessageW( m_hwnd, WM_SETREDRAW, FALSE, 0 );
		body();
		SendMessageW( m_hwnd, WM_SETREDRAW, TRUE, 0 );
		InvalidateRect( m_hwnd, nullptr, TRUE );
		SendMessageW( m_hwnd, WM_VSCROLL, SB_BOTTOM, 0 );
	}

	// 末尾の文字位置（cp）。EM_EXSETSEL / EM_REPLACESEL と同じ cp で数えるため、末尾を選択して位置を読む
	// （EM_GETTEXTLENGTHEX( GTL_NUMCHARS ) でも同じ値だった。2026-10-25 確認）
	LONG TextLength() const
	{
		CHARRANGE cr = { -1, -1 };
		SendMessageW( m_hwnd, EM_EXSETSEL, 0, (LPARAM)&cr );
		SendMessageW( m_hwnd, EM_EXGETSEL, 0, (LPARAM)&cr );
		return cr.cpMin;
	}

	void AddIndicator()
	{
		m_indicatorStart = TextLength();
		Style s;
		s.color = kClaudeBadge;
		s.italic = true;
		Para( kIndentTw, 60, 0 );
		Run( m_indicatorText, s );
	}

	void RemoveIndicator()
	{
		if( m_indicatorStart < 0 )  return;
		CHARRANGE cr = { m_indicatorStart, -1 };
		SendMessageW( m_hwnd, EM_EXSETSEL, 0, (LPARAM)&cr );
		SendMessageW( m_hwnd, EM_REPLACESEL, FALSE, (LPARAM)L"" );
		m_indicatorStart = -1;
	}

	static std::vector<std::wstring> Lines( std::wstring_view text )
	{
		std::vector<std::wstring> r;
		std::wstring cur;
		for( size_t i = 0; i < text.size(); i++ ){
			const wchar_t c = text[i];
			if( c == L'\r' || c == L'\n' ){
				if( c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n' )  i++;
				r.push_back( cur );
				cur.clear();
			}
			else {
				cur += c;
			}
		}
		r.push_back( cur );
		return r;
	}

	void SelectEnd()
	{
		CHARRANGE cr = { -1, -1 };
		SendMessageW( m_hwnd, EM_EXSETSEL, 0, (LPARAM)&cr );
	}

	// これから追記する段落の字下げと前後の間隔（twips）
	void Para( int indentTw, int spaceBeforeTw, int spaceAfterTw )
	{
		SelectEnd();
		PARAFORMAT2 pf = { sizeof( pf ) };
		pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_SPACEBEFORE | PFM_SPACEAFTER;
		pf.dxStartIndent = indentTw;
		pf.dxOffset = 0;
		pf.dySpaceBefore = spaceBeforeTw;
		pf.dySpaceAfter = spaceAfterTw;
		SendMessageW( m_hwnd, EM_SETPARAFORMAT, 0, (LPARAM)&pf );
	}

	void Run( const std::wstring& text, const Style& s, bool link = false )
	{
		SelectEnd();
		CHARFORMAT2W cf = { sizeof( cf ) };
		cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_LINK | CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BACKCOLOR | CFM_CHARSET;
		cf.dwEffects = ( s.bold ? CFE_BOLD : 0 ) | ( s.italic ? CFE_ITALIC : 0 ) | ( s.underline ? CFE_UNDERLINE : 0 ) | ( link ? CFE_LINK : 0 )
			| ( s.color == kAuto ? CFE_AUTOCOLOR : 0 ) | ( s.back == kAuto ? CFE_AUTOBACKCOLOR : 0 );
		cf.yHeight = s.sizeTw;
		cf.crTextColor = s.color == kAuto ? 0 : s.color;
		cf.crBackColor = s.back == kAuto ? 0 : s.back;
		cf.bCharSet = DEFAULT_CHARSET;
		wcscpy_s( cf.szFaceName, s.mono ? kMonoFont : kFont );
		SendMessageW( m_hwnd, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf );
		SendMessageW( m_hwnd, EM_REPLACESEL, FALSE, (LPARAM)text.c_str() );
	}

	void LinkRun( const std::wstring& text, const std::wstring& url, Style base )
	{
		const LONG start = TextLength();
		base.color = kLinkText;
		base.underline = true;
		Run( text, base, true );
		m_links.push_back( { start, TextLength(), url } );
	}

	// 白抜き文字の見出し（バッジ）
	void Badge( const std::wstring& label, COLORREF back )
	{
		Para( 0, 120, 40 );
		Style s;
		s.bold = true;
		s.color = kWhite;
		s.back = back;
		Run( label, s );
		Run( L"\r", Style{} );
	}

	void Gap()
	{
		Para( 0, 0, 0 );
		Style s;
		s.sizeTw = 80;
		Run( L"\r", s );
	}

	// 1 行の中の **太字** / `コード` / [テキスト](URL) を装飾して出す
	void Inline( std::wstring_view line, Style base )
	{
		size_t i = 0;
		std::wstring buf;
		auto flush = [&]( const Style& st ){
			if( !buf.empty() ){ Run( buf, st ); buf.clear(); }
		};
		while( i < line.size() ){
			if( line.compare( i, 2, L"**" ) == 0 ){
				const size_t end = line.find( L"**", i + 2 );
				if( end != std::wstring_view::npos && end > i + 2 ){
					flush( base );
					Style b = base;
					b.bold = true;
					buf = line.substr( i + 2, end - i - 2 );
					flush( b );
					i = end + 2;
					continue;
				}
			}
			if( line[i] == L'`' ){
				const size_t end = line.find( L'`', i + 1 );
				if( end != std::wstring_view::npos && end > i + 1 ){
					flush( base );
					Style c = base;
					c.mono = true;
					c.back = kCodeBack;
					buf = line.substr( i + 1, end - i - 1 );
					flush( c );
					i = end + 1;
					continue;
				}
			}
			if( line[i] == L'[' ){
				const size_t close = line.find( L"](", i + 1 );
				const size_t paren = close == std::wstring_view::npos ? close : line.find( L')', close + 2 );
				if( close != std::wstring_view::npos && paren != std::wstring_view::npos && close > i + 1 ){
					flush( base );
					LinkRun( std::wstring( line.substr( i + 1, close - i - 1 ) ), std::wstring( line.substr( close + 2, paren - close - 2 ) ), base );
					i = paren + 1;
					continue;
				}
			}
			buf += line[i++];
		}
		flush( base );
	}

	static size_t CountLeading( std::wstring_view s, wchar_t c )
	{
		size_t n = 0;
		while( n < s.size() && s[n] == c )  n++;
		return n;
	}

	// 簡易 Markdown: 見出し / 箇条書き / 番号付き / 引用 / 区切り線 / コードブロック / 太字 / インラインコード / リンク
	void Markdown( std::wstring_view text )
	{
		bool inCode = false;
		for( const std::wstring& raw : Lines( text ) ){
			std::wstring_view line = raw;
			const size_t lead = CountLeading( line, L' ' );
			std::wstring_view body = line.substr( lead );

			if( body.compare( 0, 3, L"```" ) == 0 ){
				inCode = !inCode;
				continue;
			}
			if( inCode ){
				Style c;
				c.mono = true;
				c.back = kCodeBack;
				c.sizeTw = kSizeTw - 10;
				Para( kIndentTw * 2, 0, 0 );
				Run( L" " + raw + L" \r", c );
				continue;
			}

			const size_t hashes = CountLeading( body, L'#' );
			if( hashes > 0 && hashes <= 6 && body.size() > hashes && body[hashes] == L' ' ){
				Style h;
				h.bold = true;
				h.sizeTw = kSizeTw + ( hashes <= 2 ? 40 : 20 );
				Para( kIndentTw, 80, 20 );
				Inline( body.substr( hashes + 1 ), h );
				Run( L"\r", h );
				continue;
			}
			if( body == L"---" || body == L"***" || body == L"___" ){
				Style g;
				g.color = kToolText;
				Para( kIndentTw, 0, 0 );
				Run( L"────────────\r", g );
				continue;
			}
			if( body.size() >= 2 && ( body[0] == L'-' || body[0] == L'*' || body[0] == L'+' ) && body[1] == L' ' ){
				Para( kIndentTw + 160 + (int)lead * 80, 0, 0 );
				Run( L"・", Style{} );
				Inline( body.substr( 2 ), Style{} );
				Run( L"\r", Style{} );
				continue;
			}
			if( body.size() >= 2 && body[0] == L'>' ){
				Style q;
				q.color = kQuoteText;
				q.italic = true;
				Para( kIndentTw + 200, 0, 0 );
				Inline( body.substr( body.size() > 1 && body[1] == L' ' ? 2 : 1 ), q );
				Run( L"\r", q );
				continue;
			}
			// 番号付き（1. など）は字下げだけする
			size_t digits = 0;
			while( digits < body.size() && iswdigit( body[digits] ) )  digits++;
			if( digits > 0 && digits + 1 < body.size() && body[digits] == L'.' && body[digits + 1] == L' ' ){
				Para( kIndentTw + 160 + (int)lead * 80, 0, 0 );
				Inline( body, Style{} );
				Run( L"\r", Style{} );
				continue;
			}
			// 表の行（| a | b |）: 区切り行（|---|---|）は出さず、| を罫線 │ に置き換える。
			// （| のまま出すと、リッチエディットがなぜか | だけ別の色で描くことがあった）
			if( body.size() >= 2 && body[0] == L'|' ){
				if( body.find_first_not_of( L"|-: " ) == std::wstring_view::npos )  continue;
				std::wstring row;
				std::wstring_view inner = body.substr( 1 );
				if( !inner.empty() && inner.back() == L'|' )  inner.remove_suffix( 1 );
				size_t start = 0;
				for( ;; ){
					const size_t bar = inner.find( L'|', start );
					std::wstring_view cell = inner.substr( start, bar == std::wstring_view::npos ? std::wstring_view::npos : bar - start );
					while( !cell.empty() && cell.front() == L' ' )  cell.remove_prefix( 1 );
					while( !cell.empty() && cell.back() == L' ' )  cell.remove_suffix( 1 );
					if( !row.empty() )  row += L" │ ";
					row += cell;
					if( bar == std::wstring_view::npos )  break;
					start = bar + 1;
				}
				Para( kIndentTw + 160, 0, 0 );
				Inline( row, Style{} );
				Run( L"\r", Style{} );
				continue;
			}

			Para( kIndentTw, 0, 0 );
			Inline( line, Style{} );
			Run( L"\r", Style{} );
		}
	}
};

} // namespace chatview
