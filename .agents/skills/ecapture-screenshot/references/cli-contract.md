# ECAPTURE 调用契约（v0.4.0）

本文是给调用方的参考。选项清单的**权威来源始终是 `ECAPTURE.EXE --help`**（退出码 3），
本文只在它之外补充 JSON 字段、诊断码全表、命名占位符和各 shell 的坑。

## 全部选项

| 选项 | 短写 | 取值 | 说明 |
| --- | --- | --- | --- |
| `--monitor` | `-m` | `[<n>\|primary\|all]`，可省略（= 主屏） | 截哪块屏。编号从 1 起，按"显示设置"的顺序；`all` = 每块屏各一张。**给了它且没有窗口匹配条件 = 整屏截图（必弹确认框）**；`--monitor <n>` 配窗口匹配条件 = 只算与该屏有重叠的窗口（不弹框，出窗口图）。`all` 与任何窗口**匹配**条件互斥（`cli.monitor_conflict`+1），但 `--all`/`--index` 这类消歧选项不算匹配条件、可以搭配。省略取值时不吃后面的参数，所以 `--monitor out.png` 里 `out.png` 仍是输出路径 |
| `--hwnd` | | 句柄 | 纯数字按十进制，`0x` 前缀或含 a-f 按十六进制；推荐写 `0x` |
| `--pid` | | 十进制 >0 | 进程 ID |
| `--process` | `-p` | 映像文件名 | 不含路径，忽略大小写；无扩展名时按 `.exe` 处理 |
| `--exe` | | 完整路径 | 忽略大小写 |
| `--title` | `-t` | 精确标题 | **区分大小写**的整串相等；中文标题直接可用（入口是宽字符，不经 UTF-8→ACP） |
| `--title-contains` | `-T` | 子串 | 同样区分大小写 |
| `--title-regex` | `-R` | ECMAScript 正则 | 解析期即校验，写错立刻报 `cli.invalid_regex` |
| `--class` | `-c` | 窗口类名 | 忽略大小写，如 `Notepad` / `CabinetWClass` / `UnityWndClass` |
| `--index` | `-i` | 从 1 起 | 多匹配消歧，按可见性/叠放次序排序 |
| `--newest` / `--oldest` | | 开关 | 取最后/最早创建的窗口 |
| `--all` | `-a` | 开关 | 每个命中窗口各存一张；与 `--monitor all` 互斥 |
| `--capture` | `-C` | `wgc`（默认）/`dwm`/`printwindow`/`bitblt`/`duplication`/`auto` | 取图通道。取值写错解析期报 `cli.unknown_capture_method`，不会退化成默认值。屏幕模式只支持 `wgc`/`duplication`/`bitblt`/`auto`，`dwm`/`printwindow` 报 `capture.unsupported` |
| `--out` | `-o` | 路径或 `-` | `-` = 图片字节写标准输出。也可用位置参数；完全不给时等同 `--out -`。整批的最终绝对路径在取第一帧（以及整屏确认框）之前一次算好：扩展名缺了就补，两个目标算出同一个名字就报 `io.output_collision`+8 且整批不作，绝不静默改名。`-` 不是路径，不参与展开与碰撞检测 |
| `--format` | `-f` | `png`/`jpg`/`jpeg`/`bmp`/`tiff`/`gif` | 不给则由扩展名判定；扩展名判不出时用 png 并发 `note.format_defaulted_png`（文件名不改）。**没有 `webp`、没有 `ico`、没有 `auto`** |
| `--quality` | | 1–100，默认 100 | 只对 jpeg 生效，给别的格式发 `note.quality_ignored` |
| `--no-overwrite` | | 开关，可写 `=true/false` | 目标已存在时报 `io.file_exists` 且不覆盖。裸写与 `=true/1/yes/y/on` 同义（禁止覆盖），`=false/0/no/n/off` 取消禁令；重复给出时最后一个生效。**已存在与否由最后那次不许替换的改名原子判定**，没有「先查一下」那种竞态预检 |
| `--dry-run` | `-d` | 开关 | 选完窗口就返回，只给 `note.dry_run`；不取帧、不写文件、不弹确认框。但仍要求给 `--out` |
| `--json` | `-j` | 开关 | 已废弃、无副作用（成功与错误本来就是 JSON）；用它会收到 `note.json_flag_deprecated` |
| `--verbose` | `-v` | 开关 | JSON 追加 `input` 段（规范化后的全部输入，含最终生效的 `lang` 与 `overwrite`） |
| `--quiet` | `-q` | 开关 | 省略 `notes`；**`errors` 不受抑制** |
| `--lang` | `-l` | `auto`（默认，跟随系统显示语言）/`zh-CN`/`zh-TW`/`en`/`ja` | 只影响给人看的 `message`/`hint`/`--help`；`code`、JSON 键名、取值枚举、`0x…` 句柄一律不变。取值宽容：忽略大小写、`_` 与 `-` 等价、`zh_TW`/`zh-Hant`/`cht`/`tw`/`chs`/`cn`/`jp` 都认。写错报 `cli.unknown_language` |
| `--help` | `-h` | | 文本帮助，退出码 3 |
| `--version` | | | 版本与阶段，纯文本 |

