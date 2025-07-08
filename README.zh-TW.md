<div align="center">

<img src="assets/logo.png" width="128" height="128" alt="EvernightCapture 圖示">

# EvernightCapture

命令列視窗截圖工具：按條件篩出視窗，把那個視窗的畫面存成圖片檔案。

[English](README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja-JP.md)

</div>

基於 Windows.Graphics.Capture 的一整套取圖通道，入口是 `ECAPTURE.EXE`——單一的可執行檔案，靜態連結 CRT，
目標機器不需要安裝 VC++ 執行時。輸出對程式友善：除 `--help`/`--version` 外一律 JSON，退出碼穩定，
診斷帶穩定的 `code`，所以既適合人手工輸入，也適合被腳本與 AI 呼叫。

目前版本 **0.4.0**：`--capture` 的取值全部可用（`wgc` / `dwm` / `printwindow` / `bitblt` / `duplication` / `auto`），
`--monitor` 提供整張螢幕截圖與「按螢幕過濾視窗」。曾經實作過的 `magnification` 已刪除（理由見 AGENTS.md）。

## 特性

- **按條件篩視窗**：句柄 / 處理程序 ID / 映像檔案名 / 完整路徑 / 標題（精確、包含、正則）/ 視窗類名，不同選項之間是 AND、同一選項寫多次是 OR
- **六條取圖通道**：被遮擋的視窗也能截（`wgc` / `dwm` / `printwindow`），或者刻意只拷螢幕上看得見的像素（`bitblt` / `duplication`）
- **多視窗一次截完**：`--all` 每個命中視窗各存一張，搭配 `%i` 這類佔位符命名
- **整張螢幕截圖附人工確認**：`--monitor` 截整張螢幕前一定先彈出模態確認框，**沒有命令列也沒有環境變數旁路**
- **四語文案**：`zh-CN` / `zh-TW` / `en` / `ja`，預設跟隨系統顯示語言，全部編在 exe 的資源裡
- **機器讀的 JSON**：只裝擷取結果與錯誤，不含工具名、版本、schema、參數回顯之類的元資訊

## 快速開始

需要 Visual Studio（「使用 C++ 的桌面開發」工作負載）+ Windows SDK，建置腳本會自動找到它們。

```powershell
.\build.ps1                                  # Release，產物 build\ecapture.exe
ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
```

輸出目錄必須**已經存在**，工具不會建立目錄。先看看會命中誰（不截圖、不寫檔案）：

```powershell
ECAPTURE.EXE --process notepad.exe --dry-run --out D:\shots\_probe.png
```

常見用法：

```powershell
# 標題 + 類名鎖定一個視窗
ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png

# 用 dry-run 的候選清單拿到句柄後點名
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite D:\shots\one.png

# 每個命中視窗各一張，檔案名帶序號
ECAPTURE.EXE --pid 12345 --title-contains 报告 --all "D:\shots\rpt_%i.png"

# 圖片位元組走標準輸出（此時 JSON 改走 stderr）
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# 整張螢幕：一定會先彈確認框，而且沒有跳過開關
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"
```

## 選項

寫法上 `--opt=value`、`-opt`、`/opt` 都接受；取值本身以 `-` 開頭時寫成 `--title=-x`，或用 `--` 結束選項解析。
短選項**不能**合併（`-qi` 會報 `cli.unknown_option`）。下面這段是 `ECAPTURE.EXE --help` 的原樣輸出，
改動選項後執行 `.\scripts\mkreadme.ps1` 重新產生，**不要手動編輯這一段的正文**。

