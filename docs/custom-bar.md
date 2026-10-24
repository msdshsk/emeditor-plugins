# カスタムバー（サイドパネル）を作る

エディタの左右上下にドッキングするパネル。公式の Search / OutlineText / Explorer などと同じ仕組み。
公式マニュアルにはほぼ説明が無いので、公式プラグインのソース
（[Emurasoft/library](https://github.com/Emurasoft/library/tree/main/plugins-source-code) の `search700s.zip`, `outlinetext700s.zip`）から読み取った作法をまとめる。
実装例は [plugins/claude_chat](../plugins/claude_chat/)。

## API

```cpp
typedef struct _CUSTOM_BAR_INFO {
    size_t  cbSize;
    HWND    hwndCustomBar;   // [out] カスタムバーのウィンドウ
    HWND    hwndClient;      // [in]  中に入れるウィンドウ
    LPCWSTR pszTitle;        // [in]  タイトル
    int     iPos;            // [in]  CUSTOM_BAR_LEFT / TOP / RIGHT / BOTTOM
} CUSTOM_BAR_INFO;

UINT Editor_CustomBarOpen( HWND hwndFrame, CUSTOM_BAR_INFO* );   // 戻り値 = バーの ID（0 は失敗）
BOOL Editor_CustomBarClose( HWND hwndFrame, UINT nID );
```

`etlframe.h` のフレームクラスでは `_USE_CUSTOM_BAR = TRUE` にしておく（EmEditor v25.1 以降が `EPGI_USE_CUSTOM_BAR` で問い合わせる）。

## 開く

1. クライアントのウィンドウを、**フレーム（`m_hWnd`）を仮の親にして**作る。
   中身はモードレスダイアログ（`CreateDialogParam`）が楽。リソースは `STYLE DS_CONTROL | WS_CHILD`。
2. `Editor_CustomBarOpen( m_hWnd, &cbi )`。成功するとクライアントの親がカスタムバーに付け替えられる。
3. 戻り値の ID を覚えておく。`ShowWindow( hDlg, SW_SHOW )`。

クライアントのサイズはカスタムバーが合わせてくれる。中のコントロールの配置は `WM_SIZE` で自分でやる。

## 閉じる・閉じられる

| 状況 | 届くもの | すること |
|---|---|---|
| プラグインのコマンドで閉じる | `Editor_CustomBarClose` → `EVENT_CUSTOM_BAR_CLOSED` | 後始末 |
| ユーザーがバーの × で閉じる | `EVENT_CUSTOM_BAR_CLOSING` → `EVENT_CUSTOM_BAR_CLOSED` | 後始末 |
| EmEditor のウィンドウを閉じる | `EVENT_CLOSE_FRAME`（と CLOSED） | 開いていたら閉じる。次回起動時に開くかを保存 |

- **`EVENT_CUSTOM_BAR_CLOSING` / `CLOSED` は他のプラグインのバーが閉じたときにも届く。**
  `lParam` の `CUSTOM_BAR_CLOSE_INFO::nID` が自分の ID か必ず確かめる。
- `CLOSED` が届いた時点で、クライアントのウィンドウは**破棄済み**（`IsWindow` が FALSE）。
- `CUSTOM_BAR_CLOSE_INFO::dwFlags & CLOSED_FRAME_WINDOW` なら、ウィンドウごと閉じられた。
  公式プラグインはこれを「次回起動時に開く」設定として保存している。
- 自分で `Editor_CustomBarClose` を呼ぶと、その中から同期的に `CLOSED` が届く（ことを前提に書いておく）。
  呼び出し元とイベント側の両方で設定を保存すると上書きし合うので、自分で閉じている間はイベント側で保存しない。

## キー入力

パネル内の EDIT にフォーカスがあっても、そのままだと Enter / Tab / Ctrl+C などが EmEditor のショートカットに取られる。
フレームクラスの `PreTranslateMessage` で横取りする（公式 Search と同じ）:

```cpp
BOOL PreTranslateMessage( HWND, MSG* pMsg )
{
    HWND hwndFocus = GetFocus();
    if( !m_hDlg || !hwndFocus || !IsChild( m_hDlg, hwndFocus ) )  return FALSE;
    if( pMsg->message == WM_KEYDOWN && pMsg->wParam == VK_ESCAPE ){
        Editor_ExecCommand( m_hWnd, EEID_ACTIVE_PANE );   // エディタにフォーカスを戻す
        return TRUE;
    }
    return IsDialogMessage( m_hDlg, pMsg );
}
```

- 複数行 EDIT で Enter を改行にしたいときは `ES_WANTRETURN`。
- Ctrl+Enter などの独自ショートカットは `IsDialogMessage` の前に処理する。

## 細かい点

- リッチエディットを使うときは、ダイアログを作る前に `LoadLibrary( L"Msftedit.dll" )` しておき、テンプレートでは
  `CONTROL "", id, "RICHEDIT50W", ...` と書く。末尾への追記は「`EM_EXSETSEL` で末尾を選択 → `EM_SETPARAFORMAT` /
  `EM_SETCHARFORMAT( SCF_SELECTION )` → `EM_REPLACESEL`」（[plugins/claude_chat/chat_view.h](../plugins/claude_chat/chat_view.h)）
- コンボボックス（`CBS_DROPDOWNLIST`）の高さはドロップダウン部分を含む。`WM_SIZE` で配置するときも、リストが開いたときの高さを指定する
- EDIT の既定の文字数上限は 32K。チャット履歴などは `EM_SETLIMITTEXT, 0` で外す。
- 読み取り専用 EDIT は背景が灰色になる。`WM_CTLCOLORSTATIC` で `COLOR_WINDOW` のブラシを返すと通常の白になる。
- EDIT は改行が `\r\n` でないと改行として表示しない。
- `Editor_*` の多くはビューの HWND でなくフレームの HWND（`m_hWnd`）でも動く（公式 OutlineText が `Editor_GetCaretPos( m_hWnd, ... )` を使っている）。
  ただし選択範囲の取得・挿入は、念のため `OnEvents` で受け取った `hwndView` を覚えておいて使っている。
- DPI: レイアウトの数値は `GetDpiForWindow` で拡大する。
- タブの切り替えは `EVENT_DOC_SEL_CHANGED` で分かる。文書の編集（`EVENT_CHANGE`）はとても頻繁に来るので、
  フラグだけ立てて `EVENT_IDLE` でまとめて処理する（公式 OutlineText と同じ）。
- 文書の全文は `Editor_GetTextW( hwndView, 0, NULL )` で必要サイズを取ってから取得する。
  取れない場合に備えて、`Editor_GetLines` ＋ `Editor_GetLineW`（`FLAG_LOGICAL | FLAG_WITH_CRLF`）で 1 行ずつ集める処理も用意している。
  ※ `Editor_GetTextW` の挙動は EmEditor 上で未確認（2026-10-24 時点）。
