// JSON オブジェクトの「トップレベル」のキーだけを読む最小限のパーサ。
// `claude -p --output-format json` の結果（result / session_id / is_error）を取り出す用途。
// ネストした値は読み飛ばすだけで、中身は解釈しない。
#pragma once

#include <string>
#include <string_view>

namespace ee::json {

namespace detail {

inline void SkipWs( std::string_view s, size_t& i )
{
	while( i < s.size() && ( s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' ) )  i++;
}

inline void AppendUtf8( std::string& out, unsigned cp )
{
	if( cp < 0x80 ){ out += (char)cp; }
	else if( cp < 0x800 ){ out += (char)( 0xC0 | ( cp >> 6 ) ); out += (char)( 0x80 | ( cp & 0x3F ) ); }
	else if( cp < 0x10000 ){ out += (char)( 0xE0 | ( cp >> 12 ) ); out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ); out += (char)( 0x80 | ( cp & 0x3F ) ); }
	else { out += (char)( 0xF0 | ( cp >> 18 ) ); out += (char)( 0x80 | ( ( cp >> 12 ) & 0x3F ) ); out += (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) ); out += (char)( 0x80 | ( cp & 0x3F ) ); }
}

inline bool ParseHex4( std::string_view s, size_t i, unsigned& v )
{
	if( i + 4 > s.size() )  return false;
	v = 0;
	for( size_t k = i; k < i + 4; k++ ){
		const char c = s[k];
		v <<= 4;
		if( c >= '0' && c <= '9' )  v |= c - '0';
		else if( c >= 'a' && c <= 'f' )  v |= c - 'a' + 10;
		else if( c >= 'A' && c <= 'F' )  v |= c - 'A' + 10;
		else return false;
	}
	return true;
}

// s[i] == '"' から文字列を読み、エスケープを解いて out に入れる（UTF-8）。i は閉じ " の次へ進む。
inline bool ParseString( std::string_view s, size_t& i, std::string* out )
{
	if( i >= s.size() || s[i] != '"' )  return false;
	i++;
	while( i < s.size() ){
		const char c = s[i++];
		if( c == '"' )  return true;
		if( c != '\\' ){
			if( out )  *out += c;
			continue;
		}
		if( i >= s.size() )  return false;
		const char e = s[i++];
		switch( e ){
		case '"':  if( out ) *out += '"';  break;
		case '\\': if( out ) *out += '\\'; break;
		case '/':  if( out ) *out += '/';  break;
		case 'b':  if( out ) *out += '\b'; break;
		case 'f':  if( out ) *out += '\f'; break;
		case 'n':  if( out ) *out += '\n'; break;
		case 'r':  if( out ) *out += '\r'; break;
		case 't':  if( out ) *out += '\t'; break;
		case 'u': {
			unsigned cp;
			if( !ParseHex4( s, i, cp ) )  return false;
			i += 4;
			// サロゲートペア
			if( cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u' ){
				unsigned lo;
				if( ParseHex4( s, i + 2, lo ) && lo >= 0xDC00 && lo <= 0xDFFF ){
					cp = 0x10000 + ( ( cp - 0xD800 ) << 10 ) + ( lo - 0xDC00 );
					i += 6;
				}
			}
			if( out )  AppendUtf8( *out, cp );
			break;
		}
		default:
			return false;
		}
	}
	return false;
}

// 任意の値を読み飛ばす
inline bool SkipValue( std::string_view s, size_t& i )
{
	SkipWs( s, i );
	if( i >= s.size() )  return false;
	const char c = s[i];
	if( c == '"' )  return ParseString( s, i, nullptr );
	if( c == '{' || c == '[' ){
		const char close = ( c == '{' ) ? '}' : ']';
		i++;
		SkipWs( s, i );
		if( i < s.size() && s[i] == close ){ i++; return true; }
		while( i < s.size() ){
			if( c == '{' ){
				SkipWs( s, i );
				if( !ParseString( s, i, nullptr ) )  return false;
				SkipWs( s, i );
				if( i >= s.size() || s[i] != ':' )  return false;
				i++;
			}
			if( !SkipValue( s, i ) )  return false;
			SkipWs( s, i );
			if( i < s.size() && s[i] == ',' ){ i++; continue; }
			if( i < s.size() && s[i] == close ){ i++; return true; }
			return false;
		}
		return false;
	}
	// 数値 / true / false / null
	while( i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\r' && s[i] != '\n' && s[i] != '\t' )  i++;
	return true;
}

// トップレベルの key の値の開始位置を返す（見つからなければ npos）
inline size_t FindTopLevel( std::string_view s, std::string_view key )
{
	size_t i = 0;
	SkipWs( s, i );
	if( i >= s.size() || s[i] != '{' )  return std::string_view::npos;
	i++;
	while( i < s.size() ){
		SkipWs( s, i );
		std::string k;
		if( !ParseString( s, i, &k ) )  return std::string_view::npos;
		SkipWs( s, i );
		if( i >= s.size() || s[i] != ':' )  return std::string_view::npos;
		i++;
		SkipWs( s, i );
		if( k == key )  return i;
		if( !SkipValue( s, i ) )  return std::string_view::npos;
		SkipWs( s, i );
		if( i < s.size() && s[i] == ',' ){ i++; continue; }
		break;
	}
	return std::string_view::npos;
}

} // namespace detail

// トップレベルの文字列値を取り出す（UTF-8）
inline bool GetString( std::string_view json, std::string_view key, std::string& out )
{
	size_t i = detail::FindTopLevel( json, key );
	if( i == std::string_view::npos )  return false;
	out.clear();
	return detail::ParseString( json, i, &out );
}

// トップレベルの真偽値を取り出す
inline bool GetBool( std::string_view json, std::string_view key, bool& out )
{
	const size_t i = detail::FindTopLevel( json, key );
	if( i == std::string_view::npos )  return false;
	if( json.substr( i, 4 ) == "true" ){ out = true; return true; }
	if( json.substr( i, 5 ) == "false" ){ out = false; return true; }
	return false;
}

} // namespace ee::json
