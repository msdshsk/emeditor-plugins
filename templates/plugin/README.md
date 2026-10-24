# templates/plugin — 新しいプラグインの雛形

`scripts/new-plugin.ps1` がこのフォルダを複製して、新しいプラグインを作ります。
中身は最小の Hello World（実行するとアウトプット バーに `Hello World!!` と EmEditor のバージョン・ファイル名を出す）です。

```powershell
.\scripts\new-plugin.ps1 -Name myplugin -MenuText "My Plugin"
```

- ファイル名（`hello.*`）、`ProjectGuid`、メニュー名、DLL 名が差し替えられ、`plugins\myplugin\` ができます
- etlframe.h が要求する定数・メソッドはすべて定義済みです（一覧と意味は [docs/anatomy.md](../../docs/anatomy.md)）
- 雛形が壊れていないよう、`.\scripts\build.ps1`（全体ビルド）はこの雛形もビルドします。プラグイン一覧（`EmEditorPlugins.sln`）には入りません
