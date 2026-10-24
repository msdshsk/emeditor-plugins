# sdk/ — EmEditor 公式プラグインヘッダ（ベンダリング）

このフォルダのファイルは **編集しない** こと。更新は公式リポジトリからの上書きコピーのみで行う。

| ファイル | 内容 |
|---|---|
| `plugin.h` | EmEditor API（`Editor_*` 関数、`EVENT_*`、`EEID_*` 定数など） |
| `etlframe.h` | ETL (EmEditor Template Library)。DLL エクスポート関数と `DllMain` を実装する |
| `exports.def` | エクスポート関数定義（全プラグイン共通で使う） |
| `LICENSE` | MIT License (Emurasoft) |

## 取得元

- リポジトリ: https://github.com/Emurasoft/emeditor-plugin-library
- コミット: `053b537c0f02973b3da737f90671bcdf09a6fbb6`（2026-10-21）
- 取得日: 2026-10-24

## 更新手順

```powershell
git clone --depth 1 https://github.com/Emurasoft/emeditor-plugin-library $env:TEMP\eepl
Copy-Item $env:TEMP\eepl\plugin.h, $env:TEMP\eepl\etlframe.h, $env:TEMP\eepl\exports.def, $env:TEMP\eepl\LICENSE sdk\
git -C $env:TEMP\eepl log -1 --format="%H %cs"   # ↑の「コミット」欄を更新
.\scripts\build.ps1                               # 全プラグインがビルドできることを確認
```

`etlframe.h` が要求するメンバ（`enum` 定数やメソッド）は版によって増えることがある。
更新後にビルドエラーが出たら [docs/troubleshooting.md](../docs/troubleshooting.md) を参照。
