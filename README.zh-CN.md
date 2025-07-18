<div align="center">

![EvernightCapture 图标](resources/icon.ico)

# EvernightCapture

命令行窗口截图工具：按条件筛出窗口，把那个窗口的画面存成图片文件。

[English](README.md) · [简体中文](README.zh-CN.md) · [繁體中文](README.zh-TW.md) · [日本語](README.ja-JP.md)

</div>

基于 Windows.Graphics.Capture 的一整套取图通道，入口是 `ECAPTURE.EXE`——单个可执行文件，静态链接 CRT，
目标机器不需要装 VC++ 运行时。输出对程序友好：除 `--help`/`--version` 外一律 JSON，退出码稳定，
诊断带稳定的 `code`，所以既适合人敲，也适合被脚本和 AI 调用。

当前版本 **0.4.0**：`--capture` 的取值全部可用（`wgc` / `dwm` / `printwindow` / `bitblt` / `duplication` / `auto`），
`--monitor` 提供整块屏幕截图与"按屏过滤窗口"。曾实现过的 `magnification` 已删除（理由见 AGENTS.md）。

## 特性

- **按条件筛窗口**：句柄 / 进程 ID / 映像名 / 完整路径 / 标题（精确、包含、正则）/ 窗口类名，不同选项之间 AND、同一选项写多次 OR
- **六条取图通道**：被遮挡的窗口也能截（`wgc` / `dwm` / `printwindow`），或者故意只拷屏幕上可见的像素（`bitblt` / `duplication`）
- **多窗口一次截完**：`--all` 每个命中窗口各存一张，配合 `%i` 之类占位符命名
- **截图授权**：凡是真要取帧的截图，连可靠的窗口通道也一样，先弹模态确认框；`--yes` 只免掉"帧绑在所选窗口本身、
  不从桌面采样"那一条层的确认——任何会拍到桌面像素的路径一定要人答，没有开关能跳过
- **兜得住的超时**：`--timeout-ms` 是自动处理那一段（条件匹配、后端重试、等帧、编码、写文件）共用的同一份预算，
  `--consent-timeout-ms` 单独给确认框计时；`PrintWindow`、DWM 回读、正则求值这些要等别的进程的调用，都跑在一个
  工具能停下来的辅助进程里，所以卡死的目标窗口再也卡不住这个工具
- **四语文案**：`zh-CN` / `zh-TW` / `en` / `ja`，默认跟随系统显示语言，全部编在 exe 的资源里
- **机器读的 JSON**：只装捕获结果与错误，不带工具名、版本、schema、参数回显之类的元信息

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

# 图片字节走标准输出（此时 JSON 改走 stderr）
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
  --monitor, -m [<n|primary|all>] 截图目标屏的编号，从 1 开始（按显示设置里的顺序）；primary = 主屏，all = 每块屏各一张。不给窗口条件时 = 整块屏幕截图，给窗口条件时 = 只算与该屏有重叠的窗口。取值可省略（= 主屏），省略时不吃后面的参数，所以 --monitor out.png 仍然可用。整块屏幕拍的是桌面像素，一定要先弹框问人，--yes 也跳不过；按屏过滤窗口出的仍是窗口图

窗口匹配条件（同一选项多次出现取并集，不同选项必须同时命中）
  --hwnd <handle>                 窗口句柄。纯数字按十进制，0x 前缀或含 a-f 按十六进制；推荐写 0x
  --pid <pid>                     进程 ID，十进制且大于 0
  --process, -p <image-name>      映像文件名（不含路径），忽略大小写；无扩展名时按 .exe 处理
  --exe <full-path>               映像完整路径，忽略大小写
  --title, -t <exact-title>       窗口标题精确匹配
  --title-contains, -T <text>     窗口标题包含子串
  --title-regex, -R <regex>       窗口标题正则匹配，ECMAScript 语法，解析期即校验
  --class, -c <class-name>        窗口类名，忽略大小写，如 Notepad / CabinetWClass