<!-- BEGIN ECAPTURE-HELP -->
```text
EvernightCapture (ECAPTURE.EXE) —— 按條件視窗截圖，基於 Windows.Graphics.Capture

用法: ECAPTURE.EXE [條件...] <輸出路徑>        不給任何條件 => 顯示本說明
      ECAPTURE.EXE [條件...] --out <路徑>      路徑寫 - 表示把圖片位元組輸出到標準輸出
      ECAPTURE.EXE [條件...]                   不給輸出路徑 => 圖片按 png 寫標準輸出
      ECAPTURE.EXE --monitor [n] <路徑>       給了 --monitor 且無視窗條件 => 該螢幕整幅截圖

截圖目標（不給 --monitor 就只按下面的視窗條件尋找）
  --monitor, -m [<n|primary|all>] 截圖目標螢幕的編號，由 1 開始（按顯示設定裡的順序）；primary = 主螢幕，all = 每張螢幕各一張。不給視窗條件時 = 整張螢幕截圖，給視窗條件時 = 只算與該螢幕有重疊的視窗。取值可省略（= 主螢幕），省略時不吃後面的參數，所以 --monitor out.png 仍然可用。整張螢幕截圖會先彈框徵求同意，且沒有跳過確認的開關

視窗匹配條件（同一選項多次出現取並集，不同選項必須同時命中）
  --hwnd <handle>                 視窗句柄。純數字按十進位，0x 前綴或含 a-f 按十六進位；推薦寫 0x
  --pid <pid>                     處理程序 ID，十進位且大於 0
  --process, -p <image-name>      映像檔案名（不含路徑），忽略大小寫；無副檔名時按 .exe 處理
  --exe <full-path>               映像完整路徑，忽略大小寫
  --title, -t <exact-title>       視窗標題精確匹配
  --title-contains, -T <text>     視窗標題包含子字串
  --title-regex, -R <regex>       視窗標題正則匹配，ECMAScript 語法，解析期即校驗
  --class, -c <class-name>        視窗類名，忽略大小寫，如 Notepad / CabinetWClass

匹配到多個視窗時（互斥）
  --index, -i <n>                 取第 n 個視窗，從 1 開始，按可見性/疊放次序排序
  --newest                        取最後建立的視窗
  --oldest                        取最早建立的視窗
  --all, -a                       每個匹配視窗各存一張

取圖方式（預設 wgc；受系統版本或視窗性質限制時會失敗）
  --capture, -C <method>          wgc(預設，被遮擋也能截) / dwm(DWM 縮圖，被遮擋也能截) / printwindow(視窗自繪) / bitblt(拷螢幕可見像素) / duplication(桌面複製後按矩形裁) / auto(按 wgc-dwm-printwindow-bitblt 退回；整張螢幕只用 wgc-duplication-bitblt)

輸出
  --out, -o <path|->              輸出路徑；特殊值 - 表示把圖片位元組寫到標準輸出。也可用位置參數；完全不給時等同 --out -
  --format, -f <name>             強制編碼格式；不給則由輸出檔案副檔名判定，副檔名也判不出時用 png
  --quality <1-100>               JPEG 品質，預設 100
  --no-overwrite                  目標已存在時不覆蓋，報錯退出

其他
  --dry-run, -d                   只解析並列出候選視窗，不截圖不寫檔案
  --json, -j                      已廢棄的相容開關，無副作用：成功與錯誤本來就輸出 JSON
  --verbose, -v                   JSON 中追加 input 段（正規化後的全部輸入），並保留 notes
  --quiet, -q                     省略 notes；errors 無論如何都會返回
  --lang, -l <language>           文案語言。auto(預設，跟隨系統顯示語言) / zh-CN / zh-TW / en / ja；系統語言不受支援時用 en
  --help, -h                      輸出文字說明（本段）
  --version                       輸出版本與階段

寫法: --opt=value / -opt / /opt 都接受；取值本身以 - 開頭時寫成 --title=-x，或用 -- 結束選項解析
輸出: 成功與錯誤都是 JSON，只含 captured / images（另有 errors / notes，--verbose 才有 input）
      --help / --version 以及不給條件時是文字
退出碼: 0 成功 / 1 參數錯 / 2 未給條件 / 3 --help / 4 無匹配視窗 / 5 匹配多個視窗 /
        6 目標受保護或被拒絕 / 7 截圖失敗 / 8 寫檔案失敗 / 9 內部異常
目前建置: --capture 的取值全部已實現（wgc / dwm / printwindow / bitblt / duplication，auto 按 wgc-dwm-printwindow-bitblt 退回；整張螢幕只用 wgc-duplication-bitblt）；輸出目錄必須已存在

示例:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 報告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
```
<!-- END ECAPTURE-HELP -->

## 匹配語義

不同選項之間是 AND（都要滿足才算命中同一個視窗），同一選項寫多次是 OR，不會跨視窗拼接條件。

```powershell
ECAPTURE.EXE --process notepad.exe --title-contains 报告 D:\shots\r.png
# 處理程序是 notepad.exe 且標題含"报告"的那些視窗
```

- `--title` 是整串相等、`--title-contains` 是子字串比對，兩者**區分大小寫**；`--class` / `--process` / `--exe` 忽略大小寫。
- 列舉時預設跳過不可見視窗與零尺寸視窗；**最小化的視窗截不到**，只在 `hint` 裡單獨說明。
- 命中多個又沒給消歧選項時不會隨便挑一個，而是報 `match.ambiguous_window`（退出碼 5），
  `hint` 裡按疊放次序列出全部候選。

## 輸出形式

`--help`、`--version`、以及不給任何條件時是純文字。其餘一律 JSON，只裝擷取結果與錯誤。

