<div align="center">

<img src="assets/logo.png" width="128" height="128" alt="EvernightCapture アイコン">

# EvernightCapture

コマンドラインのウィンドウ画面取得ツール：条件でウィンドウを選び出し、そのウィンドウの画面を画像ファイルとして保存する。

[English](README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja-JP.md)

</div>

Windows.Graphics.Capture を基にした一式の画面取得チャネルを備え、入口は `ECAPTURE.EXE` である。単一の実行ファイルで、
CRT を静的リンクしているのでターゲットマシンに VC++ ランタイムは不要。出力はプログラムに優しい作りになっている：
`--help`/`--version` を除いてすべて JSON、終了コードは安定、診断には安定した `code` を添えるので、
人が直接叩いても、スクリプトや AI から呼び出しても扱いやすい。

現在のバージョン **0.4.0**：`--capture` の値はすべて使用可能（`wgc` / `dwm` / `printwindow` / `bitblt` /
`duplication` / `auto`）、`--monitor` はモニタ全体の画面取得と「モニタでウィンドウを絞り込む」機能を提供する。
かつて実装されていた `magnification` は削除済み（理由は AGENTS.md を参照）。

## 特徴

- **条件でウィンドウを選ぶ**：ハンドル / プロセス ID / イメージファイル名 / 完全パス / ウィンドウタイトル（完全一致、部分一致、正規表現）/
  ウィンドウクラス名。異なるオプション間は AND、同一オプションの複数指定は OR
- **6 本の画面取得チャネル**：他のウィンドウに隠れていても取得できる（`wgc` / `dwm` / `printwindow`）か、
  あえて画面上に見えるピクセルだけをコピーする（`bitblt` / `duplication`）
- **複数ウィンドウを一度に取得**：`--all` は一致した各ウィンドウから 1 枚ずつ保存し、`%i` などのプレースホルダで命名する
- **モニタ全体取得は人の同意が必須**：`--monitor` でモニタ全体を取得する前は必ずモーダルの確認ダイアログを表示する、
  **コマンドラインからも環境変数からも回避できない**
- **4 か国語のメッセージ**：`zh-CN` / `zh-TW` / `en` / `ja`。既定はシステム表示言語に従い、すべて exe 内のリソースにコンパイル済み
- **機械が読む JSON**：画面取得結果とエラーだけを含み、ツール名・バージョン・schema・引数のエコーバックのようなメタ情報は載せない

## クイックスタート

Visual Studio（「C++ によるデスクトップ開発」ワークロード）と Windows SDK が必要。ビルドスクリプトが両方を自動で見つける。

```powershell
.\build.ps1                                  # Release、成果物は build\ecapture.exe
ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
```

出力ディレクトリは**すでに存在していなければならない**。ツールはディレクトリを作らない。まず、誰に一致するかを確かめられる：

```powershell
ECAPTURE.EXE --process notepad.exe --dry-run --out D:\shots\_probe.png
```

よく使う書き方：

```powershell
# タイトル + ウィンドウクラス名で 1 つのウィンドウを特定する
ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png

# dry-run の候補リストからハンドルを拾って、名指しで指定する
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite D:\shots\one.png

# 一致した各ウィンドウから 1 枚ずつ、ファイル名に連番を付ける
ECAPTURE.EXE --pid 12345 --title-contains 报告 --all "D:\shots\rpt_%i.png"

# 画像バイトを標準出力へ流す（この場合は JSON が stderr に回る）
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# モニタ全体：必ず最初に確認ダイアログを出す。飛ばすスイッチは無い
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"
```

## オプション

表記は `--opt=value`・`-opt`・`/opt` のいずれも受け付ける。値自体が `-` で始まる場合は `--title=-x` のように書くか、
`--` でオプション解釈を打ち切る。短オプションは**まとめられない**（`-qi` は `cli.unknown_option` を返す）。以下は
`ECAPTURE.EXE --help` の出力をそのまま掲げたもので、オプションを変更した後は `.\scripts\mkreadme.ps1` を実行して
再生成する。**この部分の本文を手で編集しないこと**。

