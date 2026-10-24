# Markdown プレビュー for EmEditor

EmEditor のサイドパネルに、編集中の Markdown を **GitHub に近い見た目**でリアルタイム表示するプラグインです。
EmEditor 標準のプレビューとは別に、表現力（GFM）とエディタとの連動を重視しています。

## 特長

- **GFM（GitHub Flavored Markdown）対応**: 表（左・中央・右寄せ）、タスクリスト、取り消し線、URL の自動リンク
- **GitHub のアラート記法**: `> [!NOTE]` / `[!TIP]` / `[!IMPORTANT]` / `[!WARNING]` / `[!CAUTION]`
- **コードの色分け**: 言語を指定したコードブロック（```` ```cpp ```` など）を highlight.js で色分け（GitHub 風、明暗対応）
- **数式**: KaTeX。行内 `$E=mc^2$` / `` $`\sqrt{2}`$ ``、独立 `$$ … $$` / ```` ```math ````（GitHub と同じ書き方）。`$5` のような金額は数式にしない
- **図**: Mermaid（```` ```mermaid ````）。フローチャート、シーケンス図、ガントチャートなど。図が出てきたときだけ読み込み、同じ図は描き直さない
- **脚注**: `本文[^1]` と `[^1]: 脚注の内容`。文書の末尾に一覧が出る
- **生の HTML も表示**: `<details>`、`<img>`、`<br>` など（スクリプトは一切動きません。下記「安全性」）
- **YAML front matter** は折りたたんで表示
- **相対パスの画像**: `![](images/a.png)` を文書のフォルダ基準で表示（保存済みの文書のみ）
- **エディタと連動**
  - 編集すると 0.3 秒後に描き直す（スクロール位置は保つ）
  - エディタをスクロールすると、プレビューも同じ場所へ
  - カーソルのある段落を薄く強調
  - プレビューを**ダブルクリックすると、エディタのその行へ移動**
- **リンク**: 見出しへのリンク（`#見出し`）はプレビュー内で移動、相対パスのファイルは EmEditor で開く、http(s) は既定のブラウザで開く
- 明暗テーマは Windows の設定に追従
- タブを切り替えると、その文書のプレビューに切り替わる

## 必要なもの

- EmEditor（64-bit 版なら x64 の DLL、32-bit 版なら x86 の DLL）。EmEditor 23.0.5 で確認
- WebView2 Runtime（Windows 11 には標準で入っています）

## インストール

1. DLL を任意のフォルダに置く
2. EmEditor の「プラグイン > プラグインの設定 > 追加」で DLL を選ぶ
3. 「プラグイン > Markdown プレビュー」でパネルを開閉

## 使い方

| 操作 | 動き |
|---|---|
| プラグイン > Markdown プレビュー | パネルの開閉（開いたまま EmEditor を閉じると、次回も開く） |
| 編集 | 0.3 秒後に自動で描き直す |
| エディタのスクロール | プレビューが追従 |
| プレビューをダブルクリック | エディタのその行へ |
| プレビュー内で Esc | エディタに戻る |
| Markdown 以外の文書 | 「Markdown として表示」ボタンで、その文書だけ Markdown として描画 |

Markdown 文書かどうかは、拡張子（`.md` `.markdown` `.mdown` `.mkd` `.mkdn` `.mdx` `.mdwn`）か、EmEditor の設定（構成）名に「Markdown」を含むかで判断します。

## 安全性

プレビューは WebView2（Edge）の中で表示します。

- **スクリプトは動きません**: Content-Security-Policy で、プラグイン自身のスクリプト（ランダムな nonce 付き）以外は実行されません。
  文書中の `<script>`、`onclick` などのイベント属性、`javascript:` リンク、`<iframe>`、フォーム送信はすべて無効です
- **ページ内の移動はしない**: リンクをクリックしても WebView2 の中では開かず、http(s) は既定のブラウザ、相対パスのファイルは EmEditor で開きます
- **見えるローカルファイルは文書のフォルダだけ**: 相対パスの画像のために、文書のフォルダを `https://emeditor-doc.local/` として割り当てます
- 外部の画像（`https://…`）は表示します（自分の文書のプレビューのため）

## 仕組み

```
EmEditor ──(文書の全文・表示行)──> プラグイン ──PostWebMessage──> WebView2（preview.html）
                                                             └ marked（GFM）で HTML にして表示
        <──(ダブルクリックした行 / 開くファイル)── プラグイン <──postMessage──┘
```

- Markdown の変換: [marked](https://github.com/markedjs/marked) 18.1.1（MIT、`third_party/marked`）
- 脚注: [marked-footnote](https://github.com/bent10/marked-extensions) 1.4.0（MIT）
- コードの色分け: [highlight.js](https://highlightjs.org/) 11.12.0（BSD 3 条項、主要言語入りのビルド）
- 数式: [KaTeX](https://katex.org/) 0.19.1（MIT）。フォントは CSS に data: URI で埋め込む（DLL 1 つで配るため）
- 図: [Mermaid](https://mermaid.js.org/) 12.2.0（MIT、5MB）。図があるときだけ `ExecuteScript` で読み込む
- ライブラリはすべて DLL のリソースに埋め込み済み。実行時にネットワークから何かを読み込むことはない
- スタイル: [shared/web/markdown.css](../../shared/web/markdown.css)（Claude チャットと共用）
- スクロール連動: Markdown を最上位のブロックごとに描画し、各ブロックの直前に元の行番号の目印を置く。
  エディタの表示の一番上の論理行（`Editor_GetScrollPos` → `Editor_ViewToLogical`）に対応する位置へスクロールする
- WebView2 の扱いは [docs/webview2.md](../../docs/webview2.md)、パネルの作り方は [docs/custom-bar.md](../../docs/custom-bar.md)

## ソースからのビルド

```powershell
git clone <このリポジトリ>
cd emeditor-plugins
.\scripts\build.ps1 -Plugin markdown_preview                  # x64（WebView2 SDK は自動で取得）
.\scripts\build.ps1 -Plugin markdown_preview -Platform Win32  # x86
# → out\<Platform>\Release\markdown_preview.dll
```

Visual Studio 2022（C++ によるデスクトップ開発）が必要です。
highlight.js / KaTeX / Mermaid / marked-footnote / WebView2 SDK は `scripts/fetch-deps.ps1` が版とハッシュを固定して取得します（`build.ps1` から自動で呼ばれます）。
DLL は約 6.5MB（うち Mermaid が 5MB）です。

## 今後の候補

- 目次、HTML として保存・印刷
- パネルの位置（下側）
- 言語指定のないコードの自動色分け（自動判定は遅く外れやすいので、今は色分けしない）

## ライセンス

[MIT License](../../LICENSE)。同梱物（marked、marked-footnote、highlight.js、KaTeX、Mermaid、WebView2 SDK、EmEditor プラグイン SDK）のライセンスは [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md) を参照してください。

EmEditor は Emurasoft, Inc. の商標です。本プラグインは Emurasoft, Inc. とは関係のない非公式のものです。
