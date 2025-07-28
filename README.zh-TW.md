<div align="center">

![EvernightCapture 圖示](resources/icon.ico)

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
- **截圖授權**：凡是真要取影格的截圖，連可靠的視窗通道也一樣，一定先彈出模態確認框；`--yes` 只免掉「影格綁在
  所選視窗本身、不從桌面取樣」那一層的確認——任何會拍到桌面像素的路徑一定要人回答，沒有開關能跳過
- **目標身份會覆核**：選定目標那一刻記下句柄、所屬處理程序、該處理程序的建立時間、視窗類別名與當初的選擇條件，在每一次取影格嘗試之前、
  以及人工確認回答之後各覆核一遍。目標被銷毀、句柄被另一個處理程序佔用、或者它已經不再滿足當初挑中它的條件時，交回的是
  `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`，而不是一張沒人核准過的畫面
- **兜得住的逾時**：`--timeout-ms` 是自動處理那段（條件匹配、後端重試、等影格、編碼、寫檔）共用的同一份預算，
  `--consent-timeout-ms` 單獨為確認框計時；`PrintWindow`、DWM 回讀、正則求值這些要等待別的處理程序的呼叫，
  都跑在一個工具能夠停下的輔助處理程序裡，所以卡死的目標視窗再也卡不住這個工具
- **四語文案**：`zh-CN` / `zh-TW` / `en` / `ja`，預設跟隨系統顯示語言，全部編在 exe 的資源裡
- **機器讀的 JSON**：只裝擷取結果與錯誤，不含工具名、版本、schema、參數回顯之類的元資訊
- **視窗可以不截圖就列出來看清**：`--list` 把命中的視窗列成結構化 JSON（句柄、PID、類名、標題、映像名、物理矩形、可見/最小化、Z 序，以及後續截圖要複核的身份欄位），命中多個按 `--offset` / `--limit` 分頁而不是報截圖歧義；`--inspect` 逐項查清一扇視窗，命中多扇仍算歧義而不會替你挑一個。兩條都不取畫素、不彈框、不寫檔案、不觸碰任何視窗，`--yes` 對它們沒有作用，交回的是一份被明確標註為快照的結果
- **能力可唯讀查詢**：`--capabilities` / `--diagnostics` 在不動一個像素、不彈確認框、不寫檔案、不連網的前提下
  問出這台機器現在能走哪幾條通道、`--yes` 到底管到哪一層，以及可核對的建置識別碼；「這個建置裡有這條路徑」、
  「本機現在讓不讓走」、「本專案有沒有在這種系統上實測過」是三件分開寫的事，問不出來就照實寫 `unknown`，
  也絕不靠實際截圖或實際編碼去探測能力

## 快速開始

需要 Visual Studio（「使用 C++ 的桌面開發」工作負載）+ Windows SDK，建置腳本會自動找到它們。

```powershell
.\build.ps1                                  # Release，產物 build\ecapture.exe
ECAPTURE.EXE --process notepad.exe D:\shots\epad.png   # 取影格之前會先彈一次確認框
```

輸出目錄必須**已經存在**，工具不會建立目錄。先看看會命中誰（不截圖、不寫檔案、也不彈框）：

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

# 截單一視窗又不想被問：--yes 只對「只取所選視窗畫面」那條路徑有效
ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png

# 整張螢幕：拍的是桌面像素，一定要人回答，--yes 跳不過
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
  --monitor, -m [<n|primary|all>] 截圖目標屏的編號，從 1 開始、只認十進位（本次列舉的順序，不保證等於「顯示設定」裡寫的識別號；要認螢幕請看結果裡的 device）；primary = 主螢幕，all = 每張螢幕各一張。不給視窗條件時 = 整張螢幕截圖，給視窗條件時 = 只算與該螢幕有重疊的視窗。取值可省略（= 主螢幕），省略時不吃後面的參數，所以 --monitor out.png 仍然可用。整張螢幕拍的是桌面像素，一定要先彈框問人，--yes 也跳不過；按螢幕篩選視窗出的仍是視窗圖

視窗匹配條件（同一選項多次出現取並集，不同選項必須同時命中）
  --hwnd <handle>                 視窗句柄。純數字按十進位，0x 前綴或含 a-f 按十六進位；推薦寫 0x。不接受正負號與空白，底線只能夾在兩位十六進位數字之間
  --pid <pid>                     處理程序 ID，只認十進位且大於 0
  --process, -p <image-name>      映像檔案名（不含路徑），忽略大小寫；無副檔名時按 .exe 處理
  --exe <full-path>               映像完整路徑，忽略大小寫
  --title, -t <exact-title>       視窗標題精確匹配
  --title-contains, -T <text>     視窗標題包含子字串
  --title-regex, -R <regex>       視窗標題正則匹配，ECMAScript 語法，解析期即校驗
  --class, -c <class-name>        視窗類名，忽略大小寫，如 Notepad / CabinetWClass

匹配到多個視窗時（互斥）
  --index, -i <n>                 取第 n 個視窗，從 1 開始（十進位），按可見性/疊放次序排序
  --topmost-match                 取 Z 序最靠前的相符視窗（此刻蓋在最上面的那一個；與建立時間無關）
  --bottommost-match              取 Z 序最靠後的相符視窗（此刻被壓在最下面的那一個；與建立時間無關）
  --newest                        --topmost-match 的舊名字。它選的一直是當下的 Z 序位置而不是建立時間（視窗建立時間沒有公開 API 可取），保留只為相容
  --oldest                        --bottommost-match 的舊名字。它選的一直是當下的 Z 序位置而不是建立時間，保留只為相容
  --all, -a                       每個匹配視窗各存一張

取圖方式（預設 wgc；受系統版本或視窗性質限制時會失敗）
  --capture, -C <method>          wgc(預設，被遮擋也能截) / dwm(DWM 縮圖，被遮擋也能截) / printwindow(視窗自繪) / bitblt(拷螢幕可見像素) / duplication(桌面複製後按矩形裁，會依顯示器的旋轉校正方向；只取與該目標重疊最多的一塊螢幕，沒截全時結果裡帶 capturedRect/clipped) / auto(依 wgc-dwm-printwindow-bitblt 復原；整張螢幕只用 wgc-duplication-bitblt)。只取視窗自己的畫面：wgc / printwindow / dwm 縮圖；會從螢幕上取樣：bitblt / duplication 與 dwm 的螢幕退路

截圖授權（真實截圖預設都要先彈框問一次；--yes 只免掉只取視窗畫面的那條路徑）
  --yes, -y                       跳過「只取所選視窗畫面」那條路徑的確認框。不保證目標一定有畫面，也不忽略權限、受保護內容、錯誤或覆蓋保護；任何會從螢幕上取樣的路徑（bitblt、duplication、整張螢幕任何通道、dwm 的螢幕退路）一定會彈框，這個開關跳不過。寫 --yes=false 表示明確要問

執行期限（自動處理那一段的總預算；人工確認另算，到點按拒絕處理）
  --timeout-ms <ms>               自動處理階段的總預算（毫秒）：從選定目標開始，條件求值、後端重試、取幀、編碼、寫檔共用這一份剩餘時間，任何一步都不會重新領一份完整預算。不給或寫 0 = 不設總預算，此時每次隔離呼叫仍受內建上限（5000 毫秒）約束。人工確認的等待不算在這裡，見 --consent-timeout-ms。預算用盡時那一張不落地，依階段報 match.timeout / capture.timeout / io.timeout
  --consent-timeout-ms <ms>       確認框最多等多久（毫秒）。不給或寫 0 = 一直等人回答。到點按「拒絕」處理，絕不按「預設同意」處理，報 capture.consent_timeout。這段等待單獨計時，不佔 --timeout-ms 那份自動預算；點「是」之後那約 1 秒的關閉動畫緩衝也算在這一級，不會為了趕期限而省掉

輸出
  --out, -o <path|->              輸出路徑；特殊值 - 表示把圖片位元組寫到標準輸出。也可用位置參數；完全不給時等同 --out -。整批輸出名在取影格之前一次算好，兩個目標算出同一個名字時整批報錯，不會靜默覆蓋。標準輸出一次只能交付一張影格，命中多個目標時整批報參數錯誤、一張都不截
  --format, -f <name>             強制編碼格式；不給則由輸出檔案副檔名判定，副檔名也判不出時用 png
  --quality <1-100>               JPEG 品質，十進位 1..100，預設 100
  --no-overwrite                  目標已存在時不覆蓋，報錯退出（不給取值就是禁止覆蓋）；寫 --no-overwrite=false（0 / no / n / off）取消這條禁令，=true / 1 / yes / y / on 與不給取值同義。重複給出時最後一個生效

查詢（唯讀：不截圖、不彈框、不寫檔案）
  --capabilities                  輸出本機能力報告（JSON）：版本、系統與工作階段條件、各條取圖路徑的 available / unavailable / unverified、格式與 --yes 的適用範圍。唯讀：不截圖、不彈確認框、不寫檔案，也不靠實際截圖來探測能力。available 只說明「這個建置里有這條路徑，且這次問出來的環境判準沒有擋掉它」，不保證某個視窗一定截得到。只接受 --lang / -v / -q，與任何截圖選項或輸出路徑同時給出 = cli.query_conflict + 結束代碼 1，一張都不截
  --diagnostics                   輸出診斷與版本報告（JSON）：建置版本、可核對的建置識別碼（PE 連結時間戳 + 架構 + 映像大小）、平台與後端狀態，欄位與 --capabilities 同源，不另立第二份環境資訊。預設不上傳、不擷取畫面、不列舉使用者檔案，也不輸出使用者名、環境變數與任何路徑；--verbose 追加每一問的原始答案，便於核對後再提交。互斥規則與 --capabilities 相同
  --list [<all>]                  唯讀地把滿足全部條件的頂層視窗列成 JSON（句柄 / PID / 類名 / 標題 / 映像名 / 矩形 / Z 序 / 身分約束欄位），不截圖、不彈框、不寫檔案，也不需要輸出路徑。命中多個按 --offset / --limit 分頁而不報截圖歧義，配選擇策略算衝突。取值 all = 也列最小化視窗。結果會過期，截圖時仍要複核身分
  --inspect [<path>]              唯讀地檢查同一套選擇策略定出的那一扇視窗；多匹配報 match.ambiguous_window + 退出碼 5，不替你選一個。取值 path = 也寫出歸屬映像的完整路徑（預設只寫檔案名）。讀不到的項寫欄位級 denied / failed，不建議改用管理員身分；不恢復或啟用任何視窗
  --offset <n>                    視窗查詢跳過開頭 n 個（0 起）
  --limit <n>                     視窗查詢本批最多 n 個（預設 50）