視窗圖（真實輸出的形狀，數值來自一次實際擷取）：

```json
{
  "captured": 1,
  "images": [
    {
      "file": "D:\\shots\\EvernightCapture - 檔案總管.png",
      "bytes": 60198,
      "width": 1247,
      "height": 607,
      "format": "png",
      "hwnd": "0x001B0C48",
      "pid": 31468,
      "title": "D:\\share\\EvernightCapture - 檔案總管",
      "class": "CabinetWClass",
      "image": "explorer.exe",
      "elapsedMs": 156
    }
  ]
}
```

螢幕圖（`--monitor` 且沒有視窗條件時）沒有視窗可歸屬，換成 `monitor` / `device` / `primary` 三個欄位，
`hwnd` / `pid` / `title` / `class` / `image` 整個不出現——呼叫端依 `monitor` 是否存在區分兩種圖。

出錯（`--hwnd` 寫了非法值）：

```json
{
  "captured": 0,
  "images": [],
  "errors": [
    {
      "code": "cli.invalid_number",
      "message": "--hwnd 需要有效的句柄值（十進位，或帶 0x 前綴的十六進位）",
      "option": "--hwnd",
      "value": "zzz",
      "hint": "純數字按十進位解析；十六進位請寫成 0x……，或含 a-f 時自動按十六進位"
    }
  ]
}
```

規則：

1. `captured` 與 `images` 恆在（空時 `[]`）；`errors` 只要非空就必須出現（`--quiet` 也抑制不掉）；
   `notes` 僅非空且未 `--quiet` 時出現；`input` 僅 `--verbose` 時出現。呼叫端先看 `errors` 再讀 `images`。
2. 診斷項裡為空的欄位整個鍵省略，不會輸出 `null` 佔位。
3. `code` 值穩定：`cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`，只增不改名。
4. 通道：預設全部寫 stdout、stderr 保持空；一旦圖片佔用標準輸出（顯式 `--out -`，或根本沒給輸出路徑），
   JSON 整體改走 stderr，兩個通道不會混流。
5. `captured` 等於 `images` 的條數；一個視窗一張圖，`--monitor all` 則一張螢幕一張圖。

## 退出碼

`0` 成功 / `1` 參數錯 / `2` 未給條件 / `3` `--help` / `4` 無匹配視窗 / `5` 匹配多個視窗 /
`6` 目標受保護或被拒絕 / `7` 截圖失敗 / `8` 寫檔案失敗 / `9` 內部異常。新增語義只會追加編號。

退出碼與 body 是兩套獨立的訊號，`2`/`3`/`4`/`5` 是正常控制流而不是當機。**允許部分成功**：`--all` 或
`--monitor all` 裡某些目標失敗時，已寫出的圖仍在 `images` 裡（`captured` 可以大於 0），但退出碼是 `7`。

## 取圖方式

| 取值 | 通道 | 能截被遮擋視窗 | 硬體加速內容 | 平台下限 |
| --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | 能（DWM 快取） | 正常 | Win10 1803+ |
| `dwm` | DwmRegisterThumbnail | 能 | 多數正常，受保護視窗是黑的 | Win7+ |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | 能（視窗自繪） | 常常全黑 | Win8.1+ |
| `bitblt` | BitBlt 螢幕 DC | 不能，只拷可見像素 | 部分黑 | 全版本 |
| `duplication` | DXGI 桌面複製取整幅螢幕影格後按矩形裁切 | 不能，只拷可見像素 | 正常 | Win8+，遠端桌面/虛擬顯示卡常拿不到內容 |
| `auto` | 按 wgc → dwm → printwindow → bitblt 退回 | 盡量 | 盡量 | — |

- 想要「那個視窗自己的畫面」（就算被別的東西蓋住）就用預設的 `wgc`；想要「螢幕上此刻的樣子」（連遮擋物一起）用
  `bitblt` 或 `duplication`。
- `--capture` 取值寫錯在解析期就報 `cli.unknown_capture_method`（退出碼 1），**不會退回成預設通道**；
  只有 `auto` 允許退回，退回成功會發出 `note.capture_channel` 說明實際用了哪條。
- DRM / 受保護內容一律是黑畫面；驅動造成的黑框（部分播放器）有的通道能過、有的不能，不保證。
- 整張螢幕截圖只走 `wgc` / `duplication` / `bitblt`；`--monitor` 配 `dwm` 或 `printwindow` 在解析期報
  `capture.unsupported`（退出碼 1）。`auto` 在螢幕模式下按 wgc → duplication → bitblt 退回。

## 整張螢幕截圖與隱私確認

