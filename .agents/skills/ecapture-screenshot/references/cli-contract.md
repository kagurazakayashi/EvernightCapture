# ECAPTURE 调用契约（v0.4.0）

本文是给调用方的参考。选项清单的**权威来源始终是 `ECAPTURE.EXE --help`**（退出码 3），
本文只在它之外补充截图授权的两级判据、期限与阻塞隔离、JSON 字段、诊断码全表、命名占位符和各 shell 的坑。

## 全部选项

| 选项 | 短写 | 取值 | 说明 |
| --- | --- | --- | --- |
| `--monitor` | `-m` | `[<n>\|primary\|all]`，可省略（= 主屏） | 截哪块屏。编号**只认十进制**、1 起（1..65535）—— 它是**本次进程这一次 `EnumDisplayMonitors` 枚举里的位置**，不保证等于「显示设置」里写的标识号，插拔显示器或改分辨率之后同一个编号可能指到另一块屏，所以别把编号当跨调用的屏幕身份，要认屏请用结果里的 `device`。`all` = 每块屏各一张。**给了它且没有窗口匹配条件 = 整屏截图（桌面像素，必弹确认框，`--yes` 跳不过）**；`--monitor <n>` 配窗口匹配条件 = 只算与该屏有重叠的窗口，出的是窗口像素，所以走窗口那一级（带 `--yes` 才不弹框，不给 `--yes` 照样问一次）。`all` 与任何窗口**匹配**条件互斥（`cli.monitor_conflict`+1），但 `--all`/`--index` 这类消歧选项不算匹配条件、可以搭配。省略取值时不吃后面的参数，判据与解析同源：`--monitor out.png` 里 `out.png` 仍是输出路径，而 `--monitor 1e3` 是写坏的编号，报 `cli.invalid_number` 而不会被改成输出文件名。屏幕目标在取帧之前会按设备名重新核对那块屏：已经不在了就整个不截（`capture.monitor_changed`），尺寸或位置变了就以新矩形重新向人确认，旧授权不会被用在新尺寸的屏上 |
| `--hwnd` | | 句柄（十进制 / `0x…` / 含 `a-f`） | 纯数字按十进制，`0x` 前缀或含 `a-f` 按十六进制（Spy++ 那种写法），推荐写 `0x`。正负号、空白、超过 64 位与句柄 `0` 一律拒收（`--hwnd -1` 不会变成 `UINT64_MAX`）；下划线只在十六进制写法里合法，且必须夹在两位十六进制数字之间（`0x001A_0B4C` 可，`0x_1A`、`1A__0B4C`、`1A0B4C_`、`12_34` 不可） |
| `--pid` | | 十进制 1..4294967295 | 进程 ID。**只认 `[0-9]+`**：正负号、空白、小数点、指数（`1e3`）、下划线、`0x`、非 ASCII 数字都不收，越界也不强转（`--pid 1e3` 不会变成 483） |
| `--process` | `-p` | 映像文件名 | 不含路径，忽略大小写；无扩展名时按 `.exe` 处理 |
| `--exe` | | 完整路径 | 忽略大小写 |
| `--title` | `-t` | 精确标题 | **区分大小写**的整串相等；中文标题直接可用（入口是宽字符，不经 UTF-8→ACP） |
| `--title-contains` | `-T` | 子串 | 同样区分大小写 |
| `--title-regex` | `-R` | ECMAScript 正则 | 解析期即校验，写错立刻报 `cli.invalid_regex`；匹配期撞上引擎的回溯复杂度上限同样报它（`stage=match`+1，加大 `--timeout-ms` 没用，见「期限与阻塞隔离」） |
| `--class` | `-c` | 窗口类名 | 忽略大小写，如 `Notepad` / `CabinetWClass` / `UnityWndClass` |
| `--index` | `-i` | 十进制，从 1 起（1..65535） | 多匹配消歧：取当下 Z 序（叠放次序）里的第 n 个。写法与区间见上面「取值写法」 |
| `--topmost-match` | | 开关 | 取当下 Z 序最靠前的命中窗口（此刻盖在最上面那一个） |
| `--bottommost-match` | | 开关 | 取当下 Z 序最靠后的命中窗口（此刻被压在最下面那一个） |
| `--newest` / `--oldest` | | 开关 | 上面两条的**旧名字**（兼容别名，行为完全相同）。它们选的一直是当下的 Z 序位置而不是创建时间 —— Windows 没有取窗口创建时间的公开 API，进程启动时间也不是窗口创建时间。写旧名字会多发一条 `note.deprecated_option`；同一条策略的新旧两种写法一起给（`--newest --topmost-match`）算一条策略，不是互斥冲突 |
| `--all` | `-a` | 开关 | 每个命中窗口各存一张；与 `--monitor all` 互斥 |
| `--capture` | `-C` | `wgc`（默认）/`dwm`/`printwindow`/`bitblt`/`duplication`/`auto` | 取图通道。取值写错解析期报 `cli.unknown_capture_method`，不会退化成默认值。**要不要问人，不看这里写的通道名，看实际走的那条内部路径**（`images[].path`，全表见「截图授权」一节）：`dwm` 的缩略图路径只取窗口画面，它那条"把宿主窗口盖到目标位置上再拷屏幕"的退路 `dwm.screen` 取的是桌面像素——这条退路只在缩略图那一步**真的失败**（PrintWindow 返回 FALSE、位图建不出来、宿主窗口量不出矩形）时才走，**不会因为画面正好是单色就走**（旧实现会，那等于把一扇本来就纯色的窗口升级到要另外授权的桌面取样）。`duplication` 取的是**某一块输出**的合成分：它会先枚举全部显卡适配器与输出定位目标、在该输出所属适配器上建设备（所以由第二块显卡驱动的屏也截得到，这条路上没有 WARP 兜底），并按该输出的显示方向把桌面帧顺时针转 0/90/180/270 度，交付图因此在虚拟屏幕坐标那一套系里（转了几度写在 `images[].rotation`）；一个目标只取与它重叠最多的那块输出，没截全时报 `capturedRect` / `clipped` 并发 `note.capture_clipped`，跨显卡拼图未实现。屏幕模式只支持 `wgc`/`duplication`/`bitblt`/`auto`，`dwm`/`printwindow` 报 `capture.unsupported`（整屏 `wgc` 也是桌面像素） |
| `--yes` | `-y` | 开关，可写 `=true/false` | **截图授权**：只免掉"只取所选窗口画面"那几条路径（`wgc` / `printwindow` / `dwm.thumbnail`）的确认框。裸写与 `=true/1/yes/y/on` = 开，`=false/0/no/n/off` = 关（它虽是正向开关，写 `=false` 却**有意义**：明确要问），重复给出时最后一个生效，最终结果由 `-v` 的 `input.yes` 回显；写成两头都不沾的取值（`--yes=maybe`）解析期就报 `cli.switch_takes_no_value`+1，不会当成"开了"。**其它一概不保证**：不保证目标真交出有效帧、不越过权限、不解除受保护内容、不吞掉任何错误，也不影响覆盖保护。凡是从屏幕上取像素的路径（`bitblt`、`duplication`、任何整屏、`dwm` 的屏幕退路）一定会弹框，这个开关跳不过 |
| `--timeout-ms` | | 毫秒，0–86400000 | **自动阶段的总预算**：从选定目标起，匹配（含 `--title-regex` 求值）、`auto` 的后端重试、等帧、编码、写文件 / 写 stdout 共用这一份剩余时间，整批只发一次，没有哪一步或哪个目标能另领一份。省略或 `0` = 不设总预算，此时被隔离进辅助进程执行的那几步（见「期限与阻塞隔离」）仍有内置 5000 ms 上限兜底，`--capture printwindow` / `dwm` 不再能无限期卡住。预算耗尽时受影响的那张图**不写**：按阶段报 `match.timeout`（`stage=match`）/ `capture.timeout`（`stage=capture`，编码超时也算它）/ `io.timeout`（`stage=write`/`stdout`，退出码 8）；剩下的目标不再开始，已经写好的图留着。等人工确认**不计入**这条预算。只认十进制 `[0-9]+`（`0x…`、负号、下划线、指数、空白与非 ASCII 数字一律拒收），重复给出最后一个生效，最终结果由 `-v` 的 `input.timeoutMs` 回显 |
| `--consent-timeout-ms` | | 毫秒，0–86400000 | 确认框最多等人回答多久；省略或 `0` = 一直等。超时按**拒绝**处理而绝不当作同意：报 `capture.consent_timeout` + 退出码 6、`stage=consent`。这一段单独计时，**不消耗** `--timeout-ms` 的预算；点「是」之后那约 1 秒的关框动画缓冲属于人工阶段，不会为了赶预算被跳过。取值写法与回显同上（`input.consentTimeoutMs`） |
| `--out` | `-o` | 路径或 `-` | `-` = 图片字节写标准输出。也可用位置参数；完全不给时等同 `--out -`。整批的最终绝对路径**在任何确认框与第一帧之前**一次算好（框上列的就是这些名字）：扩展名缺了就补，两个目标算出同一个名字就报 `io.output_collision`+8 且整批不作，绝不静默改名。`-` 不是路径，不参与展开与碰撞检测，而且**一次只交付一张图**：选中的目标多于一个而输出是 `-`（含没给输出路径）时，整批在确认框与取第一帧之前就报 `cli.stdout_multiple_targets`+1，一张都不截、一个文件都不写；判据是实际命中的目标数，所以 `--all` 只命中一个窗口时照样可以写 `-` |
| `--format` | `-f` | `png`/`jpg`/`jpeg`/`bmp`/`tiff`/`gif` | 不给则由扩展名判定；扩展名判不出时用 png 并发 `note.format_defaulted_png`（文件名不改）。**没有 `webp`、没有 `ico`、没有 `auto`** |
| `--quality` | | 十进制 1..100，默认 100 | 只对 jpeg 生效，给别的格式发 `note.quality_ignored`。只认 `[0-9]+`（`--quality 1e` 不会变成 30） |
| `--no-overwrite` | | 开关，可写 `=true/false` | 目标已存在时报 `io.file_exists` 且不覆盖。裸写与 `=true/1/yes/y/on` 同义（禁止覆盖），`=false/0/no/n/off` 取消禁令；重复给出时最后一个生效。**已存在与否由最后那次不许替换的改名原子判定**，没有「先查一下」那种竞态预检 |
| `--dry-run` | `-d` | 开关 | 选完窗口就返回，只给 `note.dry_run`；不取帧、不写文件、**在任何确认框之前就已返回**（注定什么都没截的调用不会先打扰人一次）。但仍要求给 `--out` |
| `--json` | `-j` | 开关 | 已废弃、无副作用（成功与错误本来就是 JSON）；用它会收到 `note.json_flag_deprecated` |
| `--verbose` | `-v` | 开关 | JSON 追加 `input` 段（规范化后的全部输入，含最终生效的 `lang`、`overwrite`、`yes` 与 `timeoutMs` / `consentTimeoutMs`），并保留 notes。**与 `--quiet` 同时给出时按 `--verbose` 处理**：notes 照常交付，另发一条 `note.flag_overrides_quiet` 说明原因 |
| `--quiet` | `-q` | 开关 | 省略 `notes`；**`errors` 不受抑制**，`images[]` 也一条不会少（`path` / `scope` / `rect` 那三项是隐私判据，尤其不许被藏起来）。它与 `--verbose` 同时给出时**按 `--verbose` 处理**（notes 留着 + 一条 `note.flag_overrides_quiet`） |
| `--lang` | `-l` | `auto`（默认，跟随系统显示语言）/`zh-CN`/`zh-TW`/`en`/`ja` | 只影响给人看的 `message`/`hint`/`--help`；`code`、JSON 键名、取值枚举、`0x…` 句柄一律不变。取值宽容：忽略大小写、`_` 与 `-` 等价、`zh_TW`/`zh-Hant`/`cht`/`tw`/`chs`/`cn`/`jp` 都认。重复给出以**最后一个有效的**为准，`auto`（或省略取值）是明确回到系统显示语言而不是保留上一条。写错报 `cli.unknown_language`，并按已经定下的那种语言写这条报错。语言在解析一开始就定下来，判据与其余选项共用同一套 token 消费规则：被 `--title` 吃掉的 `--lang`、`--` 之后的 `--lang` 都不算语言开关 |
| `--help` | `-h` | | 文本帮助，退出码 3 |
| `--version` | | | 版本与阶段，纯文本 |