<!-- BEGIN ECAPTURE-HELP -->
```text
EvernightCapture (ECAPTURE.EXE) —— 条件でウィンドウを選び Windows.Graphics.Capture で画面を取得

使い方: ECAPTURE.EXE [条件...] <出力パス>          条件を一切与えない => このヘルプ
          ECAPTURE.EXE [条件...] --out <パス>      パスに - は画像バイトを標準出力へ
          ECAPTURE.EXE [条件...]                      出力パスなし => png を標準出力へ
          ECAPTURE.EXE --monitor [n] <パス>         --monitor かつウィンドウ条件なし => そのモニタ全体

取得対象（--monitor を付けない場合は下のウィンドウ条件だけで探します）
  --monitor, -m [<n|primary|all>] 取得するモニタ番号、1 始まり（ディスプレイ設定の順序）；primary = 主モニタ、all = 全モニタを 1 枚ずつ。ウィンドウ条件なしではそのモニタ全体を、ウィンドウ条件ありではそのモニタに重なるウィンドウだけを対象にします。値は省略可（= 主モニタ）、省略時は後続の引数を消費しないので --monitor out.png も使えます。モニタ全体取得は最初に確認ダイアログで同意を求め、これを飛ばすオプションはありません

ウィンドウ検索条件（同一オプションの繰り返しは OR、異なるオプションは同時成立）
  --hwnd <handle>                 ウィンドウハンドル。数字のみは 10 進、0x 接頭辞または a-f を含む場合は 16 進。0x 推奨
  --pid <pid>                     プロセス ID。10 進かつ 0 より大きい値
  --process, -p <image-name>      イメージファイル名（パスなし）、大文字小文字を区別しない。拡張子がない場合は .exe として扱う
  --exe <full-path>               イメージの完全パス、大文字小文字を区別しない
  --title, -t <exact-title>       ウィンドウタイトル完全一致
  --title-contains, -T <text>     ウィンドウタイトルに部分文字列を含む
  --title-regex, -R <regex>       ウィンドウタイトルを正規表現で一致（ECMAScript 構文、解析時に検証）
  --class, -c <class-name>        ウィンドウクラス名、大文字小文字を区別しない。例 Notepad / CabinetWClass

複数のウィンドウに一致したとき（排他）
  --index, -i <n>                 n 番目のウィンドウを選ぶ。1 始まり、表示可否と Z 順で並ぶ
  --newest                        最後に作成されたウィンドウを選ぶ
  --oldest                        最初に作成されたウィンドウを選ぶ
  --all, -a                       一致した各ウィンドウから 1 枚ずつ保存

画面取得方式（既定 wgc；OS のバージョンやウィンドウの性質で失敗することがある）
  --capture, -C <method>          wgc（既定、隠れていても取得可）/ dwm（DWM サムネイル、隠れていても取得可）/ printwindow（ウィンドウ自身の描画）/ bitblt（画面の表示ピクセルをコピー）/ duplication（デスクトップ複製を矩形で crop）/ auto（wgc-dwm-printwindow-bitblt の順にフォールバック、モニタ全体は wgc-duplication-bitblt）

出力
  --out, -o <path|->              出力パス。特殊値 - は画像バイトを標準出力へ。位置引数でも与えられ、全く指定しないのは --out - と同じ
  --format, -f <name>             エンコード形式を指定。未指定なら出力ファイルの拡張子から判定し、それも判別不能なら png
  --quality <1-100>               JPEG の品質、既定 100
  --no-overwrite                  既存のターゲットを上書きせずエラーで終了

その他
  --dry-run, -d                   解析と候補ウィンドウの列挙だけ行い、画面取得もファイル書き出しもしない
  --json, -j                      廃止済みの互換スイッチで副作用なし：成功もエラーも元から JSON
  --verbose, -v                   JSON に input 節（正規化された全入力）を追加し、notes も残す
  --quiet, -q                     notes を省略。errors は常に返る
  --lang, -l <language>           メッセージの言語。auto（既定、システム表示言語に従う）/ zh-CN / zh-TW / en / ja。未対応のシステム言語は en
  --help, -h                      テキストのヘルプ（この部分）を出力
  --version                       バージョンと段階を出力

表記: --opt=value / -opt / /opt すべて可。値自体が - 始まりなら --title=-x のように書き、-- でオプション解釈を打ち切り
出力: 成功もエラーも JSON で、captured / images のみ（ほかに errors / notes、input は --verbose のときだけ）
          --help / --version、および条件なしのときはテキスト
終了コード: 0 成功 / 1 引数エラー / 2 条件なし / 3 --help / 4 一致ウィンドウなし / 5 複数一致 /
        6 対象が保護か拒否 / 7 画面取得失敗 / 8 ファイル書き出し失敗 / 9 内部エラー
現在のビルド: --capture の値はすべて実装済み（wgc / dwm / printwindow / bitblt / duplication、auto は wgc-dwm-printwindow-bitblt の順にフォールバック；モニタ全体は wgc-duplication-bitblt）；出力ディレクトリは既存であること

例:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 報告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
```
<!-- END ECAPTURE-HELP -->

