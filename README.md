# emeditor-plugins

EmEditor 用の自作プラグイン（C++ / DLL）をまとめたモノレポ。
公式マニュアルは情報が古い部分があるため、作り方やハマりどころを [docs/](docs/) に蓄積していく。

## プラグイン一覧

| フォルダ | 概要 |
|---|---|
| [plugins/hello](plugins/hello/) | Hello World。アウトプット バーに文字列を出す。**新規プラグインの雛形** |
| [plugins/memo_tidy](plugins/memo_tidy/) | 走り書きのメモを `claude -p` で MECE に整理し、選択範囲を置き換える |
| [plugins/claude_chat](plugins/claude_chat/) | サイドパネルで Claude と会話する。Claude は MCP ツールで文書を読み書きできる |
| [plugins/markdown_preview](plugins/markdown_preview/) | 編集中の Markdown を GFM でリアルタイムプレビュー。スクロール連動・ダブルクリックでジャンプ |

## 構成

```
emeditor-plugins/
├─ EmEditorPlugins.sln     … VS 用ソリューション（scripts/update-sln.ps1 で自動生成）
├─ build/Plugin.props      … 全プラグイン共通のビルド設定
├─ sdk/                    … 公式 plugin.h / etlframe.h / exports.def（編集禁止・ベンダリング）
├─ shared/                 … 自作の共通ヘッダ（プロセス実行・MCP サーバー・EmEditor 操作ツールなど）
├─ third_party/            … 外部ライブラリ（nlohmann/json, marked。WebView2 SDK は fetch-deps.ps1 で取得）
├─ plugins/<name>/         … プラグイン 1 個 = 1 フォルダ = 1 DLL
├─ scripts/                … build / deploy / new-plugin / update-sln
├─ docs/                   … ノウハウ集
├─ out/<Platform>/<Config>/ … ビルド成果物（git 管理外）
├─ deploy/<Platform>/      … EmEditor に登録する DLL の置き場（git 管理外）
└─ obj/                    … 中間ファイル（git 管理外）
```

## クイックスタート

前提: Visual Studio 2022（「C++ によるデスクトップ開発」ワークロード）

```powershell
.\scripts\build.ps1                                   # 全プラグインを Release|x64 でビルド
.\scripts\deploy.ps1 -Plugin hello                    # deploy\x64\ へコピー（EmEditor を終了してから）
# 初回のみ: EmEditor の「プラグイン > プラグインの設定 > 追加」で deploy\x64\hello.dll を登録
.\scripts\new-plugin.ps1 -Name myplugin -MenuText "My Plugin"   # 新しいプラグインを作る
```

詳しくは [docs/getting-started.md](docs/getting-started.md)。

## ドキュメント

- [getting-started.md](docs/getting-started.md) — 環境構築からビルド・配置・動作確認まで
- [anatomy.md](docs/anatomy.md) — プラグインの仕組み（エクスポート関数・etlframe.h・フレームの寿命）
- [project-settings.md](docs/project-settings.md) — ビルド設定とその理由
- [external-process.md](docs/external-process.md) — 外部プロセス（claude -p など）を非同期で実行する
- [custom-bar.md](docs/custom-bar.md) — カスタムバー（サイドパネル）の作り方
- [mcp-in-plugin.md](docs/mcp-in-plugin.md) — プラグイン内に MCP サーバーを立て、claude -p に EmEditor を操作させる
- [webview2.md](docs/webview2.md) — WebView2 で表示を作る（チャット履歴・Markdown プレビュー）
- [troubleshooting.md](docs/troubleshooting.md) — 実際に出たエラーと対処

## ライセンス

[MIT License](LICENSE)（Copyright (c) 2026 msd.shsk）

同梱・静的リンクしているソフトウェア（EmEditor プラグイン SDK、marked、WebView2 SDK など）のライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) を参照してください。

EmEditor は Emurasoft, Inc. の商標です。本プロジェクトは Emurasoft, Inc. とは関係のない非公式のプラグイン集です。
Claude は Anthropic, PBC の商標です。