写法：`--opt=value` / `-opt` / `/opt` 都接受；取值本身以 `-` 开头时写成 `--title=-x`，或用 `--` 结束选项解析。
开关也可以写 `--opt=true/false`（`1/0`、`yes/no`、`y/n`、`on/off` 都认，大小写与前后空格无关）：普通开关写 `=false` 等于没写，`--no-overwrite` 这种负向开关写 `=false` 才是取消禁令，`--yes` 这种登记过的正向开关写 `=false` 则是明确要人问一遍。
**布尔短选项可以合并**（`-vq` 与 `-v -q` 等价，`-yq` 与 `-y -q` 等价），但一簇里只要出现带取值的短选项就整簇不认（`-vl` / `-vp` / `-qi` 都报 `cli.unknown_option`）—— 别把 `-l`、`-o`、`-p`、`-t`、`-T`、`-R`、`-c`、`-i`、`-m`、`-f`、`-C` 混进簇里。
同一选项多次出现：条件类取并集，取值类以最后一个为准；不同选项必须同时命中（AND）。

## 取值写法（数字、位置与重复）

- **数字只认十进制**：`--pid`（1..4294967295）、`--index`（1..65535）、`--monitor <n>`（1..65535）、`--quality`（1..100）、`--timeout-ms` 与 `--consent-timeout-ms`（0..86400000）都只接受 `[0-9]+`，区间在同一次解析里判完。正负号、空白（含前后空格）、小数点、千分位逗号、指数记法（`1e3`）、下划线分隔、`0x` 前缀、非 ASCII 数字（`１２３`、`١٢`）统统报 `cli.invalid_number` + 退出码 1；值不会被强转、回绕或按另一种进制重读 —— `--pid 1e3` 不是 483，`--quality 1e` 不是 30，`--hwnd -1` 也不是 `UINT64_MAX`。期限的 `0` 是合法取值（= 不设这项期限），空白不是 `0`。
- **`--hwnd` 例外**，它文档承诺过三种写法：纯数字 = 十进制、`0x` / `0X` 前缀 = 十六进制、裸写含 `a-f` = 十六进制（Spy++ 那种形式，所以 `--hwnd 1e3` 就是 `0x1e3`）。仍然拒绝正负号、空白、小数点、非 ASCII 数字、句柄 `0` 与超过 64 位的值。下划线只在十六进制写法里合法，且必须夹在两位十六进制数字之间：`0x001A_0B4C` 可以，`0x_1A`、`1A__0B4C`、`1A0B4C_`、`12_34` 都不行。要稳定就一律写 `0x` 前缀。
- **`--monitor` 吃不吃下一个参数，判据与实际解析同一套**：`primary` / `all` / 十进制编号算取值；写坏了的数字（`1e3`、`-1`、`1.5`、`１２`）也算取值并当场报错，不会被悄悄当成输出文件名；而 `out.png`、`2.png`、`v2`、`D:\a\b.png` 这些一眼不像数字的仍是输出路径（`--monitor out.png` = 主屏 + 输出到 out.png）。
- **紧跟要吃值的选项那一条就是它的取值**，哪怕长得像另一个选项：`--title --lang ja` 找的是标题 `--lang`（这条同时决定 `--lang` 会不会生效 —— 被吃掉的那个 `--lang` 不算语言开关）。要以 `-` 开头写取值用 `--title=-x`；`--` 之后的参数一律按位置参数处理（`--` 本身丢弃），所以 `-- --lang ja` 不改语言、`-- -v` 也不是开关。
- **重复给出**：条件类是 OR（`--title A --title B` 找标题为 A 或 B 的窗口），取值类以最后一个为准（`--timeout-ms 9000 --timeout-ms 300` = 300），`--lang` 也一样，其中 `auto` 与省略取值都是**明确回到系统显示语言**，不是保留上一条；非法的 `--lang` 报 `cli.unknown_language` 并沿用已经定下的语言写这条报错。
- **`--verbose` 与 `--quiet` 同时给出时按 `--verbose` 处理**：notes 照常交付，另发一条 `note.flag_overrides_quiet` 说明原因。`errors`、以及 `images[].source` / `path` / `scope` 这些来路字段任何时候都不被 `--quiet` 隐藏。
- **输出名里的数字占位符不受这条约束**：`%i` / `%h` / `%p` 是模板而不是命令行取值，写错形式也不会报错（`%h` 给的是 `0x001B0C48` 那种形状，见下面《输出名占位符》）。

