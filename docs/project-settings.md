# ビルド設定とその理由

共通設定は `build/Plugin.props` にまとめてあり、各 vcxproj はそれを Import するだけ。
プラグインごとに設定を変えたいときは、vcxproj の Import の**後ろ**に `ItemDefinitionGroup` を書いて上書きする。

| 設定 | 値 | 理由 |
|---|---|---|
| 構成の種類 | DLL | プラグインは DLL |
| 文字セット | Unicode | etlframe.h は W 系 API 前提 |
| C++ 言語標準 | C++17 (`/std:c++17`) | 最新の etlframe.h が `if constexpr` と `[[nodiscard]]` を使う（C++14 だと警告 C4984/C5051） |
| モジュール定義ファイル | `sdk/exports.def` | 7 関数を装飾なしの名前で公開する（下記） |
| 呼び出し規約 | `__stdcall` (`/Gz`) | 公式マニュアルの指定。x86 のみ意味があり、x64 では無視される |
| ランタイムライブラリ | Release `/MT`、Debug `/MTd` | CRT を静的リンクし、VC++ 再頒布パッケージに依存しない DLL にする。マニュアルの「マルチスレッド DLL ではなくマルチスレッド」 |
| 最適化 | サイズ優先 (`/O1`) | マニュアルの「サイズの最小化」 |
| `/utf-8` | 有効 | ソースの日本語リテラルを UTF-8 として扱う |
| プラットフォーム | x64（既定）と Win32 | EmEditor 本体のビット数に合わせる。ARM64 版 EmEditor 向けには ARM64 ビルドツールの追加が必要（未対応） |
| 出力先 | `out\<Platform>\<Configuration>\` | 全プラグインの DLL を 1 か所に集め、deploy しやすくする |

## なぜ .def が要るのか

- **x86**: `extern "C" __stdcall` の関数は `_OnCommand@4` のように装飾された名前になる。
  .def で `OnCommand` という名前で公開し直さないと EmEditor が見つけられない。
  （dumpbin で確認: `OnCommand = _OnCommand@4`）
- **x64**: etlframe.h の関数には `__declspec(dllexport)` が付いていないので、.def が無いと**何もエクスポートされない**。
  ビルドはエラーなく通ってしまうので気づきにくい。

確認コマンド（VS の開発者 PowerShell）:

```powershell
dumpbin /exports out\x64\Release\markdown_preview.dll     # 7 関数が並ぶこと
dumpbin /dependents out\x64\Release\markdown_preview.dll  # Windows 標準の DLL だけで、VCRUNTIME*.dll が無いこと
```

## リソース (.rc) の文字コード

- .rc は **UTF-8（BOM なし）+ 先頭に `#pragma code_page(65001)`** で統一している。git の差分が読めるようにするため。
- `#pragma code_page(65001)` を消すと、日本語を含む行で `error RC2104` になる。
- VS のリソースエディタで .rc を開いて保存すると UTF-16 に変換されることがある。テキストエディタで編集する。

## ビットマップ

- `_IDB_BITMAP`: 16x16 ピクセル・16 色（4bpp）の BMP。透過色は `RGB(192,192,192)`。
- フルカラーや 24x24 を用意する場合は `_IDB_TRUE_*` / `_IDB_16C_24` にリソース ID を設定する（0 = 無し）。
