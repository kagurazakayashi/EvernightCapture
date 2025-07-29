<div align="center">

![EvernightCapture アイコン](resources/icon.ico)

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
- **画面取得には人の承諾**：フレームを実際に取得するなら、信頼できるウィンドウ経路でも必ずモーダルの確認
  ダイアログを先に出す。`--yes` が省略できるのは「選択したウィンドウ自体に縛られ、デスクトップからサンプルしない」
  経路だけ —— デスクトップのピクセルを読む経路は必ず人が答える必要があり、回避口は無い
- **対象の身元を確認する**：対象を選んだ瞬間にハンドル・所属プロセス・そのプロセスの作成時刻・ウィンドウクラス名・当初の条件を
  記録し、各画面取得の試行の前、および人工の確認に答えたあとにもう一度確認する。対象が破棄された、ハンドルが別プロセスに
  再利用された、もしくは当初それを選んだ条件を満たさなくなったときには、誰も承諾していない絵ではなく
  `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable` を返す
- **守られる実行期限**：`--timeout-ms` は自動処理区間（条件マッチ、バックエンド再試行、フレーム待ち、エンコード、
  ファイル書き込み）全体で共有する 1 つの予算で、`--consent-timeout-ms` は確認ダイアログを別枠で時間測定します。
  他のプロセスを待たせる呼び出し（`PrintWindow`、DWM の読み戻し、正規表現評価）はツールが止められるヘルパー
  プロセスで走るので、フリーズした対象ウィンドウがこのツールをフリーズできなくなりました
- **4 か国語のメッセージ**：`zh-CN` / `zh-TW` / `en` / `ja`。既定はシステム表示言語に従い、すべて exe 内のリソースにコンパイル済み
- **機械が読む JSON**：画面取得結果とエラーだけを含み、ツール名・バージョン・schema・引数のエコーバックのようなメタ情報は載せない
- **画面取得なしでウィンドウを一覧・検査できる**：`--list` は該当ウィンドウを構造化 JSON で返す（ハンドル、PID、クラス名、タイトル、イメージ名、物理矩形、表示/最小化、Z 順、後続の画面取得が再確認する身元欄）。複数一致は `--offset` / `--limit` でページ送りし、画面取得の歧義にしない。`--inspect` は 1 窓を項目ごとに検査し、複数一致なら代わりを選ばずに歧義として返す。この 2 つはピクセルも取らず確認も出さずファイルも書かずウィンドウに触れず、`--yes` は効果を持たず、返すものは明示されたスナップショットである
- **能力を読み取りで照会できる**：`--capabilities` / `--diagnostics` は、ピクセルも取らず確認も出さずファイルも書かず通信もしないで、この機がいま通せる経路、`--yes` が実際に及ぶ範囲、検証可能なビルド識別子を返す。「このビルドにその経路がある」「いまこの機で通せる」「本プロジェクトがこういうシステムで実測した」は別々の欄として書き分け、答えが出なければ `unknown` をそのまま返す。実際の撮影や符号化で能力を探ることはしない

## クイックスタート

Visual Studio（「C++ によるデスクトップ開発」ワークロード）と Windows SDK が必要。ビルドスクリプトが両方を自動で見つける。

```powershell
.\build.ps1                                  # Release、成果物は build\ecapture.exe
ECAPTURE.EXE --process notepad.exe D:\shots\epad.png   # フレームを取る前に確認ダイアログを出す
```

出力ディレクトリは**すでに存在していなければならない**。ツールはディレクトリを作らない。まず、誰に一致するかを
確かめられる（`--dry-run` は画面取得もファイル書き出しも確認ダイアログもしない）：

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

# ウィンドウ 1 枚を確認なしで：--yes はウィンドウ自身の画像だけを使う経路にしか効かない
ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png

# モニタ全体：デスクトップのピクセルなので必ず確認ダイアログを出す。--yes でも省略できない
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
  --monitor, -m [<n|primary|all|device:|id:>] 対象モニタの番号。1 始まりの 10 進（今回の列挙順で、「表示設定」の識別子とは限らない。モニタを特定するには結果の device を見る）；primary = メイン、all = 全モニタを 1 枚ずつ。ウィンドウ条件なし = 画面全体の撮影、ウィンドウ条件あり = そのモニタに重なるウィンドウのみ。値は省略可（= メイン）で、省略時は次の引数を取り込まないので --monitor out.png も従来どおり。画面全体はデスクトップのピクセルなので必ず確認ダイアログを出し、--yes でも省略できない；モニタでウィンドウを絞った場合は従来どおりウィンドウ画像；特定のモニタを名指しするには --screens が返す識別子を使う：device:<名前>（このデスクトップ接続）または id:<モニタのデバイスパス>（セッションをまたぐほう）。番号は抜き差しや解像度変更後に別のモニタを指す可能性がある

ウィンドウ検索条件（同一オプションの繰り返しは OR、異なるオプションは同時成立）
  --hwnd <handle>                             ウィンドウハンドル。数字のみは 10 進、0x 接頭辞または a-f を含む場合は 16 進。0x 推奨。符号と空白は不可、下線は 16 進数字の間に限り有効
  --pid <pid>                                 プロセス ID。10 進のみかつ 0 より大きい値
  --process, -p <image-name>                  イメージファイル名（パスなし）、大文字小文字を区別しない。拡張子がない場合は .exe として扱う
  --exe <full-path>                           イメージの完全パス、大文字小文字を区別しない
  --title, -t <exact-title>                   ウィンドウタイトル完全一致
  --title-contains, -T <text>                 ウィンドウタイトルに部分文字列を含む
  --title-regex, -R <regex>                   ウィンドウタイトルを正規表現で一致（ECMAScript 構文、解析時に検証）
  --class, -c <class-name>                    ウィンドウクラス名、大文字小文字を区別しない。例 Notepad / CabinetWClass

複数のウィンドウに一致したとき（排他）
  --index, -i <n>                             n 番目のウィンドウを選ぶ。1 始まりの 10 進、表示可否と Z 順で並ぶ
  --topmost-match                             現在の Z 順で最上位の一致ウィンドウを選ぶ（作成日時ではない）
  --bottommost-match                          現在の Z 順で最下位の一致ウィンドウを選ぶ（作成日時ではない）
  --newest                                    --topmost-match の旧名。作成日時ではなく現在の Z 順で選びます
  --oldest                                    --bottommost-match の旧名。作成日時ではなく現在の Z 順で選びます
  --all, -a                                   一致した各ウィンドウから 1 枚ずつ保存

画面取得方式（既定 wgc；OS のバージョンやウィンドウの性質で失敗することがある）
  --capture, -C <method>                      wgc(既定、隠れても撮影可) / dwm(DWM サムネイル、隠れても撮影可) / printwindow(ウィンドウ自前描画) / bitblt(画面の可視ピクセルをコピー) / duplication(デスクトップフレームを矩形で切り出し。モニタの回転に合わせて向きを補正し、対象と最も重なる 1 台の出力だけを取ります。切り足りないときは capturedRect/clipped が付きます) / auto(wgc-dwm-printwindow-bitblt の順にフォールバック；画面全体は wgc-duplication-bitblt)。ウィンドウ自身だけ: wgc / printwindow / dwm サムネイル。画面から取る: bitblt / duplication と dwm の画面フォールバック

撮影の承諾（実際の撮影では既定で確認します。--yes はウィンドウ内容の経路だけ省略）
  --yes, -y                                   ウィンドウ自身の画像だけを使う経路の確認を省略します。有効な画像を保証せず、権限・保護コンテンツ・エラー・上書き保護にも影響しません。画面からサンプルする経路（bitblt、duplication、画面全体のあらゆる経路、dwm の画面フォールバック）は必ずダイアログを出します。--yes=false は明示的に確認を

実行期限（自動処理区間の総予算。承諾を待つ時間は別計算で、期限切れは拒否として扱います）
  --timeout-ms <ms>                           自動処理区間の総予算（ミリ秒）。対象の選択に成功した時点から、条件評価・バックエンド再試行・フレーム取得・エンコード・ファイル書き込みが同じ残り時間を共有し、どの段階も予算を新しく受け取り直すことはありません。指定なしまたは 0 = 総予算なし。その場合も隔離実行は組み込み上限（5000 ミリ秒）で抑えます。承諾ダイアログを待つ時間はここには含めません（--consent-timeout-ms を参照）。予算を使い切った時点でその画像は書き出されず、段階に応じ match.timeout / capture.timeout / io.timeout を返します
  --consent-timeout-ms <ms>                   承諾ダイアログが応答を待つ最長時間（ミリ秒）。指定なしまたは 0 = 応答があるまで待ち続けます。期限切れは「拒否」として扱い、「既定で同意」には決してしません（capture.consent_timeout）。この待機は別計算で、--timeout-ms の予算を消費しません。はい を押した後のおよそ 1 秒の閉じるアニメーション待ちもこの段階に含め、期限に間に合わせるために省略することはありません

出力
  --out, -o <path|->                          出力パス。特殊値 - は画像バイトを標準出力へ書き込む。位置引数でも指定でき、パスを全く与えないと --out - と同じ。一括分の出力名は画面取得前にまとめて確定します。2 つのターゲットが同じ名前に 解決される場合はエラーになり、黙って上書きすることはありません。stdout には一度に 1 枚だけ出力できるため、複数のターゲットにヒットしたバッチはパラメータエラーとなり、1 枚も取得しません
  --format, -f <name>                         エンコード形式を指定。未指定なら出力ファイルの拡張子から判定し、それも判別不能なら png
  --quality <1-100>                           JPEG の品質。10 進で 1..100、既定 100
  --no-overwrite                              ターゲットが既に存在するなら上書きせずエラー終了（値を省略すれば禁止が有効）。--no-overwrite=false（0 / no / n / off）で禁止を取り消します。=true / 1 / yes / y / on は値を省略した時と同じ。複数指定した場合は最後のものが有効

照会（読取専用）
  --capabilities                              この機の能力を JSON で返す（版数、OS とセッション条件、各経路の available / unavailable / unverified、--yes の適用範囲）。読み取り専用で撮影も確認も書き込みもしない。available は保証ではない。--lang / -v / -q のみ可、撮影オプションや出力先と併用すると cli.query_conflict
  --diagnostics                               ビルドと版数の診断を JSON で返す（検証可能なビルド識別子、プラットフォームと経路の状態）。フィールドは --capabilities と共通。送信・撮影・ユーザーファイル列挙はせず、ユーザー名・環境変数・パスも出さない。--verbose で各質問の回答を追加
  --screens                                   全モニタを読み取り専用で列挙する：ツール内の番号、デバイス名、メインかどうか、物理矩形、読み取れた場合の DPI と回転、帰属アダプタ。そしてこれらの識別子がそれぞれどこまで安定かも記す。ピクセルを読まず、確認ダイアログも出さず、ファイルも書かず、表示設定も変更しない。返す device:<名前> と id:<パス> はそのまま --monitor に書ける。スクリーンショット系のオプションとは排他（cli.query_conflict + 終了コード 1）；実際に画面全体を撮るときは必ず確認ダイアログを出し、--yes では省略できない
  --list [<all>]                              条件を満たす最上位ウィンドウを JSON で一覧化。撮影も確認も書き込みもせず、出力パスも不要。ページ送りは --offset / --limit、all で最小化ウィンドウも含める。結果はこの瞬間のスナップショットなので撮影前に身元を再確認する
  --inspect [<path>]                          選択戦略が定める 1 件のウィンドウを読取専用で検査。複数一致は match.ambiguous_window + 5 を返し、代わりに選ばない。path で完全パスも書く（既定はファイル名のみ）
  --offset <n>                                照会で先頭 n 件を飛ばす（10 進）
  --limit <n>                                 照会の 1 回あたりの最大件数（既定 50）

その他
  --dry-run, -d                               解析と候補ウィンドウの列挙だけ行い、画面取得もファイル書き出しもしない
  --json, -j                                  廃止済みの互換スイッチで副作用なし：成功もエラーも元から JSON
  --verbose, -v                               JSON に input 節（正規化された全入力）を追加し、notes も残す
  --quiet, -q                                 notes を省略。errors は常に返る。--verbose と同時指定なら --verbose に従う
  --lang, -l <language>                       メッセージの言語。auto（既定、システム表示言語に従う）/ zh-CN / zh-TW / en / ja。未対応のシステム言語は en
  --help, -h                                  テキストのヘルプ（この部分）を出力
  --version                                   バージョンと段階を出力