## 截图授权（两级：谁必须问人）

**调用方规矩一句话：可靠窗口截图带 `--yes`；会拍到别家窗口时先向用户说明范围，启动后等用户本人点「是」；
不得用脚本、`SendMessage`、UI 自动化代点；用户拒绝不是技术故障，不得重试。**（细则见下面「调用方（AI）该怎么做」）

**判据是像素实际从哪来，不是通道叫什么名字。** 一条通道可以对应两条路径：`dwm` 的缩略图只碰所选窗口
自己的合成面，它那条"把宿主窗口盖到目标位置上再拷那块屏幕"的退路读的是桌面像素——这两条必须分得开，
否则一条 `--yes` 就把桌面取样也一起批掉了。

- **第一级 · 窗口内容路径**（`wgc` / `printwindow` / `dwm.thumbnail`）：**任何真实截图都先弹一次模态"是/否"框**，
  哪怕走的是最可靠的窗口内容通道。一次确认覆盖框上列出的那一批目标（不会每条通道各问一遍）；
  只有带上 `--yes` 这一级才不弹框。
- **第二级 · 桌面路径**（`bitblt.screen`、`duplication.frame`（哪怕只裁到窗口矩形）、
  任何整屏目标 `screen.wgc` / `screen.bitblt` / `screen.duplication`、以及 `dwm.screen`）：
  **一定要人答"是"**。`--yes`、`--quiet`、环境变量、标准输入、调用者是谁都跳不过，一个像素也不例外。