写法：`--opt=value` / `-opt` / `/opt` 都接受；取值本身以 `-` 开头时写成 `--title=-x`，或用 `--` 结束选项解析。
开关也可以写 `--opt=true/false`（`1/0`、`yes/no`、`y/n`、`on/off` 都认，大小写与前后空格无关）：普通开关写 `=false` 等于没写，`--no-overwrite` 这种负向开关写 `=false` 才是取消禁令。
**短选项不能合并**（`-qi` 会报 `cli.unknown_option`）。同一选项多次出现取并集，不同选项必须同时命中（AND）。

## JSON 结构

```json
{ "captured": 1,
  "images": [ … ],
  "errors": [ { "code","message","option","value","hint" } ],
  "notes":  [ 同 errors 的形状 ],
  "input":  { … 仅 --verbose } }
```

- `captured` 与 `images` 恒在（空时 `[]`）；`errors` 只要非空就必须出现；`notes` 仅非空且未 `--quiet`；
  `input` 仅 `--verbose`。**为空的字段整个键省略，不输出 `null` 占位。**
- 输出里**不含**工具名、版本、schema、stage、参数回显之类的元信息。
- 通道分配：默认全部走 stdout、stderr 为空；一旦图片占用 stdout（`--out -` 或没给输出路径），
  **整份 JSON 改走 stderr**，两个通道永不混流。
- 文本输出只有三种情况：`--help`、`--version`、没给任何条件。

### 窗口图（`images[]` 每一项）

`file` `bytes` `width` `height` `format` `hwnd`（`0x…` 字符串）`pid` `title` `class` `image`（映像文件名）`elapsedMs`

### 屏幕图（`--monitor` 且无窗口条件时换这一组字段）

`file` `bytes` `width` `height` `format` `monitor`（编号）`device`（`\DISPLAY1` 之类）`primary`（布尔）`elapsedMs`

没有窗口可归属，所以 `hwnd` / `pid` / `title` / `class` / `image` 整个不出现——调用方按 `monitor` 是否存在区分两种图。

### `--dry-run` 的候选窗口在哪

不截图时 `captured` 是 0、`images` 是 `[]`，候选信息在 `notes` 里那条 `note.dry_run` 的 `value`：

```json
{ "code": "note.dry_run", "message": "--dry-run：已选出 1 个窗口，未截图也未写文件",
  "option": "--dry-run",
  "value": "hwnd=0x001B0C48 pid=31468 1261x614+681+22 class=CabinetWClass title=D:\\share\\… - 文件资源管理器" }
```

多匹配时（退出码 5）候选在 `errors[0].hint`，形如 `0x… 标题 [进程名] | 0x… …`；
命中矩形给的是 `宽x高+左+上`（物理像素，已按 DPI 修正）。

