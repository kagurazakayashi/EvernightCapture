![EvernightCapture 圖示](resources/icon.ico)

# EvernightCapture

命令列視窗截圖工具：按條件篩出視窗，把那個視窗的畫面存成圖片檔案。

[English](README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja-JP.md)

基於 Windows.Graphics.Capture 的一整套取圖通道，入口是 `ECAPTURE.EXE`——單一的可執行檔案，靜態連結 CRT，
目標機器不需要安裝 VC++ 執行時。輸出對程式友善：`--help`、`--version`，以及「一個條件都不給」那一種情形是
純文字，其餘一律 JSON；退出碼穩定，診斷帶穩定的 `code`，所以既適合人手工輸入，也適合被腳本與 AI 呼叫。

目前版本 **0.4.0**：`--capture` 的取值全部可用（`wgc` / `dwm` / `printwindow` / `bitblt` / `duplication` / `auto`），
`--monitor` 提供整張螢幕截圖與「按螢幕過濾視窗」。曾經實作過的 `magnification` 通道已刪除：Windows 11 的
`magnification.dll` 不再匯出 `MagGetImage`，這條路沒有離屏讀圖的走法，只能把放大鏡控制項視窗擺到螢幕上搶
z 序，而拿到的畫面與 `bitblt` 等價。這段依據記在 `src/CliOptions.h` 的註解裡。

## 先看哪裡