- 认不出来的路径名（`unknown`、没登记的新路径）一律按桌面路径处理：登记漏了只会更严，不会更松。
- `--capture auto` 带 `--yes`：窗口内容那几条不问，一旦要迈进任何桌面路径就问。
  **窗口内容的批准永远不等于桌面的批准**，范围升级要重新问。
- 一份授权的作用域写死了：只覆盖这一次请求、只覆盖框上列出的那批目标。不跨请求缓存、不扩大到没列出的目标、
  也不会从窗口范围自动升到桌面范围。目标区域变了（挪位置、变大、换屏）或屏幕接拔过，对应那份授权就作废，会再问一次。

### `images[].path` 与 `images[].scope` 的取值（只增不改名）

| `path` | `scope` | 这一帧是哪来的 |
| --- | --- | --- |
| `wgc` | `window` | 那个窗口自己的 WGC 采集项 |
| `printwindow` | `window` | 让窗口自绘到 DC |
| `dwm.thumbnail` | `window` | DWM 缩略图 + 屏幕外宿主窗口 `PrintWindow`（屏幕上没动静） |
| `dwm.screen` | `desktop` | DWM 的内部退路：把宿主窗口盖到目标位置上、拷那块屏幕 |
| `bitblt.screen` | `desktop` | 从屏幕 DC 拷该窗口矩形 |
| `duplication.frame` | `desktop` | 整幅桌面帧按矩形裁（裁小了也还是桌面像素） |
| `screen.wgc` / `screen.bitblt` / `screen.duplication` | `desktop` | 整块屏幕此刻的样子 |
| `unknown` | `desktop` | 说不清来路 = 按最危险的处理 |

### 调用方（AI）该怎么做

- **可靠窗口截图带 `--yes`**（`wgc` / `printwindow` / `dwm` 缩略图这三条窗口内容路径）：不然脚本会卡在没人能点的框上，
  而 `--yes` 又确实管不到桌面那一级，所以该带就带、不该指望它就别指望。
- **脚本 / AI 调用给 `--timeout-ms`**（例如 5000）配 `--yes`（窗口内容路径）：目标 UI 线程挂死也拖不垮调用方。
  `--yes` 照旧管不到桌面路径（`bitblt` / `duplication` / 任何整屏 / `dwm` 的屏幕退路）——那些一定弹框，
  现在还可能超时成 `capture.consent_timeout`。等人工确认不消耗 `--timeout-ms`，人答得慢不会把自动阶段预算吃光。
- **会拍到别家窗口时先向用户说明范围**（要 `bitblt` / `duplication`、要整屏、或 `auto` 有可能退到桌面路径），
  启动之后**等用户本人在框上点「是」**。
- **不得用脚本、`SendMessage`、UI 自动化代点**那个框——代点等于替人做了这个决定。
- **用户拒绝不是技术故障，不得重试**：`capture.access_denied` 就停下来问用户怎么办；
  `capture.consent_unavailable` 是"那个会话里根本没有人能答"（服务、计划任务、锁屏），要做的是换会话而不是再弹一遍。
- 读到图先看 `images[].scope`：`desktop` 就意味着这张图里可能出现别人的窗口、文档、通知，
  转述与存档时按这个来说；别只看 `source` 就断定"截的是那个窗口自己"。
- **`capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable` 一律重新枚举、重新选目标**，
  不要换通道重试，也不要把条件放宽一点再试一次：这三条说的是"选定之后目标已经不是那一扇了"，而工具不会拿先前批准的
  许可去截一个后来的新对象。`capture.target_changed` 常常是因为目标改了标题、不再满足你给的 `--title*` 条件 ——
  这时该重新确认一次"要截哪个窗口"，而不是假定它还是同一个东西。

### 确认框与诊断

- 框上写什么：默认焦点在"否"（回车不会误批）；列出目标及其屏幕区域、请求的通道**加实际走的那条内部路径**、
  展开后的绝对输出路径（或"标准输出"）、这一级会不会把别的窗口拍进图；桌面那一级还明确写着 `--yes` 对它不生效。
- 点"是"之后工具等约 1 秒才取帧——框的关闭动画还在 DWM 画面上时立刻截会拍到残影；框一定在第一帧之前就没了。
- 答"否"或把框关掉 → `capture.access_denied` + 退出码 6、`stage=consent`。
  在 `--consent-timeout-ms` 之内没有人回答 → `capture.consent_timeout` + 退出码 6、`stage=consent`：
  **超时按"拒绝"处理，绝不当作同意**，之后剩下的采集同样停止。这段等待单独计时，不吃 `--timeout-ms` 的预算。
  框根本弹不出来 → `capture.consent_unavailable` + 退出码 6、`stage=consent`：**这不是人说了不**，
  下一步是换个有交互桌面的会话，而不是再问一遍。三条都带 `target`、`backend`（通道名）、`value`（实际那条路径名）。
- 批准之后目标又挪了位置或变了大小 → `capture.consent_stale` + 退出码 7、`stage=capture`：
  这一张不取，可重试（重新选定目标，再让人确认一次）。
- **任何一次拒绝之后，这一次请求剩下的截图全部停止**：不换后端、不重试，之前已经写好的图留着。
- 不给输出路径那条"偷懒路径"上，**确认被拒不再塌成 `cli.missing_output`**（其余失败照旧会塌）。
  所以"是不是被人拒了"直接看 `errors[].code` 就知道，不必为了看清而先补一个 `--out`。
- 注定不弹框、也不截图的情况：`--help`、`--version`、不给任何条件（退出码 2 —— `--yes` **不是**选择条件，
  光给它绝不等于"那就顺手拍张桌面"）、无匹配（4）、多匹配（5）、解析期错误（1）、
  输出名规划失败（如 `io.output_collision` 8）、以及 `--dry-run`。
- 诚实边界：普通 MessageBox 只是合作式自动化的误操作防护，不能鉴别人类点击，也挡不住同权限存心绕过的进程。

