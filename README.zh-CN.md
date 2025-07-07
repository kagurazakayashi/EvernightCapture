<div align="center">

<img src="assets/logo.png" width="128" height="128" alt="EvernightCapture 图标">

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
- **整屏截图带人工确认**：`--monitor` 截整块屏幕前一定先弹模态确认框，**没有命令行或环境变量旁路**
- **四语文案**：`zh-CN` / `zh-TW` / `en` / `ja`，默认跟随系统显示语言，全部编在 exe 的资源里
- **机器读的 JSON**：只装捕获结果与错误，不带工具名、版本、schema、参数回显之类的元信息

## 快速开始

需要 Visual Studio（"使用 C++ 的桌面开发"工作负载）+ Windows SDK，构建脚本会自动定位它们。

```powershell
.\build.ps1                                  # Release，产物 build\ecapture.exe
ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
```

输出目录必须**已经存在**，工具不建目录。先看看会命中谁（不截图、不写文件）：

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

# 整屏：一定会先弹确认框，且没有跳过开关
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
  --monitor, -m [<n|primary|all>] 截图目标屏的编号，从 1 开始（按显示设置里的顺序）；primary = 主屏，all = 每块屏各一张。不给窗口条件时 = 整块屏幕截图，给窗口条件时 = 只算与该屏有重叠的窗口。取值可省略（= 主屏），省略时不吃后面的参数，所以 --monitor out.png 仍然可用。整屏截图会先弹框征求同意，且没有跳过确认的开关

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
  --capture, -C <method>          wgc(默认，被遮挡也能截) / dwm(DWM 缩略图，被遮挡也能截) / printwindow(窗口自绘) / bitblt(拷屏幕可见像素) / duplication(桌面复制后按矩形裁) / auto(按 wgc-dwm-printwindow-bitblt 回退；整屏截图只用 wgc-duplication-bitblt)

输出
  --out, -o <path|->              输出路径；特殊值 - 表示把图片字节写到标准输出。也可用位置参数；完全不给时等同 --out -
  --format, -f <name>             强制编码格式；不给则由输出文件扩展名判定，扩展名也判不出时用 png
  --quality <1-100>               JPEG 质量，默认 100
  --no-overwrite                  目标已存在时不覆盖，报错退出

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
2. 诊断项里为空的字段整个键省略，不会输出 `null` 占位。
3. `code` 值稳定：`cli.*` / `note.*` / `match.*` / `capture.*` / `io.*`，只增不改名。
4. 通道：默认全部写 stdout、stderr 保持空；一旦图片占用标准输出（显式 `--out -`，或根本没给输出路径），
   JSON 整体改走 stderr，两个通道永不混流。
5. `captured` 等于 `images` 条数；一个窗口一张图，`--monitor all` 则一块屏一张图。

## 退出码

`0` 成功 / `1` 参数错 / `2` 未给条件 / `3` `--help` / `4` 无匹配窗口 / `5` 匹配多个窗口 /
`6` 目标受保护或被拒绝 / `7` 截图失败 / `8` 写文件失败 / `9` 内部异常。新增语义只会追加编号。

退出码与 body 是两套独立信号，`2`/`3`/`4`/`5` 是正常控制流而不是崩溃。**允许部分成功**：`--all` 或
`--monitor all` 里某些目标失败时，已写出的图仍在 `images` 里（`captured` 可以大于 0），但退出码是 `7`。

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
- `--capture` 取值写错在解析期就报 `cli.unknown_capture_method`（退出码 1），**不会退化成默认通道**；
  只有 `auto` 允许回退，回退成功会发 `note.capture_channel` 说明实际用了哪条。
- DRM / 受保护内容一律黑屏；驱动黑框（部分播放器）有的通道能过、有的不能，不保证。
- 整屏截图只走 `wgc` / `duplication` / `bitblt`；`--monitor` 配 `dwm` 或 `printwindow` 在解析期报
  `capture.unsupported`（退出码 1）。`auto` 在屏幕模式下按 wgc → duplication → bitblt 回退。

## 整屏截图与隐私确认

给了 `--monitor` 且没有窗口条件就是截整块屏幕，这时**一定先弹一个模态确认框**（列出目标屏、走哪条通道、
图去哪里），只有点"是"才取帧：

- **没有命令行旁路，也没有环境变量旁路**。弹不出框（服务会话、没有交互桌面）按拒绝处理。
- 答"否"或弹不出都是 `capture.access_denied` + 退出码 `6`，不写文件。
- 点"是"之后会等 1 秒再取帧，避免把对话框的关闭动画拍进图里；确认框本身不会出现在图中。
- `--dry-run` 和"按屏过滤窗口"模式不取整屏画面，所以不弹框。
- 想区分"被人拒绝"和"路径没给对"就必须显式给 `--out`：不给输出路径时任何失败都塌成
  `cli.missing_output` + 退出码 1，真实原因不外泄。

