![EvernightCapture 图标](resources/icon.ico)

# EvernightCapture

命令行窗口截图工具：按条件筛出窗口，把那个窗口的画面存成图片文件。

[English](README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja-JP.md)

基于 Windows.Graphics.Capture 的一整套取图通道，入口是 `ECAPTURE.EXE`——单个可执行文件，静态链接 CRT，
目标机器不需要装 VC++ 运行时。输出对程序友好：`--help`、`--version` 与"一个条件都没给"这三种情形是纯文本，
其余一律 JSON；退出码稳定，诊断带稳定的 `code`，所以既适合人敲，也适合被脚本和 AI 调用。

当前版本 **0.4.0**：`--capture` 的取值全部可用（`wgc` / `dwm` / `printwindow` / `bitblt` / `duplication` / `auto`），
`--monitor` 提供整块屏幕截图与"按屏过滤窗口"。更早的构建里还有一条 `magnification` 通道，已经删除：Windows 11 的
`magnification.dll` 不再导出 `MagGetImage`，这条路线因此没有离屏读图的入口，只能把放大镜控件窗口摆到屏幕上抢 z 序，
而拿到的画面与 `bitblt` 已经交回的等价——代价换不到收益。这段依据记在 `src/CliOptions.h` 的注释里。

## 先看哪里