## 期限与阻塞隔离（`--timeout-ms` / `--consent-timeout-ms`）

**自动阶段的预算整批只发一份**：目标选定之后，匹配（含 `--title-regex` 求值、取挂死窗口的标题）、
`auto` 的后端重试、等帧、编码、写文件 / 写 stdout 共用同一份剩余时间，没有哪一步或哪个目标能另领一份。
耗尽时受影响的那张图**不落地**，按阶段给码：`match.timeout`（`stage=match`）/ `capture.timeout`
（`stage=capture`，编码超时也算它）/ `io.timeout`（`stage=write` / `stdout`，退出码 8）。
剩下的目标不再开始，已经写好的图留着（同「部分成功」规矩）。省略或 `0` = 不设总预算，但即便如此，
被隔离进辅助进程执行的那几步仍有内置 5000 ms 上限兜底，`printwindow` / `dwm` 不再能无限期卡住。

等人工确认**不计入**这份预算：`--consent-timeout-ms` 单独给确认框限时，超时按拒绝处理
（`capture.consent_timeout`，见上一节），绝不因为"没人反对"就当同意。

### 阻塞隔离（内部机制，不是公开选项）

- `printwindow`（这条通道本身，以及 `dwm` 回读的那次 `PrintWindow`）与**用了 `--title-regex` 或设了预算时的窗口匹配**，
  现在跑在一个隐藏的同 EXE 辅助进程里。期限到点，父进程只结束**它自己起的**那个辅助进程；
  目标应用的窗口从来不会被杀，也不会留下孤儿 worker（作业对象 + 管道 + 空闲看门狗）。
- 调用方要知道的：**没有公开的 `--worker` 入口**，它不能被用来绕开确认框，辅助进程永远不读桌面像素，
  辅助进程内部的退出码**不属于契约**。
- 这套机制自己的失败（辅助进程起不来 / 管道断了 / 消息对不上协议 / 任务不合法，文案是 `cap.worker.*`）
  统一以 `capture.worker_failed` + 退出码 7 报出来，带 `stage` / `backend`；`hint` 里附辅助进程最后那个退出码。
  它与 `capture.failed` 分开给码，是因为下一步不同：这条要查的是执行环境（权限、策略、杀软），
  而不是"目标窗口是不是受保护"。
- `--title-regex` 写出灾难性回溯的模式现在是**照实说**的：`cli.invalid_regex` + 退出码 1、`stage=match`，
  消息讲的是回溯复杂度（正则引擎自己的复杂度上限）。**加大 `--timeout-ms` 没有用**——改写模式，或者用 `--title-contains`。

### 诚实边界（这些是限制，不是保证）

- 预算在**可中断点**和"杀掉辅助进程"这两处生效。没有取消点的阻塞系统调用——原子写文件那几步、
  往堵住的标准输出管道里写、无视取消请求的 WinRT 编码器——是**开始前检查预算、结束后再计时**，
  不会在调用中途被抢占。
- 实测 Win10 19045 上 `PrintWindow(PW_RENDERFULLCONTENT)` 从 DWM 缓存的合成面渲染、根本不发 `WM_PRINT`，
  所以"目标卡在 `WM_PRINT` 里"这个场景在那里拖不住父进程；会等目标线程的是**不带 flag 的那次退路
  `PrintWindow`**。别宣称卡死场景在每个 Windows 版本上都可达。

## JSON 结构

```json
{ "captured": 1,
  "images": [ … 每条都带 path / scope / rect，见下两节 ],
  "errors": [ { "code","message","option","value","hint","target","backend","stage","hresult","win32" } ],
  "notes":  [ 同 errors 的形状 ],
  "input":  { … 仅 --verbose } }
```

- `captured` 与 `images` 恒在（空时 `[]`）；`errors` 只要非空就必须出现；`notes` 仅非空且未 `--quiet`；
  `input` 仅 `--verbose`。**为空的字段整个键省略，不输出 `null` 占位。**
- `images[]` 里的 `path` / `scope` / `rect` 是**隐私判据**（这一帧出自哪条内部路径、像素是窗口自己的还是屏幕上那块区域、
  当初批准采样的是哪一片），`--quiet` 不抑制它们——`images` 整个数组从来不会被抑制。
- 输出里**不含**工具名、版本、schema、stage、参数回显之类的元信息。
- 通道分配：默认全部走 stdout、stderr 为空；一旦图片占用 stdout（`--out -` 或没给输出路径），
  **整份 JSON 改走 stderr**，两个通道永不混流。这条判断在任何图片写出之前就定下，连"渲染结果本身抛异常"
  的兜底诊断也跟着它（一律 stderr，工具不为此再解析一遍命令行）。**约定那条流写不出去就是失败**：
  退出码 8，即使另一条流补发成功也不留成原来的值。
- 文本输出只有三种情况：`--help`、`--version`、没给任何条件。

### 窗口图（`images[]` 每一项）

`file` `bytes` `width` `height` `format` `source`（真正出图的那条通道）`path`（实际走的那条内部路径名）`scope`（`window` / `desktop`，由 `path` 算出）`rect`（`{"x","y","width","height"}`，那次授权允许采样的屏幕区域）`requestedRect` / `capturedRect` / `clipped` / `rotation`（只有从整幅桌面帧裁目标的通道会写，见下面那段）`hwnd`（`0x…` 字符串）`pid` `title` `class` `image`（映像文件名）`elapsedMs`

**取帧位置的四个键（定位判据，`--quiet` 也不许藏）**：`duplication`、以及 `bitblt` / `dwm` 的屏幕取样那几条，是从一整块
输出的画面里把目标裁出来的，所以它们额外写 `requestedRect`（这条通道本来要截的那一块，虚拟屏幕坐标）与
`capturedRect`（实际截到的那一块，同一套坐标），两者不一样时才有 `clipped: true`，并同时发一条
`note.capture_clipped`（`hint` 给四边各少了几像素）；`rotation` 只在交付前把桌面帧顺时针转过（`90` / `180` / `270`）
才写，缺席 = 没转过。**跨屏窗口只会截到与它重叠最多的那一块输出**，剩下的部分不在图里 —— 别把这张图当成完整窗口，
先看 `clipped`。窗口内容那几条（`wgc` / `printwindow` / `dwm.thumbnail`）截的就是整个目标，这四个键一个都不出现。