給了 `--monitor` 且沒有視窗條件就是截整張螢幕，這時**一定先彈出一個模態確認框**（列出目標螢幕、走哪條通道、
圖去哪裡），只有點「是」才取影格：

- **沒有命令列旁路，也沒有環境變數旁路**。彈不出框（服務工作階段、沒有可互動的桌面）按拒絕處理。
- 答「否」或彈不出都是 `capture.access_denied` + 退出碼 `6`，不寫檔案。
- 點「是」之後會等 1 秒才取影格，避免把對話框的關閉動畫拍進圖裡；確認框本身不會出現在圖中。
- `--dry-run` 和「按螢幕過濾視窗」模式不取整張螢幕畫面，所以不彈框。
- 想區分「被人拒絕」和「路徑沒給對」就必須顯式給 `--out`：不給輸出路徑時任何失敗都收斂成
  `cli.missing_output` + 退出碼 1，真實原因不外洩。

`--monitor`（省略取值）與 `--monitor primary` 是主螢幕，`--monitor 2` 是第 2 張螢幕，`--monitor all` 每張螢幕一張。
編號按 `EnumDisplayMonitors` 的順序、從 1 起；越界報 `match.monitor_out_of_range`（退出碼 1），`hint` 裡列出本機全部螢幕。
`--monitor <n>` 與視窗條件同時給出＝按螢幕過濾視窗（視窗矩形與該螢幕有重疊即命中，跨螢幕視窗在兩張螢幕上都算），
出的仍是視窗圖，也不彈確認框。`--monitor all` 與任何視窗**匹配**條件互斥（報 `cli.monitor_conflict`，退出碼 1），
但 `--all` / `--index` 這類消歧選項不算匹配條件，可以和它搭配。

## 檔案名佔位符

用在 `--out` 的路徑裡，多張圖靠它區分：

