# トラブルシューティング

実際に遭遇（または再現確認）したエラーと対処を記録する。新しく踏んだものはここに追記する。
書式: **症状（エラーメッセージそのまま）→ 原因 → 対処**。確認日と環境も書く。

---

## ビルド時

### `error C2039: '_USE_CUSTOM_BAR': 'CMyFrame' のメンバーではありません`
### `error C2039: 'UserMessage': 'CMyFrame' のメンバーではありません`

- 原因: フレームクラスに、etlframe.h が要求する enum 定数/メソッドが足りない。
  公式サンプル `plugin_sample_hello.zip` をそのまま最新の etlframe.h と組み合わせると必ずこれになる。
- 対処: `plugins/hello/hello.cpp` を雛形にする。必須メンバ一覧は [anatomy.md](anatomy.md#-で必須のメンバetlframeh-2026-10-版)。
- 確認: 2026-10-24 / VS 2022 17.14 / etlframe.h 053b537

### `error LNK2019: 未解決の外部シンボル "... _ETLCreateFrame(void)" が関数 OnEvents で参照されました`

- 原因: フレームクラス定義の後の `_ETL_IMPLEMENT` が無い。
- 対処: クラス定義の直後に `_ETL_IMPLEMENT` を書く。
- 確認: 2026-10-24

### `error C2027: 認識できない型 'CMyFrame' が使われています` （etlframe.h 内）

- 原因: 2 つ目の .cpp でも etlframe.h を普通に include した。etlframe.h は実体を含むので 1 つの .cpp 専用。
- 対処: 他の .cpp では `#define EE_EXTERN_ONLY` してから include し、クラス定義はヘッダに分ける（[anatomy.md](anatomy.md#cpp-を分割したいとき)）。
- 確認: 2026-10-24（EE_EXTERN_ONLY 版でビルドが通ることも確認）

### `warning C4984: 'if constexpr' は C++17 の言語拡張です` / `warning C5051`

- 原因: C++14 以下でビルドしている。古い vcxproj（VS 2012 世代のサンプル）を移行した場合など。
- 対処: C++ 言語標準を C++17 以上にする（`build/Plugin.props` を Import していれば設定済み）。
- 確認: 2026-10-24

### `error RC2104: undefined keyword or key name:` （.rc の日本語を含む行）

- 原因: UTF-8 の .rc に `#pragma code_page(65001)` が無く、rc.exe が別のコードページとして読んだ。
- 対処: .rc の先頭に `#pragma code_page(65001)` を書く。
- 確認: 2026-10-24

### `dotnet sln add` で `インポートされたファイル "$(VCTargetsPath)\Microsoft.Cpp.Default.props" は存在せず`

- 原因: dotnet CLI は C++ プロジェクト（vcxproj）を評価できない。
- 対処: `.\scripts\update-sln.ps1` で .sln を再生成する。
- 確認: 2026-10-24 / dotnet 9.0.319

---

## 実行時

### EmEditor に認識されない（メニューやツールバーに出ない）

順に確認する:

0. **登録したか**: PlugIns フォルダに置いただけでは認識されない。「プラグイン > プラグインの設定 > 追加」で DLL を登録する
   （確認: 2026-10-24 / EmEditor 23.0.5）。
1. **ビット数**: 64-bit 版 EmEditor には x64 の DLL、32-bit 版には Win32 の DLL。混ぜると読み込まれない。
2. **エクスポート**: `dumpbin /exports <dll>` で 7 関数が見えるか。見えなければ .def がリンクされていない
   （x64 では .def なしでもビルドが通ってしまう。[project-settings.md](project-settings.md#なぜ-def-が要るのか)）。
3. **依存 DLL**: `dumpbin /dependents` で `VCRUNTIME140.dll` 等が出ていたら `/MT` になっていない。

### deploy で上書きできない / 「EmEditor が起動中です」

- 原因: EmEditor が DLL をロックしている。ウィンドウを全部閉じてもタスクトレイに常駐していることがある。
- 対処: トレイアイコンから終了するか、「ファイル > すべて終了」。

### 「EmEditor は、EmEditor にも Windows にも含まれないコンポーネント内で異常終了しました」（パス: `…\<plugin>.dll`）

- 意味: プラグイン DLL の中でアクセス違反などが起き、EmEditor ごと落ちた。どこで落ちたかはこのダイアログからは分からない
- 実例（claude_chat の設定ボタン）: `for( auto& [k, v] : LoadUserMcpServers().items() )`。
  関数が返した一時の JSON は、range-for が `items()` の結果を受け取った時点で破棄される（C++17 は寿命を延ばさない）。
  ループ中に破棄済みのメモリを読んで落ちた
- 対処: 一時オブジェクトを変数に入れてから回す（`const json servers = LoadUserMcpServers(); for( … : servers.items() )`）。
  nlohmann/json の `items()`、`std::string_view`、`std::span` など「元のオブジェクトを参照するだけの戻り値」は同じ罠がある
- 調べ方: Debug ビルドを `deploy.ps1 -Configuration Debug` で配置し、VS で EmEditor.exe にアタッチして同じ操作をすると、落ちた行で止まる
- 確認: 2026-10-25 / EmEditor 23.0.5

### リッチエディットで色・太字・字下げが一切効かない（箇条書きの「・」など、文字の置き換えだけは反映される）

- 原因: `EM_SETEDITSTYLE` で `SES_EMULATESYSEDIT` を付けていた。これは「普通の EDIT のふりをする」設定で、書式がすべて無視される
- 対処: 付けない
- 確認: 2026-10-25 / EmEditor 23.0.5（claude_chat の履歴欄）

### テスト用のスクリーンショットで、リッチエディットの下半分が灰色になる

- `PrintWindow` で撮ると、描画が途中までしか写らないことがある（中身のテキストは全部入っていた）
- 見た目を確かめるときは、実際の画面から撮る（`Graphics.CopyFromScreen`）。中身は `EM_GETTEXTEX` で書き出して確認する。
  その際、書き出し用のバッファは `EM_GETTEXTLENGTHEX` の値ちょうどではなく、余裕を持たせる（ちょうどにしたら末尾が切れた）
- 確認: 2026-10-25

### `_s` 系関数（wcscpy_s など）のバッファ不足で落ちずに黙って失敗する

- 原因: etlframe.h の `DllMain` が `_set_invalid_parameter_handler` に何もしないハンドラを設定している
  （`EE_HYBRID_CRT` 未定義時）。CRT の無効パラメータで例外終了しない代わりに、エラーに気づきにくい。
- 対処: `_s` 系関数の戻り値を確認する。Debug ビルドではアサートが出るので Debug で動作確認する。
- 出典: etlframe.h のソース（実際に踏んだわけではない）

### メニュー名やステータスバーの説明が途中で切れる

- 原因: etlframe.h がステータス・バージョン文字列を 80 文字のバッファで読む。
- 対処: 短くする。メニュー名・プラグイン名は 520 文字まで。
- 出典: etlframe.h のソース
