# プラグインから外部プロセスを実行する

`claude -p` のような時間のかかる外部コマンドを、EmEditor を固めずに実行して結果を受け取る方法。
実装は [shared/process_runner.h](../shared/process_runner.h)、使用例は [plugins/memo_tidy](../plugins/memo_tidy/)。

## 全体の流れ

```
UI スレッド（OnCommand）
  ├ 対象テキストと選択範囲を記録
  ├ メッセージ専用ウィンドウ（HWND_MESSAGE）を用意
  └ std::thread でワーカー起動 ──→ ワーカー: ProcessRunner::Run()（ブロッキング）
                                     └ 完了したら結果をメンバに書き、PostMessage(通知ウィンドウ)
UI スレッド（通知ウィンドウの WndProc）
  └ thread.join() → Editor_* で結果を反映
```

## 守ること

| 項目 | 理由 |
|---|---|
| `Editor_*` は UI スレッドからだけ呼ぶ | 中身は `SendMessage`。ワーカーから呼ぶとスレッド間 SendMessage になり、UI スレッドがワーカーを待っているとデッドロックする |
| UI スレッドへの戻りは、メッセージ専用ウィンドウへの `PostMessage` | EmEditor のメッセージループがそのまま配送してくれる。自前のループやタイマー監視は要らない |
| 結果はポインタで渡さず、フレームのメンバに置く | ウィンドウ破棄後に届くメッセージは捨てられるので、ヒープで渡すとリークする |
| `EVENT_CLOSE_FRAME` とデストラクタで Cancel → join → DestroyWindow | フレーム破棄後にワーカーが `this` に触るのを防ぐ |
| ハンドル継承は `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` でパイプだけに絞る | `bInheritHandles=TRUE` だけだと、EmEditor や他プラグインの継承可能ハンドルまで子に渡る |
| 子プロセスはジョブオブジェクト（`KILL_ON_JOB_CLOSE`）に入れる | キャンセル時に子孫ごと止められる。EmEditor が落ちても子が残らない |
| stdin 書き込み・stderr 読み取りは別スレッド | パイプのバッファが満杯になると、親子が互いを待ってデッドロックする |
| `CREATE_NO_WINDOW` | コンソール窓が一瞬出るのを防ぐ |
| 文字コードは UTF-16 ⇔ UTF-8 を自前で変換 | EmEditor の API は UTF-16、`claude` の入出力は UTF-8 |
| 引数のクォートは `CommandLineToArgvW` の規則で | 日本語・`"`・`\` を含むプロンプトを 1 引数として渡すため（`ee::QuoteArg`） |

## 非同期で選択範囲を置き換えるとき

応答を待つ数秒〜十数秒の間に、ユーザーが編集したりタブを切り替えたりできる。完了時に:

1. `EI_GET_FILE_NAMEW` が実行時と同じか（同じ文書か）
2. 記録した範囲（`Editor_GetSelStart/End` の `POS_LOGICAL_W`）を `Editor_SetCaretPos` + `Editor_SetCaretPosEx(..., TRUE)` で選択し直し、
   テキストが元のままか

を確かめ、どちらかが違えば置き換えずにアウトプット バーに出す。
置き換えは `Editor_InsertW( hwndView, text, true )`。`true` は KEEP_DEST_RETURN_TYPE で、改行コードを文書側に合わせる。
選択範囲を `Editor_InsertW` で置き換えると Ctrl+Z 1 回で元に戻せる（2026-10-24 / EmEditor 23.0.5 で確認）。

## claude -p を呼ぶときのオプション

| オプション | 理由 |
|---|---|
| `--tools ""` | 組み込みツールを全部無効化。ファイル操作やコマンド実行をさせない |
| `--strict-mcp-config` | **ユーザー設定の MCP サーバーを読み込まない。** `--tools ""` では MCP のツールは消えない（下記） |
| `--no-session-persistence` | 1 回ごとのセッション履歴を残さない |
| `--system-prompt "..."` | 役割と出力形式の指定。「本文だけを出力、前置き不要」を明示しないと前置きが付く |
| 作業フォルダを空のフォルダにする | claude はカレントの CLAUDE.md / `.claude/` を読み込む |

入力本文は stdin で渡す（`claude -p "指示"` に stdin を付けると、本文として扱われる）。コマンドライン長の制限（32767 文字）も避けられる。

## MCP サーバーとコスト

`--tools ""` だけだと、ユーザー設定（`~/.claude.json` など）の MCP サーバーのツールは有効なまま。
システムプロンプトにそれらのツール定義が載るので、遅く・高くなる。
`--system-prompt` を付けないと、さらに Claude Code 標準のシステムプロンプト（メモリ機能など）が載り、
「覚えておいて」と言うとメモリファイルに書こうとして MCP ツールを探し回る、ということも起きた。

短い校正 1 回の実測（2026-10-24 / Claude Code 2.1.302 / `--system-prompt` あり）:

| オプション | コスト | 応答時間 | 使われたモデル |
|---|---|---|---|
| `--tools ""` | $0.0515 | 4.4 秒 | ユーザー設定のモデル |
| `--tools "" --strict-mcp-config` | $0.0161 | 1.3 秒 | ユーザー設定のモデル |
| 上記 + `--setting-sources ""` | $0.0059 | 2.1 秒 | **既定のモデルに変わる** |

- `--setting-sources ""` で安くなったのはモデルが変わったため（ユーザー設定のモデル選択も読まなくなる）。
  このリポジトリでは `--strict-mcp-config` だけを付け、モデルはユーザー設定に従う。
- `--system-prompt` なし・MCP ありの会話 1 ターンは $0.60 かかった（Claude Code 標準プロンプト＋ MCP ツール定義のキャッシュ作成）。

`--output-format json` を付けると、`total_cost_usd` / `usage` / `modelUsage` で実際のコストとモデルを確認できる。

## モデル・Effort・Web・任意の MCP（2026-10-25 / 2.1.302 で確認）

| やりたいこと | オプション |
|---|---|
| モデル | `--model opus / sonnet / haiku / fable`（エイリアスは最新版を指す） |
| Effort | `--effort low / medium / high / xhigh / max` |
| Web 検索・取得 | `--tools "WebSearch,WebFetch" --allowedTools "WebSearch,WebFetch"`（`-p` では許可を明示しないと使えない） |
| 任意の MCP | `--strict-mcp-config` のまま、使いたいサーバーの設定だけを `--mcp-config <ファイル>` で渡し、`--allowedTools mcp__<名前>` |

- ユーザースコープの MCP サーバーは `~/.claude.json`（`CLAUDE_CONFIG_DIR` があればその下）の `"mcpServers"`。
  このファイルには、大文字小文字だけが違うキー（プロジェクトのパス）が含まれることがあり、PowerShell の `ConvertFrom-Json` は失敗する（`-AsHashtable` が必要）。nlohmann/json は問題なく読める
- MCP の設定には API キーなどが含まれうるので、コマンドラインではなく一時ファイルで渡し、終わったら消す
- `--resume` の途中で `--model` を変えても会話は続く
- 何も指定しないと、ユーザー設定のモデル（例: Fable）がそのまま使われる。文章用途なら Opus / medium で十分

## 会話を続ける（--resume）

- `--output-format json` の出力のトップレベルに `session_id` と `result` がある（[shared/json_lite.h](../shared/json_lite.h) で取り出す）。
- 次の呼び出しで `--resume <session_id>` を付けると、同じ会話の続きになる。`--system-prompt` は毎回付ける。
- `--no-session-persistence` を付けるとセッションが保存されず、resume できない。
- セッションは作業フォルダごとに保存されるので、作業フォルダは固定にする。

## ログイン状態

`claude -p` は、OS 上で一度 `claude` にログイン（OAuth）していれば、その認証を共有して動く。未ログインの検出とログイン:

| 方法 | 未ログインのとき（2026-10-24 / 2.1.302 で確認） |
|---|---|
| `claude auth status --json` | `"loggedIn": false`、終了コード 1。ログイン済みなら `true`・`subscriptionType` なども出る |
| `claude -p --output-format json` | `is_error: true`、`result: "Not logged in · Please run /login"`、終了コード 1 |
| `claude -p`（テキスト出力） | stdout に `Not logged in · Please run /login`、終了コード 1 |

- ログインは `claude auth login`（既定でサブスク / claude.ai。`--console` だと API 課金）。
  ブラウザでの認証のあと、コンソールにコードを貼る場合があるので、`CREATE_NEW_CONSOLE` で**コンソール窓を出して**起動する
- **未ログインの状態を安全に再現する方法**: 環境変数 `CLAUDE_CONFIG_DIR` を空のフォルダにする。
  本来のログイン状態には影響しない。EmEditor をこの環境変数付きで起動すれば、プラグインから起動する claude もすべて未ログインになる

## 実測

- 2026-10-24 / EmEditor 23.0.5 / Claude Code 2.1.302: プラグインから実行し、UI を止めずに選択範囲の置き換えまで動くことを確認
- 短い校正で約 6〜9 秒、7 行の会議メモの整理で約 12 秒
- キャンセル（`TerminateJobObject`）から `Run()` が戻るまで約 30ms

## プロンプト調整の記録

- 「誤字脱字を直して本文だけ出力」だけだと、誤字の無いメモはそのまま返ってくる（正しい挙動だが、ユーザーには失敗に見える）
- 「MECE に分類」＋「決定事項・TODO・未決事項を独立した節に」と指示すると、トピック別の節と同じ内容を重複して書く。
  → 分類の軸はトピックの1つにし、決定事項などは項目ごとに【決定】【TODO】【未決】の印を付ける形にしたら重複が消えた

## 代替: Editor_OutputProcess

plugin.h には、EmEditor 自身に外部プロセスを実行させてアウトプット バーに出す `Editor_OutputProcess`（`FLAG_STDIN` など）もある。
結果を表示するだけならこれで足りるが、結果を受け取って加工・置き換えしたい場合は、自前で実行する必要がある。
