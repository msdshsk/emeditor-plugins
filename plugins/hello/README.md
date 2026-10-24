# hello

Hello World プラグイン。実行すると、アウトプット バーに EmEditor のバージョン値と現在のファイル名を出力する。

`scripts/new-plugin.ps1` が複製する**雛形**でもあるので、ここを変えると今後作るプラグインの初期状態も変わる。

## 使い方

プラグイン メニュー > Hello World（またはプラグイン ツールバーの「H」ボタン）

## 使っている API

- `Editor_Info( hwndView, EI_GET_FILE_NAMEW, ... )` — 現在のファイル名
- `Editor_GetVersion( hwndView )` — EmEditor のバージョン
- `Editor_OutputString( hwndView, ..., FLAG_OPEN_OUTPUT )` — アウトプット バーに出力（閉じていれば開く）