第一次来这里：[快速开始](#快速开始)（装好、截第一张图、始终给出一个输出文件）→ 《结构化的窗口发现与检查（`--list` / `--inspect`）》（截图之前先认出目标）→ [截图授权与 --yes](#截图授权与---yes)（哪一步一定要人来确认）→ [退出码](#退出码)（出了什么事、下一步做什么）。

| 想弄清的事                                                     | 看哪一节                                                                               |
| -------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| `--yes` 究竟免掉了哪一次确认，哪些是任何开关都免不掉的         | [截图授权与 --yes](#截图授权与---yes)                                                  |
| 图片字节走哪条流、JSON 走哪条流，以及哪种 shell 保得住那些字节 | [输出形式](#输出形式)、[标准输出图片字节的 shell 差别](#标准输出图片字节的-shell-差别) |
| 期限预算怎么在整个请求里共用，以及它打断不了什么               | 《执行期限与会阻塞的调用》                                                             |
| 稳定的 `code`、`stage`、退出码与部分成功                       | [退出码](#退出码)                                                                      |
| 什么是编进这个构建的、本机现在让不让走、什么才是真测过         | [系统支持](#系统支持)、《只读的能力查询》                                              |
| 本仓库在这台机器上还没判清楚的问题                             | [边界与未验证项](#边界与未验证项)                                                      |
| 哪条测试判的是哪条规矩                                         | [构建与测试](#构建与测试)                                                              |

## 特性

- **按条件筛窗口**：句柄 / 进程 ID / 映像名 / 完整路径 / 标题（精确、包含、正则）/ 窗口类名，不同选项之间 AND、同一选项写多次 OR
- **六条取图通道**：被遮挡的窗口也能截（`wgc` / `dwm` / `printwindow`），或者故意只拷屏幕上可见的像素（`bitblt` / `duplication`）
- **多窗口一次截完**：`--all` 每个命中窗口各存一张，配合 `%i` 之类占位符命名
- **窗口内部裁剪**：`--roi x,y,w,h` 从交付的整窗图像里留一块，`--client-area` 只留客户区。坐标说的是**这张图像自己的像素**（左上角 = (0,0)，物理像素，不按 DPI 缩放），永远不会被当成桌面绝对坐标；放不下的矩形是拒绝，绝不往里挪、裁到边上为止或退回整窗交出。完整规则见下文《窗口内部裁剪（`--roi` / `--client-area`）》
- **等比缩小**：`--scale max-width=N,max-height=N,max-pixels=N` 用三条天花板里最紧的那一条算出的**同一个比例**把图等比缩进上限，宽高各自向下取整，**绝不放大**，插值只有最近邻这一种可预测的整数映射。顺序是先裁后缩，所以缩的是裁完的那一块。完整规则见下文《等比缩小（`--scale`）》
- **截图授权**：凡是真要取帧的截图，连可靠的窗口通道也一样，先弹模态确认框；`--yes` 只免掉"帧绑在所选窗口本身、
  不从桌面采样"那一条层的确认——任何会拍到桌面像素的路径一定要人答，没有开关能跳过
- **目标身份会复核**：选定目标那一刻记下句柄、归属进程、该进程的创建时间、窗口类名与当初的选择条件，在每一次取帧尝试之前、
  以及人工确认回答之后各复核一遍。目标被销毁、句柄被另一个进程占用、或者它已经不再满足当初挑中它的条件时，交回的是
  `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`，而不是一张没人批准过的画面
- **兜得住的超时**：`--timeout-ms` 是自动处理那一段（条件匹配、后端重试、等帧、编码、写文件）共用的同一份预算，
  `--consent-timeout-ms` 单独给确认框计时；`PrintWindow`、DWM 回读、正则求值这些要等别的进程的调用，都跑在一个
  工具能停下来的辅助进程里，所以卡死的目标窗口再也卡不住这个工具
- **四语文案**：`zh-CN` / `zh-TW` / `en` / `ja`，默认跟随系统显示语言，全部编在 exe 的资源里
- **机器读的 JSON**：只装捕获结果与错误，不带工具名、版本、schema、参数回显之类的元信息
- **窗口可以不截图就列出来看清**：`--list` 把命中的窗口列成结构化 JSON（句柄、PID、类名、标题、映像名、物理矩形、可见/最小化、Z 序，以及后续截图要复核的身份字段），多匹配按 `--offset` / `--limit` 分页而不是报截图歧义；`--inspect` 逐项查清一扇窗口，命中多扇仍算歧义而不会替你挑一个。两条都不取像素、不弹框、不写文件、不触碰任何窗口，`--yes` 对它们没有作用，交回的是一份被明确标注为快照的结果
- **能力可只读查询**：`--capabilities` / `--diagnostics` 在不动一个像素、不弹确认框、不写文件、不联网的前提下
  问出这台机器现在能走哪几条通道、`--yes` 到底管到哪一层、以及可核对的构建标识；「这个构建里有这条路线」「本机
  现在让不让走」「本项目有没有在这种系统上实测过」是三件分开写的事，问不出来就照实写 `unknown`，
  也绝不靠实际截屏或实际编码去探测能力

## 快速开始

需要 Visual Studio（"使用 C++ 的桌面开发"工作负载）+ Windows SDK，构建脚本会自动定位它们。

```powershell
.\build.ps1                                  # Release，产物 build\ecapture.exe
ECAPTURE.EXE --process notepad.exe D:\shots\epad.png   # 取帧之前先弹一次确认框
```

输出目录必须**已经存在**，工具不建目录。先看看会命中谁（不截图、不写文件、也不弹框）：

```powershell
ECAPTURE.EXE --process notepad.exe --dry-run --out D:\shots\_probe.png
```

常用配方：

```powershell
# 标题 + 类名锁定一个窗口
ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png

# 用 dry-run 的候选列表拿到句柄后点名
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite D:\shots\one.png

# 每个命中窗口各一张，文件名带序号
ECAPTURE.EXE --pid 12345 --title-contains 报告 --all "D:\shots\rpt_%i.png"

# 图片字节走标准输出（此时 JSON 改走 stderr）：cmd 与 PowerShell 7.4 起能保证字节无损，
# Windows PowerShell 5.1 会把图片和 JSON 一起弄坏，在那里请默认改用 --out 写文件
# （判据见下文《标准输出图片字节的 shell 差别》一节）
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# 截单个窗口又不想被问：--yes 只对"只取所选窗口画面"的那条路径有效
ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png

# 整屏：拍的是桌面像素，一定要人答，--yes 跳不过
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"
```

## 选项

写法上 `--opt=value`、`-opt`、`/opt` 都接受；取值本身以 `-` 开头时写成 `--title=-x`，或用 `--` 结束选项解析。
短选项**不能**合并（`-qi` 会报 `cli.unknown_option`）。下面这段是 `ECAPTURE.EXE --help` 的原样输出，
改动选项后运行 `.\scripts\mkreadme.ps1` 重新生成，**不要手工编辑这一段的正文**。

<!-- BEGIN ECAPTURE-HELP -->

```text
EvernightCapture (ECAPTURE.EXE) —— 按条件窗口截图，基于 Windows.Graphics.Capture

用法: ECAPTURE.EXE [条件...] <输出路径>        不给任何条件 => 显示本帮助
      ECAPTURE.EXE [条件...] --out <路径>      路径写 - 表示把图片字节输出到标准输出
      ECAPTURE.EXE [条件...]                   不给输出路径 => 图片按 png 写标准输出
      ECAPTURE.EXE --monitor [n] <路径>       给了 --monitor 且没有窗口条件 => 那块屏幕整幅截图

截图目标（不给 --monitor 就只按下面的窗口条件找）
  --monitor, -m [<n|primary|all|device:|id:>] 截图目标屏的编号，从 1 开始、只认十进制（本次枚举的顺序，不保证等于「显示设置」里写的标识号；要认屏请看结果里的 device）；primary = 主屏，all = 每块屏各一张。不给窗口条件时 = 整块屏幕截图，给窗口条件时 = 只算与该屏有重叠的窗口。取值可省略（= 主屏），省略时不吃后面的参数，所以 --monitor out.png 仍然可用。整块屏幕拍的是桌面像素，一定要先弹框问人，--yes 也跳不过；按屏过滤窗口出的仍是窗口图；要点名某一块屏请用 --screens 交回的标识：device:<设备名>（本次桌面连接的名字）或 id:<监视器设备路径>（跨会话那一条）。编号在插拔或改分辨率之后可能指到另一块屏

窗口匹配条件（同一选项多次出现取并集，不同选项必须同时命中）
  --hwnd <handle>                             窗口句柄。纯数字按十进制，0x 前缀或含 a-f 按十六进制；推荐写 0x。不接受正负号与空白，下划线只能夹在两位十六进制数字之间
  --pid <pid>                                 进程 ID，只认十进制且大于 0
  --process, -p <image-name>                  映像文件名（不含路径），忽略大小写；无扩展名时按 .exe 处理
  --exe <full-path>                           映像完整路径，忽略大小写
  --title, -t <exact-title>                   窗口标题精确匹配
  --title-contains, -T <text>                 窗口标题包含子串
  --title-regex, -R <regex>                   窗口标题正则匹配，ECMAScript 语法，匹配期即校验
  --class, -c <class-name>                    窗口类名，忽略大小写，如 Notepad / CabinetWClass

匹配到多个窗口时（互斥）
  --index, -i <n>                             取第 n 个窗口，从 1 开始（十进制），按可见性/叠放次序排序
  --topmost-match                             取 Z 序最靠前的命中窗口（此刻盖在最上面的那一个；与创建时间无关）
  --bottommost-match                          取 Z 序最靠后的命中窗口（此刻被压在最下面的那一个；与创建时间无关）
  --newest                                    --topmost-match 的旧名字。它选的一直是当下的 Z 序位置而不是创建时间（窗口创建时间没有公开 API 可取），保留只为兼容
  --oldest                                    --bottommost-match 的旧名字。它选的一直是当下的 Z 序位置而不是创建时间，保留只为兼容
  --all, -a                                   每个匹配窗口各存一张

取图方式（默认 wgc；受系统版本或窗口性质限制时会失败）
  --capture, -C <method>                      wgc(默认，被遮挡也能截) / dwm(DWM 缩略图，被遮挡也能截) / printwindow(窗口自绘) / bitblt(拷屏幕可见像素) / duplication(桌面复制后按矩形裁，会按显示器的旋转校正方向；只取与该目标重叠最多的一块屏，没截全时结果里带 capturedRect/clipped) / auto(按 wgc-dwm-printwindow-bitblt 回退；整屏截图只用 wgc-duplication-bitblt)。只取窗口自己的画面：wgc / printwindow / dwm 缩略图；会从屏幕上取样：bitblt / duplication 与 dwm 的屏幕退路
  --cursor <default|include|exclude>          画面里要不要鼠标指针：default(默认，本工具一个字都不改，结果里也不出现光标那三个键) / include(要) / exclude(不要)。只有 wgc 有能设进去也读回来的开关（要内部版本 19041 起）；printwindow / dwm / bitblt 的来源本来就没有光标，duplication 的桌面帧却可能已经把指针画在里面、又没有开关，所以 include 配这四条、exclude 配 duplication 都报 capture.cursor_unsupported（两种文案），--cursor default 照旧交图而 cursorEffective 写 unverified；绝不改走读桌面像素的通道，auto 时做不到的那几条从链里摘掉并各留一条 note.cursor_channel_skipped。这一条不改变授权；requested / effective / basis 三件事见 README
  --hdr <auto|tonemap|refuse>                 HDR 来源怎么处理：auto(默认值，本工具对色彩一个字都不改；这条选项整个没写时结果里不出现色彩那组键) / tonemap(把 HDR 帧按固定 tone mapping 映射成 8 位 SDR 交付) / refuse(核实来源是 HDR 就报错，绝不交一张被硬压成 BGRA8 的发白图)。本构建真兑现得了 tonemap/refuse 的只有 wgc 一条，所以配 printwindow / dwm / bitblt / duplication 在解析期报 capture.hdr_unsupported，绝不改走去读桌面像素的通道；--capture auto 时同一条判据会把兑现不了的通道从回退链里摘掉（各留一条 note.hdr_channel_skipped，一条都不剩就是 env.hdr_unsupported），而 capture.hdr_refused 这类策略结论会让整条链立刻停下。这一条不改变授权；来源色彩空间、位深与实际处理写进结果，判据见 README 与 --capabilities 的 color 段

窗口内部裁剪与等比缩小（对交付的整窗图像按图像自己的像素坐标再裁一次；不是桌面绝对坐标；顺序是先裁后缩，--roi 与 --client-area 两条互斥）
  --roi <x,y,w,h>                             从交付的整窗图像里裁出 x,y 起点、w×h 大小的一块。原点 (0,0) 是这张图像自己的左上角像素（图像对应的是用户看到的那圈可见边框，DWM 那圈透明 resize 边框不在里面），单位是物理像素且不按 DPI 缩放（本进程 per-monitor v2，要按逻辑像素指定就自己乘那道缩放），所以这四个数永远不会被当成桌面绝对坐标。四个数只认十进制、逗号分隔；x 与 y 可为 0，w 与 h 至少 1，都不超过 16384。放不下就整张不落地：取帧之前就看得出放不下报 match.roi_out_of_range（不弹框、不写文件），取到帧之后才发现报 capture.roi_invalid —— 不往里挪、不裁到边上为止、也不退回整窗交出。裁剪排在取帧之后，所以它不改变授权：会从屏幕上取样的那几条照样一定问人，--yes 不会因为"最后只留一小块"而生效。结果里 cropRect 是图像像素坐标，cropScreenRect 是同一块矩形的屏幕坐标（核实得出图像原点时才写），裁前尺寸在 fullWidth/fullHeight、裁后就是 width/height。与 --client-area 互斥，配整块屏幕的目标说不通（capture.unsupported）
  --client-area                               只交回窗口客户区那一块：在交付的整窗图像里再去掉标题栏与三边边框。这块矩形照目标此刻的几何量出来（GetClientRect 加 ClientToScreen），坐标系与单位跟 --roi 完全同一套。量不出客户区报 capture.roi_unmeasurable，有一边落在交付图像之外（挂在屏外、或中途改了尺寸）报 capture.roi_invalid，两种都不退回整窗交出。与 --roi 互斥
  --scale <key=N>                             把交付的这张图等比缩小到天花板之内：max-width=N 限宽、max-height=N 限高、max-pixels=N 限总像素数（N 都是十进制；边长 1..16384，像素数 1..268435456）。三条互相独立，可只给一条、也可一条里用逗号串几条（如 max-width=1920,max-pixels=2073600）；这一条写多次时每条天花板各记各的，重复给同一条时最后一个生效。三条都按同一个比例缩（取最紧的那一条），宽高各自向下取整；默认不放大，图本来就在天花板之内就原样交付（结果里 scaleApplied=false）。插值策略只有一个，而且是可预测的整数映射：最近邻。顺序是先裁（--roi / --client-area）后缩、再编码，所以缩的是裁完的那一块；结果里 scaleFromWidth/scaleFromHeight 是缩之前的尺寸、width/height 是缩之后的，scaleMethod 是插值策略，映射按这个顺序闭合。这一条不改变授权与帧上限：会读桌面像素的路径照样一定弹框问人（--yes 不会因为最后交的是张小图而生效），一帧大到过不了帧形状检查的缩不回来，--roi 的越界判据也仍按原图判

截图授权（真实截图默认都要先弹框问一次；--yes 只免掉只取窗口画面的那条路径）
  --yes, -y                                   跳过"只取所选窗口画面"那条路径的确认框。不保证目标一定有画面，也不忽略权限、受保护内容、错误或覆盖保护；任何会从屏幕上取样的路径（bitblt、duplication、整屏任何通道、dwm 的屏幕退路）一定会弹框，这个开关跳不过。写 --yes=false 表示明确要问

执行期限（自动处理那一段的总预算；人工确认另算，到点按拒绝处理）
  --timeout-ms <ms>                           自动处理阶段的总预算（毫秒）：从选定目标开始，条件求值、后端重试、取帧、编码、写文件共用这一份剩余时间，任何一步都不会重新领一份完整预算。不给或写 0 = 不设总预算，此时每次隔离调用仍受内置上限（5000 毫秒）约束。人工确认的等待不算在这里，见 --consent-timeout-ms。预算用尽时，尚未开始的阶段被拒绝、那一张不落地（已经提交完成的文件不会被回滚），按阶段报 match.timeout / capture.timeout / io.timeout
  --consent-timeout-ms <ms>                   确认框最多等多久（毫秒）。不给或写 0 = 一直等人回答。到点按"拒绝"处理，绝不按"默认同意"处理，报 capture.consent_timeout。这段等待单独计时，不占 --timeout-ms 那份自动预算；点"是"之后那约 1 秒的关闭动画缓冲也算在这一级，不会为了赶期限而省掉

输出
  --out, -o <path|->                          输出路径；特殊值 - 表示把图片字节写到标准输出。也可用位置参数；完全不给时等同 --out -。整批输出名在取帧之前一次算好，两个目标算出同一个名字时整批报错，不会静默覆盖。标准输出一次只能交付一张图，命中多个目标时整批报参数错误、一张都不截
  --format, -f <name>                         强制编码格式；不给则由输出文件扩展名判定，扩展名也判不出时用 png
  --quality <1-100>                           JPEG 质量，十进制 1..100，默认 100
  --no-overwrite                              目标已存在时不覆盖，报错退出（不给取值就是禁止覆盖）；写 --no-overwrite=false（0 / no / n / off）取消这条禁令，=true / 1 / yes / y / on 与不给取值同义。重复给出时最后一个生效

查询（只读：不截图、不弹框、不写文件）
  --capabilities                              输出本机能力报告（JSON）：版本、系统与会话条件、各条取图路线的 available / unavailable / unverified、格式与 --yes 的适用范围。只读：不截图、不弹确认框、不写文件，也不靠实际截屏来探测能力。available 只说明"这个构建里有这条路线，且这次问出来的环境判据没挡掉它"，不保证某个窗口一定截得到。只接受 --lang / -v / -q，与任何截图选项或输出路径同时给出 = cli.query_conflict + 退出码 1，一张都不截
  --diagnostics                               输出诊断与版本报告（JSON）：构建版本、可核对的构建标识（PE 链接时间戳 + 架构 + 映像大小）、平台与后端状态，字段与 --capabilities 同源，不另立第二份环境信息。默认不上传、不采集画面、不枚举用户文件，也不输出用户名、环境变量与任何路径；--verbose 追加每一问的原始答案，便于核对后再提交。互斥规则与 --capabilities 相同
  --screens                                   只读地列出本机每块屏：工具编号、设备名、是否主屏、物理矩形、DPI 与旋转（问得到的话）、归属适配器关联，并标出这几种身份各稳到哪一层。不取一个像素、不弹框、不写文件、也不改任何显示设置。交回的 device:<设备名> 与 id:<监视器设备路径> 可以直接写进 --monitor。与截图那一级的选项互斥（cli.query_conflict + 退出码 1）；真去截整屏仍然一定弹框，--yes 跳不过
  --list [<all>]                              只读地把满足全部条件的顶层窗口列成 JSON（句柄 / PID / 类名 / 标题 / 映像名 / 矩形 / Z 序 / 身份约束字段），不截图、不弹框、不写文件，也不需要输出路径。多匹配按 --offset / --limit 分页而不报截图歧义，配选择策略算冲突。取值 all = 也列最小化窗口。结果会过期，截图时仍要复核身份（详见 README）
  --inspect [<path>]                          只读地检查同一套选择策略定出的那一扇窗口；多匹配报 match.ambiguous_window + 退出码 5，不替你选一个。取值 path = 也写出归属映像的完整路径（默认只写文件名）。读不到的项写字段级 denied / failed，不建议改用管理员身份；不恢复或激活任何窗口
  --offset <n>                                窗口查询跳过开头 n 个（0 起）
  --limit <n>                                 窗口查询本批最多 n 个（默认 50）

其它
  --dry-run, -d                               只解析并列出候选窗口，不截图不写文件
  --json, -j                                  已废弃的兼容开关，无副作用：成功与错误本来就输出 JSON
  --verbose, -v                               JSON 中追加 input 段（规范化后的全部输入），并保留 notes
  --quiet, -q                                 省略 notes；errors 无论如何都会返回；与 --verbose 同时给出时按 --verbose 处理
  --lang, -l <language>                       文案语言。auto(默认，跟随系统显示语言) / zh-CN / zh-TW / en / ja；系统语言不受支持时用 en
  --help, -h                                  输出文本帮助（本段）
  --version                                   输出版本与阶段

写法: --opt=value / -opt / /opt 都接受；取值本身以 - 开头时写成 --title=-x，或用 -- 结束选项解析。数字取值只认十进制（--hwnd 另可按 0x 写十六进制）
输出: 成功与错误都是 JSON，只含 captured / images（另有 errors / notes，--verbose 才有 input）
      --help / --version 以及不给条件时是文本
退出码: 0 成功 / 1 参数错 / 2 未给条件 / 3 --help / 4 无匹配窗口 / 5 匹配多个窗口 /
        6 目标受保护或被拒绝 / 7 截图失败 / 8 写文件失败 / 9 内部异常
当前构建: --capture 的取值全部已实现（wgc / dwm / printwindow / bitblt / duplication，auto 按 wgc-dwm-printwindow-bitblt 回退，整屏目标按 wgc-duplication-bitblt）；输出目录必须已存在
运行环境: 64 位 Windows，声明的最低内部版本 18362（Windows 10 版本 1903），只在内部版本 19045 上实测过；本机版本提供不了的路线在取帧、弹框之前就报 env.os_too_old / env.channel_unsupported（前者换通道也没用），--verbose 的 input.osBuild 与 input.captureChain 回显这一次能走哪几条

示例:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 报告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
  ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png
  ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --consent-timeout-ms 60000 D:\shots\epad.png
  ECAPTURE.EXE --capabilities  先只读问一次这台机器能走哪几条路线，再决定 --capture 与目标条件
```

<!-- END ECAPTURE-HELP -->

## 参数写法

每个数字选项只认它对外承诺过的那一种写法，解析器不再自己猜进制。

- `--pid`、`--index`、`--monitor <n>`、`--quality` 与两条期限**只认十进制**（`[0-9]+`）：不要正负号、
  不要空白、不要小数点、不要指数记法（`1e3`）、不要下划线分隔、不要 `0x` 前缀，也不接受非 ASCII
  数字；区间在同一次解析里判完（`--pid` 1..4294967295，`--index` 与 `--monitor` 1..65535，
  `--quality` 1..100，期限 0..86400000）。不合就是 `cli.invalid_number` + 退出码 1——取值不会被强转、
  回绕，也不会按另一种进制重读（`--pid 1e3` 不会悄悄变成 483，`--hwnd -1` 不会变成 `UINT64_MAX`）。
- `--hwnd` 保留文档里的三种写法：纯数字按十进制、`0x`/`0X` 前缀按十六进制、裸写含 `a-f` 按十六进制
  （Spy++ 那种形式，所以 `--hwnd 1e3` 就是 `0x1e3`）。正负号、空白、超过 64 位与句柄 `0` 一律拒绝。
  下划线只在十六进制写法里合法，而且必须夹在两位十六进制数字之间：`0x001A_0B4C` 可以，`0x_1A`、
  `1A__0B4C`、`1A0B4C_`、`12_34` 都不行。
- `--monitor` 的取值可以省略，所以"下一个参数算不算它的取值"用的就是上面这套语法：`--monitor out.png`
  仍是"主屏 + 输出到 out.png"，而 `--monitor 1e3` 是一个写坏了的屏幕编号，会报错而不会被改当成输出文件名。
- 紧跟在"要吃值的选项"后面的那一条就是它的取值，哪怕它长得像另一个选项：`--title --lang ja` 找的是标题
  `--lang`。要以 `-` 开头写取值请用 `--title=-x`，或者用 `--` 结束选项解析（`--` 之后的参数一律按位置
  参数处理，`--` 本身丢弃）。
- 重复给同一个选项：匹配条件类是 OR（`--title A --title B`），取值类以最后一个为准（`--timeout-ms 9000
--timeout-ms 300` 是 300），`--lang` 也一样——`auto`（或省略取值）是**明确回到系统显示语言**，不是保留
  上一条。取值非法的 `--lang` 报 `cli.unknown_language`，并按已经定下来的那种语言写这条报错。
- `--verbose` 与 `--quiet` 同时给出时按 `--verbose` 处理：notes 照常交付，并另发一条
  `note.flag_overrides_quiet` 说明原因。`errors`、以及 `images[].source` / `path` / `scope` 这些来路
  字段任何时候都不被 `--quiet` 隐藏。

## 匹配语义

不同选项之间是 AND（都满足才算命中同一个窗口），同一选项写多次是 OR，不会跨窗口拼接条件。

```powershell
ECAPTURE.EXE --process notepad.exe --title-contains 报告 D:\shots\r.png
# 进程是 notepad.exe 且标题含"报告"的那些窗口
```

- `--title` 是整串相等、`--title-contains` 是子串匹配，两者**区分大小写**；`--class` / `--process` / `--exe` 忽略大小写。
- **问不出来的条件永远不算命中。** 系统不肯交出标题或进程信息（`denied` / `failed`）的那扇窗口，就是不满足需要这项
  信息的条件；`--title-regex` 求值失败时，已经收集到的全部命中一起丢弃，交回的是这条失败，而不是一份只求值了一半的
  候选列表。正则是**在匹配时才编译并运行**（不是在解析时），跑在隔离的辅助进程里，它失败的三种方式各自分开：
  - **语法错误**（编译不过的模式）→ `cli.invalid_regex`，退出码 `1`，`stage=match`；
  - **复杂度 / 资源上限**（像 `(a+)+$` 这样的模式在长标题上撞到 MSVC 回溯的 `error_complexity`）→ 同一个
    `cli.invalid_regex` 码、同样退出码 `1`，但这是一句"太复杂"的消息，其 hint 直说**加大 `--timeout-ms` 没有用**
    ——这是一次有界的资源停止，不是"慢但还能用"的答案；
  - **预算耗尽**（求值花光了 `--timeout-ms`）→ `match.timeout`，退出码 `7`；而跑它的那个辅助进程压根没回来时是
    `capture.worker_failed`，退出码 `7`。
- 枚举默认跳过不可见窗口和零尺寸窗口；**最小化的窗口截不到**，只在 `hint` 里单独说明。
- 命中多个又没给消歧选项时不会随便挑一个，而是报 `match.ambiguous_window`（退出码 5），
  `hint` 里按叠放次序列出全部候选。
- 候选列表按**当下的叠放次序**（Z 序）排列，最上面的在前；`--topmost-match` / `--bottommost-match` 取这个顺序里的第一个 / 最后一个。
  旧名字 `--newest` / `--oldest` 保留为兼容别名，行为完全相同 —— 它们选的一直是 Z 序位置而不是创建时间：Windows 没有取窗口创建时间
  的公开 API，进程启动时间也不是窗口创建时间。写旧名字只多发一条 `note.deprecated_option`；同一条策略的新旧两种写法一起给
  （`--newest --topmost-match`）仍然算一条策略，不会被判成互斥。

## 输出形式

`--help`、`--version`、以及不给任何条件时是纯文本。其余一律 JSON，只装捕获结果与错误。

只读查询那三条是**另外三份契约**（`--capabilities` / `--diagnostics` / `--screens`，见[系统支持](#系统支持)）：
它们装的是这台机器的环境与能力，而不是某次截图的结果，所以只有它们带 `contract` 与 `contractVersion`。
这一条**不**反过来说明截图 JSON 该加顶层元信息——那份照旧只有 `captured` / `images`（按需再加
`errors` / `notes` / `input`），也不因为查询里出现了 `program.version` 就多写一份。

窗口图（真实输出的形状，数值来自一次实际截取）：

```json
{
  "captured": 1,
  "images": [
    {
      "file": "D:\\shots\\EvernightCapture - 文件资源管理器.png",
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
      "title": "D:\\share\\EvernightCapture - 文件资源管理器",
      "class": "CabinetWClass",
      "image": "explorer.exe",
      "elapsedMs": 156
    }
  ]
}
```

屏幕图（`--monitor` 且没有窗口条件时）没有窗口可归属，换成 `monitor` / `device` / `primary` 三个字段，
`hwnd` / `pid` / `title` / `class` / `image` 整个不出现——调用方按 `monitor` 是否存在区分两种图。
写出 `--cursor` 时，每张图另外带 `cursorRequested` / `cursorEffective` / `cursorBasis`——要的是哪一种、
这条路径实际交回的是哪一种、这个结论凭什么。没写这条选项时三个键一个都不出现，这正是"默认不要求"
与这条选项存在之前逐字节相同的那一条保证，判据见
《画面里的鼠标指针》。

两种图都带 `source`，写的是真正出图的那条通道（`--capture auto` 回退成功时它是链上命中的那一条，不是 `auto`）；
也都带 `path` / `scope` / `rect`——整屏那种是 `screen.wgc` / `screen.bitblt` / `screen.duplication`，
`scope` 为 `desktop`；而 `--monitor` 配窗口条件出的仍是窗口图，`scope` 为 `window`。

从屏幕上读像素、再从整幅桌面帧里把目标裁出来的那几条通道（`duplication`，以及 `bitblt` / `dwm` 的屏幕路径）
还会报告这些像素**究竟取自桌面的哪里**：`requestedRect` 是这条通道本打算截取的屏幕区域，`capturedRect` 是它
实际截到的区域——两者都用虚拟屏幕坐标，所以和 `rect`、和确认框上列出的区域在同一套坐标里对得上。`clipped`
只在两者不相同时才出现（窗口横跨两块屏、或者挂出屏幕边缘）：图照原样交付，只是不是完整的目标，
`note.capture_clipped` 会说明四边各少了多少。`rotation` 只在桌面帧需要被转过角度（顺时针 90 / 180 / 270 度）
才能和该显示器实际显示的方向对齐时才出现；没有 `rotation` 就是没有做过任何旋转。窗口内容通道
（`wgc`、`printwindow`、`dwm.thumbnail`）天生截的就是完整的窗口，所以这几个键一个都不写——键缺席的含义是
"一个像素都没漏"，不是"没查"。

出错（`--hwnd` 写了非法值）：

```json
{
  "captured": 0,
  "images": [],
  "errors": [
    {
      "code": "cli.invalid_number",
      "message": "--hwnd 需要有效的句柄值（十进制，或带 0x 前缀的十六进制）",
      "option": "--hwnd",
      "value": "zzz",
      "hint": "纯数字按十进制解析；十六进制写成 0x……，含 a-f 时按十六进制。不要写正负号与空白；下划线只能夹在两位十六进制数字之间（0x_1A、1A__2B 都不收）"
    }
  ]
}
```

规则：

1. `captured` 与 `images` 恒在（空时 `[]`）；`errors` 只要非空就必须出现（`--quiet` 也抑制不掉）；
   `notes` 仅非空且未 `--quiet` 时出现；`input` 仅 `--verbose` 时出现。调用方先看 `errors` 再读 `images`。
2. 诊断项里为空的字段整个键省略，不会输出 `null` 占位。描述这一帧来路的三个字段是唯一的例外，`--quiet`
   也抑制不掉：每张图都带 `path`（实际走的那条内部路径——`wgc`、`printwindow`、`dwm.thumbnail`、
   `dwm.screen`、`bitblt.screen`、`duplication.frame`、`screen.wgc` 等）、`scope`（按 `path` 判出的
   `window` 或 `desktop`）与 `rect`（那条路径被授权取样的屏幕区域；量不出来时才省略）。会读屏幕像素的
   那几条通道另外保留 `requestedRect` / `capturedRect` / `clipped` / `rotation`（见上文）——这些同样是
   定位判据，`--quiet` 也不许把它们藏起来。
3. `code` 值稳定：`cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`，只增不改名。
   取帧失败里"帧超时"（`capture.frame_timeout`）与"窗口已经没了"（`capture.window_gone`）各有自己的码，
   不再和一般的 `capture.failed` 混在一起——两者的下一步动作不同（前者可以等一会儿重试，后者要重新枚举）。
   帧自己的内存形状说不通（宽高为 0、单边超过 16384 像素、行距装不下一行像素、缓冲区比行距×高还短）时给
   `capture.frame_invalid`（退出码 7）；裁剪、行重排与编码都先核这一道，坏帧不会被往下搬。
   "那台显示器已经不在桌面上 / 它的画面在确认之后变了"是 `capture.monitor_changed`（退出码 7）：下一步是
   重新枚举显示器并重新确认，而不是换一条通道碰运气——另一块屏的画面是谁都没有批准过的。
   `note.capture_clipped` 是和 `note.frame_uniform` 同类的质量提示：图片照常交付，退出码不变。
4. 通道：默认全部写 stdout、stderr 保持空；一旦图片占用标准输出（显式 `--out -`，或根本没给输出路径），
   JSON 整体改走 stderr，两个通道永不混流。连渲染结果本身都出异常时的兜底诊断也一律走 stderr（那时
   无法确定图片是否已经占了 stdout）。**结果送不到约定那条流就是失败**：退出码变成 `8`，即使另一条流
   写成功也不改回原来的值——调用方按约定流读，读不到就是没拿到。至于某种 shell 能不能把那些图片字节
   原样带出去，是另一件事，见[标准输出图片字节的 shell 差别](#标准输出图片字节的-shell-差别)。
5. `captured` 等于 `images` 条数；一个窗口一张图，`--monitor all` 则一块屏一张图。
   **标准输出一次只能交付一张图**：命中多个目标（`--all` 或多个屏幕）又要写 stdout 时，整批在弹确认框和
   取第一帧之前就被拒（`cli.stdout_multiple_targets` + 退出码 1），一张都不截、一个文件都不写。判据是
   实际命中的目标数，所以 `--all` 只命中一个窗口时照样可以写 stdout。多张 PNG 首尾拼在同一条流上不是一幅
   可解码的图像，工具也不会把 `-` 当文件名前缀算出 `-_1.png` 那种本地文件。
6. `images[].source` 与错误里的 `backend` 写的都是**真实那条通道**：`--capture auto` 回退成功时 `source`
   是链上那一条而不是 `auto`；回退链全失败时 `backend` 列出实际试过的几条。`images[].path` 比它更细：一个通道
   可能含好几条路径，授权按实际走的那条判而不按通道名判——`dwm.thumbnail` 取的是窗口自己的画面，
   `dwm.screen`（同一通道的屏幕退路）取的是屏幕。那条退路只在缩略图这一步真的失败时才走，**不会因为
   画面正好是单色就走** —— 纯色窗口照样是窗口的画面。
7. **保存**：整批最终输出路径在取第一帧之前（也在任何确认框之前）一次算好。两个目标算出同一个名字时报
   `io.output_collision`（退出码 8，`stage=plan`），整批一张都不截、一个文件都不写 —— 既不替调用方改名，也不让第二张盖掉第一张。
   每张图先写目标目录下唯一的临时文件，写全并刷新之后才改名成目标名，所以写失败不会清空也不会删掉旧文件。
   `--no-overwrite` 时"目标在不在"由那一次不许替换的改名当场判定（`io.file_exists`），不做有竞态的预检。
   **格式与文件名**：`--format` 取 `png` / `jpg` / `jpeg` / `bmp` / `tiff` / `gif`，且优先于扩展名；不给 `--format`
   时由扩展名决定（`.tif` 与 `.tiff` 都算 TIFF），扩展名也认不出时退回 png。名字**没有**扩展名时补上该编码自己的
   扩展名（`.png` / `.jpg` / `.bmp` / `.tif` / `.gif`），并且每批发一条 `note.output_extension_appended`；已经有
   扩展名的名字绝不会被改写，所以 `--out shot.png --format jpeg` 就是把 JPEG 字节原样写进 `shot.png`。
   `webp` 与 `ico` 不接受（`cli.invalid_format`）：本 SDK 没有它们的编码器，`--capabilities` 于是把它们报成
   `compiled: false`，而不是让调用方自己猜。
8. 每一步的失败诊断还带着它自己的坐标，只在这一步真拿到了值时才出现：`target`（哪个目标，窗口是
   `0x…` 句柄、屏幕是 `DISPLAY1` 这样的设备名）、`backend`（哪条通道）、`stage`（`parse` / `match` / `plan` /
   `consent` / `capture` / `encode` / `write` / `stdout` / `report` 九取其一，和 code 一样只追加不改名：`match.*`
   那组写 `match`，整批输出名的判据 `io.output_collision` 与 `cli.stdout_multiple_targets` 写 `plan`，帧在编码器里
   出的错写 `encode`；命令行解析期的错误**根本不带** `stage`）、`hresult`（`0x80070005` 这样的原值）、
   `win32`（`GetLastError` 的原值）。`message` 随 `--lang`
   变，这几个不变。授权这一关的错自成一类：`capture.access_denied` 是回答不是"是"（这个"是/否"框上，人能给出的唯一拒绝就是答"否"，而工具把任何非"是"的结果都当成拒绝），
   `capture.consent_unavailable` 是这台机器根本没有可交互的桌面、框弹不出来——两者都是退出码 `6`，都带
   `stage=consent`、`target`、`backend`（通道）和 `value`（那条路径），也都不是技术性的访问被拒
   （`capture.failed` 带 `hresult=0x80070005`），这样才分得开。确认之后目标挪了位置或变了大小给
   `capture.consent_stale`，`stage=capture`、退出码 `7`，重新选目标再截就会再问一次。
   黑帧不会被断言成 DRM——文案只列出几种可能。
   单色帧也不会被断言成"没截到"：图照常交付，另外留一条 `note.frame_uniform`（把那个颜色、那条通道、
   那个目标写清楚）。只有 `duplication` 还会因为单色拒绝一帧，而且必须两条一起成立——这一帧没有任何
   present 记录，且整幅只有一个颜色。

### 标准输出图片字节的 shell 差别

`--out -` 要的是同时两条流：stdout 走 PNG/JPEG 字节，stderr 走整份 JSON。工具自己一条都不做转换——
图片字节是用 `WriteFile` 写在裸句柄上的，文字从不进入那条流——所以图片是在工具的下游、也就是 shell 的重定向里
被弄坏的。这两条流也**不是同一种数据**：JSON 是文本，shell 重新编码一次并不影响它；图片是字节流，
被替换掉一个字节文件就废了。因此第一个要问的问题就是某种 shell 保不保得住原生命令的字节流（一份解不开的文件
也可能另有原因——写入被截断、管道只开了一半——所以先确认确实是重定向的问题，别一上来就怪到它头上）：

| Shell                  | 把原生命令的 stdout 重定向到文件                                                                                                                                                                          | 怎么办                                                                                                                                    |
| ---------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `cmd.exe`              | 逐字节无损（`1>` / `2>` 就是重接真正的文件句柄）                                                                                                                                                          | 照写就行                                                                                                                                  |
| PowerShell 7.4 起      | 逐字节无损——7.4 把重定向操作符改成保留原生命令 stdout 的字节流（官方文档写明的改动）                                                                                                                      | 照写就行                                                                                                                                  |
| PowerShell 7.0 – 7.3   | stdout 仍按文本解码后进管道                                                                                                                                                                               | 用 `cmd /c`，或 `--out <文件>`                                                                                                            |
| Windows PowerShell 5.1 | **会弄坏**：字节先按文本解码、再以 UTF-16LE 写出，所以 NUL 与所有 ≥ 0x80 的字节在落文件之前就已经丢了；连 `2>` 出来的 stderr 文件也要套上它自己的错误记录格式（`node : ` 那样的前缀、脚本位置、一个 BOM） | 用 `--out <文件>`，或 `cmd /c`，或 `Start-Process -RedirectStandardOutput … -RedirectStandardError …`（无损，因为句柄是系统直接接上去的） |

```cmd
:: cmd.exe：图片走 stdout、JSON 走 stderr，两条都逐字节无损
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json
```

```powershell
# 只在 PowerShell 7.4+ 用：这两行会保住字节流。
ECAPTURE.EXE --process notepad.exe --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# 在 Windows PowerShell 5.1 下同样这两行不行：原生 stdout 先按文本解码、再以 UTF-16LE 重写，
# NUL 和所有 >= 0x80 的字节在落文件之前就已经丢了。想复核这个机制不必真截图，用一段固定的、
# 不含敏感数据的字节流即可——重定向 `ECAPTURE.EXE --version` 再比对：在 cmd 下文件是原样的 ASCII
# 字节（`1>` 重接了句柄），在 5.1 下同样这份输出会变长约一倍且是 UTF-16LE，任何 PNG 解码器都不会认。
# 所以 5.1 下要么要一个文件，要么用 Start-Process（句柄由系统接上，逐字节无损）：
$p = Start-Process -FilePath 'D:\tools\ECAPTURE.EXE' `
  -ArgumentList '--process','notepad.exe','--yes','--out','-' `
  -NoNewWindow -Wait -PassThru `
  -RedirectStandardOutput 'D:\shots\snap.png' -RedirectStandardError 'D:\shots\result.json'
$p.ExitCode   # 退出码在这里读；只加 -Wait 并不会把它回报出来
```

`2>&1`（以及 `*>`）在这几种 shell 里都不行：两条流一合并，shell 就把结果当字符串数据处理，图片字节必坏。
而 5.1 把原生命令的 stderr 套成它自己的错误记录，是发生在进入管道的那一刻，所以分开写 `1>` 和 `2>` 也救不回
那份 JSON。既要图又要 JSON 就分两个文件收，或者干脆要一个文件、把 JSON 留在 stdout——那本来就是默认，也是这个
工具设计所绕着走的那条路。

## 窗口内部裁剪（`--roi` / `--client-area`）

这两条选项回答的是同一件事：**这一次交付的窗口图里要留哪一块**。它们互斥（`cli.crop_conflict` + 退出码 1），
配整块屏幕的目标说不通（`capture.unsupported` + 退出码 1 —— 屏幕上没有一扇窗口可以让坐标相对它的左上角去算），
与只读查询一起给出也算冲突（`cli.query_conflict` / `cli.window_query_conflict`）。

### 坐标系，以及为什么它永远不会是桌面坐标

原点 `(0,0)` 是**交付的整窗图像自己的左上角那个画素**，右下边不含。那张图像就是用户实际看到的那圈窗口可见边框
（`DWMWA_EXTENDED_FRAME_BOUNDS`）：`GetWindowRect` 还算在内的 DWM 透明 resize 边框不在图像里，而 `--client-area`
是在这块之上再去掉标题栏与三边边框。

单位是**物理画素，不按 DPI 缩放**。本进程声明了 per-monitor DPI v2，窗口矩形、帧尺寸与画素缓冲本来就都在物理画素
那一套系里，这里没有缩放这一步：同一条 `--roi 0,0,200,120` 在缩放一倍与两倍的屏上取的都是 200×120 个画素。
要按逻辑画素（DIP）指定的调用方自己乘那道缩放 —— 本工具不猜这扇窗口在哪块屏上，也不猜该用哪块的 DPI。

矩形锚定在图像上，所以**它绝不会被当成桌面绝对坐标重新解释**。这不是措辞上的选择：拿桌面坐标来截，等于允许
调用方去要一块谁都没批准过的画面。

`--roi` 要正好四个十进制数、逗号分隔（只认 `[0-9]+`：不要正负号、不要空白、不要小数点、不要指数、不要下划线、
不要 `0x`，也不认非 ASCII 数字）。`x` 与 `y` 可以是 `0`；`w` 与 `h` 至少 `1`；四条都不超过 `16384` —— 那与帧的
单边上限是同一条线，`--capabilities` 把它报成 `limits.roiMaxValue`，所以帮助、解析与查询读的是同一个数。

### 放不下就是拒绝，不替你「修好」

| 情况                                                                                       | 码                         | 退出码 | `stage` |
| ------------------------------------------------------------------------------------------ | -------------------------- | ------ | ------- |
| 四段写法不合（正负号、空白、段数不对、零宽或零高、超过上限）                               | `cli.invalid_value`        | 1      | parse   |
| 选定那一刻窗口就装不下这条矩形，于是弹框之前、输出名规划之前                               | `match.roi_out_of_range`   | 1      | match   |
| 帧交回来才发现装不下（目标在这中间改了尺寸，或有一部分挂在屏幕之外）                       | `capture.roi_invalid`      | 7      | capture |
| 定位这块矩形所需要的那一问没有答案（客户区量不出来，或这块图像核实不出它对应屏幕上哪一块） | `capture.roi_unmeasurable` | 7      | capture |

这四种都没有「往里挪一挪」「裁到边上为止」「那就整窗交出」这种下一步 —— 最后那一种等于交出一张调用方没要求的图。
也都不落地：取帧之前那一道排在确认框之前，所以一条注定裁不出来的请求不会先去打扰人；取到帧之后那一道把帧丢掉。
一批就是一批：命中的几扇窗口里只要有一扇装不下，整批一张都不截（与 `match.index_out_of_range` 同一条规矩）。
`--dry-run` 不取帧，所以它也不判裁剪几何 —— 无论判不判，`-v` 都会把请求回显在 `input.crop` 里。

### 裁剪不改变人批准过的那一片

裁剪排在取帧**之后**，这正是它不能用来碰到从没摆上桌面的画素的理由：该走哪一级判据仍然只由 `images[].path` 决定，
所以 `--roi 0,0,8,8` 配一条桌面取样路径问的是一次整屏截图同样的问题，`--yes` 管着的照旧只有窗口内容那三条
（见[截图授权与 --yes](#截图授权与---yes)）。

### 结果里写了什么

一张裁过的图多带一组字段，它们都是定位判据，`--quiet` 不许藏：

```json
"width": 120, "height": 80,
"cropMode": "roi",
"cropRect":       { "x": 20, "y": 40, "width": 120, "height": 80 },
"fullWidth": 486, "fullHeight": 293,
"cropScreenRect": { "x": 227, "y": 240, "width": 120, "height": 80 }
```

`cropRect` 是图像画素坐标，`width` / `height` 是裁完的最终尺寸，`fullWidth` / `fullHeight` 是裁之前那张整窗图像的
尺寸，`cropScreenRect` 是同一块矩形在**虚拟屏幕坐标**里的那一块 —— 与 `rect`、`requestedRect`、以及确认框上列出的
区域在同一套系里。这条映射是闭合的：`cropScreenRect − cropRect` 就是这块图像自己的屏幕原点，调用方可以拿 `rect`
去核对它，而不是必须相信它。

那一行**只在图像原点核实得出来时才写**：要么这条通道自己报了它实际截到的那一块（`capturedRect`，桌面裁切那几条
就是这么做的），要么那一刻量到的可见窗口矩形与交付尺寸完全相同。核实不出来就整个键不出现，并留一条
`note.crop_mapping_unavailable` 说清楚 —— 问不出来的东西不会被折成一个看起来合理的数。
`--client-area` 本来就要靠这条映射才知道客户区在图里落在哪儿，所以映射问不出来时它直接失败
（`capture.roi_unmeasurable`），而不是交回一张没裁的整窗图。

这几个字段与 `requestedRect` / `capturedRect` / `clipped` / `rotation` 并存而不重复：前一组说的是**整扇窗口**在
桌面上有没有被完整截到，后一组说的是截回来的那张图里要交出哪一块。

### 本机判不了的两条

`.\tests\crop.ps1` 用三条自己独立问出来的 Win32 事实
（`GetWindowRect`、`DWMWA_EXTENDED_FRAME_BOUNDS`、`GetClientRect` + `ClientToScreen`）对照几何，用「裁剪图的四个角
必须等于整窗图对应偏移处的画素」对照内容，用真实改过尺寸的窗口判失效，用桌面通道 + 极小 `--roi` 判「一定弹框、
没点头就不落地」（测试一侧只探测有没有弹出确认框，从不代答）。两条本机造不出、照实记未验证而不是推导：
同一条 `--roi` 在两块缩放比不同的屏上（这台机器只接了一块屏），以及目标在「取帧之前的预检通过之后、帧交回来之前」
这一瞬间被改小（要的就是这段竞态本身，时间点安排不出来 —— 它由离线层 `build\ecapture-crop-tests.exe` 逐条判）。

## 等比缩小（`--scale`）

`--scale max-width=N,max-height=N,max-pixels=N` 把工具即将交付的那张图缩到这几条天花板之内。三个键互相独立（可以只给一个，
也可以一条里用逗号串几个；这一条写多次时每条天花板各记各的，重复给同一条时最后一个生效）；`--scale` 什么都不写是参数错误，
因为“一条天花板都没给”与“缩到 0”没法区分。N 只认十进制，边长 1..16384（与帧的单边上限同一条线），像素数 1..268435456。

规矩很短，而它们就是全部契约：

- **一个比例，取最紧的那一条天花板。** 每条天花板各提出一个比例（宽、高，像素预算则开方），取最小的那一个。宽高各自**向下取整**，
  且各至少留 1 像素。
- **绝不放大。** 本来就在每条天花板之内的图原样交付，结果里写 `scaleApplied: false`。没有哪条天花板真的比图更紧时，
  这一条什么都不会“减少”。
- **插值策略只有一种，而且可预测**：最近邻（`scaleMethod` 恒为 `nearest`）。交付像素 `(x,y)` 取自缩之前那张图的
  `(floor(x*scaleFromWidth/width), floor(y*scaleFromHeight/height))`。没有浮点采样，也不按通道挑算法。
- **先裁、后缩、再编码。** `scaleFromWidth` / `scaleFromHeight` 是**裁之后**那张图的尺寸（不是整窗图），`cropRect` /
  `cropScreenRect` 一字不改，`width` / `height` 是最终尺寸。单色质量提示判的是交付出去那张（也就是缩过的）。
- **授权那一层一点不动。** 缩放排在授权之后，它不是降低请求风险等级的办法：判据仍然只由 `images[].path` 决定，
  所以桌面像素那条配 `--scale max-width=8`，即使给了 `--yes` 也照样弹框（见[截图授权与 --yes](#截图授权与---yes)）。
  它同样不是绕开既有上限的办法——一帧大到过不了帧形状检查的
  根本到不了这一步，`--roi` 仍按未缩的那张图判。
- **只有写过这一条时**，`scaleMethod` / `scaleApplied` / `scaleFromWidth` / `scaleFromHeight` 才出现；没写 `--scale` 时这四个键
  一个都没有（不是 `null`、`0` 或 `false`），所以那条流与之前逐字节相同。
- 编码那一层没动：`.\tests\scale.ps1` 在真机上判的是 `png`、`bmp`、`jpeg` 三种格式写下的都是缩过之后的尺寸。
  `tiff` 与 `gif` 用的是同一个编码调用、不同的 encoder id，不在这条判据覆盖的范围里。

本机未验证：单边超过 16384 的帧的真机现场（造不出一扇那么大的窗口，而且那种帧本来也过不了帧形状检查——这条由离线层判）、
HDR 与缩放同时生效（这台开发机开不了 HDR）、跨屏混合 DPI（只接了一块屏）。

## 不给输出路径（兼容性说明）

不给输出路径与显式写 `--out -` 是**同一个请求**：图片按 png 走 stdout，JSON 整份走 stderr，而每条诊断都是
那一步真实产生的那一条。

- 这条路上回来的就是真实的 code 与真实的退出码，原样不动：`match.no_window`（4）、`match.ambiguous_window`（5）、
  `capture.access_denied`（6）、`capture.failed`（7）、`io.write_failed`（8）、`cli.invalid_number`（1）……更早的构建
  会把它们整段换成一条 `cli.missing_output` + 退出码 `1`，同时清空 `images` 与 `notes`，既藏起了原因也丢掉了已经
  交付的图；那套改写已经取消。`cli.missing_output` 不再发出，但这条 code 仍留在清单里占位，为的是它不被挪作别的含义。
- 已经送到 stdout 的图仍留在 `images` 里，`captured` 照实计数，所以这条路上部分成功照样看得见。
- "本次没给输出路径"现在只是一条 `hint`，而且只在点名一个文件确实绕得开这次故障时才出现：隐式 stdout 路线上的
  `io.write_failed` + `stage=stdout`。写 `--out -` 的人本来就选定了这条管道，那句 hint 在那里不出现。

怎么分支：读 `errors[].code`（以及它的 `stage` / `target` / `backend` / `hresult` / `win32`），
既不要只看退出码，也不要根据"有没有给 `--out`"推断原因。

## 退出码

`0` 成功 / `1` 参数错 / `2` 未给条件 / `3` `--help` / `4` 无匹配窗口 / `5` 匹配多个窗口 /
`6` 目标受保护、在确认框上被答"否"、在 `--consent-timeout-ms` 内没人回答、或框根本弹不出来 /
`7` 截图失败，含 `--timeout-ms` 预算用尽，**也含这一台机器的 Windows 版本给不出所要求的东西**
（`env.os_too_old` / `env.channel_unsupported`，见[系统支持](#系统支持)） / `8` 写文件失败，含在写文件或标准输出阶段预算用尽 / `9` 内部异常。
新增语义只会追加编号。
`8` 也覆盖"结果 JSON 送不到约定那条流"（写 stdout / stderr 失败），那种情况下另一条流上补发的文字不算交付。

退出码与 body 是两套独立信号，`2`/`3`/`4`/`5` 是正常控制流而不是崩溃。**允许部分成功**：`--all` 或
`--monitor all` 里某些目标失败时，已写出的图仍在 `images` 里（`captured` 可以大于 0），但退出码是 `7`。
某个后端崩了（抛异常而不是返回失败）也只作废它所在的那一个目标：前面的图留着，这条失败以 `capture.failed`
带在 `errors` 里。内存耗尽、显卡设备被移除这类换后端也不会有区别的错误会明确终止整批，而不是一条条试下去。
访问被拒不是继续回退的理由，被人拒绝也不是：一旦有人答"否"（或这个会话根本弹不出框），本次请求剩下的目标
一律不再尝试——不换后端、不重试、不再问第二遍，之前已经完成的图全部留着。

只读查询那三条（`--capabilities` / `--diagnostics` / `--screens`）只用 `0` 与 `1` 两个编号：`0` = 这份文档出完了（哪怕里面
写着"这台机器版本太低、哪几条都不可用"——**查询成功与截图能成是两件事**，调用方按 `status` 分支而不是按退出码
猜环境）；`1` = 这条用法不合契约（`cli.query_conflict`，见[系统支持](#系统支持)）。它们不产生 `4`/`5`/`6`/`7`/`8`：
一次窗口都没枚举、一个框都没弹、一个文件都没写。

只读的窗口查询那两条（`--list` / `--inspect`）共用 `0` 与 `1`，并另外使用 `4`（`match.no_window`，只可能出自
`--inspect`，它需要一个目标）与 `5`（`match.ambiguous_window`，选择策略之后仍剩多扇），再加**只有一条来路的 `7`**：
这一次的**条件求值自己没跑完**（`match.timeout` —— `--timeout-ms` 的预算花在 `--title-regex` 的回溯或向挂起的
窗口取标题那一步，或那一步的辅助进程自己坏了）。这条 `7` 说的是"这一问没能问完"，与取帧无关，所以它的 `hint`
也是查询自己的说法，明写换 `--capture` 没有用 —— 这一路根本没有通道可换。**`6` 与 `8` 绝不出现**：一个框都没弹、
一个文件都没写，而那两条说的正是这两段事。`--list` 在一个都没命中时退出码仍是 `0` —— 空列表
就是这一次的答案。

## 系统支持

三个不同的数字不能混成一句"支持 Windows X 以上"：

| 层次                    | 取值                                                                                                 | 这个数是从哪来的                                                                                                                                                                                                                                                                                                                                     |
| ----------------------- | ---------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 各条路线的 API 历史下限 | 任何一张图 10.0.10240 · `duplication` 10.0.9200 · `printwindow` / `dwm` 10.0.9600 · `wgc` 10.0.18362 | 微软为这条路线**实际调用**的那个接口所写的下限。编码器（WinRT `BitmapEncoder`）六条通道、五种格式共用；`wgc` 走的是 `IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor`，那是 Windows 10 版本 1903 才有的互操作接口——`Windows.Graphics.Capture` 这个命名空间本身确实是 1803 出现的，但本工具没有"让用户在系统选择器里点一下"那条退路 |
| 本工具声明的下限        | 64 位 Windows 10 版本 1903（内部版本 18362）或更新                                                   | 上面那几道里最高的那一条，因为默认通道要真能截到窗口需要它；不是"某条路线碰到的最老的那个 API"                                                                                                                                                                                                                                                       |
| 已实测的版本            | Windows 10 版本 22H2（内部版本 19045），x64                                                          | `tests\` 下所有真机判据都只跑在这一台机器上。`--capabilities` 拿**本机环境与这份记录比对**，比对结果就叫 `verifiedOnThisMachine`：判的是"这台机器的内部版本与架构等于那套被测过的环境"，不是"这一台设备曾被逐台实测过"                                                                                                                               |

10240 到 18361 之间的那些版本能装载这个 exe，也能截图（工具是按每条路线各自的下限筛通道，而不是整体拒开），
但那既不在声明支持之列、也从来没有实测过，请按"预期可用、未验证"对待。

这里没有任何一句话声称支持 Windows 7 或 8 —— **这个二进制在那上面根本装载不了**。它静态导入
`api-ms-win-core-winrt-error-l1-1-1`（`RoOriginateLanguageException`，微软那份文档写的最低客户端就是
Windows 8.1），另有 `api-ms-win-core-winrt-l1-1-0` 与 `api-ms-win-core-job-l2-1-0`（Windows 8），
而 API Set 这套机制在 Windows 7 上根本不存在；这三个契约都不在 UCRT 可再分发的那一份名单里，补不到旧系统上。
`.\tests\compat.ps1` 会拿发布版二进制核对这些名字，所以"装载下限"是关于这个文件的事实，不是推测。
Windows 8.1 **能**装载也能启动 —— 在那上面起作用的正是下面那道能力检查：`Windows.Graphics.Imaging.BitmapEncoder`
在 Windows 10 之前不存在，一张图都编不出来。所以"某个 `BitBlt` 或 `DwmRegisterThumbnail` 调用在 Windows 7 上
存在"证明不了这个程序能在那上面跑；PE 头里那个 `subsystem version 6.00` 也一样，那是 MSVC 链接器默认值，
不是支持声明。

### 调用时的能力检查

在枚举窗口、规划输出名、弹确认框、读任何一个像素**之前**，工具会拿从 `ntdll!RtlGetVersion` 读到的内部版本
（绝不用 `GetVersionEx`——那个函数按应用清单与版本伪装答复）与上面那几道下限比对，并给出：

| 码                         | 什么时候                                            | 退出码 | 换通道有没有用                                                                                         |
| -------------------------- | --------------------------------------------------- | ------ | ------------------------------------------------------------------------------------------------------ |
| `env.os_too_old`           | 版本低于 10240：所有格式共用那唯一一套编码器不在    | 7      | **没有。** 这不是通道的问题，也不是目标的问题——这台机器上做不出任何一张图                              |
| `env.channel_unsupported`  | 显式指定的那条通道的下限高于本机版本                | 7      | **有**——换 `--capture` 取值或改用 `auto`。重试同一个目标没有意义，而工具不会自己把你指定的那条换成别的 |
| `note.channel_unavailable` | `auto` 链里某一条的下限高于本机版本，它已从链中去掉 | 不变   | 图仍可能由别的那几条截到，`images[].source` 写的是实际出图的那条                                       |
| `note.os_unverifiable`     | 内部版本压根没问出来                                | 不变   | 这一次没有按版本筛过任何一条——问不出来既不等于不支持，也不等于支持                                     |

`--verbose` 会回显 `input.osBuild` 与 `input.captureChain`（针对本次这类目标，这台机器实际给得出哪几条通道），
所以调用方（含 AI）不必先截图就能把能力问出来。`--dry-run` 不取帧，因此不因环境判据报错。

设备层面的能力刻意不去预测：驱动不喂桌面复制帧、这台机器拒绝 Windows.Graphics.Capture、会话里没有可交互
桌面、N 版缺媒体组件——这些都不在版本号里，也都不由本工具提前猜；那一步会交回它自己的 `capture.*` 码与
真实 HRESULT。

### 只读的能力查询（`--capabilities` / `--diagnostics`）

上面那套判据在截图**开始之前**就会起作用，但它只在真的下单一次之后才看得见。这两条命令是同一条判据的
只读出口：一次像素都不取、一个确认框都不弹、一个文件都不写、不联网、不读环境变量，也不需要窗口条件。

```powershell
ECAPTURE.EXE --capabilities              # 这台机器现在能走哪几条路线（JSON）
ECAPTURE.EXE --diagnostics               # 构建版本 + 可核对的构建标识 + 后端状态（JSON）
ECAPTURE.EXE --capabilities -v           # 再加一段 probes：每一问的原始答案与它是从哪个 API 问来的
```

三条规矩：

- **三件事分开写。** `compiled` 说的是这个二进制里有没有实现那条路线；`status` 说的是本机自己那份证据
  （版本下限 + 屏幕拓扑）此刻让不让走；`verifiedOnThisMachine` 是一次**环境比对，不是一台设备的实测记录**：
  只有本机内部版本与架构都等于本项目跑真机判据的那套环境（内部版本 19045、x64）时才写 `yes`，任何一半问不出来
  就写 `unknown`，其余一律 `no`——所以一台 26100 的机器读到的是 `no`，并因此多出一条
  `this_environment_not_tested` caveat。`os.matchesTestedEnvironment` 就是同一条判据。三者互不冒充：
  `available` + `verifiedOnThisMachine: no` 的意思是"这个构建在本机走得通这条路线，而本项目只在另一种版本上证过
  它"；而 `yes` 也照样不保证某一扇窗口一定截得到。
- **问不出来就说问不出来。** 每一条事实都是 `yes` / `no` / `unknown` 三值之一，`unknown` 既不折成"能"
  也不折成"不能"，也不整个键消失。版本没问出来时所有 `status` 都是 `unverified`，而 `autoChainWindow`
  仍然原样列出全部四条——不筛就是没筛，不是"都支持"。
- **`available` 不是保证。** 它不含"某个窗口这一次一定截得到"这层意思：驱动、受保护内容、HDR 模式都不在
  这层的断言里。文档末尾的 `caveats` 数组就是把"这份报告没说过什么"逐条列出来。

| 段                                    | 内容                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `contract` / `contractVersion`        | 只有这两份文档带契约版本（现在是 1）。普通截图 JSON 仍然按《输出形式》保持精简，不因此多出任何顶层元信息                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `program`                             | 名称、`ECAPTURE.EXE` 这个文件名本身（不含目录）、版本、架构、`buildId`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `os`                                  | 本机内部版本（`known` 为假时那一组数字写成 `unknown`）、`declaredMinBuild`（对外声明的下限）、`encoderMinBuild`、`testedMinBuild` + `testedArch`（本项目实测过的那一台）、`matchesTestedEnvironment`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| `session`                             | 是否接在控制台会话上、是否远程桌面、有没有可用的屏幕拓扑与有几块屏、本进程是否被提升过、`consentDialogExpected`（推出来的，`consentDialogProbed: false` 明说没真去弹框）                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| `authorization`                       | `yesSkips: "window-content"`、`desktopPixelsAlwaysAsk: true`、未登记路径按 `desktop` 处理，外加整份内部路径登记表（每条带 `scope` 与 `consentWithoutYes` / `consentWithYes`）——就是《截图授权与 --yes》那张表的机器可读版本                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `backends`                            | 每条路线：`compiled` / `status` / `reason` / `minBuild` / `verifiedOnThisMachine`，以及它在窗口目标与屏幕目标上各走哪条内部路径（`dwm` 那条屏幕退路也在，所以 `--yes` 的适用范围不会被人读大）                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `formats`                             | 每种格式：`compiled` / `status` / `reason` / `minBuild` / `registered`，`png` / `jpeg` / `bmp` / `tiff` / `gif` 各一行（五种都已编进这个构建，而且全都走同一个 WinRT `BitmapEncoder` 调用，只是 encoder id 不同）。`registered` 恒为 `unknown`，因为这一层不去实测编码器（实测就是"用一次编码来探测能力"，与"不靠截屏探测"是同一条理由）；曾经列过但没有编码器的 `webp` / `ico` 以 `compiled: false` + `reason: "not_compiled"` 留在这里，好让调用方拿到确定答案                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| `cursor`                              | `--cursor` 这一条的故事：`default`（不给这条选项时的下场）、三种取值、那唯一一条开关写成 `compiled` / `status` / `reason` / `minBuild`（19041）/ `verifiedOnThisMachine`，然后每条已登记的内部路径一行（`capability` 是 `settable` / `excludes_cursor` / `pointer_state_unverified` / `unregistered`，加 `reason` 与 `include` / `exclude` 各三值 `yes` / `no` / `unknown`），末尾 `pointerShapeCompositing: "never"` 与 `pixelRetouching: "never"`。duplication 那两条是 `capability: pointer_state_unverified`、`include` 与 `exclude` 都是 `no`——桌面那一帧可能已经把指针画在里面，而本工具从不合成指针形状，这证明不了帧里没有指针。没登记的路径读 `unknown` 而不是猜一个答案                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `color`                               | `--hdr` 这一条的故事：`default`（不给这条选项时的下场）、三种取值（`auto` / `tonemap` / `refuse`）、`compiled` / `status` / `reason` / `verifiedOnThisMachine`。`status` 说的是"这个构建带不带得回广色域帧 + 映射怎么做"，**不**去问那块屏此刻是不是 HDR 模式（reason 是 `hdr_display_mode_not_probed`）；`verifiedOnThisMachine` 恒为 `no`（本项目没有 HDR 屏，不宣称色彩验收通过）。每条已登记的内部路径一行，`capability` 是 `wide_gamut_capable`（只有 `wgc` / `screen.wgc`）/ `wide_gamut_unverified`（`duplication.frame` / `screen.duplication`：那块桌面纹理**可能**回来 FP16 或 10 位，但本构建在 `DuplicateOutput` 之前从不问显示器的色彩空间，所以既证明不了拿到的是什么，也兑现不了任何策略）/ `sdr_source_only`（其余只能带回 8 位 DC 的那几条）/ `unregistered`；另外逐行给 `honorsExplicitPolicy`——只有那两条 `wgc` 是 `true`，这正是"本构建只有 `wgc` 兑现得了 `tonemap` / `refuse"` 的机器可读写法。外加 `toneMapping` / `floatIntermediateFrame: "per_pixel_registers"` / `encoderOutput: "sdr_bgra8"`（HDR 一律映射成 8 位 SDR 交付，不出 HDR 原生图）                                                     |
| `autoChainWindow` / `autoChainScreen` | 本机现在能试的 `auto` 链。与截图那次 `-v` 回显的 `input.captureChain` 由**同一个** `GateChannels` 算出，`tests\capabilities.ps1` 逐条比对这两处                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `limits`                              | 单边像素上限、整帧字节上限、`--timeout-ms` 上限、隔离调用内置上限、WGC 帧池重建次数、编号与 PID 上限、`stdoutTargetsMax: 1`、JPEG 质量区间                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `privacy`                             | 自述这份查询没做的事：不采集画面、不弹框、不上传、不枚举用户文件、不读环境变量、不含用户名、不含路径                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| `caveats`                             | 稳定的 ASCII token，列"这份报告没有断言什么"：`available_is_not_a_guarantee`、`no_capture_performed`、`no_consent_dialog_shown`、`encoder_state_not_probed`、`device_capability_not_predicted`、`consent_dialog_state_inferred_not_probed`、`subsystem_version_is_linker_default`，以及按本机情况追加的 `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown`，以及恒有的 `cursor_effective_is_a_setting_not_a_pixel_check` + `pointer_shape_never_composited_nor_erased`（光标那几个字段只说得到设置与来源那一层，说不到"这一张图里看得见或看不见指针"），以及恒有的 `hdr_tone_mapping_not_verified_on_hdr_display` + `hdr_output_is_tone_mapped_to_sdr_bgra8` + `hdr_explicit_policy_only_fulfilled_by_wgc`（HDR 的映射数学离线判过但没有 HDR 屏实测，HDR 一律被映射成 8 位 SDR 交付，而明确写出来的 `tonemap` / `refuse` 要求在本构建里只有那两条 `wgc` 路径兑现得了） |

两份文档由**同一个**判据函数（`src/EnvReport.cpp` 的 `BuildEnvReport`）算出，只差段落取舍：
`--diagnostics` 固定带 `build` 那一段（PE 链接时间戳、机器类型、映像大小、子系统），`--capabilities` 只在
`--verbose` 时展开它。版本号、`status`、后端清单、`limits` 都是同一份，所以不存在"两份互相打脸的环境信息"。

**构建标识是可核对的**：`buildId` 是 `版本-架构-十六进制链接时间戳`，那一个时间戳与 `dumpbin /headers`
读发布产物读到的是同一个字段，读的是本进程自己已经映射进内存的那份 PE 头——不开文件、不枚举目录，
所以也不会因为安装路径里有用户名而漏出身份。PE 头里的 `subsystem version` 只作为事实列出，并配一条
`subsystem_version_is_linker_default` 的 caveat：那是 MSVC 链接器默认值，不是支持声明。

`--capabilities` / `--diagnostics` 只接受 `--lang`、`-v`、`-q`：截图那一套选项（窗口条件、`--monitor`、
`--capture`、`--out` 与位置参数、`--yes`、`--dry-run`、两条期限）与它们\*\*同时给出就是 `cli.query_conflict`

- 退出码 1\*\*，一次报全所有冲突项，一张都不截、一个文件都不写。这两条命令也不参加"没给条件就显示帮助"
  那一条：查询本身就是明确的意图。`-q` 对查询只去掉 `caveats` 那一段，`-v` 加的是 `probes`，都不会动答案本身。

这份文档从头到尾是 ASCII（机器读的取值一律不翻译），所以同一台机器上换任何一种 `--lang`，输出逐字节相同。

### 结构化的窗口发现与检查（`--list` / `--inspect`）

`--dry-run` 对"这批条件会命中哪些窗口"的回答，是每个候选**一行给人看的话**塞在 `note.dry_run` 里
（`hwnd=0x… pid=… 1261x614+681+22 class=… title=…`）——这种散文形状从没被承诺过保持稳定，调用方要用的话就得从
一句里把句柄、矩形、标题再解析回来。这两条命令就是同一个问题的结构化答案。它们跑的是与截图**同一套**条件求值
（同一选项写多次取并集、不同选项取交集、`--monitor` 按屏过滤，用了 `--title-regex` 或 `--timeout-ms` 时照旧整步
进辅助进程），但不产任何图片：

```powershell
ECAPTURE.EXE --list --process notepad.exe                    # 每个命中的窗口，结构化
ECAPTURE.EXE --list --class CabinetWClass --limit 5 --offset 5
ECAPTURE.EXE --list=all --title-contains 报告                # 把最小化的也列进来
ECAPTURE.EXE --inspect --hwnd 0x001A0B4C                     # 一扇窗口，逐项查清楚
ECAPTURE.EXE --inspect --process notepad.exe --topmost-match  # 与截图完全同一套消歧
```

五条规矩，每一条都因为另一条做法更坏：

- **不取像素、不问人、不写文件。** 不调任何取帧通道，不弹确认框，不建文件，不联网，不读环境变量 —— 文档自己在
  `authorization` 段写着（`pixelsRead: 0`、`consentDialogShown: false`、`filesWritten: false`）。它同样**不动任何
  目标窗口**：不恢复、不激活、不改叠放次序 —— "我先看一眼开着什么"不该改变屏幕上的样子。`caveats` 里的
  `no_capture_performed` 与 `no_window_touched` 就是钉这一条。
- **命中多扇不是截图歧义。** `--list` 把它们分页交回（`--offset` / `--limit`，本批默认 50 条），真实总数写在
  `pagination.matched`，于是"这一页很短"永远不会被读成"只有这些窗口"。一个都没命中是正常答复：`windows: []` +
  退出码 `0`，不是 `match.no_window` + `4`。`--inspect` 需要一个目标，用的正是截图那一条选择策略：策略之后仍剩多扇
  就是 `match.ambiguous_window` + 退出码 `5` —— 不替你选一个，也不会"先拿一扇看起来一样的"。
- **列表是一份快照，会过期。** 句柄会被复用、标题会变、进程会退出，所以这里的 `hwnd` / `pid` / 类名**不是**一种可以
  长期持有的凭证。每次成功的查询都带一条 `note.window_query_stale`，而每一行的 `identity` 段写着
  `verificationRequired: true`、`isAuthorizationToken: false`、`raceWindowReducedNotEliminated: true`。真去截图时
  仍在读像素之前复核目标身份（那是 `capture.target_gone` / `capture.target_changed` /
  `capture.target_unverifiable`），确认框也照旧按像素来源判：**`--yes` 在这里不起任何作用**
  （`authorization.yesAffectsResult: false`）—— 它既不会多解锁一个字段，也不会跳过一次本就不弹的框。
- **读不到的字段会说它读不到。** 跨进程的问答有三种下场，逐字段写：`readable`、`denied`（系统挡下了这个调用方）、
  `failed`（问过而没答案），后者带原始 Win32 码。读不到的值是哨兵（`0` / 空串）**加上**这个状态，不是把键悄悄省掉；
  文档也不劝你改用管理员身份 —— `caveats` 里写着 `unreadable_fields_are_not_a_prediction`。
- **可见性策略写出来，不让调用方猜。** 不可见与零尺寸的窗口被排除（与截图那一次枚举同一条规则），`policy` 段就这么
  说（`invisibleExcluded`、`zeroSizedExcluded`）；最小化窗口默认也不进列表，条数记在
  `policy.minimizedExcluded`，`--list=all` 把它们按同一根 Z 序轴并进来。这里对"系统窗口"**不作任何断言**：
  Windows 没有一个"我是系统窗口"的属性可问，所以 `policy.systemWindowAssertion` 是 `false`。

每行的字段如下（`--inspect` 把 `windows` 数组换成单个 `window` 对象，其余字段完全同一形状）：

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

`title`、`class`、`image` 逐字交付 —— 不截断、不拼进一句人话、不折叠大小写 —— 调用方读字段，不该再去解析一段句子。
归属映像的**完整路径**默认不写，因为安装路径里常含用户名；要它得显式写 `--inspect=path`。匹配 `--exe` 本来就一直
读得到路径，与报告里交不交这件事无关。`identity` 交出的正是截图那一次要复核的几件事（句柄、PID、该 PID 的创建
时间、类名，外加"要不要靠重跑当初那份条件来认它"），所以 `--inspect --hwnd <那个句柄>` 描述的约束集与截图会坚持
的那一套完全同源。枚举当时问不到创建时间就写 `unknown` —— 那是一次**没做出来**的判定，不是一个等于 0 的值。

`--list` / `--inspect` 接受窗口条件、`--monitor`、`--offset` / `--limit`、`--timeout-ms`、`--yes`（不起作用）与
`--lang` / `-v` / `-q`。与截图那一级的选项一起给出是 `cli.window_query_conflict` + 退出码 `1`（`--out`、位置参数、
`--format`、`--quality`、`--no-overwrite`、`--capture`、`--dry-run`、`--consent-timeout-ms`、`--capabilities`、
`--diagnostics`，以及两条窗口查询同时给出）。选择策略那一组按入口分别判：它们是用来把目标收窄到一扇的，所以对
`--inspect` 有效，而 `--list` 说的本来就是"全部命中"，配它算冲突。与环境查询一样，这两条不会掉进"无条件 = 帮助"；
而**参数本身**说不通时交回的形状仍是截图那一份（`captured: 0`、`images: []`、`errors[]` 用同一批码），调用方按
`errors[].code` 分支的那段代码不必分叉。`--list` 那份契约叫 `windowquery`，`--inspect` 那份叫 `windowinspect`：
形状不同、契约名不同，字段共用同一套。

`--dry-run` 原样保留，仍是那个兼容入口：它照旧把答案写在 `note.dry_run` 里，照旧不需要输出路径，也与
`--list` / `--inspect` 互为冲突 —— 而不是被这两条悄悄替换掉。

### 只读的屏幕枚举（`--screens`）

在此之前，"要那一块屏"只有一种写法：`--monitor <n>`，而那个 n 是**本次运行 `EnumDisplayMonitors` 枚举里的位置**。
它不是「显示设置」里 Windows 写的那个标识号，拔掉重插或改一次分辨率之后可能指到另一块面板上——而猜错的结果是
一张没人批准过的画面已经落盘。`--screens` 把这个问答成数据：

```powershell
ECAPTURE.EXE --screens                                  # 每块屏，连同它的几种身份
ECAPTURE.EXE --monitor device:DISPLAY1 --out shot.png   # 按本次桌面连接的设备名
ECAPTURE.EXE --monitor "id:\?\DISPLAY#GSM41A2#5&…#{…}" --out shot.png   # 按跨会话的监视器设备路径
```

它与其他只读查询一样只读：一个像素都不取、不弹确认框、不写文件、不联网，也不改任何显示设置——为了搞清"这块屏
转了多少度"去调 `SetDisplayConfig`，等于把考卷改了再答题。这份文档是第三份契约（`screens`，版本 1），与截图那份
精简 JSON 各自独立，就像 `capabilities` 与 `windowquery` 一样。

每条屏有四种身份，各自写明它稳到哪一层（写成字段，不是正文）：

| 字段                | 是什么                        | 稳定范围                 | 选择器           |
| ------------------- | ----------------------------- | ------------------------ | ---------------- |
| `ordinal`           | 本次枚举里的位置              | `this_invocation`        | `--monitor <n>`  |
| `deviceName`        | GDI 视图设备名 `\\.\DISPLAY1` | `this_desktop_attach`    | `device:`        |
| `monitorDevicePath` | 监视器 devnode 设备接口路径   | `cross_session_expected` | `id:`            |
| `adapterLuid`       | 适配器的本地唯一标识          | `this_session`           | 无——只作关联信息 |

`screens[].selectors` 直接给出可以原样抄回去的那两条（`device:DISPLAY1`、`id:\?\DISPLAY#…`），`identity.*` 写明
哪一种能当选择器用。适配器 LUID 故意没有选择器写法：它只在本次会话内唯一，拿它点名一块屏是下注而不是引用。
`adapter.devicePath`（适配器自己的设备接口路径）、`adapter.outputTechnology`、`targetId`、`targetAvailable` 来自
同一次问答，"这块屏挂在哪块卡上"就是这么交回的。

这一层守三条规矩：

- **绝不换成另一块屏。** 标识在本机不存在 = `match.monitor_unknown_id`（退出码 4）；同一标识命中多块 =
  `match.monitor_ambiguous_id`（5，候选全列出来，工具不替你挑一块）；那一问没给出答案 =
  `match.monitor_id_unverifiable`（7，`hint` 明说换 `--capture` 不是下一步——这一路根本没选过通道）。
  三条都不会悄悄退化成"那就用主屏"：没人批准的整屏画面正是确认框要拦住的东西。
- **问不出来就说问不出来，不写成空值。** `dpi`（有效值与原始值，走 `shcore!GetDpiForMonitor`，Win8.1 起）、
  `rotation.degrees`（人看到的朝向，取当前 `DEVMODE`）与 `rotation.panel`（相对面板原生朝向，取显示配置）
  是各独立的问题，各有自己的 `readability`（`readable` / `denied` / `failed`）与那一条 API 自己的错误码。
  某个键不见了就只意味着"没答案"，原因写在旁边；这里不建议以管理员身份运行。
- **是快照，不是凭证。** 每次成功的列表都带 `note.screen_query_stale`，`caveats` 里有
  `device_names_are_not_persistent`、`cross_session_stability_not_tested`、`screen_capture_always_asks`。
  点名一块屏不替代取帧之前的身份复核，也不替代授权：桌面像素一定要人点头，`--yes` 也不例外
  （见下面《截图授权与 --yes》那一节）。

按标识点名在一切"要选一块屏"的地方都成立：与窗口条件同用（`--monitor id:… --class …` 就是按那块屏过滤窗口）、
与 `--list` / `--inspect` 同用（走的是同一个选择函数），以及取帧之前的复核——它按**选定当时问得到的那条身份**核对，
设备名若已属于另一块面板就以 `capture.monitor_changed` 停下，复核本身问不出来就以 `capture.monitor_unverifiable`
停下，而不是退回去照名字截。`--monitor <n>` 的语义一字未动，越界照旧是 `match.monitor_out_of_range` 并在 `hint`
里列出本机全部屏幕；两条新写法写在 `--help` 与上面的取值写法一节里。

`--screens` 属于环境查询那一家族：只接受 `--lang` / `-v` / `-q`，其余（含 `--yes` 与 `--monitor`）都是
`cli.query_conflict` + 退出码 1，也不参加"零条件就出帮助"那一条。`--quiet` 只去掉 `notes`：`identity`、
`readability`、`authorization`、`caveats` 是判据而不是礼貌性提示。这份文档会打印设备路径，但不含文件系统路径与
用户名（`privacy.includesDevicePaths: true`、`includesFileSystemPaths: false`）。

## 取图方式

| 取值          | 通道                                     | 能截被遮挡窗口     | 硬件加速内容           | 光标（`--cursor`）                                                           | API 历史下限                                                                                          |
| ------------- | ---------------------------------------- | ------------------ | ---------------------- | ---------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `wgc`         | Windows.Graphics.Capture                 | 能（DWM 缓存）     | 正常                   | 有一条真能设进去、也读得回来的开关（要 19041 起）                            | Win10 1903（18362）——是 `CreateForWindow` / `CreateForMonitor` 那条互操作接口，不是 1803 那个命名空间 |
| `dwm`         | DwmRegisterThumbnail                     | 能                 | 多数正常，受保护窗口黑 | 来源像素里没有光标                                                           | Win8.1（9600）——注册缩略图更早，但读回靠 `PrintWindow(PW_RENDERFULLCONTENT)`                          |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT       | 能（窗口自绘）     | 常常全黑               | 来源像素里没有光标                                                           | Win8.1（9600），指那个 flag                                                                           |
| `bitblt`      | BitBlt 屏幕 DC                           | 不能，只拷可见像素 | 部分黑                 | 来源像素里没有光标                                                           | 本身没有版本门槛                                                                                      |
| `duplication` | DXGI 桌面复制整屏帧后按矩形裁            | 不能，只拷可见像素 | 正常                   | 没有开关：桌面帧里可能已经把指针画进去了，所以 include 与 exclude 都无法保证 | Win8（9200），远程桌面/虚拟显卡常拿不到内容                                                           |
| `auto`        | 按 wgc → dwm → printwindow → bitblt 回退 | 尽量               | 尽量                   | `include` 会把链收窄成只剩 `wgc`                                             | 这条链减去本机版本挡掉的那几条                                                                        |

那一列是**各条路线的 API 历史下限**，逐条对到微软为该路线实际调用的那个接口所写的文档。它们既不是这个程序
声明能跑的版本，也不是实测过的版本：声明下限（Win10 1903，x64）、六条通道共用的那道编码器下限、真正实测过的
版本（Win10 22H2 / 19045），以及运行时会把模糊失败换成哪一条清楚的报告，都见[系统支持](#系统支持)。

- 想要"那个窗口自己的画面"（哪怕被别的东西盖住）用默认的 `wgc`；想要"屏幕上此刻的样子"（连遮挡物一起）用
  `bitblt` 或 `duplication`。
- `wgc` 跟随窗口的实时尺寸：它读每一帧自带的内容尺寸，而不是只看建帧池那一刻采集项的尺寸。窗口在"选到它"与
  "取到帧"之间被缩小时，只复制那块有效矩形（不会把较大纹理里多出来的没定义边缘当成画面）；被放大到超过帧池时，
  会在 `--timeout-ms` 预算内重建帧池再取一帧。绝不交一张被裁掉却按整窗宣称完整的图，`images[].width`/`height`
  就是那一刻的实际尺寸。
- 要不要问人，取决于这条路径实际从哪儿取像素，而不是取决于你敲的通道名——见下一节。
- `--capture` 取值写错在解析期就报 `cli.unknown_capture_method`（退出码 1），**不会退化成默认通道**；
  只有 `auto` 允许回退，回退成功会发 `note.capture_channel` 说明实际用了哪条。
- DRM / 受保护内容一律黑屏；驱动黑框（部分播放器）有的通道能过、有的不能，不保证。
- 整屏截图只走 `wgc` / `duplication` / `bitblt`；`--monitor` 配 `dwm` 或 `printwindow` 在解析期报
  `capture.unsupported`（退出码 1）。`auto` 在屏幕模式下按 wgc → duplication → bitblt 回退。
- `duplication` 对目标屏幕做三件事，每一件都如实报告而不是默认成立：
  - **旋转校正。** 驱动交回来的桌面帧未必处在该显示器实际显示的方向上。这条路径先比对输出自己声称的
    （`DesktopCoordinates`）与真正拿到的纹理，再把裁剪按顺时针转 0 / 90 / 180 / 270 度，交付的图因此
    永远和确认框上列出的矩形在同一套坐标里——绝不出现第二次翻转。实际转了多少写在 `images[].rotation`。
    两种形状都对不上的纹理直接判 `capture.frame_invalid`，而不是照裁不误。
  - **落在哪块显卡适配器上。** 先枚举全部适配器与输出、在这张表里定位目标，然后在**拥有该输出的那个
    适配器**上创建 D3D11 设备——这正是 `DuplicateOutput` 的要求。由第二块显卡驱动的屏幕因此也够得着。
    这条路径没有 WARP 退路：软件设备不拥有任何物理输出，只会交回
    一幅空的帧却仍然报告正确的尺寸。
  - **一个目标只对应一块输出。** 横跨两块屏（或挂出屏幕边缘）的窗口，取的是它与重叠面积最大的那块
    输出相交的区域，其余部分*不在*图里。这表现为 `capturedRect` != `requestedRect`、`clipped` 和
    `note.capture_clipped`，而不是静默地看起来像一整个窗口。把一扇窗口跨适配器拼起来没有实现。
- 如果目标屏幕在确认之后离开了桌面或改变了形状，截图即停并报 `capture.monitor_changed`（退出码 7）——
  工具绝不拿另一块屏顶替，授权始终绑在人看过的那一块上。

## 画面里的鼠标指针（`--cursor`）

`--cursor default|include|exclude` 说的是画面里要不要鼠标指针。默认值 `default` 的含义是本工具对这件事
**一个字都不改**：不碰任何通道的光标设置，结果里也不出现光标那三个键 —— 输出与这条选项存在之前
逐字节相同。刻意写 `--cursor default` 是另一件事：同样不要求改动，但结果要报这条路径实际交回的是什么。

各条路线能承诺到哪一层，判据是**它的像素从哪来**，不是通道名字。那份登记表在 `src/CursorControl.h`：
按 `images[].path` 那个内部路径名一行一条，与授权那张登记表同一种形状；没登记的路径按严格处理
（两种要求都不敢声称）。

- `wgc` 与 `screen.wgc` 是仅有两条真有可设进去、也能读回来核实的开关的路径 ——
  `IGraphicsCaptureSession2::IsCursorCaptureEnabled`，Windows 内部版本 19041 引入。这道门槛**比 `wgc`
  通道自己的 18362 还高**：1903 的机器能用 `wgc` 截到图，却仍然对光标这件事说不出任何保证。
- `printwindow`（让窗口自己画进 DC）、`dwm.thumbnail`（DWM 重定向位图）、`dwm.screen` /
  `bitblt.screen` / `screen.bitblt`（屏幕 DC，系统指针画在 DC 内容之外）这几类的来源像素里根本没有光标：
  所以对它们 `exclude` 是来源那一层的事实，而 `include` 就是做不到。
- `duplication.frame` / `screen.duplication`（桌面合成分）属于**指针状态无法核实**的那一类：桌面帧有可能
  已经把指针画在里面了，而这条路径没有可设的开关，既不能保证画、也不能保证不画。所以对 duplication，
  `include` 与 `exclude` 都兑现不了；`--cursor default` 交回的帧其指针状态一律记 `unverified`（见下面的字段表）。

由这张表推出两条规矩，两条都是为了不让"我要求过"被读成"已经办到了"：

- **做不到的那条就拒绝，不偷偷改道。** `--cursor include` 配 `printwindow` / `dwm` / `bitblt`（来源里
  根本没有指针可加）在解析期就是 `capture.cursor_unsupported`（退出码 `1`）—— 在弹框之前、在算输出名之前、
  在读任何一个像素之前。配 `duplication` 时 `include` 和 `exclude` **两样都**在解析期是
  `capture.cursor_unsupported`（退出码 `1`），各有一种措辞：`include` 是这条来源没有指针开关，`exclude` 是
  桌面帧里指针的状态证明不了。换成会读桌面像素的通道既加不回光标，也保证不了去掉一个，交回的更是一份
  没人批准过的画面。`--capture auto` 时兑现不了的那几条从链里摘掉，各留一条
  `note.cursor_channel_skipped`；摘到一条不剩，或者本机版本低于 19041 而要求是明确写出来的那一种，
  就是 `env.cursor_unsupported`（退出码 `7`），一个像素都不取。`-v` 回显的 `input.captureChain` 与真去
  截图时用的是同一个函数，所以 `--cursor include` + `auto` 那里看到的就只剩 `["wgc"]`。
- **不拿图像修补冒充能力。** 本工具不去取桌面复制那份指针形状来画，不会把光标画进帧里，
  也不试图把已经画进去的光标抹掉 —— 那些都是图像修补，也都没有一样能核实。`src/` 里一旦出现这种调用，
  `tests\cursor.ps1` 就红。

只要写过 `--cursor`，每张交出的图就带这三个字段（没写过时一个都不出现）：

| 字段              | 取值                                                                                                                                                                                       | 说的是哪件事               |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------- |
| `cursorRequested` | `default` / `include` / `exclude`                                                                                                                                                          | 要求的是哪一种             |
| `cursorEffective` | `include` / `exclude` / `unverified`                                                                                                                                                       | 这条路径实际交回的是哪一种 |
| `cursorBasis`     | `wgc_session_property_set` / `wgc_session_property_read` / `path_excludes_cursor` / `path_pointer_state_unverified` / `wgc_cursor_property_unavailable` / `path_capability_not_registered` | 这个结论凭什么             |

`wgc_session_property_set` 是按这一次的要求设过、再把读回来的值核对了；`wgc_session_property_read`
是没设过（`--cursor default`）只读当前值；`path_excludes_cursor` 是这条路径的来源像素里没有光标；
`path_pointer_state_unverified` 是桌面复制的那一帧、它的指针状态证明不了（所以 `cursorEffective` 是 `unverified`）；
`wgc_cursor_property_unavailable` 是那一问没答案；`path_capability_not_registered` 是这条路径没进登记表、
于是按严格处理而不是去猜。凡是无法核实的这一类，`cursorEffective` 都写 `unverified`，不折成「画」或「不画」任何一种。

`cursorEffective` 就断言到这一层为止。它说的是"这条会话被设成画/不画光标"，或者"这块来源里没有光标"，
**不是**"这一张图里此刻看得见或看不见指针"。本 SDK 的会话接口没有 `IsCursorVisible` 那个只读属性，
所以像素级的事本工具一条都不声称，而 `--capabilities` 把这条边界写成
`cursor_effective_is_a_setting_not_a_pixel_check` 那条 caveat。明确要求过（`include` / `exclude`）而在 `wgc`
那一次核实不了（接口取不到、设不下去、或读回来是相反的那一件）时，代码是 `capture.cursor_unverifiable`
（退出码 `7`），且发生在 `StartCapture` **之前** —— 与要求相反的那一张根本不会交出去；
`--cursor default` 遇到同一情况就报 `unverified`，而不是折成任何一个答案。

要求光标这件事不改变授权。判据仍然是"这条路径的像素从哪来"，所以 `--cursor exclude` 配 `bitblt` 的屏幕路径
或任何整屏目标，以及 `--cursor default` 配 `duplication` 路径，照样一定弹框，`--yes` 照样管不着；`wgc`
不带 `--yes` 照样要问。（而 `--cursor exclude` 配 `duplication` 在解析期就被拒绝，根本走不到弹框那一步。）
那几种弹框现场在 `tests\cursor.ps1` 里用自建窗口判。这三个字段与 `path` / `scope` / `rect` 以及裁剪那几项一样是定位判据，
`--quiet` 不许抑制。

`--capabilities` 不截任何一个像素就能回答上面这些，那一段叫 `cursor`：默认值、三种取值、那唯一一条开关的
`compiled` / `status` / `minBuild` / `verifiedOnThisMachine`，每条已登记路径一行
（`capability` / `reason` / `include` / `exclude`，后两者各是 `yes` / `no` / `unknown`），
以及 `pointerShapeCompositing: "never"` 与 `pixelRetouching: "never"`。

## HDR 色彩处理（`--hdr`）

显示器处在 HDR 模式时，采集回来的帧可能带着超出 SDR 的亮度范围与另一种传递函数。把那种帧硬按
8 位 BGRA 解释，得到的是一张发白、去饱和、亮部一团糊的图，而它"看着像一张正常图"—— 本工具不把这种
结果默认为正确。`--hdr auto|tonemap|refuse` 就是让你对这件事作出明确决定。有两种在**取图**时行为相同、
却在**上报**时必须分开的状态：

- **完全没写 `--hdr`** —— 本工具对色彩一个字都不改：不探测显示状态、不改采集格式、不做映射，
  **而且结果里不出现色彩那组键** —— 输出与这条选项存在之前逐字节相同。
- **明确写了 `--hdr auto`** —— 取图行为一样（不探测、不改格式、不映射），**但色彩那组键会照常出现**，
  只如实报这一帧被动带回来的样子。在 `wgc` 路径上一张 8 位交付帧报 `hdrEffective: unverified` /
  `hdrBasis: bgra8_source_unverified`，因为 `auto` 从没问过显示器是否处于 HDR 模式，而一张 8 位 surface
  并不能证明来源就是 SDR（见下面的字段表）。

`auto` 也是省略这条选项时所用的默认*取值*——但省略与明确写出在结果里可以用 `input.hdrGiven`（`-v`）区分开，
而这正是本工具拒绝合并的那一对"没写"与"写了"。

哪条路线带得回广色域帧、哪条真兑现得了明确写出来的策略，是两个不同的问题，判据与别处同源：**它的像素从哪来**，
不是通道名字。那份登记表在 `src/HdrColor.h`，按 `images[].path` 一行一条，`--capabilities` 把它打成 `color.paths`，
每行带 `capability` 与 `honorsExplicitPolicy`：

- `wgc` / `screen.wgc` —— `wide_gamut_capable`、`honorsExplicitPolicy: true`。来源跟随显示模式，而这条路径会读回
  自己实际拿到的格式（FP16 scRGB 线性，或 8 位 BGRA），所以它说得出是哪一种、也映射得了。本构建里兑现
  `tonemap` / `refuse` 的只有这两条。
- `duplication.frame` / `screen.duplication` —— `wide_gamut_unverified`、`honorsExplicitPolicy: false`。那块桌面纹理
  *可能*回来 FP16 scRGB，也可能回来 10 位 ST.2084 (PQ) / HLG BT.2020，但本构建在 `DuplicateOutput` 之前从不问
  显示器的色彩空间，所以它证明不了自己拿到的是什么，也就承诺不了任何策略。在这里要 `tonemap` / `refuse` 得到的
  是拒绝，而不是一个含糊的答案。
- `printwindow`（窗口自绘进 8 位 DC）、`dwm.thumbnail` / `dwm.screen`、`bitblt.screen` / `screen.bitblt`
  —— `sdr_source_only`、`honorsExplicitPolicy: false`。它们结构上只带得回 8 位 SDR，所以对它们而言 HDR 处理没有
  对象，是恒等而不是"做不到就换一条"。

三条取值：

- `tonemap` —— 要求把 HDR 帧映射成 SDR 交付。本工具在编码之前建一份**逐像素的浮点中间量**（不分配整幅
  浮点帧，免得把 1 GiB 的整帧预算乘四撑爆），按固定曲线处理：解传递函数（scRGB 线性 / PQ→绝对亮度→
  相对线性 / HLG 反 OETF）→ BT.2020 到 BT.709 的原色矩阵 → 按亮度做**扩展 Reinhard** tone mapping
  （确定、单调，`white=1` 时退化为恒等）→ sRGB 编码 → 不透明 alpha 直通。来源本就是 SDR 时是恒等透传。
- `refuse` —— 一旦核实来源确是 HDR 帧就报错、一个像素都不落地，绝不交一张被硬压成 BGRA8 的发白图。
- `auto`（默认取值）—— 不启用上面那条链路：不探测显示、不改格式、不做映射。明确写出 `--hdr auto` 时
  仍照常发出色彩那组键，只被动如实上报这一帧带回的样子（一张 8 位的 `wgc` 帧是 `unverified`，不是
  「确认了 SDR」）；整条选项被省略时那组键根本不出现（见本节开头）。

由这张表推出两条规矩，与光标那一条同源：

- **做不到的那条就拒绝，不偷偷改道。** 点名 `printwindow` / `dwm` / `bitblt` / `duplication` 其中一条并要
  `--hdr tonemap` / `refuse`，在解析期就是 `capture.hdr_unsupported`（退出码 `1`），**不换后端**（换成会读桌面像素的
  那条既没有更多 HDR 可映射，交回的也是一份没人批准过的画面）。`--capture auto` 时这条要求改去收窄回退链——用的
  就是 `FilterChainForHdr` 那一条判据，与 `-v` 回显 `input.captureChain` 时是同一个函数，所以 `auto` 配 `tonemap`
  只剩 `wgc`：每条被摘掉的通道各留一条 `note.hdr_channel_skipped`，一条不剩时整次以 `env.hdr_unsupported`
  （退出码 `7`）停下，而不是交出一张没映射过的帧。而一次取回来是 `capture.hdr_refused` 的结果会让整条链当场停下——
  绝不换后端重试，因为换个后端只会换来另一张同样没被批准的画面。
- **认不出就是认不出。** 带回一个本构建认不出的广色域像素格式时是 `capture.hdr_unverifiable`（退出码 `7`），
  既不硬按 BGRA8 解释，也不"猜一个映射"；`--hdr refuse` 且核实来源是 HDR 时是 `capture.hdr_refused`
  （退出码 `7`）。三条都在编码之前给出，都不落地。

只要写过 `--hdr`，每张交出的图就带这组字段（没写过时一个都不出现，与这条选项存在之前逐字节相同）：

| 字段               | 取值                                                                                                                                                                                                                                  | 说的是哪件事         |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------- |
| `hdrRequested`     | `auto` / `tonemap` / `refuse`                                                                                                                                                                                                         | 要求的是哪一种       |
| `hdrEffective`     | `sdr_passthrough` / `tone_mapped` / `unverified`                                                                                                                                                                                      | 这一帧实际经历的处理 |
| `hdrBasis`         | `delivered_bgra8_sdr` / `scrgb_float_tone_mapped` / `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `path_sdr_source` / `format_unrecognized` / `transfer_function_unknown` / `bgra8_source_unverified` / `tone_map_not_applied` | 这个结论凭什么       |
| `sourceColorSpace` | `srgb_bgra8` / `scrgb_float` / `pq_bt2020` / `hlg_bt2020` / `rgb10a2_unverified` / `unknown`                                                                                                                                          | 编码之前那份来源     |
| `sourceBitDepth`   | `8` / `10` / `16`（认不出来时整个键不出现）                                                                                                                                                                                           | 来源每通道位数       |

本构建里真机截图只走得到的映射路径是 `scrgb_float_tone_mapped`（FP16 的 `wgc` 纹理池），因为一张 10 位
打包帧会被判为 `rgb10a2_unverified`，而不是当成 PQ 或 HLG——所以 `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped`
与 `transfer_function_unknown` 是离线走过的枚举取值，并不是"本机真有一块 PQ/HLG 面板被映射过"的承诺。一个带
广色域标记却没有映射记录的来源是 `unverified` / `tone_map_not_applied`，绝不当作"已经映射"。

明确要过处理（`tonemap` / `refuse`）而这一帧以 8 位 BGRA 交付时，图照常交付（映射对 8 位来源是恒等的），
但留哪一条提示取决于取图之前到底问过没有：如果那次只读地问过显示器状态且答的是 SDR，提示是
`note.hdr_source_sdr`（"确认是 8 位 SDR"）；如果那一问压根没问、或没给出答案，提示是
`note.hdr_source_unverified`（"按 8 位交付，但来源没有被确认是 SDR"）——一张未核实的 8 位帧绝不会被写成
一张已确认 SDR 的帧。两种情况都把"我要过 HDR 处理"与"这一帧没有已确认的 HDR"分开放在你眼前，而不是拿一次
静默的通过冒充"HDR 已经被正确映射"。`--hdr auto` 这两条提示都不发（它只通过上面那组机器字段被动上报）。

HDR 色彩这件事不改变授权：整个处理排在取帧之后、编码之前，判据仍然是"这条路径的像素从哪来"。会读到
桌面像素的那几条照样一定弹框、`--yes` 照样管不着，也不引入任何"映射过就算免确认"的旁路。

**这台开发机的显示器不支持开启 HDR**，所以"真带回一幅 HDR 帧并映射"的端到端现场在本机造不出来：
tone mapping 的数学由离线判据用已知色块与亮度梯度逐点判（`tests\hdr_state.cpp`），`--capabilities` 里
`color.verifiedOnThisMachine` 因此恒记 `no`（`hdr_tone_mapping_not_verified_on_hdr_display` 那条 caveat），
`tests\hdr.ps1` 那几条要 HDR 设备的判据一律记未验证。在 HDR 显示器上复核之前，本工具不宣称色彩验收通过。

## 截图授权与 --yes

凡是真要取帧的截图，都先弹一个模态确认框，**可靠的窗口通道也一样**。不弹框也不截的只有这些：没给任何条件
（文本帮助 + `2`）、`--help`、`--version`、`--dry-run`、无匹配（`4`）、匹配多个（`5`）、解析期参数错（`1`）、
以及输出名规划失败（例如 `io.output_collision` + `8`）——整批名字在任何一次发问之前就算完了。

`--yes`（`-y`，正向布尔开关：裸写或 `=true/1/yes/y/on` 是开，`=false/0/no/n/off` 是关，重复给出最后一个生效，
`-v` 的 `input.yes` 回显最终结果）只免掉**一层**确认：帧绑在所选窗口本身、绝不从桌面采样的那些路径。
它别的一概不保证：不保证图是有效的、不忽略权限、不管受保护内容、不管错误、也不管覆盖保护。
决定属于哪一层的是实际走的那条路径，不是通道名：

| 路径（`images[].path`）                             | 像素从哪来                 | 不给 `--yes` | 给了 `--yes` |
| --------------------------------------------------- | -------------------------- | ------------ | ------------ |
| `wgc`、`printwindow`、`dwm.thumbnail`               | 只有所选窗口自己           | 问一次       | 不问         |
| `dwm.screen`、`bitblt.screen`、`duplication.frame`  | 那个窗口所在的那块屏幕区域 | 要问         | **照样要问** |
| `screen.wgc`、`screen.bitblt`、`screen.duplication` | 整块屏幕                   | 要问         | **照样要问** |

判不出来或没登记的路径一律按桌面路径处理，所以新增通道忘了登记只会更严不会更松。`--monitor` 与窗口条件同时
给出时筛的是**窗口**，出的仍是窗口图，按上面窗口那两行走。

- **桌面路径没有任何旁路**：`--yes`、`--quiet`、环境变量、stdin、调用者是谁，都跳不过它——分级就是为了这一件事。
- 一次确认可以覆盖本次请求里明确列出的那一批目标，所以多个后端、多个窗口不会各问一遍。但这份许可是一个**快照**
  而不是一项长期授权：它绑的就是对话框上列出的那些目标连同当时展示的区域、整批改好的绝对输出名、以及屏幕拓扑的
  指纹，而且复核两次——签发之前一次（框开着的时候拓扑挪过位，那就是一次新的提问，不是复用旧答案），以及每一次
  真要采样像素之前一次（那块区域必须还在批准过的范围之内）。授权绝不跨请求缓存，也绝不扩大到框上没列出的目标，
  "同意截窗口画面"更不等于"同意截桌面"：`--capture auto` 配 `--yes` 可以不打扰人地走完窗口那几条，
  可一进桌面路径就必须再问一次。
- 这是一个只有「是 / 否」两个按钮、默认焦点落在「否」上的 `MB_YESNO` 框：只有明确点「是」才算同意，其余一切结果都算拒绝。要拒绝就点「否」。别指望 `X` 或 `Esc`——`MB_YESNO` 下标题栏的 `X` 虽然显示但被禁用、也没有可供 `Esc` 触发的 Cancel 按钮，所以两者都不是可靠的「否」操作，这个框终究得有人来答；而工具本来就把任何非「是」的结果一律当拒绝处理。除此之外，`--consent-timeout-ms` 之内没人回答，本身就是一次独立的拒绝（`capture.consent_timeout`）。有人答「否」、超时没人回答、或者根本没有可交互的桌面——这几种结局都会停掉**同一次请求**剩下的部分：`auto` 回退链剩下的那几条一起停，不只是剩下的 `--all` 目标；不换后端、不重试、不再问第二遍，已经完成的图留在 `images` 里。人的回答是一条边界，不是"再换条路试试，总有一条能问出来"的提示。
- 目标区域挪动过、或者屏幕拓扑变了，覆盖它的授权当场作废并重新问一次；已经绑在旧区域上的那一帧给
  `capture.consent_stale`（退出码 `7`，重新选目标再截）。
- 确认框的默认焦点在"否"上，内容列出目标及其区域、将要走的那条路径、每张图展开后的绝对路径（或"标准输出"）、
  以及画面里会不会混进别的窗口；这条路径会拍到桌面时，框上还明写着"你给的 `--yes` 对它不生效"。框在取帧之前就
  已经关闭，所以不会出现在图里；点"是"之后工具仍要等约 1 秒，因为关闭动画还在 DWM 的画面上。
- 没有可交互桌面时（服务会话、计划任务、锁屏），带 `--yes` 的窗口内容截图照旧正常完成，而桌面路径只能被拒绝——
  绝不会因为"弹不出框"就放行。答"否"给 `capture.access_denied` + `6`，弹不出框给新的稳定码
  `capture.consent_unavailable` + `6`；两者都带 `stage=consent`、`target`、`backend`，`value` 写的是那条路径。
- 被人拒绝就是被人拒绝：`capture.access_denied`（`stage=consent`）+ 退出码 `6`，与有没有给输出路径无关。
  省略 `--out` 就等于 `--out -`，授权结果不因它而变，见《不给输出路径（兼容性说明）》。
- 多块屏 + 写 stdout（`--monitor all --out -`）在任何弹框之前就被拒：一次确认换不来"每块屏一张图挤进同一条流"。
  只有一块屏时 `--monitor all` 是一个目标，那条路仍然按单张走 stdout。
- 说清楚边界：这就是一个 `MessageBox`。它是给合作式自动化（人或 AI）准备的误点防护，既证明不了按下按钮的是人，
  也挡不住同一个权限级别里存心绕过的进程。它能保证的是：照这套规矩跑的调用方，一定会被问上一次。

`--monitor`（省略取值）与 `--monitor primary` 是主屏，`--monitor 2` 是第 2 块屏，`--monitor all` 每块屏一张。
编号是**本次运行 `EnumDisplayMonitors` 枚举里的位置**，从 1 起——它不是 Windows 在设置里写的那个标识，
拔一块显示器或改一次分辨率都可能把它重排，所以不要把编号存下来跨运行指代某块屏。要再次认出同一台显示器，跑一次 `--screens`，把它交回的标识原样写回来：
`--monitor device:DISPLAY1`（本次桌面连接的设备名）或 `--monitor "id:\\?\DISPLAY#…"`（监视器的 devnode
设备接口路径，跨会话还成立的那一条）。标识在本机不存在报 `match.monitor_unknown_id`（退出码 4），命中多块报
`match.monitor_ambiguous_id`（5，候选全列出来，工具不替你挑一块），那一问没答案报
`match.monitor_id_unverifiable`（7）——三条都不会悄悄退化成"那就用主屏"。
编号越界照旧报 `match.monitor_out_of_range`（退出码 1），
`hint` 里列出本机全部屏幕。屏幕目标在截图之前会按**身份**重新核对该屏（选定当时问得到跨会话
标识就按它核，问不到才照旧按名字核）：它若已离开桌面、或那个设备名已经发给了另一块面板，截图停止并报
`capture.monitor_changed`（复核本身问不出答案时报 `capture.monitor_unverifiable`）；它的矩形或位置变了，要人批准的就得是新矩形——对一台被改过尺寸或挪过位置的
显示器，旧的确认绝不复用。
`--monitor <n>` 与窗口条件同时给出＝按屏过滤窗口（窗口矩形与该屏有重叠即命中，跨屏窗口在两块屏上都算），
出的仍是窗口图，所以按上面窗口那两行的规矩授权。`--monitor all` 与任何窗口**匹配**条件互斥
（报 `cli.monitor_conflict`，退出码 1），但 `--all` / `--index` 这类消歧选项不算匹配条件，可以和它搭配。

## 目标身份与句柄复用

选中的窗口在真正读像素之前可能已经不是那一扇了：条件求值与取帧之间隔着输出名规划、人工确认框（人可能想几秒才点，点完之后还有约 1 秒关闭动画）、以及 `auto` 回退链最多四条通道。这段时间里目标可以被销毁，它的 HWND 值可以被另一扇窗口拿走，它的 PID 也可以被另一个进程复用 —— 而一个 64 位整数区分不了「还是它」与「一个长得像它的新对象」。

所以选定那一刻就把身份记下来：句柄、归属 PID、**该进程的创建时间**（把「PID 被复用」与「还是那个进程」分开的就是这一条）、窗口类名，以及当初使它成为目标的那些条件。复核分两档：

- **每一条通道的每一次尝试之前**（授权之后、读像素之前再做一次）：句柄还有效吗、还属于当初那个进程吗、那个 PID 的进程创建时间变了吗、类名对得上吗。这四问的答案都在 user32 / kernel32 自己那份结构里，不往目标线程发消息，所以问得起，也不会在这里被一扇卡住的窗口拖住。
- **每个目标开工之前一次**，外加任何要改走桌面像素之前（`dwm` 的屏幕退路）：拿**当初那份条件重新求值一次**，看这个句柄还在不在命中列表里。标题这类易变属性就是这么判的 —— 应用刷新自己的标题（播放进度、文档修改标记、标签页标题）仍是同一个目标，而不再满足 `--title` 那条条件的就不是。这一问要枚举一遍窗口，所以不按回退链的次数乘上去；`--title-regex` 那种没有中断点的匹配照旧沿用第一次求值那同一条隔离判据（设了期限就进辅助进程）。

判不过就一个像素都不读，交回的是三条稳定的码：

| 码                            | 退出码 | 含义                                                   | 下一步                                                     |
| ----------------------------- | ------ | ------------------------------------------------------ | ---------------------------------------------------------- |
| `capture.target_gone`         | 7      | 句柄在本次请求中被销毁                                 | 重新枚举窗口再来一次                                       |
| `capture.target_changed`      | 7      | 这个句柄值现在属于另一个对象（或不再满足当初的条件）   | 重新选目标；**你批准的许可不会转给新对象**                 |
| `capture.target_unverifiable` | 7      | 有一道判据问不出来（读不到进程信息、条件求值没能跑完） | 查执行环境（权限、策略、杀软），或加大 `--timeout-ms` 再来 |

身份变了**不会**去放宽条件另找一扇「看起来一样」的窗口 —— 这与 `--yes` 那条规矩同源：许可绑定的是当初列给人看的那个对象。

**保证的边界**：这条复核把竞态窗口缩小了，不声称把它消除。判据与取帧之间不是原子的，而 HWND 既不是可等待对象，也没有「锁住一个窗口不让它消失」的公开 API。判据与取帧之间那一瞬仍可能发生变化 —— 只是不再有「整整一段规划加人工确认」那么长的时间可以用来变。

## 执行期限与会阻塞的调用（`--timeout-ms` / `--consent-timeout-ms`）

`--timeout-ms <ms>` 是本次运行自动处理那一段的**总预算**，从开始选目标那一刻起按单调时钟计时。窗口/屏幕匹配
（含 `--title-regex`）、`auto` 回退链、等帧、编码、最后那次提交，花的都是**同一份**预算：没有任何一步、也没有
批次里任何一个后续目标能重新领一份完整预算，所以四条后端不可能各等 2 秒、两个目标也不可能各再等一遍。不给或写
`0` 就是不设总预算；即便如此每次隔离调用仍受一个内置的 5000 ms 上限约束。
预算用尽时，**尚未开始的阶段被拒绝、那一张不落地**——条件求值阶段把预算耗尽给 `match.timeout`（`stage=match`），
帧始终没来给 `capture.timeout` 且 `stage=capture`，而预算死在编码器里时同样是 `capture.timeout`、只是
`stage=encode`（没有 `encode.timeout` 这么一条码，把两者分开的就是这个 stage），写文件/标准输出那一步没能开始则给
`io.timeout`（`stage=write` / `stdout`，退出码 `8`）；批次里剩下的目标不再开始，已经写完的图仍留在 `images` 里。
所以部分完成的一批和局部截图失败表现完全一致：退出码非 0，凡是已经落地的都照样交付。

这道闸门是刻意做成"开工之前拦一道"的，而诚实的边界正是它的另一面：原子写文件没有取消点，所以预算**在提交过程中**
到期的那一张照样会落地，想在调用中途抢停下来做不到。写完返回之后会再复核一次期限：若这时预算已经越过，就在交付记录
之外**额外**记一条 `io.timeout`（消息写明这张图是完整交付的、只是写完时预算已耗尽）——但已经提交的那张图仍留在 `images`
里、`captured` 照样把它计入，本次按部分成功报告、退出码 `7`（已交付 + 出错），而不是那种写都没落地的 `8`。已经交付这件
事实与期限是否达标，是刻意分开的两件事。超时不回滚、也不删除任何东西——磁盘上那个文件存在，是因为调用方要求过它，
悄悄把它删掉等于再写一次没人要的东西。

等一个人是**另一条**时钟：`--consent-timeout-ms <ms>` 只给确认框设上限，绝不动自动处理那份预算（人走开了不等于
"机器慢"）；这段时间确实被从自动预算里暂停出来了，所以一次漫长的阅读不会把那张图自己的期限花掉。到了时间没人回答，
本次请求就按**拒绝**处理——`capture.consent_timeout`、退出码 `6`、`stage=consent`——绝不当成同意，
批次剩下的部分也就像有人明确答"否"之后那样停下。不给或写 `0` 还是一直等，和从前一样。这道限期是**轮询**到的，
不是抢占那个对话框：到期要等下一个等待片段才看得见，而且对话框还有约 3 秒的关闭宽限，所以框可能比写出来的数字多留
一小会儿。点"是"之后那约 1 秒的缓冲是为了把对话框的关闭动画挡在画面之外，它同样属于人工那一阶段，绝不会为了赶期限
而被省掉：被限制的是"等一个回答"，不是"回答之后等画面安定"。

**阻塞到底堵在哪。** `PrintWindow` 是给目标窗口发一个绘制请求、再等它自己的线程；`--capture printwindow` 和
`dwm` 的回读干的就是这件事，而这个调用内部没有能拿来核对期限的打断点。`std::regex` 也一样：像 `(a+)+$` 这种
模式去匹配一个长标题可能回溯好几分钟，而对模式限长度并不是执行期限。这些调用现在都跑在同一个 `ECAPTURE.EXE`
拉起的辅助进程里，父进程通过一条私有管道把已经解析好的一个任务递给它；期限一到，父进程就停掉**它自己的**那个
辅助进程并报出超时。目标应用的窗口绝不会被杀掉，也没有哪个辅助进程能活得比父进程久（一个"关闭即结束"的作业对象，
加一次断管检查，再加一个按预算走的看门狗：它只给 await 握手那一段设一个上限，之后就盯着父进程剩下的预算再加一点点
交付宽限，绝不会把一次明确接受的长预算提前截短）。

收尾那条边界和期限本身同样重要，因为管道恰好会在不对的时机答复。连接时要分清三种情形：辅助进程已经连上了
（没有待取消的 I/O）、写操作同步就完成了、以及返回了 `ERROR_IO_PENDING`。`CancelIoEx` 只是**请求**取消，所以父进程
要等完成被上报，并让每一个 `OVERLAPPED` 连同它的事件一直活到那一刻；完成迟迟不来时，给那个辅助进程约 2 秒自行退出，
之后才 `TerminateProcess`，而这段宽限跑在请求预算**之外**。判完之后才出现的回包绝不读取：一条来晚了的消息既不能把
已经报出的超时改写成成功，也不能作为图片交付出去。而这**不**改变的事：辅助进程只会读单个窗口自己的画面、或列举顶层
窗口，它从不采样桌面像素、也从不写文件，所以每一条桌面路径照旧要走上面那套授权——没有 `--worker` 这个选项，
`--yes` 的任何规矩也都没有变松。

把限度说成限度：这份预算只在能打断的地方、以及靠停掉辅助进程起作用。原子写文件、被人停止读取的 stdout 管道、
无视取消请求的 WinRT 编码器都没有取消点，所以这三样是在开始之前拦一道、在结束之后再计时，而不是在调用中途抢占。
而且在 `Win10 19045` 上，`PrintWindow(PW_RENDERFULLCONTENT)` 是从 DWM 缓存的那张表面上渲染的，根本不发
`WM_PRINT`，所以一个卡在 `WM_PRINT` 里的窗口在那个系统上并不会把父进程拖住；真正会等目标线程的是不带这个 flag
的那次 `PrintWindow` 退路调用。别假定"卡死"这种场景在每个 Windows 版本上都碰得到——只要假定这个工具会在它的
期限内返回。

## 文件名占位符

用在 `--out` 的路径里，多张图靠它区分：

| 占位符 | 含义                                                                                                                                                                                                                                                |
| ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `%i`   | 序号，从 1 起（`--all` 多窗口、`--monitor all` 多屏）                                                                                                                                                                                               |
| `%h`   | 窗口句柄，形如 `0x001B0C48`；屏幕目标给 0                                                                                                                                                                                                           |
| `%p`   | 进程 ID；屏幕目标给 0                                                                                                                                                                                                                               |
| `%n`   | 窗口标题；屏幕目标给去掉 `\\.\` 前缀的设备名（如 `DISPLAY1`）。标题会被清洗成能用的文件名片段：非法字符换成 `_`、去掉尾部的点与空格、整段正好是保留设备名（`CON` / `NUL` / `COM1` / `LPT1` …）时加 `_` 前缀、按 80 个 UTF-16 码元截断且不劈开代理对 |
| `%d`   | 本地日期 `YYYYMMDD`                                                                                                                                                                                                                                 |
| `%t`   | 本地时间 `HHMMSS`                                                                                                                                                                                                                                   |
| `%%`   | 一个字面 `%`；其余 `%x` 原样保留两个字符                                                                                                                                                                                                            |

`--all` 的输出名里没有占位符时会自动追加 `_序号`，并发 `note.all_without_placeholder`。
占位符分不开目标时（只写 `%d`，或同一进程的两个窗口写 `%p`）不会被悄悄改名：整批名字事先算好，撞名就报
`io.output_collision`。`%d` / `%t` 用的是本批次那一次时钟，所以跨午夜的一批也全用同一个日期与时间。规划出的名字
按绝对路径、不区分大小写、逐码元比较（NTFS 就是这样看名字的）；字符串比较看不见的别名（8.3 短名、硬链接、目录
junction 与符号链接、UNC 与盘符两种写法）交给提交那一次原子操作判定，所以预检认不出的占用同样不会被静默替换。
`--out -` 不是路径：不展开、不补扩展名、不查碰撞，而且一次只交付一张图——占位符在这里没有任何作用，
命中多个目标又要写 stdout 时整批报 `cli.stdout_multiple_targets`+1（见《输出形式》的规则 5）。

## 文案语言

`--lang`（`-l`）选 `zh-CN` / `zh-TW` / `en` / `ja`，不给或给 `auto` 时用 Windows 显示语言，判出来不在这四种里就用
`en`。取值写得宽容：忽略大小写、`_` 与 `-` 等价，`zh_TW` / `zh-Hant` / `cht` / `tw` 走繁体，`chs` / `cn` /
`zh-Hans` 走简体，`jp` 走日语；写错在解析期报 `cli.unknown_language`（退出码 1），不退化成默认语言。

**只有给人看的文字随语言变**：诊断项的 `message` / `hint`、`--help` 全文。`code`、JSON 键名、取值枚举、
`0x…` 句柄、`HRESULT` 数值一律不变，调用方按 `code` 分支即可。文案是 exe 自带的嵌入资源
（`resources/strings-<语言>.txt` 按语言编成四份 `RCDATA`），所以离线也能切语言。

## 边界与未验证项

这里集中回答"这个工具**不**承诺什么"。各功能小节解释的是每条界线为什么存在；下面这份清单才是自动化调用方
应当当成残余风险的东西，而不是一条 bug。

**设计边界，不是缺陷：**

- 确认框就是一个普通的 `MessageBox`。它是协作式的误点防护：既分辨不出按下按钮的是人还是脚本，也不是操作系统的
  安全边界。它保证的是——照这套规矩走的调用方，每条桌面路径至少会被问上一次。
- 复核与取帧永远不是同一个原子操作。身份与拓扑的复核把 HWND 被换走的那道窗口缩小了，并没有把它关上：Windows
  没有可等待的句柄，也没有"钉住一扇窗口不让它消失"的公开 API。
- 期限预算只在能打断的地方、以及靠停掉辅助进程起作用。预算在原子提交过程中到期的那张文件照样落地——超时
  既不回滚也不删除。确认的限期是轮询出来的（约 3 秒关闭宽限），不是抢占。
- `cursorEffective` 只说得到"这个会话被设成画指针"或"这块来源里没有指针"这一层。本 SDK 的会话接口没有只读的
  `IsCursorVisible`，所以关于指针在像素上可见与否一句都不声称，也从不把光标画进帧里或从帧里抹掉。
- `--hdr tonemap` 交出的永远是 8 位 SDR（`encoderOutput: "sdr_bgra8"`）：本构建没有 HDR 原生或 10 位输出路线；
  而明确写出来的 `tonemap` / `refuse` 要求在本构建里只有那两条 `wgc` 路径兑现得了——duplication 那一面是
  `wide_gamut_unverified`，所以那里得到的是拒绝而不是猜。
- `duplication` 一次只取一块输出：横跨两块屏的窗口取的是它与重叠面积最大的那块输出相交的那一块，其余部分回来时
  带着 `clipped` 与 `note.capture_clipped`。把一扇窗口跨适配器拼起来没有实现。远程桌面与虚拟显卡经常一整帧都拿不到。
- DRM 与受保护内容恒为黑屏，某些播放器驱动级的黑框在有的通道前能活下来。单色结果只是报出来（`note.frame_uniform`）
  并照常交付，绝不被当成失败而拒绝。
- 声明的支持下限（内部版本 18362）与实测过的那套环境（内部版本 19045、x64）是两个不同的数，而
  `verifiedOnThisMachine` 拿本机比对的是后一个：那是一次环境匹配，不是逐台设备的实测记录。这个二进制在 Windows 7
  上根本装载不了，在 Windows 8.1 上装得了却没有编码器可用。

**照实记为未验证，而不是推导出来**（下面每一行都是那条测试在这台开发机上报出的 SKIP /"未验证"，没有一条被
声称是通过）：

| 本机判不了的                                                                                    | 原因，以及这条缺口记在哪儿                                                                                              |
| ----------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| 真 HDR 帧的实拍、真 HDR 帧上 `refuse` 的结局、FP16 帧池出图、HLG 的端到端                       | 没接 HDR 屏；映射数学离线判（`tests\hdr.ps1`、`build\ecapture-hdr-tests.exe`），`color.verifiedOnThisMachine` 恒为 `no` |
| HDR 配 `--scale`，以及 `--roi` / `--scale` 的跨屏混合 DPI                                       | 只接了一块屏，也没有 HDR 模式可开（`tests\scale.ps1`、`tests\crop.ps1`）                                                |
| 交付帧单边超过 16384 的真机现场；旋转过的面板；热插拔拔掉一台显示器                             | 造不出那么大一扇窗口，而测试绝不会去重排或旋转显示器（`tests\image.ps1`、`tests\dup.ps1`）                              |
| 刻意安排 `HWND` / PID 被回收，以及身份在真实确认框那一段的窗口期                                | 那意味着要结束别人的进程，而框的回答得由人来给（`tests\identity.ps1` 需要 `-SimulateConsent`）                          |
| 像素级"这张图里看得见/看不见指针"；低于内部版本 19041 的机器                                    | SDK 给不出像素级的答案，而这台机器比 19041 更新（`tests\cursor.ps1`）                                                   |
| 19045 以外的 Windows 版本 · ARM64 · Server · 远程桌面 · 没有可交互桌面的会话 · 真的缺某个编码器 | 这里安排不出第二套操作系统；这些判据改用注入的假版本在离线层判（`tests\compat.ps1`、`tests\capabilities.ps1`）          |
| `tiff` 与 `gif` 在 `--scale` 下的下场，以及其他只与编码器有关的行为                             | `tests\scale.ps1` 判的是 `png` / `bmp` / `jpeg`；那两种共用编码调用却没被这条判据覆盖                                   |
| `--capabilities` 用"真的产出一张图"来探测能力，以及确认框真的被显示出来过                       | 这两种探测就等于去动手做它只想报告的那件事（`encoder_state_not_probed`、`consent_dialog_state_inferred_not_probed`）    |

## 给 AI / 脚本的调用指南

工具就是为程序化调用设计的，按下面这套约定走最省事。项目里还带了一份教 AI 用它的 skill：
`.agents/skills/ecapture-screenshot/`（里面有 `SKILL.md`、`references/cli-contract.md` 和一份 exe 副本）。

1. **先问一次能力，再用 `--list` / `--inspect` 发现窗口，最后真截图。** `--capabilities` 是只读的：不取像素、不弹确认框、
   不写文件，也就不会打扰人，适合放在自动化流程的最前面。它把这台机器的版本、会话、屏幕拓扑、每条路线的
   `available` / `unavailable` / `unverified`、每种格式、`--yes` 到底管哪几条内部路径，以及取值上限一次交给你，
   于是"该用哪条 `--capture`""这次失败该换通道还是这台机器不行""这里弹框有没有人会答"都在动手之前就有答案。
   要提交问题报告就再跑一次 `--diagnostics`（同一批判据，另加可核对的构建标识；`-v` 展开每一问的原始答案）。
   两份文档都不随 `--lang` 变（全 ASCII），所以比对结果稳定。注意 `available` **不是**"这个窗口一定截得到"：
   驱动、受保护内容、HDR 都不在那层的断言里，文档末尾的 `caveats` 就是写来钉住这一点的。
   之后照旧 `--dry-run`：它不取帧、不写文件、也不弹确认框，候选在 `notes[0].value`：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。`--dry-run` 也不必给 `--out`
   （那一次没有图片要交付，结果整份在 stderr）；而**只给 `--dry-run` 不给任何窗口条件 = 文本帮助 + 退出码 2**。
   要的是**列表**而不是一行人话时，用 `--list`（结构化、分页，多匹配不算错误，一个都没命中是空列表 + 退出码
   `0`）与 `--inspect`（一扇窗口，多匹配仍算歧义 —— 它不会替你挑一个）。两条都不取像素、不弹框，`--yes`
   对它们也没有任何作用。它们交回的是快照：真去截图仍要复核目标身份，所以请把**刚做完的一次** `--inspect`
   里的句柄传给截图，而不是缓存早前那一轮的。详见上文《结构化的窗口发现与检查》一节。
2. **按 `errors[].code` 分支，不要匹配 `message` 文字**（那随 `--lang` 变），也不要拿"有没有给 `--out`"当原因——
   省略它那条路与 `--out -` 报的是同样的码。自动化调用方真正会撞上的那些码：

   | 码                                                                                                                                                                                                                                                                                                                                                                                                                                                                  | 退出码 | 下一步                                                                                                                                                                                                                                                                                           |
   | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
   | `cli.invalid_number`、`cli.invalid_value`、`cli.invalid_format`、`cli.unknown_option`、`cli.unknown_language`、`cli.crop_conflict`、`cli.query_conflict`、`cli.window_query_conflict`、`cli.monitor_conflict`、`cli.monitor_selector_empty`、`cli.monitor_selector_kind`、`cli.stdout_multiple_targets`                                                                                                                                                             | 1      | 改命令行——一个都没截、一个框都没弹、一个文件都没写                                                                                                                                                                                                                                               |
   | `match.index_out_of_range`、`match.monitor_out_of_range`、`match.roi_out_of_range`                                                                                                                                                                                                                                                                                                                                                                                  | 1      | `hint` 列了全部候选；请求的那块裁剪按选定那一刻的目标就是放不下                                                                                                                                                                                                                                  |
   | `match.no_window`                                                                                                                                                                                                                                                                                                                                                                                                                                                   | 4      | 条件太窄，或者目标是最小化窗口（最小化的窗口永远截不到）                                                                                                                                                                                                                                         |
   | `match.ambiguous_window`、`match.monitor_ambiguous_id`                                                                                                                                                                                                                                                                                                                                                                                                              | 5      | 命中的不止一个，而工具不替你挑——用 `--index` / `--topmost-match` / `--all` 消歧，或用 `--screens` 交回的标识                                                                                                                                                                                     |
   | `match.monitor_unknown_id`                                                                                                                                                                                                                                                                                                                                                                                                                                          | 4      | 那个标识此刻不在桌面上；再跑一次 `--screens`                                                                                                                                                                                                                                                     |
   | `capture.access_denied`、`capture.consent_timeout`、`capture.consent_unavailable`                                                                                                                                                                                                                                                                                                                                                                                   | 6      | 这是人的边界：回答不是"是"（这个"是/否"框上，人能给出的唯一拒绝就是答"否"）、限期之内没人回答、或者根本没有可交互的桌面。本次请求剩下的部分已经没有去尝试——再问一次是一个**新请求**，不是一次重试                                                                                                |
   | `match.timeout`、`capture.worker_failed`、`capture.failed`、`capture.frame_timeout`、`capture.window_gone`、`capture.frame_invalid`、`capture.roi_invalid`、`capture.roi_unmeasurable`、`capture.monitor_changed`、`capture.monitor_unverifiable`、`capture.monitor_id_unverifiable`、`capture.consent_stale`、`capture.timeout`、`capture.hdr_refused`、`capture.hdr_unverifiable`、`capture.target_gone`、`capture.target_changed`、`capture.target_unverifiable` | 7      | 先读 `stage`（`match` / `capture` / `encode`）再决定下一步；通常该重跑一次 `--list` / `--screens`，而不是换一条 `--capture`                                                                                                                                                                      |
   | `env.os_too_old`、`env.channel_unsupported`、`env.hdr_unsupported`、`env.cursor_unsupported`                                                                                                                                                                                                                                                                                                                                                                        | 7      | 是**这一台机器**给不出所要求的东西——见下面那段                                                                                                                                                                                                                                                   |
   | `io.write_failed`、`io.file_exists`、`io.output_collision`、`io.timeout`                                                                                                                                                                                                                                                                                                                                                                                            | 8      | 把目录建出来，或换一个撞不了的名字；`io.file_exists` 是 `--no-overwrite` 正在尽职；这里的 `io.timeout` 指的是那一步根本没开始的写——如果一张文件**已经落地**、只是写完之后的期限复核越了线，那张图仍留在 `images` 里、本次按部分成功报告退出码 `7`（已交付 + 出错），见《执行期限与会阻塞的调用》 |
   | `capture.hdr_unsupported`、`capture.cursor_unsupported`、`capture.unsupported`                                                                                                                                                                                                                                                                                                                                                                                      | 1      | 这一对选项在解析期就被拒了，在任何确认框之前                                                                                                                                                                                                                                                     |

   那四个 `env.*` 说的是这台机器而不是那个目标，所以重试同一个窗口毫无意义：`env.os_too_old` 是本机 Windows 内部版本
   低于所有格式共用的那唯一一套编码器所在的下限（换 `--capture` 也不会变好），`env.channel_unsupported` /
   `env.hdr_unsupported` / `env.cursor_unsupported` 是你明确写出的那道要求，这个构建加这套系统满足不了——工具不会
   为了把报错消掉而替你换一条通道。`note.channel_unavailable` 说的是 `auto` 链里被去掉的那一条，而剩下的几条仍然
   可能截成交功。先问一句"这台机器给得出哪几条通道"不必截图：`--verbose` 的 `input.osBuild` 与
   `input.captureChain` 就是它。那几道下限与实测范围见[系统支持](#系统支持)。
   每条错误还带 `target` / `backend` / `stage` / `hresult` / `win32`（见上面的输出规则），拿到多少写多少，
   不必从 `message` 里抠。

3. **读流要分情况**：给 `--out <文件>` 时 JSON 在 stdout、stderr 是空的，直接解析就行；用 `--out -` 或没给输出路径时
   图片字节占了 stdout，JSON 整体改到 stderr。stdout 一次只交付一张图，多个目标请写到文件。在 shell 里也要把两条流
   分开，并知道自己用的 shell 会对字节做什么：`cmd` 与 PowerShell 7.4 起保留原生命令 stdout 的字节流，
   Windows PowerShell 5.1 不保留（stdout 按文本重新编码，原生 stderr 还要经它自己的错误记录渲染），所以从 5.1 调用
   就请要一个文件，或用 `Start-Process -RedirectStandardOutput` / `-RedirectStandardError`，或把命令放进 `cmd /c` 里跑。
   对图片而言 `2>&1` 永远不是答案。判据与实测数字见
   [标准输出图片字节的 shell 差别](#标准输出图片字节的-shell-差别)。
4. **别把非 0 退出码当全盘失败**：部分成功时 `captured` 大于 0 而退出码是 7，已经落地的图照样可用；
   `images[].source` / `path` / `scope` 分别告诉你那张图出自哪条通道、走了哪条内部路径、像素是窗口自己的还是屏幕上的。
5. **退出码 0 不等于画面是对的**：受保护内容、某些播放器驱动会在成功返回的同时给你黑帧。工具自己
   会告诉你整幅是不是只有一个颜色——它把每个像素与左上角那个逐字节比过（BGRA 四个通道都算，行末
   填充不算），确实单色就发 `note.frame_uniform`（颜色写成 `0xAARRGGBB`）而图片照常交付。单色只是
   质量提示，不是失败：纯色窗口、单色壁纸本来就是这个样子。要判正确性还得校验像素——
   比如把一个纯色窗口盖住目标再截，看拿到的是目标内容还是遮挡物；至少比对 `width`/`height` 与目标窗口矩形。
6. **默认会被弹框打断**：不给 `--yes` 时，任何真实截图（连只截一个窗口也算）都会阻塞到有人回答为止。目标是一个
   窗口、而且走的是窗口内容路径（`wgc` / `printwindow` / `dwm.thumbnail`）时才适合加 `--yes`；对 `bitblt`、
   `duplication`、`dwm` 的屏幕退路以及任何整屏，这个开关一点作用都没有，一定要人答——调用前先告知用户，并显式给
   `--out`。事后读 `images[].scope`：写成 `desktop` 就意味着图里可能混进别的窗口、打开的文档和通知。
   只想截某个窗口，就别把目标升格成整屏。
7. 想稳定拿到"某个应用"，优先用 `--process`/`--exe` + `--class`；标题匹配区分大小写，跨语言环境不可靠。
8. **给自动化留一条退路**：调用方加一个 `--timeout-ms`（比如 5000），免得一个卡死的目标窗口把你也一起吊住。预算
   用尽会给你 `match.timeout` / `capture.timeout` / `io.timeout` 而不是整趟挂死；确认框在 `--consent-timeout-ms`
   之内没人回答就是拒绝（`capture.consent_timeout`、退出码 6）。看到 `capture.timeout` 且 `backend=printwindow` /
   `dwm`，多半是目标的 UI 线程卡住了，请改用 `--capture wgc` 或放宽预算。

## 构建与测试

| 命令                        | 这条测什么                                                                                                                                                                                                                                                                                                                                          |
| --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `.\build.ps1`               | Release 构建，产物 `build\ecapture.exe`；`-Config Debug`、`-Clean` 可选                                                                                                                                                                                                                                                                             |
| `.\tests\cli.ps1`           | 输出契约本身：解析期错误与退出码、`--yes` 与 `--no-overwrite` 的每种布尔写法、查询与截图选项互斥那几组、双流分离、省略 `--out` 与 `--out -` 的等价对拍，以及四种文案语言。一律只跑 `--dry-run`——不截图、不写文件                                                                                                                                  |
| `.\tests\windows.ps1`       | `--list` / `--inspect`：分页、可见性策略、逐字段的可读性、文档形状、隐私、歧义，以及这条查询真的什么都没碰（离线层 `build\ecapture-windows-tests.exe`，真机层用自建窗口）                                                                                                                                                                           |
| `.\tests\screens.ps1`       | `--screens` 与 `--monitor=device:` / `id:`：每种身份各稳到哪一层、交回来的选择器真的选到那一块面板，而认不出的标识绝不会悄悄退化成主屏                                                                                                                                                                                                              |
| `.\tests\capabilities.ps1`  | `--capabilities` / `--diagnostics` 对着一组假探针跑（一块屏都没有、正好低于某条下限、版本问不出来、某个编码器没登记、`--yes` 的适用范围对上登记表、两份查询出自同一批判据）；真机一层判这条查询不会卡在框上、不落地、与 WMI 及 `--dry-run -v` 各自问来的值同源，且全 ASCII 所以 `--lang` 改不动它                                                   |
| `.\tests\streams.ps1`       | 标准流与结构化结果的可靠性：单目标写 stdout 时图与 JSON 各归其位、算出多个目标的整批在弹框之前就被拒、判据用的是实际命中的目标数、批次中途失败时已经截到的图留在 `images`、结果送不到约定那条流就是退出码 `8`，以及省略 `--out` 与显式 `--out -` 在成功 / 无匹配 / 歧义 / 非法参数 / 后端失败 / 被拒绝 / 写入断管七个场景上的对拍（只截自建的窗口） |
| `.\scripts\check-lang.ps1`  | 四语文案的 key / 占位符对齐检查，并确认 exe 里真编进了四份资源                                                                                                                                                                                                                                                                                      |
| `.\tests\invoker.ps1`       | 共享的那个测试进程调用器本身：argv 引号规则、两条流同时排空、二进制不被转码、卡死的子进程、每次运行各自的临时目录                                                                                                                                                                                                                                   |
| `.\tests\orchestration.ps1` | 一键总跑 `test-all.ps1` 本身：筛完是空计划要明说不算验收，计划内套件整套没跑成（脚本不见了、源码解析不干净）会让退出码判失败；刻意筛掉的、`-StopOnFail` 主动停下的剩余与各套件自己记的 SKIP 分开                                                                                                                                                    |
| `.\tests\build-path.ps1`    | 在含中文、空格、括号与 `%` 的目录里（外加中文 `%TEMP%`）构建，以及临时批处理的正文始终只能是 ASCII                                                                                                                                                                                                                                                  |
| `.\tests\smoke.ps1`         | 端到端：截自己建的测试窗口，校验 PNG 尺寸与像素内容                                                                                                                                                                                                                                                                                                 |
| `.\tests\image.ps1`         | 帧形状校验与像素操作，对着手工摆出的排布逐条判（竖条纹 / 棋盘 / alpha / 行末填充 / 超限与短缓冲区 / 越界裁剪），真机一层再判单色窗口的质量提示与来路                                                                                                                                                                                                |
| `.\tests\wgc.ps1`           | WGC 的实时尺寸：内容尺寸对着纹理尺寸、帧池重建，以及绝不把被裁掉的帧当成整窗交付（离线层 `build\ecapture-wgc-tests.exe`，真机一层自己改窗口大小）                                                                                                                                                                                                   |
| `.\tests\dup.ps1`           | 桌面复制：四种旋转对着生产几何判据各判一次、一张伪造的双适配器输出表、真实图片上的 `requestedRect` / `capturedRect` / `clipped` / `rotation`，以及"那台显示器在确认之后变了"那几种情形。测试绝不重排或旋转显示器                                                                                                                                    |
| `.\tests\compat.ps1`        | 那几道版本下限：把假的 Windows 内部版本注入生产能力判据本体，判被挡下的显式通道绝不会被顶替、`auto` 链少的是哪一条，以及发布版二进制确实导入了 Windows 8 那个年代的 WinRT / job API Set 契约                                                                                                                                                        |
| `.\tests\save.ps1`          | 文件交付：每种 `--no-overwrite` 布尔写法对着真实文件的效果、整批输出名规划与撞名检测、原子提交对着被占用的目标 / 目标是目录 / 目录不存在 / 写到一半被硬杀，以及并发的禁止覆盖竞态                                                                                                                                                                   |
| `.\tests\channels.ps1`      | 六条通道对着自己建的窗口跑，外加遮挡对照。窗口内容那几条带 `--yes` 跑，一旦弹框就判失败；`bitblt` / `duplication` 的画面判据要 `-SimulateConsent` 才跑                                                                                                                                                                                              |
| `.\tests\consent.ps1`       | 那张授权分级表，以及一次拒绝是怎么传播的：离线一层对着注入的假应答器把 `ConsentGate` 整台状态机跑完，真机一层把所有确认框一律代答"否"——哪些路径必须弹、拒绝之后报什么、有没有落地。绝不代人答"是"                                                                                                                                                 |
| `.\tests\isolation.ps1`     | 归属：同名的既有进程保持存活且绝不被当成目标、并发两轮互不串、被中断的一轮只清理自身                                                                                                                                                                                                                                                                |
| `.\tests\identity.ps1`      | 两档身份复核按各自发生的顺序问哪几问、`capture.target_gone` / `capture.target_changed`，以及 Z 序选择（`--topmost-match` / `--bottommost-match`）                                                                                                                                                                                                   |
| `.\tests\hdr.ps1`           | `--hdr`：DXGI 格式与显示器 color space 的分类（认不出的一律留 `unknown`、不猜）、tone 曲线的那几条性质、`ConvertWideFrameToSdrBgra8` 逐点判、那组结果键，以及本机诚实的 SDR 现场（没写这条选项时那些键不出现，对 SDR 帧提出处理要求时留一条 `note.hdr_source_sdr`）                                                                                 |
| `.\tests\cursor.ps1`        | `--cursor`：那张按路径登记的能力表、回退链在 19041 那道门槛两侧各怎么收窄、`cursorRequested` / `cursorEffective` / `cursorBasis` 是怎么合成的，以及一条源码级扫描——`src/` 里一旦出现在取指针形状、把光标画进帧里、动使用者鼠标那类调用就判红                                                                                                      |
| `.\tests\crop.ps1`          | `--roi` / `--client-area`：几何对着三条独立问出来的 Win32 事实判、也对着像素内容判，越界的请求在弹框与写文件之前就被挡掉，以及桌面路径上哪怕只裁一小块照样一定弹框                                                                                                                                                                                  |
| `.\tests\scale.ps1`         | `--scale`：绝不放大、取最紧的那条天花板、向下取整、最近邻映射逐点对拍、先裁后缩的顺序（`scaleFromWidth` / `scaleFromHeight` 说的是裁完那张），以及 `png` / `bmp` / `jpeg` 写下的都是缩过的尺寸                                                                                                                                                      |
| `.\tests\timeout.ps1`       | 期限与辅助进程：一份后续步骤只能花掉剩余部分的预算，辅助进程的线路协议对任何不合形状的回包一律拒绝、而不是"看起来像成功"，以及那个辅助模式拒绝被当成公开选项启动                                                                                                                                                                                    |
| `.\tests\screen.ps1`        | 整屏截图对着三条桌面路径跑（每一条都必须弹框），外加红块定位与阴性对照。只有 `-SimulateConsent` 会代答确认框，而且只该在专门腾给测试的桌面上这么用                                                                                                                                                                                                  |
| `.\tests\window_shot.bat`   | 给人走的流程：编译一扇测试窗口 → 逐通道截图（框由人自己点）→ 整屏那一步                                                                                                                                                                                                                                                                           |
| `.\scripts\mkreadme.ps1`    | 用各语言 `--help` 的原样输出重生成四份 README 的帮助段；`-Check` 只判不改写，过期的段落就是这样被抓出来的                                                                                                                                                                                                                                           |

所有桌面测试的目标窗口一律是自家的：`tests\helper\ec_window.cs` 编到本次运行的临时目录里，测试握着它的
PID 与 HWND，因此既不按进程名去找目标、也不按进程名批量收尾，删除的也只有自己建的那个目录。
这些测试要拿窗口图时一律带 `--yes`——那是唯一不会弹框的情形；桌面路径只在 `-SimulateConsent`（专门腾出的桌面）
或 `tests\window_shot.bat`（人自己点）里才真跑。
`tests\harness.psm1` 放着共享的进程调用器（argv 引号规则、两条流并发消费、有期限的等待、超时只结束自己
那棵进程树），以及临时目录与测试窗口的建立和收尾；`tests\invoker.ps1` 就是拿来证明这个调用器本身的。

`build.ps1` 用 vswhere 定位 VS，并优先使用 VS 自带的 cmake/ninja。仓库目录、build 目录与工具链路径只经子进程的
环境块递给那个临时批处理，正文里一个绝对路径都不写，所以仓库放在含中文、空格、括号或 `%` 的目录里也能照常构建，
而正文一旦混进非 ASCII 会在写盘前被当场拦下（判据见 `.\tests\build-path.ps1`）。构建要求 `/W4` 下零警告。
在 Git Bash 里手工测试要先 `export MSYS2_ARG_CONV_EXCL='*'`，否则 `/help` 会被当成路径改写、`--out /tmp/x.png` 会被转成怪路径。

## 许可

EvernightCapture 采用 [木兰宽松许可证第 2 版（Mulan PSL v2）](http://license.coscl.org.cn/MulanPSL2)，
中英双语全文见 [LICENSE](LICENSE)。

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
