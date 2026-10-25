# リリースの手順

プラグインごとに、タグを push すると GitHub Actions（[.github/workflows/release.yml](../.github/workflows/release.yml)）が
ビルド・zip 作成・SHA-256 計算・GitHub Release の作成まで行う。

## 手順

1. `plugins/<plugin>/version.h` の版を上げる（`PLUGIN_VER_MAJOR/MINOR/PATCH` と `PLUGIN_VER_STR`）
2. コミットして push
3. タグを付けて push。**タグのメッセージの 2 行目以降が、Release 本文の「変更点」になる**（1 行目はタイトル、2 行目は空行）

   ```powershell
   git tag -a claude-chat-v1.1.0 -F tag-message.txt   # 1 行目: タイトル / 空行 / 変更点の箇条書き
   git push origin claude-chat-v1.1.0
   ```

4. Actions の「Release」が終わると、Release ページに zip と `*.sha256.txt` が付く（1 分前後）

タグ名は `<product>-v<major>.<minor>.<patch>`。`<product>` はプラグインのフォルダ名の `_` を `-` にしたもの（`claude_chat` → `claude-chat`）。
**タグの版と version.h の版が違うと、ビルド前に失敗する**（DLL のバージョン情報とタグがずれるのを防ぐ）。

新しいプラグインをリリース対象にするときは、release.yml の `on.push.tags` と、タイトルの対応表（`$titles`）に追加する。

## 配布物

```
<product>-<version>.zip
  x64/<plugin>.dll         … 64-bit 版 EmEditor 用
  x86/<plugin>.dll         … 32-bit 版 EmEditor 用
  README.md / LICENSE / THIRD_PARTY_NOTICES.md
<product>-<version>.sha256.txt   … zip と各 DLL の SHA-256（Release の本文にも載せる）
```

- ローカルで同じものを作る: `.\scripts\package.ps1 -Plugin claude_chat -Version 1.0.0` → `dist/`
- ランナーは `windows-2025`（Visual Studio 2022 の v143 ツールセット）。ランナーのイメージが変わってビルドできなくなったら、ここを見直す
- コード署名はしていない。代わりに SHA-256 と「GitHub Actions がこのリポジトリのソースからビルドした」旨を Release 本文に書いている

## 失敗したタグを出し直す

```powershell
gh release delete claude-chat-v1.1.0 --yes          # Release があれば
git push origin :refs/tags/claude-chat-v1.1.0       # リモートのタグを消す
git tag -d claude-chat-v1.1.0                       # ローカルのタグを消す
# 直してからタグを付け直す
```

## 実績

| 日付 | タグ | 備考 |
|---|---|---|
| 2026-10-25 | claude-chat-v1.0.0 | 初回。zip 507KB |
| 2026-10-25 | markdown-preview-v1.0.0 | 初回。zip 4.4MB（DLL 6.5MB、うち Mermaid 5MB） |
| 2026-10-25 | claude-chat-v1.1.0 | 巨大ファイル対策、EmEditor の検索・抽出・置換・フィルターをツール化 |
| 2026-10-25 | markdown-preview-v1.1.0 | 脚注の見出しを日本語に、大きすぎる文書をプレビューしない |