表記: --opt=value / -opt / /opt すべて可。値自体が - 始まりなら --title=-x のように書き、-- でオプション解釈を打ち切り。数値は 10 進のみ（--hwnd は 0x 付きの 16 進も可）
出力: 成功もエラーも JSON で、captured / images のみ（ほかに errors / notes、input は --verbose のときだけ）
          --help / --version、および条件なしのときはテキスト
終了コード: 0 成功 / 1 引数エラー / 2 条件なし / 3 --help / 4 一致ウィンドウなし / 5 複数一致 /
        6 対象が保護か拒否 / 7 画面取得失敗 / 8 ファイル書き出し失敗 / 9 内部エラー
現在のビルド: --capture の値はすべて実装済み（wgc / dwm / printwindow / bitblt / duplication、auto は wgc-dwm-printwindow-bitblt の順にフォールバック；モニタ全体は wgc-duplication-bitblt）；出力ディレクトリは既存であること
実行環境: 64 ビット版 Windows、宣言上の下限はビルド 18362（Windows 10 version 1903）、実測はビルド 19045 のみ。この機のバージョンでは提供されない経路は、取得も確認表示も前に env.os_too_old / env.channel_unsupported として報告します（前者は経路を変えても変わりません）。--verbose の input.osBuild と input.captureChain に今回の利用可能な経路が出力されます

例:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 報告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
  ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png
  ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --consent-timeout-ms 60000 D:\shots\epad.png
  ECAPTURE.EXE --capabilities  --capture を決める前に照会する
```
<!-- END ECAPTURE-HELP -->

## 引数の表記

数値を取るオプションは、各自が文書で約束した表記しか受け付けません。パーサは進数を推測しません。

- `--pid`、`--index`、`--monitor <n>`、`--quality` と 2 つの期限は **10 進のみ**（`[0-9]+`）です。
  符号・空白・小数点・指数記法（`1e3`）・桁区切りの下線・`0x` 接頭辞・ASCII 以外の数字はいずれも不可。
  範囲も同じ解析処理で判定します（`--pid` 1..4294967295、`--index` と `--monitor` 1..65535、
  `--quality` 1..100、期限 0..86400000）。合わなければ `cli.invalid_number` + 終了コード 1。値は
  型変換も周回もしないため、`--pid 1e3` が 483 になることも `--hwnd -1` が `UINT64_MAX` になることもありません。
- `--hwnd` は文書どおり 3 通りの表記を受け付けます：数字のみは 10 進、`0x` / `0X` 接頭辞は 16 進、
  `a-f` を含む素の表記は 16 進（Spy++ の形式なので `--hwnd 1e3` は `0x1e3` です）。符号・空白・64 ビスを
  超える値・ハンドル `0` は拒否します。下線は 16 進表記でのみ、16 進数字の間に限って有効です：
  `0x001A_0B4C` は可、`0x_1A`、`1A__0B4C`、`1A0B4C_`、`12_34` は不可。
- `--monitor` の値は省略できるため、「次の引数を自分の値として取り込むか」は上の構文そのもので判断します。
  `--monitor out.png` は従来どおり「メイン画面 + out.png に出力」ですが、`--monitor 1e3` は書き間違えた
  モニタ番号として報告し、出力ファイル名にすり替えません。
- 値を必要とするオプションの直後の引数は、別のオプションに似ていてもその値です。`--title --lang ja` は
  タイトル `--lang` を探します。`-` 始まりの値は `--title=-x` と書くか、`--` でオプション解釈を打ち切って
  ください（`--` 以降はすべて位置引数、`--` 自体は捨てられます）。
- 同じオプションの重複指定：条件系は OR（`--title A --title B`）、値を取るオプションは最後のものが有効
  （`--timeout-ms 9000 --timeout-ms 300` は 300）、`--lang` も同じです。`auto`（または値の省略）は
  **明示的にシステム表示言語へ戻す**意味で、前回の指定を保持しません。不正な `--lang` は
  `cli.unknown_language` を、その時点で確定済みの言語で報告します。
- `--verbose` と `--quiet` を同時に指定した場合は `--verbose` に従います：notes はそのまま渡され、理由を
  示す `note.flag_overrides_quiet` が 1 条加わります。`errors` と、`images[].source` / `path` / `scope`
  といった出所フィールドは `--quiet` でも隠しません。

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
- 候補は**現在の Z 順**（一番上にあるものから）に並ぶ。`--topmost-match` / `--bottommost-match` はその並びの最初 / 最後を選ぶ。
  古い `--newest` / `--oldest` は互換のための別名として残り、動作はまったく同じ —— これまで選んできたのは常に Z 順の位置であって作成時刻ではない。
  Windows にはウィンドウの作成日時を取る公開 API が無く（プロセスの起動時刻もそれではない）。古い名前を使うと `note.deprecated_option` が
  1 件増えるだけ。同じ戦略の新旧両方の書き方を一緒に与えても（`--newest --topmost-match`）1 つの戦略であり、衝突としては扱われない。

## 出力の形

`--help`、`--version`、そして条件を一切与えない場合はプレーンテキスト。それ以外はすべて JSON で、
画面取得結果とエラーだけを入れる。

読み取り専用の環境照会 3 つ（`--capabilities` / `--diagnostics` / `--screens`）は**別の契約書**で、載せているのはこのマシンの
環境と能力、つまり 1 回の撮影結果ではない。だから `contract` と `contractVersion` を持つのはこの 2 つだけ。
逆に通常の撮影 JSON がトップレベルのメタ情報を増やす理由にはならない —— あちらはこれまでどおり
`captured` / `images`（必要に応じ `errors` / `notes` / `input`）だけで、照会側に `program.version` があっても
写しは書かない。

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
      "source": "wgc",
      "path": "wgc",
      "scope": "window",
      "rect": { "x": 237, "y": 418, "width": 1261, "height": 614 },
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
まるごと現れない。呼び出し側は `monitor` の有無で 2 種類の画像を区別する。`path` / `scope` / `rect` はどちらの形にも付く：`path` は実際に通った内部経路の名前、`scope` はそこから導いた `window` / `desktop`、そして `rect` はその経路が承諾された取得領域（モニタ画像ならそのモニタの矩形なので、常に `scope=desktop`）。

デスクトップ全体のフレームから対象を切り出して読むチャネル（`duplication`、および `bitblt` / `dwm` の
画面経路）は、さらに、そのピクセルをデスクトップの**どこ**から実際に得たかも報告する：`requestedRect` は
そのチャネルが取りに行こうとした領域、`capturedRect` は実際に取り得た領域で、両方とも仮想スクリーン座標、
つまり `rect` とも確認ダイアログが列挙した矩形とも座標系が揃っている。`clipped` は両者が一致しないときだけ
現れる（ウィンドウが 2 台のモニタにまたがっているか、画面の縁からはみ出している場合）：画像はそのまま
渡されるが、対象の全体ではないということで、`note.capture_clipped` が左右上下それぞれ何ピクセル失われたかを
記す。`rotation` は、デスクトップのフレームを、そのモニタが実際に表示している向きに合わせるために回転
（時計回り 90 / 180 / 270 度）する必要があったときだけ現れ、現れない場合は回転を適用していないことを意味する。
ウィンドウ内容のチャネル（`wgc`、`printwindow`、`dwm.thumbnail`）は構造上いつも対象を全体ごと取得するので、
これらのキーは一切出さない——キーが無いのは「何も取り落としていない」の意味であって「不明」の意味ではない。

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
      "hint": "数字のみは 10 進として解釈。16 進は 0x… の形で書くか、a-f を含めば 16 進として扱います。符号と空白は不可；下線は 16 進数字の間に限ります（0x_1A や 1A__2B は認めません）"
    }
  ]
}
```

規則：

1. `captured` と `images` は常に存在する（空なら `[]`）。`errors` は空でなければ必ず出力する（`--quiet` でも抑止できない）。
   `notes` は空でないかつ `--quiet` 未指定のときだけ現れる。`input` は `--verbose` のときだけ現れる。
   呼び出し側は `errors` を先に見てから `images` を読む。
2. 診断項目で空のフィールドはキーごと省略し、`null` のプレースホルダは出力しない。ただしこのフレームの来歴を示す
   3 フィールドは `--quiet` でも抑止されない：すべての画像が `path`（実際に通った内部経路 —— `wgc`、
   `printwindow`、`dwm.thumbnail`、`dwm.screen`、`bitblt.screen`、`duplication.frame`、`screen.wgc` など）、
   `scope`（`path` から導いた `window` / `desktop`）、`rect`（その経路に承諾した画面領域。測れないときだけ省略）を持つ。
   画面を読むチャネルは `requestedRect` / `capturedRect` / `clipped` / `rotation`（上の説明を参照）も同様に保持する——
   これらも位置の判断の一部なので、`--quiet` でも隠れない。
3. `code` の値は安定している：`cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`。追加はあっても改名はない。
   フレーム取得の失敗のうち「フレームが届かなかった」（`capture.frame_timeout`）と「対象ウィンドウはもう無い」
   （`capture.window_gone`）はそれぞれ専用の code を持ち、汎用の `capture.failed` に混ぜることはしない——次の
   動作が両者で違うためである（前者は少し待って再試行、後者はウィンドウを再び列挙）。
   フレーム自身のメモリ上の形状が成り立たない場合（幅または高さが 0、一辺が 16384 px を超える、行間隔が
   行長より短い、バッファが行間隔×高さより短い）は `capture.frame_invalid`（終了コード 7）で断る。
   切り出し・行の詰め直し・エンコードはいずれもこの検査を通過したフレームだけを扱う。
   「あのモニタはもうデスクトップに無い／確認のあとにその画面の形状が変わった」は `capture.monitor_changed`
   （終了コード 7）：次の一歩はモニタを再列挙して改めて承諾を取り直すことであり、チャネルを換えて運を待つこと
   ではない——別のモニタの画像は誰も承諾していないものになる。`note.capture_clipped` は `note.frame_uniform`
   と同じ品質通知で、画像はそのまま渡され、終了コードは変わらない。
4. ストリーム：既定ではすべて stdout に書き、stderr は空のまま。画像が標準出力を占有した瞬間（明示的な `--out -`、
   または出力パスを全く与えない場合）に JSON 全体が stderr へ移る。2 つのストリームが混ざることは絶対にない。
   結果のレンダリング自体が例外を投げた場合の保険的な診断でさえ、一律 stderr へ流す（その時点では画像がすでに
   stdout を占めているかどうか確認のしようがなく、推測するためにコマンドラインをもう一度解析することはしない）。
   **結果が取り決めたストリームに届かなければ失敗**である：終了コードは `8` になり、もう一方のストリームへの
   書き出しが成功しても元の値へは戻さない——呼び出し側は取り決めたストリームを読むので、そこに無ければ
   受け取っていないのと同じ。
5. `captured` は `images` の件数と等しい。ウィンドウ 1 つにつき画像 1 枚で、`--monitor all` ならモニタ 1 台につき画像 1 枚。
   **標準出力に渡せる画像は 1 回につき 1 枚だけ**：複数のターゲットにヒットしている（`--all`、または複数のモニタ）
   うえで stdout へ書くなら、一括分は確認ダイアログにも最初のフレーム取得にも到達する前に断られる
   （`cli.stdout_multiple_targets` + 終了コード 1）。1 枚も取得せず、ファイルも 1 つ書かない。判定材料は実際に
   ヒットしたターゲット数なので、`--all` が 1 つのウィンドウにしかヒットしなければ stdout は使える。PNG を 1 本の
   ストリームに頭から尾へ連結してもデコードできる画像にはならないし、`-` をファイル名の接頭辞として
   `-_1.png` のようなローカルファイルに解決することもしない。
6. `images[].path` はその一歩下に入ります。チャネルの内部で実際に通った分岐を書くので、`dwm.thumbnail` と
   `dwm.screen`（画面上に重ねウィンドウを戻してそこからコピーする退路）はここで別物になります。この退路は
   縮略図のステップそのものが失敗したときだけ通るもので、画面上の色が一色だったから通るものではありません。`images[].scope`
   は `path` から導くので、`--capture dwm --yes` が本当に退路へ落ちたとき、画像には `desktop` と書かれます。
   `images[].source` もエラーの `backend` も、書くのは**実際に走ったチャネル**である：`--capture auto` が
   フォールバックに成功した時、`source` はチェーンのうちそのフレームを出した 1 本であって `auto` ではない。
   フォールバック・チェーンが全て失敗した時、`backend` は実際に試したチャネルを並べる。ウィンドウの画像も
   モニタの画像も `source` を持つ。