`width`/`height` 是**取到那一帧时**画面的实际尺寸，不是选窗口那一刻记下的尺寸。`wgc` 尤其如此：它读每帧自带的
`ContentSize`，窗口在"选中"与"取帧"之间被缩小就只交有效那块（不会多出一圈没定义的边缘），被放大到超出采集帧池
就在 `--timeout-ms` 预算内重建帧池再取一帧——总之绝不交一张被裁掉却按整窗宣称完整的图。追不上或形状自相矛盾时
按 `capture.frame_timeout` / `capture.frame_invalid` 报告，而不是给一个尺寸不对的"成功"。

### 屏幕图（`--monitor` 且无窗口条件时换这一组字段）

`file` `bytes` `width` `height` `format` `source`（同上）`path`（`screen.wgc` / `screen.bitblt` / `screen.duplication`，三条都是桌面）`scope`（`desktop`）`rect`（那次授权允许采样的屏幕区域，= 那块屏的矩形）`requestedRect` / `capturedRect` / `rotation`（`duplication` 这条会写，见上面那段；整屏本该 `capturedRect` 等于 `rect`，裁不全就直接报 `capture.monitor_changed` 而不是交一张偏小的图）`monitor`（编号）`device`（`\DISPLAY1` 之类）`primary`（布尔）`elapsedMs`

没有窗口可归属，所以 `hwnd` / `pid` / `title` / `class` / `image` 整个不出现——调用方按 `monitor` 是否存在区分两种图。
`source` 两种图都有：`--capture auto` 回退成功时它写的是链上实际命中的那一条，不是请求值 `auto`。
`path` / `scope` / `rect` 两种图也都有，取值全表见「截图授权」一节：

- `scope` 由 `path` 算出来，所以两者同源、永远不会各说一套（`dwm` 这一条通道可能是 `dwm.thumbnail`
  也可能是 `dwm.screen`，光看 `source` 分不出来）。
- `rect` 是那条路径被授权采样的那块屏幕区域（物理像素）。窗口目标给的是**整个窗口框架矩形**，
  可能比图片大几个像素（图会按 DWM 扩展边框裁掉透明边）；屏幕目标是那块屏的矩形。量不出来时整个键省略。

### 诊断项的定位字段（只在真拿到值时才出现）

| 字段 | 含义 |
| --- | --- |
| `target` | 哪个目标：窗口给 `0x…` 句柄（与 `images[].hwnd` 同形），屏幕给设备名（如 `DISPLAY1`） |
| `backend` | 哪条通道；`auto` 全链失败时列出真实试过的那几条，而不是 `auto`。授权类诊断（`stage=consent`）给的是**通道名**（`bitblt` / `dwm` / `wgc`…） |
| `stage` | 哪一步：`parse` / `match` / `plan` / `consent` / `capture` / `encode` / `write` / `stdout` / `report`。`match` = 目标匹配求值这一步（`match.timeout`，以及回溯复杂度版的 `cli.invalid_regex`），`consent` = 人工确认这一关（答"否"、弹不出、`--consent-timeout-ms` 内没人答），`capture` 里也可能出"批了之后目标挪了位置"（`capture.consent_stale`） |
| `value` | 出错那个取值/名字；**在 `stage=consent` 的授权诊断上它是内部路径名**（`bitblt.screen` / `dwm.screen` / `screen.wgc`…），与 `backend` 的通道名分开发，所以调用方既能按通道分支、也看得见实际走了哪条支路 |
| `hresult` | 形如 `0x80070005` 的原值（照实传，不会被 `E_FAIL` / `E_NOINTERFACE` 顶掉） |
| `win32` | `GetLastError` 的原值（数字，0 不写） |

这几个不随 `--lang` 变，`message` / `hint` 才变。于是"用户拒绝"（`capture.access_denied` + `stage=consent`）、
"没有人能答"（`capture.consent_unavailable` + `stage=consent`）、"批了之后画面已经变了"
（`capture.consent_stale` + `stage=capture`）与"技术性访问被拒"（`capture.failed` + `hresult=0x80070005`）能分开判；
`capture.worker_failed` 说的是本工具自己的辅助进程没能跑起来，与前两者都不同；
黑帧只报"没拿到内容"，不断言成 DRM，也不写成 `capture.access_denied`。
**单色同样不等于采集失败**：整帧逐像素比过之后确实只有一个颜色时，图照常交付，只另发一条质量提示`note.frame_uniform`（`message` 里给那个颜色 `0xAARRGGBB`，带 `backend` / `target` / `stage=capture`，`--quiet` 会把它连同整段 notes 一起去掉）。只有 `duplication` 会因此拒绝一帧，而且要两条一起成立才判失败：这一帧**没有任何 present 记录**（`LastPresentTime` 与 `AccumulatedFrames` 都是 0）且整幅只有一个颜色——有 present 记录的单色就是屏幕上此刻的样子（单色壁纸、纯色窗口）。

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
| 6 | 这次截图没拿到人的同意：人在确认框上答"否"或把框关掉（`capture.access_denied`），那个会话根本没有可交互的桌面、框弹不出来（`capture.consent_unavailable`），或在 `--consent-timeout-ms` 之内没有人回答（`capture.consent_timeout`）；也包括目标受保护 |
| 7 | 截图失败（含 `--timeout-ms` 预算耗尽的 `match.timeout` / `capture.timeout`，也含身份复核没过的 `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`） |
| 8 | 写文件失败（也含结果 JSON 没送到约定那条流，以及预算耗尽落在写/stdout 阶段的 `io.timeout`） |
| 9 | 内部异常 |

退出码与 body 是两套独立信号：先看 `errors`，再看 `captured`，最后才用退出码做粗分支。