## 一致の規則

異なるオプション間は AND（すべてを満たすものだけを 1 つのウィンドウとして扱う）、同一オプションの複数指定は OR であり、
条件を複数のウィンドウまたぎで組み合わせることはない。

```powershell
ECAPTURE.EXE --process notepad.exe --title-contains 报告 D:\shots\r.png
# プロセスが notepad.exe かつタイトルに"报告"を含むウィンドウ
```

- `--title` は文字列全体の一致、`--title-contains` は部分文字列の一致で、両者とも**大文字小文字を区別する**。
  `--class` / `--process` / `--exe` は大文字小文字を区別しない。
- 列挙は既定で非表示のウィンドウとサイズ 0 のウィンドウを飛ばす。**最小化中のウィンドウは取得できない**ので、
  `hint` に個別に書き添えるだけにしている。
- 複数一致なのに区別のための指定が無い場合は勝手に 1 つを選ばず、`match.ambiguous_window`（終了コード 5）を返す。
  `hint` には Z 順で全候補を並べる。

## 出力の形

`--help`、`--version`、そして条件を一切与えない場合はプレーンテキスト。それ以外はすべて JSON で、
画面取得結果とエラーだけを入れる。

ウィンドウの画像（実際の出力の形。数値は一度の実際の画面取得から来たもの）：

```json
{
  "captured": 1,
  "images": [
    {
      "file": "D:\\shots\\EvernightCapture - エクスプローラー.png",
      "bytes": 60198,
      "width": 1247,
      "height": 607,
      "format": "png",
      "hwnd": "0x001B0C48",
      "pid": 31468,
      "title": "D:\\share\\EvernightCapture - エクスプローラー",
      "class": "CabinetWClass",
      "image": "explorer.exe",
      "elapsedMs": 156
    }
  ]
}
```

モニタの画像（`--monitor` かつウィンドウ条件が無いとき）は帰属するウィンドウが無いので、
`monitor` / `device` / `primary` の 3 フィールドに置き換わり、`hwnd` / `pid` / `title` / `class` / `image` は
まるごと現れない。呼び出し側は `monitor` の有無で 2 種類の画像を区別する。

エラー（`--hwnd` に無効な値を渡したとき）：

```json
{
  "captured": 0,
  "images": [],
  "errors": [
    {
      "code": "cli.invalid_number",
      "message": "--hwnd には有効なハンドル値が必要です（10 進、または 0x 付きの 16 進）",
      "option": "--hwnd",
      "value": "zzz",
      "hint": "数字のみは 10 進として解釈。16 進は 0x… の形で書くか、a-f を含めば 16 進として扱います"
    }
  ]
}
```

規則：

1. `captured` と `images` は常に存在する（空なら `[]`）。`errors` は空でなければ必ず出力する（`--quiet` でも抑止できない）。
   `notes` は空でないかつ `--quiet` 未指定のときだけ現れる。`input` は `--verbose` のときだけ現れる。
   呼び出し側は `errors` を先に見てから `images` を読む。
2. 診断項目で空のフィールドはキーごと省略し、`null` のプレースホルダは出力しない。
3. `code` の値は安定している：`cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`。追加はあっても改名はない。
4. ストリーム：既定ではすべて stdout に書き、stderr は空のまま。画像が標準出力を占有した瞬間（明示的な `--out -`、
   または出力パスを全く与えない場合）に JSON 全体が stderr へ移る。2 つのストリームが混ざることは絶対にない。