7. **保存**：一括分の最終的な出力パスは最初のフレームを取る前（全画面確認ダイアログの前）に一括して確定します。
   2 つのターゲットが同じ名前に解決されると `io.output_collision`（終了コード 8）を返し、一括分は 1 枚も取得も
   書き出しもしません。呼び出し側に代わって名前を変えることも、2 枚目で 1 枚目を上書きすることもありません。
   各画像は同じディレクトリ内の一意な一時ファイルへ書き、書き切りとフラッシュ後にターゲット名へリネームするので、
   書き込み失敗時に古いファイルは空にも消えもしません。`--no-overwrite` の時は「既に有るか無いか」を置き換え不可の
   あの一回のリネームが原子的に判定します（`io.file_exists`）。競合のある事前チェックはしません。
8. 各段階の失敗診断は、その段階が実際に得た値だけを座標として添える：`target`（どのターゲットか。ウィンドウは
   `0x…` のハンドル、モニタは `DISPLAY1` のようなデバイス名）、`backend`（どのチャネルか）、`stage`（`consent` /
   `capture` / `encode` / `write` / `stdout`。解析段階のエラーにはこのフィールド自体が無い）、`hresult`
   （`0x80070005` のような生の値）、`win32`（`GetLastError` の生の値）。`message` は `--lang` に追随するがこちらは
   追随しないので、人に拒否された（`capture.access_denied` + `stage=consent`）ことと技術的なアクセス拒否
   （`capture.failed` に `hresult=0x80070005` が付く）を分けられる。バックエンドが返した HRESULT / Win32 の
   エラーコードはそのまま外へ出し、`E_FAIL` や `E_NOINTERFACE` で本物のコードを覆い隠すことはしない。黒いフレームを
   DRM だと断定することも無く、文言はいくつかの可能性を列挙するだけにとどめる。
   単色のフレームを「取得失敗」とも断定しない：画像はそのまま渡され、`note.frame_uniform`（その色・
   そのチャネル・そのターゲットを記す）が 1 条残るだけ。この色ゆえにフレームを断るのは `duplication` だけ
   で、しかも 2 条が揃ったとき限り——このフレームに present の記録が全く無い、かつ全体が一色。

## 出力パスを省略した場合（互換性の注記）

出力パスを何も与えないことと `--out -` を明示することは**同じ要求**です：画像は png として stdout に流れ、
JSON は丸ごと stderr へ移り、各診断は実際にそのステップが返したものそのままになります。

| | 以前の挙動 | 現在の挙動 |
| --- | --- | --- |
| 出力パスなしで失敗したとき | すべて `cli.missing_output` + 終了コード 1 に潰し、`images` と `notes` を空にして本当の理由を出さない（例外は `cli.stdout_multiple_targets` と承諾拒否だけ） | code も終了コードもそのまま：`match.no_window`（4）、`match.ambiguous_window`（5）、`capture.access_denied`（6）、`capture.failed`（7）、`io.write_failed`（8）、`cli.invalid_number`（1）…… |
| 出力パスなしの部分成功 | 見えない（`images` が丸ごと消える） | stdout に届けられた画像は `images` に残り、`captured` が実数を数える |
| `cli.missing_output` | 終了コード 1 | 出さなくなります。この code は一覧に残してあり、別の意味に流用されないための印です |
| 「今回、明示的な出力パスが無い」という事実 | code | `hint` としてだけ、それを本当に回避できる一手のところで出ます：暗黙 stdout で `io.write_failed` + `stage=stdout`。`--out -` と書いた人はその管道を選んだのですから、その hint は現れません |

分岐のしかた：`errors[].code`（とその `stage` / `target` / `backend` / `hresult` / `win32`）を読みます。
終了コードだけを見るのも、「`--out` を付けたかどうか」で原因を推測するのも、どちらも不可です。code は追加だけで
改名しないので、以前の挙動に合わせて書いた呼び出し側もそのまま動きます——読めなくなるのは嘘の原因ただ一つです。

## 終了コード

