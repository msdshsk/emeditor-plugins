# claude_chat

EmEditor の右側にパネルを出し、`claude -p` と会話する。
Claude は EmEditor の文書を**ツールで読み書きできる**（Claude Code の Read / Edit と同じ感覚）。

## 使い方

- プラグイン メニュー > Claude チャット でパネルを開閉
- 下の入力欄に書いて **Ctrl+Enter** か「送信」。Enter は改行
- **Esc** でエディタにフォーカスを戻す
- 入力欄の下に「アクティブ: <文書名>（N 行）」が出る。送信時にこの文書の**名前だけ**が Claude に伝わり、内容は Claude が必要なときに読みに来る
- 「〜を直して」「全体を整理して」と頼むと、Claude が文書を直接編集する。**編集は Ctrl+Z で戻せる**
- 履歴欄に、Claude が何をしたか（📖 読み取り / ✏️ 置換 など）が出る
- 「応答を挿入」で、最後の応答をエディタのカーソル位置に入れる
- 「新しい会話」で会話をリセット。応答待ちの間は「送信」が「キャンセル」になる
- Claude Code に未ログインだと「ログイン」ボタンが出る（パネルを開いたときに `claude auth status` で確認、送信時に「Not logged in」が返った場合も）。
  押すとコンソール窓で `claude auth login` が起動し、窓が閉じたらログイン状態を確認し直す

## お願いテンプレート（ワンタッチ）

履歴の下にテンプレートのボタンが並び、押すとすぐ送信する。

- 入力欄に何か書いてあれば「補足」として一緒に送る（例: 「✏ 校正」＋「です・ます調で」）
- 既定のテンプレート: ✏ 校正 / 🗂 MECE に整理 / 📝 要約を新しいタブに / • 箇条書きに / 🌐 英訳 / 👔 ビジネス文に / ✂ もっと簡潔に / 🔎 Web で裏付け
- 「＋ 編集」で `%APPDATA%\EmEditorPlugins\claude_chat\templates.json` を EmEditor で開く。**保存すると 2 秒以内に読み込み直す**
- 書式（JSONC。`//` コメント可）:

  ```jsonc
  {
    "templates": [
      {
        "name": "校正",          // ボタンの名前
        "icon": "✏",             // 先頭に付ける文字（省略可）
        "prompt": "選択範囲（選択が無ければ文書全体）の…",   // Claude への依頼文
        "newChat": true,          // 新しい会話で実行（省略時 true）
        "model": "haiku",         // 省略時はパネルの選択
        "effort": "low",          // 省略時はパネルの選択
        "web": true               // 省略時はパネルの Web チェック
      }
    ]
  }
  ```
- `newChat: true` のテンプレートは、これまでの会話を引きずらずに実行する（履歴の表示は残し、「── 新しい会話 ──」を挟む）
- 応答待ちの間はボタンが押せない
- JSON の書式が壊れていると、ボタン列の下に赤字で出て、前回読めた内容のまま動く
- リッチエディットの簡易表示（WebView2 が無い環境）ではボタン列は出ない

## モデル・Effort・Web・MCP

| 項目 | 置き場所 | 内容 |
|---|---|---|
| モデル | パネル上部 | Opus / Sonnet / Haiku / Fable（`--model`）。会話の途中でも切り替えられ、次の送信から有効 |
| Effort | パネル上部 | low / medium / high / xhigh / max（`--effort`） |
| Web | パネル上部 | オンで WebSearch / WebFetch を使わせる（`--tools "WebSearch,WebFetch"` ＋ `--allowedTools`） |
| 既定値 | 設定画面 | パネルを開いたときのモデル・Effort・Web。既定は **Opus / medium / Web オフ** |
| MCP サーバー | 設定画面 | `~/.claude.json` のユーザー設定の MCP から、使うものをチェック。EmEditor 操作用は常に有効 |

- 設定画面は、パネルの「設定」ボタンか「プラグインの設定 > Claude チャット > プロパティ」で開く
- 設定画面で既定値を変えても、開いているパネルのモデル等は変わらない（次にパネルを開いたときから）
- MCP の設定（API キーなどを含みうる）はコマンドラインに載せず、一時ファイル（`%TEMP%\emeditor-claude-chat\mcp-*.json`）で渡し、応答が終わったら削除する
- 送信のたびに、使ったモデル等が履歴に小さく出る（例: `（Opus / medium / Web / MCP: x-research）`）
- `--resume` の途中でモデルを変えても会話は続く（Sonnet → Haiku で確認, 2026-10-25）

## 表示

