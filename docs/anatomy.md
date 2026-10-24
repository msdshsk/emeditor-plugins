# プラグインの仕組み

## 全体像

EmEditor のプラグインは、**決まった名前の関数を 7 つエクスポートしている DLL**。
EmEditor は `PlugIns` フォルダの DLL を読み込み、これらの関数を呼び出す。

| 序数 | 関数 | 呼ばれるとき |
|---|---|---|
| 1 | `OnCommand(HWND hwndView)` | メニュー/ツールバーから実行されたとき |
| 2 | `QueryStatus(HWND, LPBOOL pbChecked)` | メニュー表示時・アイドル時（**頻繁**）。有効/チェック状態を返す |
| 3 | `OnEvents(HWND, UINT nEvent, LPARAM)` | ファイルを開いた、文書が変わった等のイベント |
| 4 | `GetMenuTextID()` | メニュー文字列のリソース ID |
| 5 | `GetStatusMessageID()` | ステータスバー文字列のリソース ID |
| 6 | `GetBitmapID()` | ツールバー画像のリソース ID |
| 7 | `PlugInProc(HWND, UINT, WPARAM, LPARAM)` | 上記以外の問い合わせ全般（名前・バージョン・設定ダイアログ等） |

この 7 関数を自分で書く必要はない。`sdk/etlframe.h`（ETL）が実装していて、
自作の「フレームクラス」のメソッドへ振り分けてくれる。エクスポート名は `sdk/exports.def` で定義する。

## ファイルの役割（このリポジトリでの形）

| ファイル | 役割 | 編集 |
|---|---|---|
| `sdk/plugin.h` | EmEditor API。`Editor_*` 関数（中身は `SendMessage` の薄いラッパ）と定数 | しない |
| `sdk/etlframe.h` | 7 関数・`DllMain`・フレーム管理の実装 | しない |
| `sdk/exports.def` | エクスポート定義 | しない |
| `shared/eecommon.h` | windows.h 等の include と `_DEBUG_NEW` 定義（公式サンプルの stdafx.h 相当） | 必要なら |
| `plugins/<n>/<n>.cpp` | フレームクラス（＝プラグイン本体） | する |
| `plugins/<n>/<n>.rc`, `resource.h`, `version.h` | 文字列・ビットマップ・バージョン情報 | する |
| `plugins/<n>/toolbar.bmp` | ツールバー画像 | する |

## .cpp の決まった形

```cpp
#include "eecommon.h"
#include "resource.h"

#define ETL_FRAME_CLASS_NAME CMyFrame   // ① etlframe.h より前に
#include "etlframe.h"

class CMyFrame : public CETLFrame<CMyFrame>
{
    // ② etlframe.h が参照する enum 定数とメソッドを「全部」定義する
};

_ETL_IMPLEMENT                          // ③ クラス定義の後に
```

### ② で必須のメンバ（etlframe.h 2026-10 版）

欠けると `error C2039: '...': 'CMyFrame' のメンバーではありません` になる。

**enum 定数**

| 名前 | 意味 |
|---|---|
| `_USE_LOC_DLL` | 文字列リソースの場所。`LOC_USE_PLUGIN_DLL`(0)=自 DLL / `LOC_USE_LOC_DLL`(1)=`mui\<言語>\<名前>_loc.dll` / `LOC_USE_EMEDLOC_DLL`(2)=EmEditor 本体 |
| `_IDS_MENU` / `_IDS_STATUS` / `_IDS_NAME` / `_IDS_VER` | メニュー名 / ステータスバー説明 / 設定画面の名前 / バージョン文字列の ID（`_IDS_VER`=0 ならバージョン無し） |
| `_IDB_BITMAP` | 16x16・16 色ビットマップ |
| `_IDB_16C_24` | 24x24・16 色（0 = 無し） |
| `_IDB_TRUE_16_DEFAULT` / `_HOT` / `_BW` | 16x16 フルカラー（通常/ホット/無効） |
| `_IDB_TRUE_24_DEFAULT` / `_HOT` / `_BW` | 24x24 フルカラー |
| `_MASK_TRUE_COLOR` | 透過色（通常 `RGB(192,192,192)`） |
| `_ALLOW_OPEN_SAME_GROUP` | 実行中に同じウィンドウグループでファイルを開いてよいか |
| `_ALLOW_MULTIPLE_INSTANCES` | 複数インスタンスを許すか |
| `_SUPPORT_EE_PRO` | Professional 版をサポートするか |
| `_USE_CUSTOM_BAR` | カスタムバー（サイドのパネル）を使うか |