第一次來這裡：[快速開始](#快速開始)（裝好、截第一張圖、始終給出一個輸出檔案）→ 《結構化的視窗發現與檢查（`--list` / `--inspect`）》（截圖之前先認出目標）→ [截圖授權與 --yes](#截圖授權與---yes)（哪一步一定要人來確認）→ [退出碼](#退出碼)（出了什麼事、下一步做什麼）。

| 想查的是什麼                                                               | 看這一節                                                                                   |
| -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `--yes` 到底免掉了哪一層確認，以及哪一層是任何開關都免不掉的               | [截圖授權與 --yes](#截圖授權與---yes)                                                      |
| 圖片位元組走哪條串流、JSON 走哪條串流，以及某個 shell 能不能把它完好傳下去 | [輸出形式](#輸出形式)、[標準輸出圖片位元組的 shell 差別](#標準輸出圖片位元組的-shell-差別) |
| 那份期限預算怎麼共用，以及它打斷不了什麼                                   | 《執行期限與會阻塞的呼叫》                                                                 |
| 穩定的 code、stage、退出碼、部分成功                                       | [退出碼](#退出碼)                                                                          |
| 哪些是建置裡有的、哪些是本機現在能走的、哪些真的測過                       | [系統支援](#系統支援)、《唯讀的能力查詢》                                                  |
| 從 skill 目錄裡跑時回報 `0x80070005` 或錯誤 `5`，下一步做什麼              | 《處理序完整性層級》                                                                       |
| 這個倉庫還沒有在真機上確認下來的問題                                       | [邊界與未驗證項](#邊界與未驗證項)                                                          |
| 哪一條測試證明了哪一條規矩                                                 | [建置與測試](#建置與測試)                                                                  |

## 特性

- **按條件篩視窗**：句柄 / 處理程序 ID / 映像檔案名 / 完整路徑 / 標題（精確、包含、正則）/ 視窗類名，不同選項之間是 AND、同一選項寫多次是 OR
- **六條取圖通道**：被遮擋的視窗也能截（`wgc` / `dwm` / `printwindow`），或者刻意只拷螢幕上看得見的像素（`bitblt` / `duplication`）
- **多視窗一次截完**：`--all` 每個命中視窗各存一張，搭配 `%i` 這類佔位符命名
- **視窗內部裁剪**：`--roi x,y,w,h` 從交付的整張視窗畫面裡留一塊，`--client-area` 只留客戶區。座標說的是**這張畫面自己的像素**（左上角 = (0,0)，物理像素，不依 DPI 縮放），永遠不會被當成桌面絕對座標；放不下的矩形是拒絕，絕不往裡挪、裁到邊上為止或退回整張交出。完整規則見下文《視窗內部裁剪（`--roi` / `--client-area`）》
- **等比縮小**：`--scale max-width=N,max-height=N,max-pixels=N` 用三條上限裡最緊的那條算出的**同一個比例**把圖等比縮進上限，寬高各自向下取整，**絕不放大**，插值只有最近鄰這一種可預測的整數映射。順序是先裁後縮，所以縮的是裁完的那一塊。完整規則見下文《等比縮小（`--scale`）》
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
- **每張成功的圖另存一份歷史副本**：預設開啟，寫在**實際執行的那個 exe 旁邊**的 `history\本地日期\` 裡，用的是
  主交付那一份已經編碼的位元組（不重拍、不重新編碼、不去讀主輸出檔案、也不建硬連結），獨佔提交從不覆蓋既有歷史。
  主圖已交付而副本沒落地時是部分成功（退出碼 `7`，`images[].history` 說清是哪一種），不自動輪轉也不自動刪除，
  清理由使用者自己顯式做。完整規則見下文《截圖歷史歸檔（預設開啟）》
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

# 圖片位元組走標準輸出、JSON 走 stderr：只有 cmd 與 PowerShell 7.4 起保證位元組無損，
# Windows PowerShell 5.1 會把兩邊都弄壞，在那裡請改用 --out <檔案>（見「標準輸出圖片位元組的 shell 差別」）
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
  --monitor, -m [<n|primary|all|device:|id:>] 截圖目標屏的編號，從 1 開始、只認十進位（本次列舉的順序，不保證等於「顯示設定」裡寫的識別號；要認螢幕請看結果裡的 device）；primary = 主螢幕，all = 每張螢幕各一張。不給視窗條件時 = 整張螢幕截圖，給視窗條件時 = 只算與該螢幕有重疊的視窗。取值可省略（= 主螢幕），省略時不吃後面的參數，所以 --monitor out.png 仍然可用。整張螢幕拍的是桌面像素，一定要先彈框問人，--yes 也跳不過；按螢幕篩選視窗出的仍是視窗圖；要點名叫螢幕請用 --screens 交回的身分：device:<裝置名>（本次桌面連線的名字）或 id:<螢幕裝置路徑>（跨工作階段那一條）。編號在拔插或改解析度之後可能指到另一張螢幕

視窗匹配條件（同一選項多次出現取並集，不同選項必須同時命中）
  --hwnd <handle>                             視窗句柄。純數字按十進位，0x 前綴或含 a-f 按十六進位；推薦寫 0x。不接受正負號與空白，底線只能夾在兩位十六進位數字之間
  --pid <pid>                                 處理程序 ID，只認十進位且大於 0
  --process, -p <image-name>                  映像檔案名（不含路徑），忽略大小寫；無副檔名時按 .exe 處理
  --exe <full-path>                           映像完整路徑，忽略大小寫
  --title, -t <exact-title>                   視窗標題精確匹配
  --title-contains, -T <text>                 視窗標題包含子字串
  --title-regex, -R <regex>                   視窗標題正則匹配，ECMAScript 語法，匹配期即校驗
  --class, -c <class-name>                    視窗類名，忽略大小寫，如 Notepad / CabinetWClass

匹配到多個視窗時（互斥）
  --index, -i <n>                             取第 n 個視窗，從 1 開始（十進位），按可見性/疊放次序排序
  --topmost-match                             取 Z 序最靠前的相符視窗（此刻蓋在最上面的那一個；與建立時間無關）
  --bottommost-match                          取 Z 序最靠後的相符視窗（此刻被壓在最下面的那一個；與建立時間無關）
  --newest                                    --topmost-match 的舊名字。它選的一直是當下的 Z 序位置而不是建立時間（視窗建立時間沒有公開 API 可取），保留只為相容
  --oldest                                    --bottommost-match 的舊名字。它選的一直是當下的 Z 序位置而不是建立時間，保留只為相容
  --all, -a                                   每個匹配視窗各存一張

取圖方式（預設 wgc；受系統版本或視窗性質限制時會失敗）
  --capture, -C <method>                      wgc(預設，被遮擋也能截) / dwm(DWM 縮圖，被遮擋也能截) / printwindow(視窗自繪) / bitblt(拷螢幕可見像素) / duplication(桌面複製後按矩形裁，會依顯示器的旋轉校正方向；只取與該目標重疊最多的一塊螢幕，沒截全時結果裡帶 capturedRect/clipped) / auto(依 wgc-dwm-printwindow-bitblt 復原；整張螢幕只用 wgc-duplication-bitblt)。只取視窗自己的畫面：wgc / printwindow / dwm 縮圖；會從螢幕上取樣：bitblt / duplication 與 dwm 的螢幕退路
  --cursor <default|include|exclude>          畫面裡要不要滑鼠指標：default(預設，本工具一個字都不改，結果裡也不出現滑鼠指標那三個鍵) / include(要) / exclude(不要)。只有 wgc 有能設進去也讀回來的開關（要內部版本 19041 起）；printwindow / dwm / bitblt 的來源本來就沒有指標，duplication 的桌面影格卻可能已經把指標畫在裡面、又沒有開關，所以 include 配這四條、exclude 配 duplication 都報 capture.cursor_unsupported（兩種文案），--cursor default 照舊交影而 cursorEffective 寫 unverified；絕不改用會從螢幕上取樣的通道，auto 時做不到的那幾條從鏈裡摘除並各留一條 note.cursor_channel_skipped。這一個不改變授權；requested / effective / basis 三件事見 README
  --hdr <auto|tonemap|refuse>                 HDR 來源怎麼處理：auto(預設值，本工具對色彩一個字都不改；這條選項整個沒寫時結果裡不出現色彩那組鍵) / tonemap(把 HDR 影格按固定 tone mapping 對映成 8 位元 SDR 後交付) / refuse(核實來源是 HDR 就報錯，絕不交一張被硬壓成 BGRA8 的發白圖)。本建置真兌現得了 tonemap/refuse 的只有 wgc 一條，所以配 printwindow / dwm / bitblt / duplication 在剖析期報 capture.hdr_unsupported，絕不該走去讀桌面像素的通道；--capture auto 時同一條判據會把兌現不了的通道從回退鏈裡摘除（各留一條 note.hdr_channel_skipped，一條都不剩就是 env.hdr_unsupported），而 capture.hdr_refused 這類策略結論會讓整條鏈立刻停下。這一條不改變授權；來源色彩空間、位元深度與實際處理寫進結果，判據見 README 與 --capabilities 的 color 段

視窗內部裁剪與等比縮小（對交付的整張視窗畫面，依畫面自己的像素座標再裁一次；不是桌面絕對座標；順序是先裁後縮，--roi 與 --client-area 兩條互斥）
  --roi <x,y,w,h>                             從交付的整張視窗畫面裡裁出以 x,y 為起點、w×h 大小的一塊。原點 (0,0) 是這張畫面自己的左上角像素（畫面對應的是使用者看到的那圈可見邊框，DWM 那圈透明 resize 邊框不在裡面），單位是物理像素且不依 DPI 縮放（本程序是 per-monitor v2，要按邏輯像素指定就自己乘那道縮放），所以這四個數永遠不會被當成桌面絕對座標。四個數只認十進位、逗號分隔；x 與 y 可為 0，w 與 h 至少 1，都不超過 16384。放不下就整張不落地：取影格之前就看得出放不下報 match.roi_out_of_range（不彈框、不寫檔案），取到影格之後才發現的報 capture.roi_invalid —— 不往裡挪、不裁到邊上為止、也不退回整張交出。裁剪排在取影格之後，所以它不改變授權：會從螢幕上取樣的那幾條照樣一定問人，--yes 不會因為「最後只留一小塊」而生效。結果裡 cropRect 是畫面像素座標，cropScreenRect 是同一個矩形的螢幕座標（核實得出畫面原點時才寫），裁前尺寸在 fullWidth/fullHeight、裁後就是 width/height。與 --client-area 互斥，配整張螢幕的目標說不通（capture.unsupported）
  --client-area                               只交回視窗客戶區那一塊：在交付的整張視窗畫面裡再去掉標題列與三邊邊框。這個矩形照目標此刻的幾何量出來（GetClientRect 加 ClientToScreen），座標系與單位跟 --roi 完全同一套。量不出客戶區報 capture.roi_unmeasurable，有一邊落在交付畫面之外（掛在螢幕外、或中途改了尺寸）報 capture.roi_invalid，兩種都不退回整張交出。與 --roi 互斥
  --scale <key=N>                             把交付的這張圖等比縮小到上限之內：max-width=N 限寬、max-height=N 限高、max-pixels=N 限總像素數（N 都是十進位；邊長 1..16384，像素數 1..268435456）。三條互相獨立，可只給一條、也可一條裡用逗號串幾條（如 max-width=1920,max-pixels=2073600）；這一條寫多次時每條上限各記各的，重複給同一條時最後一個生效。三條都按同一個比例縮（取最緊的那一條），寬高各自向下取整；預設不放大，圖本來就在上限之內就原樣交付（結果裡 scaleApplied=false）。插值策略只有一個，而且是可預測的整數映射：最近鄰。順序是先裁（--roi / --client-area）後縮、再編碼，所以縮的是裁完的那一塊；結果裡 scaleFromWidth/scaleFromHeight 是縮之前的尺寸、width/height 是縮之後的，scaleMethod 是插值策略，映射按這個順序閉合。這一條不改變授權與影格上限：會讀桌面像素的路徑照樣一定彈框問人（--yes 不會因為最後交的是張小圖而生效），影格大到過不了影格形狀檢查的縮不回來，--roi 的越界判據也仍按原圖判

截圖授權（真實截圖預設都要先彈框問一次；--yes 只免掉只取視窗畫面的那條路徑）
  --yes, -y                                   跳過「只取所選視窗畫面」那條路徑的確認框。不保證目標一定有畫面，也不忽略權限、受保護內容、錯誤或覆蓋保護；任何會從螢幕上取樣的路徑（bitblt、duplication、整張螢幕任何通道、dwm 的螢幕退路）一定會彈框，這個開關跳不過。寫 --yes=false 表示明確要問

執行期限（自動處理那一段的總預算；人工確認另算，到點按拒絕處理）
  --timeout-ms <ms>                           自動處理階段的總預算（毫秒）：從選定目標開始，條件求值、後端重試、取幀、編碼、寫檔共用這一份剩餘時間，任何一步都不會重新領一份完整預算。不給或寫 0 = 不設總預算，此時每次隔離呼叫仍受內建上限（5000 毫秒）約束。人工確認的等待不算在這裡，見 --consent-timeout-ms。預算用盡時，尚未開始的階段被拒絕、那一張不落地（已經提交完成的檔案不會被回滾），依階段報 match.timeout / capture.timeout / io.timeout
  --consent-timeout-ms <ms>                   確認框最多等多久（毫秒）。不給或寫 0 = 一直等人回答。到點按「拒絕」處理，絕不按「預設同意」處理，報 capture.consent_timeout。這段等待單獨計時，不佔 --timeout-ms 那份自動預算；點「是」之後那約 1 秒的關閉動畫緩衝也算在這一級，不會為了趕期限而省掉

輸出
  --out, -o <path|->                          輸出路徑；特殊值 - 表示把圖片位元組寫到標準輸出。也可用位置參數；完全不給時等同 --out -。整批輸出名在取影格之前一次算好，兩個目標算出同一個名字時整批報錯，不會靜默覆蓋。標準輸出一次只能交付一張影格，命中多個目標時整批報參數錯誤、一張都不截
  --format, -f <name>                         強制編碼格式；不給則由輸出檔案副檔名判定，副檔名也判不出時用 png
  --quality <1-100>                           JPEG 品質，十進位 1..100，預設 100
  --no-overwrite                              目標已存在時不覆蓋，報錯退出（不給取值就是禁止覆蓋）；寫 --no-overwrite=false（0 / no / n / off）取消這條禁令，=true / 1 / yes / y / on 與不給取值同義。重複給出時最後一個生效

查詢（唯讀：不截圖、不彈框、不寫檔案）
  --capabilities                              輸出本機能力報告（JSON）：版本、系統與工作階段條件、各條取圖路徑的 available / unavailable / unverified、格式與 --yes 的適用範圍。唯讀：不截圖、不彈確認框、不寫檔案，也不靠實際截圖來探測能力。available 只說明「這個建置里有這條路徑，且這次問出來的環境判準沒有擋掉它」，不保證某個視窗一定截得到。只接受 --lang / -v / -q，與任何截圖選項或輸出路徑同時給出 = cli.query_conflict + 結束代碼 1，一張都不截
  --diagnostics                               輸出診斷與版本報告（JSON）：建置版本、可核對的建置識別碼（PE 連結時間戳 + 架構 + 映像大小）、平台與後端狀態，欄位與 --capabilities 同源，不另立第二份環境資訊。預設不上傳、不擷取畫面、不列舉使用者檔案，也不輸出使用者名、環境變數與任何路徑；--verbose 追加每一問的原始答案，便於核對後再提交。互斥規則與 --capabilities 相同
  --screens                                   唯讀地列出本機每張螢幕：工具編號、裝置名、是否主螢幕、實體矩形、DPI 與旋轉（問得到的話）、歸屬配接器關聯，並標出這幾種身分各自穩到哪一層。不取一個影格、不彈框、不寫檔案，也不改任何顯示設定。交回的 device:<裝置名> 與 id:<螢幕裝置路徑> 可以直接寫進 --monitor。與截圖那一級的選項互斥（cli.query_conflict + 結束碼 1）；真去截整張螢幕仍然一定彈框，--yes 跳不過
  --list [<all>]                              唯讀地把滿足全部條件的頂層視窗列成 JSON（句柄 / PID / 類名 / 標題 / 映像名 / 矩形 / Z 序 / 身分約束欄位），不截圖、不彈框、不寫檔案，也不需要輸出路徑。命中多個按 --offset / --limit 分頁而不報截圖歧義，配選擇策略算衝突。取值 all = 也列最小化視窗。結果會過期，截圖時仍要複核身分
  --inspect [<path>]                          唯讀地檢查同一套選擇策略定出的那一扇視窗；多匹配報 match.ambiguous_window + 退出碼 5，不替你選一個。取值 path = 也寫出歸屬映像的完整路徑（預設只寫檔案名）。讀不到的項寫欄位級 denied / failed，不建議改用管理員身分；不恢復或啟用任何視窗
  --offset <n>                                視窗查詢跳過開頭 n 個（0 起）
  --limit <n>                                 視窗查詢本批最多 n 個（預設 50）

其他
  --dry-run, -d                               只解析並列出候選視窗，不截圖不寫檔案
  --json, -j                                  已廢棄的相容開關，無副作用：成功與錯誤本來就輸出 JSON
  --verbose, -v                               JSON 中追加 input 段（正規化後的全部輸入），並保留 notes
  --quiet, -q                                 省略 notes；errors 無論如何都會返回；與 --verbose 同時給出時按 --verbose 處理
  --lang, -l <language>                       文案語言。auto(預設，跟隨系統顯示語言) / zh-CN / zh-TW / en / ja；系統語言不受支援時用 en
  --help, -h                                  輸出文字說明（本段）
  --version                                   輸出版本與階段

寫法: --opt=value / -opt / /opt 都接受；取值本身以 - 開頭時寫成 --title=-x，或用 -- 結束選項解析。數字取值只認十進位（--hwnd 另可按 0x 寫十六進位）
輸出: 成功與錯誤都是 JSON，只含 captured / images（另有 errors / notes，--verbose 才有 input）
      --help / --version 以及不給條件時是文字
退出碼: 0 成功 / 1 參數錯 / 2 未給條件 / 3 --help / 4 無匹配視窗 / 5 匹配多個視窗 /
        6 目標受保護或被拒絕 / 7 截圖失敗 / 8 寫檔案失敗 / 9 內部異常
目前建置: --capture 的取值全部已實現（wgc / dwm / printwindow / bitblt / duplication，auto 按 wgc-dwm-printwindow-bitblt 退回；整張螢幕只用 wgc-duplication-bitblt）；輸出目錄必須已存在；每張成功交付的圖另存一份到程式目錄的 history 下（不會自動清理）
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
- **問不出來的條件永遠不算命中。** 系統不肯交出標題或處理程序資訊的視窗（`denied` / `failed`），就是不滿足需要
  這兩樣的那條條件；`--title-regex` 求值失敗時，已經收集起來的命中全部丟棄，交回的是這條失敗，而不是一份只求值
  了一半的候選清單。正則式是**在匹配時才編譯並執行**（不是在解析時），跑在隔離的輔助程序裡，它失敗的三種方式各自分開：
  - **語法錯誤**（編譯不過的模式）→ `cli.invalid_regex`，退出碼 `1`，`stage=match`；
  - **複雜度 / 資源上限**（像 `(a+)+$` 這樣的形式在長標題上撞到 MSVC 回溯的 `error_complexity`）→ 同一個
    `cli.invalid_regex` 碼、同樣退出碼 `1`，但這是一句「太複雜」的訊息，其 hint 直說**加大 `--timeout-ms` 沒有用**
    ——這是一次有界的資源停止，不是「慢但還能用」的答案；
  - **預算耗盡**（求值花光了 `--timeout-ms`）→ `match.timeout`，退出碼 `7`；而跑它的那個輔助程序压根沒回來時是
    `capture.worker_failed`，退出碼 `7`。
- 列舉時預設跳過不可見視窗與零尺寸視窗；**最小化的視窗截不到**，只在 `hint` 裡單獨說明。
- 命中多個又沒給消歧選項時不會隨便挑一個，而是報 `match.ambiguous_window`（退出碼 5），
  `hint` 裡按疊放次序列出全部候選。
- 候選清單按**當下疊放次序**（Z 序）排列，最上面的在前；`--topmost-match` / `--bottommost-match` 取這個順序的第一個 / 最後一個。
  舊名字 `--newest` / `--oldest` 保留為相容別名，行為完全相同 —— 它們選的一直是 Z 序位置而不是建立時間：Windows 沒有取得視窗建立時間
  的公開 API，處理程序的啟動時間也不是視窗的建立時間。寫舊名字只多發一條 `note.deprecated_option`；同一條策略的新舊兩種寫法一起給
  （`--newest --topmost-match`）仍然算一條策略，不會被判定互斥。

## 輸出形式

`--help`、`--version`、以及不給任何條件時是純文字。其餘一律 JSON，只裝擷取結果與錯誤。

唯讀查詢那三條是**另三份契約**（`--capabilities` / `--diagnostics` / `--screens`，見[系統支援](#系統支援)）：
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
      "elapsedMs": 156,
      "history": {
        "status": "saved",
        "file": "D:\\shots\\history\\2026-10-10\\20261010-113122-31468-1a2b3c4d5e6f-1.png"
      }
    }
  ]
}
```

上面這一張另外帶 `history` 那一段：主交付之外那份獨立副本的下落（預設開啟，位置跟著實際執行的程式目錄，
規則與各碼含義見[截圖歷史歸檔](#截圖歷史歸檔預設開啟)）。它是**第二次交付**的結論，改不到上面那個 `file`、
`bytes` 與 `captured`：副本失敗時那張圖仍然算已經交付，而退出碼按部分成功給 `7`。

螢幕圖（`--monitor` 且沒有視窗條件時）沒有視窗可歸屬，換成 `monitor` / `device` / `primary` 三個欄位，
`hwnd` / `pid` / `title` / `class` / `image` 整個不出現——呼叫端依 `monitor` 是否存在區分兩種圖。
寫出 `--cursor` 時，每張圖另外帶 `cursorRequested` / `cursorEffective` / `cursorBasis`——要的是哪一種、
這條路徑實際交回的是哪一種、這個結論憑什麼。沒寫這條選項時三個鍵一個都不出現，這正是「預設不要求」
與這條選項存在之前逐位元組相同的那條保證，判據見《畫面裡的滑鼠指標》。
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
   寫成功也不改回原來的值——呼叫端依約定串流讀取，讀不到就是沒拿到。至於某個 shell 傳那些圖片位元組時
   會不會把它改壞，是另一件事，見[標準輸出圖片位元組的 shell 差別](#標準輸出圖片位元組的-shell-差別)。
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
   `io.output_collision`（退出碼 8、`stage=plan`），整批一張都不截、一個檔案都不寫 —— 既不替呼叫端改名，也不讓第二張蓋掉第一張。
   每張圖先寫進目標目錄下唯一的暫存檔案，寫完並刷新之後才改名為目標名稱，所以寫入失敗不會清空也不會刪掉舊檔案。
   `--no-overwrite` 時「目標在不在」由那一次不許替換的改名當場判定（`io.file_exists`），不做有競態的預檢。
   **格式與檔名的契約**：`--format` 的取值是 `png` / `jpg` / `jpeg` / `bmp` / `tiff` / `gif`，而且它**優先於**副檔名；
   沒給 `--format` 時由副檔名決定（`.tif` 與 `.tiff` 都算 TIFF），判不出來就退回 `png`。檔名**沒有**副檔名時會補上
   該格式自己的副檔名（`.png` / `.jpg` / `.bmp` / `.tif` / `.gif`），每批另外發一條 `note.output_extension_appended`；
   已經有副檔名的絕不替你改名，所以 `--out shot.png --format jpeg` 就是把 JPEG 位元組寫進 `shot.png`。
   `webp` 與 `ico` 不接受（`cli.invalid_format`）：這套 SDK 沒有它們的編碼器，`--capabilities` 把它們報成
   `compiled: false`，而不是留給呼叫端去猜。
8. 每一步的失敗診斷還帶著它自己的座標，只在這一步真拿到了值時才出現：`target`（哪個目標，視窗是
   `0x…` 句柄、螢幕是裝置名，例如 `DISPLAY1`）、`backend`（哪條通道）、`stage`（`parse` / `match` / `plan` /
   `consent` / `capture` / `encode` / `write` / `stdout` / `report` 之一，與 code 一樣只追加不改名；`match.*` 用
   `match`，整批輸出名那兩條判據 `io.output_collision` 與 `cli.stdout_multiple_targets` 用 `plan`，影格在編碼這一步
   出的錯用 `encode`——並沒有一條 `encode.timeout`，編碼期預算用盡仍是 `capture.timeout`，靠 `stage` 分別；
   命令列的解析期錯誤根本不帶這個欄位）、`hresult`（`0x80070005` 這樣的原值）、`win32`（`GetLastError` 的原值）。
   `message` 隨 `--lang` 變，這幾個不變。授權這一關的錯自成一類：`capture.access_denied` 是回答不是「是」（這個 `MB_YESNO` 框上，人能給出的唯一拒絕就是答「否」，而工具把任何非「是」的結果都當成拒絕），`capture.consent_unavailable` 是這台機器根本沒有可互動的桌面、框彈不出來——兩者都是
   退出碼 `6`，都帶 `stage=consent`、`target`、`backend`（通道）與 `value`（那條路徑），也都不是技術性的存取被拒
   （`capture.failed` 帶 `hresult=0x80070005`），這樣才分得開。確認之後目標挪了位置或變了大小給
   `capture.consent_stale`，`stage=capture`、退出碼 `7`，重新選目標再截就會再問一次。後端回傳的 HRESULT / Win32
   錯誤碼會原樣帶出，不會被 `E_FAIL` 或 `E_NOINTERFACE` 頂掉真實的錯誤碼。黑影格不會被斷言成 DRM——文案只列出幾種可能。
   單色影格也不會被斷言成「沒截到」：圖照常交付，另外留一條 `note.frame_uniform`（把那個顏色、那條通道、
   那個目標寫清楚）。只有 `duplication` 還會因為單色拒絕一影格，而且必須兩條一起成立——這一個影格沒有任何
   present 記錄，且整幅只有一個顏色。

### 標準輸出圖片位元組的 shell 差別

`--out -` 同時要兩條串流：stdout 上是 PNG/JPEG 位元組，stderr 上是整份 JSON 文件。工具自己兩條都不做文字
轉換——圖片位元組是用 `WriteFile` 寫那條裸控制代碼，文字一個字元都不碰它——所以圖片是在工具的下游、也就是
shell 的重導向裡被弄壞的。兩條串流也不是同一種資料：JSON 是被 shell 重新編碼也無害的文字，圖片是位元組流，
換掉一個位元組這張圖就廢了。因此第一個要問的問題是「這個 shell 保不保留原生命令的位元組流」（一張解不開的檔案
也可能另有原因——寫入被截斷、管線只開了一半——所以先確認確實是重導向的問題，別一上來就怪到它頭上）：

| Shell                  | 把原生命令的 stdout 重導向到檔案時                                                                                                                                                                                    | 該怎麼辦                                                                                                                                          |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cmd.exe`              | 逐位元組無損（`1>` / `2>` 是把真正的檔案控制代碼重接）                                                                                                                                                                | 照寫即可                                                                                                                                          |
| PowerShell 7.4 起      | 逐位元組無損——7.4 起的官方文件就規定重導向運算元保留原生命令 stdout 的位元組流                                                                                                                                        | 照寫即可                                                                                                                                          |
| PowerShell 7.0 – 7.3   | stdout 仍經文字管線解碼                                                                                                                                                                                               | 用 `cmd /c`，或 `--out <檔案>`                                                                                                                    |
| Windows PowerShell 5.1 | **會損壞**：位元組先被當文字解碼、再以 UTF-16LE 寫出，所以 NUL 與所有 ≥0x80 的位元組在檔案寫出來之前就已經遺失；連重導向出來的 stderr 檔案都要套上 PowerShell 自己的錯誤記錄排版（`node : ` 那種前綴、腳本位置、BOM） | 用 `--out <檔案>`，或 `cmd /c`，或 `Start-Process -RedirectStandardOutput … -RedirectStandardError …`（逐位元組無損，因為接控制代碼的是作業系統） |

```cmd
:: cmd.exe：stdout 出圖片、stderr 出 JSON，兩邊都逐位元組無損
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json
```

```powershell
# 只有 PowerShell 7.4 起適用：這兩行會保住位元組流。
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# 在 Windows PowerShell 5.1 下同樣這兩行不行：原生 stdout 先被當文字解碼、再以 UTF-16LE 重寫，
# NUL 與所有 >= 0x80 的位元組在檔案寫出來之前就已經遺失。想覆核這個機制不必真截圖，用一段固定的、
# 不含敏感資料的位元組流即可——重導向 `ECAPTURE.EXE --version` 再比對：在 cmd 下檔案是原樣的 ASCII
# 位元組（`1>` 重接了控制代碼），在 5.1 下同一份輸出會拉長約一倍且是 UTF-16LE，任何 PNG 解碼器都不會認。
# 所以 5.1 下要一個檔案，或改用 Start-Process（控制代碼由作業系統接上，逐位元組無損）：
$p = Start-Process -FilePath 'D:\tools\ECAPTURE.EXE' `
  -ArgumentList '--process','notepad.exe','--yes','--out','-' `
  -NoNewWindow -Wait -PassThru `
  -RedirectStandardOutput 'D:\shots\snap.png' -RedirectStandardError 'D:\shots\result.json'
$p.ExitCode   # 退出碼在這裡讀；只有 -Wait 並不會把它回報出來
```

`2>&1`（以及 `*>`）在上面哪一種 shell 裡都不是答案：把兩條串流合併，shell 就會把結果當成字串資料處理，圖片
位元組隨之消失；而在 5.1 上，原生 stderr 被套成 PowerShell 自己的錯誤記錄是發生在進入管線的那一刻，所以分開寫
`1>` 與 `2>` 也救不回那份 JSON。要同時拿圖片和 JSON 就把它們寫進兩個不同的檔案——或者改要一個檔案、讓 JSON
留在 stdout，那才是這個工具的預設走法。

## 視窗內部裁剪（`--roi` / `--client-area`）

這兩條選項回答的是同一件事：**這次交付的視窗畫面裡要留哪一塊**。它們互斥（`cli.crop_conflict` + 退出碼 1），
配整張螢幕的目標說不通（`capture.unsupported` + 退出碼 1 —— 螢幕上沒有一扇視窗可以讓座標相對它的左上角去算），
與唯讀查詢一起給出也算衝突（`cli.query_conflict` / `cli.window_query_conflict`）。

### 座標系，以及為什麼它永遠不是桌面絕對座標

原點 `(0,0)` 是**交付的整張視窗畫面自己的左上角那個画素**，右下邊不含。那張畫面就是使用者實際看到的那圈視窗可見邊框
（`DWMWA_EXTENDED_FRAME_BOUNDS`）：`GetWindowRect` 還算在內的 DWM 透明 resize 邊框不在畫面裡，而 `--client-area`
是在這塊之上再去掉標題列與三邊邊框。

單位是**物理画素，不依 DPI 縮放**。本程序宣告了 per-monitor DPI v2，視窗矩形、影格尺寸與画素緩衝本來就都在物理画素
那一套系裡，這裡沒有縮放這一步：同一條 `--roi 0,0,200,120` 在縮放一倍與兩倍的螢幕上取的都是 200×120 個画素。
要按邏輯画素（DIP）指定的呼叫端自己乘那道縮放 —— 本工具不猜這扇視窗在哪張螢幕上，也不猜該用哪邊的 DPI。

矩形錨定在畫面上，所以**它絕不會被當成桌面絕對座標重新解釋**。這不是措辭上的選擇：拿桌面座標來截，等於允許
呼叫端去要一塊誰都沒批准過的畫面。

`--roi` 要正好四個十進位數、逗號分隔（只認 `[0-9]+`：不要正負號、不要空白、不要小數點、不要指數、不要底線、
不要 `0x`，也不認非 ASCII 數字）。`x` 與 `y` 可以是 `0`；`w` 與 `h` 至少 `1`；四條都不超過 `16384` —— 那與影格的
單邊上限是同一條線，`--capabilities` 把它報成 `limits.roiMaxValue`，所以說明、解析與查詢讀的是同一個數。

### 放不下就是拒絕，不替你「修好」

| 情況                                                                                       | 碼                         | 退出碼 | `stage` |
| ------------------------------------------------------------------------------------------ | -------------------------- | ------ | ------- |
| 四段寫法不合（正負號、空白、段數不對、零寬或零高、超過上限）                               | `cli.invalid_value`        | 1      | parse   |
| 選定那一刻視窗就裝不下這條矩形，於是在彈框之前、輸出名規劃之前                             | `match.roi_out_of_range`   | 1      | match   |
| 取到影格才發現裝不下（目標在這中間改了尺寸，或有一邊掛在螢幕之外）                         | `capture.roi_invalid`      | 7      | capture |
| 定位這條矩形所需要的那一問沒有答案（客戶區量不出來，或這塊畫面核實不出它對應螢幕上的一塊） | `capture.roi_unmeasurable` | 7      | capture |

這四種都沒有「往裡挪一挪」「裁到邊上為止」「那就整張交出」這種下一步 —— 最後那一種等於交出一張呼叫端沒要求的圖。
也都不落地：取影格之前那一道排在確認框之前，所以一條注定裁不出來的請求不會先去打擾人；取到影格之後那一道把影格丟掉。
一批就是一批：命中的幾扇視窗裡只要有一扇裝不下，整批一張都不截（與 `match.index_out_of_range` 同一條規矩）。
`--dry-run` 不取影格，所以它也不判裁剪幾何 —— 無論判不判，`-v` 都會把請求回顯在 `input.crop` 裡。

### 裁剪不改變人批准過的那一片

裁剪排在取影格**之後**，這正是它不能碰到從未擺上桌面的画素的理由：一個請求屬於哪一級，仍然只由 `images[].path`
決定（見[截圖授權與 --yes](#截圖授權與---yes)）——桌面取樣那條配 `--roi 0,0,8,8`，問人問得和截整張螢幕一樣，
`--yes` 照舊只管得著那三條視窗內容路徑。

### 結果裡寫了什麼

一張裁過的圖多帶一組欄位，它們都是定位判據，`--quiet` 不許藏：

```json
"width": 120, "height": 80,
"cropMode": "roi",
"cropRect":       { "x": 20, "y": 40, "width": 120, "height": 80 },
"fullWidth": 486, "fullHeight": 293,
"cropScreenRect": { "x": 227, "y": 240, "width": 120, "height": 80 }
```

`cropRect` 是畫面画素座標，`width` / `height` 是裁完的最終尺寸，`fullWidth` / `fullHeight` 是裁之前那張整張畫面的
尺寸，`cropScreenRect` 是同一條矩形在**虛擬螢幕座標**裡的那一塊 —— 與 `rect`、`requestedRect`、以及確認框上列出的
區域在同一套系裡。這條映射是閉合的：`cropScreenRect − cropRect` 就是這塊畫面自己的螢幕原點，呼叫端可以拿 `rect`
去核對它，而不是必須相信它。

那一行**只在畫面原點核實得出來時才寫**：要嘛這條通道自己報了它實際截到的那一塊（`capturedRect`，桌面裁切那幾條
就是這麼做的），要嘛那一刻量到的可見矩形與交付尺寸完全相同。核實不出來就整個鍵不出現，並留一條
`note.crop_mapping_unavailable` 說清楚 —— 問不出來的東西不會被折成一個看起來合理的數。
`--client-area` 本來就要靠這條映射才知道客戶區在圖裡落在哪兒，所以映射問不出來時它直接失敗
（`capture.roi_unmeasurable`），而不是交回一張沒裁的整張畫面。

這幾個欄位與 `requestedRect` / `capturedRect` / `clipped` / `rotation` 並存而不重複：前一組說的是**整扇視窗**在
桌面上有沒有被完整截到，後一組說的是截回來那張圖裡要交出哪一塊。

### 本機判不了的兩條

`.\tests\crop.ps1` 用三條自己獨立問出來的 Win32 事實（`GetWindowRect`、`DWMWA_EXTENDED_FRAME_BOUNDS`、
`GetClientRect` + `ClientToScreen`）對照幾何，用「裁剪圖的四個角必須等於整張圖對應偏移處的画素」對照內容，
用真的改過尺寸的視窗判失效，用桌面通道 + 極小 `--roi` 判「一定彈框、沒點頭就不落地」（測試一側只探測有沒有彈出
確認框，從不代答）。兩條本機造不出、照實記未驗證而不是推導：同一條 `--roi` 在兩張縮放比不同的螢幕上
（這台機器只接了一張螢幕），以及目標在「取影格之前的預檢通過之後、影格交回來之前」這一瞬間被改小
（要的就是這段競態本身，時間點安排不出來 —— 它由離線層 `build\ecapture-crop-tests.exe` 逐條判）。

## 等比縮小（`--scale`）

`--scale max-width=N,max-height=N,max-pixels=N` 把工具即將交付的那張圖縮到這幾條上限之內。三個鍵互相獨立（可以只給一個，
也可以一條裡用逗號串幾個；這一條寫多次時每條上限各記各的，重複給同一條時最後一個生效）；`--scale` 什麼都不寫是引數錯誤，
因為「一條上限都沒給」與「縮到 0」沒法區分。N 只認十進位，邊長 1..16384（與影格的單邊上限同一條線），像素數 1..268435456。

規矩很短，而它們就是全部契約：

- **一個比例，取最緊的那一條上限。** 每條上限各提出一個比例（寬、高，像素預算則開方），取最小的那一個。寬高各自**向下取整**，
  且各至少留 1 像素。
- **絕不放大。** 本來就在每條上限之內的圖照原樣交付，結果裡寫 `scaleApplied: false`。沒有哪條上限真的比圖更緊時，
  這一條什麼都不會「減少」。
- **插值策略只有一種，而且可預測**：最近鄰（`scaleMethod` 恆為 `nearest`）。交付像素 `(x,y)` 取自縮之前那張圖的
  `(floor(x*scaleFromWidth/width), floor(y*scaleFromHeight/height))`。沒有浮點取樣，也不按通道挑演算法。
- **先裁、後縮、再編碼。** `scaleFromWidth` / `scaleFromHeight` 是**裁之後**那張圖的尺寸（不是整張視窗畫面），`cropRect` /
  `cropScreenRect` 一字不改，`width` / `height` 是最終尺寸。單色品質提示判的是交出去那張（也就是縮過的）。
- **授權那一層一點不動。** 縮小排在授權之後，它不是降低請求風險等級的辦法：屬哪一級仍然只由 `images[].path` 決定
  （見[截圖授權與 --yes](#截圖授權與---yes)），所以桌面像素那條配 `--scale max-width=8`，即使給了 `--yes` 也照樣彈框。
  它同樣不是繞開既有上限的辦法——影格大到過不了影格形狀檢查的根本到不了這一步，`--roi` 仍按未縮的那張圖判。
- **只有寫過這一條時**，`scaleMethod` / `scaleApplied` / `scaleFromWidth` / `scaleFromHeight` 才出現；沒寫 `--scale` 時這四個鍵
  一個都沒有（不是 `null`、`0` 或 `false`），所以那條流與之前逐位元組相同。
- 編碼那一層沒動：`.\tests\scale.ps1` 在真機上判的是 `png`、`bmp`、`jpeg` 三種格式寫下的都是縮過之後的尺寸。
  `tiff` 與 `gif` 走同一個編碼呼叫、只是換了 encoder id，那條測試沒有涵蓋它們。

本機未驗證：單邊超過 16384 的影格的真機現場（造不出一扇那麼大的視窗，而且那種影格本來也過不了影格形狀檢查——這條由離線層判）、
HDR 與縮小同時生效（這台開發機開不了 HDR）、跨螢幕混合 DPI（只接了一塊螢幕）。

## 不給輸出路徑（相容性說明）

沒給輸出路徑與顯式寫 `--out -` 是**同一個請求**：圖片按 png 走 stdout，JSON 整份走 stderr，而每條診斷都是
那一步真實產生的那一條。

- 這條路上原樣的 code 與原樣的退出碼照舊交回：`match.no_window`（4）、`match.ambiguous_window`（5）、
  `capture.access_denied`（6）、`capture.failed`（7）、`io.write_failed`（8）、`cli.invalid_number`（1）……
  較早的一個建置曾在這裡把整段換成一條 `cli.missing_output` + 退出碼 `1`，還清空 `images` 與 `notes`，
  既藏起真實原因也丟掉已經交付的圖；那層改寫已經拿掉了。`cli.missing_output` 不再產生，這條 code 保留不下來
  複用，為的是它不被挪作別的含義。
- 已經送到 stdout 的圖仍留在 `images` 裡，`captured` 照實計數，所以這條路上也看得見部分成功。
- 「這次沒給輸出路徑」這句話現在只是一條 `hint`，而且只出現在「補上檔名確實繞得開這次故障」的那一條上：
  隱式 stdout 且 `io.write_failed` + `stage=stdout`。寫 `--out -` 的人本來就選定了這條管道，所以那裡不說這句話。

怎麼分支：讀 `errors[].code`（以及它的 `stage` / `target` / `backend` / `hresult` / `win32`），
既不要只看退出碼，也不要根據「有沒有給 `--out`」推斷原因。

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
**歷史副本失敗也是這一種部分成功**：圖已經按 `--out` 交出去了，而 `images[].history.status` 寫著 `failed`
（或 `skipped`）並且 `errors` 裡多一條 `history.*`（`stage` = `history`）時，退出碼是 `7` 而不是 `0`，
也絕不是「什麼都沒寫」的 `8`——主圖不會被刪，已發出的標準輸出不會被回滾，呼叫端也不該為此重拍一次（見
[截圖歷史歸檔](#截圖歷史歸檔預設開啟)）。

唯讀查詢那三條（`--capabilities` / `--diagnostics` / `--screens`）只用 `0` 與 `1` 兩個編號：`0` = 這份文件出完了（哪怕裡面寫著
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

| 層次                    | 取值                                                                                                   | 這個數是從哪來的                                                                                                                                                                                                                                                                                                                                           |
| ----------------------- | ------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 各條路徑的 API 歷史下限 | 任何一張影像 10.0.10240 · `duplication` 10.0.9200 · `printwindow` / `dwm` 10.0.9600 · `wgc` 10.0.18362 | 微軟為這條路徑**實際呼叫**的那個介面所寫的下限。編碼器（WinRT `BitmapEncoder`）六條通道、五種格式共用；`wgc` 走的是 `IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor`，那是 Windows 10 版本 1903 才有的互通操作介面——`Windows.Graphics.Capture` 這個命名空間本身確實是 1803 出現的，但本工具沒有「讓使用者在系統選取器裡點一下」那條退路 |
| 本工具聲明的下限        | 64 位元 Windows 10 版本 1903（內部版本 18362）或更新                                                   | 上面那幾道裡最高的那一條，因為預設通道要真能截到視窗需要它；不是「某條路徑碰到的最老的那個 API」                                                                                                                                                                                                                                                           |
| 已實測的版本            | Windows 10 版本 22H2（內部版本 19045），x64                                                            | `tests\` 下所有真機判準都只跑在這一台機器上。`--capabilities` 是拿**本機環境**與這份記錄比對，比出來的答案就叫 `verifiedOnThisMachine`；比得上是一份環境相符的判定，不是「這台裝置曾經被實測過」的證明                                                                                                                                                     |

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

| 碼                         | 什麼時候                                              | 退出碼 | 換通道有沒有用                                                                                         |
| -------------------------- | ----------------------------------------------------- | ------ | ------------------------------------------------------------------------------------------------------ |
| `env.os_too_old`           | 版本低於 10240：所有格式共用那唯一一套編碼器不在      | 7      | **沒有。** 這不是通道的問題，也不是目標的問題——這台機器上做不出任何一張影像                            |
| `env.channel_unsupported`  | 顯式指定的那條通道的下限高於本機版本                  | 7      | **有**——換 `--capture` 取值或改用 `auto`。重試同一個目標沒有意義，而工具不會自己把你指定的那條換成別的 |
| `note.channel_unavailable` | `auto` 鏈裡某條通道的下限高於本機版本，它已從鏈中移除 | 不變   | 影像仍可能由別的那幾條截到，`images[].source` 寫的是實際出圖的那條                                     |
| `note.os_unverifiable`     | 內部版本压根問不出來                                  | 不變   | 這一次沒有按版本篩過任何一條——問不出來既不等於不支援，也不等於支援                                     |

`--verbose` 會回顯 `input.osBuild` 與 `input.captureChain`（針對本次這類目標，這台機器實際給得出哪幾條通道），
所以呼叫端（含 AI）不必先截圖就能把能力問出來。`--dry-run` 不取影格，因此不因環境判準報錯。

裝置層面的能力刻意不去預測：驅動不餵桌面複製影格、這台機器拒絕 Windows.Graphics.Capture、工作階段裡沒有可互動
桌面、N 版缺媒體元件——這些都不在版本號裡，也都不由本工具提前猜；那一步會交回它自己的 `capture.*` 碼與
真實 HRESULT。

### 處理序完整性層級

**這一檔的憑證只有一個**：`--capabilities` / `--diagnostics` 裡 `session.integrityLevel`，它問的是 ECAPTURE
自己那個處理程序那一檔。「目錄帶著 `Mandatory Label\Low Mandatory Level` 這條明示標籤」、「目錄裡的檔案繼承到的標籤」
與「你此刻那個 shell 的 `whoami /groups`」是三條互不等價的事實——前兩條是*可能*把從那條路徑啟動的處理程序
帶到更低那一檔的成因，第三條說的是另一個處理程序。低完整性處理程序向中完整性的目錄建立檔案會被拒
（`io.write_failed` + Win32 `5`），而**存取被拒的成因不只這一條**：同一對碼也可能出自佔用、路徑本身不合
或那個目錄真的不讓寫。所以這一節說的是「低於 medium 時*通常*壞在哪幾類事上」，不是一張「看到這個碼就一定是
這個原因」的對照表。從這條路徑啟動的處理程序被帶到低於 medium 時，壞掉的通常是這三類彼此無關的事，而它們過去
都只報成一句普通的「採集失敗／寫入失敗」，呼叫端據此換後端、加期限、重試，全是白做功：

- `wgc`：取影格那一步被拒，`GraphicsCaptureItem.CreateForWindow` 回 `E_ACCESSDENIED`（`0x80070005`）
- `printwindow`：被拒，Win32 錯誤碼 `5`——低完整性處理程序不能向中完整性視窗發那條跨處理程序繪製訊息（UIPI）
- 落盤：向中完整性的目錄建立檔案會被拒，於是報 `io.write_failed` + `5`，而**影像已經在記憶體裡拿到了**；寫進同樣帶這條標籤的目錄就成功

這一檔在本機上沒擋掉的：`--capture dwm`（`dwm.thumbnail` 那條）照常出圖，前提是輸出目錄讓低完整性處理程序建檔；
`bitblt.screen` / `duplication.frame` 也照常出圖——這兩條本來就要人親自批准，屬另一層的事（見《截圖授權與 --yes》）。
這兩句是**這台開發機上實測過的一次結果**，不是對所有機器的承諾：低完整性不是「這台機器截不了圖」，這裡也沒說換一條
就一定成功；而 `dwm` 這一條在視窗自己拿不到畫面時會**升級到讀桌面像素那條退路**，那一條照舊一定彈框問人——
授權分級不因完整性層級而改變。`--capabilities` 裡各條通道的 `status` 不因這一檔被改動，那個欄位管的是版本下限與
螢幕拓撲。實測範圍只有這台 19045 開發機（Windows 10 版本 22H2 x64，2026-10-09），不是普遍結論：逐位元組相同的 exe
放在 `%UserProfile%\.agents` 及其子目錄裡失敗，放在試過的其他位置（`%LOCALAPPDATA%\Temp`、`%APPDATA%\Roaming`、
`%UserProfile%`、本倉庫的 `build\`）全部正常。核對時兩條要分開看：

```powershell
icacls "%UserProfile%\.agents"                # 那個目錄帶不帶 Mandatory Label\Low Mandatory Level（一條成因線索）
whoami /groups | findstr /i "Mandatory Label"   # 此刻這個 shell 自己那一檔（本機 S-1-16-8192 = Medium），不等於 ECAPTURE 那一檔
ECAPTURE.EXE --capabilities | findstr integrityLevel   # 本工具那個處理程序的證據：低於 medium 時這裡寫著 low
```

現在程式會把這一檔報出來：`--capabilities` / `--diagnostics` 的 `session` 段帶 `integrityLevel`（ASCII token：`unknown` / `untrusted` / `low` / `medium` / `high` / `system` / `protected_process`；問不出來時是 `unknown`，既不報成夠用也不報成被降級），低於 `medium` 時 `caveats` 多一條 `process_integrity_below_medium`，`--diagnostics -v` 的 probes 段多一問 `tokenIntegrityLevel`。真的截圖時，本處理程序低於 `medium` 且這一輪裡有「系統不讓」那類失敗（HRESULT `0x80070005` 或 Win32 `5`，發生在取影格／寫檔案／寫標準輸出這三步上），這些失敗各自的 `hint` 就追加一句可操作的說明，整輪另追加一條 `notes` 裡的 `note.low_integrity`（帶 `target` / `backend` / `stage`）。`code`、`stage`、`hresult` / `win32`、退出碼與已經交付的影像全部原樣不動，按 `code` 分支的老呼叫端不受影響；`--yes` 與授權分級也不受影響。`note.low_integrity` 屬於 notes，`--quiet` 會隱藏它，補寫進 errors 的 `hint` 則不受 `--quiet` 影響。

能做的三件事，按代價由小到大：

1. 把 `--out` 指向同樣帶這條標籤的目錄裡的路徑（工具從不建立目錄，那一層要先存在）
2. 要視窗自己的畫面時改用 `--capture dwm`（這台開發機實測這一檔下它照常出圖；這一條不是對所有機器的承諾，
   而它內部升級到桌面像素那條退路時照樣一定彈框問人）
3. 換一個沒有被這樣降級的安裝位置，或由那個目錄的所有者去掉這條標籤——**改標籤、改 ACL、改安裝位置、
   改完整性層級都不在本工具會自動做的範圍裡**，那屬改動系統安全設定，要誰做誰自己單獨決定並授權；
   本工具既不提權，也不改這些設定來「讓截圖通過」

**以系統管理員身分執行不是這條提示的解法**，這條訊息也沒有建議那麼做。

### 唯讀的能力查詢（`--capabilities` / `--diagnostics`）

上面那套判準在截圖**開始之前**就會起作用，但只有真的下單一次之後才看得見。這兩條命令是同一套判準的唯讀出口：一條像素都不取、一個確認框都不彈、一個檔案都不寫、不連網、不讀環境變數，也不需要視窗條件。

```powershell
ECAPTURE.EXE --capabilities              # 這台機器現在能走哪幾條路線（JSON）
ECAPTURE.EXE --diagnostics               # 建置版本 + 可核對的建置識別碼 + 後端狀態（JSON）
ECAPTURE.EXE --capabilities -v           # 另加 probes 段：每一問的原始答案與它是從哪個 API 問來的
```

三條規矩：

- **三件事分開寫。** `compiled` 說的是這個二進位檔裡有沒有實現那條路線；`status` 說的是本機自己那份證據（版本下限 + 螢幕拓撲）現在讓不讓走；`verifiedOnThisMachine` 是**與本機環境比對**，不是一台一台裝置的實測記錄：只有本機的 Windows 內部版本與架構等於本專案跑過真機判準的那套環境（內部版本 19045、x64）才是 `yes`，兩邊任一項問不出來就是 `unknown`，其餘就是 `no`——這正是同一份文件在 26100 的機器上讀作 `no`、並附上 `this_environment_not_tested` 這條 caveat 的原因。`os.matchesTestedEnvironment` 用的就是這同一道判準。三者互不冒充：`available` 配 `verifiedOnThisMachine: no` 說的是「這個建置在本機走得了這條路，而本專案只在另一種版本上證過」；至於 `yes`，它照樣不保證某一扇視窗這一次截得到。
- **問不出來就說問不出來。** 每一條事實都是 `yes` / `no` / `unknown` 三值之一，`unknown` 既不折成「能」也不折成「不能」，那個鍵也不會整個消失。版本問不出來時所有 `status` 都是 `unverified`，而 `autoChainWindow` 仍原樣列出全部四條——沒篩就是沒篩，不是「都支援」。
- **`available` 不是保證。** 它不含「某個視窗這一次一定截得到」那層意思：驅動、受保護內容、HDR 都不在這層的斷言裡。文件末尾的 `caveats` 陣列就是把「這份報告沒說過什麼」逐條列出來。

| 段                                    | 內容                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `contract` / `contractVersion`        | 只有這兩份文件帶契約版本（目前是 1）。普通截圖 JSON 仍照《輸出形式》保持精簡，不因此多出任何頂層元資訊                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `program`                             | 名稱、`ECAPTURE.EXE` 這個檔案名本身（不含目錄）、版本、架構、`buildId`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `os`                                  | 本機內部版本（`known` 為假時那一組數字寫成 `unknown`）、`declaredMinBuild`（對外聲明的下限）、`encoderMinBuild`、`testedMinBuild` + `testedArch`（本專案真的測過的那種環境）、`matchesTestedEnvironment`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `session`                             | 是否接在控制台工作階段、是否遠端桌面、有沒有可用的螢幕拓撲與有幾塊螢幕、本行程是否被提升過、本行程那一檔強制完整性層級（ASCII token `integrityLevel`）、`consentDialogExpected`（推出來的，`consentDialogProbed: false` 明說沒有真去彈框）                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `authorization`                       | `yesSkips: "window-content"`、`desktopPixelsAlwaysAsk: true`、未登記的路徑按 `desktop` 處理，外加整份內部路徑登記表（每條帶 `scope` 與 `consentWithoutYes` / `consentWithYes`）——就是《截圖授權與 --yes》那張表的機器可讀版本                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `backends`                            | 每條路線：`compiled` / `status` / `reason` / `minBuild` / `verifiedOnThisMachine`，以及它在視窗目標與螢幕目標上各走哪條內部路徑（`dwm` 那條螢幕退路也在，所以 `--yes` 的適用範圍不會被人讀大）                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `formats`                             | 每種格式：`compiled` / `status` / `reason` / `minBuild` / `registered`，`png` / `jpeg` / `bmp` / `tiff` / `gif` 五種各一行——五種都編進了這個建置，而且每一種走的都是同一個 WinRT `BitmapEncoder` 呼叫、只換 encoder id。`registered` 恆為 `unknown`，因為這層不去實測編碼器（實測就是「用一次編碼來探能力」，與「不靠截圖探測」是同條理由）；曾經列過但沒有編碼器的 `webp` / `ico` 以 `compiled: false` + `reason: "not_compiled"` 留在這裡，好讓呼叫方拿到確定答案                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `cursor`                              | `--cursor` 這一條的故事：`default`（不給這條選項時的下場）、三種取值、那唯一一條開關寫成 `compiled` / `status` / `reason` / `minBuild`（19041）/ `verifiedOnThisMachine`，然後每條已登記的內部路徑一行（`capability` 是 `settable` / `excludes_cursor` / `pointer_state_unverified` / `unregistered`，加 `reason` 與 `include` / `exclude` 各三值 `yes` / `no` / `unknown`），末尾 `pointerShapeCompositing: "never"` 與 `pixelRetouching: "never"`。duplication 那兩條是 `capability: pointer_state_unverified`、`include` 與 `exclude` 都是 `no`——桌面那一影格可能已經把指標畫在裡面，而本工具從不合成指標形狀，這證明不了影格裡沒有指標。沒登記的路徑讀 `unknown` 而不是猜一個答案                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| `color`                               | `--hdr` 這一條的故事：`default`（不給這條選項時的下場）、三種取值（`auto` / `tonemap` / `refuse`）、`compiled` / `status` / `reason` / `verifiedOnThisMachine`。`status` 說的是「這個建置帶不帶得回廣色域影格 + 怎麼對映」，**不**去問那塊螢幕此刻是不是 HDR 模式（reason 是 `hdr_display_mode_not_probed`）；`verifiedOnThisMachine` 恆為 `no`（本專案沒有 HDR 螢幕，不宣稱色彩驗收通過）。每條已登記的內部路徑一行，`capability` 分三檔：`wide_gamut_capable`（只有 `wgc` / `screen.wgc`）、`wide_gamut_unverified`（`duplication.frame` / `screen.duplication`：那塊桌面影格確實可能以 FP16 或 10 位元回來，但本建置在 `DuplicateOutput` 之前從不問顯示器的色彩空間，所以既證明不了拿到的是什麼、也就兌現不了策略）、`sdr_source_only`（凡是讀 8 位元 DC 的那幾條），另有 `unregistered`；再加上 `honorsExplicitPolicy`——只有那兩條 `wgc` 是 `true`，也就是「本建置裡兌現得了 `tonemap` / `refuse` 的只有 wgc」這句話的機器可讀版本。最後是 `toneMapping` / `floatIntermediateFrame: "per_pixel_registers"` / `encoderOutput: "sdr_bgra8"`（HDR 一律對映成 8 位元 SDR 交付，不出 HDR 原生圖）                              |
| `autoChainWindow` / `autoChainScreen` | 本機現在能試的 `auto` 鏈。與截圖那次 `-v` 回顯的 `input.captureChain` 由**同一個** `GateChannels` 算出，`tests\capabilities.ps1` 逐條比對這兩處                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `history`                             | 截圖歷史歸檔的**規則**自述（預設開啟）：`enabledByDefault: true`、`relativeTo: "executable-directory"`（跟著實際執行的那一個 exe，不是工作目錄也不是 `--out` 的目錄）、`location: "history/YYYY-MM-DD/"`、`naming: "YYYYMMDD-HHMMSS-<pid>-<token>-<seq>.<ext>"`、`source: "same-encoded-bytes"`（副本用主交付那一份已編碼的位元組：不重拍、不重新編碼、不去讀主輸出檔案、不建硬連結）、`commit: "exclusive-create"`（獨佔提交，從不覆蓋既有歷史檔案）、`created: "after-first-delivered-image"`（只讀查詢與失敗的那些次一個目錄都不建）、`retention: "never-pruned-automatically"`、`uploads: false`、`backgroundPruning: false`、`writabilityProbed: false`，再加副本失敗時那一種部分成功的退出碼 `partialSuccessExit: 7`。這一段沒有一個欄位是「這一次寫成功了」那種斷言——那一句在截圖結果的 `images[].history` 裡，這裡連「那裡寫不寫得動」都沒去試（`caveats` 裡恆帶 `history_root_writability_not_probed`），而且全段是相對程式目錄的寫法，不含任何絕對路徑 |
| `limits`                              | 單邊像素上限、整影格位元組上限、`--timeout-ms` 上限、隔離開呼叫內建上限、WGC 影格池重建次數、編號與 PID 上限、`stdoutTargetsMax: 1`、JPEG 品質區間                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `privacy`                             | 自述這份查詢沒做的事：不擷取畫面、不彈框、不上傳、不列舉使用者檔案、不讀環境變數、不含使用者名、不含路徑                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `caveats`                             | 穩定的 ASCII token，列「這份報告沒有斷言什麼」：`available_is_not_a_guarantee`、`no_capture_performed`、`no_consent_dialog_shown`、`encoder_state_not_probed`、`device_capability_not_predicted`、`consent_dialog_state_inferred_not_probed`、`subsystem_version_is_linker_default`，以及依本機情況追加的 `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `process_integrity_below_medium` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown`，以及恆有的 `cursor_effective_is_a_setting_not_a_pixel_check` + `pointer_shape_never_composited_nor_erased`（滑鼠指標那幾個欄位只說得到設定與來源那一層，說不到「這一個圖裡看得見或看不見指標」），以及恆有的 `hdr_tone_mapping_not_verified_on_hdr_display` + `hdr_output_is_tone_mapped_to_sdr_bgra8` + `hdr_explicit_policy_only_fulfilled_by_wgc`（HDR 的對映數學離線判過但沒有 HDR 螢幕實測、HDR 一律被對映成 8 位元 SDR 交付，而明確寫出的 `tonemap` / `refuse` 要求只有那兩條 `wgc` 路徑兌現得了），以及恆有的 `history_root_writability_not_probed`（`history` 那一段說的是歸檔規則，這份查詢沒去試過寫那個位置，也不預測某一次落盤必然成功） |

兩份文件由**同一個**判準函式（`src/EnvReport.cpp` 的 `BuildEnvReport`）算出，只差段落取捨：`--diagnostics` 固定帶 `build` 那一段（PE 連結時間戳、機器類型、映像大小、子系統），`--capabilities` 只在 `--verbose` 時展開它。版本號、`status`、後端清單、`limits` 都是同一份，所以不存在「兩份會互相打臉的環境資訊」。

**建置識別碼是可核對的**：`buildId` 是 `版本-架構-十六進位連結時間戳`，那一個時間戳與 `dumpbin /headers` 讀發布產物讀到的是同一個欄位，讀的是本行程自己已經對映進記憶體的那份 PE 標頭——不開檔案、不列舉目錄，所以也不會因為安裝路徑裡有使用者名而漏出身份。PE 標頭裡的 `subsystem version` 只作為事實列出，並配一條 `subsystem_version_is_linker_default` 的 caveat：那是 MSVC 連結器預設值，不是支援聲明。

`--capabilities` / `--diagnostics` 只接受 `--lang`、`-v`、`-q`：截圖那一套選項（視窗條件、`--monitor`、`--capture`、`--out` 與位置引數、`--yes`、`--dry-run`、兩條期限）與它們**同時給出就是 `cli.query_conflict` + 退出碼 1**，一次報全所有衝突項，一張都不截、一個檔案都不寫。這兩條命令也不參加「沒給條件就顯示說明」那一條：查詢本身就是明確的意圖。`-q` 對查詢只去掉 `caveats` 那一段，`-v` 加的是 `probes`，兩邊都不動答案本身。

這份文件從頭到尾是 ASCII（機器讀的取值一律不翻譯），所以同一台機器上換任何一種 `--lang`，輸出逐位元組相同。

### 結構化的視窗發現與檢查（`--list` / `--inspect`）

`--dry-run` 回答「這批條件到底命中了哪些視窗」的方式，是把每個候選寫成**一行給人看的話**塞在 `note.dry_run` 裡
（`hwnd=0x… pid=… 1261x614+681+22 class=… title=…`），於是呼叫端得從一句話裡把句柄、矩形、標題再解析出來 ——
而工具的契約從來沒有承諾這句話的形狀穩定。這兩條命令就是同一個問題的結構化答案。它們跑的是與截圖
**同一套**條件求值（同一選項寫多次取並集、不同選項取交集、
`--monitor` 按屏過濾，用了 `--title-regex` 或 `--timeout-ms` 時照舊整步進輔助程序），但不產任何圖片：

```powershell
ECAPTURE.EXE --list --process notepad.exe                    # 每個命中的視窗，結構化
ECAPTURE.EXE --list --class CabinetWClass --limit 5 --offset 5
ECAPTURE.EXE --list=all --title-contains 報告                # 把最小化的也列進去
ECAPTURE.EXE --inspect --hwnd 0x001A0B4C                     # 一扇視窗，逐項查清楚
ECAPTURE.EXE --inspect --process notepad.exe --topmost-match  # 與截圖完全同一套消歧
```

五條規矩，每一條都因為另一條做法更壞：

- **不取像素、不問人、不寫檔案。** 不調任何取幀通道，不彈確認框，不建檔案，不聯網，不讀環境變量 —— 文檔自己在
  `authorization` 段寫着（`pixelsRead: 0`、`consentDialogShown: false`、`filesWritten: false`）。它同樣**不動任何
  目標視窗**：不恢復、不激活、不改疊放次序 —— "我先看一眼開着什麼"不該改變屏幕上的樣子。`caveats` 裡的
  `no_capture_performed` 與 `no_window_touched` 就是釘這一條。
- **命中多扇不是截圖歧義。** `--list` 把它們分頁交回（`--offset` / `--limit`，本批預設 50 條），真實總數寫在
  `pagination.matched`，於是"這一頁很短"永遠不會被讀成"只有這些視窗"。一個都沒命中是正常答覆：`windows: []` +
  退出碼 `0`，不是 `match.no_window` + `4`。`--inspect` 需要一個目標，用的正是截圖那一條選擇策略：策略之後仍剩多扇
  就是 `match.ambiguous_window` + 退出碼 `5` —— 不替你選一個，也不會"先拿一扇看起來一樣的"。
- **列表是一份快照，會過期。** 句柄會被複用、標題會變、處理程序會退出，所以這裡的 `hwnd` / `pid` / 類名**不是**一種可以
  長期持有的憑證。每次成功的查詢都帶一條 `note.window_query_stale`，而每一行的 `identity` 段寫着
  `verificationRequired: true`、`isAuthorizationToken: false`、`raceWindowReducedNotEliminated: true`。真去截圖時
  仍在讀像素之前複核目標身份（那是 `capture.target_gone` / `capture.target_changed` /
  `capture.target_unverifiable`），確認框也照舊按像素來源判：**`--yes` 在這裡不起任何作用**
  （`authorization.yesAffectsResult: false`）—— 它既不會多解鎖一個欄位，也不會跳過一次本就不彈的框。
- **讀不到的欄位會說它讀不到。** 跨處理程序的問答有三種下場，逐欄位寫：`readable`、`denied`（系統擋下了這個調用方）、
  `failed`（問過而沒答案），後者帶原始 Win32 碼。讀不到的值是哨兵（`0` / 空串）**加上**這個狀態，不是把鍵悄悄省掉；
  文檔也不勸你改用管理員身份 —— `caveats` 裡寫着 `unreadable_fields_are_not_a_prediction`。
- **可見性策略寫出來，不讓調用方猜。** 不可見與零尺寸的視窗被排除（與截圖那一次枚舉同一條規則），`policy` 段就這麼
  說（`invisibleExcluded`、`zeroSizedExcluded`）；最小化視窗預設也不進列表，條數記在
  `policy.minimizedExcluded`，`--list=all` 把它們按同一根 Z 序軸並進來。這裡對"系統視窗"**不作任何斷言**：
  Windows 沒有一個"我是系統視窗"的屬性可問，所以 `policy.systemWindowAssertion` 是 `false`。

每行的欄位如下（`--inspect` 把 `windows` 數組換成單個 `window` 對象，其餘欄位完全同一形狀）：

```json
{
  "contract": "windowquery",
  "contractVersion": 1,
  "query": "list",
  "program": { "version": "0.4.0" },
  "authorization": {
    "readOnly": true,
    "pixelsRead": 0,
    "consentDialogShown": false,
    "filesWritten": false,
    "yesAffectsResult": false,
    "identityFieldsAreNotConsent": true
  },
  "policy": {
    "invisibleExcluded": true,
    "zeroSizedExcluded": true,
    "minimizedIncluded": false,
    "minimizedExcluded": 2,
    "systemWindowAssertion": false,
    "order": "zOrder"
  },
  "pagination": {
    "offset": 0,
    "limit": 50,
    "limitDefaulted": true,
    "defaultLimit": 50,
    "maxLimit": 8192,
    "matched": 52,
    "returned": 50,
    "truncated": true,
    "nextOffset": 50
  },
  "windows": [
    {
      "hwnd": "0x001A0B4C",
      "pid": 31468,
      "title": "…",
      "class": "CabinetWClass",
      "image": "explorer.exe",
      "rect": { "x": 681, "y": 22, "width": 1261, "height": 614 },
      "visible": true,
      "minimized": false,
      "zOrder": 3,
      "readability": {
        "process": { "state": "readable" },
        "imagePath": { "state": "denied", "win32": 5 },
        "processStart": { "state": "readable" },
        "rect": { "state": "readable" }
      },
      "identity": {
        "hwnd": "0x001A0B4C",
        "pid": 31468,
        "class": "CabinetWClass",
        "processStartTicks": 134351142668527401,
        "selectionNeedsRecheck": false,
        "verificationRequired": true,
        "isAuthorizationToken": false,
        "raceWindowReducedNotEliminated": true
      }
    }
  ],
  "caveats": [
    "no_capture_performed",
    "no_consent_dialog_shown",
    "no_window_touched",
    "snapshot_expires",
    "identity_fields_are_not_a_token",
    "invisible_and_zero_sized_excluded",
    "unreadable_fields_are_not_a_prediction",
    "list_may_be_partial"
  ]
}
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

### 唯讀的螢幕列舉（`--screens`）

在此之前，「要那一張螢幕」只有一種寫法：`--monitor <n>`，而那個 n 是**本次執行 `EnumDisplayMonitors` 列舉中的位置**。
它不是「顯示設定」裡 Windows 寫的那個識別號，拔掉重插或改一次解析度之後可能指到另一張面板——而猜錯的結果是一張
沒人批准過的畫面已經落盤。`--screens` 把這個問答成資料：

```powershell
ECAPTURE.EXE --screens                                  # 每張螢幕，連同它的幾種身分
ECAPTURE.EXE --monitor device:DISPLAY1 --out shot.png   # 按本次桌面連線的裝置名
ECAPTURE.EXE --monitor "id:\?\DISPLAY#GSM41A2#5&…#{…}" --out shot.png   # 按跨工作階段的螢幕裝置路徑
```

它與其他唯讀查詢一樣唯讀：不取一個影格、不彈確認框、不寫檔案、不連網，也不改任何顯示設定——為了搞清「這張螢幕
轉了多少度」去呼叫 `SetDisplayConfig`，等於把考卷改了再答題。這份文件是第三份契約（`screens`，版本 1），與截圖
那份精簡 JSON 各自獨立，就像 `capabilities` 與 `windowquery` 一樣。

每張螢幕有四種身分，各自寫明它穩到哪一層（寫成欄位，不是正文）：

| 欄位                | 是什麼                        | 穩定範圍                 | 選擇器           |
| ------------------- | ----------------------------- | ------------------------ | ---------------- |
| `ordinal`           | 本次列舉中的位置              | `this_invocation`        | `--monitor <n>`  |
| `deviceName`        | GDI 檢視裝置名 `\\.\DISPLAY1` | `this_desktop_attach`    | `device:`        |
| `monitorDevicePath` | 螢幕 devnode 的裝置介面路徑   | `cross_session_expected` | `id:`            |
| `adapterLuid`       | 配接器的本機唯一識別          | `this_session`           | 無——只作關聯資訊 |

`screens[].selectors` 直接給出可以原樣抄回去的那兩條（`device:DISPLAY1`、`id:\?\DISPLAY#…`），`identity.*` 寫明
哪一種能當選擇器用。配接器 LUID 故意沒有選擇器寫法：它只在工作階段內唯一，拿它點名一張螢幕是下注而不是引用。
`adapter.devicePath`（配接器自己的裝置介面路徑）、`adapter.outputTechnology`、`targetId`、`targetAvailable` 來自
同一次問答，「這張螢幕掛在哪塊顯示卡上」就是這樣交回的。

這一層守三條規矩：

- **絕不換成另一張螢幕。** 識別在本機不存在 = `match.monitor_unknown_id`（結束碼 4）；同一識別命中多張 =
  `match.monitor_ambiguous_id`（5，候選全部列出，工具不替你挑一張）；那一問沒給出答案 =
  `match.monitor_id_unverifiable`（7，`hint` 明說換 `--capture` 不是下一步——這一路根本沒選過通道）。
  三條都不會悄悄退化成「那就用主螢幕」：沒人批准的整張螢幕畫面正是確認框要擋住的東西。
- **問不出來就說問不出來，不寫成空值。** `dpi`（有效值與原始值，走 `shcore!GetDpiForMonitor`，Win8.1 起）、
  `rotation.degrees`（人看到的朝向，取目前 `DEVMODE`）與 `rotation.panel`（相對面板原生朝向，取顯示設定）
  是各自獨立的問題，各有自己的 `readability`（`readable` / `denied` / `failed`）與那一條 API 自己的錯誤碼。
  某個鍵不見了就只代表「沒答案」，原因寫在旁邊；這裡不建議改用管理員身分。
- **是快照，不是憑證。** 每次成功的清單都帶 `note.screen_query_stale`，`caveats` 裡有
  `device_names_are_not_persistent`、`cross_session_stability_not_tested`、`screen_capture_always_asks`。
  點名一張螢幕不取代取影格之前的身分複核，也不取代授權：桌面像素一定要人點頭，`--yes` 也不例外
  （見下面《截圖授權與 --yes》那一節）。

按識別點名在一切「要選一張螢幕」的地方都成立：與視窗條件同用（`--monitor id:… --class …` 就是按那張螢幕過濾視窗）、
與 `--list` / `--inspect` 同用（走的是同一個選擇函式），以及取影格之前的複核——它按**選定當時問得到的那條身分**核對，
裝置名若已屬於另一張面板就以 `capture.monitor_changed` 停下，複核本身問不出來就以 `capture.monitor_unverifiable`
停下，而不是退回去照名稱截。`--monitor <n>` 的語意一字未動，越界照舊是 `match.monitor_out_of_range` 並在 `hint`
裡列出本機全部螢幕；兩種新寫法寫在 `--help` 與上面的取值寫法一節裡。

`--screens` 屬於環境查詢那一家族：只接受 `--lang` / `-v` / `-q`，其餘（含 `--yes` 與 `--monitor`）都是
`cli.query_conflict` + 結束碼 1，也不參加「零條件就出說明」那一條。`--quiet` 只去掉 `notes`：`identity`、
`readability`、`authorization`、`caveats` 是判準而不是禮貌性提示。這份文件會列印裝置路徑，但不含檔案系統路徑與
使用者名（`privacy.includesDevicePaths: true`、`includesFileSystemPaths: false`）。

## 取圖方式

| 取值          | 通道                                     | 能截被遮擋視窗     | 硬體加速內容               | 滑鼠指標（`--cursor`）                                                         | API 歷史下限                                                                                            |
| ------------- | ---------------------------------------- | ------------------ | -------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------- |
| `wgc`         | Windows.Graphics.Capture                 | 能（DWM 快取）     | 正常                       | 有一條真能設進去、也讀得回來的開關（要 19041 起）                              | Win10 1903（18362）——是 `CreateForWindow` / `CreateForMonitor` 那條互通操作介面，不是 1803 那個命名空間 |
| `dwm`         | DwmRegisterThumbnail                     | 能                 | 多數正常，受保護視窗是黑的 | 來源像素裡沒有滑鼠指標                                                         | Win8.1（9600）——註冊縮圖更早，但讀回靠 `PrintWindow(PW_RENDERFULLCONTENT)`                              |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT       | 能（視窗自繪）     | 常常全黑                   | 來源像素裡沒有滑鼠指標                                                         | Win8.1（9600），指那個 flag                                                                             |
| `bitblt`      | BitBlt 螢幕 DC                           | 不能，只拷可見像素 | 部分黑                     | 來源像素裡沒有滑鼠指標                                                         | 本身沒有版本門檻                                                                                        |
| `duplication` | DXGI 桌面複製取整幅螢幕影格後按矩形裁切  | 不能，只拷可見像素 | 正常                       | 沒有開關：桌面影格裡可能已經把指標畫進去了，所以 include 與 exclude 都無法保證 | Win8（9200），遠端桌面/虛擬顯示卡常拿不到內容                                                           |
| `auto`        | 按 wgc → dwm → printwindow → bitblt 退回 | 盡量               | 盡量                       | `include` 會把鏈收窄成只剩 `wgc`                                               | 這條鏈減去本機版本擋掉的那幾條                                                                          |

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
    D3D11 裝置——這是 `DuplicateOutput` 的要求。由第二張 GPU 驅動的螢幕因此也截得到。
    這條路徑沒有 WARP 退路：軟體裝置不擁有任何實體輸出，那樣做只會交回一張內容為空的影格、
    尺寸卻報得對。
  - **一個目標只取一塊輸出。**跨了兩張螢幕（或懸出邊緣）的視窗，只在與它重疊最多的那塊輸出上、於重疊處截取；
    剩下的部分*不會*在圖裡。這會表現為 `capturedRect` != `requestedRect`、`clipped` 與 `note.capture_clipped`，
    而不會靜默地看起來像整個視窗。把一個視窗跨配接器拼成一張圖這件事沒有實作。
- 目標螢幕在確認之後離開了桌面或變了形狀，截圖以 `capture.monitor_changed`（退出碼 7）停止——工具絕不會拿
  另一張螢幕頂替，授權也始終綁在那個人親眼看過的那張上。

## 畫面裡的滑鼠指標（`--cursor`）

`--cursor default|include|exclude` 說的是畫面裡要不要滑鼠指標。預設值 `default` 的含義是本工具對這件事
**一個字都不改**：不碰任何通道的滑鼠指標設定，結果裡也不出現滑鼠指標那三個鍵 —— 輸出與這條選項存在之前
逐位元組相同。刻意寫 `--cursor default` 是另一件事：同樣不要求改動，但結果要報這條路徑實際交回的是什麼。

各條路徑能承諾到哪一層，判據是**它的像素從哪裡來**，不是通道名字。那份登記表在 `src/CursorControl.h`：
按 `images[].path` 那個內部路徑名一行一條，與授權那張登記表同一種形狀；沒登記的路徑按嚴格處理
（兩種要求都不敢聲稱）。

- `wgc` 與 `screen.wgc` 是僅有兩條真有可設進去、也能讀回來核實的開關的路徑 ——
  `IGraphicsCaptureSession2::IsCursorCaptureEnabled`，Windows 內部版本 19041 引入。這道門檻**比 `wgc`
  通道自己的 18362 還高**：1903 的機器能用 `wgc` 截到圖，卻仍然對滑鼠指標這件事說不出任何保證。
- `printwindow`（讓視窗自己畫進 DC）、`dwm.thumbnail`（DWM 重新導向點陣圖）、`dwm.screen` /
  `bitblt.screen` / `screen.bitblt`（螢幕 DC，系統指標畫在 DC 內容之外）這幾類的來源像素裡根本沒有滑鼠指標：
  所以對它們 `exclude` 是來源那一層的事實，而 `include` 就是做不到。
- `duplication.frame` / `screen.duplication`（桌面合成分）是**指標狀態無法核實**的那一類：桌面影格有可能已經把指標畫在裡面了，而這條路徑沒有可設的開關，既不能保證畫、也不能保證不畫。所以對 duplication，`include` 與 `exclude` 都兌現不了；`--cursor default` 交回的影格其指標狀態一律記 `unverified`（見下面的欄位表）。

由這張表推出兩條規矩，兩條都是為了不讓「我要求過」被讀成「已經辦到了」：

- **做不到的那條就拒絕，不偷偷改道。** `--cursor include` 配 `printwindow` / `dwm` / `bitblt`（來源裡
  根本沒有指標可加）在解析期就是 `capture.cursor_unsupported`（退出碼 `1`）—— 在彈框之前、在算輸出名之前、
  在讀任何一個像素之前。配 `duplication` 時 `include` 與 `exclude` **兩樣都**在解析期是 `capture.cursor_unsupported`（退出碼 `1`），各有各的措辭：`include` 是這條來源沒有指標開關，`exclude` 是桌面影格裡指標的狀態證明不了。換成會讀桌面像素的通道既加不回滑鼠指標，也保證不了去掉一個，交回的更是一份沒人批准過的畫面。`--capture auto` 時兌現不了的那幾條從鏈裡摘掉，各留一條
  `note.cursor_channel_skipped`；摘到一條不剩，或者本機版本低於 19041 而要求是明確寫出來的那一種，
  就是 `env.cursor_unsupported`（退出碼 `7`），一個像素都不取。`-v` 回顯的 `input.captureChain` 與真去
  截圖時用的是同一個函式，所以 `--cursor include` + `auto` 那裡看到的就只剩 `["wgc"]`。
- **不拿影像修補冒充能力。** 本工具不去取桌面複製那份指標形狀來畫，不會把滑鼠指標畫進影格裡，
  也不試圖把已經畫進去的滑鼠指標抹掉 —— 那些都是影像修補，也都沒有一樣能核實。`src/` 裡一旦出現這種呼叫，
  `tests\cursor.ps1` 就紅。

只要寫過 `--cursor`，每張交出的圖就帶這三個欄位（沒寫過時一個都不出現）：

| 欄位              | 取值                                                                                                                                                                                       | 說的是哪件事               |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------- |
| `cursorRequested` | `default` / `include` / `exclude`                                                                                                                                                          | 要求的是哪一種             |
| `cursorEffective` | `include` / `exclude` / `unverified`                                                                                                                                                       | 這條路徑實際交回的是哪一種 |
| `cursorBasis`     | `wgc_session_property_set` / `wgc_session_property_read` / `path_excludes_cursor` / `path_pointer_state_unverified` / `wgc_cursor_property_unavailable` / `path_capability_not_registered` | 這個結論憑什麼             |

`wgc_session_property_set` 是按這一次的請求設過、再把讀回來的值核對了；`wgc_session_property_read`
是沒設過（`--cursor default`）只讀目前值；`path_excludes_cursor` 是這條路徑的來源像素裡沒有滑鼠指標；
`path_pointer_state_unverified` 是桌面複製的那一影格、它的指標狀態證明不了（所以 `cursorEffective` 是 `unverified`）；
`wgc_cursor_property_unavailable` 是那一問沒答案；`path_capability_not_registered` 是這條路徑沒進登記表、
於是按嚴格處理而不是去猜。凡是無法核實的這一類，`cursorEffective` 都寫 `unverified`，不折成「畫」或「不畫」任何一種。

`cursorEffective` 就斷言到這一層為止。它說的是「這條工作階段被設成畫/不畫滑鼠指標」，或者「這塊來源裡沒有滑鼠指標」，
**不是**「這一個圖裡此刻看得見或看不見指標」。本 SDK 的工作階段介面沒有 `IsCursorVisible` 那個唯讀屬性，
所以像素級的事本工具一條都不聲稱，而 `--capabilities` 把這條界線寫成
`cursor_effective_is_a_setting_not_a_pixel_check` 那條 caveat。明確要求過（`include` / `exclude`）而在 `wgc`
那一次核實不了（介面取不到、設不下去、或讀回來是相反的那一件）時，程式碼是 `capture.cursor_unverifiable`
（退出碼 `7`），且發生在 `StartCapture` **之前** —— 與要求相反的那一個根本不會交出去；
`--cursor default` 遇到同一情況就報 `unverified`，而不是折成任何一個答案。

滑鼠指標這件事不改變授權。判據仍然是「這條路徑的像素從哪裡來」，所以 `--cursor exclude` 配 `bitblt` 的螢幕路徑或任何整張螢幕目標，以及 `--cursor default` 配 `duplication` 路徑，照樣一定彈框，`--yes` 照樣管不著；`wgc` 不帶 `--yes` 照樣要問。（而 `--cursor exclude` 配 `duplication` 在解析期就被拒絕，根本走不到彈框那一步。）那幾種彈框現場在 `tests\cursor.ps1` 裡用自建視窗判。這三個欄位與 `path` / `scope` / `rect` 以及裁剪那幾項一樣是定位判據，
`--quiet` 不許抑制。

`--capabilities` 不截任何一個像素就能回答上面這些，那一段叫 `cursor`：預設值、三種取值、那唯一一條開關的
`compiled` / `status` / `minBuild` / `verifiedOnThisMachine`，每條已登記路徑一行
（`capability` / `reason` / `include` / `exclude`，後兩者各是 `yes` / `no` / `unknown`），
以及 `pointerShapeCompositing: "never"` 與 `pixelRetouching: "never"`。

## HDR 色彩處理（`--hdr`）

螢幕處於 HDR 模式時，採集回來的影格可能帶著超出 SDR 的亮度範圍與另一種傳遞函數。把那種影格硬按 8 位元 BGRA 解釋，得到的是一張發白、去飽和、亮部一团糊的圖，而它「看著像一張正常圖」—— 本工具不把這種結果默認為正確。`--hdr auto|tonemap|refuse` 就是讓你對這件事作出明確決定。有兩種在**取影**時行為相同、卻在**上報**時必須分開的狀態：

- **完全沒寫 `--hdr`** —— 本工具對色彩一個字都不改：不探測顯示狀態、不改採集格式、不做對映，**而且結果裡不出現色彩那組鍵** —— 輸出與這條選項存在之前逐位元組相同。
- **明確寫了 `--hdr auto`** —— 取影行為一樣（不探測、不改格式、不對映），**但色彩那組鍵照樣出現**，只如實報這一個影格被動帶回的樣子。在 `wgc` 路徑上一張 8 位元交付影格報 `hdrEffective: unverified` / `hdrBasis: bgra8_source_unverified`，因為 `auto` 從沒問過顯示器是否處於 HDR 模式，而一張 8 位元 surface 並不能證明來源就是 SDR（見下面的欄位表）。

`auto` 也是省略這條選項時所用的預設*取值*——但省略與明確寫出在結果裡可以用 `input.hdrGiven`（`-v`）分開，而這正是本工具拒絕合併的那一對「沒寫」與「寫了」。

哪條路線帶得回廣色域影格、哪條路線真的兌現得了你寫下的策略，是兩個不同的問題，判據與別處一樣：**它的影像素從哪來**，不是通道名字。那份登記表在 `src/HdrColor.h`，按 `images[].path` 一行一條，`--capabilities` 把它印成 `color.paths`，每行帶 `capability` 與 `honorsExplicitPolicy`：

- `wgc` / `screen.wgc` —— `wide_gamut_capable`、`honorsExplicitPolicy: true`。來源跟隨顯示模式，而這條路會讀回它實際拿到的格式（FP16 scRGB 線性，或 8 位元 BGRA），所以說得出是哪一種、也對映得了。本建置裡兌現得了 `tonemap` / `refuse` 的就是這兩條。
- `duplication.frame` / `screen.duplication` —— `wide_gamut_unverified`、`honorsExplicitPolicy: false`。那塊桌面影格確實可能以 FP16 scRGB 或 10 位元 ST.2084 (PQ) / HLG BT.2020 回來，但本建置在 `DuplicateOutput` 之前從不問顯示器的色彩空間，所以既證明不了拿到的是什麼、也就無法承諾任何策略。在這裡要求 `tonemap` / `refuse` 於是遭到拒絕，而不是被含糊地當成已回答。
- `printwindow`（視窗自繪進 8 位元 DC）、`dwm.thumbnail` / `dwm.screen`、`bitblt.screen` / `screen.bitblt` —— `sdr_source_only`、`honorsExplicitPolicy: false`。結構上只帶得回 8 位元 SDR，所以對它們而言 HDR 處理沒有對象，是恆等而不是「做不到就換一條」。

三種取值：

- `tonemap` —— 要求把 HDR 影格對映成 SDR 交付。本工具在編碼之前建一份**逐像素的浮點中間量**（不分配整幅浮點影格，免得把 1 GiB 的整影格預算乘四撐爆），按固定曲線處理：解傳遞函數（scRGB 線性 / PQ→絕對亮度→相對線性 / HLG 反 OETF）→ BT.2020 到 BT.709 的原色矩陣 → 按亮度做**擴展 Reinhard** tone mapping（確定、單調，`white=1` 時退化為恆等）→ sRGB 編碼 → 不透明 alpha 直通。來源本就是 SDR 時是恆等透傳。
- `refuse` —— 一旦核實來源確是 HDR 影格就報錯、一個像素都不落地，絕不交一張被硬壓成 BGRA8 的發白圖。
- `auto`（預設取值）—— 不啟用上面那條鏈路：不探測顯示、不改格式、不做對映。明確寫出 `--hdr auto` 時照樣發出色彩那組鍵，只被動如實上報這一個影格帶回的樣子（一張 8 位元的 `wgc` 影格是 `unverified`，不是「確認了 SDR」）；整條選項被省略時那組鍵根本不出現（見本節開頭）。

由這張表推出兩條規矩，與游標那一條同源：

- **做不到的那條就拒絕，不偷偷改道。** `--hdr tonemap` / `refuse` 指名 `printwindow` / `dwm` / `bitblt` / `duplication` 時，在解析期就是 `capture.hdr_unsupported`（退出碼 `1`），而且**不換後端**（換成會讀桌面像素的那條既沒有更多 HDR 可對映，交回的也是一份沒人批准過的畫面）。`--capture auto` 不在解析期判（落到哪條通道要到執行期才知道），而是用同一道 `FilterChainForHdr` 判準把回退鏈收窄——它與 `-v` 回顯的 `input.captureChain` 算的是同一件事，所以 `auto` 配 `tonemap` 剩下的只有 `wgc`：被摘掉的每條通道各留一條 `note.hdr_channel_skipped`，一條都不剩時整趟以 `env.hdr_unsupported`（退出碼 `7`）停下，而不是交出一張沒對映過的影格。取影格回報 `capture.hdr_refused` 時整條鏈當場停下——絕不再換一個後端重試，因為另一個後端只會生出另一張沒人批准過的畫面。
- **認不出就是認不出。** 帶回一個本建置認不出的廣色域像素格式時是 `capture.hdr_unverifiable`（退出碼 `7`），既不硬按 BGRA8 解釋，也不「猜一個對映」；`--hdr refuse` 且核實來源是 HDR 時是 `capture.hdr_refused`（退出碼 `7`）。三條都在編碼之前給出，都不落地。

只要寫過 `--hdr`，每張交出的圖就帶這組欄位（沒寫過時一個都不出現，與這條選項存在之前逐位元組相同）：

| 欄位               | 取值                                                                                                                                                                                                                                  | 說的是哪件事             |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------ |
| `hdrRequested`     | `auto` / `tonemap` / `refuse`                                                                                                                                                                                                         | 要求的是哪一種           |
| `hdrEffective`     | `sdr_passthrough` / `tone_mapped` / `unverified`                                                                                                                                                                                      | 這一個影格實際經歷的處理 |
| `hdrBasis`         | `delivered_bgra8_sdr` / `scrgb_float_tone_mapped` / `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `path_sdr_source` / `format_unrecognized` / `transfer_function_unknown` / `bgra8_source_unverified` / `tone_map_not_applied` | 這個結論憑什麼           |
| `sourceColorSpace` | `srgb_bgra8` / `scrgb_float` / `pq_bt2020` / `hlg_bt2020` / `rgb10a2_unverified` / `unknown`                                                                                                                                          | 編碼之前那份來源         |
| `sourceBitDepth`   | `8` / `10` / `16`（認不出時整個欄位不出現）                                                                                                                                                                                           | 來源每通道位元數         |

本建置裡真機截圖只走得到的對映路徑是 `scrgb_float_tone_mapped`（FP16 的 `wgc` 影格池），因為一張 10 位元打包影格會被判為 `rgb10a2_unverified`，而不是當成 PQ 或 HLG——所以 `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` 與 `transfer_function_unknown` 是離線走過的列舉取值，並不是「本機真有一塊 PQ/HLG 面板被對映過」的承諾。一個帶廣色域標記卻沒有對映記錄的來源是 `unverified` / `tone_map_not_applied`，絕不當成「已經對映」。

明確要過處理（`tonemap` / `refuse`）而這一個影格以 8 位元 BGRA 交付時，圖照常交付（對映對 8 位元來源是恆等的），但留哪一條提示取決於取影之前到底問過沒有：如果那次唯讀地問過顯示器狀態且答的是 SDR，提示是 `note.hdr_source_sdr`（「確認是 8 位元 SDR」）；如果那一問根本沒問、或沒給出答案，提示是 `note.hdr_source_unverified`（「按 8 位元交付，但來源沒有被確認是 SDR」）——一張未核實的 8 位元影格絕不會被寫成一張已確認 SDR 的影格。兩種情況都把「我要過 HDR 處理」與「這一個影格沒有已確認的 HDR」分開放在你眼前，而不是拿一次靜默的通過冒充「HDR 已經被正確對映」。`--hdr auto` 這兩條提示都不發（它只透過上面那組機器欄位被動上報）。

HDR 色彩這件事不改變授權：整個處理排在取影之後、編碼之前，判據仍然是「這條路徑的像素從哪來」。會讀到桌面像素的那幾條照樣一定彈框、`--yes` 照樣管不著，也不引入任何「對映過就算免確認」的旁路。

**這台開發機的螢幕不支援開啟 HDR**，所以「真帶回一幅 HDR 影格並對映」的端對端現場在本機造不出來：tone mapping 的數學由離線判據用已知色塊與亮度梯度逐點判（`tests\hdr_state.cpp`），`--capabilities` 裡 `color.verifiedOnThisMachine` 因此恆記 `no`（`hdr_tone_mapping_not_verified_on_hdr_display` 那條 caveat），`tests\hdr.ps1` 那幾條要 HDR 裝置的判據一律記未驗證。在 HDR 螢幕上覆核之前，本工具不宣稱色彩驗收通過。

## 截圖授權與 --yes

凡是真要取影格的截圖，都先彈出一個模態確認框，**可靠的視窗通道也一樣**。不彈框也不截的只有這些：沒給任何條件
（文字說明 + `2`）、`--help`、`--version`、`--dry-run`、無匹配（`4`）、匹配多個（`5`）、解析期參數錯（`1`）、
以及輸出名規劃失敗（例如 `io.output_collision` + `8`）——整批名字在任何一次發問之前就算完了。

`--yes`（`-y`，正向布林開關：裸寫或 `=true/1/yes/y/on` 是開，`=false/0/no/n/off` 是關，重複給出時最後一個生效，
`-v` 的 `input.yes` 回顯最終結果）只免掉**一層**確認：影格綁在所選視窗本身、絕不從桌面取樣的那些路徑。
它別的一概不保證：不保證圖是有效的、不忽略權限、不管受保護內容、不管錯誤、也不管覆蓋保護。
決定屬於哪一層的是實際走的那條路徑，不是通道名：

| 路徑（`images[].path`）                             | 像素從哪裡來               | 不給 `--yes` | 給了 `--yes` |
| --------------------------------------------------- | -------------------------- | ------------ | ------------ |
| `wgc`、`printwindow`、`dwm.thumbnail`               | 只有所選視窗自己           | 問一次       | 不問         |
| `dwm.screen`、`bitblt.screen`、`duplication.frame`  | 那個視窗所在的那塊螢幕區域 | 要問         | **照樣要問** |
| `screen.wgc`、`screen.bitblt`、`screen.duplication` | 整張螢幕                   | 要問         | **照樣要問** |

判不出來或沒登記的路徑一律按桌面路徑處理，所以新增通道忘了登記只會更嚴不會更鬆。`--monitor` 與視窗條件同時
給出時篩的是**視窗**，出的仍是視窗圖，所以按上面視窗那兩行走。

- **桌面路徑沒有任何旁路**：`--yes`、`--quiet`、環境變數、stdin、呼叫者是誰，都跳不過它——分層就是為了這件事。
- 一次確認可以覆蓋本次請求裡明確列出的那一批目標，所以多條後端、多個視窗不會各問一遍。但那張許可是一份**快照**，
  不是一筆長期授權：它綁的是框上列出的那些目標與當時顯示的區域、整批解析完的絕對輸出路徑，以及螢幕拓撲的指紋；
  而它要複核兩次——簽發之前一次（框開著的時候拓撲變了就重新問一次，不是把舊答案拿來用），以及每一步真要取樣像素
  之前一次（那一塊必須還在人批准過的範圍裡）。授權絕不跨請求快取，也不會擴大到框上沒列出的目標，「同意截視窗畫面」
  更不等於「同意截桌面」：`--capture auto` 配 `--yes` 可以不打擾人地走完視窗那幾條，可一旦進入桌面路徑就必須再問一次。
- 這是一個只有「是 / 否」兩個按鈕、預設焦點落在「否」上的 `MB_YESNO` 框：只有明確點「是」才算同意，其餘一切結果都算拒絕。要拒絕就點「否」。別指望 `X` 或 `Esc`——`MB_YESNO` 下標題列的 `X` 雖然顯示但被停用、也沒有可讓 `Esc` 觸發的 Cancel 按鈕，所以兩者都不是可靠的「否」操作，這個框終究得有人來答；而工具本來就把任何非「是」的結果一律當拒絕處理。除此之外，`--consent-timeout-ms` 之內沒人回答，本身就是一次獨立的拒絕（`capture.consent_timeout`）。有人答「否」、超時沒人回答、或根本沒有可互動的桌面——這幾種結局都會停掉本次請求剩下的部分：`auto` 回退鏈上剩下的每一條通道一起停，不只是剩下的 `--all` 目標；不換後端、不重試、不再問第二遍，已經完成的圖全部留在 `images` 裡。人的回答是一道邊界，不是「再換一條路試到誰點頭」的提示。
- 目標區域挪動過、或者螢幕拓撲變了，覆蓋它的授權當場作廢並重新問一次；已經綁在舊區域上的那一影格給
  `capture.consent_stale`（退出碼 `7`，重新選目標再截）。
- 確認框的預設焦點在「否」上，內容列出目標及其區域、將要走的那條路徑、每張圖展開後的絕對路徑（或「標準輸出」）、
  以及畫面裡會不會混進別的視窗；這條路徑會拍到桌面時，框上還明寫著「你給的 `--yes` 對它不生效」。框在取影格之前
  就已經關閉，所以不會出現在圖中；點「是」之後工具仍要等約 1 秒，因為關閉動畫還留在 DWM 的畫面上。
- 沒有可互動桌面時（服務工作階段、排程工作、鎖屏），帶 `--yes` 的視窗內容截圖照舊正常完成，而桌面路徑只能被拒絕——
  絕不會因為「彈不出框」就放行。答「否」給 `capture.access_denied` + `6`，彈不出框給新的穩定碼
  `capture.consent_unavailable` + `6`；兩者都帶 `stage=consent`、`target`、`backend`，`value` 寫的是那條路徑。
- 被人拒絕就是被人拒絕：`capture.access_denied`（`stage=consent`）+ 退出碼 `6`，與有沒有給輸出路徑無關。
  省略 `--out` 就等於 `--out -`，授權結果不因它而變，見《不給輸出路徑（相容性說明）》。
- 多張螢幕 + 寫 stdout（`--monitor all --out -`）在任何彈框之前就被拒：一次確認換不來「每張螢幕一張圖擠進同一條串流」。
  只有一張螢幕時 `--monitor all` 是一個目標，那條路仍然按單張走 stdout。
- 說清楚邊界：這就是一個 `MessageBox`。它是給合作式自動化（人或 AI）準備的誤點防護，既證明不了按下按鈕的是人，
  也擋不住同一個權限等級裡存心要繞過的處理程序。它能保證的是：照這套規矩跑的呼叫端，一定會被問上這一次。

`--monitor`（省略取值）與 `--monitor primary` 是主螢幕，`--monitor 2` 是第 2 張螢幕，`--monitor all` 每張螢幕一張。
編號是**本次執行 `EnumDisplayMonitors` 列舉中的位置**，從 1 起——它不是「顯示設定」裡 Windows 寫的那個識別號，
而且拔掉一張螢幕或改一次解析度都可能把它重新排過，所以不要把編號存下來當作跨執行的螢幕識別。要再次認出同一張
螢幕時，跑一次 `--screens`，把它交回的識別原樣寫回來：`--monitor device:DISPLAY1`（本次桌面連線的
裝置名）或 `--monitor "id:\\?\DISPLAY#…"`（螢幕的 devnode 裝置介面路徑，跨工作階段還成立的那一條）。
識別在本機不存在報 `match.monitor_unknown_id`（結束碼 4），命中多張報 `match.monitor_ambiguous_id`（5，
候選全部列出，工具不替你挑一張），那一問沒答案報 `match.monitor_id_unverifiable`（7）——三條都不會悄悄
退化成「那就用主螢幕」。編號越界照舊報 `match.monitor_out_of_range`（退出碼 1），
`hint` 裡列出本機全部螢幕。螢幕目標在截圖之前，工具會按**身分**重新核對那張螢幕（選定當時問得到跨工作階段
識別就按它核，問不到才照舊按名稱核）：它如果已經離開桌面，或那個裝置名已經發給了另一張面板，截圖以
`capture.monitor_changed` 停止（複核本身問不出答案時，報 `capture.monitor_unverifiable` 而不是照名稱續截）；它的矩形或位置如果變了，要人批准的是那個新矩形——一份舊確認絕不會被拿去用在
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

| 碼                            | 退出碼 | 含義                                                     | 下一步                                                         |
| ----------------------------- | ------ | -------------------------------------------------------- | -------------------------------------------------------------- |
| `capture.target_gone`         | 7      | 句柄在本次請求中被銷毀                                   | 重新列舉視窗再來一次                                           |
| `capture.target_changed`      | 7      | 這個句柄值現在屬於另一個物件（或不再滿足當初的條件）     | 重新選定目標；**你核准的許可不會轉給新物件**                   |
| `capture.target_unverifiable` | 7      | 有一道判據問不出來（讀不到處理程序資訊、條件求值沒跑完） | 查執行環境（權限、策略、防毒軟體），或加大 `--timeout-ms` 再來 |

身份變了**不會**去放寬條件另找一扇看起來一樣的視窗 —— 這與 `--yes` 那條規矩同源：許可綁的是列給人看的那個物件。

**保證的邊界**：這條覆核把競態視窗縮小了，不聲稱把它消除。判據與取影格之間不是原子操作，而 HWND 既不是可等待物件，也沒有「鎖住一個視窗不讓它消失」的公開 API。判據與取影格之間那一瞬仍可能變化 —— 只是不再有「整整一段規劃加人工確認」那樣長的時間可以拿來變。

## 執行期限與會阻塞的呼叫（`--timeout-ms` / `--consent-timeout-ms`）

`--timeout-ms <ms>` 是本次執行自動處理那一段的**總預算**，從開始選目標那一刻起依單調時鐘計時。視窗/螢幕匹配
（含 `--title-regex`）、`auto` 退回鏈、等影格、編碼、最後那次提交，花的都是**同一份**預算：沒有任何一步、也沒有
批次裡任何一個後續目標能重新領一份完整預算，所以四條後端不可能各等 2 秒、兩個目標也不可能各再等一遍。不給或寫
`0` 就是不設總預算；即便如此每次隔離呼叫仍受一個內建的 5000 ms 上限約束。
預算用盡時，**尚未開始的那一步會被拒絕，它那一張不落地**——條件求值階段把預算耗盡給 `match.timeout`
（`stage=match`）；影格遲遲不來給 `capture.timeout`（`stage=capture`），而預算死在編碼器裡時同是一條
`capture.timeout`、只是 `stage=encode`（並沒有一條 `encode.timeout` 碼，把兩者分開的就是 `stage`）；寫檔案從頭到尾
沒能開始時給 `io.timeout`（`stage=write` / `stdout`，退出碼 `8`）。批次裡剩下的目標不再開始，已經寫完的圖仍留在
`images` 裡。所以部分完成的一批和局部截圖失敗表現完全一致：退出碼非 0，凡是已經落地的都照樣交付。

這道閘刻意做成「每步開始之前」的閘，而它的誠實邊界就是同一件事的背面：原子寫檔案沒有取消點，所以預算**在提交的過程中**到期的那一張照舊會落地，想在呼叫中途搶停它做不到。寫完返回之後會再複核一次期限：若這時預算已經越線，就在交付記錄之外**額外**記一條 `io.timeout`（訊息寫明這張圖是完整交付的、只是寫完時預算已耗盡）——但已經提交的那張圖仍留在 `images` 裡、`captured` 照樣把它計入，本次按部分成功報告、退出碼 `7`（已交付＋出錯），而不是那種寫都沒落地的 `8`。已經交付這件事實與期限是否達標，是刻意分開的兩件事。逾時不回滾、也不刪除——磁碟上那份檔案是呼叫端自己要的，悄悄把它移除等於又做一次沒人要求過的寫入。

等一個人是**另一條**時鐘：`--consent-timeout-ms <ms>` 只為確認框設上限，絕不動自動處理那份預算（人走開了不等於
「機器慢」）；這段等待在進行之時被暫停出自動預算，所以框開得再久，也不會把那張圖自己的期限花光。
到了時間沒人回答，本次請求就按**拒絕**處理——`capture.consent_timeout`、退出碼 `6`、`stage=consent`——絕不當成同意，
批次剩下的部分也就會像有人明確答「否」之後那樣停下。不給或寫 `0` 還是一直等，和從前一樣。這條上限是靠**輪詢**
判定的，不是搶佔那個對話框：到期是在下一個等待切片上被發現，框還另有約 3 秒的關閉寬限，所以它可能在螢幕上比
你要的那個數字多留一會兒。點「是」之後那約 1 秒的緩衝是為了把對話框的關閉動畫擋在畫面之外，它屬於人工那一階段，
既不計進自動預算，也絕不會為了趕期限而被省掉：被限制的是「等一個回答」，不是「回答之後等畫面安定」。

**阻塞到底堵在哪。** `PrintWindow` 是給目標視窗發一個繪製請求、再等它自己的執行緒；`--capture printwindow` 與
`dwm` 的回讀乾的就是這件事，而這個呼叫內部沒有能拿來核對期限的打斷點。`std::regex` 也一樣：像 `(a+)+$` 這種
模式去匹配一個長標題可能回溯好幾分鐘，而對模式限長度並不是執行期限。這些呼叫現在都跑在同一個 `ECAPTURE.EXE`
拉起的輔助處理程序裡，父處理程序透過一條私有管線把已經解析好的一個任務遞給它；期限一到，父處理程序就停掉
**它自己的**那個輔助處理程序並報出逾時。目標應用的視窗絕不會被殺掉，也沒有哪個輔助處理程序能活得比父處理程序久
（一個「關閉即結束」的工作物件，加一次斷管檢查，再加一個按預算走的看門狗：它只給 await 握手那一段設一個上限，之後就盯著父處理程序剩下的預算再加一點交付寬限，絕不會把一次明確接受的長預算提前截短）。

收尾的那道邊界和逾時本身一樣重要，因為那條管線可能在最壞的瞬間回話。連線這一步分三種情形判開：已經連上的輔助程序
（沒有還在等待的 I/O 要取消）、同步就完成的寫入，以及回了 `ERROR_IO_PENDING` 的寫入。`CancelIoEx` 只是**請求**
取消，所以父程序要等到完成被回報出來，並讓每一個 `OVERLAPPED` 與它的事件一路活到那時候；完成遲遲不來時，給那個
輔助程序約 2 秒自行離開，之後才 `TerminateProcess`，而這段寬限跑在請求預算**之外**。判完之後才到的回包一律不讀：
一張來得太晚的圖既不能把已經回報的逾時改寫成成功，也不會被當成圖片交付。而這**不**改變的事：輔助處理程序只會讀
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

| 佔位符 | 含義                                                                                                                                                                                                                                                  |
| ------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `%i`   | 序號，從 1 起（`--all` 多視窗、`--monitor all` 多螢幕）                                                                                                                                                                                               |
| `%h`   | 視窗句柄，形如 `0x001B0C48`；螢幕目標給 0                                                                                                                                                                                                             |
| `%p`   | 處理程序 ID；螢幕目標給 0                                                                                                                                                                                                                             |
| `%n`   | 視窗標題；螢幕目標給去掉 `\\.\` 前綴的裝置名（如 `DISPLAY1`）。標題會被清洗成能用的檔案名片段：非法字元換成 `_`、去掉尾端的點與空格、整段剛好是保留裝置名稱（`CON` / `NUL` / `COM1` / `LPT1` …）時加 `_` 前綴、依 80 個 UTF-16 碼元截斷且不劈開代理對 |
| `%d`   | 本地日期 `YYYYMMDD`                                                                                                                                                                                                                                   |
| `%t`   | 本地時間 `HHMMSS`                                                                                                                                                                                                                                     |
| `%%`   | 一個字面上的 `%`；其餘 `%x` 原樣保留兩個字元                                                                                                                                                                                                          |

`--all` 的輸出名裡沒有佔位符時會自動追加 `_序號`，並發出 `note.all_without_placeholder`。
佔位符分不開目標時（只寫 `%d`，或同一處理程序的兩個視窗寫 `%p`）不會被悄悄改名：整批名字事先算好，撞名就報
`io.output_collision`。`%d` / `%t` 用的是本批次那一次時鐘，所以跨午夜的一批也全用同一個日期與時間。規劃出的名字
依絕對路徑、不區分大小寫、逐碼元比較（NTFS 就是這樣看名字的）；字串比較看不見的別名（8.3 短名、硬連結、目錄
junction 與符號連結、UNC 與磁碟代號兩種寫法）交給提交那一次原子操作判定，所以預檢認不出的佔用同樣不會被靜默替換。
`--out -` 不是路徑：不展開、不補副檔名、不查碰撞；而標準輸出一次只交付一張影格，所以那條串流上永遠不會有整批圖
（見前面「輸出形式」的規則）。

## 截圖歷史歸檔（預設開啟）

每張**完成主交付**的圖，除了使用者寫的那條輸出之外，還會在**實際執行的那一個 `ECAPTURE.EXE` 所在目錄**下的
`history\YYYY-MM-DD\`（依本地日期分目錄）裡另存一份獨立副本。開發版就躺在 `build\` 裡，所以開發時截的圖
歷史在 `build\history\`；裝在自訂目錄時，歷史就在那個目錄裡——它跟著程式自己，不跟著工作目錄、
不跟著 `--out` 的目錄，也不跟著某個固定的預設安裝位置。

| 項目 | 行為 |
| ---- | ---- |
| 來源 | 主交付那一次**已經編好的位元組**：不重拍、不重新編碼、不去讀主輸出檔案（它可能正被別的程式改），也不建硬連結。之後刪掉或覆蓋主輸出都不影響歷史裡那一張。 |
| 入口 | 視窗、整張螢幕、`--all` / `--monitor all` 批次、`--roi` / `--client-area`、`--scale`、五種編碼格式與 `--out -` 都經過同一個歸檔入口。 |
| 時機 | 只在真實截圖主交付成功之後按需建立目錄與寫檔案。`--capabilities` / `--diagnostics` / `--screens` / `--list` / `--inspect` / `--help` / `--version`、參數錯、無匹配、`--dry-run`、人答「否」、以及一張有效影像都沒有的那些次，一個目錄都不建，也不探測那裡能不能寫。 |
| 名字 | 一次歸檔決策只取一次本地時間，目錄名與檔案名同源自它（跨午夜不會出現「目錄寫今天、檔案名寫明天」）：`YYYYMMDD-HHMMSS-<PID>-<識別>-<序號>.<副檔名>`。副檔名跟著這張圖實際被編成的容器。檔案名裡沒有視窗標題、裝置名、使用者名，也沒有任何完整路徑。 |
| 提交 | 複用生產那一次「不許替換」的原子改名：獨佔建立，撞上既有名字就換下一個序號再試，換不出來照實報 `history.file_exists`。**從不覆蓋既有歷史檔案**，多個處理程序同時截圖與系統時鐘回撥都蓋不掉已經存下的那一張。 |
| 與主交付的關係 | 兩次交付、兩個位置（可能落在不同的卷上），各有各的結論，不承諾跨這兩個地方的全有或全無交易。主輸出沒完成時不發布副本（半段標準輸出也不算完成）；主圖已落地而副本失敗時，主圖不刪、已發出的標準輸出不回滾、也不重拍，`images` 與 `captured` 照舊，而退出碼是按部分成功給的 `7`。 |
| 保留 | 不自動輪轉、不按天數或容量刪除、不背景掃描、不上傳。歷史是持續保留的截圖資料，不是可以隨手重建的快取：`.\clean.ps1` 與 `.\build.ps1 -Clean` 清掉產物但**保留 `build\history`**（不能安全保留時明確拒絕並說明為什麼，既不靜默刪除也不靜默搬移），安裝封裝不含它也不把它寫進發行清單，升級與卸載都不碰它。清理由使用者自己顯式做。 |
| 換安裝目錄 | 歸檔跟著當時那一個 exe 走，換目錄**不會**把舊歷史遷移過去；舊的那一棵仍留在舊目錄裡，要找回它就照 `--capabilities` 的 `history.location` 對著舊位置看。 |

結果裡那一段是 `images[].history`（屬於交付事實，`--quiet` 藏不掉）：

| 鍵 | 含義 |
| ---- | ---- |
| `status` | `saved` = 副本已獨佔提交；`failed` = 開始過而沒提交；`skipped` = 這一次根本沒開始寫副本 |
| `file` | 只在 `saved` 時出現，而且此時磁碟上確實有這一份；失敗時不給一個「本來要用的名字」冒充既有副本 |
| `code` | `failed` / `skipped` 時的穩定碼：`history.unavailable` / `history.write_failed` / `history.file_exists` / `history.budget_spent` / `history.same_file` |

失敗細節（含 Win32 原值、是哪一路走不通）在 `errors` 裡同碼的那一條，`stage` 寫作 `history`；`skipped`
那兩種沒有「哪一步失敗」可報（那一次根本沒開始），所以只寫在這一格里。

- `history.budget_spent`：`--timeout-ms` 那份自動預算是整批共用的一份，歸檔不另領一份，也不留一個誰也
  等不起的背景寫入。主圖剛落盤而期限才跨的那些次，副本記這一條，而**主圖與它的交付事實原樣保留**。
- `history.same_file`：`--out` 直接寫進 `history\` 那棵樹、而算出來的名字正好是這一次要用的歸檔名時不寫
  副本——寫下去是把剛交出去的那一張自己蓋掉。兩個都想要就把主輸出寫在別處。
- 主輸出落在 `history\` 樹下但名字不同時，副本照舊另存成另一個名字，既不自覆蓋也不會迴圈複製：副本的
  位元組來自記憶體裡那一份編碼緩衝，從不去讀主輸出檔案。

確認框在人點頭之前就把「程式自己那個目錄裡還會另存一份持久副本」寫在框上（那一路歸檔本身說不通的那些次就不
寫這句，不承諾一份根本不會存在的副本）；`--capabilities` 的 `history` 段說的是這套規則本身，不是某一次的
下場——它不建立目錄、不試寫，`caveats` 裡因此恆帶 `history_root_writability_not_probed`。歸檔失敗絕不改變
授權分級，也絕不為了「讓歸檔成功」而提權、改 ACL 或改目錄標籤。

## 文案語言

`--lang`（`-l`）選 `zh-CN` / `zh-TW` / `en` / `ja`，不給或給 `auto` 時用 Windows 顯示語言，判出來的結果不在這四種裡時用
`en`。取值寫得寬容：忽略大小寫、`_` 與 `-` 等價，`zh_TW` / `zh-Hant` / `cht` / `tw` 走繁體，`chs` / `cn` /
`zh-Hans` 走簡體，`jp` 走日語；寫錯在解析期報 `cli.unknown_language`（退出碼 1），不會退回成預設語言。

**只有給人看的文字會隨語言變**：診斷項的 `message` / `hint`、`--help` 全文。`code`、JSON 鍵名、取值列舉、
`0x…` 句柄、`HRESULT` 數值一律不變，呼叫端依 `code` 分支即可。文案是 exe 自帶的嵌入資源
（`resources/strings-<語言>.txt` 依語言編成四份 `RCDATA`），所以離線也能切換語言。

## 邊界與未驗證項

這一節專門回答「這個工具**不**承諾什麼」。各功能那一節解釋每一條為什麼存在；這份清單則是自動化呼叫端應該當成
**殘餘風險**、而不是當成 bug 來讀的東西。

**設計邊界，不是缺陷：**

- 那個確認框就是一個 `MessageBox`。它是一份為合作式自動化（人或 AI）準備的協作式防誤點設計：既證明不了按下按鈕的
  是人，也不是一道作業系統的安全邊界。它能保證的是：照這套規矩跑的呼叫端，每一條桌面路徑至少會被問上一次。
- 複核與取圖從來不是同一個原子操作。身份與拓撲的複核把句柄被換掉的那段競態視窗縮小了，並沒有把它關掉——
  Windows 沒有一個可等待的句柄、也沒有「讓一扇視窗不消失」的公開 API 能拿來做這件事。
- 期限預算只在能打斷的地方、以及靠停掉輔助程序起作用。預算死在原子提交的過程中時那一張照舊落地——
  **逾時不回滾、也不刪除**。確認期限是輪詢出來的（另有約 3 秒關閉寬限），不是搶佔出來的。
- `cursorEffective` 只說到「這條工作階段被設成畫指標」或「這塊來源裡沒有指標」這一層。SDK 的工作階段介面沒有
  唯讀的 `IsCursorVisible`，所以本工具從不作「指標在這張圖裡看不看得見」那種像素級的斷言，也從不把指標畫進影格、
  或把已經在裡面的指標抹掉。
- `--hdr tonemap` 交出的永遠是 8 位元 SDR（`encoderOutput: "sdr_bgra8"`）：本建置沒有原生 HDR 或 10 位元的輸出路線；
  而明確寫出的 `tonemap` / `refuse` 要求，本建置只有那兩條 `wgc` 路徑兌現得了——`duplication` 那面是
  `wide_gamut_unverified`，所以對它的要求是被拒絕，不是被猜著回答。
- `duplication` 一次只取一塊輸出：跨了兩張螢幕的視窗，只在與它重疊最多的那塊上截取，剩下的部分不在圖裡，
  表現為 `clipped` + `note.capture_clipped`。把一扇視窗跨配接器拼成一張圖這件事沒有實作；RDP 與虛擬顯示卡則常常
  根本拿不到複製影格。
- DRM 與受保護內容一律是黑畫面，某些播放器那種驅動級黑框也還在某些通道上攔不掉。整幅單色的結果是被**回報**
  （`note.frame_uniform`）並照常交付，從不被判定成失敗而拒絕。
- 聲明的支援下限（內部版本 18362）與實測過的環境（內部版本 19045、x64）是兩個不同的數，而
  `verifiedOnThisMachine` 拿本機去比的是後面那一個——那是一份環境相符的判定，不是逐台裝置的實測記錄。這個二進位檔
  在 Windows 7 上連載入都做不到，在 Windows 8.1 上載得了、卻沒有編碼器可用。
- 主輸出與歷史副本**不是一次跨卷交易**：那是兩次獨立的交付，可能落在不同的卷上，所以兩邊的結果各說各的。
  副本失敗絕不刪主圖、絕不回滾已經發出的標準輸出、也絕不為「補上副本」而重拍一次；反過來主圖沒成交時不發布
  副本。歸檔不另領一份預算，也不做無期限的背景寫入。歷史不自動輪轉、不自動刪除，也沒有開關去關掉它——這一版
  給的只有「預設另存一份」與可靠的失敗語義。

**照實記為未驗證、而不是推導出來**（每一行都是對應的測試在這台開發機上回報 SKIP／「未驗證」的東西，沒有一行被宣稱通過）：

| 本機判不了的                                                                               | 為什麼，以及這個缺口記在哪裡                                                                                                      |
| ------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------- |
| 真 HDR 影格的實拍、真 HDR 上 `refuse` 的下場、FP16 影格池、HLG 端對端                      | 沒有接 HDR 螢幕；對映數學由離線判準判（`tests\hdr.ps1`、`build\ecapture-hdr-tests.exe`），`color.verifiedOnThisMachine` 恆為 `no` |
| HDR 與 `--scale` 同時生效，以及 `--roi` / `--scale` 的跨螢幕混合 DPI                       | 只接了一塊螢幕，也沒有 HDR 模式可開（`tests\scale.ps1`、`tests\crop.ps1`）                                                        |
| 交付影格單邊超過 16384 的真機現場；旋轉的面板；把螢幕熱拔除                                | 造不出一扇那麼大的視窗，而測試一律不重新排列、不重新旋轉顯示器（`tests\image.ps1`、`tests\dup.ps1`）                              |
| 刻意的句柄／PID 重用，以及跨過真確認框的那段身份時間                                       | 那要結束別人的處理程序，而框上的答案是人給的（`tests\identity.ps1` 需要 `-SimulateConsent`）                                      |
| 像素級的「這張圖裡看不看得見指標」；內部版本低於 19041 的機器                              | SDK 給不出像素級的答案，而本機比 19041 新（`tests\cursor.ps1`）                                                                   |
| 19045 以外的 Windows 版本・ARM64・Server・遠端桌面・沒有可互動桌面的工作階段・真的缺編碼器 | 這裡安排不出第二個作業系統；那些判準改成離線注入假的內部版本來判（`tests\compat.ps1`、`tests\capabilities.ps1`）                  |
| `tiff` 與 `gif` 在 `--scale` 之下的下場，以及其他只在編碼器那一層的行為                    | `tests\scale.ps1` 判的是 `png` / `bmp` / `jpeg`；另外兩種共用那道編碼呼叫，但沒被涵蓋                                             |
| 能力查詢「靠截圖或靠彈框來探測」這件事，以及確認框真的被顯示出來的場景                     | 去探測就等於去做了它本來只回報的那件事（`encoder_state_not_probed`、`consent_dialog_state_inferred_not_probed`）                  |
| 歷史副本遇到「真盤滿」「真沒權限」那兩種現場                                               | 造這兩樣要改這台機器的儲存設定或 ACL，而本任務不許；離線那一層判的是**歸類**（那一次呼叫交回的 win32 與穩定碼原樣進 `errors`，不被摺成別的原因，也不被隨後看見的逾時覆蓋），真機那一層用「那個位置是檔案」與「是個重解析點」兩種能誠實造出來的現場（`tests\history.ps1`） |
| 真人在確認框上答「否」那一次不建立歷史                                                     | 判準不彈真實確認框、也不替任何人答它。「主交付沒成功就不發布副本」由離線那一層判（半段標準輸出與寫入失敗那兩種都斷言歸檔一次都沒被呼叫），目錄確實沒多出來的那一條由 `tests\consent.ps1` 與 `tests\history.ps1` 第 7 節各自負責 |

## 給 AI / 腳本的呼叫指南

這個工具就是為程式化呼叫設計，照下面這套約定做最省事。專案裡還附了一份教 AI 使用它的 skill：
`.agents/skills/yashi-evernight-capture/`（裡有 `SKILL.md`、`references/cli-contract.md` 和一份 exe 副本）。
安裝（見「安裝」一節）預設會裝到 `%UserProfile%\.agents\skills\yashi-evernight-capture`；早期版本曾用名
`ecapture-screenshot`，見到那個名字當成同一個 skill 即可。

1. **先問一次能力，再用 `--list` / `--inspect` 發現視窗，最後真的截圖。** `--capabilities` 是唯讀的：不取像素、不彈確認框、
   不寫檔案，所以不會打擾任何人，適合放在自動化流程最前面。它把這台機器的版本、工作階段、螢幕拓撲、每條路線的
   `available` / `unavailable` / `unverified`、每種格式、`--yes` 到底管哪幾條內部路徑，以及取值上限一次交給你，
   於是「該用哪條 `--capture`」「這次失敗該換通道還是這台機器不行」「這裡彈框有沒有人會答」在動手之前就有答案。
   要送出問題報告就再跑一次 `--diagnostics`（同一批判準，另加可核對的建置識別碼；`-v` 展開每一問的原始答案）。
   兩份文件都不隨 `--lang` 變（全 ASCII），所以比對結果穩定。注意 `available` **不是**「這個視窗一定截得到」：
   驅動、受保護內容、HDR 都不在那層的斷言裡，文件末尾的 `caveats` 就是寫來釘住這一時點的。
   之後照舊 `--dry-run`：它不取影格、不寫檔案、也不彈確認框，候選在 `notes[0].value`：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。`--dry-run` 也不必給 `--out`
   （那一次沒有圖片要交付，所以結果整份在 stderr，並帶一條 `note.output_defaulted_stdout`）；而**只給 `--dry-run` 不給任何視窗條件 = 文字說明 + 退出碼 2**。
   要的是**清單**而不是一行人話時，用 `--list`（結構化、分頁，多匹配不算錯誤，一個都沒命中是空清單 + 退出碼
   `0`）與 `--inspect`（一扇視窗，多匹配仍算歧義 —— 它不會替你挑一個）。兩條都不取畫素、不彈框，`--yes`
   對它們也沒有任何作用。它們交回的是快照：真去截圖仍要複核目標身份，所以請把**剛做完的一次** `--inspect`
   裡的句柄傳給截圖，而不是緩存早前那一輪的。詳見上文《結構化的視窗發現與檢查》一節。
2. **依 `errors[].code` 分支，不要比對 `message` 文字**（那會隨 `--lang` 變），也不要拿「有沒有給 `--out`」當原因——
   省略它那條路與 `--out -` 報的是同樣的碼。自動化呼叫端真正會撞上的幾組：

   | 這批 code                                                                                                                                                                                                                                                                                                                                                                                                                                                           | 退出碼 | 下一步                                                                                                                                                                                                                                                                                          |
   | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
   | `cli.invalid_number`、`cli.invalid_value`、`cli.invalid_format`、`cli.unknown_option`、`cli.unknown_language`、`cli.crop_conflict`、`cli.query_conflict`、`cli.window_query_conflict`、`cli.monitor_conflict`、`cli.monitor_selector_empty`、`cli.monitor_selector_kind`、`cli.stdout_multiple_targets`                                                                                                                                                             | 1      | 改命令列——一張都沒截、一個框都沒彈、一個檔案都沒寫                                                                                                                                                                                                                                              |
   | `match.index_out_of_range`、`match.monitor_out_of_range`、`match.roi_out_of_range`                                                                                                                                                                                                                                                                                                                                                                                  | 1      | `hint` 列了全部候選；那條裁剪矩形裝不下選定時的目標                                                                                                                                                                                                                                             |
   | `match.no_window`                                                                                                                                                                                                                                                                                                                                                                                                                                                   | 4      | 把條件放寬，或者目標被最小化了（最小化的視窗一律截不到）                                                                                                                                                                                                                                        |
   | `match.ambiguous_window`、`match.monitor_ambiguous_id`                                                                                                                                                                                                                                                                                                                                                                                                              | 5      | 有好幾個東西同時命中，而工具不替你挑一個——用 `--index` / `--topmost-match` / `--all` 消歧，或改用 `--screens` 交回的識別                                                                                                                                                                        |
   | `match.monitor_unknown_id`                                                                                                                                                                                                                                                                                                                                                                                                                                          | 4      | 那個識別此刻不在桌面上了；再跑一次 `--screens`，別猜編號                                                                                                                                                                                                                                        |
   | `capture.access_denied`、`capture.consent_timeout`、`capture.consent_unavailable`                                                                                                                                                                                                                                                                                                                                                                                   | 6      | 這是人那一邊的邊界：回答不是「是」（這個 `MB_YESNO` 框上，人能給出的唯一拒絕就是答「否」）、期限之內沒人回答、或根本沒有可互動的桌面。本次請求剩下的部分沒有被嘗試——再跑一次是一筆新請求，不是重試                                                                                              |
   | `match.timeout`、`capture.worker_failed`、`capture.failed`、`capture.frame_timeout`、`capture.window_gone`、`capture.frame_invalid`、`capture.roi_invalid`、`capture.roi_unmeasurable`、`capture.monitor_changed`、`capture.monitor_unverifiable`、`capture.monitor_id_unverifiable`、`capture.consent_stale`、`capture.timeout`、`capture.hdr_refused`、`capture.hdr_unverifiable`、`capture.target_gone`、`capture.target_changed`、`capture.target_unverifiable` | 7      | 先讀 `stage`（`match` / `capture` / `encode`）再決定下一步；通常該重新問一次 `--list` / `--screens`，而不是再換一個 `--capture`                                                                                                                                                                 |
   | `env.os_too_old`、`env.channel_unsupported`、`env.hdr_unsupported`、`env.cursor_unsupported`                                                                                                                                                                                                                                                                                                                                                                        | 7      | 是**這台機器**給不出你要的東西——見下面那一段                                                                                                                                                                                                                                                    |
   | `io.write_failed`、`io.file_exists`、`io.output_collision`、`io.timeout`                                                                                                                                                                                                                                                                                                                                                                                            | 8      | 目錄要先存在，或挑一個撞不上的名字；`io.file_exists` 就是 `--no-overwrite` 在盡職；這裡的 `io.timeout` 指的是那一步根本沒開始的寫——如果一張檔案**已經落地**、只是寫完之後的期限複核越了線，那張圖仍留在 `images` 裡、本次按部分成功報告退出碼 `7`（已交付＋出錯），見《執行期限與會阻塞的呼叫》 |
   | `history.unavailable`、`history.write_failed`、`history.file_exists`、`history.budget_spent`、`history.same_file` | 7      | **主圖已經交付，只有那份歷史副本沒落地**（後兩條是「這一次根本沒開始寫副本」）。看 `images[].history.status` 與同碼那條 `errors`（`stage=history`）就知道是哪一路；**不要為此重拍一次**——重拍既補不上副本，又多要人批准一張。想換的應該是那個位置本身（可寫、是目錄、不是重解析點），而不是命令列 |
   | `capture.hdr_unsupported`、`capture.cursor_unsupported`、`capture.unsupported`                                                                                                                                                                                                                                                                                                                                                                                      | 1      | 這一組選項搭配在解析期就被拒了，還沒有到彈框那一步                                                                                                                                                                                                                                              |

   那四條 `env.*` 說的是這台機器、不是這個目標，對同一扇視窗重試沒有任何意義：`env.os_too_old` 是本機 Windows
   內部版本低於所有格式共用的那唯一一套編碼器所在的下限（換 `--capture` 也不會變好），`env.channel_unsupported` /
   `env.hdr_unsupported` / `env.cursor_unsupported` 則是明確寫下來的那項要求，本建置加上本機版本湊不出來——工具
   不會自己把你指定的那條通道頂替掉好讓錯誤消失。`note.channel_unavailable` 說的是 `auto` 鏈裡被去掉的那一條，
   剩下的幾條仍可能截成交功。先問一句「這台機器給得出哪幾條通道」不必截圖：`--verbose` 的 `input.osBuild` 與
   `input.captureChain` 就是它。那幾道下限、宣告範圍與實測範圍見[系統支援](#系統支援)。
   每條錯誤還帶 `target` / `backend` / `stage` / `hresult` / `win32`（見前面「輸出形式」的規則），拿到多少寫多少，
   不必從 `message` 裡摳。

3. **讀取資料流要分情況**：給 `--out <檔案>` 時 JSON 在 stdout、stderr 是空的，直接解析就行；用 `--out -` 或沒給輸出路徑時
   圖片位元組佔了 stdout，JSON 整體改到 stderr。stdout 一次只交付一張圖，多個目標請寫到檔案。在 shell 那一層也要把
   兩條串流分開，而且要知道你的 shell 會對位元組做什麼：`cmd` 與 PowerShell 7.4 起保留原生命令 stdout 的位元組流，
   Windows PowerShell 5.1 不保留（它把 stdout 當文字重新編碼，還用自己的錯誤記錄去排版原生 stderr），所以在 5.1
   請改要一個檔案、或用 `Start-Process -RedirectStandardOutput` / `-RedirectStandardError`、或把那條命令放進
   `cmd /c` 裡跑。對圖片而言 `2>&1` 永遠不是答案。實測數字與細節見
   [標準輸出圖片位元組的 shell 差別](#標準輸出圖片位元組的-shell-差別)。
4. **別把非 0 退出碼當成全盤失敗**：部分成功時 `captured` 大於 0 而退出碼是 7，已經寫出的圖照樣可用；
   `images[].source` / `path` / `scope` 分別告訴你那張圖出自哪條通道、走了哪條內部路徑、像素是視窗自己的還是螢幕上的。
   `images[].history.status` 是**另一件事**：那張圖已經交付了，而它那份歷史副本可能 `failed` 或 `skipped`。
   這時候圖照用、**不要重拍**（重拍不會補上副本，只會再多要一次批准）；要處理的是那個位置寫不寫得下去，
   或者接受「這一張只有主輸出」。見[截圖歷史歸檔](#截圖歷史歸檔預設開啟)。
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

## 安裝（Windows，普通使用者不需要開發工具）

使用這個程式完全不需要工具鏈：不用 Visual Studio、不用 CMake、也不用原始碼樹。那些只在**建置安裝封裝**時才需要（見下一節）。

### 1. 取得安裝封裝

倉庫根的 `build-installer.ps1` 會產生 `build\installer\EvernightCapture-<版本>-<架構>-setup.exe`，並在旁邊寫一份 `.sha256`。**本專案目前還沒有發佈附件，所以這裡給不出下載網址**——在有之前，安裝封裝只能從檢出（checkout）建置出來；執行前請先對照 `.sha256` 核對檔案。

### 2. 系統需求

- Windows 10 build **18362** 以上，**x64**。這個二進位自己宣告並強制的下限由 `ECAPTURE.EXE --capabilities` 報出（`os.declaredMinBuild`，目前機器則是 `os.build`）；真正實測過的範圍見上面的「系統支援」。
- **不需要任何 VC++ 執行階段**：執行階段是靜態連結的，沒有額外要裝的東西。
- **不需要管理員權限**：安裝程式按目前使用者安裝。就算你選的目錄它寫不進去，它也只如實報錯，不會自動提權、也不會改裝到別處。
- HDR、多螢幕、桌面路徑的確認框都是**執行時期**特性，不是安裝需求；它們不改變安裝程式需要什麼。

### 3. 執行安裝精靈並選擇目錄

預設目錄是 `%UserProfile%\.agents\skills\yashi-evernight-capture`，精靈允許你選任意目錄。**你選的目錄就是 Skill 根目錄**：`SKILL.md` 直接位於其中，`ECAPTURE.EXE` 就在它旁邊，不會再多套一層同名目錄。升級時安裝程式會記住你上次選的目錄。

```bat
:: 安裝後做一次唯讀自檢（cmd.exe）。路徑要帶引號：預設路徑沒有空白，自訂路徑可能有。
powershell -NoProfile -ExecutionPolicy Bypass -File "%UserProfile%\.agents\skills\yashi-evernight-capture\verify-install.ps1"
```

```powershell
# PowerShell：同上，另跑一遍可選的安裝版離線測試
& "$env:USERPROFILE\.agents\skills\yashi-evernight-capture\verify-install.ps1" -RunOfflineTests
# 自訂安裝目錄同理——顯式傳進去，不要假設預設路徑：
& "D:\我的 工具\ecapture\verify-install.ps1" -InstallDir "D:\我的 工具\ecapture"
```

### 4. 唯讀安裝自檢

`verify-install.ps1`（裝在所選目錄的根）**不需要 Visual Studio、不需要原始碼樹、也不需要網路**。它依次核對：`install-manifest.json` 裡每個檔案是否存在、大小與 SHA-256 是否一致；四份 README 與 `SKILL.md` 裡的相對連結是否都指向安裝目錄內真實存在的檔案；`ECAPTURE.EXE --version` 是否退出碼 0 且版本與宣告一致；`--capabilities` / `--diagnostics` 是否退出碼 0、可解析為 JSON，且 `program.version` / `arch` / `buildId` 與清單一致；`--help` 是否退出碼 3（這是它的契約，不是失敗）。加 `-RunOfflineTests` 時，它會用隨包的 `test-all.ps1` 跑清單宣告的離線測試集，且指向**本目錄**的 `ECAPTURE.EXE`（絕不誤用 `PATH` 上的舊版），日誌寫在系統臨時目錄。退出碼：`0` 全過，`1` 有失敗，`2` 前置不成立（目錄不對、沒有清單）。真實視窗 / HDR / 多螢幕 / 確認框測試**不在這裡跑**，那要由人在合適的環境裡安排。

### 5. 讓 AI 工具發現並使用這個 Skill

三件不同的事，混為一談通常就是「裝了沒用」的原因：

- **檔案安裝**——`SKILL.md` 與 `ECAPTURE.EXE` 實際在哪。就是你選的那個目錄。
- **Skill 發現**——工具會不會自己找到那個目錄。**不是每個工具都會掃 `.agents\skills`。**
- **命令執行權限**——裝了檔案不等於有權截圖。上面「截圖授權與 `--yes`」那套規則照舊，尤其是桌面確認框仍然要由人回答。

**OpenCode**（已對照其官方 Agent Skills 文件核實）：除了 `.opencode/skills/<名稱>/SKILL.md` 與 Claude 相容路徑，它還會載入全域「agent 相容」路徑 `~/.agents/skills/<名稱>/SKILL.md`；在 Windows 上就是 `%UserProfile%\.agents\skills\<名稱>\SKILL.md`，與本安裝程式的預設目錄完全一致，所以**預設安裝**會在下次啟動時被自動發現。它的規則還要求 `name` 與所在目錄名一致（這裡是 `yashi-evernight-capture`），別只改其中一個。**自訂目錄不會被自動發現**，需要手動指向它。

對不會自動發現的工具，把目錄顯式交給它（這是「指路」，**不是**自動註冊）：

```text
請顯式讀取這個路徑的 SKILL.md：<安裝目錄裡 SKILL.md 的絕對路徑>，並按其說明執行。
呼叫同一個目錄裡的 ECAPTURE.EXE，用絕對路徑並加引號。
先只做唯讀檢查（--version、--capabilities）。不要用截圖來證明安裝成功。
```

### 6. 升級、卸載與排障

- **升級**：跑新的安裝封裝、保持同一個目錄即可原地升級，並會記住你的選擇。失敗或中斷會回復（rollback），不會留下寫了一半的目錄。
- **換目錄**等於裝第二份獨立副本：舊目錄原樣保留（不會替你遷移或刪除）。兩份都在時，卸載項目會各自列出——刪掉不要的那份，或讓工具只指向其中一份。
- **同名但不屬於本產品的目錄**：安裝程式會說缺少 `install-manifest.json`，並在動手前問你，絕不會靜默覆蓋原有內容。無人值守安裝（`/VERYSILENT`）沒有可問的人，於是它**一個檔案都不寫**，以非 0 退出碼中止，原因記在 `/LOG` 那份日誌裡。
- **卸載**只刪安裝程式記錄過的檔案。你後來加的檔案、日誌、相鄰的其它 Skill 一律保留；不會遞迴刪除 `.agents`、`skills` 這些父目錄或你目錄裡的其它內容。
- **截圖歷史**（`<安裝目錄>\history\日期\`，見[截圖歷史歸檔](#截圖歷史歸檔預設開啟)）在安裝程式眼裡與「你後來加的檔案」同一類：它不在受管清單裡，也不在 `install-manifest.json` / `payload.sha256.txt` 這份發行清單裡。**升級、重裝與卸載都不刪它、不改它、也不把它當使用者改動備份**；目錄裡多出這一棵也不會讓安裝程式把這個位置誤判成「不屬於本產品的目錄」（歸屬只認那個 ownership 標記）。卸載後歷史仍然留在那個目錄裡，位置會在完成頁與 README 裡說明；要清理由你自己顯式刪那棵目錄。
- **換安裝目錄時歷史不遷移**：舊的截圖仍然留在舊目錄那一個 `history\` 旁邊（歸檔跟著當時執行的 exe 走）。想繼續用舊的那一批就自己拿過去，或讓工具同時知道兩個位置。
- **檔案被佔用**：安裝程式如實報出被佔用的檔案並回復，不會為了讓安裝通過去結束你的程式，也不會假報成功。
- **安裝位置被安全策略限制**：若所選目錄落在某種「該目錄裡的程式不許建立臨時目錄」的策略之下（企業終端防護常見），原地解除安裝會報 `Setup was unable to create the directory "…-uninstall.tmp". Error 5`，隨包自檢的 `delivery` 那層會報「臨時目錄建立得出來」失敗。這是環境在拒絕，不是產品壞了：換一個目錄重裝，或請管理員放行這個目錄；`verify-install.ps1` 的唯讀部分（檔案雜湊、文件引用、二進位身分）不受影響，照常核對。
- **「裝了但 AI 不理它」**：先確認那個目錄確實是工具會掃描的（見上），然後核對二進位身分而不是版本號字串——把 `install-manifest.json` 的 `version`、`arch`、`buildId` 與 `ECAPTURE.EXE --capabilities` 對上。原始碼檢出旁的舊 `ECAPTURE.EXE` 可能滯後；**新裝的那份不會**，因為它來自本次安裝程式自己的建置。

## 建置與測試（維護者）

| 命令                        | 這條測什麼                                                                                                                                                                                                                                                                                                              |
| --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `.\build.ps1`               | Release 建置，產物 `build\ecapture.exe`；`-Config Debug`、`-Clean` 可選                                                                                                                                                                                                                                                 |
| `.\tests\cli.ps1`           | 輸出契約本身：解析期錯誤與退出碼、`--yes` 與 `--no-overwrite` 的每種布林寫法、查詢與截圖選項互斥那幾組、兩條串流分離、省略 `--out` 與 `--out -` 的等價對拍、四語文案。一律 `--dry-run`，不截圖、不寫檔                                                                                                                  |
| `.\tests\windows.ps1`       | `--list` / `--inspect`：分頁、可見性策略、逐欄位可讀性、文件形狀、隱私、歧義，以及這條查詢真的什麼都不碰（離線 `build\ecapture-windows-tests.exe` 加上自建視窗）                                                                                                                                                        |
| `.\tests\screens.ps1`       | `--screens` 與 `--monitor=device:` / `id:`：哪一種身分穩到哪一層、交回的選擇器真的選到那一塊面板，以及不存在的識別絕不退化成主螢幕                                                                                                                                                                                      |
| `.\tests\capabilities.ps1`  | `--capabilities` / `--diagnostics` 對上假探針（一塊螢幕都沒有、正好低於某條下限、版本問不出來、某個編碼器沒登記、`--yes` 的適用範圍與登記表一致、兩份查詢共享同一批判準），加上真機那一半：查詢不會卡在框上、不落地、與 WMI 及 `--dry-run -v` 兩處獨立值同源、全 ASCII 所以 `--lang` 動不了它                           |
| `.\tests\compat.ps1`        | 那幾道版本下限：假 Windows 內部版本注進生產判準本體、被擋下的顯式通道絕不替換、`auto` 鏈少的是哪一條，以及發布版二進位檔那份 Windows 8 世代的 winrt / job API Set 匯入                                                                                                                                                  |
| `.\tests\channels.ps1`      | 六條通道對上自己建的視窗，並帶遮擋對照。視窗內容那幾條帶 `--yes` 跑、一旦彈框就判失敗；`bitblt` / `duplication` 的畫面判據要 `-SimulateConsent` 才跑                                                                                                                                                                    |
| `.\tests\wgc.ps1`           | WGC 的即時尺寸：內容尺寸對紋理尺寸、影格池重建，以及「被裁掉的影格絕不宣稱是整扇視窗」（離線 `build\ecapture-wgc-tests.exe` 加上它自己改尺寸的視窗）                                                                                                                                                                    |
| `.\tests\image.ps1`         | 影格形狀守衛與画素操作，跑在手工擺出的排布上（直條紋、棋盤格、alpha、行末填充、過大與過短的緩衝、越界裁剪），加上真機的單色擷取                                                                                                                                                                                         |
| `.\tests\crop.ps1`          | `--roi` / `--client-area`：幾何對照三條獨立的 Win32 問答與画素內容、在彈框與寫檔之前就被擋掉，以及桌面路徑上極小的裁剪照樣要問人                                                                                                                                                                                        |
| `.\tests\scale.ps1`         | `--scale`：不放大、取最緊的那條上限、向下取整、最近鄰映射逐點對拍、先裁後縮的順序，以及 `png` / `bmp` / `jpeg` 寫下的都是縮過的尺寸                                                                                                                                                                                     |
| `.\tests\dup.ps1`           | 桌面複製：四種旋轉對照正式碼的幾何判據、一份假的雙配接器輸出表、`requestedRect` / `capturedRect` / `clipped` / `rotation` 對真實圖片，以及「那張螢幕在確認之後變了」那些案例。測試從不重新排列、也不重新旋轉顯示器                                                                                                      |
| `.\tests\consent.ps1`       | 那一張分層表與拒絕怎麼傳開：離線用注入的假應答器跑完整台 `ConsentGate` 狀態機，真機則所有確認框一律代答「否」——哪些路徑必須彈、被拒之後報什麼、有沒有落地。**絕不代人答「是」**                                                                                                                                       |
| `.\tests\identity.ps1`      | 身份複核的兩檔與它們進行的順序、`capture.target_gone` / `capture.target_changed`，以及 Z 序選擇（`--topmost-match` / `--bottommost-match`）                                                                                                                                                                             |
| `.\tests\timeout.ps1`       | 期限與輔助程序：同一份預算後面的步驟只能花剩下的、輔助程序的管線協定把畸形的回包一律拒收而不是「看起來像成功」，以及輔助模式拒絕被當成公開選項啟動                                                                                                                                                                      |
| `.\tests\isolation.ps1`     | 產權：同名的既有處理程序保持存活且不會被當成目標、並發兩輪互不串、異常退出只清理自身                                                                                                                                                                                                                                    |
| `.\tests\cursor.ps1`        | `--cursor`：那張按路徑登記的表、鏈在 19041 那道門檻兩側怎麼收窄、`cursorRequested` / `cursorEffective` / `cursorBasis` 怎麼合成，外加一條原始碼層級的掃描——只要 `src/` 裡出現取指標形狀、畫滑鼠指標、動使用者滑鼠那類呼叫就紅                                                                                         |
| `.\tests\hdr.ps1`           | `--hdr`：DXGI 格式與顯示色彩空間的分類（認不出就留 `unknown`）、tone 曲線的性質、`ConvertWideFrameToSdrBgra8` 逐點判、那組結果鍵，以及誠實的 SDR 真機場景（預設那些鍵不出現、對 SDR 影格要求處理時發 `note.hdr_source_sdr`）                                                                                            |
| `.\tests\save.ps1`          | 檔案交付：每種 `--no-overwrite` 布林寫法對真實檔案的效果、整批輸出名規劃與撞名偵測、原子提交對著被佔用的目標／是目錄的目標／不存在的目錄／寫到一半被硬殺，以及併發禁止覆蓋那場競態                                                                                                                                      |
| `.\tests\history.ps1`       | 截圖歷史歸檔：離線那一層對著生產的 `HistoryArchive` + `Delivery` + `FileSave` 判命名與本地日期同源、同刻兩張／時鐘回撥／跨午夜、同名獨佔提交與換名重試、那一路走不通的失敗歸類、主輸出名字落在 history 樹裡時不自覆蓋，以及「主圖成功而副本失敗」「主圖失敗與半段標準輸出都不發布副本」「主圖落盤後預算才耗盡時副本不開始」「歸檔拋出東西不抹掉交付事實」（`build\ecapture-history-tests.exe`）；真機那一層把 exe 複製進本次目錄，用自建視窗核對落點跟著程式自己、主輸出與副本逐位元組／SHA-256 相同、批次與 `--out -` 那兩路、唯讀與參數失敗一個歷史都不建、重解析點與「那個位置是檔案」時的部分成功 `7` |
| `.\clean.ps1`               | 與 `.\build.ps1 -Clean` 共用 `scripts\build-clean.psm1` 那一份判準：清掉 `build\` 裡的產物，但保留開發版 exe 旁邊的 `build\history`（那是真實截圖資料，不是建置產物）；`build\` 或 `build\history` 是重解析點、或那個名字被檔案佔著時明確拒絕並說明原因，既不靜默刪除也不靜默搬移（`tests\history.ps1` 第 10 節對著這份本體判） |
| `.\tests\screen.ps1`        | 整張螢幕的三條桌面路徑，加紅塊定位與陰性對照。只有 `-SimulateConsent` 會代答，而且只該在專門騰給測試的桌面上這麼用                                                                                                                                                                                                      |
| `.\tests\smoke.ps1`         | 端對端：截自己建立的測試視窗，校驗 PNG 尺寸與像素內容                                                                                                                                                                                                                                                                   |
| `.\tests\invoker.ps1`       | 共用的測試程序呼叫器本身：argv 引號、兩條流同時消費、二進位不被轉碼、卡死的子程序、每次執行各自的暫存目錄                                                                                                                                                                                                               |
| `.\tests\orchestration.ps1` | 一鍵總跑 `test-all.ps1` 本身：篩完是空計畫要明說不算驗收，計畫內套件整套沒跑成（腳本不見了、原始碼解析不乾淨）會讓出口碼判失敗；刻意篩掉的、`-StopOnFail` 主動停下的剩餘與各套件自己記的 SKIP 分開                                                                                                                      |
| `.\tests\build-path.ps1`    | 在含中日韓文字、空白、括號與 `%` 的目錄裡建置（外加一個中文的 `%TEMP%`），以及那個暫存批次檔的正文必須留在 ASCII 之內                                                                                                                                                                                                   |
| `.\tests\streams.ps1`       | 串流與結構化結果的可靠性：單一個目標在 stdout 上交付一張、解析成多個目標的批次在彈框之前就被拒、判據是實際命中的目標數、批次中途失敗時已經截到的圖仍留在 `images`、結果送不到約定那條串流時報退出碼 `8`，以及省略 `--out` 與顯式 `--out -` 在成功／無匹配／歧義／非法參數／後端失敗／被拒絕／寫入斷管這七個場景上的對拍 |
| `.\tests\window_shot.bat`   | 給人跑的走查：逐通道對一扇編譯出來的測試視窗截圖、由人親自點那些確認框，最後是整張螢幕那一步                                                                                                                                                                                                                            |
| `.\scripts\check-lang.ps1`  | 四語文案的 key 與佔位符對齊，並確認 exe 裡真的編進了四份資源                                                                                                                                                                                                                                                            |
| `.\scripts\mkreadme.ps1`    | 用各語言 `--help` 的原樣輸出重新產生四份 README 的說明段；`-Check` 改成只判不寫，過期的說明段就是這樣被抓出來的                                                                                                                                                                                                         |

所有實機測試的目標視窗一律是自家的：`tests\helper\ec_window.cs` 編到本次執行的暫存目錄裡，測試握著它的
PID 與 HWND，因此既不按處理程序名去找目標、也不按處理程序名批次結束，刪除的也只有自己建立的那個目錄。
`tests\harness.psm1` 放著共用的程序呼叫器（argv 引號規則、兩條流併發消費、有期限的等待、逾時只結束自己
那棵程序樹），以及暫存目錄與測試視窗的建立與收尾；`tests\invoker.ps1` 就是用來證明這個呼叫器本身的。

`build.ps1` 用 vswhere 定位 VS，並優先使用 VS 自帶的 cmake/ninja。倉庫目錄、build 目錄與工具鏈路徑只經子程序的
環境塊遞給那個暫存批次檔，正文裡一個絕對路徑都不寫，所以倉庫放在含中文、空白、括號或 `%` 的目錄裡也能照常建置，
而正文一旦混進非 ASCII 會在寫入前被當場攔下（判據見 `.\tests\build-path.ps1`）。建置要求在 `/W4` 下零警告。
在 Git Bash 裡手動測試要先 `export MSYS2_ARG_CONV_EXCL='*'`，否則 `/help` 會被當成路徑改寫、`--out /tmp/x.png` 會被轉成怪異的路徑。

### 測試總入口：`test-all.ps1`

`test-all.ps1` 只做編排，自己不含任何判據：先建置（除非你讓它別建），再按固定順序**串行**跑上面那些套件（子程序走共用的 `tests\harness.psm1` 呼叫器），最後輸出一張匯總表與日誌目錄。

```powershell
.\test-all.ps1                       # 建置 Release，全跑
.\test-all.ps1 -Quick                # 不跑 build-path.ps1，並讓 timeout.ps1 跳過長等待
.\test-all.ps1 -Only cli,windows     # 只跑這兩套
.\test-all.ps1 -Except build-path    # 除了最慢的那套
.\test-all.ps1 -Offline              # 只跑各套件的離線層
.\test-all.ps1 -List                 # 只印計劃，不建置也不啟動任何測試
.\test-all.ps1 -NoBuild              # 用現有 build\ecapture.exe
.\test-all.ps1 -Exe D:\tools\ECAPTURE.EXE   # 或直接指一個二進位
.\test-all.ps1 -StopOnFail           # 第一套失敗就停
```

- **`-List` 只是預覽**：印出計劃，什麼都不啟動；篩完沒有可跑套件按「前置不成立」報錯，**空計劃永遠不會被當成成功**。
- **`-Offline` 按元資料選路，不靠名字猜**：純離線套件（唯讀 CLI 查詢、純函式與協定、臨時檔案）照常跑；混合套件帶上它自己宣告的開關（`-SkipReal` / `-OfflineOnly`），並**在任何東西啟動之前**就寫進命令列——本來會開視窗的混合腳本根本沒有機會先開窗；只有真實層、又沒宣告離線入口的套件，在計劃階段就整套移出並逐條給原因（`OFFLINE-EXCLUDED`），一個位元組都不啟動。宣告了離線入口卻找不到那個參數的，屬於元資料與原始碼不一致：**不跑並判失敗**，絕不退化成「沒開關就照跑真實層」，也不悄悄當成合理 SKIP。`-Offline` 仍可要求 Windows 工具鏈（有一套會用系統自帶的 .NET Framework `csc` 在臨時目錄編一個輔助視窗程式），它不是「斷網也能跑」的承諾。
- **確認不被假定**：`-Force` 只免去「按 Enter 開始」那句，**不能**代答真實確認框，也不能與那些代答開關併用。`-Offline` 與 `-SimulateConsent` / `-TimeoutConsent` 在任何執行之前就被判衝突（`OFFLINE-CONSENT-CONFLICT`）；給一個並不接受該開關的套件賒 `-TimeoutConsent` 同樣被拒。代答確認框必須在提示後親自輸入 `yes`：輸入被重導向、無主控台、非互動一律按**沒確認**中止，絕不預設同意。
- **退出碼**：`0` = 計劃內套件全部跑到並通過（套件自己記的 SKIP 算未驗證，不算失敗）；`1` = 至少一套 FAIL / TIMEOUT / NO EXIT / NO START，**或計劃裡點名要跑的整套根本沒啟動**（腳本不見了、原始碼解析不乾淨、離線元資料不符）；`2` = 前置不成立（建置失敗、找不到產物、參數不認識、篩完沒有可執行測試、參數衝突、已有另一個總跑在跑）。建置失敗以退出碼 2 結束，絕不拿舊產物繼續。
- **匯總只計真實結果**：「交出結果」只算真正給出終態的套件，NO START / NO EXIT 不算。三種「沒跑/未驗證」分開寫、不合并成一個數字：被 `-Quick` / `-Only` / `-Except` / `-Offline` 刻意沒排的、因 `-StopOnFail` 停下的剩餘、以及意外整套沒跑成的（只有最後這類判退出碼 1）；各套件自己記的 SKIP 屬於環境與安全邊界，既不升成失敗也不當成通過。
- **日誌**寫在 `build\test-logs\<時間戳>\`（每套一份，另有一份合并的）；`build\` 已在 `.gitignore` 裡。

### 建置安裝封裝：`build-installer.ps1`

這個根腳本就是要從任意工作目錄按**絕對路徑**呼叫：倉庫位置取自腳本自身，不取自目前目錄。

```powershell
# PowerShell（維護機，任意目錄）
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1"                 # Release 建置 + 安裝封裝
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1" -Clean          # 先乾淨重建
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1" -StageOnly      # 只建置 + 收集/驗證乘載，不需要編譯器
& "$env:USERPROFILE\src\EvernightCapture\build-installer.ps1" -SkipBuild      # 复用 build\，仍核對身分
```

```bat
:: cmd.exe 同理，注意引號；%UserProfile% 由 cmd 展開，不是腳本
powershell -NoProfile -ExecutionPolicy Bypass -File "%UserProfile%\src\EvernightCapture\build-installer.ps1"
```

- **先建置，失敗即停**（退出碼 11）：呼叫 `build.ps1`（`-Config` 預設 `Release`），產出程式**與**全部測試程式。`-SkipBuild` 是顯式選擇复用 `build\`，但依舊核對真源（版本取自 `src\Version.h`、PE 架構、`program.buildId`）——建置失敗絕不退回舊產物，「檔案存在」也絕不等於「本次建置成功」。
- **乘載收集**按 `installer\payload.manifest.json` 落到 `build\` 下的獨立 staging：`ECAPTURE.EXE`、`SKILL.md` + `references\`、四份 README、`LICENSE`、`resources\icon.ico`、`verify-install.ps1`、`test-all.ps1`、`tests\`（腳本、`harness.psm1`、輔助視窗原始碼、手工精靈批次檔）與 `build\` 下的測試二進位。內容守衛會攔下原始碼檔案、開發用 `AGENTS.md` / `MEMORY.md`、日誌與截圖；若乘載裡的 `ECAPTURE.EXE` 不是本次建置的那份，同樣攔下。
- **乘載清單** `install-manifest.json`（另有 `payload.sha256.txt`）記下版本、架構、組態、`buildId` 與每個檔案的 SHA-256；安裝後的 `verify-install.ps1` 就是拿它核對。
- **編譯器**要求 Inno Setup 6.3 或更高（腳本用到 `ArchitecturesAllowed=x64compatible`）。它會在 `PATH` 與常見安裝目錄裡找，也可以用 `-IsccPath "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"` 顯式指定。腳本不會自動下載或安裝任何工具：找不到編譯器就印出缺什麼並以退出碼 `32` 結束（`-StageOnly` 不需要編譯器，照樣能跑）。`build.ps1` 自己的依賴是帶 C++ 桌面工作負載的 Visual Studio，由 `vswhere` 定位。
- **產物**：`build\installer\EvernightCapture-<版本>-<架構>-setup.exe` 與緊貼的 `<...>.exe.sha256`。版本取自 `src\Version.h`（沒有第二份手寫版本），命名帶版本與架構，末尾摘要會重中路径、版本、`buildId` 與 SHA-256。`build\`（staging、安裝封裝、日誌）已在 `.gitignore` 裡，不含入版發佈產物。帶時間戳的產物不承諾逐位元組可重現。
- 兩個腳本都不要求管理員權限，也不修改系統的執行原則；上面的例子只用 `-ExecutionPolicy Bypass` 對單一程序生效，不是全域設定。

## 授權

EvernightCapture 採用 [Mulan PSL v2（木蘭寬鬆許可證，第2版）](http://license.coscl.org.cn/MulanPSL2)，
中英雙語全文見 [LICENSE](LICENSE)。

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