履歴欄は **WebView2**（[web/chat.html](web/chat.html) を `history.h` から表示）。詳細は [docs/webview2.md](../../docs/webview2.md)。

- ユーザーの発言は右寄せの吹き出し。下に使ったモデル等を小さく表示
- Claude の応答は marked（GFM）で描画: 見出し・表・タスクリスト・コードブロック・引用・リンクなど。明暗テーマに追従
- 応答ごとに「コピー」「エディタに挿入」ボタン
- ツールの実行（📖 読み取り / ✏️ 置換 など）は小さな灰色の文字
- 応答待ちの間は「●●● Claude が考え中… N 秒」（点が跳ねるアニメーション）
- リンクはクリックすると http(s) のものだけ既定のブラウザで開く
- WebView2 Runtime が無い環境では、リッチエディットの簡易表示（`chat_view.h`）に自動で切り替わる

## Claude が使えるツール

| ツール | 内容 |
|---|---|
| `get_active_document` | アクティブな文書の情報と本文（行の範囲を指定して読む。既定 2000 行まで） |
| `search_document` | EmEditor の検索で、一致の件数と一致した行を返す（大きなログから目的の箇所を探す） |
| `extract_lines` | 一致した行だけを新しいタブに抜き出す（EmEditor の「抽出」） |
| `replace_all` | 一致をすべて置き換える（EmEditor の「すべて置換」。正規表現・後方参照可） |
| `filter_view` / `get_filter` | 一致した行だけを画面に表示する（EmEditor のフィルター）／今のフィルター設定を返す |
| `get_selection` | 選択範囲 |
| `list_documents` | 開いているタブの一覧 |
| `replace_text` | 部分的な置換（一致が 1 か所のときだけ） |
| `insert_text` | カーソル位置 / 先頭 / 末尾に挿入 |
| `set_document_text` | 全体を置き換え |
| `new_document` | 新しいタブに書き込む |

仕組みと安全策は [docs/mcp-in-plugin.md](../../docs/mcp-in-plugin.md)。

**大きなファイルについて**: EmEditor は数 GB のログも開けるので、Claude が文書を読むときは全文を一度に取り出さず、必要な行の範囲だけを読みます。
大きな文書（2 万行超など）では、Claude は検索してから周辺を読み、置換には範囲の指定が必要になり、全体の置き換えはできません。

## 設計の経緯（アクティブな文書の扱い）

1. **全文を毎回添付**（2026-10-24 前半）: 送信時のアクティブな文書を `<active_document>` で全文添付し、変わっていなければ `unchanged` とだけ送る。
   動いたが、「添付」のチェック次第で Claude から文書が見えたり見えなかったりし、Claude が自分で読む・直すことはできなかった
2. **ツールで読み書き**（現在）: プラグイン内に MCP サーバーを立て、Claude が必要なときに読み、直接編集する。
   送信時には文書の名前だけを `<editor_state active_document="..." />` で伝える

どちらも「会話は 1 本（EmEditor のウィンドウごと）。タブを切り替えても何も送らず、送信時点の状態を伝える」という考え方は同じ。

## 仕組み

```
1 回目: claude.exe -p --strict-mcp-config --output-format json --model opus --effort medium
                   --tools ""（Web オンなら "WebSearch,WebFetch"） --system-prompt "..."
                   --mcp-config <一時ファイル: emeditor ＋ 選んだユーザー MCP>
                   --allowedTools "mcp__emeditor,..."
2 回目以降: 上記 + --resume <1 回目の session_id>
  stdin  ← <editor_state active_document="..."/> ＋ 入力
  stdout → JSON（result / session_id / is_error）
```

- 作業フォルダは `%TEMP%\emeditor-claude-chat`（固定。`--resume` はフォルダ単位でセッションを探すため）
- セッション履歴は Claude Code の通常の保存先（`~/.claude/projects/` 以下）に残る

## ライセンス

[MIT License](../../LICENSE)。同梱物（nlohmann/json、marked、WebView2 SDK、EmEditor プラグイン SDK）のライセンスは [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) を参照してください。

EmEditor は Emurasoft, Inc. の商標、Claude は Anthropic, PBC の商標です。本プラグインはどちらの会社とも関係のない非公式のものです。

## 制限・今後

- 応答は完了してからまとめて表示する（ストリーミング表示は未実装）
- 明暗は Windows のアプリモード（`prefers-color-scheme`）に追従し、EmEditor のテーマとは連動しない
- パネルの位置は右固定
- `list_documents` の `Editor_DocInfo( …, EI_GET_FILE_NAMEW, … )` は EmEditor 上で未確認
