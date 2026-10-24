// お願いテンプレート（ワンタッチで送る依頼文）。
// %APPDATA%\EmEditorPlugins\claude_chat\templates.json に保存し、ユーザーは EmEditor で開いて編集する。
// ファイルが無ければ既定のテンプレートで作る。更新日時を見て、保存されたら読み込み直す。
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "process_runner.h"

struct ChatTemplate
{
	std::wstring         name;
	std::wstring         icon;
	std::wstring         prompt;
	bool                 newChat = true;   // これまでの会話を引きずらず、新しい会話で実行する
	std::wstring         model;            // 空ならパネルの選択に従う
	std::wstring         effort;
	std::optional<bool>  web;              // 指定があればパネルの Web チェックより優先
};

class TemplateStore
{
public:
	const std::vector<ChatTemplate>& Items() const { return m_items; }
	const std::wstring& Path() const { return m_path; }
	const std::wstring& LastError() const { return m_error; }
	void SetPath( std::wstring path ){ m_path = std::move( path ); }   // テスト用（通常は %APPDATA% 以下）

	// 読み込む（無ければ既定で作る）。変化が無ければ false
	bool Reload( bool force = false )
	{
		if( m_path.empty() )  m_path = DefaultPath();
		if( GetFileAttributesW( m_path.c_str() ) == INVALID_FILE_ATTRIBUTES )  WriteDefaults();

		const FILETIME ft = ModifiedTime();
		if( !force && CompareFileTime( &ft, &m_loadedTime ) == 0 )  return false;
		m_loadedTime = ft;

		m_error.clear();
		std::string data = ReadAll( m_path );
		if( data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF )  data.erase( 0, 3 );
		const nlohmann::json root = nlohmann::json::parse( data, nullptr, false, true );   // コメント可
		if( !root.is_object() || !root.contains( "templates" ) || !root["templates"].is_array() ){
			m_error = L"templates.json を読めません（JSON の書式を確認してください）";
			return true;   // 以前の内容を残す
		}
		std::vector<ChatTemplate> items;
		for( const auto& t : root["templates"] ){
			if( !t.is_object() )  continue;
			ChatTemplate c;
			c.name = W( t.value( "name", "" ) );
			c.icon = W( t.value( "icon", "" ) );
			c.prompt = W( t.value( "prompt", "" ) );
			c.newChat = t.value( "newChat", true );
			c.model = W( t.value( "model", "" ) );
			c.effort = W( t.value( "effort", "" ) );
			if( t.contains( "web" ) && t["web"].is_boolean() )  c.web = t["web"].get<bool>();
			if( !c.name.empty() && !c.prompt.empty() )  items.push_back( std::move( c ) );
		}
		m_items = std::move( items );
		return true;
	}

private:
	std::wstring              m_path;
	std::vector<ChatTemplate> m_items;
	FILETIME                  m_loadedTime = {};
	std::wstring              m_error;

	static std::wstring W( const std::string& s ){ return ee::Utf8ToWide( s ); }