5. `captured` は `images` の件数と等しい。ウィンドウ 1 つにつき画像 1 枚で、`--monitor all` ならモニタ 1 台につき画像 1 枚。

## 終了コード

`0` 成功 / `1` 引数エラー / `2` 条件なし / `3` `--help` / `4` 一致ウィンドウなし / `5` 複数一致 /
`6` 対象が保護か拒否 / `7` 画面取得失敗 / `8` ファイル書き出し失敗 / `9` 内部エラー。新しい意味は番号の追加でのみ表現する。

終了コードと body は互いに独立した 2 つの信号であり、`2`/`3`/`4`/`5` はクラッシュではなく正常な制御フローである。
**部分成功を許す**：`--all` や `--monitor all` でいくつかの対象が失敗した場合、すでに書き出した画像は `images` に残る
（`captured` は 0 より大きくなり得る）が、終了コードは `7` になる。

## 画面取得方式

| 値 | チャネル | 隠れたウィンドウを撮れるか | ハードウェアアクセラレーション描画 | 必要 OS |
| --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | 撮れる（DWM のキャッシュ） | 正常 | Win10 1803+ |
| `dwm` | DwmRegisterThumbnail | 撮れる | 大半は正常、保護されたウィンドウは黒 | Win7+ |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | 撮れる（ウィンドウ自身の描画） | 多くの場合まっ黒 | Win8.1+ |
| `bitblt` | 画面 DC からの BitBlt | 撮れない、見えるピクセルだけをコピー | 一部が黒 | 全バージョン |
| `duplication` | DXGI デスクトップ複製でモニタ全体のフレームを取り矩形で crop | 撮れない、見えるピクセルだけをコピー | 正常 | Win8+、リモートデスクトップ/仮想 GPU は内容を得られないことが多い |
| `auto` | wgc → dwm → printwindow → bitblt の順にフォールバック | 尽力 | 尽力 | — |

- 「そのウィンドウ自身の画面」が欲しいとき（別のものに覆われていても）は既定の `wgc`。「今この瞬間の画面の見た目」が
  欲しいとき（覆っているもの込みで）は `bitblt` か `duplication` を使う。
- `--capture` の値を間違えたら解析時に `cli.unknown_capture_method`（終了コード 1）を返し、**既定チャネルに
  フォールバックすることはない**。フォールバックを許されるのは `auto` だけで、フォールバック成功時は `note.capture_channel` で
  実際にどのチャネルを使ったか伝える。
- DRM / 保護されたコンテンツは常に黒画面になる。ドライバ側の黒枠（一部のプレイヤー）は通るチャネルと通らないチャネルが
  あり、保証はない。
- モニタ全体取得が使えるのは `wgc` / `duplication` / `bitblt` だけ。`--monitor` に `dwm` や `printwindow` を
  組み合わせると解析時に `capture.unsupported`（終了コード 1）を返す。`auto` はモニタ全体では
  wgc → duplication → bitblt の順にフォールバックする。

## モニタ全体取得とプライバシー確認

`--monitor` を付けてウィンドウ条件が無いときはモニタ全体を取得する。このとき**必ず最初にモーダルの確認ダイアログを
出す**（対象モニタ、使うチャネル、画像の行き先を列挙し）、「はい」を押したときだけフレームを取得する：

- **コマンドラインでの回避も環境変数での回避も無い**。ダイアログを出せない場合（サービスセッション、対話できる
  デスクトップが無い）は拒否として扱う。
- 「いいえ」と答えた場合も出せなかった場合も `capture.access_denied` + 終了コード `6` で、ファイルは書き出さない。
- 「はい」の後 1 秒待ってからフレームを取得する。ダイアログが閉じるアニメーションが画像に写るのを避けるためで、
  確認ダイアログ自体は画像に写らない。
- `--dry-run` と「モニタでウィンドウを絞り込む」モードはモニタ全体の画面を取得しないので、ダイアログも出ない。
- 「人に拒否された」と「パスが間違っている」を区別したいなら `--out` を明示するしかない。出力パスが無いと、
  いかなる失敗も `cli.missing_output` + 終了コード 1 に潰され、本当の理由は外に出さない。