`0` 成功 / `1` 引数エラー / `2` 条件なし / `3` `--help` / `4` 一致ウィンドウなし / `5` 複数一致 /
`6` 対象が保護、または確認が通らなかった（人が「いいえ」、`--consent-timeout-ms` 以内に誰も応答しなかった、
ダイアログを出せない） / `7` 画面取得失敗（`--timeout-ms` の予算を使い切った場合、および**このマシンの Windows
バージョンが要求するものを用意できない場合**（`env.os_too_old` / `env.channel_unsupported`、
「[動作環境のサポート](#動作環境のサポート)」参照」を含む） / `8` ファイル書き出し失敗
（write / stdout の段階で予算を使い切った場合を含む） / `9` 内部エラー。新しい意味は番号の追加でのみ表現する。
`8` は「結果 JSON を取り決めたストリームへ届けられなかった」（stdout / stderr への書き出し失敗）もカバーする。その場合、
もう一方のストリームに補発したテキストは納入とは数えない。

終了コードと body は互いに独立した 2 つの信号であり、`2`/`3`/`4`/`5` はクラッシュではなく正常な制御フローである。
**部分成功を許す**：`--all` や `--monitor all` でいくつかの対象が失敗した場合、すでに書き出した画像は `images` に残る
（`captured` は 0 より大きくなり得る）が、終了コードは `7` になる。あるバックエンドが失敗を返す代わりに例外を
投げた場合も、無効になるのはその 1 ターゲットだけである：それまでに成功した画像は残り、フォールバックの連鎖も
`--all` の残りのターゲットも巻き添えにせず、この失敗は `capture.failed` として `errors` に載る。チャネルを替えても
変わりがないエラー——メモリ枯渇、グラフィックデバイスの喪失（`DXGI_ERROR_DEVICE_REMOVED` / `DXGI_ERROR_DEVICE_RESET` /
`DXGI_ERROR_DEVICE_HUNG`）——は 1 件ずつ試し続けるのではなく、一括分を明確に打ち切る。アクセス拒否や人の拒否は、
フォールバックを続ける理由にならない。

読み取り専用の環境照会 3 つ（`--capabilities` / `--diagnostics` / `--screens`）が使う番号は `0` と `1` だけである。`0` =
その文書を出し終えた（なかに「このマシンは古くてどの経路も使えない」と書いてあっても同じ。
**照会が成功したことと撮影が可能なことは別物**なので、環境を推測するには終了コードではなく `status` を見る）。
`1` = この使い方自体が契約に合わない（`cli.query_conflict`。「[動作環境のサポート](#動作環境のサポート)」参照）。
`4`/`5`/`6`/`7`/`8` は出さない：ウィンドウも列挙せず、確認も出さず、ファイルも書かない。

読み取り専用のウィンドウ照会 2 つ（`--list` / `--inspect`）は `0` と `1` を共通に使い、さらに `4`
（`match.no_window`、対象を 1 つ必要とする `--inspect` だけがこのコードを返す）と `5`（`match.ambiguous_window`、
選択戦略の後も複数残る）を使う。加えて **`7` は来路が 1 つだけ**ある：今回の**条件評価が途中で終わらなかった**
とき（`match.timeout` —— `--timeout-ms` の予算を `--title-regex` のバックトラッキングか、応答のないウィンドウから
タイトルを取る処理に使いきった、あるいはその段階のヘルパープロセス自体が壊れた）だ。その `7` は「この質問に
答えられなかった」の意味で、影格取得の失敗ではない。だから `hint` も照会向けの書き方になっていて、
`--capture` を切り替えても意味がないことを明言している —— この経路には切り替える取得経路が存在しない。
**`6` と `8` は絶対に出ない**：確認も出さずファイルも書かないのであり、その 2 つのコードはまさに那段の話である。
`--list` は 0 件のときも終了コード `0` である — 空の一覧が今回の回答だからだ。

## 動作環境のサポート

三つの違う数字を「Windows X 以上に対応」という一文に混ぜてはいけない。

| 層 | 値 | その値の根拠 |
| --- | --- | --- |
| 各経路の API 上の下限 | 画像なら何でも 10.0.10240 · `duplication` 10.0.9200 · `printwindow` / `dwm` 10.0.9600 · `wgc` 10.0.18362 | その経路が**実際に呼んでいる** API について Microsoft が書いている下限。エンコーダ（WinRT の `BitmapEncoder`）は 6 経路・全形式で共通。`wgc` は `IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor` を使うので、それは Windows 10 version 1903 のインターオペレーション API である —— `Windows.Graphics.Capture` 名前空間そのものが 1803 出現なのは別問題で、本ツールには「システムピッカーでユーザーに選ばせる」という退路がない |
| 本ツールが宣言する下限 | 64 ビット版 Windows 10 version 1903（ビルド 18362）以上 | 上のうち最も高い一つ。既定の経路がウィンドウ画像を本当に得るためにそれが必要だからであって、「どれか一つの経路が触れる一番古い API」ではない |
| 実測済みのバージョン | Windows 10 version 22H2（ビルド 19045）、x64 | `tests\` の実機判定はすべてこの 1 台でしか走らせていない |

10240 から 18361 まではこの exe を起動でき、撮影もできる（本ツールは経路ごとの下限で経路を絞り込み、プログラム
全体を拒否はしない）。ただし宣言上のサポート範囲外であり、実測もしていないので「動く見込み、未検証」として
扱ってください。

Windows 7 や 8 に対応するという文はここに一つもない —— **このバイナリはそれらでは読み込めない**。
`api-ms-win-core-winrt-error-l1-1-1`（`RoOriginateLanguageException`。Microsoft の文書に書いてある最小
クライアントが Windows 8.1）を静的にインポートし、ほかに `api-ms-win-core-winrt-l1-1-0` と
`api-ms-win-core-job-l2-1-0`（Windows 8）もそうだが、API Set という仕組み自体が Windows 7 に存在しない。
この三つのコントラクトは UCRT の再配布可能リストに含まれず、古いシステムに後付けもできない。
`.\tests\compat.ps1` が出荷するバイナリの実際のインポート名を照合するので、読み込み下限は推論ではなく
このファイルに関する事実である。Windows 8.1 では**読み込めて起動もできる** —— そこで効くのが下の能力
チェックだ。`Windows.Graphics.Imaging.BitmapEncoder` は Windows 10 より前に存在しないので画像が一枚も
作れない。だから「ある `BitBlt` や `DwmRegisterThumbnail` の呼び出しが Windows 7 に存在する」ことは、
このプログラムがそこで動く証拠にならない。PE ヘッダの `subsystem version 6.00` も同じで、あれは MSVC
リンカの規定値であってサポート宣言ではない。
ない。

### 呼び出し時の能力チェック

ウィンドウ列挙、出力名の計画、確認ダイアログ、ピクセル読み取りの**すべてより前**に、本ツールは
`ntdll!RtlGetVersion` から読んだビルド番号（`GetVersionEx` は使わない — あちらはマニフェストとバージョン
偽装に従って答える関数）を上の下限群と突き合わせ、こう報告する：

| コード | いつ | 終了コード | 経路を変えれば何とかなるか |
| --- | --- | --- | --- |
| `env.os_too_old` | ビルドが 10240 未満：全形式が通る唯一のエンコーダが存在しない | 7 | **ならない。** 経路の話でも対象の話でもなく、この機では画像が一枚も作れない |
| `env.channel_unsupported` | 明示指定の経路の下限が本機のビルドより上 | 7 | **なる** —— `--capture` を変えるか `auto` にする。同じ対象の再試行に意味はない。指定した経路をよそへ差し替えることはしない |
| `note.channel_unavailable` | `auto` の列のうち下限に届かない経路を除外した | 変わらない | 他の経路で成功して画像は出る。実際に写した経路は `images[].source` に入る |
| `note.os_unverifiable` | ビルド番号がまったく取得できなかった | 変わらない | 今回はバージョンによる絞り込みをしていない —— 答えが出ないことは「未対応」でも「対応」でもない |

`--verbose` は `input.osBuild` と `input.captureChain`（今回の対象種別について、この機が実際に出せる経路）を
返すので、撮影せずに能力を問い合わせられる。`--dry-run` はフレームを取らないので環境判定による失敗を返さない。

デバイス単位の能力は意図的に予測しない。デスクトップ複製に内容を出さないドライバ、Windows.Graphics.Capture を
断る機械、対話デスクトップのないセッション、メディア機能パックのない N エディション —— これらはどれも
バージョン番号には現れず、本ツールは先に推測しない。該当するステップが各自の `capture.*` コードと実際の
HRESULT を返す。

### 読み取り専用の能力照会（`--capabilities` / `--diagnostics`）

上の判定は撮影より前に効くが、実際に見えるのは発注した後だけ。この 2 コマンドは同じ判定の読み取り専用口で、
ピクセルも取らず確認ダイアログも出さずファイルも書かず、通信も環境変数読みもしない。ウィンドウ条件も不要。

```powershell
ECAPTURE.EXE --capabilities              # この機がいま通せる経路（JSON）
ECAPTURE.EXE --diagnostics               # ビルド版数 + 検証可能なビルド識別子 + 経路の状態（JSON）
ECAPTURE.EXE --capabilities -v           # probes 段を追加：各質問の生の答えと、どの API から出たか
```

三つの決まり：

* **三件事を分けて書く。** `compiled` はこのバイナリにその経路が実装されているか、`status` は本機のいまの判定（バージョン下限と画面トポロジー）が通すか、`verifiedOnThisMachine` は**本プロジェクト**がそっくりのシステムで実測したか（開発機 1 台のみ、上の表を参照）。互いに代役しない。
* **答えが出なければ出ないと言う。** 各事実はいずれも `yes` / `no` / `unknown` の三値で、`unknown` は「できる」にも「できない」にも丸めず、キーごと消えたりもしない。ビルド番号が取れなかったとき `status` はすべて `unverified` になり、`autoChainWindow` は 4 本そのまま並ぶ —— 絞っていないだけで「全部対応」ではない。
* **`available` は保証ではない。** 「このウィンドウが必ず撮れる」という意味はない。ドライバ・保護内容・HDR はこの層の外で、文末の `caveats` 配列はまさに「この報告が何を言ってないか」を並べるためにある。

| 段 | 内容 |
| --- | --- |
| `contract` / `contractVersion` | 契約版数を持つのはこの 2 つの文書だけ（いまは 1）。通常の撮影 JSON は《出力の形》どおり簡潔なままで、ここに入ったからといってトップレベルのメタ情報は増えない |
| `program` | 名称、`ECAPTURE.EXE` というファイル名そのもの（ディレクトリなし）、版数、アーキテクチャ、`buildId` |
| `os` | 本機のビルド（`known` が偽ならその group は `unknown`）、`declaredMinBuild`（対外宣言の下限）、`encoderMinBuild`、`testedMinBuild` + `testedArch`（実測した 1 台）、`matchesTestedEnvironment` |
| `session` | コンソールセッションに付しているか、リモートデスクトップか、画面トポロジーの有無と画面数、本プロセスが昇格しているか、`consentDialogExpected`（推定で、`consentDialogProbed: false` が「実際には出していない」を明言） |
| `authorization` | `yesSkips: "window-content"`、`desktopPixelsAlwaysAsk: true`、未登録パスは `desktop` 扱い、加えて内部経路登録表の全行（各行に `scope` と `consentWithoutYes` / `consentWithYes`）。《撮影の承諾と --yes》の表の機械可読版 |
| `backends` | 各経路の `compiled` / `status` / `reason` / `minBuild` / `verifiedOnThisMachine`、そしてウィンドウ対象・画面対象でそれぞれどの内部経路を通るか（`dwm` のデスクトップ退路も含む。だから `--yes` の適用範囲を読み広げられない） |
| `formats` | 各形式の `compiled` / `status` / `reason` / `minBuild` / `registered`。`registered` は常に `unknown` —— この層はエンコーダを実際に試さない（試すと「1 枚符号化して能力を探る」になり、撮影で探らないのと同じ理由に触れる）。かつて挙げたがエンコーダが無い `webp` / `ico` は `compiled: false` + `reason: "not_compiled"` に残し、推測ではなく確定した答えを渡す |
| `autoChainWindow` / `autoChainScreen` | いま試せる `auto` の列。実際の撮影時に `-v` が返す `input.captureChain` とは**同一の** `GateChannels` の出力で、`tests\capabilities.ps1` が両者を突き合わせる |
| `limits` | 1 辺の画素上限、フレーム全体のバイト上限、`--timeout-ms` の上限、隔離呼び出しの内蔵上限、WGC のフレームプール再構築回数、番号と PID の上限、`stdoutTargetsMax: 1`、JPEG 品質の範囲 |
| `privacy` | この照会がやらなかったと自己申告する項目：画面取得なし、確認表示なし、送信なし、ユーザーファイル列挙なし、環境変数読みなし、ユーザー名なし、パスなし |
| `caveats` | 安定した ASCII token。「この報告が断言していないこと」を並べる：`available_is_not_a_guarantee`、`no_capture_performed`、`no_consent_dialog_shown`、`encoder_state_not_probed`、`device_capability_not_predicted`、`consent_dialog_state_inferred_not_probed`、`subsystem_version_is_linker_default`、そして本機の状況で追加分の `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown` |

両方の文書は**同一の**判定関数（`src/EnvReport.cpp` の `BuildEnvReport`）から出る。違いは段落の取捨だけで、
`--diagnostics` は `build` 段（PE のリンク時刻・機械種別・イメージサイズ・subsystem）を常に載せ、
`--capabilities` は `--verbose` のときだけ展開する。版数も `status` も経路一覧も `limits` も同じオブジェクトなので、
互いに矛盾しうる環境情報の第二の複製は存在しない。

**ビルド識別子は検証できる**：`buildId` は `版数-アーキテクチャ-16 進のリンク時刻` で、その時刻は
`dumpbin /headers` が公開成果物から読むのと同じフィールド。読んでいるのは自プロセスがすでにメモリへ
マッピング済みの PE ヘッダで、ファイルも開かなければディレクトリも列挙しない。だからインストールパスに
ユーザー名が入っていても漏れない。PE ヘッダの `subsystem version` は事実としてだけ載せ、
`subsystem_version_is_linker_default` を添えてある —— MSVC のリンカ既定値であって対応宣言ではない。

`--capabilities` / `--diagnostics` が受けするのは `--lang`、`-v`、`-q` だけ。撮影側の選択肢（ウィンドウ条件、
`--monitor`、`--capture`、`--out` と位置引数、`--yes`、`--dry-run`、2 つの期限）と同時に指定すれば
`cli.query_conflict` + 終了コード 1 で、衝突項は一度に全部列挙し、撮影も書き込みもしない。「条件なしならヘルプ」にも
該当しない：照会それ自体が明確な意図だからだ。`-q` は照会では `caveats` 段だけを外し、`-v` は `probes` を足す。
どちらも答え自体は動かない。

この文書は最初まで ASCII（機械が読む値は翻訳しない）。同じ機で `--lang` をどれに変えても出力はバイト単位でまったく同じ。

### 構造化されたウィンドウの発見と検査（`--list` / `--inspect`）

この 2 つの照会ができる以前、呼び出し側（特に AI）が「この条件群に実際にどのウィンドウが当たるか」を知る手立ては
2 つしかなく、どちらも不適切でした。`--dry-run` は候補を**人間が読む 1 行**にまとめて `note.dry_run` に置きます
（`hwnd=0x… pid=… 1261x614+681+22 class=… title=…`）。つまりハンドル・矩形・タイトルをその文章から逆パーズする
ことになり、しかもこの文章の形は安定すると約束されたものではありません — タイトルに空白や `|` が 1 つあるだけで
パーズがずれます。もう 1 つは実際に画像を取ることですが、それには出力パスが必要になり、画面取得向けの確認
ダイアログも出ます。さらに「複数該当」をエラーにします — 撮影の連続実行では妥当な結論でも、「まず見たい」だけに
対しては筋違いです。

この 2 つが、その問いに対する読み取り専用の出口です。評価は画面取得と**まったく同じ**経路を通ります（同一オプションの
複数指定は和集合、異なるオプションは積集合、`--monitor` の画面絞り込み、`--title-regex` または `--timeout-ms` が
あるときは補助プロセスへ丸ごと委譲）。ただし画像は一切生成しません。

```powershell
ECAPTURE.EXE --list --process notepad.exe                    # 該当ウィンドウを構造化して一覧化
ECAPTURE.EXE --list --class CabinetWClass --limit 5 --offset 5
ECAPTURE.EXE --list=all --title-contains レポート            # 最小化ウィンドウも含める
ECAPTURE.EXE --inspect --hwnd 0x001A0B4C                    # 1 窓を項目ごとに検査
ECAPTURE.EXE --inspect --process notepad.exe --topmost-match # 画面取得と同じ消歧
```

ルールは 5 つ。いずれも、もう一方の做法がより悪いために存在します。

* **画素も取らなければ、聞かず、書かない。** どの画面取得経路も呼ばず、確認ダイアログも出さず、ファイルも作らず、
  通信も環境変数の読み取りもしません — 文書自身が `authorization` にそれを明記します
  （`pixelsRead: 0` / `consentDialogShown: false` / `filesWritten: false`）。対象ウィンドウに**一切触れない**ことも
  同じ約束です：復元も前面化も Z 順の変更もしません。「開いているものを見たい」が画面の様子を変えてはいけないため、
  `caveats` は `no_capture_performed` と `no_window_touched` を載せます。
* **複数該当は画面取得の歧義ではない。** `--list` はページ送りして返します（`--offset` / `--limit`、既定は 1 回
  50 件）ので、実際の総数は `pagination.matched` に書かれ、「このページが短い」が「このしかない」に読めることを
  防ぎます。0 件は正常な回答です：`windows: []` と終了コード `0` であって、`match.no_window` + `4` ではありません。
  `--inspect` は 1 つの対象を必要とするため、画面取得が使うのと**同じ**選択戦略を適用します。その後も複数残れば
  `match.ambiguous_window` + 終了コード `5` — 代わりの 1 つを選んだり、よく似た窓を先に取ったりはしません。
* **一覧はスナップショットで、古くなります。** ハンドルは再利用され、タイトルは変わり、プロセスは終わるので、ここにある
  `hwnd` / `pid` / クラス名は**長く保持できる資格証ではありません**。成功した照会は毎回 `note.window_query_stale` を
  伴い、各行の `identity` は `verificationRequired: true` / `isAuthorizationToken: false` /
  `raceWindowReducedNotEliminated: true` を書きます。あとで画面取得するときは、画素を読む前に対象の身元を再確認します
  （それが `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`）。確認ダイアログも
  画素の出所にしたがって判定されます。**`--yes` はここで何も変えません**（`authorization.yesAffectsResult: false`）—
  項目を解除もしなければ、そもそも出ない框をスキップもしません。
* **読めない項目は「読めない」と書きます。** プロセスをまたぐ質問には 3 つの結末があり、項目ごとに書かれます：
  `readable`、`denied`（この呼び出し側がシステムに拒まれた）、`failed`（聞いたが答えがなかった）。後者 2 つは
  Win32 の生のコードも添えます。読めない値はセンチネル（`0` / 空文字）**プラス**その状態であって、鍵が黙って
  消えるのではありません。管理者実行を促すこともなく、`caveats` は
  `unreadable_fields_are_not_a_prediction` を載せます。
* **表示方針は推測させず書きます。** 非表示とサイズ 0 のウィンドウは除外されます（画面取得の列挙と同じ規則）し、
  `policy` がそのまま `invisibleExcluded` / `zeroSizedExcluded` と書きます。最小化ウィンドウも既定では一覧に入らず、
  その件数は `policy.minimizedExcluded` に数えられ、`--list=all` で同じ Z 順軸に併合されます。
  「システムウィンドウ」については**何も主張しません**：Windows には「私はシステムウィンドウだ」という属性が
  存在しないため、`policy.systemWindowAssertion` は `false` です。

1 行のフィールドは次の通りです（`--inspect` は `windows` 配列を同じ形状の単一 `window` に差し替えます）：

```json
{ "contract": "windowquery", "contractVersion": 1, "query": "list",
  "program": { "version": "0.4.0" },
  "authorization": { "readOnly": true, "pixelsRead": 0, "consentDialogShown": false,
                     "filesWritten": false, "yesAffectsResult": false,
                     "identityFieldsAreNotConsent": true },
  "policy": { "invisibleExcluded": true, "zeroSizedExcluded": true, "minimizedIncluded": false,
              "minimizedExcluded": 2, "systemWindowAssertion": false, "order": "zOrder" },
  "pagination": { "offset": 0, "limit": 50, "limitDefaulted": true, "defaultLimit": 50,
                  "maxLimit": 8192, "matched": 52, "returned": 50, "truncated": true,
                  "nextOffset": 50 },
  "windows": [ { "hwnd": "0x001A0B4C", "pid": 31468, "title": "…", "class": "CabinetWClass",
                 "image": "explorer.exe",
                 "rect": { "x": 681, "y": 22, "width": 1261, "height": 614 },
                 "visible": true, "minimized": false, "zOrder": 3,
                 "readability": { "process": { "state": "readable" },
                                  "imagePath": { "state": "denied", "win32": 5 },
                                  "processStart": { "state": "readable" },
                                  "rect": { "state": "readable" } },
                 "identity": { "hwnd": "0x001A0B4C", "pid": 31468, "class": "CabinetWClass",
                               "processStartTicks": 134351142668527401,
                               "selectionNeedsRecheck": false, "verificationRequired": true,
                               "isAuthorizationToken": false,
                               "raceWindowReducedNotEliminated": true } } ],
  "caveats": [ "no_capture_performed", "no_consent_dialog_shown", "no_window_touched",
               "snapshot_expires", "identity_fields_are_not_a_token",
               "invisible_and_zero_sized_excluded", "unreadable_fields_are_not_a_prediction",
               "list_may_be_partial" ] }
```

`title` / `class` / `image` は一言一句そのまま渡します — 切り詰めも、文章への埋め込み、大文字小文字の折込みもしません。
呼び出し側はフィールドを読むのであって、文をパーズするのではありません。帰属イメージの**完全パス**は既定で書きません
（インストール先にはユーザ名が含まれがちだから）で、必要なら `--inspect=path` を明示します。`--exe` の照合はいつも
パスを読んでいるので、これを報告するかとは無関係です。`identity` が渡すのは、まさに画面取得の回が再確認する事実群
（ハンドル、PID、その PID の生成時刻、クラス名、それに「当初の条件を再実行して認めるかどうか」）なので、
`--inspect --hwnd <あのハンドル>` が記述する制約集合は画面取得が守る組と完全に同源です。列挙の時点で生成時刻が
問えなければ `unknown` と書きます — それは**実施できなかった**判定であって、0 という値ではありません。

`--list` / `--inspect` は、ウィンドウ条件・`--monitor`・`--offset` / `--limit`・`--timeout-ms`・`--yes`
（効果なし）・`--lang` / `-v` / `-q` を受けします。画面取得向けの選択肢と同時に指定すると
`cli.window_query_conflict` + 終了コード `1` です（`--out`、位置引数、`--format`、`--quality`、`--no-overwrite`、
`--capture`、`--dry-run`、`--consent-timeout-ms`、`--capabilities`、`--diagnostics`、および 2 つのウィンドウ照会の
同時指定）。選択戦略の組は入口ごとに判定します：対象を 1 つに絞るためのものなので `--inspect` では有効で、
「全て該当」を意味する `--list` とは衝突します。環境照会と同じく「条件なし = ヘルプ」には落ちません。そして
**引数として**筋が通らない回は、画面取得と同じ形（`captured: 0` / `images: []` / 同じコードの `errors[]`）を
返すので、`errors[].code` で分岐する呼び出し側のコードは分岐を増やしません。`--list` の契約は `windowquery`、
`--inspect` のそれは `windowinspect`：形も契約名も別で、フィールドは同じ組を共有します。

`--dry-run` はそのまま残る互換入口です：これまでどおり答えを `note.dry_run` に置き、これまでどおり出力パスを
必要とせず、`--list` / `--inspect` とは互いに衝突します — この 2 つに黙って置き換えられるのではありません。

### 読み取り専用のモニタ列挙（`--screens`）

これまでの「あのモニタが欲しい」には書き方がひとつしかなかった。`--monitor <n>` の n は**この回の
`EnumDisplayMonitors` 列挙での位置**であり、Windows が設定画面に書く識別番号ではなく、抜き差しや解像度変更のあとに
別のパネルを指しうる。当たりどころを間違えると、誰も承認していない画面がそのままディスクに載る。
`--screens` はこの問いをデータとして返す。

```powershell
ECAPTURE.EXE --screens                                  # 全モニタと、それぞれ複数の識別子
ECAPTURE.EXE --monitor device:DISPLAY1 --out shot.png   # このデスクトップ接続での名前で指定
ECAPTURE.EXE --monitor "id:\?\DISPLAY#GSM41A2#5&…#{…}" --out shot.png   # セッションをまたぐモニタのデバイスパス
```

他の読み取り専用照会と同じく読むだけである。ピクセルを取らず、確認ダイアログも出さず、ファイルも書かず、
通信もせず、表示設定も一切変更しない。「このモニタは何度回転しているか」を知りたいからといって
`SetDisplayConfig` を呼ぶのは、試験問題を自分で書き換えて答えを読むことに等しい。この文書は三つめの契約
（`screens`、版 1）で、`capabilities` や `windowquery` 同体、画面取得の結果 JSON とは独立している。

ひとつのモニタに四種類の識別子があり、それぞれ「どこまで安定か」をフィールドで宣言する（本文ではなく字段で）：

| フィールド | 何なのか | 安定する範囲 | 指定の形 |
| --- | --- | --- | --- |
| `ordinal` | この回の列挙での位置 | `this_invocation` | `--monitor <n>` |
| `deviceName` | GDI のビュー端末名 `\\.\DISPLAY1` | `this_desktop_attach` | `device:` |
| `monitorDevicePath` | モニタの devnode デバイスインターフェースパス | `cross_session_expected` | `id:` |
| `adapterLuid` | アダプタのローカル一意識別子 | `this_session` | なし——関連情報のみ |

`screens[].selectors` にはそのまま書き戻せる二通り（`device:DISPLAY1`、`id:\?\DISPLAY#…`）が入り、`identity.*` が
どれを指定に使えるかを書く。アダプタの LUID には意図的に指定の形を与えていない：それは今回のセッション内でしか
一意ではないので、それでモニタを名指しするのは参照ではなく賭けになる。`adapter.devicePath`（アダプタ自身の
devnode パス）、`adapter.outputTechnology`、`targetId`、`targetAvailable` は同じ問いの答えで、「どのモニタがどの
GPU についているか」はこうして返る。

この層は三つの規則を守る：

- **別のモニタに置き換えない。** 識別子がこの機に存在しない = `match.monitor_unknown_id`（終了コード 4）、
  同一の識別子が複数に当たる = `match.monitor_ambiguous_id`（5、候補をすべて列挙し、代行して 1 台は選ばない）、
  その問いが回答を返さなかった = `match.monitor_id_unverifiable`（7、`hint` は「--capture を変えるのは次の一手では
  ない」と明記する——この経路はそもそも経路を選んでいない）。三つとも「ではメインでいいか」に静かに退化することは
  ない：誰も承認していない画面全体こそ、確認ダイアログが防ごうとしているものだからだ。
- **読み取れないものは空値にしない。** `dpi`（実効値と生の値、`shcore!GetDpiForMonitor` を使用、Win8.1 以降）、
  `rotation.degrees`（人が見る向き、現在の `DEVMODE` から）と `rotation.panel`（パネル本来の向きに対する回転、
  表示設定から）は別々の質問で、`readability` も別々に（`readable` / `denied` / `failed`）、その API 自身の
  エラーコードを添える。キーが無いのは「回答が無い」ことを意味し、理由は隣に書いてある。管理者で実行せよとも
  言わない。
- **スナップショットであって認証情報ではない。** 成功した一覧は必ず `note.screen_query_stale` を添え、`caveats` には
  `device_names_are_not_persistent`、`cross_session_stability_not_tested`、`screen_capture_always_asks` が入る。
  モニタを名指ししても、影格取得前の身元再確認は省略できず、承諾も省略できない：デスクトップの画素は常に人が
  必要で、`--yes` があっても同じである（下の《撮影の承諾と --yes》参照）。

識別子による名指しは「モニタを選ぶ」場所ならどこでも効く。ウィンドウ条件と併用すれば（`--monitor id:… --class …`）
そのモニタに絞り込みになり、`--list` / `--inspect` でも同じ選択関数を使い、影格取得前の再確認では**選んだ時点で
分かった身元**で照合する——デバイス名が別パネルのものになっていれば `capture.monitor_changed` で止まり、再確認そのも
のが答えを返さなければ名前に退かずに `capture.monitor_unverifiable` で止まる。`--monitor <n>` の意味は一文字も変わって
おらず、範囲外は従来どおり `match.monitor_out_of_range` で `hint` に全モニタを列挙する。新しい二通りの書き方は
`--help` と上の引数の表記の節に書いてある。

`--screens` は環境照会の一族なので、受け付けるのは `--lang` / `-v` / `-q` だけである。ほか的一切り（`--yes` も
`--monitor` も含む）は `cli.query_conflict` + 終了コード 1 で、「条件なしならヘルプ」にも入らない。`--quiet` が
消すのは `notes` だけ：`identity`、`readability`、`authorization`、`caveats` は判拠であり気遣いではない。この文書は
デバイスパスを印字するが、ファイルシステムへのパスとユーザー名は出さない
（`privacy.includesDevicePaths: true`、`includesFileSystemPaths: false`）。

## 画面取得方式

| 値 | チャネル | 隠れたウィンドウを撮れるか | ハードウェアアクセラレーション描画 | API 上の下限 |
| --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | 撮れる（DWM のキャッシュ） | 正常 | Win10 1903（18362）—— `CreateForWindow` / `CreateForMonitor` の側で、1803 の名前空間側ではない |
| `dwm` | DwmRegisterThumbnail | 撮れる | 大半は正常、保護されたウィンドウは黒 | Win8.1（9600）—— サムネイル登録自体は古いが、読み戻しが `PrintWindow(PW_RENDERFULLCONTENT)` |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | 撮れる（ウィンドウ自身の描画） | 多くの場合まっ黒 | Win8.1（9600）、その flag の話 |
| `bitblt` | 画面 DC からの BitBlt | 撮れない、見えるピクセルだけをコピー | 一部が黒 | それ自体にバージョン下限なし |
| `duplication` | DXGI デスクトップ複製でモニタ全体のフレームを取り矩形で crop | 撮れない、見えるピクセルだけをコピー | 正常 | Win8（9200）、リモートデスクトップ/仮想 GPU は内容を得られないことが多い |
| `auto` | wgc → dwm → printwindow → bitblt の順にフォールバック | 尽力 | 尽力 | この列から、本機のバージョンでは足りない経路を引いたもの |

この列は**各経路の API 上の下限**で、その経路が実際に呼ぶ API について Microsoft が書いた文書に対応させてある。
このプログラムが動作を宣言するバージョンでも、実測したバージョンでもない：宣言下限（Win10 1903、x64）、
6 経路が共用するエンコーダの下限、実測済みバージョン（Win10 22H2 / 19045）、そして実行時に曖昧な失敗のかわりに
何を報告するかは [動作環境のサポート](#動作環境のサポート) を参照。

- 「そのウィンドウ自身の画面」が欲しいとき（別のものに覆われていても）は既定の `wgc`。「今この瞬間の画面の見た目」が
  欲しいとき（覆っているもの込みで）は `bitblt` か `duplication` を使う。
- `wgc` はウィンドウの現在の大きさに追従する。フレーム生成時に持っていたサイズだけでなく、各フレーム自身の
  内容サイズを読む。選択からフレーム取得の間にウィンドウが縮小した場合は、有効な矩形だけをコピーする
  （大きいテクスチャに残った未定義の縁を画面として扱わない）。フレーム・プールを超えて拡大した場合は
  `--timeout-ms` の予算内でプールを作り直してもう一度取る。切り取られたフレームをウィンドウ全体として
  完全だと偽って渡すことはなく、`images[].width`/`height` はその時点の実サイズになる。
- `--capture` の値を間違えたら解析時に `cli.unknown_capture_method`（終了コード 1）を返し、**既定チャネルに
  フォールバックすることはない**。フォールバックを許されるのは `auto` だけで、フォールバック成功時は `note.capture_channel` で
  実際にどのチャネルを使ったか伝える。
- DRM / 保護されたコンテンツは常に黒画面になる。ドライバ側の黒枠（一部のプレイヤー）は通るチャネルと通らないチャネルが
  あり、保証はない。
- モニタ全体取得が使えるのは `wgc` / `duplication` / `bitblt` だけ。`--monitor` に `dwm` や `printwindow` を
  組み合わせると解析時に `capture.unsupported`（終了コード 1）を返す。`auto` はモニタ全体では
  wgc → duplication → bitblt の順にフォールバックする。
- `duplication` はモニタを 3 つの意味で認識する。いずれも仮定せず報告する：
  - **回転。** ドライバが返すデスクトップのフレームは、そのモニタが実際に表示している向きと限らない。この経路は
    出力が主張する内容（`DesktopCoordinates`）と実際に得たテクスチャの形を突き合わせ、切り出しを時計回り
    0 / 90 / 180 / 270 度で回すので、渡される画像は常に、確認ダイアログが列挙した矩形と同じ座標系にある——
    二重の入れ替えは起きない。何を適用したかは `images[].rotation` に出る。どちらの形にも一致しないテクスチャは
    無理に切り出さず `capture.frame_invalid` で断る。
  - **どのグラフィックアダプタを使うか。** まず全アダプタ・全出力を列挙してその表の中で対象の位置を確かめ、
    その出力を所有しているアダプタの上に D3D11 デバイスを作る——`DuplicateOutput` が要求するのはそれである。
    したがって第 2 GPU が駆動している画面も到達可能で、「既定アダプタから順に試す」の旧来の盲点は消えている。
    この経路に WARP へのフォールバックは無い：ソフトウェア デバイスは物理出力を所有しないので、サイズだけは
    正しく報告して中身が空のフレームを返すだけになる。
  - **1 ターゲットにつき 1 出力。** 2 台のモニタにまたがる（または画面の縁からはみ出る）ウィンドウは、
    重なりが最も大きい出力と重なる部分を取得し、残りは画像に**入らない**。それ静かにウィンドウ全体であるかのように
    見せるのではなく、`capturedRect` != `requestedRect`、`clipped`、`note.capture_clipped` として現れる。
    1 つのウィンドウをアダプタまたぎで継ぎ合わせることは実装していない。
- 確認のあとに対象モニタがデスクトップから外れたり形状が変わったりしたら、取得は `capture.monitor_changed`
  （終了コード 7）で止まる——このツールは決して別のモニタで代用せず、承諾は人が見たあの 1 台に結び付いたまま。

## 撮影の承諾と --yes

実際にフレームを取得する撮影は、**信頼できるウィンドウ用チャネルを含めて**、必ずモーダルの確認ダイアログを
先に出します。ダイアログも撮影もしないのは、条件を何も付けない場合（テキストヘルプ + `2`）、`--help`、
`--version`、`--dry-run`、一致なし（`4`）、複数一致（`5`）、引数エラー（`1`）、出力名の計画失敗
（`io.output_collision` + `8`）だけです —— 一括分の出力名は人に同意を求める前に確定します。

`--yes`（`-y`、正方向のブールスイッチ：裸書きと `=true/1/yes/y/on` が有効、`=false/0/no/n/off` が無効、
重複指定は最後が効き、`-v` の `input.yes` が最終値を返す）が省略するのは**一段だけ**、つまり画像が選択した
ウィンドウ自身に縛られ、デスクトップから一切サンプルしない経路の確認です。それ以外は何も保証しません：
有効な画像も、権限も、保護コンテンツも、エラーも、上書き保護も無関係です。

| 内部経路（`images[].path`） | ピクセルの出具合 | `--yes` なし | `--yes` あり |
| --- | --- | --- | --- |
| `wgc` / `printwindow` / `dwm.thumbnail` | 選択したウィンドウだけ | 1 回聞く | 聞かない |
| `bitblt.screen` / `duplication.frame` / `dwm.screen` | 画面上のあの領域（他のウィンドウが写り込む） | 必ず聞く | **やはり必ず聞く** |
| `screen.wgc` / `screen.bitblt` / `screen.duplication` | モニタ全体 | 必ず聞く | **やはり必ず聞く** |
| 未登録の名前 | 判定できない | 必ず聞く | 必ず聞く（厳しめ側に倒す） |

- **デスクトップのピクセルを読む経路に回避口はありません**：`--yes`、`--quiet`、環境変数、stdin、呼び出し元
  どれも効かない。二段階に分けた目的はまさにここにあります。
- 承諾は**リクエストをまたいで記憶せず**、対象を拡大せず、ウィンドウの承諾をデスクトップの承諾に昇格させません。
  一括分まとめて 1 回の確認で足りるのは、ダイアログにその対象たち（各自の矩形）と展開済みの絶対パス（または
  標準出力）と実際の経路名が列挙されているときだけです。対象の領域が動いたりモニタ構成が変わったりすれば、
  その承諾は無効になります（もう一度聞きます）。
- 「いいえ」・ダイアログを閉じる・ダイアログを出せない、のいずれでも**そのリクエストの残りの撮影は止まります**。
  別のチャネルへフォールバックもせず、再試行もしません。すでに書き終わった画像はそのまま残ります。
- 「はい」と答えた後も約 1 秒待ってからフレームを取ります。ダイアログが閉じるアニメーションが画像に写るのを避ける
  ためで、ダイアログ自体は画像に写りません。
- ダイアログは「いいえ」に既定フォーカスを置き、対象とその矩形、通ろうとする実際の経路、展開済みの絶対出力パス
  （または標準出力）、他のウィンドウが写り込み得るかどうかを列挙します。経路がデスクトップを読むなら、ここで
  `--yes` は効かないと明記します。
- 対話できるデスクトップが無いとき（サービスセッション、タスクスケジューラ、ロック画面）は、`--yes` を付けた
  ウィンドウ内容の撮影はそのまま正常に終わり、デスクトップ経路は拒否されるだけです（ダイアログを出せないからと
  進行することはない）。拒否は `capture.access_denied` + 終了コード `6`、出せない場合は別の安定コード
  `capture.consent_unavailable` + 同じく `6`。どちらも `stage=consent`、`target`、`backend`、`value`（経路名）を
  運びます。承諾後に目標が動いたりサイズが変わったりした場合は `capture.consent_stale`（終了コード `7`、
  対象を選び直すと再び聞きます）。
- 人が断ったことは、そのまま「人が断った」として出ます —— `capture.access_denied`（`stage=consent`）+
  終了コード `6`。出力パスを与えるかどうかとは関係がありません。`--out` を省略するのは `--out -` と同じで、
  承諾の結果はそれで変わりません（[出力パスを省略した場合（互換性の注記）](#出力パスを省略した場合互換性の注記) 参照）。
- 素朴な `MessageBox` は**協力する自動化の誤操作防止**です。人間がクリックしたことの認証ではなく、同じ権限で
  回避を狙うプロセスに対する防御でもありません。

`--monitor`（値を省略）と `--monitor primary` は主モニタ、`--monitor 2` は 2 台目のモニタ、
`--monitor all` は全モニタを 1 枚ずつ。番号は**この回の実行における `EnumDisplayMonitors` 列挙での位置**で、
1 始まり——Windows が設定画面に書く識別番号ではなく、モニタを抜いたり解像度を変えたりすると並びは変わり得る
ので、実行をまたいで画面を識別するために番号を保存しないでほしい。同じモニタをもう一度名指ししたいなら
`--screens` を実行して、返ってきた識別子をそのまま書き戻す：`--monitor device:DISPLAY1`（この接続での
デバイス名）または `--monitor "id:\\?\DISPLAY#…"`（モニタの devnode デバイスインターフェースパス、セッションを
またいで成立するほう）。識別子がこの機に存在しなければ`match.monitor_unknown_id`（終了コード 4）、複数に当たれば
`match.monitor_ambiguous_id`（5、候補を全部列挙し代行で 1 台は選ばない）、その問いが回答を返さなければ
`match.monitor_id_unverifiable`（7）を返す。三つとも「ではメインで」に静かに退化することはない。番号が範囲外なら
従来どおり
`match.monitor_out_of_range`（終了コード 1）を返し、`hint` にこの PC の全モニタを列挙する。
画面ターゲットを取得する前に、ツールはそのモニタを**身元**で確かめ直す（選んだ時点でセッションをまたぐ識別子が
分かっていればそれで、分からなければ従来どおり名前で）：そのモニタがデスクトップから消えた場合も、そのデバイス名が
別パネルに割り当て直されていた場合も、取得は
`capture.monitor_changed` で止まる（複核そのものが答えを返さなければ `capture.monitor_unverifiable`）。矩形や位置が
変わっていれば、人に承諾を求めるのは新しい矩形である——
サイズを変えたり位置を動かしたりしたモニタに古い承諾を流用することは決してない。
`--monitor <n>` をウィンドウ条件と同時に指定すると＝モニタでウィンドウを絞り込む（ウィンドウ矩形とそのモニタに
重なりがあれば一致し、モニタをまたぐウィンドウは両方のモニタで一致扱い）で、出てくるのは引き続きウィンドウ画像、
上の表のウィンドウの行に従う。`--monitor all` はウィンドウ**一致**条件と排他（`cli.monitor_conflict`、終了コード 1）だが、
`--all` / `--index` のような区別用のオプションは一致条件に数えないので、これらと一緒に使える。
複数モニタ + stdout（`--monitor all --out -`）はダイアログを出す前に断られる。確認 1 回では「全モニタの画像
1 枚ずつを同じストリームに押し込む」ことは買えない。モニタが 1 台なら `--monitor all` は 1 ターゲットなので、
その道は引き続き 1 枚として stdout を使う。

## 対象の身元とハンドルの再利用

選んだウィンドウは、実際にピクセルを読む時点でそのウィンドウでなくなっている可能性がある。条件の列挙と画面取得の間には、出力名の計画、人工の確認ダイアログ（人が答えるまで数秒かかることもあり、答えた後にも約 1 秒の閉じるアニメーションがある）、それに `auto` のフォールバックでは最大 4 チャネルがはさまっている。その時間のあいだに対象は破棄され、その HWND の値は別のウィンドウに受け継がれ、その PID は別プロセスに再利用され得る —— 64 ビットの整数だけでは「まだそれ」と「それによく似た新しい対象」を区別できない。

そこで選んだ瞬間に身元を記録する：ハンドル、所属 PID、**そのプロセスの作成時刻**（「PID が再利用された」と「まだ同じプロセス」を分けるのはこれ）、ウィンドウクラス名、そしてそれを対象にした条件。確認は 2 段階に分けて行う：

- **各チャネルの各試行の前**（承認のあと、ピクセルを読む前にもう一度）：ハンドルはまだ有効か、まだあのプロセスに属するか、その PID のプロセス作成時刻は同じか、クラス名は一致するか。この 4 問の答えは user32 / kernel32 自身のデータ構造にあり、対象のスレッドへメッセージを送らない。だからこの頻度で聞けるし、ここで固まったウィンドウに引きずられることもない。
- **各対象を開始する前に 1 度**、さらにデスクトップのピクセルを読み始める前（`dwm` の画面への退路）は必ず：**当初の条件でもう一度評価し直し**、このハンドルがまだ候補に入っているかを見る。タイトルのような移りやすい属性はこのやり方で判定する —— アプリが自分のタイトルを更新する（再生位置、文書の変更標識、タブの見出し）のは同じ対象のまま、`--title` の条件を満たさなくなったものはそうでない。この問いはウィンドウを列挙し直すので、フォールバックの回数だけ掛け算にはしない。中断点を持たない `--title-regex` は、最初の評価と同じ隔離判定をそのまま使う（期限が指定されていれば補助プロセスで実行）。

通らなければピクセルを 1 つも読まず、次の 3 つの安定したコードを返す：

| コード | 終了コード | 意味 | 次の一手 |
| --- | --- | --- | --- |
| `capture.target_gone` | 7 | このリクエスト中にハンドルが破棄された | ウィンドウを読み直してもう一度実行する |
| `capture.target_changed` | 7 | そのハンドル値はいま別の対象に属する（あるいは当初の条件を満たさない） | 対象を選び直す。**承認した許諾は新しい対象には移らない** |
| `capture.target_unverifiable` | 7 | 判定基準の一つが答えられなかった（プロセス情報が読めない、条件の再評価が終わらない） | 実行環境を調べる（権限、ポリシー、セキュリティソフト）、または `--timeout-ms` を大きくする |

身元が変わったときに「条件を緩めて似たウィンドウを拾う」ことはしない —— `--yes` の規則と同じ根拠で、許諾は人に一覧で示した対象に結び付いている。

**保証の範囲**：この確認は競態の時間を縮めるだけで、消したとは主張しない。判定と画面取得は 1 つの原子的な操作ではなく、HWND は wait できる対象でもなく、「ウィンドウを存在し続けるように固定する」公開 API も無い。判定と 1 フレーム目の間のひとときに起きた変化はまだ起こり得る —— ただ「計画から人工確認までの長い時間」がそのために使えなくなっただけである。

## 実行期限とブロックする呼び出し（`--timeout-ms` / `--consent-timeout-ms`）

`--timeout-ms <ms>` は、この回の自動処理区間の**総予算**で、対象の選択が始まった時点から単調時計で計測します。
ウィンドウ/モニタのマッチング（`--title-regex` 含む）、`auto` のフォールバック連鎖、フレーム待ち、エンコード、
最後のコミットが、すべて**同じ**予算を使います。どの段階も、そして一括分のそれ以降のターゲットも、予算を新しく
受け取り直すことはないので、4 本のバックエンドがそれぞれ 2 秒ずつ待つことも、2 つのターゲットがそれぞれもう一度
待つこともあり得ません。指定なしまたは `0` は総予算なしを意味します。その場合も隔離実行はすべて組み込みの
5000 ms 上限で抑えられます——これはかつての `timeoutMs` 引数が果たすべきだったものです。予算を使い切ると、
影響を受ける画像は**書き出されません**——条件評価の最中に予算が尽きたら `match.timeout`（`stage=match`）、
フレーム取得とエンコードの段階なら `capture.timeout`（`stage=capture`、エンコード込み）、ファイル書き込み/
stdout の段階なら `io.timeout`（`stage=write` / `stdout`、終了コード `8`）を返し、一括分の残りターゲットは開始せず、
すでに書き終わった画像は `images` に残ります。したがって部分完了の一括は、部分的な画面取得失敗とまったく同じ
振る舞いです：終了コードは 0 以外で、すでに着地したものはそのまま納入されます。

人を待つのは**別枠**の時計です：`--consent-timeout-ms <ms>` は確認ダイアログにだけ上限を設け、自動処理の予算を
一切消費しません（人が席を外したことを「機械が遅い」とは数えない）。時間までに誰も応答しなければ、その要求は
**拒否**として扱われます——`capture.consent_timeout`、終了コード `6`——決して同意として扱うことはなく、
一括分の残りも、まさに明示的な「いいえ」の後と同じく停止します。指定なしまたは `0` は以前どおり無限に待ちます。
「はい」を押した後のおよそ 1 秒のバッファは、ダイアログが閉じるアニメーションを画像に写さないためのもので、
これは人の段階に属し、期限に間に合わせるために省略されることはありません。上限で押さえるのは「応答を待つ時間」
であって、「応答後に画面が落ち着くまでの時間」ではありません。

**ブロックが実際にどこで起きるか。** `PrintWindow` は対象ウィンドウへ描画要求を渡してそのスレッドを待ちます。
`--capture printwindow` と `dwm` の読み戻しがやっているのがまさにそれで、この呼び出しの内側に期限を照合できる
中断点はありません。`std::regex` も同様です：`(a+)+$` のようなパターンを長いタイトルに当てると数分間
バックトラックしかねず、パターンの長さ上限は実行期限にはなりません。これらの呼び出しは現在、同じ `ECAPTURE.EXE`
を補助プロセスとして起動し、解析済みの 1 タスクを秘密のパイプ経由で流して走らせます。期限が切れたら、親は
**自分自身の**その補助プロセスを止めてタイムアウトを報告します。対象アプリのウィンドウが殺されることは決してなく、
ワーカーが親より長く生きることもありません（閉じれば殺すジョブオブジェクト、パイプ切断の検査、アイドル時の
ウォッチドッグ）。そしてもっとも**変わらない**こと：補助プロセスが読むのは単一ウィンドウ自身の画面だけか最上位
ウィンドウの列挙で、デスクトップのピクセルをサンプルすることも、ファイルを書くことも決してありません。よって
あらゆるデスクトップ経路は引き続き上の承諾手続きを通ります——`--worker` というオプションは無く、`--yes` の扱いが
少しでも甘くなることもありません。

限度は限度として言い切ります：予算が効くのは割り込める地点と、補助プロセスを止めることの 2 つです。アトミックな
ファイル書き込み、誰かが読み取りを止めた stdout パイプ、取り消し要求を無視する WinRT エンコーダにはキャンセル点
がないので、これらは開始前に門番し、終了後に時間測定する——呼び出しの途中で押しのけはしません。そして
`Win10 19045` では、`PrintWindow(PW_RENDERFULLCONTENT)` は DWM のキャッシュ面から描画するだけで `WM_PRINT` を
一度も送らないため、`WM_PRINT` の中で止まったウィンドウはそこで親を詰まらせません。対象スレッドを待るのは、その
フラグなしの `PrintWindow` フォールバック呼び出しのほうです。「フリーズする筋」があらゆる Windows ビルドで通るとは
思い込まないでください——信じてよいのは、このツールが自分の期限内に復帰するということだけです。

## ファイル名プレースホルダ

`--out` のパスの中で使う。複数枚の画像はこれで区別する：

| プレースホルダ | 意味 |
| --- | --- |
| `%i` | 連番、1 始まり（`--all` の複数ウィンドウ、`--monitor all` の複数モニタ） |
| `%h` | ウィンドウハンドル、`0x001B0C48` の形。モニタ対象は 0 |
| `%p` | プロセス ID。モニタ対象は 0 |
| `%n` | ウィンドウタイトル。画面ターゲットは `\\.\` 接頭辞を除いたデバイス名（例 `DISPLAY1`）。ファイル名の断片として使えるよう整えます：不正文字は `_`、末尾のドットと空白は除く、全体が予約デバイス名（`CON` / `NUL` / `COM1` / `LPT1` など）なら `_` を接頭辞に、80 の UTF-16 コードユニットで切り詰める際にサロゲート対を割らない |
| `%d` | ローカル日付 `YYYYMMDD` |
| `%t` | ローカル時刻 `HHMMSS` |
| `%%` | リテラルの `%` を 1 つ。それ以外の `%x` は 2 文字そのままに残す |

`--all` の出力名にプレースホルダが無いと、自動的に `_連番` を追加し、`note.all_without_placeholder` を出す。
プレースホルダでターゲットを分けられない時（`%d` だけ、または同一プロセスの 2 つのウィンドウで `%p`）は黙って
改名せず、一括分を先にプランして衝突を `io.output_collision` で報告します。`%d` / `%t` は一括につき 1 回の時計を
読むので、日付をまたぐ一括でも同じ日付時刻になります。プランした名前は絶対パス同士を大文字小文字を区別せず
コード単位で比較します（NTFS の見方と同じ）。文字列比較では見えない別名（8.3 短縮名、ハードリンク、ディレクトリの
junction とシンボリックリンク、UNC とドライブ文字の二通り）は確定時のアトミックな操作に委ねるので、事前チェックが
気づけない占有も黙って上書きされることはありません。`--out -` はパスではないので展開も拡張子補完も衝突検査も無く、
しかも標準出力は 1 回に 1 枚しか渡さないため、あのストリームへ一括分の画像が並ぶことはない
（「出力の形」の規則を参照）。

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

1. **まず能力を一度照会し、それから `--list` / `--inspect` でウィンドウを発見し、最後に本番の画面取得。**
   `--capabilities` は読み取り専用で、ピクセルも取らなければ確認ダイアログも出さず、ファイルも書かない。
   だから人を邪魔しないので、自動化の先頭に置ける。このマシンのビルド・セッション・画面トポロジー、
   各経路の `available` / `unavailable` / `unverified`、各形式、`--yes` が実際に及ぶ内部経路の一覧、
   そして値の上限を一度で返す。「どの `--capture` を頼むべきか」「今回の失敗はチャネルの話か機械の話か」
   「ここで確認を出しても誰も答えないのか」が、手を動かす前に分かる。報告を投稿するなら `--diagnostics` を
   もう一度（同じ判定に加えて検証可能なビルド識別子も載せる。`-v` で各質問の生の答えも展開）。
   どちらも `--lang` で変わらない（全域 ASCII）ので、突き合わせは安定する。
   `available` は**保証ではない**：ドライバ・保護内容・HDR はその層の外で、文末の `caveats` はまさにそれを
   釘を刺すためにある。
   そのあと従来どおり `--dry-run`：フレームも取得せずファイルも書き出さず、候補は `notes[0].value` に入る：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。`--dry-run` に `--out` は
   要りません（届ける画像が無いので、結果は丸ごと stderr に出ます）。そして
   **`--dry-run` だけ・ウィンドウ条件を一切与えない = テキストのヘルプ + 終了コード 2** である。
   必要なのが行間の人向けの文章ではなく**一覧**なら、`--list`（構造化・ページ送り、複数一致はエラーではなく、
   0 件なら空の一覧 + 終了コード `0`）と `--inspect`（1 窓、複数一致は歧義のまま —— 代わりの 1 つを選ばない）
   を使う。この 2 つは画素も取らなければ確認も出さず、`--yes` は効果を持たない。返すものはスナップショットで
   ある：実際の画面取得では対象の身元を再確認するので、スクショに渡すハンドルは**たったいま作った**
   `--inspect` のものを使い、以前の回のものをキャッシュして使わない。詳しくは上の
   「構造化されたウィンドウの発見と検査」の節を参照。
2. **`errors[].code` で分岐し、`message` の文字列を突き合わせないこと**（あちらは `--lang` に追随する）。
   「`--out` を付けたかどうか」を原因にもしないこと —— 省略する道も `--out -` と同じ code を返します。
   よく出るいくつか：`match.no_window`（4、条件が狭いか対象が最小化中）、`match.ambiguous_window`（5、`hint` の候補から
   選ぶ）、`match.index_out_of_range` / `match.monitor_out_of_range`（1、`hint` に全候補を列挙してある）、
   `match.monitor_unknown_id`（4、`--screens` が前に返した識別子がいま桌面に無い）、
   `match.monitor_ambiguous_id`（5、同一識別子が複数モニタに一致、代行で 1 台は選ばない）、
   `match.monitor_id_unverifiable`（7、モニタの身元という問いが答えを返さない）があり、さらに
   `cli.monitor_selector_empty` / `cli.monitor_selector_kind`（1、`--monitor` の識別子の形）と
   `capture.monitor_unverifiable`（7、影格取得前の身元再確認が答えを返さない。名前に退いて撮ることもしない）。
   モニタを識別子で名指すにはまず `--screens` を実行して、番号を推測しないでほしい。
   `cli.invalid_format`（1）、`cli.stdout_multiple_targets`（1、複数のターゲットが
   同じ stdout を共有しようとしている）、`capture.failed`（7）、`capture.frame_timeout`（7、フレーム待ちが
   タイムアウト）、`capture.window_gone`（7、対象はもう無いので列挙し直すべき）、
   `capture.frame_invalid`（7、返ってきたフレームのメモリ形状が不正）、`capture.access_denied`（6）、
   `io.write_failed`（8、ディレクトリが存在しないか確定に失敗）、`io.file_exists`（8、`--no-overwrite` と併用したとき）、
   `io.output_collision`（8、2 つのターゲットが同じ出力名に解決され、一括分は取得も書き出しも無し）。
   このうち 2 つはその対象ではなく**このマシン**の話で、同じウィンドウの再試行に意味がない：`env.os_too_old`
   （7、本機のビルドが、全形式が通る唯一のエンコーダの下限より下 —— `--capture` を変えても良くならない）と
   `env.channel_unsupported`（7、明示した経路がもっと新しいビルドを要求する —— 経路を変えるか `auto` にするのが
   効く。指定した経路を差し替えることはしない）。`note.channel_unavailable` は `auto` の列から外れた 1 経路の
   話で、残りで成功して画像は出る。「この機が何本の経路を出せるか」を撮影前に聞くには `--verbose` の
   `input.osBuild` と `input.captureChain` を読む。各下限・宣言範囲・実測範囲は「動作環境のサポート」参照。
   各エラーにはさらに `target` / `backend` / `stage` / `hresult` / `win32` が付く（「出力の形」の規則を参照）。
   その段階で得られた分だけ書かれるので、`message` からこじ開ける必要はない。
3. **読むストリームは場合分けする**：`--out <ファイル>` を指定すれば JSON は stdout にあり stderr は空なので、
   そのままパースしてよい。`--out -` または出力パス無しでは画像バイトが stdout を占め、JSON 全体が stderr に移る。
   stdout が受け渡すのは 1 枚だけなので、複数のターゲットはファイルへ書き出すこと。PowerShell 5.1 の `2>&1` は
   stderr をエラーレコードに包装してしまうので、画像と JSON の両方が欲しければ `1>`/`2>` を分けてリダイレクトする。
4. **0 以外の終了コードを即・全滅と扱わないこと**：部分成功では `captured` は 0 より大きく終了コードは 7 だが、
   すでに書き上がった画像はそのまま使える。`images[].source` を見れば、その画像が実際にどのチャネルから出たか分かる。
5. **終了コード 0 は「画面が正しい」の意味ではない**：保護されたコンテンツや一部のプレイヤーの
   ドライバは、成功を返しながらいっしょに黒フレームを渡してくる。ツール自体が「全体が一色か
   どうか」を教える——各ピクセルを左上のピクセルとバイト単位で比べ（BGRA の 4 チャンネルすべて、
   行末の合わせ込みは対象外）、本当に一色なら `note.frame_uniform`（色は `0xAARRGGBB`）を出しつつ
   画像はいつものとおり渡す。単色は品質に関する通知であって失敗ではない。
   成功を返しながらいっしょに黒フレームを渡してくる。正しさを判定したいならピクセルを検証する——たとえば単色の
   ウィンドウを対象に覆いかぶせてから再取得し、掴んだのが対象の画面か覆ったものかを見る。最低でも
   `width`/`height` を対象ウィンドウの矩形と突き合わせる。
6. **承諾の扱い**：`--yes` を付けないと、ウィンドウ 1 枚の撮影也包括して、あらゆる実際の撮影が人が答えるまで
   プロセスを止めます。対象が 1 つのウィンドウで、経路がウィンドウ内容のまま（`wgc` / `printwindow` /
   `dwm.thumbnail`）なら `--yes` を付けてよい。`bitblt`、`duplication`、`dwm` の画面フォールバック、そして
   モニタ全体ではこのスイッチは何もしないので、撮れる範囲を先に使用者へ伝え、`--out` を明示し、**使用者本人が**
   「はい」を押すのを待つ（スクリプトや `SendMessage`、UI 自動化での代打は禁物）。後から `images[].scope` を
   読めば、その画像に他のウィンドウや開いている文書や通知が写り込む可能性があるかどうかが分かります。あるウィンドウ
   だけを撮りたいなら、対象をモニタ全体に拡張しないこと。
7. 「あるアプリケーション」を安定して掴みたいなら `--process`/`--exe` に `--class` を添えるのが優先。
   タイトル照合は大文字小文字を区別するため、言語環境をまたぐと信用できない。
8. **自動化に逃げ道を残す**：呼び出し側は `--timeout-ms`（たとえば 5000）を付けて、フリーズした対象ウィンドウに
   自分まで吊し込まれないようにする。予算を使い切ると、全体がハングする代わりに `match.timeout` /
   `capture.timeout` / `io.timeout` が返る。`--consent-timeout-ms` 以内に誰も答えなかった確認ダイアログは拒否
   （`capture.consent_timeout`、終了コード 6）。`capture.timeout` に `backend=printwindow` / `dwm` が付いていたら、
   対象の UI スレッドが詰まっている可能性が高いので、`--capture wgc` を選ぶか予算を緩める。

## ビルドとテスト

| コマンド | 用途 |
| --- | --- |
| `.\build.ps1` | Release ビルド、成果物 `build\ecapture.exe`。`-Config Debug`、`-Clean` が選べる |
| `.\tests\cli.ps1` | 出力契約の断言 518 例（`--yes` と `--no-overwrite` の各真偽表記、照会と撮影オプションの排他群を含む）+ ストリーム分離 + `--out` 省略と `--out -` の等価取り合わせ + 多言語チェック（すべて `--dry-run`、画面取得なし） |
| `.\tests\capabilities.ps1` | 能力と診断の照会（`--capabilities` / `--diagnostics`）：オフラインでは `build\ecapture-capabilities-tests.exe` を走らせ（仮の probe を注入して「画面が 1 つもない」「あるチャネルの下限ちょうだ下」「ビルド番号が取れない」「エンコーダが 1 つ登録されていない」「`--yes` の適用範囲が登録表と一致」「2 つの照会が同じ判定群を共有する」を項目ごとに判定）、実機では照会が本当にダイアログを出さない（期限そのものが判定になっている）、ファイルも残さない、`os` / `arch` / 経路の列が WMI と `--dry-run -v` の別経路の値と同源、文書が全域 ASCII で `--lang` に左右されない、ユーザー名もパスも含ない、を判定する。対話デスクトップのないセッション、それより古いビルド、エンコーダの欠落、ARM64 / Server / リモートデスクトップは本機では作れないので未検証として記録する |
| `.\scripts\check-lang.ps1` | 4 か国語のメッセージの key / プレースホルダの対応チェック。exe に本当に 4 本のリソースがコンパイル済みかも確かめる |
| `.\tests\invoker.ps1` | 共有テスト起動ラッパーのオフライン検査：argv のクォーティング、2 ストリーム同時出力、バイナリが変換されないこと、ハングした子プロセス、実行ごとの一時ディレクトリ（画面取得なし） |
| `.\tests\build-path.ps1` | ビルド経路の検査：オフライン層では一時バッチ本文が ASCII のみであること、VS 環境の読み込み失敗が cmake 実行前に報告されることを確認。実機層では中国語・空白・括弧・`%` を含むディレクトリで Release / Debug / RelWithDebInfo と `-Clean` をビルドし、`%TEMP%` を中国語のディレクトリにしてもう一度ビルドする（画面取得なし。`-OfflineOnly` でオフライン層だけ） |
| `.\tests\smoke.ps1` | 実機スモーク：自分で起動したテストウィンドウを画面取得 → PNG のサイズとピクセル内容を検証 |
| `.\tests\image.ps1` | フレーム検証：オフライン層でピクセル配置を手で組み（縦縞 / チェッカーボード / 
  alpha / 行末の合わせ込み / 上限超過と短いバッファ / 範囲外の切り出し）、実機層で単色ウィンドウの
  品質通知と来路を確認 |
| `.\tests\dup.ps1` | Desktop Duplication のマルチモニタ検査：オフライン層（`build\ecapture-dup-tests.exe`、出所は `tests\dup_state.cpp`）では 4 通りの回転を実製品の幾何判定に注入する——ピクセルの判定はテスト自身の素朴な「フレーム全体を先に回転してから切り出す」に対して行う。他に負の座標、cropped / clipped の矩形、偽の 2 アダプタ出力表（対象が第 2 アダプタにある、出力が無い、外された）、「確認のあとにあのモニタが変わった」の各ケースも通す。実機層では、ダイアログが必ず出ること（デスクトップ経路は `--yes` でスキップできない）、実画像に対する `requestedRect` / `capturedRect` / `clipped` / `rotation` の確認、モニタごとの四隅の向きプローブ、全モニタに到達できることを調べる。ディスプレイの並べ替えや回転は決して行わない：回転パネルとホット unplug の判定は、その状況をマシンが用意できない限り SKIP（「未検証」）として記録する |
| `.\tests\compat.ps1` | 動作環境サポートの検査。オフライン層（`build\ecapture-compat-tests.exe`、ソース `tests\compat_state.cpp`）は偽の Windows ビルドを実産の能力判定に関数注入する：各下限の両側を一度ずつ、明示指定の経路が下限に届かない時に差し替えないこと、`auto` の列で落ちるのはどれか、ビルドが取得できない時は一切絞り込まないこと。実機層（何も撮影しない）：探測したビルド番号が WMI 独立の答えと一致（バージョン偽装に負けていない）、エコーされた経路列がそのビルドと自己整合、`--dry-run` は環境判定で失敗しない、四言語のヘルプに下限と `env.*` の二語があること、出荷バイナリが実際に Windows 8 以降の winrt / job の API Set をインポートしていること（読み込み下限の根拠）。別の Windows バージョンを要する判定はすべて未検証として記録し、文書からの推論を実測と書き替えない |
| `.\tests\save.ps1` | 実機のファイル保存と上書き保護：`--no-overwrite` の各真偽表記を実ファイルで確認、一括分の出力名プランと衝突検出（`%p` / `%n` / `%d` / `%t` / `%%` / 未知の `%x` / 大文字小文字 / クリーニング / 切り詰め）、アトミックな確定（ターゲットが使用中、ターゲット名がディレクトリ、ディレクトリ無し、書き込み中の強制終了）、並発の上書き禁止 |
| `.\tests\channels.ps1` | 実機のチャネル比較：6 チャネル + 遮蔽対照。対象も遮蔽物も自作ウィンドウ。`bitblt` / `duplication` は承諾が必要なので `-SimulateConsent` を付けない限り未検証として記録する |
| `.\tests\consent.ps1` | 撮影の承諾と `--yes`：まず離線の状態機械 `build\ecapture-consent-tests.exe`（偽の応答器と偽のモニタ構成を注入）を走らせ、次に実機で「ダイアログが出るか」を判定する。テスト側が代打するのは常に「いいえ」だけ |
| `.\tests\isolation.ps1` | 実機のリソース分離：同名の既存プロセスは生存したまま対象にならない、2 回の並行実行が混ざらない、異常終了時は自分だけを後始末する |
| `.\tests\identity.ps1` | 対象の身元と Z 順の選択の判拠。オフライン層（`build\ecapture-identity-tests.exe`、偽の問い合わせ層を注入）：ハンドルが別プロセスに再利用された、PID は同じだが別プロセス、クラス名が変わった、当初の条件を満たさない、各問いが答えられない、そして 2 段階の確認がそれぞれ何をどの順で聞くか。実機層（自分で作ったウィンドウだけ）：健康な対象を一度も止めない、バルクの途中で対象が破棄されれば `capture.target_gone`、改名して `--title` の条件を満たさなくなれば `capture.target_changed`、改名しても条件が成立ならそのまま撮れる、`--topmost-match` / `--bottommost-match` は現在の Z 順で判定する（先に作ったが最上位帯のウィンドウが勝つ —— 「最後に作成された」の読み違いはまさにここ）。ハンドルと PID の回収は意図的に現場を作れない（他人のプロセスを終了することになる）し、確認ダイアログの区間は `-SimulateConsent` が要るので、どちらも未検証として記録し、通ったことにしない |
| `.\tests\screen.ps1` | 実機のモニタ全体テスト：確認ダイアログの挙動 + 3 本のモニタチャネル + 赤い塊の位置 + 陰性対照。`-SimulateConsent` を付けたときだけ確認ダイアログを代行クリックするので、テスト専用デスクトップでのみ使う |
| `.\tests\streams.ps1` | 実機の標準ストリームと構造化結果の信頼性：単一ターゲットの stdout 出力、複数ターゲット一括分の拒否、判定が実際のターゲット数に基づくこと、複数モニタの拒否と確認ダイアログが一切出ないこと、診断の位置特定フィールド、一括の途中で失敗しても先に成功した画像は残ること、結果が取り決めたストリームへ届けられないと 8 になること、そして `--out` 省略と `--out -` が成功 / 一致なし / 歧義 / 不正な引数 / バックエンド失敗 / 拒否 / stdout 断管の七場景で同じ機械語義を返すこと（自作ウィンドウだけを画面取得） |
| `.\tests\window_shot.bat` | 人が回す流れ：テストウィンドウをコンパイル → 全チャネルで画面取得 → 保存フォルダを開く → 自分で起動した PID だけ終了 |
| `.\scripts\mkreadme.ps1` | 各言語の `--help` の出力をそのまま使って 4 本の README のヘルプ節を再生成 |

どの実機テストも対象は自前のウィンドウです：`tests\helper\ec_window.cs` をその回の一時フォルダに
コンパイルし、テストがその PID と HWND を握るので、プロセス名で対象を探すことも名前で一括終了することも
なく、削除するのも自分の作ったフォルダだけです。`tests\harness.psm1` は共有の起動ラッパー
（argv クォーティング、2 ストリーム並行消費、待ち時間に上限、タイムアウト時は自分のプロセスツリーだけ終了）
と一時ディレクトリ／テストウィンドウの作成・後始末を持っています。`tests\invoker.ps1` はそのラッパー
自体を検証するものです。

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