	static std::wstring DefaultPath()
	{
		PWSTR appdata = nullptr;
		std::wstring dir;
		if( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_RoamingAppData, 0, nullptr, &appdata ) ) ){
			dir = std::wstring( appdata ) + L"\\EmEditorPlugins";
			CreateDirectoryW( dir.c_str(), nullptr );
			dir += L"\\claude_chat";
			CreateDirectoryW( dir.c_str(), nullptr );
		}
		CoTaskMemFree( appdata );
		return dir + L"\\templates.json";
	}

	FILETIME ModifiedTime() const
	{
		WIN32_FILE_ATTRIBUTE_DATA fad = {};
		GetFileAttributesExW( m_path.c_str(), GetFileExInfoStandard, &fad );
		return fad.ftLastWriteTime;
	}

	static std::string ReadAll( const std::wstring& path )
	{
		std::string data;
		HANDLE h = CreateFileW( path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr );
		if( h == INVALID_HANDLE_VALUE )  return data;
		LARGE_INTEGER size;
		if( GetFileSizeEx( h, &size ) && size.QuadPart < 4 * 1024 * 1024 ){
			data.resize( (size_t)size.QuadPart );
			DWORD read = 0;
			ReadFile( h, data.data(), (DWORD)data.size(), &read, nullptr );
			data.resize( read );
		}
		CloseHandle( h );
		return data;
	}

	void WriteDefaults()
	{
		const std::string text = DefaultJson();
		HANDLE h = CreateFileW( m_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr );
		if( h == INVALID_HANDLE_VALUE )  return;
		DWORD written = 0;
		WriteFile( h, text.data(), (DWORD)text.size(), &written, nullptr );
		CloseHandle( h );
	}

	// 既定のテンプレート。「選択範囲（無ければ文書全体）」は Claude が get_selection / get_active_document で判断する
	static std::string DefaultJson()
	{
		return u8R"({
  // Claude チャットのお願いテンプレート。保存すると自動で読み込み直します。
  //   name    : ボタンの名前          icon : ボタンの先頭に付ける文字（絵文字など）
  //   prompt  : Claude への依頼文。パネルの入力欄に書いた内容は「補足」として後ろに付きます
  //   newChat : true なら、これまでの会話を引きずらず新しい会話で実行（省略時 true）
  //   model   : "opus" / "sonnet" / "haiku" / "fable"（省略時はパネルの選択）
  //   effort  : "low" / "medium" / "high" / "xhigh" / "max"（省略時はパネルの選択）
  //   web     : true / false（省略時はパネルの Web チェック）
  "templates": [
    {
      "name": "校正",
      "icon": "✏",
      "prompt": "選択範囲（選択が無ければ文書全体）の誤字脱字・表記ゆれ・文法の誤りを直してください。内容や文体は変えずに、文書を直接書き換えてください。最後に、直した点を箇条書きで短く教えてください。"
    },
    {
      "name": "MECE に整理",
      "icon": "🗂",
      "prompt": "選択範囲（選択が無ければ文書全体）は、話を聞きながら取った走り書きのメモです。トピックごとに MECE（漏れなく・重複なく）に分類し、Markdown の見出し（## から）と箇条書きで構造化してください。決定事項・TODO・未決事項に当たる項目は、行頭に【決定】【TODO】【未決】を付けてください。TODO は担当や期限が書かれていれば併記します。メモに無い情報は足さず、誤字脱字は直してください。結果で該当範囲を直接書き換えてください。"
    },
    {
      "name": "要約を新しいタブに",
      "icon": "📝",
      "prompt": "文書全体（選択範囲があればその部分）を要約し、new_document で新しいタブに出力してください。最初に全体像を一文で書き、続けて要点を 3〜7 個の箇条書きにしてください。元の文書は変えないでください。"
    },
    {
      "name": "箇条書きに",
      "icon": "•",
      "prompt": "選択範囲（選択が無ければ文書全体）の文章を、意味を変えずに Markdown の箇条書きに書き換えてください。関係のある項目は入れ子にしてまとめます。文書を直接書き換えてください。"
    },
    {
      "name": "英訳",
      "icon": "🌐",
      "prompt": "選択範囲（選択が無ければ文書全体）を自然な英語に翻訳し、new_document で新しいタブに出力してください。元の文書は変えないでください。"
    },
    {
      "name": "ビジネス文に",
      "icon": "👔",
      "prompt": "選択範囲（選択が無ければ文書全体）を、社外にも出せる丁寧なビジネス文（敬語）に書き換えてください。内容は変えずに、文書を直接書き換えてください。"
    },
    {
      "name": "もっと簡潔に",
      "icon": "✂",
      "prompt": "選択範囲（選択が無ければ文書全体）を、意味を保ったまま短く簡潔に書き換えてください。冗長な言い回しや重複を削ります。文書を直接書き換えてください。"
    },
    {
      "name": "Web で裏付け",
      "icon": "🔎",
      "web": true,
      "prompt": "選択範囲（選択が無ければ文書全体）に書かれている事実関係（数値・日付・固有名詞・主張など）を Web 検索で確認してください。主張ごとに「✅ 確認できた / ⚠ 食い違いあり / ❓ 確認できず」の判定と、根拠の URL を表にして回答してください。食い違いがあれば正しいと考えられる内容も書いてください。文書は書き換えないでください。"
    }
  ]
}
)";
	}
};