其他
  --dry-run, -d                   只解析並列出候選視窗，不截圖不寫檔案
  --json, -j                      已廢棄的相容開關，無副作用：成功與錯誤本來就輸出 JSON
  --verbose, -v                   JSON 中追加 input 段（正規化後的全部輸入），並保留 notes
  --quiet, -q                     省略 notes；errors 無論如何都會返回；與 --verbose 同時給出時按 --verbose 處理
  --lang, -l <language>           文案語言。auto(預設，跟隨系統顯示語言) / zh-CN / zh-TW / en / ja；系統語言不受支援時用 en
  --help, -h                      輸出文字說明（本段）
  --version                       輸出版本與階段

寫法: --opt=value / -opt / /opt 都接受；取值本身以 - 開頭時寫成 --title=-x，或用 -- 結束選項解析。數字取值只認十進位（--hwnd 另可按 0x 寫十六進位）
輸出: 成功與錯誤都是 JSON，只含 captured / images（另有 errors / notes，--verbose 才有 input）
      --help / --version 以及不給條件時是文字
退出碼: 0 成功 / 1 參數錯 / 2 未給條件 / 3 --help / 4 無匹配視窗 / 5 匹配多個視窗 /
        6 目標受保護或被拒絕 / 7 截圖失敗 / 8 寫檔案失敗 / 9 內部異常
目前建置: --capture 的取值全部已實現（wgc / dwm / printwindow / bitblt / duplication，auto 按 wgc-dwm-printwindow-bitblt 退回；整張螢幕只用 wgc-duplication-bitblt）；輸出目錄必須已存在
執行環境: 64 位元 Windows，聲明的最低內部版本 18362（Windows 10 版本 1903），只在內部版本 19045 上實測過；本機版本提供不了的路線在取影格、彈框之前就報 env.os_too_old / env.channel_unsupported（前者換通道也沒用），--verbose 的 input.osBuild 與 input.captureChain 回顯這一次能走哪幾條

示例:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 報告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
  ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png
  ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --consent-timeout-ms 60000 D:\shots\epad.png
  ECAPTURE.EXE --capabilities  先唯讀問一次這台機器能走哪幾條路徑，再決定 --capture 與目標條件
```
<!-- END ECAPTURE-HELP -->

## 引數寫法

每個數字選項只認它對外承諾過的那一種寫法，解析器不再自己猜進位。

- `--pid`、`--index`、`--monitor <n>`、`--quality` 與兩條期限**只認十進位**（`[0-9]+`）：不要正負號、
  不要空白、不要小數點、不要指數記法（`1e3`）、不要底線分隔、不要 `0x` 前綴，也不接受非 ASCII
  數字；區間在同一次解析裡判完（`--pid` 1..4294967295，`--index` 與 `--monitor` 1..65535，
  `--quality` 1..100，期限 0..86400000）。不合就是 `cli.invalid_number` + 退出碼 1——取值不會被強制
  轉換、回繞，也不會按另一種進位重讀（`--pid 1e3` 不會悄悄變成 483，`--hwnd -1` 不會變成 `UINT64_MAX`）。
- `--hwnd` 保留文件裡的三種寫法：純數字按十進位、`0x`/`0X` 前綴按十六進位、裸寫含 `a-f` 按十六進位
  （Spy++ 那種形式，所以 `--hwnd 1e3` 就是 `0x1e3`）。正負號、空白、超過 64 位與句柄 `0` 一律拒絕。
  底線只在十六進位寫法裡合法，而且必須夾在兩位十六進位數字之間：`0x001A_0B4C` 可以，`0x_1A`、
  `1A__0B4C`、`1A0B4C_`、`12_34` 都不行。
- `--monitor` 的取值可以省略，所以「下一個引數算不算它的取值」用的就是上面這套語法：`--monitor out.png`
  仍是「主螢幕 + 輸出到 out.png」，而 `--monitor 1e3` 是一個寫壞了的螢幕編號，會報錯而不會被改當成輸出檔名。
- 緊跟在「要吃值的選項」後面的那一條就是它的取值，哪怕它長得像另一個選項：`--title --lang ja` 找的是標題
  `--lang`。要以 `-` 開頭寫取值請用 `--title=-x`，或者用 `--` 結束選項解析（`--` 之後的引數一律按位置
  引數處理，`--` 本身丟棄）。
- 重複給同一個選項：匹配條件類是 OR（`--title A --title B`），取值類以最後一個為準（`--timeout-ms 9000
  --timeout-ms 300` 是 300），`--lang` 也一樣——`auto`（或省略取值）是**明確回到系統顯示語言**，不是保留
  上一條。取值非法的 `--lang` 報 `cli.unknown_language`，並按已經定下來的那種語言寫這條報錯。
- `--verbose` 與 `--quiet` 同時給出時按 `--verbose` 處理：notes 照常交付，並另發一條
  `note.flag_overrides_quiet` 說明原因。`errors`、以及 `images[].source` / `path` / `scope` 這些來路
  欄位任何時候都不被 `--quiet` 隱藏。

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
- 候選清單按**當下疊放次序**（Z 序）排列，最上面的在前；`--topmost-match` / `--bottommost-match` 取這個順序的第一個 / 最後一個。
  舊名字 `--newest` / `--oldest` 保留為相容別名，行為完全相同 —— 它們選的一直是 Z 序位置而不是建立時間：Windows 沒有取得視窗建立時間
  的公開 API，處理程序的啟動時間也不是視窗的建立時間。寫舊名字只多發一條 `note.deprecated_option`；同一條策略的新舊兩種寫法一起給
  （`--newest --topmost-match`）仍然算一條策略，不會被判定互斥。

## 輸出形式

`--help`、`--version`、以及不給任何條件時是純文字。其餘一律 JSON，只裝擷取結果與錯誤。

唯讀查詢那兩條是**另兩份契約**（`--capabilities` / `--diagnostics`，見[系統支援](#系統支援)）：
裝的是這台機器的環境與能力，而不是某一次截圖的結果，所以只有它們帶 `contract` 與 `contractVersion`。
這一句**不**反過來說明截圖 JSON 該加頂層元資訊——那份照舊只有 `captured` / `images`（依需要再加
`errors` / `notes` / `input`），也不會因為查詢裡有 `program.version` 就多寫一份。

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
      "source": "wgc",
      "path": "wgc",
      "scope": "window",
      "rect": {
        "x": 688,
        "y": 29,
        "width": 1247,
        "height": 607
      },
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
兩種圖都帶 `path` / `scope` / `rect`：整張螢幕那種是 `screen.wgc` / `screen.bitblt` / `screen.duplication`，
`scope` 是 `desktop`；而 `--monitor` 配視窗條件出的仍是視窗圖，`scope` 是 `window`。

會讀螢幕、並且把目標從整幅桌面影格裡裁出來的通道（`duplication`，以及 `bitblt` / `dwm` 的螢幕路徑）還會回報
那些像素實際上是從桌面哪裡拿到的：`requestedRect` 是這條通道原本打算截取的區域、`capturedRect` 是它實際截取到的
區域——兩者都是虛擬螢幕座標，所以與 `rect` 以及確認框上列出的內容對得起來。`clipped` 只在兩者不一致時
（視窗跨了兩張螢幕、或懸在邊緣之外）才出現：圖片照常交付，只是它不是整個目標，`note.capture_clipped` 那一條
會說明每一邊少了多少。`rotation` 只在桌面影格必須轉一下（順時針 90 / 180 / 270 度）才配合顯示器實際顯示的
方位時才出現；沒有 `rotation` 就表示沒有做過旋轉。只取視窗自己畫面的通道（`wgc`、`printwindow`、
`dwm.thumbnail`）天生就是完整地截目標，所以這幾個鍵一個都不會出現——鍵不存在的意思是「沒有少到任何東西」，
不是「不知道」。

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
      "hint": "純數字按十進位解析；十六進位寫成 0x……，含 a-f 時按十六進位。不要寫正負號與空白；底線只能夾在兩位十六進位數字之間（0x_1A、1A__2B 都不收）"
    }
  ]
}
```

規則：

1. `captured` 與 `images` 恆在（空時 `[]`）；`errors` 只要非空就必須出現（`--quiet` 也抑制不掉）；
   `notes` 僅非空且未 `--quiet` 時出現；`input` 僅 `--verbose` 時出現。呼叫端先看 `errors` 再讀 `images`。
2. 診斷項裡為空的欄位整個鍵省略，不會輸出 `null` 佔位。描述這一影格來路的三個欄位是唯一的例外，`--quiet`
   也抑制不掉：每張圖都帶 `path`（實際走的那條內部路徑——`wgc`、`printwindow`、`dwm.thumbnail`、
   `dwm.screen`、`bitblt.screen`、`duplication.frame`、`screen.wgc` 等）、`scope`（由 `path` 判出的
   `window` 或 `desktop`）與 `rect`（那條路徑被授權取樣的螢幕區域；量得不出來時才省略）。會讀螢幕的通道也保留
   `requestedRect` / `capturedRect` / `clipped` / `rotation`（見上文）——那些同樣是定位判據，所以 `--quiet`
   也不會把它們藏起來。
3. `code` 值穩定：`cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`，只增不改名。
   取影格失敗裡「影格逾時」（`capture.frame_timeout`）與「視窗已經沒了」（`capture.window_gone`）各有自己的碼，
   不再和一般的 `capture.failed` 混在一起——兩者的下一步動作不同（前者可以等一會兒重試，後者要重新列舉）。
   影格自己的記憶體形狀說不通（寬高為 0、單邊超過 16384 像素、行距裝不下一行像素、緩衝區比行距×高還短）時
   給 `capture.frame_invalid`（退出碼 7）；裁剪、行重排與編碼都先核這一道的，壞影格不會被往下搬。
   「那張螢幕已經不在桌面上／它的畫面在確認之後變了」就是 `capture.monitor_changed`（退出碼 7）：下一步是重新
   列舉螢幕並重新確認，不是換一條通道碰碰運氣——換一張螢幕等於交出一張沒人批准過的圖。`note.capture_clipped`
   是一條品質提示，和 `note.frame_uniform` 一樣：圖片照常交付，退出碼不變。
4. 通道：預設全部寫 stdout、stderr 保持空；一旦圖片佔用標準輸出（顯式 `--out -`，或根本沒給輸出路徑），
   JSON 整體改走 stderr，兩個通道不會混流。連渲染結果本身都出異常時的兜底診斷也一律走 stderr（那時
   無法確定圖片是否已經佔了 stdout）。**結果送不到約定那條串流就是失敗**：退出碼變成 `8`，即使另一條串流
   寫成功也不改回原來的值——呼叫端依約定串流讀取，讀不到就是沒拿到。