## 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 参数错 |
| 2 | 未给条件（输出文本帮助） |
| 3 | `--help` |
| 4 | 无匹配窗口 |
| 5 | 匹配多个窗口 |
| 6 | 目标受保护或被用户拒绝（含整屏确认框答"否"） |
| 7 | 截图失败 |
| 8 | 写文件失败 |
| 9 | 内部异常 |

退出码与 body 是两套独立信号：先看 `errors`，再看 `captured`，最后才用退出码做粗分支。

- 只有 `io.write_failed`、`io.file_exists` 与 `io.output_collision` 会给出 8；截图/编码阶段的其它失败（含
  `capture.failed`、`capture.encoder_unavailable`）都给 7；`capture.access_denied` 给 6。
- **部分成功**：`--all` / `--monitor all` 里某些目标失败时，已写出的图照样在 `images` 里
  （`captured` 可以大于 0），但退出码仍是 7。所以"退出码非 0"不等于"什么都没拿到"。

## 诊断码全表

只按 `code` 分支。码值只增不改名。

**`cli.*`（解析期，全部退出码 1）**
`cli.unknown_option` `cli.missing_value` `cli.switch_takes_no_value` `cli.invalid_number`
`cli.invalid_regex` `cli.invalid_value` `cli.invalid_format` `cli.unrecognized_extension`
`cli.unexpected_positional` `cli.missing_output` `cli.duplicate_output` `cli.conflicting_options`
`cli.unknown_capture_method` `cli.unknown_language` `cli.monitor_conflict` `cli.internal_error`
`cli.no_condition`（→ 文本帮助 + 2）

**`match.*`**
`match.no_window`（4）`match.ambiguous_window`（5）`match.index_out_of_range`（1）`match.monitor_out_of_range`（1）

**`capture.*`**
`capture.access_denied`（6，受保护窗口或整屏确认被拒/弹不出）`capture.unsupported`（1，屏幕模式配 `dwm`/`printwindow`）
`capture.encoder_unavailable`（7）`capture.failed`（7）

**`io.*`**
`io.write_failed`（8，临时文件建不出来 / 写或刷新中断 / 提交为目标名失败）`io.file_exists`（8，配合 `--no-overwrite`）`io.output_collision`（8，整批输出名撞车，一张都没截也没写）

**`note.*`（不是错误，`--quiet` 会去掉）**
`note.dry_run` `note.capture_channel`（`auto` 回退后实际用了哪条）`note.duplicate_value`
`note.extension_appended` `note.exe_path_looks_like_name`（`--exe` 传的像文件名不像完整路径）
`note.format_extension_mismatch` `note.format_defaulted_png` `note.output_defaulted_stdout`
`note.output_extension_appended` `note.quality_ignored` `note.all_without_placeholder`
`note.flag_overrides_quiet` `note.pipe_default_format` `note.json_flag_deprecated`
`note.help_ignored_arguments`

## 输出名占位符

用在 `--out` 的路径里，多张图靠它区分：

