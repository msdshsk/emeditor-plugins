# WebView2 でプラグインの表示を作る

リッチエディットや自前描画では表現力が足りない場面（チャット履歴、Markdown プレビュー）で、
Edge の描画エンジン（WebView2）をプラグインのウィンドウに埋め込む方法。

- 部品: [shared/webview_host.h](../shared/webview_host.h)
- Markdown の共通スタイル: [shared/web/markdown.css](../shared/web/markdown.css)（GFM 対応、明暗テーマ両対応）
- Markdown の変換: [third_party/marked](../third_party/marked/)（marked 18.1.1, MIT）
- 使用例: [plugins/claude_chat](../plugins/claude_chat/)（`history.h`, `web/chat.html`）

## 依存物

| もの | 入手 | 備考 |
|---|---|---|
| WebView2 Runtime | Windows 11 は標準で入っている（この PC: 155.0） | 無い環境ではリッチエディット表示に切り替える（claude_chat） |
| WebView2 SDK（`WebView2.h`, `WebView2LoaderStatic.lib`） | `scripts/fetch-deps.ps1` が NuGet から版・ハッシュ固定で取得（`build.ps1` から自動） | 静的ライブラリが 1 つ 11MB あるので git に入れない。BSD 3 条項ライセンス |
| marked | `third_party/marked/marked.umd.js`（git に同梱、48KB） | npm の integrity（sha512）と一致を確認して取得 |
| highlight.js / KaTeX / Mermaid / marked-footnote | `scripts/fetch-deps.ps1` が npm から版・integrity 固定で取得 | Markdown プレビューで使う。KaTeX のフォントは CSS に埋め込む |

- `WebView2LoaderStatic.lib` を静的リンクすると、`WebView2Loader.dll` を配る必要がない。DLL の依存は Windows 標準の DLL だけになる
- 静的ライブラリは `advapi32.lib`（レジストリ・ETW）と `version.lib` を要求する。プラグインは既定でリンク済みだが、
  テスト用 exe などでは足りずに `LNK2019 __imp_EventRegister / RegOpenKeyExW` になった → `webview_host.h` で `#pragma comment` 済み
- `#include <wrl.h>` の `Microsoft::WRL::Callback` で COM のコールバックを作る（追加ライブラリ不要）

## 作り方（webview_host.h）

```cpp
ee::web::Host host;
host.Create( hwndParent, callbacks );                // 非同期。Runtime が無ければ onError
host.AddScriptOnDocumentCreated( markedJs );         // ライブラリはここで先に読み込む（下記）
host.NavigateToString( html );                       // HTML を表示（2MB まで）
host.SetBounds( rect );                              // 親ウィンドウ内の位置
host.PostJson( json );                               // ページの 'message' イベントへ
// ページ → window.chrome.webview.postMessage( obj ) → callbacks.onMessage( JSON 文字列 )
host.Close();
```

- ユーザーデータフォルダは `%LOCALAPPDATA%\EmEditorPlugins\WebView2`（プラグイン共通）
- UI スレッドで使う（EmEditor の UI スレッドは COM 初期化済み）

## ハマったところ

### ライブラリを `<script>` に直接埋め込むと、ソースが文字として表示される

marked のソース（正規表現の中）に `<!--` と `<script` が含まれていて、HTML パーサーが `<script>` の中で特殊なモード
（script data double escaped state）に入り、スクリプトの終わりがずれた。`</script` だけを気にしても防げない。
→ **`AddScriptToExecuteOnDocumentCreated` でページより先に実行させる**。HTML の解析を通らないので、中身に何が書いてあっても壊れない。
登録は非同期なので、すべて完了してから最初の `NavigateToString` をする。（2026-10-25 確認）

### ページの「準備完了」より NavigationCompleted の方が後に来る

ページのスクリプトが最後に `postMessage({type:'ready'})` すると、それが `NavigationCompleted` より**先に**届く。
`PostWebMessageAsJson` を NavigationCompleted まで送らないようにしていたら、「準備完了」を受けて送った溜まりのメッセージが全部捨てられて、画面が真っ白になった。
→ 送ってよいかの判断は、ページからの「準備完了」メッセージだけで行う。（2026-10-25 確認）

### 大きいライブラリは必要になってから読み込む

Mermaid（5MB）は、ページから「必要」と言われたときだけ `ExecuteScript` で注入する（Markdown プレビュー）。
`ExecuteScript` で入れたスクリプトもページの CSP の対象外。Mermaid は `securityLevel: 'strict'` で、CSP（`unsafe-eval` なし）の下でも描画できた。

### lexer と parser を別々に呼ぶなら、拡張の walkTokens も自分で呼ぶ

`marked.parse` は「lexer → 拡張の walkTokens → parser」の順に処理する。行番号の目印を差し込むために `marked.lexer` と
`marked.parser` を別々に呼ぶと walkTokens が抜け、marked-footnote の脚注一覧が先頭に出たうえ、状態が戻らず 2 回目以降の描画で脚注が壊れた。
→ `marked.walkTokens( tokens, marked.defaults.walkTokens )` を間に呼ぶ。（2026-10-25 確認）

### 別プロセスと同じユーザーデータフォルダを使うと、起動が進まない

EmEditor（プラグインの WebView2 が動いている）を起動したまま、テスト用 exe が同じユーザーデータフォルダで WebView2 を作ると、
起動に十数秒かかったり、いつまでもページが準備完了にならなかったりした。テストでは `Host::Create` の第 3 引数で別のフォルダを使う（1.5 秒で起動）。

### 真っ白で原因が分からない

ページ側で `window.addEventListener('error', …)` を仕込み、エラーを画面に出す。
それでも何も出ないときは `ExecuteScript` でページの状態（`location.href`, `typeof marked`, 要素数など）を取り出して確かめる。

## 安全策

| 対策 | 内容 |
|---|---|
| CSP | `default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-<毎回ランダム>'; img-src data:;` 外部読み込みなし。応答に紛れた `<script>` や `onclick` は動かない（`AddScriptToExecuteOnDocumentCreated` で入れたスクリプトは CSP の対象外） |
| 生 HTML | marked の `renderer.html` を上書きしてエスケープ。`<script>alert(1)</script>` は文字として表示されることを確認 |
| 遷移 | 最初の `NavigateToString` 以外の遷移は取り消す。リンクはページ側でクリックを横取りしてネイティブに渡し、http(s) だけ既定のブラウザで開く。新しいウィンドウ要求も同様 |
| 設定 | 開発者ツール（Release のみ）・ホストオブジェクト・既定のスクリプトダイアログ・ステータスバーを無効 |

## キー入力とフォーカス

- WebView2 の中にフォーカスがあるときのキー入力は、別プロセスで処理されるので EmEditor の `PreTranslateMessage` には来ない
- Esc などで EmEditor 側に戻したいキーは、コントローラーの `AcceleratorKeyPressed` で拾う
- 入力欄はネイティブの EDIT のまま残し、WebView2 は表示専用にした（フォーカスの取り合いを避けるため）

## テストのしかた

EmEditor に入れる前に、テスト用の exe（`CreateWindow` の上に `ee::web::Host` を載せるだけ）で表示を確かめ、
`Graphics.CopyFromScreen` で実際の画面を撮る（`PrintWindow` は WebView2 の中身を撮れないことがある）。
