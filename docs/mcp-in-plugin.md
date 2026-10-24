# プラグインの中に MCP サーバーを立て、claude -p に EmEditor を操作させる

Claude Code の Read / Edit ツールと同じ感覚で、Claude が自分で「文書を読む」「ここを書き換える」を判断して実行できるようにする方法。
`claude -p`（サブスクで使える）のまま、組み込みツールは無効にしたうえで、EmEditor 用のツールだけを渡す。

- 実装: [shared/mcp_server.h](../shared/mcp_server.h)（HTTP + JSON-RPC）、[shared/editor_tools.h](../shared/editor_tools.h)（ツール本体）
- 使用例: [plugins/claude_chat](../plugins/claude_chat/)

## 構成

```
claude.exe -p --tools "" --strict-mcp-config
           --mcp-config '{"mcpServers":{"emeditor":{"type":"http","url":"http://127.0.0.1:<port>/mcp","headers":{"Authorization":"Bearer <token>"}}}}'
           --allowedTools mcp__emeditor
     │ ツール呼び出し（HTTP POST, JSON-RPC）
     ▼
プラグイン DLL 内の MCP サーバー（専用スレッド）
     │ SendMessageTimeout（UI スレッドへ受け渡し）
     ▼
ダイアログの WndProc（UI スレッド）で Editor_* を実行
```

- **別の exe は不要。** HTTP 方式の MCP なら、DLL の中で待ち受けられる（stdio 方式だと claude が子プロセスを起動するので exe が要る）
- `--mcp-config` には JSON 文字列をそのまま渡せる（一時ファイル不要）
- `--allowedTools mcp__emeditor` で、このサーバーのツールは確認なしで使える（`-p` では確認ダイアログを出せないので必須）
- `--tools ""` と `--strict-mcp-config` は付けたまま。組み込みツールもユーザー設定の MCP も使わせない

## 安全策

| 対策 | 確認結果（2026-10-24, curl で検証） |
|---|---|
| 127.0.0.1 にだけ bind | LAN 側の IP からは接続できない |
| ポートは OS 任せ（毎回変わる） | — |
| 起動ごとのランダムトークン（`BCryptGenRandom` 24 バイト）。`Authorization: Bearer` が違えば 401 | トークンなし・違い → 401 |
| `Origin` ヘッダー付き（ブラウザから）は 403 | 403 |

## MCP（Streamable HTTP）の最小実装で分かったこと

claude（Claude Code 2.1.302）が送ってくるもの:

1. `server/discover`（新しい仕様のプローブ）→ `-32601 method not found` を返せば、`initialize` に切り替えてくる
2. `initialize` → `protocolVersion` は要求に入っている値をそのまま返せばよい。`capabilities: { tools: {} }`
3. `notifications/initialized`（id なし）→ HTTP 202 で空応答
4. `GET /mcp`（サーバーからのストリーム用）→ 405 を返せば、POST だけで動く
5. `tools/list` → `tools/call`

- 応答は `Content-Type: application/json` の 1 回で返せばよく、SSE は要らない
- 毎回 `Connection: close` にして 1 接続ずつ処理しても問題なかった
- ツールの結果は `{ content: [{ type: "text", text: "..." }], isError: bool }`。エラーは `isError: true` で返すと、Claude が読んでやり方を変える

## スレッドとデッドロック

- ツール呼び出しはサーバーのスレッドに届く。`Editor_*` は UI スレッドで呼ぶ必要があるので、ダイアログへ `SendMessageTimeout` で渡す
- UI スレッドが「ワーカー（claude 待ち）」や「サーバーのスレッド」の終了を待つとき、ふつうの `join` だと
  「サーバーのスレッドが UI へ SendMessage して待つ」「UI はサーバーの終了を待つ」でデッドロックする
  → `MsgWaitForMultipleObjects( ..., QS_SENDMESSAGE )` ＋ `PeekMessage( PM_QS_SENDMESSAGE )` で、**送られてきたメッセージを処理しながら待つ**（`ee::mcp::JoinPumpingSentMessages`）
- パネルを閉じる処理の途中に届いたツール呼び出しは、実行せずにエラーを返す（claude はもう止めているので、編集だけが反映されてしまう）

## ツール設計（editor_tools.h）

| ツール | 内容 |
|---|---|
| `get_active_document` | 名前・パス・行数・全文（`with_line_numbers` で行番号付き） |
| `get_selection` | 選択範囲のテキストと位置 |
| `list_documents` | タブの一覧（`EI_GET_DOC_COUNT` / `Editor_DocInfo`） |
| `replace_text` | Claude Code の Edit と同じ。`old_string` が 1 か所だけ一致するときに置換。0 か所・複数は失敗を返す |
| `insert_text` | カーソル位置 / 先頭 / 末尾 |
| `set_document_text` | 全体を置き換え（メモ全体を整理し直すとき） |
| `new_document` | 新しいタブに書き込む |

- 置換は「\n に揃えたテキストで検索 → オフセットを論理行・桁に変換 → `Editor_SetCaretPos` / `Editor_SetCaretPosEx(…, TRUE)` で選択 → `Editor_InsertW`」
- 複数か所の置換は後ろから行う（前の一致の位置がずれない）
- EmEditor には Undo をまとめる API が無いので、1 回の置換ごとに Ctrl+Z 1 回で戻る
- 送信時には文書の本文を送らず、`<editor_state active_document="名前" />` だけを付ける。内容は Claude が必要なときに読みに来る

## 実測（コマンドラインからの模擬, 2026-10-24）

- 「アクティブな文書の『未定』を『デザイン案を来週提示』に書き換えて」→ `get_active_document` → `replace_text` の順に呼び、その行だけ書き換えた（$0.05）
- 読むだけの質問は 2 ターン（ツールを呼ぶ回と答える回）、$0.03 程度