`--monitor`（値を省略）と `--monitor primary` は主モニタ、`--monitor 2` は 2 台目のモニタ、
`--monitor all` は全モニタを 1 枚ずつ。番号は `EnumDisplayMonitors` の順で 1 始まり。範囲外は
`match.monitor_out_of_range`（終了コード 1）を返し、`hint` にこの PC の全モニタを列挙する。
`--monitor <n>` をウィンドウ条件と同時に指定すると＝モニタでウィンドウを絞り込む（ウィンドウ矩形とそのモニタに
重なりがあれば一致し、モニタをまたぐウィンドウは両方のモニタで一致扱い）で、出てくるのは引き続きウィンドウ画像、
確認ダイアログも出ない。`--monitor all` はウィンドウ**一致**条件と排他（`cli.monitor_conflict`、終了コード 1）だが、
`--all` / `--index` のような区別用のオプションは一致条件に数えないので、これらと一緒に使える。

## ファイル名プレースホルダ

`--out` のパスの中で使う。複数枚の画像はこれで区別する：

| プレースホルダ | 意味 |
| --- | --- |
| `%i` | 連番、1 始まり（`--all` の複数ウィンドウ、`--monitor all` の複数モニタ） |
| `%h` | ウィンドウハンドル、`0x001B0C48` の形。モニタ対象は 0 |
| `%p` | プロセス ID。モニタ対象は 0 |
| `%n` | モニタ対象は `\\.\` 接頭辞を除いたデバイス名（例 `DISPLAY1`） |
| `%d` | ローカル日付 `YYYYMMDD` |
| `%t` | ローカル時刻 `HHMMSS` |
| `%%` | リテラルの `%` を 1 つ。それ以外の `%x` は 2 文字そのままに残す |

`--all` の出力名にプレースホルダが無いと、自動的に `_連番` を追加し、`note.all_without_placeholder` を出す。

## メッセージの言語

`--lang`（`-l`）で `zh-CN` / `zh-TW` / `en` / `ja` を選ぶ。付けないか `auto` にしたときは Windows の表示言語を使い、
それがこの 4 か国語以外なら `en` になる。値の受け付けは寛容：大文字小文字を区別せず、`_` と `-` は同等、
`zh_TW` / `zh-Hant` / `cht` / `tw` は繁体字、`chs` / `cn` / `zh-Hans` は簡体字、`jp` は日本語。
間違えると既定言語にフォールバックするのではなく、解析時に `cli.unknown_language`（終了コード 1）を返す。

**人に見せる文字だけが言語に追随する**：診断項目の `message` / `hint`、`--help` の全文である。
`code`、JSON のキー名、値の列挙、`0x…` のハンドル、`HRESULT` の数値は一切変わらないので、呼び出し側は `code` で
分岐すればよい。メッセージは exe 自身の組み込みリソース（`resources/strings-<言語>.txt` を言語ごとに 4 本の
`RCDATA` としてコンパイル）なので、オフラインでも言語を切り替えられる。

## AI / スクリプトからの呼び出しガイド

このツールはプログラマブルな呼び出しを前提に設計されているので、以下の約束に従うのが一番手間がかからない。
リポジトリには AI に使い方を教える skill も同梱してある：`.agents/skills/ecapture-screenshot/`
（`SKILL.md`、`references/cli-contract.md`、exe のコピー入り）。

1. **まず `--dry-run` で一度探る**、それから区別を付け、最後に本番の画面取得。`--dry-run` はフレームも取得せず
   ファイルも書き出さず、候補は `notes[0].value` に入る：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。注意すべきは、`--dry-run` でも
   `--out` は必須で、無いと `cli.missing_output` + 1 になること。そして
   **`--dry-run` だけ・ウィンドウ条件を一切与えない = テキストのヘルプ + 終了コード 2** である。
2. **`errors[].code` で分岐し、`message` の文字列を突き合わせないこと**（あちらは `--lang` に追随する）。よく出る
   いくつか：`match.no_window`（4、条件が狭いか対象が最小化中）、`match.ambiguous_window`（5、`hint` の候補から
   選ぶ）、`match.index_out_of_range` / `match.monitor_out_of_range`（1、`hint` に全候補を列挙してある）、
   `cli.missing_output`（1）、`cli.invalid_format`（1）、`capture.failed`（7）、`capture.access_denied`（6）、
   `io.write_failed`（8、ディレクトリが存在しない）、`io.file_exists`（8、`--no-overwrite` と併用したとき）。
3. **読むストリームは場合分けする**：`--out <ファイル>` を指定すれば JSON は stdout にあり stderr は空なので、
   そのままパースしてよい。`--out -` または出力パス無しでは画像バイトが stdout を占め、JSON 全体が stderr に移る。
   PowerShell 5.1 の `2>&1` は stderr をエラーレコードに包装してしまうので、画像と JSON の両方が欲しければ
   `1>`/`2>` を分けてリダイレクトする。
4. **0 以外の終了コードを即・全滅と扱わないこと**：部分成功では `captured` は 0 より大きく終了コードは 7 だが、
   すでに書き上がった画像はそのまま使える。
5. **終了コード 0 は「画面が正しい」の意味ではない**：保護されたコンテンツや一部のプレイヤーのドライバは、
   成功を返しながらいっしょに黒フレームを渡してくる。正しさを判定したいならピクセルを検証する——たとえば単色の
   ウィンドウを対象に覆いかぶせてから再取得し、掴んだのが対象の画面か覆ったものかを見る。最低でも
   `width`/`height` を対象ウィンドウの矩形と突き合わせる。
6. **モニタ全体は人に聞いてから**：`--monitor` によるモニタ全体取得はモーダルダイアログを表示し、人が答えるまで
   プロセスを止める。回避口は無い。自動化のフローで「お手頃に取れる画面全体の画像」として扱ってはならず、呼び出す前に
   ユーザーへ伝え、`--out` を明示する。あるウィンドウだけを撮りたいなら、対象をモニタ全体に拡張しないこと。
7. 「あるアプリケーション」を安定して掴みたいなら `--process`/`--exe` に `--class` を添えるのが優先。
   タイトル照合は大文字小文字を区別するため、言語環境をまたぐと信用できない。

## ビルドとテスト

| コマンド | 用途 |
| --- | --- |
| `.\build.ps1` | Release ビルド、成果物 `build\ecapture.exe`。`-Config Debug`、`-Clean` が選べる |
| `.\tests\cli.ps1` | 出力契約の断言 77 例 + ストリーム分離 + 多言語チェック（すべて `--dry-run`、画面取得なし） |
| `.\scripts\check-lang.ps1` | 4 か国語のメッセージの key / プレースホルダの対応チェック。exe に本当に 4 本のリソースがコンパイル済みかも確かめる |
| `.\tests\smoke.ps1` | 実機スモーク：メモ帳を開く → 画面取得 → PNG のサイズとピクセル内容を検証 |
| `.\tests\channels.ps1` | 実機のチャネル比較：6 チャネル + 遮蔽対照 |
| `.\tests\screen.ps1` | 実機のモニタ全体テスト：確認ダイアログの挙動 + 3 本のモニタチャネル + 赤い塊の位置 + 陰性対照 |
| `.\scripts\mkreadme.ps1` | 各言語の `--help` の出力をそのまま使って 4 本の README のヘルプ節を再生成 |

`build.ps1` は vswhere で VS を見つけ、VS 同梱の cmake/ninja を優先して使う。ビルドは `/W4` で警告ゼロが要求される。
Git Bash で手でテストするときは、まず `export MSYS2_ARG_CONV_EXCL='*'` を実行すること。でないと `/help` がパスとして
書き換えられ、`--out /tmp/x.png` が変なパスに変換される。

## ライセンス

EvernightCapture は [Mulan Permissive Software License v2（Mulan PSL v2、木蘭寬鬆許可證 第2版）](http://license.coscl.org.cn/MulanPSL2) の下で
公開している。中国語と英語の全文（同一ファイルに bilingual で収録）は [LICENSE](LICENSE) を参照。

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