匹配到多个窗口时（互斥）
  --index, -i <n>                 取第 n 个窗口，从 1 开始，按可见性/叠放次序排序
  --newest                        取最后创建的窗口
  --oldest                        取最早创建的窗口
  --all, -a                       每个匹配窗口各存一张

取图方式（默认 wgc；受系统版本或窗口性质限制时会失败）
  --capture, -C <method>          wgc(默认，被遮挡也能截) / dwm(DWM 缩略图，被遮挡也能截) / printwindow(窗口自绘) / bitblt(拷屏幕可见像素) / duplication(桌面复制后按矩形裁) / auto(按 wgc-dwm-printwindow-bitblt 回退；整屏截图只用 wgc-duplication-bitblt)。只取窗口自己的画面：wgc / printwindow / dwm 缩略图；会从屏幕上取样：bitblt / duplication 与 dwm 的屏幕退路

截图授权（真实截图默认都要先弹框问一次；--yes 只免掉只取窗口画面的那条路径）
  --yes, -y                       跳过"只取所选窗口画面"那条路径的确认框。不保证目标一定有画面，也不忽略权限、受保护内容、错误或覆盖保护；任何会从屏幕上取样的路径（bitblt、duplication、整屏任何通道、dwm 的屏幕退路）一定会弹框，这个开关跳不过。写 --yes=false 表示明确要问

执行期限（自动处理那一段的总预算；人工确认另算，到点按拒绝处理）
  --timeout-ms <ms>               自动处理阶段的总预算（毫秒）：从选定目标开始，条件求值、后端重试、取帧、编码、写文件共用这一份剩余时间，任何一步都不会重新领一份完整预算。不给或写 0 = 不设总预算，此时每次隔离调用仍受内置上限（5000 毫秒）约束。人工确认的等待不算在这里，见 --consent-timeout-ms。预算用尽时那一张不落地，按阶段报 match.timeout / capture.timeout / io.timeout
  --consent-timeout-ms <ms>       确认框最多等多久（毫秒）。不给或写 0 = 一直等人回答。到点按"拒绝"处理，绝不按"默认同意"处理，报 capture.consent_timeout。这段等待单独计时，不占 --timeout-ms 那份自动预算；点"是"之后那约 1 秒的关闭动画缓冲也算在这一级，不会为了赶期限而省掉

输出
  --out, -o <path|->              输出路径；特殊值 - 表示把图片字节写到标准输出。也可用位置参数；完全不给时等同 --out -。整批输出名在取帧之前一次算好，两个目标算出同一个名字时整批报错，不会静默覆盖。标准输出一次只能交付一张图，命中多个目标时整批报参数错误、一张都不截
  --format, -f <name>             强制编码格式；不给则由输出文件扩展名判定，扩展名也判不出时用 png
  --quality <1-100>               JPEG 质量，默认 100
  --no-overwrite                  目标已存在时不覆盖，报错退出（不给取值就是禁止覆盖）；写 --no-overwrite=false（0 / no / n / off）取消这条禁令，=true / 1 / yes / y / on 与不给取值同义。重复给出时最后一个生效

其它
  --dry-run, -d                   只解析并列出候选窗口，不截图不写文件
  --json, -j                      已废弃的兼容开关，无副作用：成功与错误本来就输出 JSON
  --verbose, -v                   JSON 中追加 input 段（规范化后的全部输入），并保留 notes
  --quiet, -q                     省略 notes；errors 无论如何都会返回
  --lang, -l <language>           文案语言。auto(默认，跟随系统显示语言) / zh-CN / zh-TW / en / ja；系统语言不受支持时用 en
  --help, -h                      输出文本帮助（本段）
  --version                       输出版本与阶段

写法: --opt=value / -opt / /opt 都接受；取值本身以 - 开头时写成 --title=-x，或用 -- 结束选项解析
输出: 成功与错误都是 JSON，只含 captured / images（另有 errors / notes，--verbose 才有 input）
      --help / --version 以及不给条件时是文本
退出码: 0 成功 / 1 参数错 / 2 未给条件 / 3 --help / 4 无匹配窗口 / 5 匹配多个窗口 /
        6 目标受保护或被拒绝 / 7 截图失败 / 8 写文件失败 / 9 内部异常
