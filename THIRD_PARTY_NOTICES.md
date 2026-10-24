# Third-Party Notices

このリポジトリのプラグイン（DLL）には、以下のソフトウェアが含まれます（ソースの同梱、リソースへの埋め込み、または静的リンク）。
配布物（Release の zip）にはこのファイルを同梱します。各ライセンスの全文は、ビルド時に取得されるファイル、または各プロジェクトのリポジトリを参照してください。

This project includes the following third-party software. Full license texts are in `sdk/`, `third_party/`, or the upstream repositories.

| ソフトウェア | 版 | ライセンス | 著作権表示 | 使っているプラグイン | 入手先 |
|---|---|---|---|---|---|
| EmEditor Plug-in Library（`plugin.h` / `etlframe.h` / `exports.def`） | 2026-10-21 (053b537) | MIT | Copyright (c) 2018 Emurasoft | すべて | https://github.com/Emurasoft/emeditor-plugin-library |
| nlohmann/json | 3.11.3 | MIT | Copyright (c) 2013-2022 Niels Lohmann | claude_chat | https://github.com/nlohmann/json |
| marked | 18.1.1 | MIT | Copyright (c) 2018+, MarkedJS / Copyright (c) 2011-2018, Christopher Jeffrey / Copyright (c) 2004, John Gruber（Markdown.pl 由来部分） | claude_chat, markdown_preview | https://github.com/markedjs/marked |
| marked-footnote | 1.4.0 | MIT | Copyright (c) Beni Arisandi | markdown_preview | https://github.com/bent10/marked-extensions |
| highlight.js | 11.12.0 | BSD 3-Clause | Copyright (c) 2006, Ivan Sagalaev | markdown_preview | https://github.com/highlightjs/highlight.js |
| KaTeX（KaTeX フォントを含む） | 0.19.1 | MIT | Copyright (c) 2013-2020 Khan Academy and other contributors | markdown_preview | https://github.com/KaTeX/KaTeX |
| Mermaid | 12.2.0 | MIT | Copyright (c) 2014 - 2022 Knut Sveidqvist | markdown_preview | https://github.com/mermaid-js/mermaid |
| Microsoft Edge WebView2 SDK（`WebView2LoaderStatic.lib`） | 1.0.4314.35 | BSD 3-Clause | Copyright (C) Microsoft Corporation. All rights reserved. | claude_chat, markdown_preview | https://www.nuget.org/packages/Microsoft.Web.WebView2 |

## 商標 / Trademarks

- EmEditor は Emurasoft, Inc. の商標です。本プロジェクトは Emurasoft, Inc. とは関係のない、非公式のプラグイン集です。
- Claude および Claude Code は Anthropic, PBC の商標です。本プロジェクトは Anthropic とは関係ありません。
- Microsoft Edge および WebView2 は Microsoft Corporation の商標です。

EmEditor is a trademark of Emurasoft, Inc. Claude is a trademark of Anthropic, PBC. This project is not affiliated with or endorsed by either company.