**メソッド**

`OnCommand`, `QueryStatus`, `OnEvents`, `QueryUninstall`, `SetUninstall`, `QueryProperties`, `SetProperties`,
`PreTranslateMessage`, `UseDroppedFiles`, `DisableAutoComplete`, `UserMessage`

> 公式サンプル（`plugin_sample_hello.zip`, Version 11 時代）には `_USE_CUSTOM_BAR` と `UserMessage` が無く、
> 最新の etlframe.h と組み合わせるとコンパイルできない。逆にサンプルにある `_MAX_EE_VERSION` / `_MIN_EE_VERSION` /
> `_SUPPORT_EE_STD` / `_MASK_256_COLOR` / `_IDB_256C_*` は現行 etlframe.h では参照されない（あっても害は無い）。
> SDK を更新したら etlframe.h の `T::_` と `pT->` を grep して必須メンバを確認すること。

## フレームクラスの寿命

- `CMyFrame` は **EmEditor のウィンドウ（フレーム）1 つにつき 1 インスタンス**。
  `EVENT_CREATE_FRAME` で生成、`EVENT_CLOSE_FRAME` で破棄される（etlframe.h が管理）。
- だからメンバ変数に持った状態はウィンドウごとに独立する。全ウィンドウ共通の状態は static/グローバルに置く。
- 各エクスポート関数は `hwndView` の親ウィンドウ（`GetAncestor(..., GA_ROOTOWNER)`）から該当フレームを探して呼ぶ。
- `m_hWnd` はフレームのウィンドウハンドル。

## イベント（OnEvents）

`nEvent` はビットフラグ。`if( nEvent & EVENT_CHANGE )` のように判定する。主なもの（全量は `plugin.h` の `EVENT_*`）:

| フラグ | 内容 |
|---|---|
| `EVENT_FILE_OPENED` | ファイルを開いた |
| `EVENT_CHANGE` / `EVENT_MODIFIED` | 文書が変更された / 変更フラグが変わった |
| `EVENT_SEL_CHANGED` / `EVENT_CARET_MOVED` | 選択範囲 / カーソル位置の変化 |
| `EVENT_DOC_SEL_CHANGED` | アクティブな文書（タブ）が切り替わった |
| `EVENT_SAVING` | 保存直前 |
| `EVENT_IDLE` | アイドル時 |
| `EVENT_CREATE_FRAME` / `EVENT_CLOSE_FRAME` | ウィンドウの生成/破棄 |
| `EVENT_UI_CHANGED` | UI 言語などの変更（v13 以前は `EVENT_LANGUAGE`） |

`EVENT_CHANGE` や `EVENT_CARET_MOVED` は高頻度なので、重い処理はしない。

## 文字列リソースの注意

etlframe.h はリソース文字列を固定長バッファで読む:

- 名前（`_IDS_NAME`）・メニュー（`_IDS_MENU`）: 520 文字。**`\n` を含むと、`\n` より後ろだけが使われる**
- ステータス（`_IDS_STATUS`）・バージョン（`_IDS_VER`）: **80 文字で切れる**

## 設定値の保存

`CETLFrame` に `GetProfileString` / `GetProfileInt` / `WriteProfileString` / `WriteProfileInt` 系がある。
内部では `Editor_RegQueryValue` / `Editor_RegSetValue` を使い、EmEditor の設定保存先（レジストリ or INI）に従う。
キー名を省略した版は DLL 名（拡張子なし）をセクションにする。

## .cpp を分割したいとき

etlframe.h は `DllMain` やエクスポート関数の**実体**を含むので、そのまま 2 つの .cpp で include すると壊れる。

1. `CMyFrame` のクラス定義をヘッダ（例 `frame.h`）に移す
2. メインの .cpp: これまで通り `etlframe.h` → `frame.h` → `_ETL_IMPLEMENT`
3. 他の .cpp: `#define EE_EXTERN_ONLY` してから `etlframe.h` → `frame.h` を include

## API を調べるには

- `sdk/plugin.h` を `Editor_` で検索するのが早い。各関数は `SendMessage` 1 発のラッパなので引数の意味も読み取りやすい。
- EmEditor ヘルプの「プラグイン リファレンス」（関数・イベント一覧）。
- 公式プラグインのソース: https://github.com/Emurasoft/library/tree/main/plugins-source-code
  （HTMLBar = カスタムツールバー、OutlineText / Search / WordCount = カスタムバーの参考。いずれも古い版）
