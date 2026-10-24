# はじめに: ビルドから動作確認まで

## 1. 必要なもの

| もの | 備考 |
|---|---|
| Visual Studio 2022（Community / Build Tools 可） | 「C++ によるデスクトップ開発」ワークロード。ツールセット v143 |
| Windows SDK 10 | VS と一緒に入る |
| EmEditor | **ビット数を確認しておく**（64-bit 版なら x64 の DLL が必要） |

公式マニュアル（[プラグインの作り方](https://jp.emeditor.com/text-editor-features/extensibility/plug-ins/how-to-make-emeditor-plug-ins/)）は
Visual C++ 2012 Express 時代の内容。考え方はそのまま使えるが、手順・サンプルは古い（[anatomy.md](anatomy.md) 参照）。

## 2. ビルド

```powershell
.\scripts\build.ps1                                     # 全プラグイン Release|x64
.\scripts\build.ps1 -Plugin hello -Configuration Debug  # 1 個だけ Debug
.\scripts\build.ps1 -Platform Win32                     # 32-bit 版 EmEditor 用
```

成果物は `out\<Platform>\<Configuration>\<name>.dll`。
VS で作業する場合は `EmEditorPlugins.sln` を開く（ソリューションの `x86` 構成 = プロジェクトの `Win32`）。

## 3. 配置と登録

```powershell
.\scripts\deploy.ps1 -Plugin hello
```

`out\<Platform>\<Configuration>\` の DLL を、リポジトリ内の固定フォルダ **`deploy\<Platform>\`** にコピーする。

**EmEditor は PlugIns フォルダに DLL を置いただけでは認識しない。**「プラグイン > プラグインの設定 > 追加」で
DLL を選んで登録する必要がある（2026-10-24 / EmEditor 23.0.5 で確認）。
登録は任意の場所の DLL を指定できるので、EmEditor の PlugIns には置かず、`deploy\<Platform>\` を登録して使う。

- 登録は**プラグインごとに初回 1 回だけ**。以降は `deploy.ps1` で上書きすれば反映される。
- `out\` を直接登録しないのは、Debug/Release でパスが変わるのを避けるためと、EmEditor の起動中でもビルド（リンク）を通すため。
- **登録済みの DLL は EmEditor の起動中ロックされ、上書きできない。** タスクトレイ常駐も含めて終了してから配置する
  （スクリプトは上書きに失敗したら中断する）。未登録の初回配置は EmEditor を開いたままでよい。
- `-Configuration Debug` のときは `.pdb` も一緒にコピーする（デバッガでソースを追えるように）。

## 4. 動作確認

1. EmEditor を起動する。
2. プラグイン ツールバーに追加されたボタン、または「プラグイン」メニューから実行する。
3. hello の場合、アウトプット バーに `Hello World!! (EmEditor 23005) file=無題-1` のように出れば成功。
   （`Editor_GetVersion` は 23.0.5 → `23005` の形式で返る）

表示されない場合は [troubleshooting.md](troubleshooting.md) の「EmEditor に認識されない」を参照。

## 5. デバッグ

1. Debug 構成でビルドし、`deploy.ps1 -Configuration Debug` で配置する。
2. EmEditor を起動し、VS の「デバッグ > プロセスにアタッチ」で `EmEditor.exe` を選ぶ。
3. ブレークポイントを置いて、プラグインのコマンドを実行する。

Debug ビルドでは `_ASSERTE` が有効。etlframe.h 内のアサートもダイアログで止まるので、使い方の誤りに気づきやすい。

## 6. 新しいプラグインを作る

```powershell
.\scripts\new-plugin.ps1 -Name wordstat -MenuText "Word Stat"
```

`plugins/hello` を複製し、ファイル名・ProjectGuid・メニュー名を差し替えて `.sln` を再生成する。
その後:

1. `<name>.cpp` の `OnCommand` などを実装する
2. `<name>.rc` の `IDS_STATUS_MESSAGE`（ステータスバーの説明）を書く
3. `toolbar.bmp`（16x16・16 色・背景 RGB(192,192,192)）を差し替える
4. `README.md` を書き、ルートの README のプラグイン一覧に追記する