5. `captured` 等於 `images` 的條數；一個視窗一張圖，`--monitor all` 則一張螢幕一張圖。
   **標準輸出一次只能交付一張影格**：命中多個目標（`--all` 或多張螢幕）又要寫 stdout 時，整批在彈確認框和
   取第一張影格之前就被拒（`cli.stdout_multiple_targets` + 退出碼 1），一張都不截、一個檔案都不寫。判據是
   實際命中的目標數，所以 `--all` 只命中一個視窗時照樣可以寫 stdout。多張 PNG 首尾接在同一條串流上不是一幅
   可解碼的影像，工具也不會把 `-` 當檔案名前綴算出 `-_1.png` 那種本機檔案。
6. `images[].source` 與錯誤裡的 `backend` 寫的都是**真實那條通道**：`--capture auto` 退回成功時 `source`
   是鏈上那一條而不是 `auto`；退回鏈全失敗時 `backend` 列出實際試過的幾條。視窗圖與螢幕圖都帶這個欄位。
   `images[].path` 比它更細：一條通道可能含好幾條路徑，授權是按實際走的那條判的，不是按通道名判——
   `dwm.thumbnail` 取的是視窗自己的畫面，`dwm.screen`（同一條通道的螢幕退路）取的是螢幕。那條退路只在縮圖
   這一步真的失敗時才走，**不會因為畫面剛好是單色就走** —— 純色視窗照樣是視窗的畫面。
7. **儲存**：整批最終輸出路徑在取第一張影格之前（也在任何確認框之前）一次算好。兩個目標算出同一個名字時報
   `io.output_collision`（退出碼 8），整批一張都不截、一個檔案都不寫 —— 既不替呼叫端改名，也不讓第二張蓋掉第一張。
   每張圖先寫進目標目錄下唯一的暫存檔案，寫完並刷新之後才改名為目標名稱，所以寫入失敗不會清空也不會刪掉舊檔案。
   `--no-overwrite` 時「目標在不在」由那一次不許替換的改名當場判定（`io.file_exists`），不做有競態的預檢。
8. 每一步的失敗診斷還帶著它自己的座標，只在這一步真拿到了值時才出現：`target`（哪個目標，視窗是
   `0x…` 句柄、螢幕是裝置名）、`backend`（哪條通道）、`stage`（`consent` / `capture` / `encode` / `write` /
   `stdout`，解析期的錯誤沒有這個欄位）、`hresult`（`0x80070005` 這樣的原值）、`win32`（`GetLastError` 的原值）。
   `message` 隨 `--lang` 變，這幾個不變。授權這一關的錯自成一類：`capture.access_denied` 是有人在確認框上答了
   「否」或是把框關掉，`capture.consent_unavailable` 是這台機器根本沒有可互動的桌面、框彈不出來——兩者都是
   退出碼 `6`，都帶 `stage=consent`、`target`、`backend`（通道）與 `value`（那條路徑），也都不是技術性的存取被拒
   （`capture.failed` 帶 `hresult=0x80070005`），這樣才分得開。確認之後目標挪了位置或變了大小給
   `capture.consent_stale`，`stage=capture`、退出碼 `7`，重新選目標再截就會再問一次。後端回傳的 HRESULT / Win32
   錯誤碼會原樣帶出，不會被 `E_FAIL` 或 `E_NOINTERFACE` 頂掉真實的錯誤碼。黑影格不會被斷言成 DRM——文案只列出幾種可能。
單色影格也不會被斷言成「沒截到」：圖照常交付，另外留一條 `note.frame_uniform`（把那個顏色、那條通道、
那個目標寫清楚）。只有 `duplication` 還會因為單色拒絕一影格，而且必須兩條一起成立——這一個影格沒有任何
present 記錄，且整幅只有一個顏色。

## 不給輸出路徑（相容性說明）

沒給輸出路徑與顯式寫 `--out -` 是**同一個請求**：圖片按 png 走 stdout，JSON 整份走 stderr，而每條診斷都是
那一步真實產生的那一條。

| | 舊行為 | 現在的行為 |
| --- | --- | --- |
| 沒給輸出路徑時失敗 | 整段換成一條 `cli.missing_output` + 退出碼 1，`images` 與 `notes` 清空、真實原因不外洩（例外只有 `cli.stdout_multiple_targets` 與確認被拒） | 原樣的 code 與原樣的退出碼：`match.no_window`（4）、`match.ambiguous_window`（5）、`capture.access_denied`（6）、`capture.failed`（7）、`io.write_failed`（8）、`cli.invalid_number`（1）…… |
| 沒給輸出路徑時的部分成功 | 看不見（`images` 被整段丟掉） | 已經送到 stdout 的圖仍在 `images` 裡，`captured` 照實計數 |
| `cli.missing_output` | 退出碼 1 | 不再發出。這條 code 仍留在清單裡，為的是它不被挪作別的含義 |
| 「這次沒給輸出路徑」這句話 | 一個 code | 只在「補上檔名確實繞得開這次故障」的那一條上作為 `hint` 出現：隱式 stdout 且 `io.write_failed` + `stage=stdout`。寫 `--out -` 的人本來就選定了這條管道，所以那句 hint 不出現 |

怎麼分支：讀 `errors[].code`（以及它的 `stage` / `target` / `backend` / `hresult` / `win32`），
既不要只看退出碼，也不要根據「有沒有給 `--out`」推斷原因。code 只追加不改名，所以按舊行為寫出來的呼叫端
仍然能用——它只是再也讀不到一條假原因。

## 退出碼