当前构建: --capture 的取值全部已实现（wgc / dwm / printwindow / bitblt / duplication，auto 按 wgc-dwm-printwindow-bitblt 回退，整屏目标按 wgc-duplication-bitblt）；输出目录必须已存在

示例:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 报告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
  ECAPTURE.EXE --monitor all D:\shots\screen_%i.png
  ECAPTURE.EXE --process notepad.exe --yes D:\shots\epad.png
  ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --consent-timeout-ms 60000 D:\shots\epad.png
```
<!-- END ECAPTURE-HELP -->

## 匹配语义

不同选项之间是 AND（都满足才算命中同一个窗口），同一选项写多次是 OR，不会跨窗口拼接条件。

```powershell
ECAPTURE.EXE --process notepad.exe --title-contains 报告 D:\shots\r.png
# 进程是 notepad.exe 且标题含"报告"的那些窗口
```

- `--title` 是整串相等、`--title-contains` 是子串匹配，两者**区分大小写**；`--class` / `--process` / `--exe` 忽略大小写。
- 枚举默认跳过不可见窗口和零尺寸窗口；**最小化的窗口截不到**，只在 `hint` 里单独说明。
- 命中多个又没给消歧选项时不会随便挑一个，而是报 `match.ambiguous_window`（退出码 5），
  `hint` 里按叠放次序列出全部候选。

## 输出形式

`--help`、`--version`、以及不给任何条件时是纯文本。其余一律 JSON，只装捕获结果与错误。

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
两种图都带 `source`，写的是真正出图的那条通道（`--capture auto` 回退成功时它是链上命中的那一条，不是 `auto`）；
也都带 `path` / `scope` / `rect`——整屏那种是 `screen.wgc` / `screen.bitblt` / `screen.duplication`，
`scope` 为 `desktop`；而 `--monitor` 配窗口条件出的仍是窗口图，`scope` 为 `window`。

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
      "hint": "纯数字按十进制解析；十六进制请写成 0x……，或含 a-f 时自动按十六进制"
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
   `window` 或 `desktop`）与 `rect`（那条路径被授权取样的屏幕区域；量不出来时才省略）。
3. `code` 值稳定：`cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`，只增不改名。
   取帧失败里"帧超时"（`capture.frame_timeout`）与"窗口已经没了"（`capture.window_gone`）各有自己的码，
   不再和一般的 `capture.failed` 混在一起——两者的下一步动作不同（前者可以等一会儿重试，后者要重新枚举）。
   帧自己的内存形状说不通（宽高为 0、单边超过 16384 像素、行距装不下一行像素、缓冲区比行距×高还短）时给
   `capture.frame_invalid`（退出码 7）；裁剪、行重排与编码都先核这一道，坏帧不会被往下搬。
4. 通道：默认全部写 stdout、stderr 保持空；一旦图片占用标准输出（显式 `--out -`，或根本没给输出路径），
   JSON 整体改走 stderr，两个通道永不混流。连渲染结果本身都出异常时的兜底诊断也一律走 stderr（那时
   无法确定图片是否已经占了 stdout）。**结果送不到约定那条流就是失败**：退出码变成 `8`，即使另一条流
   写成功也不改回原来的值——调用方按约定流读，读不到就是没拿到。
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
   `io.output_collision`（退出码 8），整批一张都不截、一个文件都不写 —— 既不替调用方改名，也不让第二张盖掉第一张。
   每张图先写目标目录下唯一的临时文件，写全并刷新之后才改名成目标名，所以写失败不会清空也不会删掉旧文件。
   `--no-overwrite` 时“目标在不在”由那一次不许替换的改名当场判定（`io.file_exists`），不做有竞态的预检。
8. 每一步的失败诊断还带着它自己的坐标，只在这一步真拿到了值时才出现：`target`（哪个目标，窗口是
   `0x…` 句柄、屏幕是设备名）、`backend`（哪条通道）、`stage`（`consent` / `capture` / `encode` / `write` /
   `stdout`）、`hresult`（`0x80070005` 这样的原值）、`win32`（`GetLastError` 的原值）。`message` 随 `--lang`
   变，这几个不变。授权这一关的错自成一类：`capture.access_denied` 是有人答了"否"或把框关掉，
   `capture.consent_unavailable` 是这台机器根本没有可交互的桌面、框弹不出来——两者都是退出码 `6`，都带
   `stage=consent`、`target`、`backend`（通道）和 `value`（那条路径），也都不是技术性的访问被拒
   （`capture.failed` 带 `hresult=0x80070005`），这样才分得开。确认之后目标挪了位置或变了大小给
   `capture.consent_stale`，`stage=capture`、退出码 `7`，重新选目标再截就会再问一次。
   黑帧不会被断言成 DRM——文案只列出几种可能。
   单色帧也不会被断言成"没截到"：图照常交付，另外留一条 `note.frame_uniform`（把那个颜色、那条通道、
   那个目标写清楚）。只有 `duplication` 还会因为单色拒绝一帧，而且必须两条一起成立——这一帧没有任何
   present 记录，且整幅只有一个颜色。

## 退出码

`0` 成功 / `1` 参数错 / `2` 未给条件 / `3` `--help` / `4` 无匹配窗口 / `5` 匹配多个窗口 /
`6` 目标受保护、在确认框上被答"否"、在 `--consent-timeout-ms` 内没人回答、或框根本弹不出来 /
`7` 截图失败，含 `--timeout-ms` 预算用尽 / `8` 写文件失败，含在写文件或标准输出阶段预算用尽 / `9` 内部异常。
新增语义只会追加编号。
`8` 也覆盖"结果 JSON 送不到约定那条流"（写 stdout / stderr 失败），那种情况下另一条流上补发的文字不算交付。

退出码与 body 是两套独立信号，`2`/`3`/`4`/`5` 是正常控制流而不是崩溃。**允许部分成功**：`--all` 或
`--monitor all` 里某些目标失败时，已写出的图仍在 `images` 里（`captured` 可以大于 0），但退出码是 `7`。
某个后端崩了（抛异常而不是返回失败）也只作废它所在的那一个目标：前面的图留着，这条失败以 `capture.failed`
带在 `errors` 里。内存耗尽、显卡设备被移除这类换后端也不会有区别的错误会明确终止整批，而不是一条条试下去。
访问被拒不是继续回退的理由，被人拒绝也不是：一旦有人答"否"（或这个会话根本弹不出框），本次请求剩下的目标
一律不再尝试——不换后端、不重试、不再问第二遍，之前已经完成的图全部留着。

## 取图方式

| 取值 | 通道 | 能截被遮挡窗口 | 硬件加速内容 | 平台下限 |
| --- | --- | --- | --- | --- |
| `wgc` | Windows.Graphics.Capture | 能（DWM 缓存） | 正常 | Win10 1803+ |
| `dwm` | DwmRegisterThumbnail | 能 | 多数正常，受保护窗口黑 | Win7+ |
| `printwindow` | PrintWindow + PW_RENDERFULLCONTENT | 能（窗口自绘） | 常常全黑 | Win8.1+ |
| `bitblt` | BitBlt 屏幕 DC | 不能，只拷可见像素 | 部分黑 | 全版本 |
| `duplication` | DXGI 桌面复制整屏帧后按矩形裁 | 不能，只拷可见像素 | 正常 | Win8+，远程桌面/虚拟显卡常拿不到内容 |
| `auto` | 按 wgc → dwm → printwindow → bitblt 回退 | 尽量 | 尽量 | — |

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

## 截图授权与 --yes

凡是真要取帧的截图，都先弹一个模态确认框，**可靠的窗口通道也一样**。不弹框也不截的只有这些：没给任何条件
（文本帮助 + `2`）、`--help`、`--version`、`--dry-run`、无匹配（`4`）、匹配多个（`5`）、解析期参数错（`1`）、
以及输出名规划失败（例如 `io.output_collision` + `8`）——整批名字在任何一次发问之前就算完了。

`--yes`（`-y`，正向布尔开关：裸写或 `=true/1/yes/y/on` 是开，`=false/0/no/n/off` 是关，重复给出最后一个生效，
`-v` 的 `input.yes` 回显最终结果）只免掉**一层**确认：帧绑在所选窗口本身、绝不从桌面采样的那些路径。
它别的一概不保证：不保证图是有效的、不忽略权限、不管受保护内容、不管错误、也不管覆盖保护。
决定属于哪一层的是实际走的那条路径，不是通道名：

| 路径（`images[].path`） | 像素从哪来 | 不给 `--yes` | 给了 `--yes` |
| --- | --- | --- | --- |
| `wgc`、`printwindow`、`dwm.thumbnail` | 只有所选窗口自己 | 问一次 | 不问 |
| `dwm.screen`、`bitblt.screen`、`duplication.frame` | 那个窗口所在的那块屏幕区域 | 要问 | **照样要问** |
| `screen.wgc`、`screen.bitblt`、`screen.duplication` | 整块屏幕 | 要问 | **照样要问** |

判不出来或没登记的路径一律按桌面路径处理，所以新增通道忘了登记只会更严不会更松。`--monitor` 与窗口条件同时
给出时筛的是**窗口**，出的仍是窗口图，按上面窗口那两行走。

- **桌面路径没有任何旁路**：`--yes`、`--quiet`、环境变量、stdin、调用者是谁，都跳不过它——分级就是为了这一件事。
- 一次确认可以覆盖本次请求里明确列出的那一批目标，所以多个后端、多个窗口不会各问一遍。但授权绝不跨请求缓存，
  也不会扩大到框上没列出的目标，"同意截窗口画面"更不等于"同意截桌面"：`--capture auto` 配 `--yes` 可以不打扰人
  地走完窗口那几条，可一进桌面路径就必须再问一次。
- 一旦有人答"否"、框被关掉、或者根本没有可交互的桌面，本次请求剩下的截图就停了：不换后端、不重试、不再问第二遍，
  已经完成的图留在 `images` 里。
- 目标区域挪动过、或者屏幕拓扑变了，覆盖它的授权当场作废并重新问一次；已经绑在旧区域上的那一帧给
  `capture.consent_stale`（退出码 `7`，重新选目标再截）。
- 确认框的默认焦点在"否"上，内容列出目标及其区域、将要走的那条路径、每张图展开后的绝对路径（或"标准输出"）、
  以及画面里会不会混进别的窗口；这条路径会拍到桌面时，框上还明写着"你给的 `--yes` 对它不生效"。框在取帧之前就
  已经关闭，所以不会出现在图里；点"是"之后工具仍要等约 1 秒，因为关闭动画还在 DWM 的画面上。
- 没有可交互桌面时（服务会话、计划任务、锁屏），带 `--yes` 的窗口内容截图照旧正常完成，而桌面路径只能被拒绝——
  绝不会因为"弹不出框"就放行。答"否"给 `capture.access_denied` + `6`，弹不出框给新的稳定码
  `capture.consent_unavailable` + `6`；两者都带 `stage=consent`、`target`、`backend`，`value` 写的是那条路径。
- 被拒绝**不再**塌成 `cli.missing_output`，所以就算没给输出路径，调用方也看得见"是人拒了"。这条偷懒路径上其余
  失败照旧一律塌成 `cli.missing_output` + 退出码 1、真实原因不外泄；另一个例外还是 `cli.stdout_multiple_targets`
  ——多个目标要共用同一条 stdout 本来就是参数错，报成缺少输出路径只会把人引向补 `--out`。
- 多块屏 + 写 stdout（`--monitor all --out -`）在任何弹框之前就被拒：一次确认换不来"每块屏一张图挤进同一条流"。
  只有一块屏时 `--monitor all` 是一个目标，那条路仍然按单张走 stdout。
- 说清楚边界：这就是一个 `MessageBox`。它是给合作式自动化（人或 AI）准备的误点防护，既证明不了按下按钮的是人，
  也挡不住同一个权限级别里存心绕过的进程。它能保证的是：照这套规矩跑的调用方，一定会被问上一次。

`--monitor`（省略取值）与 `--monitor primary` 是主屏，`--monitor 2` 是第 2 块屏，`--monitor all` 每块屏一张。
编号按 `EnumDisplayMonitors` 的顺序、从 1 起；越界报 `match.monitor_out_of_range`（退出码 1），`hint` 里列出本机全部屏幕。
`--monitor <n>` 与窗口条件同时给出＝按屏过滤窗口（窗口矩形与该屏有重叠即命中，跨屏窗口在两块屏上都算），
出的仍是窗口图，所以按上面窗口那两行的规矩授权。`--monitor all` 与任何窗口**匹配**条件互斥
（报 `cli.monitor_conflict`，退出码 1），但 `--all` / `--index` 这类消歧选项不算匹配条件，可以和它搭配。

## 执行期限与会阻塞的调用（`--timeout-ms` / `--consent-timeout-ms`）

`--timeout-ms <ms>` 是本次运行自动处理那一段的**总预算**，从开始选目标那一刻起按单调时钟计时。窗口/屏幕匹配
（含 `--title-regex`）、`auto` 回退链、等帧、编码、最后那次提交，花的都是**同一份**预算：没有任何一步、也没有
批次里任何一个后续目标能重新领一份完整预算，所以四条后端不可能各等 2 秒、两个目标也不可能各再等一遍。不给或写
`0` 就是不设总预算；即便如此每次隔离调用仍受一个内置的 5000 ms 上限约束——这本是旧的 `timeoutMs` 参数该做到的事。
预算用尽时，受影响的那张图**不会**写出——条件求值阶段把预算耗尽给 `match.timeout`（`stage=match`），取帧与编码
阶段给 `capture.timeout`（`stage=capture`，编码也算在这一级），写文件/标准输出阶段给 `io.timeout`
（`stage=write` / `stdout`，退出码 `8`）；批次里剩下的目标不再开始，已经写完的图仍留在 `images` 里。所以部分完成
的一批和局部截图失败表现完全一致：退出码非 0，凡是已经落地的都照样交付。

等一个人是**另一条**时钟：`--consent-timeout-ms <ms>` 只给确认框设上限，绝不动自动处理那份预算（人走开了不等于
"机器慢"）。到了时间没人回答，本次请求就按**拒绝**处理——`capture.consent_timeout`、退出码 `6`——绝不当成同意，
批次剩下的部分也就像有人明确答"否"之后那样停下。不给或写 `0` 还是一直等，和从前一样。点"是"之后那约 1 秒的缓冲
是为了把对话框的关闭动画挡在画面之外，它属于人工那一阶段，绝不会为了赶期限而被省掉：被限制的是"等一个回答"，
不是"回答之后等画面安定"。

**阻塞到底堵在哪。** `PrintWindow` 是给目标窗口发一个绘制请求、再等它自己的线程；`--capture printwindow` 和
`dwm` 的回读干的就是这件事，而这个调用内部没有能拿来核对期限的打断点。`std::regex` 也一样：像 `(a+)+$` 这种
模式去匹配一个长标题可能回溯好几分钟，而对模式限长度并不是执行期限。这些调用现在都跑在同一个 `ECAPTURE.EXE`
拉起的辅助进程里，父进程通过一条私有管道把已经解析好的一个任务递给它；期限一到，父进程就停掉**它自己的**那个
辅助进程并报出超时。目标应用的窗口绝不会被杀掉，也没有哪个辅助进程能活得比父进程久（一个"关闭即结束"的作业对象，
加一次断管检查，再加一个空闲看门狗）。而这**不**改变的事：辅助进程只会读单个窗口自己的画面、或列举顶层窗口，
它从不采样桌面像素、也从不写文件，所以每一条桌面路径照旧要走上面那套授权——没有 `--worker` 这个选项，
`--yes` 的任何规矩也都没有变松。

把限度说成限度：这份预算只在能打断的地方、以及靠停掉辅助进程起作用。原子写文件、被人停止读取的 stdout 管道、
无视取消请求的 WinRT 编码器都没有取消点，所以这三样是在开始之前拦一道、在结束之后再计时，而不是在调用中途抢占。
而且在 `Win10 19045` 上，`PrintWindow(PW_RENDERFULLCONTENT)` 是从 DWM 缓存的那张表面上渲染的，根本不发
`WM_PRINT`，所以一个卡在 `WM_PRINT` 里的窗口在那个系统上并不会把父进程拖住；真正会等目标线程的是不带这个 flag
的那次 `PrintWindow` 退路调用。别假定"卡死"这种场景在每个 Windows 版本上都碰得到——只要假定这个工具会在它的
期限内返回。

## 文件名占位符

用在 `--out` 的路径里，多张图靠它区分：

| 占位符 | 含义 |
| --- | --- |
| `%i` | 序号，从 1 起（`--all` 多窗口、`--monitor all` 多屏） |
| `%h` | 窗口句柄，形如 `0x001B0C48`；屏幕目标给 0 |
| `%p` | 进程 ID；屏幕目标给 0 |
| `%n` | 窗口标题；屏幕目标给去掉 `\\.\` 前缀的设备名（如 `DISPLAY1`）。标题会被清洗成能用的文件名片段：非法字符换成 `_`、去掉尾部的点与空格、整段正好是保留设备名（`CON` / `NUL` / `COM1` / `LPT1` …）时加 `_` 前缀、按 80 个 UTF-16 码元截断且不劈开代理对 |
| `%d` | 本地日期 `YYYYMMDD` |
| `%t` | 本地时间 `HHMMSS` |
| `%%` | 一个字面 `%`；其余 `%x` 原样保留两个字符 |

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

## 给 AI / 脚本的调用指南

工具就是为程序化调用设计的，按下面这套约定走最省事。项目里还带了一份教 AI 用它的 skill：
`.agents/skills/ecapture-screenshot/`（里面有 `SKILL.md`、`references/cli-contract.md` 和一份 exe 副本）。

1. **先 `--dry-run` 探一次**，再消歧，最后真截图。`--dry-run` 不取帧、不写文件、也不弹确认框，候选在
   `notes[0].value`：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。注意 `--dry-run` 仍要求给 `--out`，
   否则报 `cli.missing_output` + 1；而**只给 `--dry-run` 不给任何窗口条件 = 文本帮助 + 退出码 2**。
2. **按 `errors[].code` 分支，不要匹配 `message` 文字**（那随 `--lang` 变）。常用的几条：
   `match.no_window`（4，条件太窄或目标最小化）、`match.ambiguous_window`（5，从 `hint` 的候选里挑）、
   `match.index_out_of_range` / `match.monitor_out_of_range`（1，`hint` 列了全部候选）、
   `cli.missing_output`（1）、`cli.invalid_format`（1）、`cli.stdout_multiple_targets`（1，多个目标要共用
   同一条 stdout）、`capture.failed`（7）、`capture.frame_timeout`（7，等帧超时）、
   `capture.window_gone`（7，目标已经没了，该重新枚举）、`capture.frame_invalid`（7，交回来的帧内存形状不合法）、
   `capture.consent_unavailable`（6，这个会话没有可交互的桌面，没人能同意）、
   `capture.consent_stale`（7，确认之后目标挪了位置，要重新选目标并再问一次）、
   `io.write_failed`（8，目录不存在或提交失败）、`io.file_exists`（8，配合 `--no-overwrite`）、
   `io.output_collision`（8，两个目标算出同一个输出名，整批没截图也没写文件）。
   每条错误还带 `target` / `backend` / `stage` / `hresult` / `win32`（见上一节），拿到多少写多少，
   不必从 `message` 里抠。
3. **读流要分情况**：给 `--out <文件>` 时 JSON 在 stdout、stderr 是空的，直接解析就行；用 `--out -` 或没给输出路径时
   图片字节占了 stdout，JSON 整体改到 stderr。stdout 一次只交付一张图，多个目标请写到文件。PowerShell 5.1 里
   `2>&1` 会把 stderr 包装成错误记录，想同时拿图片和 JSON 就 `1>`/`2>` 分开重定向。
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

| 命令 | 用途 |
| --- | --- |
| `.\build.ps1` | Release 构建，产物 `build\ecapture.exe`；`-Config Debug`、`-Clean` 可选 |
| `.\tests\cli.ps1` | 135 例输出契约断言（含 `--yes` 与 `--no-overwrite` 的每种布尔写法）+ 通道分离 + 多语言检查（一律 `--dry-run`，不截图） |
| `.\tests\streams.ps1` | 真机标准流与结构化结果：单目标写 stdout 时图与 JSON 各归其位、多目标写 stdout 整批被拒（含隐式 stdout 不被折叠成"缺少输出路径"）、判据是实际命中的目标数、多屏被拒且确认框根本不弹、诊断的定位字段、批次中途失败保留前面已成功的图、结果送不到约定那条流时报 8（只截自建的窗口） |
| `.\scripts\check-lang.ps1` | 四语文案的 key / 占位符对齐检查，并确认 exe 里真编进了四份资源 |
| `.\tests\invoker.ps1` | 离线检查共享的测试进程调用器：argv 引号、双流同时输出、二进制不被转码、卡死的子进程、每次运行各自的临时目录（不截图） |
| `.\tests\build-path.ps1` | 构建路径判据：离线一层验临时批处理正文只能是 ASCII、VS 环境导入失败在跑 cmake 之前就报错；真机一层在含中文、空格、括号、百分号的目录里跑 Release / Debug / RelWithDebInfo 与 `-Clean`，再把 `%TEMP%` 换成中文目录构建一次（不截图；`-OfflineOnly` 只跑离线那层） |
| `.\tests\smoke.ps1` | 真机冒烟：截自己建的测试窗口 → 校验 PNG 尺寸与像素内容 |
| `.\tests\image.ps1` | 帧校验：离线层手工摆像素排布（竖条纹 / 棋盘 / alpha / 行末填充 / 超限与短缓冲区 / 
  越界裁剪），真机层验单色窗口的质量提示与来路 |
| `.\tests\save.ps1` | 真机文件保存与覆盖保护：每种 `--no-overwrite` 布尔写法对真实文件的效果、整批输出名规划与撞名检测（`%p` / `%n` / `%d` / `%t` / `%%` / 未知 `%x` / 大小写 / 清洗 / 截断）、原子提交（目标被占用、目标名是目录、目录不存在、写到一半被硬杀）、并发禁止覆盖 |
| `.\tests\channels.ps1` | 真机通道对比：六条通道 + 遮挡对照，目标与遮挡物都是自建的窗口。窗口内容那几条带 `--yes` 跑，一旦弹框就判失败；`bitblt` / `duplication` 取的是桌面像素，它们的画面判据要 `-SimulateConsent` 才跑，不给就如实记 SKIP（未验证） |
| `.\tests\consent.ps1` | 截图授权分级：离线一层用注入的假应答器与假屏幕布局把 `ConsentGate` 整台状态机跑完（`build\ecapture-consent-tests.exe`，源码 `tests\consent_state.cpp`）；真机一层把所有确认框一律代答"否"，判哪些路径必须弹、拒绝之后报什么（`code` / `stage` / `target` / `value`）、有没有落地，以及 `images[].path` / `scope` / `rect` 对不对。绝不代人答"是" |
| `.\tests\isolation.ps1` | 真机资源隔离：同名的既有进程保持存活且不被当成目标、并发两轮互不串、异常退出只清理自身 |
| `.\tests\screen.ps1` | 真机整屏测试：三条屏幕通道（都属于桌面路径，每一条都必须弹框）+ 红块定位 + 阴性对照。只有加了 `-SimulateConsent` 才会代答确认框，且只该在专门腾给测试的桌面上这么用；不给时凡是要答框的判据记 SKIP（未验证） |
| `.\tests\window_shot.bat` | 给人跑的批处理：编译测试窗口程序 → 逐通道截图（框由人自己点）→ 整屏那一步 → 打开截图目录 → 只结束自己起的那个 PID |
| `.\scripts\mkreadme.ps1` | 用各语言 `--help` 的原样输出重生成四份 README 的帮助段 |

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