- `io.write_failed`、`io.file_exists`、`io.output_collision`、`io.timeout` 与"结果送不到约定流"给出 8；截图/编码阶段的其它失败（含
  `capture.failed`、`capture.timeout`、`capture.encoder_unavailable`、`capture.consent_stale`、`capture.worker_failed`）都给 7；`capture.access_denied`、
  `capture.consent_unavailable` 与 `capture.consent_timeout` 给 6（都算"这一张没人批准"，但下一步动作不同：
  有人答了否 / 那里没有桌面可弹 / 人在 `--consent-timeout-ms` 之内没答）。
- 某个后端抛异常（而不是返回失败）只作废它所在的那一个目标：前面成功的图留着，剩下的目标照旧继续；
  内存耗尽与显卡设备被移除这类"换后端也不会有区别"的错误会明确终止整批。
- **部分成功**：`--all` / `--monitor all` 里某些目标失败时，已写出的图照样在 `images` 里
  （`captured` 可以大于 0），但退出码仍是 7。所以"退出码非 0"不等于"什么都没拿到"。

## 诊断码全表

只按 `code` 分支。码值只增不改名。

**`cli.*`（解析期，全部退出码 1）**
`cli.unknown_option` `cli.missing_value` `cli.switch_takes_no_value` `cli.invalid_number`
`cli.invalid_regex` `cli.invalid_value` `cli.invalid_format` `cli.unrecognized_extension`
`cli.unexpected_positional` `cli.missing_output` `cli.duplicate_output` `cli.conflicting_options`
`cli.unknown_capture_method` `cli.unknown_language` `cli.monitor_conflict` `cli.internal_error`
`cli.stdout_multiple_targets`（stdout 一次只交付一张图，实际目标多于一个；整批没截也没写，也不弹框）
`cli.no_condition`（→ 文本帮助 + 2）
`cli.invalid_regex` 还有匹配期这一处：模式撞上正则引擎的回溯复杂度上限（`stage=match` + 1，消息说的就是回溯复杂度）——
**加大 `--timeout-ms` 没有用**，改写模式或换 `--title-contains`

**`match.*`**
`match.no_window`（4）`match.ambiguous_window`（5）`match.index_out_of_range`（1）`match.monitor_out_of_range`（1）
`match.timeout`（7，`--timeout-ms` 预算在窗口/屏幕匹配途中耗尽——含正则求值或取挂死窗口的标题，`stage=match`；
加大预算或简化条件）

**`capture.*`**
`capture.access_denied`（6，人在确认框上答"否"或把框关掉——窗口内容路径没带 `--yes` 时也要弹，所以这一条不再只代表整屏。
受保护内容不给这个码：它表现为黑帧，由 `capture.failed` 一类照实说"没拿到内容"）
`capture.consent_unavailable`（6，确认框根本弹不出来：服务会话 / 计划任务 / 锁屏，那里没有交互桌面，
**不是人说了不**——该换会话，而不是把同一个框再弹一遍）
`capture.consent_timeout`（6，`--consent-timeout-ms` 之内没有人回答确认框：按拒绝处理而**不是**同意，
之后剩下的采集同样停止，同 `capture.access_denied`）
`capture.consent_stale`（7，批准之后目标又挪了位置或变了大小，要取样的矩形已经不在人批准的那一片里：
这一张不取，可以重新选目标再问一次）
`capture.unsupported`（1，屏幕模式配 `dwm`/`printwindow`）
`capture.encoder_unavailable`（7）`capture.failed`（7）`capture.worker_failed`（7，`cap.worker.*` 那组辅助进程机制的失败：
起不来 / 管道断 / 协议不符 / 任务不合法，`hint` 里附辅助进程最后那个退出码）
`capture.timeout`（7，`--timeout-ms` 在取帧或编码阶段耗尽，`stage=capture`：`printwindow`/`dwm` 多半是目标
UI 线程挂死，同后端重试还会超时——换 `wgc` 或加大预算）
`capture.frame_timeout`（7，等帧超时：等一下可以重试）`capture.window_gone`（7，目标已经没了：要重新枚举窗口）
`capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`（都是 7，`stage=capture`，三条讲的都是
**选定之后、取帧之前**目标已经不是那一扇窗口了 —— 与上面那条 `capture.window_gone` 的分别在于发现得有多早：那三条是
一个像素都还没读就被判掉的）：句柄被销毁 = `capture.target_gone`；这个句柄值现在属于另一个对象（归属进程变了、
同一 PID 上是另一个进程、类名变了），或者它已经不再满足当初挑中它的那个条件（`--title` / `--title-contains` /
`--title-regex` / 按 `--monitor` 那块屏）= `capture.target_changed`；有一道判据问不出来（读不到进程信息、条件求值
没能跑完）= `capture.target_unverifiable`。调用方的下一步一律是**重新枚举、重新选目标**：这三条都不是"换一条通道
再试"的理由，工具也不会拿旧许可去截一个新对象，更不会放宽条件替你另找一个长得一样的窗口。`machine` 细节以 ASCII
形式写在 `message` 里（例如 `pid 1234 -> 5678 (handle reused)`），不随 `--lang` 变
`capture.frame_invalid`（7，交回来的那帧像素自己说不通：宽高为 0、单边超过 16384 像素、行距装不下一行像素
（`< width*4`）或超过两倍行长、缓冲区比 `行距×高` 还短、整帧超过 1 GiB。裁剪 / 行重排 / 单色判定 / 编码之前都先核
这一道，所以坏帧不会被告知"成功"，也不会被读越界。上限与实际数字写在 `hint` 里，`stage` 是出问题那一步）
`capture.monitor_changed`（7，那块屏在本次请求里已经不在这台机器的桌面中，或它的画面（分辨率 / 旋转 / 位置）在人工
确认之后变了：下一步是**重新枚举显示器并重新确认**，不是换一条通道碰运气——另一块屏的画面谁都没批准过。本工具
绝不会静默改截别的屏幕，授权始终绑在人看过的那一块上。屏幕目标裁不出与该屏等大的图、以及桌面复制的
`AcquireNextFrame` 回 `DXGI_ERROR_ACCESS_LOST` / `NOT_FOUND` 都走这一条码）