`--monitor`（省略取值）与 `--monitor primary` 是主屏，`--monitor 2` 是第 2 块屏，`--monitor all` 每块屏一张。
编号按 `EnumDisplayMonitors` 的顺序、从 1 起；越界报 `match.monitor_out_of_range`（退出码 1），`hint` 里列出本机全部屏幕。
`--monitor <n>` 与窗口条件同时给出＝按屏过滤窗口（窗口矩形与该屏有重叠即命中，跨屏窗口在两块屏上都算），
出的仍是窗口图，也不弹确认框。`--monitor all` 与任何窗口**匹配**条件互斥（报 `cli.monitor_conflict`，退出码 1），
但 `--all` / `--index` 这类消歧选项不算匹配条件，可以和它搭配。

## 文件名占位符

用在 `--out` 的路径里，多张图靠它区分：

| 占位符 | 含义 |
| --- | --- |
| `%i` | 序号，从 1 起（`--all` 多窗口、`--monitor all` 多屏） |
| `%h` | 窗口句柄，形如 `0x001B0C48`；屏幕目标给 0 |
| `%p` | 进程 ID；屏幕目标给 0 |
| `%n` | 屏幕目标给去掉 `\\.\` 前缀的设备名（如 `DISPLAY1`） |
| `%d` | 本地日期 `YYYYMMDD` |
| `%t` | 本地时间 `HHMMSS` |
| `%%` | 一个字面 `%`；其余 `%x` 原样保留两个字符 |

`--all` 的输出名里没有占位符时会自动追加 `_序号`，并发 `note.all_without_placeholder`。

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

1. **先 `--dry-run` 探一次**，再消歧，最后真截图。`--dry-run` 不取帧不写文件，候选在 `notes[0].value`：
   `hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=…`。注意 `--dry-run` 仍要求给 `--out`，
   否则报 `cli.missing_output` + 1；而**只给 `--dry-run` 不给任何窗口条件 = 文本帮助 + 退出码 2**。
2. **按 `errors[].code` 分支，不要匹配 `message` 文字**（那随 `--lang` 变）。常用的几条：
   `match.no_window`（4，条件太窄或目标最小化）、`match.ambiguous_window`（5，从 `hint` 的候选里挑）、
   `match.index_out_of_range` / `match.monitor_out_of_range`（1，`hint` 列了全部候选）、
   `cli.missing_output`（1）、`cli.invalid_format`（1）、`capture.failed`（7）、`capture.access_denied`（6）、
   `io.write_failed`（8，目录不存在）、`io.file_exists`（8，配合 `--no-overwrite`）。
3. **读流要分情况**：给 `--out <文件>` 时 JSON 在 stdout、stderr 是空的，直接解析就行；用 `--out -` 或没给输出路径时
   图片字节占了 stdout，JSON 整体改到 stderr。PowerShell 5.1 里 `2>&1` 会把 stderr 包装成错误记录，想同时拿图片和
   JSON 就 `1>`/`2>` 分开重定向。
4. **别把非 0 退出码当全盘失败**：部分成功时 `captured` 大于 0 而退出码是 7，已经落地的图照样可用。
5. **退出码 0 不等于画面是对的**：受保护内容、某些播放器驱动会在成功返回的同时给你黑帧。要判正确性就校验像素——
   比如把一个纯色窗口盖住目标再截，看拿到的是目标内容还是遮挡物；至少比对 `width`/`height` 与目标窗口矩形。
6. **整屏要先问人**：`--monitor` 截整屏会弹模态框并阻塞进程直到人答复，没有旁路。自动化流程里别把它当"随手能拿的
   全屏图"；调用前告知用户、并显式给 `--out`。只想截某个窗口就别升格成整屏。
7. 想稳定拿到"某个应用"，优先用 `--process`/`--exe` + `--class`；标题匹配区分大小写，跨语言环境不可靠。

## 构建与测试

| 命令 | 用途 |
| --- | --- |
| `.\build.ps1` | Release 构建，产物 `build\ecapture.exe`；`-Config Debug`、`-Clean` 可选 |
| `.\tests\cli.ps1` | 77 例输出契约断言 + 通道分离 + 多语言检查（一律 `--dry-run`，不截图） |
| `.\scripts\check-lang.ps1` | 四语文案的 key / 占位符对齐检查，并确认 exe 里真编进了四份资源 |
| `.\tests\smoke.ps1` | 真机冒烟：开记事本 → 截图 → 校验 PNG 尺寸与像素内容 |
| `.\tests\channels.ps1` | 真机通道对比：六条通道 + 遮挡对照 |
| `.\tests\screen.ps1` | 真机整屏测试：确认框行为 + 三条屏幕通道 + 红块定位 + 阴性对照 |
| `.\scripts\mkreadme.ps1` | 用各语言 `--help` 的原样输出重生成四份 README 的帮助段 |

`build.ps1` 用 vswhere 定位 VS，并优先使用 VS 自带的 cmake/ninja。构建要求 `/W4` 下零警告。
在 Git Bash 里手工测试要先 `export MSYS2_ARG_CONV_EXCL='*'`，否则 `/help` 会被当成路径改写、`--out /tmp/x.png` 会被转成怪路径。

## 许可

EvernightCapture 采用 [木兰宽松许可证第 2 版（Mulan PSL v2）](http://license.coscl.org.cn/MulanPSL2)，
中英双语全文见 [LICENSE](LICENSE)。

```
Copyright (c) 2025 KagurazakaYashi (KagurazakaMiyabi)
EvernightCapture is licensed under Mulan PSL v2.
```