`0` 成功 / `1` 參數錯 / `2` 未給條件 / `3` `--help` / `4` 無匹配視窗 / `5` 匹配多個視窗 /
`6` 目標受保護、在確認框上被答「否」、在 `--consent-timeout-ms` 內沒人回答、或框根本彈不出來 /
`7` 截圖失敗，含 `--timeout-ms` 預算用盡，**也含這一台機器的 Windows 版本給不出所要求的東西**
（`env.os_too_old` / `env.channel_unsupported`，見[系統支援](#系統支援)） / `8` 寫檔案失敗，含在寫檔案或標準輸出階段預算用盡 / `9` 內部異常。
新增語義只會追加編號。
`8` 也涵蓋「結果 JSON 送不到約定那條串流」（寫 stdout / stderr 失敗），那種情況下另一條串流上補發的文字不算交付。

退出碼與 body 是兩套獨立的訊號，`2`/`3`/`4`/`5` 是正常控制流而不是當機。**允許部分成功**：`--all` 或
`--monitor all` 裡某些目標失敗時，已寫出的圖仍在 `images` 裡（`captured` 可以大於 0），但退出碼是 `7`。
某個後端崩了（拋出例外而不是回傳失敗）也只作廢它所在的那一個目標：前面的圖留著，這條失敗以 `capture.failed`
帶在 `errors` 裡。記憶體耗盡、顯示裝置被移除（`DXGI_ERROR_DEVICE_REMOVED` / `_RESET` / `_HUNG`）這類換後端
也不會有分別的錯誤會明確終止整批，而不是一條條試下去。存取被拒不是繼續退回的理由，被人拒絕也不是：一旦有人答
「否」（或這個工作階段根本彈不出框），本次請求剩下的目標一律不再嘗試——不換後端、不重試、也不再問第二遍，
之前已經完成的圖全部留在 `images` 裡。

唯讀查詢那兩條（`--capabilities` / `--diagnostics`）只用 `0` 與 `1` 兩個編號：`0` = 這份文件出完了（哪怕裡面寫著
「這台機器版本太低、哪幾條都不可用」——**查詢成功與截圖能成是兩件事**，呼叫端依 `status` 分支，而不是依退出碼猜環境）；
`1` = 這次用法不合契約（`cli.query_conflict`，見[系統支援](#系統支援)）。它們不產生 `4`/`5`/`6`/`7`/`8`：
一個視窗都沒列舉、一個框都沒彈、一個檔案都沒寫。

只讀的視窗查詢那兩條（`--list` / `--inspect`）共用 `0` 與 `1`，並另外使用 `4`（`match.no_window`，只可能出自
`--inspect`，它需要一個目標）與 `5`（`match.ambiguous_window`，選擇策略之後仍剩多扇），再加**只有一條來路的 `7`**：
這一次的**條件求值自己沒跑完**（`match.timeout` —— `--timeout-ms` 的預算花在 `--title-regex` 的迴溯或向掛起的
視窗取標題那一步，或那一步的輔助程序自己壞了）。那條 `7` 說的是「這一問沒能問完」，與取影格無關，所以它的 `hint`
也是查詢自己的說法，明寫換 `--capture` 沒有用 —— 這一路根本沒有通道可換。**`6` 與 `8` 絕不出現**：一個框都沒彈、
一個檔案都沒寫，而那兩條說的正是這兩段事。`--list` 在一個都沒命中時退出碼仍是 `0` —— 空列表
就是這一次的答案。

## 系統支援

三個不同的數字不能混成一句「支援 Windows X 以上」：

| 層次 | 取值 | 這個數是從哪來的 |
| --- | --- | --- |
| 各條路徑的 API 歷史下限 | 任何一張影像 10.0.10240 · `duplication` 10.0.9200 · `printwindow` / `dwm` 10.0.9600 · `wgc` 10.0.18362 | 微軟為這條路徑**實際呼叫**的那個介面所寫的下限。編碼器（WinRT `BitmapEncoder`）六條通道、五種格式共用；`wgc` 走的是 `IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor`，那是 Windows 10 版本 1903 才有的互通操作介面——`Windows.Graphics.Capture` 這個命名空間本身確實是 1803 出現的，但本工具沒有「讓使用者在系統選取器裡點一下」那條退路 |
| 本工具聲明的下限 | 64 位元 Windows 10 版本 1903（內部版本 18362）或更新 | 上面那幾道裡最高的那一條，因為預設通道要真能截到視窗需要它；不是「某條路徑碰到的最老的那個 API」 |
| 已實測的版本 | Windows 10 版本 22H2（內部版本 19045），x64 | `tests\` 下所有真機判準都只跑在這一台機器上 |

10240 到 18361 之間的那些版本可以裝載這個 exe，也可以截圖（工具是按各條路徑各自的下限篩通道，而不是整體拒開），
但那既不在宣告支援之列、也從來沒有實測過，請按「預期可用、未驗證」對待。

這裡沒有任何一句話聲稱支援 Windows 7 或 8 —— **這個二進位檔在那上面根本裝載不了**。它匯入
`api-ms-win-core-winrt-error-l1-1-1`（`RoOriginateLanguageException`，微軟那份文件寫的最低客戶端就是
Windows 8.1），另有 `api-ms-win-core-winrt-l1-1-0` 與 `api-ms-win-core-job-l2-1-0`（Windows 8），
而 API Set 這套機制在 Windows 7 上根本不存在；這三個契約都不在 UCRT 可再分發的那一份清單裡，補不到舊系統上。
`.\tests\compat.ps1` 會拿發布版二進位檔核對這些名字，所以「裝載下限」是關於這個檔案的事實，不是推測。
Windows 8.1 **可以**裝載也可以啟動 —— 在那上面起作用的正是下面那道能力檢查：`Windows.Graphics.Imaging.BitmapEncoder`
在 Windows 10 之前不存在，一張圖都編不出來。所以「某個 `BitBlt` 或 `DwmRegisterThumbnail` 呼叫在 Windows 7 上
存在」證明不了這個程式能在那上面跑；PE 頭裡那個 `subsystem version 6.00` 也一樣，那是 MSVC 連結器的預設值，
不是支援宣告。

### 呼叫時的能力檢查

在列舉視窗、規劃輸出名、彈確認框、讀任何一個像素**之前**，工具會拿從 `ntdll!RtlGetVersion` 讀到的內部版本
（絕不用 `GetVersionEx`——那個函式依應用程式清單與版本偽裝回覆）與上面那幾道下限比對，並給出：

| 碼 | 什麼時候 | 退出碼 | 換通道有沒有用 |
| --- | --- | --- | --- |
| `env.os_too_old` | 版本低於 10240：所有格式共用那唯一一套編碼器不在 | 7 | **沒有。** 這不是通道的問題，也不是目標的問題——這台機器上做不出任何一張影像 |
| `env.channel_unsupported` | 顯式指定的那條通道的下限高於本機版本 | 7 | **有**——換 `--capture` 取值或改用 `auto`。重試同一個目標沒有意義，而工具不會自己把你指定的那條換成別的 |
| `note.channel_unavailable` | `auto` 鏈裡某條通道的下限高於本機版本，它已從鏈中移除 | 不變 | 影像仍可能由別的那幾條截到，`images[].source` 寫的是實際出圖的那條 |
| `note.os_unverifiable` | 內部版本压根問不出來 | 不變 | 這一次沒有按版本篩過任何一條——問不出來既不等於不支援，也不等於支援 |

`--verbose` 會回顯 `input.osBuild` 與 `input.captureChain`（針對本次這類目標，這台機器實際給得出哪幾條通道），
所以呼叫端（含 AI）不必先截圖就能把能力問出來。`--dry-run` 不取影格，因此不因環境判準報錯。

裝置層面的能力刻意不去預測：驅動不餵桌面複製影格、這台機器拒絕 Windows.Graphics.Capture、工作階段裡沒有可互動
桌面、N 版缺媒體元件——這些都不在版本號裡，也都不由本工具提前猜；那一步會交回它自己的 `capture.*` 碼與
真實 HRESULT。

### 唯讀的能力查詢（`--capabilities` / `--diagnostics`）

上面那套判準在截圖**開始之前**就會起作用，但只有真的下單一次之後才看得見。這兩條命令是同一套判準的唯讀出口：一條像素都不取、一個確認框都不彈、一個檔案都不寫、不連網、不讀環境變數，也不需要視窗條件。

```powershell
ECAPTURE.EXE --capabilities              # 這台機器現在能走哪幾條路線（JSON）
ECAPTURE.EXE --diagnostics               # 建置版本 + 可核對的建置識別碼 + 後端狀態（JSON）
ECAPTURE.EXE --capabilities -v           # 另加 probes 段：每一問的原始答案與它是從哪個 API 問來的
```

三條規矩：

* **三件事分開寫。** `compiled` 說的是這個二進位檔裡有沒有實現那條路線；`status` 說的是本機現在的判準（版本下限 + 螢幕拓撲）讓不讓走；`verifiedOnThisMachine` 說的是**本專案**有沒有在一模一樣的系統上實測過（只有開發機那一台，見上表）。三者互不冒充。
* **問不出來就說問不出來。** 每一條事實都是 `yes` / `no` / `unknown` 三值之一，`unknown` 既不折成「能」也不折成「不能」，那個鍵也不會整個消失。版本問不出來時所有 `status` 都是 `unverified`，而 `autoChainWindow` 仍原樣列出全部四條——沒篩就是沒篩，不是「都支援」。
* **`available` 不是保證。** 它不含「某個視窗這一次一定截得到」那層意思：驅動、受保護內容、HDR 都不在這層的斷言裡。文件末尾的 `caveats` 陣列就是把「這份報告沒說過什麼」逐條列出來。

| 段 | 內容 |
| --- | --- |
| `contract` / `contractVersion` | 只有這兩份文件帶契約版本（目前是 1）。普通截圖 JSON 仍照《輸出形式》保持精簡，不因此多出任何頂層元資訊 |
| `program` | 名稱、`ECAPTURE.EXE` 這個檔案名本身（不含目錄）、版本、架構、`buildId` |
| `os` | 本機內部版本（`known` 為假時那一組數字寫成 `unknown`）、`declaredMinBuild`（對外聲明的下限）、`encoderMinBuild`、`testedMinBuild` + `testedArch`（本專案實測過的那一台）、`matchesTestedEnvironment` |
| `session` | 是否接在控制台工作階段、是否遠端桌面、有沒有可用的螢幕拓撲與有幾塊螢幕、本行程是否被提升過、`consentDialogExpected`（推出來的，`consentDialogProbed: false` 明說沒有真去彈框） |
| `authorization` | `yesSkips: "window-content"`、`desktopPixelsAlwaysAsk: true`、未登記的路徑按 `desktop` 處理，外加整份內部路徑登記表（每條帶 `scope` 與 `consentWithoutYes` / `consentWithYes`）——就是《截圖授權與 --yes》那張表的機器可讀版本 |
| `backends` | 每條路線：`compiled` / `status` / `reason` / `minBuild` / `verifiedOnThisMachine`，以及它在視窗目標與螢幕目標上各走哪條內部路徑（`dwm` 那條螢幕退路也在，所以 `--yes` 的適用範圍不會被人讀大） |
| `formats` | 每種格式：`compiled` / `status` / `reason` / `minBuild` / `registered`。`registered` 恆為 `unknown`，因為這層不去實測編碼器（實測就是「用一次編碼來探能力」，與「不靠截屏探測」是同條理由）；曾經列過但沒有編碼器的 `webp` / `ico` 以 `compiled: false` + `reason: "not_compiled"` 留在這裡，好讓呼叫方拿到確定答案 |
| `autoChainWindow` / `autoChainScreen` | 本機現在能試的 `auto` 鏈。與截圖那次 `-v` 回顯的 `input.captureChain` 由**同一個** `GateChannels` 算出，`tests\capabilities.ps1` 逐條比對這兩處 |
| `limits` | 單邊像素上限、整影格位元組上限、`--timeout-ms` 上限、隔離開呼叫內建上限、WGC 影格池重建次數、編號與 PID 上限、`stdoutTargetsMax: 1`、JPEG 品質區間 |
| `privacy` | 自述這份查詢沒做的事：不擷取畫面、不彈框、不上傳、不列舉使用者檔案、不讀環境變數、不含使用者名、不含路徑 |
| `caveats` | 穩定的 ASCII token，列「這份報告沒有斷言什麼」：`available_is_not_a_guarantee`、`no_capture_performed`、`no_consent_dialog_shown`、`encoder_state_not_probed`、`device_capability_not_predicted`、`consent_dialog_state_inferred_not_probed`、`subsystem_version_is_linker_default`，以及依本機情況追加的 `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown` |

兩份文件由**同一個**判準函式（`src/EnvReport.cpp` 的 `BuildEnvReport`）算出，只差段落取捨：`--diagnostics` 固定帶 `build` 那一段（PE 連結時間戳、機器類型、映像大小、子系統），`--capabilities` 只在 `--verbose` 時展開它。版本號、`status`、後端清單、`limits` 都是同一份，所以不存在「兩份會互相打臉的環境資訊」。

**建置識別碼是可核對的**：`buildId` 是 `版本-架構-十六進位連結時間戳`，那一個時間戳與 `dumpbin /headers` 讀發布產物讀到的是同一個欄位，讀的是本行程自己已經對映進記憶體的那份 PE 標頭——不開檔案、不列舉目錄，所以也不會因為安裝路徑裡有使用者名而漏出身份。PE 標頭裡的 `subsystem version` 只作為事實列出，並配一條 `subsystem_version_is_linker_default` 的 caveat：那是 MSVC 連結器預設值，不是支援聲明。

`--capabilities` / `--diagnostics` 只接受 `--lang`、`-v`、`-q`：截圖那一套選項（視窗條件、`--monitor`、`--capture`、`--out` 與位置引數、`--yes`、`--dry-run`、兩條期限）與它們**同時給出就是 `cli.query_conflict` + 退出碼 1**，一次報全所有衝突項，一張都不截、一個檔案都不寫。這兩條命令也不參加「沒給條件就顯示說明」那一條：查詢本身就是明確的意圖。`-q` 對查詢只去掉 `caveats` 那一段，`-v` 加的是 `probes`，兩邊都不動答案本身。

這份文件從頭到尾是 ASCII（機器讀的取值一律不翻譯），所以同一台機器上換任何一種 `--lang`，輸出逐位元組相同。

### 結構化的視窗發現與檢查（`--list` / `--inspect`）

在這兩條命令之前，調用方（尤其是 AI）想知道"這批條件到底命中了哪些視窗"只有兩條路，而兩條都不對：
`--dry-run` 把每個候選寫成**一行給人看的話**塞在 `note.dry_run` 裡
（`hwnd=0x… pid=… 1261x614+681+22 class=… title=…`），要用的就得從這句裡把句柄、矩形、標題再解析出來 ——
而工具從沒承諾過這句話的形狀穩定，標題裡有一個空格或一個 `|` 就解析錯位。另一條是真去截一張圖：它要求一個輸出
路徑、會按截圖那一級彈確認框，還會把"命中多扇"報成錯誤 —— 那對"我要截一批圖"是合理的結論，對"我先看一眼"
完全是幫倒忙。

這兩條命令就是這個問題的唯讀出口。它們跑的是與截圖**同一套**條件求值（同一選項寫多次取並集、不同選項取交集、
`--monitor` 按屏過濾，用了 `--title-regex` 或 `--timeout-ms` 時照舊整步進輔助程序），但不產任何圖片：

```powershell
ECAPTURE.EXE --list --process notepad.exe                    # 每個命中的視窗，結構化
ECAPTURE.EXE --list --class CabinetWClass --limit 5 --offset 5
ECAPTURE.EXE --list=all --title-contains 報告                # 把最小化的也列進去
ECAPTURE.EXE --inspect --hwnd 0x001A0B4C                     # 一扇視窗，逐項查清楚
ECAPTURE.EXE --inspect --process notepad.exe --topmost-match  # 與截圖完全同一套消歧
```

五條規矩，每一條都因為另一條做法更壞：

* **不取像素、不問人、不寫檔案。** 不調任何取幀通道，不彈確認框，不建檔案，不聯網，不讀環境變量 —— 文檔自己在
  `authorization` 段寫着（`pixelsRead: 0`、`consentDialogShown: false`、`filesWritten: false`）。它同樣**不動任何
  目標視窗**：不恢復、不激活、不改疊放次序 —— "我先看一眼開着什麼"不該改變屏幕上的樣子。`caveats` 裡的
  `no_capture_performed` 與 `no_window_touched` 就是釘這一條。
* **命中多扇不是截圖歧義。** `--list` 把它們分頁交回（`--offset` / `--limit`，本批預設 50 條），真實總數寫在
  `pagination.matched`，於是"這一頁很短"永遠不會被讀成"只有這些視窗"。一個都沒命中是正常答覆：`windows: []` +
  退出碼 `0`，不是 `match.no_window` + `4`。`--inspect` 需要一個目標，用的正是截圖那一條選擇策略：策略之後仍剩多扇
  就是 `match.ambiguous_window` + 退出碼 `5` —— 不替你選一個，也不會"先拿一扇看起來一樣的"。
* **列表是一份快照，會過期。** 句柄會被複用、標題會變、處理程序會退出，所以這裡的 `hwnd` / `pid` / 類名**不是**一種可以
  長期持有的憑證。每次成功的查詢都帶一條 `note.window_query_stale`，而每一行的 `identity` 段寫着
  `verificationRequired: true`、`isAuthorizationToken: false`、`raceWindowReducedNotEliminated: true`。真去截圖時
  仍在讀像素之前複核目標身份（那是 `capture.target_gone` / `capture.target_changed` /
  `capture.target_unverifiable`），確認框也照舊按像素來源判：**`--yes` 在這裡不起任何作用**
  （`authorization.yesAffectsResult: false`）—— 它既不會多解鎖一個欄位，也不會跳過一次本就不彈的框。
* **讀不到的欄位會說它讀不到。** 跨處理程序的問答有三種下場，逐欄位寫：`readable`、`denied`（系統擋下了這個調用方）、
  `failed`（問過而沒答案），後者帶原始 Win32 碼。讀不到的值是哨兵（`0` / 空串）**加上**這個狀態，不是把鍵悄悄省掉；
  文檔也不勸你改用管理員身份 —— `caveats` 裡寫着 `unreadable_fields_are_not_a_prediction`。
* **可見性策略寫出來，不讓調用方猜。** 不可見與零尺寸的視窗被排除（與截圖那一次枚舉同一條規則），`policy` 段就這麼
  說（`invisibleExcluded`、`zeroSizedExcluded`）；最小化視窗預設也不進列表，條數記在
  `policy.minimizedExcluded`，`--list=all` 把它們按同一根 Z 序軸並進來。這裡對"系統視窗"**不作任何斷言**：
  Windows 沒有一個"我是系統視窗"的屬性可問，所以 `policy.systemWindowAssertion` 是 `false`。

每行的欄位如下（`--inspect` 把 `windows` 數組換成單個 `window` 對象，其餘欄位完全同一形狀）：

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

`title`、`class`、`image` 逐字交付 —— 不截斷、不拼進一句人話、不折疊大小寫 —— 調用方讀欄位，不該再去解析一段句子。
歸屬映像的**完整路徑**預設不寫，因為安裝路徑裡常含用戶名；要它得顯式寫 `--inspect=path`。匹配 `--exe` 本來就一直
讀得到路徑，與報告裡交不交這件事無關。`identity` 交出的正是截圖那一次要複核的幾件事（句柄、PID、該 PID 的創建
時間、類名，外加"要不要靠重跑當初那份條件來認它"），所以 `--inspect --hwnd <那個句柄>` 描述的約束集與截圖會堅持
的那一套完全同源。枚舉當時問不到創建時間就寫 `unknown` —— 那是一次**沒做出來**的判定，不是一個等於 0 的值。

`--list` / `--inspect` 接受視窗條件、`--monitor`、`--offset` / `--limit`、`--timeout-ms`、`--yes`（不起作用）與
`--lang` / `-v` / `-q`。與截圖那一級的選項一起給出是 `cli.window_query_conflict` + 退出碼 `1`（`--out`、位置參數、
`--format`、`--quality`、`--no-overwrite`、`--capture`、`--dry-run`、`--consent-timeout-ms`、`--capabilities`、
`--diagnostics`，以及兩條視窗查詢同時給出）。選擇策略那一組按入口分別判：它們是用來把目標收窄到一扇的，所以對
`--inspect` 有效，而 `--list` 說的本來就是"全部命中"，配它算衝突。與環境查詢一樣，這兩條不會掉進"無條件 = 幫助"；
而**參數本身**說不通時交回的形狀仍是截圖那一份（`captured: 0`、`images: []`、`errors[]` 用同一批碼），調用方按
`errors[].code` 分支的那段代碼不必分叉。`--list` 那份契約叫 `windowquery`，`--inspect` 那份叫 `windowinspect`：
形狀不同、契約名不同，欄位共用同一套。

`--dry-run` 原樣保留，仍是那個兼容入口：它照舊把答案寫在 `note.dry_run` 裡，照舊不需要輸出路徑，也與
`--list` / `--inspect` 互爲衝突 —— 而不是被這兩條悄悄替換掉。

## 取圖方式

| 取值 | 通道 | 能截被遮擋視窗 | 硬體加速內容 | API 歷史下限 |
| --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | 能（DWM 快取） | 正常 | Win10 1903（18362）——是 `CreateForWindow` / `CreateForMonitor` 那條互通操作介面，不是 1803 那個命名空間 |
| `dwm` | DwmRegisterThumbnail | 能 | 多數正常，受保護視窗是黑的 | Win8.1（9600）——註冊縮圖更早，但讀回靠 `PrintWindow(PW_RENDERFULLCONTENT)` |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | 能（視窗自繪） | 常常全黑 | Win8.1（9600），指那個 flag |
| `bitblt` | BitBlt 螢幕 DC | 不能，只拷可見像素 | 部分黑 | 本身沒有版本門檻 |
| `duplication` | DXGI 桌面複製取整幅螢幕影格後按矩形裁切 | 不能，只拷可見像素 | 正常 | Win8（9200），遠端桌面/虛擬顯示卡常拿不到內容 |
| `auto` | 按 wgc → dwm → printwindow → bitblt 退回 | 盡量 | 盡量 | 這條鏈減去本機版本擋掉的那幾條 |

那一列是**各條路徑的 API 歷史下限**，逐條對到微軟為該路徑實際呼叫的那個介面所寫的文件。它們既不是這個程式
宣告能跑的版本，也不是實測過的版本：宣告下限（Win10 1903，x64）、六條通道共用的那道編碼器下限、真正實測過的
版本（Win10 22H2 / 19045），以及執行時會把模糊失敗換成哪一條清楚的報告，都見[系統支援](#系統支援)。

- 想要「那個視窗自己的畫面」（就算被別的東西蓋住）就用預設的 `wgc`；想要「螢幕上此刻的樣子」（連遮擋物一起）用
  `bitblt` 或 `duplication`。
- `wgc` 跟著視窗的即時尺寸：它讀每一幀自帶的內容尺寸，而不是只看建影格池那一刻擷取項的尺寸。視窗在「選到它」與
  「取到幀」之間被縮小時，只複製那塊有效矩形（不會把較大紋理裡多出來的未定義邊緣當成畫面）；被放大到超過影格池時，
  會在 `--timeout-ms` 預算內重建影格池再取一幀。絕不交一張被裁掉卻按整窗宣稱完整的圖，`images[].width`/`height`
  就是那一刻的實際尺寸。
- 要不要問人，取決於這條路徑實際從哪裡取像素，而不是取決於你敲的通道名——見下一節。
- `--capture` 取值寫錯在解析期就報 `cli.unknown_capture_method`（退出碼 1），**不會退回成預設通道**；
  只有 `auto` 允許退回，退回成功會發出 `note.capture_channel` 說明實際用了哪條。
- DRM / 受保護內容一律是黑畫面；驅動造成的黑框（部分播放器）有的通道能過、有的不能，不保證。
- 整張螢幕截圖只走 `wgc` / `duplication` / `bitblt`；`--monitor` 配 `dwm` 或 `printwindow` 在解析期報
  `capture.unsupported`（退出碼 1）。`auto` 在螢幕模式下按 wgc → duplication → bitblt 退回。
- `duplication` 在螢幕這塊上是三重感知的，而且全部照實回報而不是假設：
  - **旋轉。**驅動交回來的桌面影格不一定處於該顯示器實際顯示的方位。這條路徑會比對該輸出自己宣稱的矩形
    （`DesktopCoordinates`）與實際拿到的紋理，再把裁剪順時針轉 0 / 90 / 180 / 270 度，所以交付的圖片永遠和
    確認框列出的那些矩形在同一個座標空間——絕不會轉兩次。實際做了多少旋轉會顯示在 `images[].rotation` 裡。
    跟兩種形狀都對不上的紋理會被以 `capture.frame_invalid` 拒絕，而不是照裁不誤。
  - **哪張顯示卡。**先列舉全部配接器與輸出、在那張表裡定位目標，然後**在擁有該輸出的那個配接器上**建立
    D3D11 裝置——這是 `DuplicateOutput` 的要求。由第二張 GPU 驅動的螢幕因此也截得到，舊實作「預設配接器優先」
    的盲點沒有了。這條路徑沒有 WARP 退路：軟體裝置不擁有任何實體輸出，那樣做只會交回一張內容為空的影格、
    尺寸卻報得對。
  - **一個目標只取一塊輸出。**跨了兩張螢幕（或懸出邊緣）的視窗，只在與它重疊最多的那塊輸出上、於重疊處截取；
    剩下的部分*不會*在圖裡。這會表現為 `capturedRect` != `requestedRect`、`clipped` 與 `note.capture_clipped`，
    而不會靜默地看起來像整個視窗。把一個視窗跨配接器拼成一張圖這件事沒有實作。
- 目標螢幕在確認之後離開了桌面或變了形狀，截圖以 `capture.monitor_changed`（退出碼 7）停止——工具絕不會拿
  另一張螢幕頂替，授權也始終綁在那個人親眼看過的那張上。

## 截圖授權與 --yes

凡是真要取影格的截圖，都先彈出一個模態確認框，**可靠的視窗通道也一樣**。不彈框也不截的只有這些：沒給任何條件
（文字說明 + `2`）、`--help`、`--version`、`--dry-run`、無匹配（`4`）、匹配多個（`5`）、解析期參數錯（`1`）、
以及輸出名規劃失敗（例如 `io.output_collision` + `8`）——整批名字在任何一次發問之前就算完了。

`--yes`（`-y`，正向布林開關：裸寫或 `=true/1/yes/y/on` 是開，`=false/0/no/n/off` 是關，重複給出時最後一個生效，
`-v` 的 `input.yes` 回顯最終結果）只免掉**一層**確認：影格綁在所選視窗本身、絕不從桌面取樣的那些路徑。
它別的一概不保證：不保證圖是有效的、不忽略權限、不管受保護內容、不管錯誤、也不管覆蓋保護。
決定屬於哪一層的是實際走的那條路徑，不是通道名：

| 路徑（`images[].path`） | 像素從哪裡來 | 不給 `--yes` | 給了 `--yes` |
| --- | --- | --- | --- |
| `wgc`、`printwindow`、`dwm.thumbnail` | 只有所選視窗自己 | 問一次 | 不問 |
| `dwm.screen`、`bitblt.screen`、`duplication.frame` | 那個視窗所在的那塊螢幕區域 | 要問 | **照樣要問** |
| `screen.wgc`、`screen.bitblt`、`screen.duplication` | 整張螢幕 | 要問 | **照樣要問** |

判不出來或沒登記的路徑一律按桌面路徑處理，所以新增通道忘了登記只會更嚴不會更鬆。`--monitor` 與視窗條件同時
給出時篩的是**視窗**，出的仍是視窗圖，所以按上面視窗那兩行走。

- **桌面路徑沒有任何旁路**：`--yes`、`--quiet`、環境變數、stdin、呼叫者是誰，都跳不過它——分層就是為了這件事。
- 一次確認可以覆蓋本次請求裡明確列出的那一批目標，所以多條後端、多個視窗不會各問一遍。但授權絕不跨請求快取，
  也不會擴大到框上沒列出的目標，「同意截視窗畫面」更不等於「同意截桌面」：`--capture auto` 配 `--yes` 可以不打擾人
  地走完視窗那幾條，可一旦進入桌面路徑就必須再問一次。
- 一旦有人答「否」、框被關掉、或者根本沒有可互動的桌面，本次請求剩下的截圖就停了：不換後端、不重試、不再問第二遍，
  已經完成的圖留在 `images` 裡。
- 目標區域挪動過、或者螢幕拓撲變了，覆蓋它的授權當場作廢並重新問一次；已經綁在舊區域上的那一影格給
  `capture.consent_stale`（退出碼 `7`，重新選目標再截）。
- 確認框的預設焦點在「否」上，內容列出目標及其區域、將要走的那條路徑、每張圖展開後的絕對路徑（或「標準輸出」）、
  以及畫面裡會不會混進別的視窗；這條路徑會拍到桌面時，框上還明寫著「你給的 `--yes` 對它不生效」。框在取影格之前
  就已經關閉，所以不會出現在圖中；點「是」之後工具仍要等約 1 秒，因為關閉動畫還留在 DWM 的畫面上。
- 沒有可互動桌面時（服務工作階段、排程工作、鎖屏），帶 `--yes` 的視窗內容截圖照舊正常完成，而桌面路徑只能被拒絕——
  絕不會因為「彈不出框」就放行。答「否」給 `capture.access_denied` + `6`，彈不出框給新的穩定碼
  `capture.consent_unavailable` + `6`；兩者都帶 `stage=consent`、`target`、`backend`，`value` 寫的是那條路徑。
- 被人拒絕就是被人拒絕：`capture.access_denied`（`stage=consent`）+ 退出碼 `6`，與有沒有給輸出路徑無關。
  省略 `--out` 就等於 `--out -`，授權結果不因它而變，見[不給輸出路徑（相容性說明）](#不給輸出路徑相容性說明)。
- 多張螢幕 + 寫 stdout（`--monitor all --out -`）在任何彈框之前就被拒：一次確認換不來「每張螢幕一張圖擠進同一條串流」。
  只有一張螢幕時 `--monitor all` 是一個目標，那條路仍然按單張走 stdout。
- 說清楚邊界：這就是一個 `MessageBox`。它是給合作式自動化（人或 AI）準備的誤點防護，既證明不了按下按鈕的是人，
  也擋不住同一個權限等級裡存心要繞過的處理程序。它能保證的是：照這套規矩跑的呼叫端，一定會被問上這一次。

`--monitor`（省略取值）與 `--monitor primary` 是主螢幕，`--monitor 2` 是第 2 張螢幕，`--monitor all` 每張螢幕一張。
編號是**本次執行 `EnumDisplayMonitors` 列舉中的位置**，從 1 起——它不是「顯示設定」裡 Windows 寫的那個識別號，
而且拔掉一張螢幕或改一次解析度都可能把它重新排過，所以不要把編號存下來當作跨執行的螢幕識別。要再次認出同一張
螢幕時，用 `images[].device`（`\\.\DISPLAY1` 那種形狀的裝置名）。越界報 `match.monitor_out_of_range`（退出碼 1），
`hint` 裡列出本機全部螢幕。螢幕目標在截圖之前，工具會按名稱重新核對那張螢幕：它如果已經離開桌面，截圖以
`capture.monitor_changed` 停止；它的矩形或位置如果變了，要人批准的是那個新矩形——一份舊確認絕不會被拿去用在
一張變過大小或搬過位置的螢幕上。
`--monitor <n>` 與視窗條件同時給出＝按螢幕過濾視窗（視窗矩形與該螢幕有重疊即命中，跨螢幕視窗在兩張螢幕上都算），
出的仍是視窗圖，所以按上面視窗那兩行的規矩授權。`--monitor all` 與任何視窗**匹配**條件互斥
（報 `cli.monitor_conflict`，退出碼 1），但 `--all` / `--index` 這類消歧選項不算匹配條件，可以和它搭配。

## 目標身份與句柄重複使用

選定的視窗在真正讀取像素之前可能已經不是那一扇了：條件求值與取影格之間隔著檔案名規劃、人工確認框（人可能想幾秒才點，點完還有約 1 秒關閉動畫）、以及 `auto` 退回鏈最多四條通道。這段時間裡目標可能被銷毀，它的 HWND 值可能被另一扇視窗拿走，它的 PID 也可能被另一個處理程序重複使用 —— 而一個 64 位元整數分不出「還是它」與「一個長得像它的新物件」。

所以選定那一刻就把身份記下來：句柄、所屬 PID、**該處理程序的建立時間**（把「PID 被重複使用」與「還是那個處理程序」分開的就是這一條）、視窗類別名，以及當初使它成為目標的那些條件。覆核分兩檔：

- **每一條通道的每一次嘗試之前**（授權之後、讀取像素之前再做一次）：句柄還有效嗎、還屬於當初那個處理程序嗎、那個 PID 的處理程序建立時間變了嗎、類別名對得上嗎。這四問的答案都在 user32 / kernel32 自己那份結構裡，不往目標執行緒傳訊息，所以問得起，也不會在這裡被一扇卡住的視窗拖住。
- **每個目標開工之前一次**，外加任何要改走桌面像素之前（`dwm` 的螢幕退路）：拿**當初那份條件重新求值一次**，看這個句柄還在不在候選清單裡。標題這類易變屬性就是這麼判的 —— 應用程式重新整理自己的標題（播放進度、文件修改標記、分頁標題）仍是同一個目標，而不再滿足 `--title` 那條條件的就不是。這一問要列舉一遍視窗，所以不按退回鏈的次數乘上去；`--title-regex` 那種沒有中斷點的比對照舊沿用第一次求值那同一條隔離判據（設了期限就進輔助程序）。

判不過就一個像素都不讀，交回的是三條穩定的碼：

| 碼 | 退出碼 | 含義 | 下一步 |
| --- | --- | --- | --- |
| `capture.target_gone` | 7 | 句柄在本次請求中被銷毀 | 重新列舉視窗再來一次 |
| `capture.target_changed` | 7 | 這個句柄值現在屬於另一個物件（或不再滿足當初的條件） | 重新選定目標；**你核准的許可不會轉給新物件** |
| `capture.target_unverifiable` | 7 | 有一道判據問不出來（讀不到處理程序資訊、條件求值沒跑完） | 查執行環境（權限、策略、防毒軟體），或加大 `--timeout-ms` 再來 |

身份變了**不會**去放寬條件另找一扇看起來一樣的視窗 —— 這與 `--yes` 那條規矩同源：許可綁的是列給人看的那個物件。

**保證的邊界**：這條覆核把競態視窗縮小了，不聲稱把它消除。判據與取影格之間不是原子操作，而 HWND 既不是可等待物件，也沒有「鎖住一個視窗不讓它消失」的公開 API。判據與取影格之間那一瞬仍可能變化 —— 只是不再有「整整一段規劃加人工確認」那樣長的時間可以拿來變。

## 執行期限與會阻塞的呼叫（`--timeout-ms` / `--consent-timeout-ms`）

`--timeout-ms <ms>` 是本次執行自動處理那一段的**總預算**，從開始選目標那一刻起依單調時鐘計時。視窗/螢幕匹配
（含 `--title-regex`）、`auto` 退回鏈、等影格、編碼、最後那次提交，花的都是**同一份**預算：沒有任何一步、也沒有
批次裡任何一個後續目標能重新領一份完整預算，所以四條後端不可能各等 2 秒、兩個目標也不可能各再等一遍。不給或寫
`0` 就是不設總預算；即便如此每次隔離呼叫仍受一個內建的 5000 ms 上限約束——這本是舊的 `timeoutMs` 參數該做到的事。
預算用盡時，受影響的那張圖**不會**寫出——條件求值階段把預算耗盡給 `match.timeout`（`stage=match`），取影格與編碼
階段給 `capture.timeout`（`stage=capture`，編碼也算在這一級），寫檔案/標準輸出階段給 `io.timeout`
（`stage=write` / `stdout`，退出碼 `8`）；批次裡剩下的目標不再開始，已經寫完的圖仍留在 `images` 裡。所以部分完成
的一批和局部截圖失敗表現完全一致：退出碼非 0，凡是已經落地的都照樣交付。

等一個人是**另一條**時鐘：`--consent-timeout-ms <ms>` 只為確認框設上限，絕不動自動處理那份預算（人走開了不等於
「機器慢」）。到了時間沒人回答，本次請求就按**拒絕**處理——`capture.consent_timeout`、退出碼 `6`——絕不當成同意，
批次剩下的部分也就會像有人明確答「否」之後那樣停下。不給或寫 `0` 還是一直等，和從前一樣。點「是」之後那約 1 秒
的緩衝是為了把對話框的關閉動畫擋在畫面之外，它屬於人工那一階段，絕不會為了趕期限而被省掉：被限制的是「等一個
回答」，不是「回答之後等畫面安定」。

**阻塞到底堵在哪。** `PrintWindow` 是給目標視窗發一個繪製請求、再等它自己的執行緒；`--capture printwindow` 與
`dwm` 的回讀乾的就是這件事，而這個呼叫內部沒有能拿來核對期限的打斷點。`std::regex` 也一樣：像 `(a+)+$` 這種
模式去匹配一個長標題可能回溯好幾分鐘，而對模式限長度並不是執行期限。這些呼叫現在都跑在同一個 `ECAPTURE.EXE`
拉起的輔助處理程序裡，父處理程序透過一條私有管線把已經解析好的一個任務遞給它；期限一到，父處理程序就停掉
**它自己的**那個輔助處理程序並報出逾時。目標應用的視窗絕不會被殺掉，也沒有哪個輔助處理程序能活得比父處理程序久
（一個「關閉即結束」的工作物件，加一次斷管檢查，再加一個閒置看門狗）。而這**不**改變的事：輔助處理程序只會讀
單一視窗自己的畫面、或列舉頂層視窗，它從不取樣桌面像素、也從不寫檔案，所以每一條桌面路徑照舊要走上面那套授權
——沒有 `--worker` 這個選項，`--yes` 的任何規矩也沒有變鬆。

把限度說成限度：這份預算只在能打斷的地方、以及靠停掉輔助處理程序起作用。原子寫檔案、被人停止讀取的 stdout 管線、
無視取消請求的 WinRT 編碼器都沒有取消點，所以這三樣是在開始之前攔一道、在結束之後再計時，而不是在呼叫中途搶佔。
而且在 `Win10 19045` 上，`PrintWindow(PW_RENDERFULLCONTENT)` 是從 DWM 快取的那張表面上渲染的，根本不發
`WM_PRINT`，所以一個卡在 `WM_PRINT` 裡的視窗在那個系統上並不會把父處理程序拖住；真正會等目標執行緒的是不帶這個
flag 的那次 `PrintWindow` 退路呼叫。別假定「卡死」這種場景在每個 Windows 版本上都碰得到——只要假定這個工具會在
它的期限內返回。

## 檔案名佔位符

用在 `--out` 的路徑裡，多張圖靠它區分：

| 佔位符 | 含義 |
| --- | --- |
| `%i` | 序號，從 1 起（`--all` 多視窗、`--monitor all` 多螢幕） |
| `%h` | 視窗句柄，形如 `0x001B0C48`；螢幕目標給 0 |
| `%p` | 處理程序 ID；螢幕目標給 0 |
| `%n` | 視窗標題；螢幕目標給去掉 `\\.\` 前綴的裝置名（如 `DISPLAY1`）。標題會被清洗成能用的檔案名片段：非法字元換成 `_`、去掉尾端的點與空格、整段剛好是保留裝置名稱（`CON` / `NUL` / `COM1` / `LPT1` …）時加 `_` 前綴、依 80 個 UTF-16 碼元截斷且不劈開代理對 |
| `%d` | 本地日期 `YYYYMMDD` |
| `%t` | 本地時間 `HHMMSS` |
| `%%` | 一個字面上的 `%`；其餘 `%x` 原樣保留兩個字元 |

`--all` 的輸出名裡沒有佔位符時會自動追加 `_序號`，並發出 `note.all_without_placeholder`。
佔位符分不開目標時（只寫 `%d`，或同一處理程序的兩個視窗寫 `%p`）不會被悄悄改名：整批名字事先算好，撞名就報
`io.output_collision`。`%d` / `%t` 用的是本批次那一次時鐘，所以跨午夜的一批也全用同一個日期與時間。規劃出的名字
依絕對路徑、不區分大小寫、逐碼元比較（NTFS 就是這樣看名字的）；字串比較看不見的別名（8.3 短名、硬連結、目錄
junction 與符號連結、UNC 與磁碟代號兩種寫法）交給提交那一次原子操作判定，所以預檢認不出的佔用同樣不會被靜默替換。
`--out -` 不是路徑：不展開、不補副檔名、不查碰撞；而標準輸出一次只交付一張影格，所以那條串流上永遠不會有整批圖
（見前面「輸出形式」的規則）。

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

1. **先問一次能力，再用 `--list` / `--inspect` 發現視窗，最後真的截圖。** `--capabilities` 是唯讀的：不取像素、不彈確認框、
   不寫檔案，所以不會打擾任何人，適合放在自動化流程最前面。它把這台機器的版本、工作階段、螢幕拓撲、每條路線的
   `available` / `unavailable` / `unverified`、每種格式、`--yes` 到底管哪幾條內部路徑，以及取值上限一次交給你，
   於是「該用哪條 `--capture`」「這次失敗該換通道還是這台機器不行」「這裡彈框有沒有人會答」在動手之前就有答案。
   要送出問題報告就再跑一次 `--diagnostics`（同一批判準，另加可核對的建置識別碼；`-v` 展開每一問的原始答案）。
   兩份文件都不隨 `--lang` 變（全 ASCII），所以比對結果穩定。注意 `available` **不是**「這個視窗一定截得到」：
   驅動、受保護內容、HDR 都不在那層的斷言裡，文件末尾的 `caveats` 就是寫來釘住這一時點的。
   之後照舊 `--dry-run`：它不取影格、不寫檔案、也不彈確認框，候選在 `notes[0].value`：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。`--dry-run` 也不必給 `--out`
   （那一次沒有圖片要交付，結果整份在 stderr）；而**只給 `--dry-run` 不給任何視窗條件 = 文字說明 + 退出碼 2**。
   要的是**清單**而不是一行人話時，用 `--list`（結構化、分頁，多匹配不算錯誤，一個都沒命中是空清單 + 退出碼
   `0`）與 `--inspect`（一扇視窗，多匹配仍算歧義 —— 它不會替你挑一個）。兩條都不取畫素、不彈框，`--yes`
   對它們也沒有任何作用。它們交回的是快照：真去截圖仍要複核目標身份，所以請把**剛做完的一次** `--inspect`
   裡的句柄傳給截圖，而不是緩存早前那一輪的。詳見上文《結構化的視窗發現與檢查》一節。
2. **依 `errors[].code` 分支，不要比對 `message` 文字**（那會隨 `--lang` 變），也不要拿「有沒有給 `--out`」當原因——
   省略它那條路與 `--out -` 報的是同樣的碼。常用的幾條：
   `match.no_window`（4，條件太窄或目標被最小化）、`match.ambiguous_window`（5，從 `hint` 的候選裡挑）、
   `match.index_out_of_range` / `match.monitor_out_of_range`（1，`hint` 列了全部候選）、
   `cli.invalid_format`（1）、`cli.stdout_multiple_targets`（1，多個目標要共用
   同一條 stdout）、`capture.failed`（7）、`capture.frame_timeout`（7，等影格逾時）、
   `capture.window_gone`（7，目標已經沒了，該重新列舉）、`capture.frame_invalid`（7，交回來的影格記憶體形狀不合法）、
   `capture.consent_unavailable`（6，這個工作階段沒有可互動的桌面，沒人能夠同意）、
   `capture.consent_stale`（7，確認之後目標挪了位置，要重新選目標並再問一次）、
   `io.write_failed`（8，目錄不存在或提交失敗）、`io.file_exists`（8，搭配 `--no-overwrite`）、
   `io.output_collision`（8，兩個目標算出同一個輸出名，整批沒截圖也沒寫檔）。
   其中有兩條說的不是這個目標、而是**這一台機器**，對它們重試同一個視窗沒有任何意義：`env.os_too_old`
   （7，本機 Windows 內部版本低於所有格式共用的那唯一一套編碼器所在的下限——換 `--capture` 也不會變好）與
   `env.channel_unsupported`（7，你顯式指定的那條通道要更新的版本——換一條通道或改用 `auto` 才有意義，而工具
   不會自己把你指定的那條頂替掉）。`note.channel_unavailable` 說的是 `auto` 鏈裡被去掉的那一條，而剩下的幾條
   仍可能截成交功。先問一句「這台機器給得出哪幾條通道」不必截圖：`--verbose` 的 `input.osBuild` 與
   `input.captureChain` 就是它。那幾道下限、宣告範圍與實測範圍見[系統支援](#系統支援)。
   每條錯誤還帶 `target` / `backend` / `stage` / `hresult` / `win32`（見前面「輸出形式」的規則），拿到多少寫多少，
   不必從 `message` 裡摳。
3. **讀取資料流要分情況**：給 `--out <檔案>` 時 JSON 在 stdout、stderr 是空的，直接解析就行；用 `--out -` 或沒給輸出路徑時
   圖片位元組佔了 stdout，JSON 整體改到 stderr。stdout 一次只交付一張圖，多個目標請寫到檔案。PowerShell 5.1 裡
   `2>&1` 會把 stderr 包裝成錯誤記錄，想同時拿圖片和 JSON 就用 `1>`/`2>` 分開重新導向。
4. **別把非 0 退出碼當成全盤失敗**：部分成功時 `captured` 大於 0 而退出碼是 7，已經寫出的圖照樣可用；
   `images[].source` / `path` / `scope` 分別告訴你那張圖出自哪條通道、走了哪條內部路徑、像素是視窗自己的還是螢幕上的。
5. **退出碼 0 不等於畫面是對的**：受保護內容、某些播放器的驅動會在成功回傳的同時給你黑影格。工具
   自己會告訴你整幅是不是只有一個顏色——它把每個像素與左上角那個逐位元組比過（BGRA 四個通道都算，
   行末填充不算），確實單色就發 `note.frame_uniform`（顏色寫成 `0xAARRGGBB`）而圖片照常交付。單色
   只是品質提示，不是失敗：純色視窗、單色桌布本來就是這個樣子。要判斷正確性還得校驗像素——
   例如把一個純色視窗蓋住目標再截，看拿到的是目標內容還是遮擋物；至少比對 `width`/`height` 與目標視窗矩形。
6. **預設會被彈框打斷**：不給 `--yes` 時，任何真實截圖（連只截一個視窗也算）都會擋住處理程序直到有人回應。
   目標是一個視窗、而且走的是視窗內容路徑（`wgc` / `printwindow` / `dwm.thumbnail`）時才適合加 `--yes`；對
   `bitblt`、`duplication`、`dwm` 的螢幕退路以及任何整張螢幕，這個開關一點作用都沒有，一定要人回答——呼叫前先告知
   使用者，並顯式給 `--out`。事後讀 `images[].scope`：寫成 `desktop` 就代表圖裡可能混進別的視窗、開啟的文件與通知。
   只想截某個視窗，就不要把目標升格成整張螢幕。
7. 想穩定拿到「某個應用程式」，優先使用 `--process`/`--exe` + `--class`；標題比對區分大小寫，跨語言環境不可靠。
8. **給自動化留一條退路**：呼叫端加一個 `--timeout-ms`（例如 5000），免得一個卡死的目標視窗把你也一起吊住。預算
   用盡會給你 `match.timeout` / `capture.timeout` / `io.timeout` 而不是整趟掛死；確認框在 `--consent-timeout-ms`
   之內沒人回答就是拒絕（`capture.consent_timeout`、退出碼 6）。看到 `capture.timeout` 且 `backend=printwindow` /
   `dwm`，多半是目標的 UI 執行緒卡住了，請改用 `--capture wgc` 或放寬預算。

## 建置與測試

| 命令 | 用途 |
| --- | --- |
| `.\build.ps1` | Release 建置，產物 `build\ecapture.exe`；`-Config Debug`、`-Clean` 可選 |
| `.\tests\cli.ps1` | 518 例輸出契約斷言（含 `--yes` 與 `--no-overwrite` 的每種布林寫法、查詢與截圖選項互斥那一組）+ 通道分離 + 省略 `--out` 與 `--out -` 的等價對拍 + 多語言檢查（一律 `--dry-run`，不截圖） |
| `.\tests\capabilities.ps1` | 能力與診斷查詢（`--capabilities` / `--diagnostics`）：離線跑 `build\ecapture-capabilities-tests.exe`（注入假探針判「一塊螢幕都沒有」「版本正好低於某條下限」「版本問不出來」「某個編碼器沒登記」「`--yes` 的適用範圍與登記表一致」「兩份查詢共享同一批判準」）；真機層判查詢真的不彈框（期限本身就是斷言）、不落地、`os` / `arch` / 通道鏈與 WMI 及 `--dry-run -v` 兩處獨立值同源、文件全 ASCII 且不隨 `--lang` 變、不含使用者名與任何路徑。無圖形工作階段、更低版本、編碼器缺失、ARM64 / Server / 遠端桌面這幾項本機造不出來，一律記未驗證 |
| `.\scripts\check-lang.ps1` | 四語文案的 key / 佔位符對齊檢查，並確認 exe 裡真的編進了四份資源 |
| `.\tests\invoker.ps1` | 離線檢查共用的測試程序呼叫器：argv 引號、兩條流同時輸出、二進位不被轉碼、卡死的子程序、每次執行各自的暫存目錄（不截圖） |
| `.\tests\build-path.ps1` | 建置路徑判據：離線那層驗暫存批次檔正文只能是 ASCII、VS 環境匯入失敗要在跑 cmake 之前就報錯；真機那層在含中文、空白、括號、百分號的目錄裡跑 Release / Debug / RelWithDebInfo 與 `-Clean`，再把 `%TEMP%` 換成中文目錄建置一次（不截圖；`-OfflineOnly` 只跑離線那層） |
| `.\tests\smoke.ps1` | 實機冒煙：截自己建立的測試視窗 → 校驗 PNG 尺寸與像素內容 |
| `.\tests\image.ps1` | 影格校驗：離線層手工擺像素排布（直條紋 / 棋盤格 / alpha / 行末填充 / 超限與短緩衝 / 
  越界裁剪），實機層驗單色視窗的品質提示與來路 |
| `.\tests\dup.ps1` | Desktop Duplication 多螢幕判據：離線那層（`build\ecapture-dup-tests.exe`，原始碼 `tests\dup_state.cpp`）把四種旋轉注入正式碼的幾何判據——像素判據拿測試自己那份樸素的「先把整幅影格轉好、再裁剪」來對照——另加負座標、裁出的／被截斷的矩形、一份假的雙配接器輸出表（目標在第二張配接器上、沒有任何輸出、目標已拔除），以及「那張螢幕在確認之後變了」那些案例；實機那層判確認框必須彈出（`--yes` 跳不過桌面路徑）、`requestedRect` / `capturedRect` / `clipped` / `rotation` 對真實圖片的正確性、每張螢幕做一個四角方位探針，以及每張螢幕都截得到。測試絕不重新排列或重新旋轉顯示器：機台給不出那種場景時，旋轉面板與熱拔除的判據一律記 SKIP（未驗證） |
| `.\tests\compat.ps1` | 系統支援檢查。離線層（`build\ecapture-compat-tests.exe`，原始碼 `tests\compat_state.cpp`）把假版本注入生產能力判準本體：每道下限兩側各判一次、顯式指定的通道被擋下時絕不換成別的、auto 鏈少的是哪一條、以及版本問不出來時一條都不篩。真機層（不截任何圖）：探測到的內部版本與 WMI 獨立問來的那份相同（說明探測沒被版本偽裝）、回顯的通道鏈與該版本自相一致、`--dry-run` 不因環境判準報錯、四語說明都帶上下限與 `env.*` 那兩條碼，以及發布版二進位檔確實匯入了 Windows 8 才有的 winrt / job 那批 API Set 契約（裝載下限的說法就靠它）。凡是需要另一台 Windows 版本的判準一律記未驗證，不拿文件推導冒充實測 |
| `.\tests\save.ps1` | 實機檔案儲存與覆蓋保護：每種 `--no-overwrite` 布林寫法對真實檔案的效果、整批輸出名規劃與撞名偵測（`%p` / `%n` / `%d` / `%t` / `%%` / 未知 `%x` / 大小寫 / 清洗 / 截斷）、原子提交（目標被佔用、目標名是目錄、目錄不存在、寫到一半被硬殺）、併發禁止覆蓋 |
| `.\tests\channels.ps1` | 實機通道對比：六條通道 + 遮擋對照，目標與遮擋物都是自建的視窗。視窗內容那幾條帶 `--yes` 跑，一旦彈框就判失敗；`bitblt` / `duplication` 取的是桌面像素，它們的畫面判據要加 `-SimulateConsent` 才跑，不加就如實記 SKIP（未驗證） |
| `.\tests\consent.ps1` | 截圖授權分級：離線那層用注入的假應答器與假螢幕佈局把 `ConsentGate` 整台狀態機跑完（`build\ecapture-consent-tests.exe`，原始碼 `tests\consent_state.cpp`）；實機那層把所有確認框一律代答「否」，判哪些路徑必須彈、被拒之後報什麼（`code` / `stage` / `target` / `value`）、有沒有落地，以及 `images[].path` / `scope` / `rect` 對不對。絕不代人答「是」 |
| `.\tests\isolation.ps1` | 實機資源隔離：同名的既有處理程序保持存活且不會被當成目標、並發兩輪互不串、異常退出只清理自身 |
| `.\tests\identity.ps1` | 目標身份與 Z 序選擇判據。離線層（`build\ecapture-identity-tests.exe`，注入假查詢層）：句柄被另一個處理程序佔用、PID 相同但那是另一個處理程序、類別名換了、當初的條件不再成立、每一問各自問不出來，以及兩檔覆核各問哪幾問與短路順序。實機層（只用自建視窗）：健康目標一次都不誤傷、批次中途目標被銷毀確實報 `capture.target_gone`、改了名而 `--title` 條件不再成立確實報 `capture.target_changed`、改名而條件仍成立就照常出圖、`--topmost-match` / `--bottommost-match` 對著當下的 Z 序判（先建但置頂的那扇贏，正是「最後建立」那種讀法會挑錯的情形）。句柄與 PID 何時被回收沒辦法安排現場（那要結束別人的處理程序），確認框那一段又要代人點「是」，兩者一律記未驗證而不偽造通過 |
| `.\tests\screen.ps1` | 實機整張螢幕測試：三條螢幕通道（都屬於桌面路徑，每一條都必須彈框）+ 紅塊定位 + 陰性對照。只有加上 `-SimulateConsent` 才會代答確認框，且只該在專門騰給測試的桌面上這麼用；不加時凡是要答框的判據一律記 SKIP（未驗證） |
| `.\tests\streams.ps1` | 實機標準串流與結構化結果可靠性：單個目標寫 stdout、多個目標被拒、判據是實際命中的目標數、多螢幕被拒而且確認框根本不彈、診斷的定位欄位、批次中途失敗時保留前面已經成功的圖、結果送不到約定的那條串流時報 8，以及省略 `--out` 與顯式 `--out -` 在成功 / 無匹配 / 歧義 / 非法參數 / 後端失敗 / 被拒絕 / 寫入斷管七個場景上的機器語義對拍（只截自己建的視窗） |
| `.\tests\window_shot.bat` | 給人跑的批次檔：編譯測試視窗程式 → 逐通道截圖 → 開啟截圖目錄 → 只結束自己起的那個 PID |
| `.\scripts\mkreadme.ps1` | 用各語言 `--help` 的原樣輸出重新產生四份 README 的說明段 |

所有實機測試的目標視窗一律是自家的：`tests\helper\ec_window.cs` 編到本次執行的暫存目錄裡，測試握著它的
PID 與 HWND，因此既不按處理程序名去找目標、也不按處理程序名批次結束，刪除的也只有自己建立的那個目錄。
`tests\harness.psm1` 放著共用的程序呼叫器（argv 引號規則、兩條流併發消費、有期限的等待、逾時只結束自己
那棵程序樹），以及暫存目錄與測試視窗的建立與收尾；`tests\invoker.ps1` 就是用來證明這個呼叫器本身的。

`build.ps1` 用 vswhere 定位 VS，並優先使用 VS 自帶的 cmake/ninja。倉庫目錄、build 目錄與工具鏈路徑只經子程序的
環境塊遞給那個暫存批次檔，正文裡一個絕對路徑都不寫，所以倉庫放在含中文、空白、括號或 `%` 的目錄裡也能照常建置，
而正文一旦混進非 ASCII 會在寫入前被當場攔下（判據見 `.\tests\build-path.ps1`）。建置要求在 `/W4` 下零警告。
在 Git Bash 裡手動測試要先 `export MSYS2_ARG_CONV_EXCL='*'`，否則 `/help` 會被當成路徑改寫、`--out /tmp/x.png` 會被轉成怪異的路徑。

## 授權

EvernightCapture 採用 [Mulan PSL v2（木蘭寬鬆許可證，第2版）](http://license.coscl.org.cn/MulanPSL2)，
中英雙語全文見 [LICENSE](LICENSE)。

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