**`io.*`**
`io.write_failed`（8，临时文件建不出来 / 写或刷新中断 / 提交为目标名失败）`io.file_exists`（8，配合 `--no-overwrite`）`io.output_collision`（8，整批输出名撞车，一张都没截也没写）`io.timeout`（8，`--timeout-ms` 在写文件 / 写 stdout 阶段耗尽，`stage=write`/`stdout`：已截好的那一张也不写）

**`note.*`（不是错误，`--quiet` 会去掉）**
`note.dry_run` `note.capture_channel`（`auto` 回退后实际用了哪条）`note.duplicate_value`
`note.extension_appended` `note.exe_path_looks_like_name`（`--exe` 传的像文件名不像完整路径）
`note.format_extension_mismatch` `note.format_defaulted_png` `note.output_defaulted_stdout`
`note.output_extension_appended` `note.quality_ignored` `note.all_without_placeholder`
`note.flag_overrides_quiet` `note.pipe_default_format` `note.json_flag_deprecated` `note.frame_uniform`（这一张整幅只有一个颜色：质量提示，图片照常交付）`note.capture_clipped`（目标没被完整截下来：`message` 给"要截多大 / 只截到多大"，`hint` 给四边各少了几像素。图照常交付、退出码不变，配 `capturedRect` / `clipped` 一起看）
`note.help_ignored_arguments`

## 帧的形状与像素上限

一帧 BGRA8 像素要同时满足：宽高都不为 0、单边不超过 **16384 像素**（D3D11 纹理边的上限，GDI 建位图与辅助进程
的管道协议用的是同一条线）、行距落在 `width*4` 与 `2*width*4` 之间（下界是"装得下一行像素"，上界给 GPU 的
对齐填充留一倍余量）、整帧缓冲不超过 **1 GiB**、缓冲区至少 `行距×高` 字节。任何一条不成立就报
`capture.frame_invalid`（7），而且是在**分配之前**判出来的——不靠"分配失败抛异常"当检查。裁剪、行重排、单色判定、
编码、GPU 拷回 CPU 与辅助进程交回的帧都过同一道检查。超出这条上限的截图（比如拼到 16384 像素以上的虚拟屏幕）
会照实报错，不会截半张。本工具不缩放图像，所以没有"改小一点就能过"的选项。

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

`--all` 的输出名里没有占位符时会自动追加 `_序号` 并发 `note.all_without_placeholder`。**模板里已有 `%` 时不再追加**，所以占位符分不开目标就得由调用方负责：整批名字在确认框与取帧之前一次算完，任意两个撞车（绝对路径、不区分大小写、逐码元比较）就报 `io.output_collision`+8，一张也不会落地。`%d` / `%t` 一批发一次时钟，跨午夜也不会劈成两天。8.3 短名、硬链接、junction / 符号链接、UNC 与盘符这类字符串比不出来的别名，预检认不到，只能由提交那一次原子操作当场判定。`--out -` 不展开也不查碰撞，而且**一次只交付一张图**：命中多个目标时整批在确认框与取帧之前就报 `cli.stdout_multiple_targets`+1，不会把多张 PNG 首尾拼进同一条流。

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
  `ECAPTURE.EXE --process notepad.exe --yes --out - 1> shot.png 2> result.json`
  （`--yes` 是窗口内容路径的免问开关；不给它，这一条会先弹框等人点，脚本就卡在那里了）
- 输出目录必须**已存在**，工具不建目录。
- 控制台代码页不是 65001 时中文照样正常（工具直接写 UTF-8 字节 + CRLF），但**别用 `Write-Host` 之外的
  管道去二次编码**。

## 常用配方

```powershell
# 1) 先看命中谁（不写文件，但 --out 必须给；--dry-run 在任何确认框之前就返回，不打扰人）
ECAPTURE.EXE --process notepad.exe --dry-run --out D:\shots\_probe.png

# 2) 精确锁定一个窗口：默认 wgc 是窗口内容路径，带 --yes 才真的不弹框
ECAPTURE.EXE --title LocalSend --class UnityWndClass --yes --out D:\shots\game.png

# 3) 多匹配：按 hint 里的句柄回来点名（这条没给 --yes，所以照样弹一次"是/否"框）
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite D:\shots\one.png

# 4) 每个命中窗口各一张（授权不跨请求缓存，所以每次调用都要重新带上 --yes）
ECAPTURE.EXE --pid 12345 --title-contains 报告 --all --yes --out "D:\shots\rpt_%i.png"

# 5) 图片进管道（JSON 于是在 stderr；stdout 一次只一张，多个目标请写到文件）
ECAPTURE.EXE --process notepad.exe --yes --out - > D:\shots\snap.png

# 6) 整屏 = 桌面像素：一定要人本人点"是"，--yes / --quiet / 环境变量都跳不过。
#    先把"会拍到什么范围"说清楚再启动，然后等用户点；被拒不再塌成 cli.missing_output，
#    所以不补 --out 也看得见 capture.access_denied（弹不出框则是 capture.consent_unavailable）
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"

# 7) 按屏过滤窗口（出的是窗口图字段，走窗口那一级：带 --yes 才不弹框，不给照样问一次）
ECAPTURE.EXE --monitor 2 --process chrome.exe --all --yes --out "D:\shots\m2_%i.png"

# 8) 只要屏幕上此刻的样子（连遮挡物一起要）：bitblt 整条都是桌面路径，一定弹框，--yes 在这里不起作用
ECAPTURE.EXE --class CabinetWClass --index 1 --capture bitblt --out D:\shots\visible.png

# 9) auto 带 --yes：窗口内容那三条不问，一旦要迈进桌面路径照样弹框；拿到图看 images[].path / scope 才知道走了哪条
ECAPTURE.EXE --class CabinetWClass --index 1 --capture auto --yes --out D:\shots\auto.png

# 10) 给英文环境的人看诊断文字
ECAPTURE.EXE --process notepad.exe --yes --out D:\shots\a.png --lang en

# 11) 脚本里不许卡死：自动阶段一份总预算，耗尽给 match/capture/io.timeout（图不写、好图留着）；
#     等人工确认另计，超时算拒绝（capture.consent_timeout，退出码 6）
ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --out D:\shots\epad.png
ECAPTURE.EXE --monitor primary --consent-timeout-ms 60000 --out D:\shots\screen.png
```