| 佔位符 | 含義 |
| --- | --- |
| `%i` | 序號，從 1 起（`--all` 多視窗、`--monitor all` 多螢幕） |
| `%h` | 視窗句柄，形如 `0x001B0C48`；螢幕目標給 0 |
| `%p` | 處理程序 ID；螢幕目標給 0 |
| `%n` | 螢幕目標給去掉 `\\.\` 前綴的裝置名（如 `DISPLAY1`） |
| `%d` | 本地日期 `YYYYMMDD` |
| `%t` | 本地時間 `HHMMSS` |
| `%%` | 一個字面上的 `%`；其餘 `%x` 原樣保留兩個字元 |

`--all` 的輸出名裡沒有佔位符時會自動追加 `_序號`，並發出 `note.all_without_placeholder`。

## 文案語言

`--lang`（`-l`）選 `zh-CN` / `zh-TW` / `en` / `ja`，不給或給 `auto` 時用 Windows 顯示語言，判出來的結果不在這四種裡時用
`en`。取值寫得寬容：忽略大小寫、`_` 與 `-` 等價，`zh_TW` / `zh-Hant` / `cht` / `tw` 走繁體，`chs` / `cn` /
`zh-Hans` 走簡體，`jp` 走日語；寫錯在解析期報 `cli.unknown_language`（退出碼 1），不會退回成預設語言。

**只有給人看的文字會隨語言變**：診斷項的 `message` / `hint`、`--help` 全文。`code`、JSON 鍵名、取值列舉、
`0x…` 句柄、`HRESULT` 數值一律不變，呼叫端依 `code` 分支即可。文案是 exe 自帶的嵌入資源
（`resources/strings-<語言>.txt` 依語言編成四份 `RCDATA`），所以離線也能切換語言。

## 給 AI / 腳本的呼叫指南

這個工具就是為程式化呼叫設計，照下面這套約定做最省事。專案裡還附了一份教 AI 使用它的 skill：
`.agents/skills/ecapture-screenshot/`（裡有 `SKILL.md`、`references/cli-contract.md` 和一份 exe 副本）。

1. **先 `--dry-run` 探一次**，再消歧，最後真的截圖。`--dry-run` 不取影格也不寫檔案，候選在 `notes[0].value`：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。注意 `--dry-run` 仍要求給 `--out`，
   否則報 `cli.missing_output` + 1；而**只給 `--dry-run` 不給任何視窗條件 = 文字說明 + 退出碼 2**。
2. **依 `errors[].code` 分支，不要比對 `message` 文字**（那會隨 `--lang` 變）。常用的幾條：
   `match.no_window`（4，條件太窄或目標被最小化）、`match.ambiguous_window`（5，從 `hint` 的候選裡挑）、
   `match.index_out_of_range` / `match.monitor_out_of_range`（1，`hint` 列了全部候選）、
   `cli.missing_output`（1）、`cli.invalid_format`（1）、`capture.failed`（7）、`capture.access_denied`（6）、
   `io.write_failed`（8，目錄不存在）、`io.file_exists`（8，搭配 `--no-overwrite`）。
3. **讀取資料流要分情況**：給 `--out <檔案>` 時 JSON 在 stdout、stderr 是空的，直接解析就行；用 `--out -` 或沒給輸出路徑時
   圖片位元組佔了 stdout，JSON 整體改到 stderr。PowerShell 5.1 裡 `2>&1` 會把 stderr 包裝成錯誤記錄，想同時拿圖片和
   JSON 就用 `1>`/`2>` 分開重新導向。
4. **別把非 0 退出碼當成全盤失敗**：部分成功時 `captured` 大於 0 而退出碼是 7，已經寫出的圖照樣可用。
5. **退出碼 0 不等於畫面是對的**：受保護內容、某些播放器的驅動會在成功回傳的同時給你黑影格。要判斷正確性就校驗像素——
   例如把一個純色視窗蓋住目標再截，看拿到的是目標內容還是遮擋物；至少比對 `width`/`height` 與目標視窗矩形。
6. **整張螢幕要先問人**：`--monitor` 截整張螢幕會彈出模態框並擋住處理程序直到人回應，沒有旁路。自動化流程裡別把它當成「隨手
   能拿的全螢幕圖」；呼叫前告知使用者、並顯式給 `--out`。只想截某個視窗就不要升格成整張螢幕。
7. 想穩定拿到「某個應用程式」，優先使用 `--process`/`--exe` + `--class`；標題比對區分大小寫，跨語言環境不可靠。

## 建置與測試

| 命令 | 用途 |
| --- | --- |
| `.\build.ps1` | Release 建置，產物 `build\ecapture.exe`；`-Config Debug`、`-Clean` 可選 |
| `.\tests\cli.ps1` | 77 例輸出契約斷言 + 通道分離 + 多語言檢查（一律 `--dry-run`，不截圖） |
| `.\scripts\check-lang.ps1` | 四語文案的 key / 佔位符對齊檢查，並確認 exe 裡真的編進了四份資源 |
| `.\tests\invoker.ps1` | 離線檢查共用的測試程序呼叫器：argv 引號、兩條流同時輸出、二進位不被轉碼、卡死的子程序、每次執行各自的暫存目錄（不截圖） |
| `.\tests\smoke.ps1` | 實機冒煙：截自己建立的測試視窗 → 校驗 PNG 尺寸與像素內容 |
| `.\tests\channels.ps1` | 實機通道對比：六條通道 + 遮擋對照，目標與遮擋物都是自建的視窗 |
| `.\tests\isolation.ps1` | 實機資源隔離：同名的既有處理程序保持存活且不會被當成目標、並發兩輪互不串、異常退出只清理自身 |
| `.\tests\screen.ps1` | 實機整張螢幕測試：確認框行為 + 三條螢幕通道 + 紅塊定位 + 陰性對照。只有加上 `-SimulateConsent` 才會代答確認框，且只該在專門騰給測試的桌面上這麼用 |
| `.\tests\window_shot.bat` | 給人跑的批次檔：編譯測試視窗程式 → 逐通道截圖 → 開啟截圖目錄 → 只結束自己起的那個 PID |
| `.\scripts\mkreadme.ps1` | 用各語言 `--help` 的原樣輸出重新產生四份 README 的說明段 |

所有實機測試的目標視窗一律是自家的：`tests\helper\ec_window.cs` 編到本次執行的暫存目錄裡，測試握著它的
PID 與 HWND，因此既不按處理程序名去找目標、也不按處理程序名批次結束，刪除的也只有自己建立的那個目錄。
`tests\harness.psm1` 放著共用的程序呼叫器（argv 引號規則、兩條流併發消費、有期限的等待、逾時只結束自己
那棵程序樹），以及暫存目錄與測試視窗的建立與收尾；`tests\invoker.ps1` 就是用來證明這個呼叫器本身的。

`build.ps1` 用 vswhere 定位 VS，並優先使用 VS 自帶的 cmake/ninja。建置要求在 `/W4` 下零警告。
在 Git Bash 裡手動測試要先 `export MSYS2_ARG_CONV_EXCL='*'`，否則 `/help` 會被當成路徑改寫、`--out /tmp/x.png` 會被轉成怪異的路徑。

## 授權

EvernightCapture 採用 [Mulan PSL v2（木蘭寬鬆許可證，第2版）](http://license.coscl.org.cn/MulanPSL2)，
中英雙語全文見 [LICENSE](LICENSE)。

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
