# third_party/ — 外部ライブラリ（ベンダリング）

編集しない。更新は上書きコピーで行う。

| ライブラリ | 版 | ライセンス | 取得元 | 用途 |
|---|---|---|---|---|
| nlohmann/json (`nlohmann/json.hpp`) | v3.11.3 | MIT (`nlohmann/LICENSE.MIT`) | https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp | MCP（JSON-RPC）の読み書き |

| marked (`marked/marked.umd.js`) | v18.1.1 | MIT (`marked/LICENSE.md`) | https://registry.npmjs.org/marked/-/marked-18.1.1.tgz（npm の integrity sha512 と一致を確認） | Markdown → HTML（GFM）。WebView2 の表示で使う |
| WebView2 SDK (`webview2/`) | 1.0.4314.35 | BSD 3 条項 (`webview2/LICENSE.txt`) | NuGet `Microsoft.Web.WebView2` | **git に入れない**（静的ライブラリが 11MB×2）。`scripts/fetch-deps.ps1` が SHA-256 を照合して取得する |
| highlight.js (`highlight/`) | 11.12.0 | BSD 3 条項 | npm `@highlightjs/cdn-assets` | **git に入れない**。fetch-deps.ps1 が npm の integrity を照合して取得 |
| KaTeX (`katex/`) | 0.19.1 | MIT | npm `katex` | **git に入れない**。取得時にフォント（woff2）を data: URI で埋め込んだ `katex.inline.css` を生成 |
| Mermaid (`mermaid/`) | 12.2.0 | MIT | npm `mermaid`（`dist/mermaid.min.js` だけ取り出す） | **git に入れない**（5MB）|
| marked-footnote (`marked-footnote/`) | 1.4.0 | MIT（`package.json`） | npm `marked-footnote` | **git に入れない** |

- json.hpp SHA-256: `9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6`（公式リリースの値と一致）
- 取得日: 2026-10-24
- include は `#include <nlohmann/json.hpp>`（`build/Plugin.props` で `third_party/` を include パスに追加済み）
- プラグインの .cpp では、`#define new _RELEASE_NEW` より**前**に include すること（ライブラリ内の `new` が置き換わらないように）