| 占位符 | 含义 |
| --- | --- |
| `%i` | 序号，从 1 起（`--all` 多窗口、`--monitor all` 多屏都用它） |
| `%h` | 窗口句柄，形如 `0x001B0C48`（`0x%08X`）；屏幕目标给 0 |
| `%p` | 进程 ID；屏幕目标给 0 |
| `%n` | 窗口标题；屏幕目标给去掉 `\\.\` 前缀的设备名（如 `DISPLAY1`）。会被清洗成能用的文件名片段：非法字符换成 `_`、去掉尾部的点与空格、整段正好是保留设备名（`CON` / `NUL` / `COM1` / `LPT1` …）时加 `_` 前缀、按 80 个 UTF-16 码元截断且不劈开代理对 |
| `%d` | 本地日期 `YYYYMMDD` |
| `%t` | 本地时间 `HHMMSS` |
| `%%` | 一个字面 `%`；其余 `%x` 原样保留两个字符 |

`--all` 的输出名里没有占位符时会自动追加 `_序号` 并发 `note.all_without_placeholder`。**模板里已有 `%` 时不再追加**，所以占位符分不开目标就得由调用方负责：整批名字在取帧前一次算完，任意两个撞车（绝对路径、不区分大小写、逐码元比较）就报 `io.output_collision`+8，一张也不会落地。`%d` / `%t` 一批发一次时钟，跨午夜也不会劈成两天。8.3 短名、硬链接、junction / 符号链接、UNC 与盘符这类字符串比不出来的别名，预检认不到，只能由提交那一次原子操作当场判定。`--out -` 不展开也不查碰撞，整批按顺序进同一条流。

写文件是原子的：先写目标目录下唯一的临时文件（`~<目标名>.ecapture-<pid>-…`），写完、刷新、关闭之后才改名成目标名。允许覆盖走可替换的改名，禁止覆盖走不可替换的改名（撞名即 `io.file_exists`）；任何一步失败都只清掉本次自己的临时文件，旧文件既不会被截断也不会被删。保证边界：本机文件系统上同卷改名是原子的，网络共享上只看服务端实现；只读或被人占用的目标照旧报错，不会被硬换掉。
这些模板占位符**不是**文案里的 `%1..%9` 参数占位符，别混。

## 各 shell 的坑（实测）

- **Git Bash / MSYS2**：`/help` 会被当路径转换，先 `export MSYS2_ARG_CONV_EXCL='*'`。
  给 `--out` 传 `/tmp/x.png` 会被改写成驱动器相关的怪路径（实测变成 `D:\tmp\x.png` 并因目录不存在报
  `io.write_failed`）——**一律传 `D:\dir\name.png` 这种 Windows 形式**。
  `--help` 退出码 3 会断掉 `&&` 链，用 `;`。
- **PowerShell 5.1**：`2>&1` 会把原生 stderr 包装成错误记录文字，`--out -` 那种"JSON 走 stderr"的调用
  会解析不出 JSON。要么显式给 `--out <文件>`（JSON 就在 stdout），要么用 .NET `Process` 分别读两个流。
- **stdout 拿图片**：`--out -` 出的是**二进制 PNG 字节**，JSON 同时在 stderr。重定向要分开写：
  `ECAPTURE.EXE --process notepad.exe --out - 1> shot.png 2> result.json`
- 输出目录必须**已存在**，工具不建目录。
- 控制台代码页不是 65001 时中文照样正常（工具直接写 UTF-8 字节 + CRLF），但**别用 `Write-Host` 之外的
  管道去二次编码**。

## 常用配方

```powershell
# 1) 先看命中谁（不写文件，但 --out 必须给）
ECAPTURE.EXE --process notepad.exe --dry-run --out D:\shots\_probe.png

# 2) 精确锁定一个窗口
ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png

# 3) 多匹配：按 hint 里的句柄回来点名
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite D:\shots\one.png

# 4) 每个命中窗口各一张
ECAPTURE.EXE --pid 12345 --title-contains 报告 --all --out "D:\shots\rpt_%i.png"

# 5) 图片进管道（JSON 于是在 stderr）
ECAPTURE.EXE --process notepad.exe --out - > D:\shots\snap.png

# 6) 整屏（会弹确认框，必须先跟人打招呼；显式 --out 才看得出"是被拒绝"）
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"

# 7) 按屏过滤窗口（不弹框，出的是窗口图字段）
ECAPTURE.EXE --monitor 2 --process chrome.exe --all --out "D:\shots\m2_%i.png"

# 8) 只要屏幕上此刻的样子（连遮挡物一起要）
ECAPTURE.EXE --class CabinetWClass --index 1 --capture bitblt --out D:\shots\visible.png

# 9) 给英文环境的人看诊断文字
ECAPTURE.EXE --process notepad.exe --out D:\shots\a.png --lang en
```
