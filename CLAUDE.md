# CLAUDE.md

EmEditor プラグイン（C++ DLL）のモノレポ。全体像は README.md、仕組みは docs/anatomy.md。

## ルール

- `sdk/` は公式ファイルのコピー、`third_party/` は外部ライブラリのコピー。どちらも**編集しない**。更新手順は各 README.md。
- `claude -p` を呼ぶときは必ず `--tools ""` と `--strict-mcp-config` を付ける（ユーザー設定の MCP を読ませない。コストと挙動のため）。
  EmEditor を操作させたいときは、自前の MCP サーバー（shared/mcp_server.h + editor_tools.h）だけを `--mcp-config` で渡す。
- 新しいプラグインは `scripts/new-plugin.ps1` で作る（vcxproj を手書きしない）。.sln は `scripts/update-sln.ps1` で再生成し、手で編集しない。
- ビルド設定は `build/Plugin.props` に集約。プラグイン個別の設定が必要なときだけ vcxproj で上書きする。
- .rc は UTF-8（BOM なし）+ `#pragma code_page(65001)`。
- ビルド確認は `.\scripts\build.ps1`（x64）と `.\scripts\build.ps1 -Platform Win32` の両方。
- deploy は EmEditor が起動中だと失敗する。勝手に EmEditor を終了させない（ユーザーに頼む）。

## ノウハウの記録

このリポジトリの目的の半分は、情報の少ない EmEditor プラグイン開発のノウハウを残すこと。

- ビルドエラー・実行時の不具合を解決したら、`docs/troubleshooting.md` に「症状（メッセージそのまま）→ 原因 → 対処 → 確認日/環境」で追記する。
- 実際に再現・確認したことと、ソースを読んだだけの推測は区別して書く（「確認:」/「出典:」）。
- API の使い方で分かったことは `docs/anatomy.md` か、テーマが大きければ `docs/` に新しいファイルを作る。
