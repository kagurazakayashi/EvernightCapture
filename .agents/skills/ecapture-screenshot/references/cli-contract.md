# ECAPTURE 调用契约（v0.4.0）

本文是给调用方的参考。选项清单的**权威来源始终是 `ECAPTURE.EXE --help`**（退出码 3），
本文只在它之外补充截图授权的两级判据、期限与阻塞隔离、JSON 字段、诊断码全表、命名占位符和各 shell 的坑。

## 全部选项

| 选项 | 短写 | 取值 | 说明 |
| --- | --- | --- | --- |
| `--monitor` | `-m` | `[<n>\|primary\|all\|device:..\|id:..]`，可省略（= 主屏） | 截哪块屏。编号**只认十进制**、1 起（1..65535）—— 它是**本次进程这一次 `EnumDisplayMonitors` 枚举里的位置**，不保证等于「显示设置」里写的标识号，插拔显示器或改分辨率之后同一个编号可能指到另一块屏，所以别把编号当跨调用的屏幕身份。**要点名某一块屏，先跑 `--screens` 列出本机此刻的标识，再原样写回来**：`--monitor device:DISPLAY1`（本次桌面连接的设备名，带不带 `\\.\` 前缀都认）或 `--monitor id:<监视器 devnode 设备接口路径>`（跨会话那一条，也是本工具推荐存下来做"同一块屏"的那一条）。前缀只认 `device` 与 `id` 这两个词，冒号后面不能空着（`cli.monitor_selector_empty`+1），前缀不认识报 `cli.monitor_selector_kind`+1；认不得的前缀**不算**这条选项的取值，所以 `--monitor D:\a.png` 里那条路径照旧是输出路径。标识定位不到唯一一块屏时三条码各归一种：本机没有 = `match.monitor_unknown_id`（4）、命中多块 = `match.monitor_ambiguous_id`（5，候选全列出、不替你挑）、那一问没答案 = `match.monitor_id_unverifiable`（7）——三条都不会退化成"那就用主屏"。`all` = 每块屏各一张。**给了它且没有窗口匹配条件 = 整屏截图（桌面像素，必弹确认框，`--yes` 跳不过）**；`--monitor <n>` 配窗口匹配条件 = 只算与该屏有重叠的窗口，出的是窗口像素，所以走窗口那一级（带 `--yes` 才不弹框，不给 `--yes` 照样问一次）。`all` 与任何窗口**匹配**条件互斥（`cli.monitor_conflict`+1），但 `--all`/`--index` 这类消歧选项不算匹配条件、可以搭配。省略取值时不吃后面的参数，判据与解析同源：`--monitor out.png` 里 `out.png` 仍是输出路径，而 `--monitor 1e3` 是写坏的编号，报 `cli.invalid_number` 而不会被改成输出文件名。屏幕目标在取帧之前会按**身份**重新核对那块屏（选定当时问得到跨会话标识就按它核，问不到才照旧按设备名核）：已经不在了、或者那个设备名已经发给了另一块面板，就整个不截（`capture.monitor_changed`），复核本身问不出答案时报 `capture.monitor_unverifiable` 而不是退回按名字截，尺寸或位置变了就以新矩形重新向人确认，旧授权不会被用在新尺寸的屏上 |
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
| `--cursor` | | `default`（默认）/`include`/`exclude` | **画面里要不要鼠标指针**。判据是那条路径的**来源像素里有没有光标**，不是通道名字：`wgc` / `screen.wgc` 有一条真能设进去、也能读回来核实的开关（`IGraphicsCaptureSession2::IsCursorCaptureEnabled`，**要 build 19041**，比 `wgc` 通道自己的 18362 还高）；`printwindow` / `dwm.thumbnail` / `dwm.screen` / `bitblt.screen` / `screen.bitblt` / `duplication.frame` / `screen.duplication` 交回的画面里根本没有光标（窗口自绘、DWM 重定向面、屏幕 DC，以及桌面复制那份当独立元数据交回的指针形状）。所以：`include` 配后面那几条在**解析期**就报 `capture.cursor_unsupported`+1（`option`=`--cursor`、`value`=规范化取值，`message` 里带实际通道与做得到的那一条），**绝不改走去读桌面像素的通道**——那既加不回光标，交回的也是一份没人批准过的画面；`--capture auto` 时兑现不了的那几条从链里摘掉并各留一条 `note.cursor_channel_skipped`（`backend`=被摘的那条，`message` 末尾是 ASCII 原因 token：`os_below_min_build:19041` / `dwm_redirection_surface` / `screen_dc_has_no_pointer` / `pointer_shape_is_separate_metadata` / `window_self_drawn` / `not_registered`），摘到一条不剩就是 `env.cursor_unsupported`+7，一个像素都不取、也不弹框。取值只认那三个词（忽略大小写与首尾空白，内联 `--cursor=exclude` 也认），写别的报 `cli.invalid_value`+1 而不退化成 `default`；重复给出最后一个生效；`-v` 回显 `input.cursor` 与 `input.cursorGiven`。**默认值真的不动任何东西**：没写这条选项时不碰任何开关、结果里那三个键一个都不出现。写了 `--cursor default` 是"不要求改动，但把读到的状态报出来"。本工具**从不**用图像修补去加或去抹光标（不取指针形状来合成、不画光标、也不动使用者的鼠标），所以 `cursorEffective` 只断言到"这条会话被设成画/不画"或"这条路径的来源没有光标"那一层，**不**断言这一张图里此刻看得见或看不见指针。这条选项不改变授权：`exclude` 配 `bitblt` / `duplication` / 任何整屏照样一定弹框，`--yes` 照样管不着 |
| `--hdr` | | `auto`（默认）/`tonemap`/`refuse` | **HDR 来源怎么处理**。显示器在 HDR 模式时采集回来的帧可能带超出 SDR 的亮度范围与另一种传递函数（WGC 可按 FP16 scRGB 线性交回；桌面复制的桌面纹理可能是 FP16 scRGB 或 10 位 ST.2084 (PQ) / HLG BT.2020）。把它硬按 8 位 BGRA 解释会得到一张发白、去饱和、亮部一团糊却"看着像正常图"的结果——本工具不把这当成正确的默认交付。判据是那条路径的**来源带不带得回广色域帧**，不是通道名字：`wgc` / `screen.wgc` / `duplication.frame` / `screen.duplication` 带得回（来源跟随显示模式）；`printwindow` / `dwm.thumbnail` / `dwm.screen` / `bitblt.screen` / `screen.bitblt` 结构上只有 8 位 SDR。`tonemap` = 在编码之前把广色域帧经一份逐像素浮点中间量按固定曲线映射成 8 位 sRGB 交付（不分配整幅浮点帧；来源本就是 SDR 时是恒等透传）；`refuse` = 核实来源确是 HDR 就报错、一个像素都不落地；`auto`（默认）= 本工具对色彩一个字都不改。**做不到的那条就拒绝、绝不改走去读桌面像素的通道**：`tonemap` / `refuse` 配 `printwindow` / `dwm` / `bitblt` 在解析期报 `capture.hdr_unsupported`+1（`option`=`--hdr`、`value`=规范化取值、`message` 带实际通道与做得到的那两条）；`refuse` 核实是 HDR = `capture.hdr_refused`+7；带回一个认不出的广色域格式 = `capture.hdr_unverifiable`+7。与 `--cursor` 不同：这条**不摘链**，`--capture auto` 也在解析期放行（落到哪条通道要到运行期才知道）。探测显示 HDR 状态走只读的 `IDXGIOutput6::GetDesc1`，绝不改显示设置，问不出来给 `unknown` 不猜。取值只认那三个词（忽略大小写与空白，内联 `--hdr=tonemap` 也认），写别的报 `cli.invalid_value`+1 而不退化成 `auto`；重复给出最后一个生效；`-v` 回显 `input.hdr` 与 `input.hdrGiven`。**默认值真的不动任何东西**：没写这条选项时不改采集格式、结果里色彩那组键（`hdrRequested` / `hdrEffective` / `hdrBasis` / `sourceColorSpace` / `sourceBitDepth`）一个都不出现。写了 `--hdr` 才报这一帧实际的来源色彩空间、位深与经历的处理；`--hdrEffective` 只说这台机器把 HDR 映射成了 SDR（`tone_mapped`）或来源本就是 SDR（`sdr_passthrough`），**不**说这台机器验过色彩正确性（本项目无 HDR 屏，`--capabilities` 的 `color.verifiedOnThisMachine` 恒 `no`）。这条选项不改变授权：会读桌面像素那几条照样一定弹框、`--yes` 照样管不着 |
| `--roi` | | `<x,y,w,h>` | **窗口内部裁剪**：从交付的整窗图像里裁出 x,y 起点、w×h 大小的一块。原点 `(0,0)` 是**这张图像自己的左上角像素**（图像对应的是用户看到的可见边框，`GetWindowRect` 还算在内的 DWM 透明 resize 边框不在里面），单位物理像素且**不按 DPI 缩放**（本进程 per-monitor v2，要按逻辑像素指定就自己乘缩放） —— 所以这四个数永远不会被当成桌面绝对坐标。四段都只认 `[0-9]+`、逗号分隔、不认空白/正负号/小数点/指数/下划线/`0x`与非 ASCII 数字；`x`/`y` 可为 0，`w`/`h` 至少 1，四条都不超过 16384（= 帧的单边上限，`--capabilities` 报成 `limits.roiMaxValue`）。写法不合 = `cli.invalid_value`+1。放不下 = 拒绝，绝不往里挪、裁到边上为止、也不退回整窗交出：取帧之前就看得出放不下报 `match.roi_out_of_range`+1（在确认框与输出名规划之前，不弹框、不写文件），取到帧之后才发现报 `capture.roi_invalid`+7（目标改了尺寸或被屏幕边缘裁短）。与 `--client-area` 互斥（`cli.crop_conflict`+1），与整块屏幕的目标说不通（`capture.unsupported`+1，**不会**改按桌面坐标去截），与只读查询一起给也算冲突。`--dry-run` 不取帧所以不判这条几何。结果里多带 `cropMode` / `cropRect`（图像坐标）/ `fullWidth` / `fullHeight` / `cropScreenRect`（屏幕坐标，只在图像原点核实得出来时才写，否则整个键不出现并留 `note.crop_mapping_unavailable`） |
| `--client-area` | | 开关 | 只交回窗口客户区那一块：在交付的整窗图像里再去掉标题栏与三边边框。这块矩形照目标此刻的几何量出来（`GetClientRect` + `ClientToScreen`），所以坐标系与单位跟 `--roi` 完全同一套。客户区问不出来 = `capture.roi_unmeasurable`+7，客户区有一边落在交付图像之外 = `capture.roi_invalid`+7（挂在屏外、或中途改了尺寸），两种都不退回整窗交出。`--client-area=false` 与普通开关同义 = 没写。与 `--roi` 互斥 |
| `--scale` | | `key=N[,...]` | **等比缩小**：把即将交付的这张图缩到天花板之内，**只缩不放**。三个键：`max-width=N` 限宽、`max-height=N` 限高、`max-pixels=N` 限总像素数（大小写不敏感；N 只认十进制，边长 1..16384（与 `--roi` 同一条线）、像素数 1..268435456）。可以只给一条，也可以一条里用逗号串几条；这一条写多次时每条天花板各记各的，重复给同一条时最后一个生效；一条都不给 = `cli.invalid_value`+1，**整条不生效**，不留下认得的那半。三条都按同一个比例缩（取最紧的那一条），宽高各自**向下取整**且各至少留 1 像素；本来就在天花板之内就原样交付（结果里 `scaleApplied: false`）。插值策略只有一种而且可预测：最近邻。顺序是**先裁（`--roi` / `--client-area`）后缩、再编码**，所以缩的是裁完的那一块；结果里 `scaleFromWidth` / `scaleFromHeight` 是缩之前的尺寸、`width` / `height` 是缩之后的。这一条**不改变授权与帧上限**：会从屏幕上取样的那几条照样一定弹框（`--yes` 不因为最后交的是小图而生效），`--roi` 的越界判据仍按未缩的那张图判。与任何环境查询（`cli.query_conflict`）或窗口查询（`cli.window_query_conflict`）同时给出都是冲突 |
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
| `--capabilities` | | 开关 | **只读能力查询**，输出 JSON（见「只读的能力查询」一节）。问的是这台机器现在能走哪几条路线：版本与会话条件、每条通道的 `available` / `unavailable` / `unverified`、每种格式、`--yes` 实际管到哪几条内部路径、以及各种取值上限。一个像素都不取、不弹确认框、不写文件、不联网、不读环境变量，也不需要窗口条件。只接受 `--lang` / `-v` / `-q`，与截图那一套选项或输出路径同时给出 = `cli.query_conflict` + 退出码 1 |
| `--screens` | | 开关 | **只读屏幕枚举**，输出 JSON（见「只读的屏幕枚举」一节）。列本机每块屏的编号、设备名、是否主屏、物理矩形、DPI 与旋转（问得到的话）、归属适配器关联，并写明这几种身份各自稳到哪一层；`screens[].selectors` 里的 `device:…` / `id:…` 可以直接写回 `--monitor`。一个像素都不取、不弹确认框、不写文件、不改任何显示设置。只接受 `--lang` / `-v` / `-q`，与截图那一套选项同时给出 = `cli.query_conflict` + 退出码 1 |
| `--diagnostics` | | 开关 | **只读诊断/版本查询**，输出 JSON：构建版本、可核对的构建标识（PE 链接时间戳 + 架构 + 映像大小）、平台与后端状态。字段与 `--capabilities` 出自同一个判据函数，不是第二份环境信息。默认不上传、不采集画面、不枚举用户文件，也不输出用户名、环境变量与任何路径；`-v` 追加每一问的原始答案。互斥规则同上 |
| `--list` | | `[<all>]`，可省略 | **只读的窗口发现**：把满足全部条件的顶层窗口列成结构化 JSON（见「只读的窗口查询」一节）。取值可省略，省略时不吃后面的参数（`--list out.png` 里 `out.png` 仍是位置参数并因此算冲突）；写 `--list=all` 时把最小化窗口也并进同一根 Z 序轴。取值只认 `all`，写成别的（`--list=allx`）报 `cli.invalid_value`+1 而不退化成默认策略 |
| `--inspect` | | `[<path>]`，可省略 | **只读的单窗口检查**：按与截图同一套选择策略定出的那一扇窗口，交回单个 `window` 对象（含后续截图要复核的身份约束字段）。取值写 `path` = 同时写出归属映像的完整路径（默认只写文件名，路径常含用户名）。多匹配仍报 `match.ambiguous_window`+5，不会替你挑一个。取值只认 `path` |
| `--offset` | | 十进制 0..8192 | 仅窗口查询：跳过命中列表开头 n 个。上界是"一次求值本来能拿到多少条"那道线，超过它一定是对真实命中数没意义的编号 |
| `--limit` | | 十进制 1..8192，默认 50 | 仅窗口查询：本批最多交回 n 个。命中的总数看结果里的 `pagination.matched`，不是看本批几条 |
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

## 运行环境与能力检查（三件事不要混）

| 层次 | 取值 | 判据来源 |
| --- | --- | --- |
| 各条路线的 API 历史下限 | 任何一张图 `10240`（WinRT `BitmapEncoder`，六条通道共用）· `duplication` `9200` · `printwindow` / `dwm` `9600`（`PW_RENDERFULLCONTENT`）· `wgc` `18362`（`IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor`；命名空间本身 1803 就有，但本工具不经选择器） | 微软为**该路线实际调用**的那个接口写的文档下限 |
| 本工具声明的下限 | 64 位 Windows 10 version 1903（build `18362`）以上 | 上面最高的那一条（默认通道要真能出图所要求的），不是最老的那一条 |
| 已实测的版本 | Windows 10 22H2（build `19045`）x64 | 仓库里所有真机判据都只在这一台机器上跑过 |

10240..18361 之间能装载也能截（工具按各条路线的下限筛通道，不整体拒绝），但**不在声明支持之列、也没实测过**。
Windows 7 / 8 上这个 exe **根本装载不了**：它静态导入 `api-ms-win-core-winrt-error-l1-1-1.dll`
（`RoOriginateLanguageException`，微软文档写的最低客户端就是 Windows 8.1）、`api-ms-win-core-winrt-l1-1-0`
与 `api-ms-win-core-job-l2-1-0`（Windows 8），而 API Set 这套机制在 Windows 7 上根本不存在；
这三个契约也不在 UCRT 可再分发的名单里，补不上去。`tests\compat.ps1` 会拿发布版二进制核对这些导入名，
所以"装载下限"是关于这个文件的事实而不是推测。**Windows 8.1 能装载也能启动**，在那上面起作用的就是下面那道
能力检查（Windows 10 之前没有 `BitmapEncoder`，一张图都编不出来）。所以"某个 `BitBlt` /
`DwmRegisterThumbnail` 在 Windows 7 上存在"证明不了这个程序能在那儿跑；PE 头里的 `subsystem 6.00`
同理只是链接器默认值，不是支持声明。
`tests\compat.ps1` 会拿发布版二进制核对这些导入名。

**能力检查发生的时机**：枚举窗口、规划输出名、弹确认框、读像素**之前**（版本号取自 `ntdll!RtlGetVersion`，
不是会被清单与版本伪装影响的 `GetVersionEx`）。所以环境不满足时一个像素都没读、人也不会被打扰，
之前写完的图仍然留着。

| 情况 | 码 | 退出码 | 换 `--capture` 有没有用 |
| --- | --- | --- | --- |
| 本机 build < 10240 | `env.os_too_old` | 7 | **没有**。这是这台机器整体的事，与选哪条通道、与目标都无关 |
| 显式指定的那条通道下限高于本机 | `env.channel_unsupported` | 7 | **有**。换一条或改 `auto`；重试同一个目标没有意义，工具也不会自己把你指定的那条换成别的 |
| `auto` 链里某条被挡下 | `note.channel_unavailable`（提示） | 不变 | 剩下的几条照旧退回，实际出图的写在 `images[].source` |
| 版本号问不出来 | `note.os_unverifiable`（提示） | 不变 | 这次没按版本筛过任何一条：问不出来既不等于不支持，也不等于支持 |

**不截图就能问出能力**：`--verbose` 的 `input.osBuild`（本机 build；整个键不出现就是没问出来）与
`input.captureChain`（本次这类目标实际可用的通道链，按尝试顺序；显式指定的那条被挡时是空数组）。
`--dry-run` 不取帧，所以不会因为环境判据报错。设备层面的能力（驱动不喂帧、系统拒绝 WGC、会话里没有桌面、
N 版缺媒体组件）**不由版本号预测**，那一步会交回它自己的 `capture.*` 码与真实 HRESULT
（例如 `capture.encoder_unavailable`）。

## 只读的能力查询（`--capabilities` / `--diagnostics`）

上面那道检查在每次截图之前都会跑，但只有下单之后才看得见。这两条命令是同一判据的**只读出口**：
不取一个像素、不弹确认框、不写文件、不联网、不读环境变量，也不需要窗口条件。

```powershell
ECAPTURE.EXE --capabilities        # 该走哪条路线（JSON）
ECAPTURE.EXE --diagnostics         # 出问题要提交的东西：构建标识 + 平台 + 后端状态（JSON）
ECAPTURE.EXE --capabilities -v     # 另加 probes 段：每一问的原始答案与出处 API
```

三条读法（彼此不能混）：

| 字段 | 意思 | 不是什么意思 |
| --- | --- | --- |
| `compiled` | 这个二进制里有没有实现这条路线 / 编出这种格式 | 不是"本机让不让用" |
| `status` | `available` / `unavailable` / `unverified` —— 本机**现在**的判据（版本下限 + 屏幕拓扑）让不让走 | 不是"某个窗口一定截得到"；驱动、受保护内容、HDR 都不在这层断言里 |
| `verifiedOnThisMachine` / `os.matchesTestedEnvironment` | 本项目有没有在这一模一样的系统上实测过（只有开发机那台 19045 x64） | 不是"能用"也不是"不能用"，只是"我们没在这上面跑过判据" |

**未知就写 `unknown`。** 每一条事实都是 `yes` / `no` / `unknown` 三值之一，绝不折成两边之一，也不整个键消失；
数字类问不出来时配一个 `known: false`（例如 `os.known`）。版本号问不出来时所有通道的 `status` 都是
`unverified`，而 `autoChainWindow` 仍原样列出全部四条 —— 没筛就是没筛，不等于都支持。

| 段 | 内容 |
| --- | --- |
| `contract` / `contractVersion` | 只有这两份文档带契约版本（现为 1）。**截图结果那份照旧精简**，不因此多出顶层元信息 |
| `program` | `name`、`binary`（只有 `ECAPTURE.EXE` 这个名字，不含目录）、`version`、`arch`、`buildId` |
| `os` | 本机版本三要件 + `declaredMinBuild`（对外声明下限）+ `encoderMinBuild` + `testedMinBuild`/`testedArch`（实测过的那一台）+ `matchesTestedEnvironment` |
| `session` | `attachedToConsoleSession`、`remoteSession`、`displayTopology`、`monitors`、`elevated`、`consentDialogExpected`（+ `consentDialogProbed: false`：这一条是推出来的，查询没有真去弹框） |
| `authorization` | `yesSkips: "window-content"`、`desktopPixelsAlwaysAsk: true`、`unregisteredPathScope: "desktop"`，以及整份内部路径登记表：每条带 `scope` 与 `consentWithoutYes` / `consentWithYes`（后者为 `true` 就是"`--yes` 也跳不过"） |
| `backends[]` | 每条路线的 `compiled` / `status` / `reason` / `minBuild` / `verifiedOnThisMachine` 与 `paths[]`（窗口目标与屏幕目标各走哪条内部路径；`dwm` 的屏幕退路也列出来，免得 `--yes` 被读大） |
| `formats[]` | 每种格式的 `compiled` / `status` / `reason` / `minBuild` / `registered`。`registered` 恒为 `unknown`：这一层不去实测编码器。`webp` / `ico` 以 `compiled: false` + `reason: "not_compiled"` 留在这里 |
| `cursor` | `option` / `default`（`default` = 不要求，本工具一个字都不改）/ `values` 三种取值；`switch` 那一条唯一的开关（`api` = `IGraphicsCaptureSession2::IsCursorCaptureEnabled`、`compiled` / `status` / `reason` / `minBuild` 19041 / `verifiedOnThisMachine`）；`paths[]` 每条已登记内部路径一行（`capability` 是 `settable` / `excludes_cursor` / `unregistered`，`reason` 是那条路径的根据，`include` / `exclude` 各是三值 `yes` / `no` / `unknown` —— 问不出来就是 `unknown`，不折成任何一边）；末尾 `pointerShapeCompositing: "never"` 与 `pixelRetouching: "never"` 说本工具不动指针形状、也不修图 |
| `color` | `option` / `default`（`auto` = 不要求，本工具一个字都不改）/ `values` 三种取值（`auto` / `tonemap` / `refuse`）；`compiled` / `status` / `reason` / `verifiedOnThisMachine`。`status` 说的是这个构建带不带得回广色域帧 + tone mapping 怎么做，**不**去问那块屏此刻是不是 HDR 模式（`reason` = `hdr_display_mode_not_probed`，无可用显示拓扑时才是 `unavailable`）；`verifiedOnThisMachine` 恒 `no`（本项目没有能开 HDR 的显示器，不宣称色彩验收通过）。`paths[]` 每条已登记内部路径一行（`capability` 是 `wide_gamut_capable` / `sdr_source_only` / `unregistered`，`reason` 是那条路径的根据）。再加 `toneMapping`（那条固定曲线的名字）/ `floatIntermediateFrame: "per_pixel_registers"`（不分配整幅浮点帧）/ `encoderOutput: "sdr_bgra8"`（HDR 一律映射成 8 位 SDR 交付，不出 HDR 原生图）。caveats 恒含 `hdr_tone_mapping_not_verified_on_hdr_display` 与 `hdr_output_is_tone_mapped_to_sdr_bgra8` |
| `autoChainWindow` / `autoChainScreen` | 本机现在能试的 `auto` 链。与真实截图那次 `-v` 回显的 `input.captureChain` 由**同一个** `GateChannels` 算出，`tests\capabilities.ps1` 判两处一致 |
| `limits` | `maxFrameSide` 16384、`maxFrameBytes` 1 GiB、`maxTimeoutMs` 86400000、`isolatedCallMs` 5000、`maxWgcRecreates` 4、`maxOrdinal` 65535、`maxPid` 4294967295、`stdoutTargetsMax` 1、`jpegQualityMin`/`Max` 1/100 |
| `privacy` | 自述：`capturesScreen` / `showsDialog` / `uploads` / `enumeratesUserFiles` / `readsEnvironmentVariables` / `includesUsernames` / `includesPaths` 全为 `false` |
| `caveats` | 稳定 ASCII token，列"这份报告没断言什么"：`no_capture_performed`、`no_consent_dialog_shown`、`available_is_not_a_guarantee`、`device_capability_not_predicted`、`encoder_state_not_probed`、`consent_dialog_state_inferred_not_probed`、`subsystem_version_is_linker_default`，按本机情况追加 `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown` |

`reason` 的取值同样稳定：`none`、`not_compiled`、`os_below_min_build`、`os_version_unavailable`、
`no_display_topology`、`display_topology_unavailable`、`encoder_not_registered`。`cursor` 那一段的 `include` / `exclude` 同样三值；未登记的路径读 `unknown` 而 `reason` 写 `not_registered`，不猜一个答案。

**两份文档不是两套信息**：都由 `src/EnvReport.cpp` 的 `BuildEnvReport` 算出，只差段落取舍（`--diagnostics`
固定带 `build` 段，`--capabilities` 只在 `-v` 时展开）。版本、`status`、后端清单、`limits` 都是同一份。

**构建标识可核对**：`buildId` = `版数-架构-十六进制链接时间戳`（例：`0.4.0-x64-6ABC6DF2`），那个时间戳与
`dumpbin /headers` 读发布产物读到的是同一个字段。读的是本进程已映射进内存的 PE 头，不开文件也不枚举目录，
所以安装路径里的用户名不会跟着漏出来。`build.subsystemVersion` 只是事实，配 `subsystem_version_is_linker_default`
这一条 caveat：那是 MSVC 链接器默认值，不是支持声明。

**互斥与流**：这两条只接受 `--lang` / `-v` / `-q`。与窗口条件、`--monitor`、`--capture`、`--out` 或位置参数、
`--yes`、`--dry-run`、`--timeout-ms` / `--consent-timeout-ms` 中任何一条同时给出，或两条查询同时给出，
都是 `cli.query_conflict` + 退出码 1（`value` 一次列全所有冲突项；位置参数报成 `--out`，不回显那条路径本身），
一张都不截、一个文件都不写。它们也不参加"没给条件就出帮助"那一条。查询没有图片要交付，所以结果恒在 **stdout**，
stderr 为空；`-v` 加 `probes`，`-q` 只去掉 `caveats`。

**退出码只有两个**：`0` = 文档出完了（哪怕里面写着这台机器哪条都不行——查询成功与截图能成是两件事，
按 `status` 分支而不是按退出码猜环境）；`1` = 用法不合契约。不会出现 `4`/`5`/`6`/`7`/`8`。

整份文档是 ASCII（机器读的取值不翻译），所以同一台机器上换 `--lang` 输出逐字节相同 —— 可以放心做前后两次比对。

## 只读的窗口查询（`--list` / `--inspect`）

这两条命令回答的是「哪一扇窗口命中这批条件」，把窗口条件求值的结果**作为数据**交回，而不是拼成一句要再解析的话。
一个像素都不取、不调用任何截图后端、不弹确认框、不写文件、不激活也不恢复任何窗口，也**不需要输出路径**。
原来的 `--dry-run` 入口照旧可用（它挑中目标后就返回），这一节是加在它旁边的结构化出口，没有替换它。

```powershell
ECAPTURE.EXE --list --title-contains 记事本          # 列表（JSON，contract=windowquery）
ECAPTURE.EXE --list=all --class Notepad              # 把最小化窗口也并进同一根 Z 序轴
ECAPTURE.EXE --list --offset 50 --limit 50            # 翻页：总数看 pagination.matched
ECAPTURE.EXE --inspect --hwnd 0x001A0B4C             # 单扇窗口的身份快照（contract=windowinspect）
ECAPTURE.EXE --inspect=path --title 订单              # 同上，并写出归属映像完整路径（默认只写文件名）
```

条件语义与截图**同一套**：同一个选项写多次 = OR，不同选项 = AND，`--monitor` 按屏过滤，
`--title-regex` 与设了 `--timeout-ms` 时同样走辅助进程。所以一次查询里 `--list` 与 `--inspect` 只能选一个入口。

| 项 | `--list` | `--inspect` |
| --- | --- | --- |
| 结果字段 | `windows[]`（0 条就是空数组） | `window`（恰好一条） |
| 多匹配 | **不算歧义**，列全并分页 | 按截图那套选择策略定不出唯一一条 → `match.ambiguous_window` + 退出码 5，**不会替你挑一个** |
| 零条件 | 允许（列全部顶层可见窗口），不走「没给条件就出帮助 + 退出码 2」 | 同上，但零条件必然多匹配 → 5 |
| 无匹配 | `windows: []`，退出码 0 | `match.no_window` + 退出码 4 |

`--inspect` 走的是 `SelectFromHits`（与真去截图那次同一个函数），所以 `--index` / `--topmost-match` /
`--bottommost-match` / `--all` 在这里的含义与截图一致；`--all` 与 `--inspect` 同时给出算用法冲突。

### 每一问的读数：读不到 ≠ 空值

跨进程问的每一件事都带一个三态答案，问不出来绝不写成空串、0 或 false：

```json
"readability": {
  "process":      { "state": "denied", "win32": 5 },
  "imagePath":    { "state": "readable" },
  "processStart": { "state": "failed", "win32": 87 },
  "rect":         { "state": "readable" }
}
```

`state` 只有 `readable` / `denied` / `failed` 三种，`win32` 是失败点当场取走的 `GetLastError` 原值。
`denied` 就是字面意思——权限不够，**不因此要求你以管理员运行**，也不预测截图会不会成功
（`unreadable_fields_are_not_a_prediction` 那条 caveat 钉的就是这点）。
`exePath` 这个键在默认输出里**根本不出现**（完整路径常含用户名），只有 `--inspect=path` 才逐条写出，
并且同时写 `exePathRequested: true` 与 `exePathReadable: true/false`，让「没写」与「写了但问不到」分得开。

### 这份快照会过期，身份字段不是凭证

```json
"identity": {
  "hwnd": "0x001A0B4C", "pid": 27256, "class": "Notepad",
  "processStartTicks": 134351164333279485,
  "selectionNeedsRecheck": true,
  "verificationRequired": true,
  "isAuthorizationToken": false,
  "raceWindowReducedNotEliminated": true
}
```

- `processStartTicks` 把「PID 被系统复用」与「还是那个进程」分开；问不到时写字符串 `"unknown"`，不写 0。
- `selectionNeedsRecheck: true` = 当初的条件含易变项（标题、按屏过滤），截图前要拿条件重跑一次；`false` = 只按类名/句柄选中，比类名就够。
- 这三件是**给下一次截图带回去的约束**，不是许可凭证：真去截图时本工具仍会在取帧之前复核目标身份
  （`capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`），确认框也照旧按像素来源判。
- 每次窗口查询都固定带一条 `note.window_query_stale` 说这件事，`-v` 的 `policy` 与 `caveats` 里也各列一份。

默认策略是显式写出来的，不是隐含的：`policy.invisibleExcluded` / `zeroSizedExcluded` 恒为 `true`，
`minimizedIncluded` 默认 `false`（`--list=all` 改成 `true`），`systemWindowAssertion: false` 表示
这份列表**不**声称某条不是系统窗口——它只报告问到的事实，不替你分类。`order: "zOrder"` 说明列表按当下的叠放次序，
每条带 `zOrder` 下标。`--inspect=path` 之外没有第二条路线能拿到完整路径。

### `--yes` 对查询没有任何作用

`authorization.yesAffectsResult: false` 是当场判出来的事实：带不带 `--yes`，这一份列表的字段与条数完全相同。
`--yes` 在窗口查询里被接受（写了不算用法错），因为它是截图授权那一级的开关，一次不出图的查询无从受它影响。
查询**不**产生任何桌面像素，也就不产生任何一级确认框。

### 流、分页与退出码

结果恒在 **stdout**（查询没有图片要交付，所以不触发「图片占用 stdout 时 JSON 改走 stderr」那条规则），stderr 为空。
`-q` 只去掉 `notes`，`policy` / `authorization` / `caveats` / `readability` 这些隐私与自述判据不许被抑制；
`-v` 追加 `input` 段（规范化后的条件、`offset` / `limit` / `limitGiven` / `includeIconic` / `exePath` / `policy`）。

`pagination` 一次说清翻到哪了：`offset`、`limit`（本批实际用的上限，没给 `--limit` 时是 `defaultLimit` 50 并配
`limitDefaulted: true`）、`maxLimit` 8192、`matched`（命中总数）、`returned`（本批条数）、`truncated`
（`offset` 之后确实还有剩下的）与 `nextOffset`（下一次翻页该写的偏移；没有剩下的东西时**整个键不出现**，
写 0 会被读成「从头再来」）。**翻页靠 `matched` 与 `nextOffset`，别按本批条数猜还有多少。**
`policy.minimizedExcluded` 另写本次被默认策略挡掉了几条最小化窗口，与 `truncated` 一起决定
`caveats` 里的 `list_may_be_partial`——只数 `windows[]` 会以为整机就这几个窗口。

`cli.window_query_conflict` + 退出码 1：窗口查询与截图那一级的选项（`--out` / 位置参数 / `--format` /
`--quality` / `--capture` / `--dry-run` / `--consent-timeout-ms` / 选择策略与 `--all` 在 `--inspect` 那条入口下）
或环境查询（`--capabilities` / `--diagnostics`）同时给出。`value` 一次列全所有冲突项，位置参数报成 `--out`
而不回显那条路径本身。允许清单：窗口条件、`--monitor`、`--timeout-ms`、`--yes`、`--offset` / `--limit`、
`--list` / `--inspect` 自己的取值、`--lang` / `-v` / `-q`。

退出码用这六个：`0` 查询成功（包括命中 0 条）/ `1` 用法不合契约 / `4` `--inspect` 无匹配 /
`5` `--inspect` 多匹配歧义 / `7` **只有一条来路** —— 这一次的条件求值自己没跑完（`match.timeout`，
或那一步的辅助进程故障），说的是「这一次问答没能问完」，与取帧无关；`9` 内部异常。
**`6` 与 `8` 不可能出现** —— 那两条说的是「没人批准」与「写文件失败」，而一条不弹框、不落地的
命令没有资格报它们。窗口查询那一条 `match.timeout` 的 `hint` 也是查询自己的说法：它明说
「换 `--capture` 没有用」，因为这一路根本没有通道可换。

参数级失败（与截图选项冲突、条件写坏）交回的是与**截图结果同形**的失败文档：`captured: 0`、`images: []`、
`errors: [...]`，调用方按 `errors[].code` 分支的那段代码不必为窗口查询再写一份。

## 只读的屏幕枚举（`--screens`）

「要那一块屏」此前只有一种写法：`--monitor <n>`，而那个 n 是本次枚举顺序里的位置。猜错编号不会响亮地失败——
它会把另一块屏拍到磁盘上，而那块画面没人批准过。这条命令把屏幕身份当数据交回来，其中两种可以直接写回 `--monitor`。

```powershell
ECAPTURE.EXE --screens                                   # 契约名 screens：每块屏连同它的几种身份
ECAPTURE.EXE --monitor device:DISPLAY1 --out shot.png    # 按本次桌面连接的设备名点名
ECAPTURE.EXE --monitor "id:\?\DISPLAY#GSM41A2#5&…#{…}" --out shot.png   # 按跨会话的监视器设备路径点名
```

四条规矩：

1. **只读**：一个像素都不取、不调任何取图通道、不弹确认框、不写文件、不联网，也**不改任何显示设置**
   （绝不为了"看清这块屏转了多少度"去调 `SetDisplayConfig`）。不需要窗口条件，也不需要输出路径，
   更不参加「零条件就出帮助」那一条。
2. **这份列表是快照，不是凭证**：每次成功都带 `note.screen_query_stale`，`caveats` 里有
   `device_names_are_not_persistent` / `cross_session_stability_not_tested` / `screen_capture_always_asks`。
   点名一块屏不替代取帧之前的身份复核，也不替代授权：整屏是桌面像素，**一定要人点头，`--yes` 跳不过**。
3. **四种身份各说各的稳定范围**（写在 `identity.*` 与每条屏的字段里，不靠调用方读源码）：
   `ordinal` = `this_invocation`（只有本次枚举有意义）、`deviceName` = `this_desktop_attach`（本次桌面连接里发的
   名字，拔掉重插之后可能发给另一块面板）、`monitorDevicePath` = `cross_session_expected`（设备节点决定的那一条，
   跨会话与跨重启——本项目只在同一会话里观察过，所以写的是 expected 而不是保证）、
   `adapterLuid` = `this_session`（本次会话内唯一，**没有**选择器写法，只作关联信息）。
   `screens[].selectors` 里给的就是能原样抄回 `--monitor` 的那两条字符串。
4. **问不出来 ≠ 空值**：`dpi`（有效值与原始值，走 `shcore!GetDpiForMonitor`，Win8.1 起）、`rotation.degrees`
   （人看到的朝向，取当前 `DEVMODE`）与 `rotation.panel`（相对面板原生朝向，取显示配置）是各独立的一问，
   各有自己的 `readability`（`readable` / `denied` / `failed`）与那一条 API 的错误码。读不到的那一项整个键不出现，
   也不建议以管理员运行。适配器那一侧同时交回 `adapter.devicePath`（跨会话的那一条）、`outputTechnology`、
   `targetId`、`targetAvailable`。

按标识点名在一切"要选一块屏"的地方都成立：配窗口条件就是按那块屏过滤窗口，`--list` / `--inspect` 走的是同一个
选择函数，取帧之前的复核也按**选定当时问得到的那条身份**核对——设备名若已属于另一块面板，报
`capture.monitor_changed` 而不是照名字截下去；复核本身问不出答案时报 `capture.monitor_unverifiable`，
同样不退回去按名字截。

允许清单与 `--capabilities` 同一家族：只接受 `--lang` / `-v` / `-q`，其余（含 `--yes`、`--monitor`、输出路径、
另一条查询）都是 `cli.query_conflict`+1，`value` 一次列全冲突项。`-q` 只去掉 `notes`：`identity` /
`readability` / `authorization` / `privacy` / `caveats` 是判据，不许抑制。退出码只有 `0`（这份文档出完了，
哪怕里面写着本机问不出来）与 `1`（用法不合契约）；`4`/`5`/`6`/`7`/`8` 都不可能出现——它不选目标、不弹框、不落地。

隐私：交回的是**设备路径**（硬件身份），不含文件系统路径与用户名
（`privacy.includesDevicePaths: true`、`includesFileSystemPaths: false`、`includesUsernames: false`）。
这与 `--capabilities` 那份"不含任何路径"的自述是两份不同的取舍，各写各的，不互相覆盖。

## 窗口内部裁剪（`--roi` / `--client-area`）

这两条回答的是同一件事：**这一次交付的窗口图里要留哪一块**。判据本体是纯算术（`src/CropGeometry.h/.cpp`），
测量与落地在 `src/Capture.cpp`，离线判据在 `tests\crop_state.cpp` → `build\ecapture-crop-tests.exe`。

- **坐标系只有一种**：`cropRect` 说的是交付图像自己的像素坐标，左上角 = `(0,0)`，右下边不含。那张图像是用户看到的
  可见边框之内（`DWMWA_EXTENDED_FRAME_BOUNDS`）。**它永远不会被当成桌面绝对坐标** —— 那等于允许调用方用一个窗口之外的
  位置去要一块谁都没批准过的画面。要截屏幕上某一块位置，用 `--monitor`（整屏）而不是 `--roi`。
- **单位是物理像素，没有 DPI 换算**：进程声明 per-monitor DPI v2，窗口矩形与帧尺寸本来就都在物理像素那一套系里。
  同一条 `--roi 0,0,200,120` 在缩放一倍与两倍的屏上取的都是 200×120 个像素。按逻辑像素（DIP）思考的调用方自己乘缩放；
  本工具不猜窗口在哪块屏上，也不猜该用哪块的 DPI。
- **`--client-area` 是"再往里一圈"**：在交付图像内去掉标题栏与三边边框；那块矩形由目标此刻的几何量出来，
  与 `--roi` 共用同一套坐标与同一道越界判据。
- **放不下就是拒绝，四种码各归一种下一步**：写法不合 `cli.invalid_value`(1) / 取帧之前就看得出放不下
  `match.roi_out_of_range`(1，排在确认框与输出名规划之前) / 取到帧才发现放不下 `capture.roi_invalid`(7) /
  定位所需的那一问没答案 `capture.roi_unmeasurable`(7)。四种都不落地，都不"往里挪一挪""裁到边上为止""那就整窗交出"。
  一批里有一扇放不下就整批一张都不截（与 `match.index_out_of_range` 同一条规矩）。
- **裁剪不改变授权**：它排在取帧之后，所以"这条路径的像素从哪来"这件事一点没变 —— 会从屏幕上取样的那几条
  （`bitblt` / `duplication` / 任何整屏 / `dwm.screen`）即使 `--roi` 只要 8×8 也照样一定弹框，`--yes` 在这里不起作用。
  只有窗口内容那三条（`wgc` / `printwindow` / `dwm.thumbnail`）是 `--yes` 管得着的。
- **结果里的四个新字段**（都是定位判据，`--quiet` 不许藏）：`cropMode`（`roi` / `client-area`）、
  `cropRect`（图像坐标）、`fullWidth` / `fullHeight`（裁之前的整窗图像尺寸；`width` / `height` 是裁之后的最终尺寸）、
  `cropScreenRect`（同一块矩形的虚拟屏幕坐标，与 `rect` / `requestedRect` 同一套系）。映射是闭合的：
  `cropScreenRect − cropRect` = 这块图像自己的屏幕原点，调用方可以拿 `rect` 核对它。`cropScreenRect` **只在图像原点
  核实得出来时才写**（那条通道自己报了实际截到的那一块 = `capturedRect`；或者此刻量到的可见矩形尺寸与交付尺寸完全相同），
  核实不出来就整个键不出现并留 `note.crop_mapping_unavailable` —— 问不出来不会被折成一个看起来合理的数。
  `--client-area` 本来就要靠这条映射，所以映射问不出来时它直接失败（`capture.roi_unmeasurable`）。
- **与 `requestedRect` / `capturedRect` / `clipped` / `rotation` 并存不重复**：前一组说整扇窗口在桌面上有没有被完整
  截到，后一组说截回来的那张图里交出哪一块。

```powershell
# 只要标题栏以下、左边起 20 像素那块 120×80
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --roi 20,40,120,80 --out D:\shots\part.png
# 只要客户区（去掉标题栏与边框）
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --client-area --out D:\shots\client.png
# 先看这次的裁剪请求被理解成什么，不截图也不弹框
ECAPTURE.EXE --hwnd 0x001A0B4C --dry-run --roi 20,40,120,80 --verbose --out D:\shots\plan.png
```

## 等比缩小（`--scale`）

这一条回答的是：**交付的这张图要不要再取小一点**。判据本体是纯算术（`src/ImageOps.h/.cpp` 的 `ResolveScale` /
`ScaleFrame`），落地在 `src/Capture.cpp`，离线判据在 `tests\image_state.cpp` → `build\ecapture-image-tests.exe`。

- **取值是 `key=N` 的清单**：`max-width` / `max-height` / `max-pixels` 三个键（大小写不敏感），N 只认十进制，
  边长 1..16384（与 `--roi` 同一条线）、像素数 1..268435456。可以只给一条、也可一条里用逗号串几条；这一条写多次时
  每条天花板各记各的，重复给同一条时最后一个生效。一条都不给 = `cli.invalid_value`(1)，**整条不生效**，
  不留下认得的那半；`1e3` / `0x400` / `+40` / `1_000` 一样不收。
- **一个比例，取最紧的那一条**：三条天花板各提出一个比例（宽、高，像素预算则开方），取最小的那一个；宽高各自
  **向下取整**，且各至少留 1 像素。**默认不放大**：本来就在天花板之内的图原样交付（`scaleApplied: false`）。
- **插值策略只有一种，而且可预测**：最近邻（`scaleMethod` 恒 `nearest`）。交付像素 `(x,y)` 取自缩之前那张图的
  `(floor(x*scaleFromWidth/width), floor(y*scaleFromHeight/height))`，是调用方能自己复算的整数映射，
  没有浮点采样、也不按通道挑算法。
- **顺序：先裁（`--roi` / `--client-area`）后缩、再编码。** 所以 `scaleFromWidth` / `scaleFromHeight` 是**裁之后**
  那张图的尺寸（不是整窗图），`cropRect` / `cropScreenRect` 一字不改，`width` / `height` 是最终尺寸；
  单色质量提示判的是交付出去那张（缩过的）。
- **不改变授权，也不是绕开上限的路径**：缩放排在取帧与授权之后，所以会从屏幕上取样的那几条即使
  `--scale max-width=8` 也照样一定弹框、`--yes` 不因此生效；一帧大到过不了形状检查的根本到不了这一步，
  `--roi` 的越界判据也仍按未缩的那张图判。
- **结果里的四个键（写过 `--scale` 才出现，`--quiet` 不许藏）**：`scaleMethod` / `scaleApplied` /
  `scaleFromWidth` / `scaleFromHeight`。没写 `--scale` 时一个都不出现（不是 `null` / `0` / `false`）。
- **失败只有一种码**：`cap.scale_apply_failed`(7) —— 形状判据放得下却缩不下来（帧自相矛盾），这一张不落地，
  与 `cap.crop_apply_failed` 同一类。

```powershell
# 一扇 1920 宽的窗口，最多交 1280 宽（高按同一比例）
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --scale max-width=1280 --out D:\shots\big.png
# 先裁出 800×600，再缩进 400000 像素的预算里
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --roi 0,0,800,600 --scale max-pixels=400000 --out D:\shots\part.png
```


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
- **省略 `--out` 与显式 `--out -` 是同一条路**：同样的 code、同样的退出码、同样的部分成功结果。
  所以"是不是被人拒了"直接看 `errors[].code` 就知道，不必为了看清而先补一个 `--out`；
  旧实现那种"没给输出路径就把一切失败换成 `cli.missing_output` + 1"的行为已经删除（见《行为变更》一节）。
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
- 只有只读查询那三份例外，它们各自带 `contract` / `contractVersion`：`windowquery` / `windowinspect`
  与 `capabilities` / `diagnostics`（见上面两节）。这两份**不**反推截图那份去加顶层元信息，
  截图结果的 `captured` / `images` / `errors` / `notes` / `input` 规则也与它们无关。
- 通道分配：默认全部走 stdout、stderr 为空；一旦图片占用 stdout（`--out -` 或没给输出路径），
  **整份 JSON 改走 stderr**，两个通道永不混流。这条判断在任何图片写出之前就定下，连"渲染结果本身抛异常"
  的兜底诊断也跟着它（一律 stderr，工具不为此再解析一遍命令行）。**约定那条流写不出去就是失败**：
  退出码 8，即使另一条流补发成功也不留成原来的值。
- 文本输出只有三种情况：`--help`、`--version`、没给任何条件。
- `--verbose` 的 `input` 里另有两项环境判据：`osBuild`（本机 Windows 内部版本；整个键不出现 = 这一问没成功）与
  `captureChain`（本次这类目标实际可用的通道链，按尝试顺序；显式指定的那条被挡下时是空数组）。用它们可以在
  不截图、不打扰人的情况下问出「这台机器给得出哪几条通道」，见上面「运行环境与能力检查」一节。

### 窗口图（`images[]` 每一项）

`file` `bytes` `width` `height` `format` `source`（真正出图的那条通道）`path`（实际走的那条内部路径名）`scope`（`window` / `desktop`，由 `path` 算出）`rect`（`{"x","y","width","height"}`，那次授权允许采样的屏幕区域）`requestedRect` / `capturedRect` / `clipped` / `rotation`（只有从整幅桌面帧裁目标的通道会写，见下面那段）`hwnd`（`0x…` 字符串）`pid` `title` `class` `image`（映像文件名）`elapsedMs`；给了 `--roi` / `--client-area` 时再多 `cropMode` `cropRect` `fullWidth` `fullHeight`（以及图像原点核实得出来时的 `cropScreenRect`），见「窗口内部裁剪」一节；写过 `--scale` 时再多 `scaleMethod` `scaleApplied` `scaleFromWidth` `scaleFromHeight`（见「等比缩小」一节）；写过 `--cursor` 时再多 `cursorRequested` `cursorEffective` `cursorBasis`（见下面「光标那三个键」那段）；写过 `--hdr` 时再多 `hdrRequested` `hdrEffective` `hdrBasis` `sourceColorSpace` `sourceBitDepth`（见下面「HDR 那一组键」那段）

**光标那三个键（写过 `--cursor` 才出现，`--quiet` 也不许藏）**：`cursorRequested` 是要求的那一种（`default` / `include` / `exclude`）；`cursorEffective` 是**这条路径实际**交回的那一种（`include` / `exclude` / `unverified`）；`cursorBasis` 说这个结论凭什么 —— `wgc_session_property_set`（按这次要求设过、再把读回来的值核对过）、`wgc_session_property_read`（没设过，只读当前值，即 `--cursor default`）、`path_excludes_cursor`（这条路径的来源像素里没有光标）、`wgc_cursor_property_unavailable`（那一问没答案，此时 `cursorEffective` 就是 `unverified`）。三个键各说一件事，谁也不冒充谁：`effective` 说不到"这一张图里看得见或看不见指针"那一层（本 SDK 的会话接口没有 `IsCursorVisible` 那个只读属性，像素级的事本工具一条都不声称，而 `--capabilities` 把这条边界写成 `cursor_effective_is_a_setting_not_a_pixel_check`）。没写 `--cursor` 时三个键一个都不出现（那才是"默认不要求"与从前逐字节相同的保证）。

**HDR 那一组键（写过 `--hdr` 才出现，`--quiet` 也不许藏）**：`hdrRequested` 是要求的策略（`auto` / `tonemap` / `refuse`）；`hdrEffective` 是**这一帧实际**经历的处理（`sdr_passthrough` = 来源核实是 8 位 SDR、没做也不需要映射；`tone_mapped` = 来源是 HDR、已按固定的浮点曲线映射成 8 位 sRGB；`unverified` = 带回一个认不出的广色域格式，既不敢说映射对也不敢说就是 SDR）；`hdrBasis` 说这个结论凭什么（`delivered_bgra8_sdr` / `scrgb_float_tone_mapped` / `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `path_sdr_source` / `format_unrecognized`）；`sourceColorSpace` 是编码之前那份来源（`srgb_bgra8` / `scrgb_float` / `pq_bt2020` / `hlg_bt2020` / `unknown`）；`sourceBitDepth` 是来源每通道位数（`8` / `10` / `16`，来源认不出时整个键不出现，绝不写 0）。明确要过处理（`tonemap` / `refuse`）而来源其实是 8 位 SDR 时图照常交付（映射对 SDR 恒等）并留一条 `note.hdr_source_sdr`；`--hdr auto` 不发这条（它只被动上报）。`hdrEffective: "tone_mapped"` 只说这台机器过了那条映射链路，**不**说色彩正确性被验过（本项目无 HDR 屏，`--capabilities` 的 `color.verifiedOnThisMachine` 恒 `no`）。没写 `--hdr` 时这一组键一个都不出现（与这条选项存在之前逐字节相同）。

**等比缩小那一组键（写过 `--scale` 才出现，`--quiet` 也不许藏）**：`scaleMethod` 是插值策略（恒为 `nearest`，本工具只有这一种，而且映射是能自己复算的整数式）；`scaleApplied` 说这一次真的缩小了没有（本来就在天花板之内就是 `false`，此时图一个像素都没动）；`scaleFromWidth` / `scaleFromHeight` 是**缩之前**那张图、也就是**裁之后**那张图的尺寸（不是整窗图），所以映射按「有效帧 → 裁剪 → 缩放 → 编码」这个顺序闭合：交付像素在 `cropRect` 里再按 `scaleFromWidth/width` 与 `scaleFromHeight/height` 反查。这四个键与 `cropRect` / `cropScreenRect` 并存而不重复：后一组说缩之前交出哪一块，前一组说那一块最后被取成了多大。没写 `--scale` 时四个键一个都不出现（那才是"默认不缩放"与从前逐字节相同的保证）。

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

`file` `bytes` `width` `height` `format` `source`（同上）`path`（`screen.wgc` / `screen.bitblt` / `screen.duplication`，三条都是桌面）`scope`（`desktop`）`rect`（那次授权允许采样的屏幕区域，= 那块屏的矩形）`requestedRect` / `capturedRect` / `rotation`（`duplication` 这条会写，见上面那段；整屏本该 `capturedRect` 等于 `rect`，裁不全就直接报 `capture.monitor_changed` 而不是交一张偏小的图）`monitor`（编号）`device`（`\DISPLAY1` 之类）`primary`（布尔）`elapsedMs`；写过 `--cursor` 时同样多 `cursorRequested` / `cursorEffective` / `cursorBasis`；写过 `--hdr` 时同样多 `hdrRequested` / `hdrEffective` / `hdrBasis` / `sourceColorSpace` / `sourceBitDepth`（屏幕目标走 `screen.wgc` / `screen.duplication` 时也可能带回广色域帧，同窗口目标一套键）

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
| 1 | 参数错（也含 `match.index_out_of_range` / `match.monitor_out_of_range` / `match.roi_out_of_range` 这些「编号或裁剪矩形对不上实际命中的目标」的越界用法） |，也含 `--cursor include` 配那条结构上做不到的通道 `capture.cursor_unsupported`，也含 `--hdr tonemap`/`refuse` 配那条结构上带不回广色域帧的通道 `capture.hdr_unsupported`
| 2 | 未给条件（输出文本帮助） |
| 3 | `--help` |
| 4 | 无匹配窗口 |
| 5 | 匹配多个窗口 |
| 6 | 这次截图没拿到人的同意：人在确认框上答"否"或把框关掉（`capture.access_denied`），那个会话根本没有可交互的桌面、框弹不出来（`capture.consent_unavailable`），或在 `--consent-timeout-ms` 之内没有人回答（`capture.consent_timeout`）；也包括目标受保护 |
| 7 | 截图失败（含 `--timeout-ms` 预算耗尽的 `match.timeout` / `capture.timeout`，含身份复核没过的 `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`，也含交付图像放不下请求的裁剪 `capture.roi_invalid` / 定位裁剪所需那一问答不出 `capture.roi_unmeasurable`） |，也含要求的光标状态核实不了 `capture.cursor_unverifiable` 与本机给不了那个开关 `env.cursor_unsupported`，也含 `--hdr refuse` 核实来源是 HDR `capture.hdr_refused` 与带回认不出的广色域格式 `capture.hdr_unverifiable`（都不落地）
| 8 | 写文件失败（也含结果 JSON 没送到约定那条流，以及预算耗尽落在写/stdout 阶段的 `io.timeout`） |
| 9 | 内部异常 |

退出码与 body 是两套独立信号：先看 `errors`，再看 `captured`，最后才用退出码做粗分支。
只读查询那两条（`--capabilities` / `--diagnostics`）只用 `0` 与 `1`：`0` = 文档出完了（里面写"这台机器哪条都不行"也算成功，环境要看 `status` 而不是
退出码），`1` = `cli.query_conflict`。它们不产生 `4`/`5`/`6`/`7`/`8`，因为一次窗口都没枚举、一个框都没弹、
一个文件都没写。
窗口查询那两条（`--list` / `--inspect`）用 `0`/`1`/`4`/`5`/`9`：`1` = `cli.window_query_conflict` 或条件本身写坏，
`4` 与 `5` 只有 `--inspect` 会出（一次定不出唯一目标 —— 没命中 / 命中好几种写法都定不到），`--list` 命中 0 条是
正常答复给 `0`。同样**不会出现 `6` 与 `8`**：不弹框、不落地，那两条说的就是那两段的事；
`7` 只在条件求值自己没跑完时出现（见上面的退出码一节）。

- `io.write_failed`、`io.file_exists`、`io.output_collision`、`io.timeout` 与"结果送不到约定流"给出 8；截图/编码阶段的其它失败（含
  `capture.failed`、`capture.timeout`、`capture.encoder_unavailable`、`capture.consent_stale`、`capture.worker_failed`，
  以及环境类的 `env.os_too_old` / `env.channel_unsupported`）都给 7；`capture.access_denied`、
  `capture.consent_unavailable` 与 `capture.consent_timeout` 给 6（都算"这一张没人批准"，但下一步动作不同：
  有人答了否 / 那里没有桌面可弹 / 人在 `--consent-timeout-ms` 之内没答）。
- 某个后端抛异常（而不是返回失败）只作废它所在的那一个目标：前面成功的图留着，剩下的目标照旧继续；
  内存耗尽与显卡设备被移除这类"换后端也不会有区别"的错误会明确终止整批。
- **部分成功**：`--all` / `--monitor all` 里某些目标失败时，已写出的图照样在 `images` 里
  （`captured` 可以大于 0），但退出码仍是 7。所以"退出码非 0"不等于"什么都没拿到"。

## 行为变更：省略 `--out` 不再折叠错误

省略 `--out` 与显式 `--out -` 从此是**同一个请求**：图片按 png 走 stdout、JSON 整份走 stderr，
每条诊断都是那一步真实产生的那一条。

| | 原行为 | 新行为 |
| --- | --- | --- |
| 没给输出路径而这次又失败 | 整段换成一条 `cli.missing_output` + 退出码 1，`images` 与 `notes` 清空，真实原因不外泄（例外只有 `cli.stdout_multiple_targets` 与 `stage=consent` 的拒绝 / 弹不出） | 原样的 code 与原样的退出码：`match.no_window`（4）、`match.ambiguous_window`（5）、`capture.access_denied`（6）、`capture.failed`（7）、`io.write_failed`（8）、`cli.invalid_number`（1）…… |
| 没给输出路径时的部分成功 | 看不见（`images` 被整段丢掉） | 已送到 stdout 的图仍在 `images` 里，`captured` 照实计数 |
| `cli.missing_output` | 解析期 / 输出期的兜底码，退出码 1 | **不再发出**。这条 code 保留在编号空间里，为的是它不被挪作别的含义；读到旧日志里有这一条，说明那次运行的版本早于本次变更，真实原因当时没交出来 |
| "这次没给输出路径"这句话 | 一个 code | 只在补上文件名确实绕得开这次故障的那一条上作为 `hint` 出现：隐式 stdout 且 `io.write_failed` + `stage=stdout`。写 `--out -` 的人本来就选定了这条管道，那句 hint 不出现 |

调用方怎么分支：

1. 先看 `errors[].code`，再用它的 `stage` / `target` / `backend` / `hresult` / `win32` 定位；
   `message` / `hint` 随 `--lang` 变，只能给人看。
2. 退出码与 body 是两套独立信号：`0` 也可能带着 notes，`7` 也可能已经有图（部分成功）。
3. **不要根据"有没有给 `--out`"推断原因**，也不要因为看见 `cli.missing_output` 就补一个 `--out` 再截一次——
   旧实现正是这么把它说成缺少输出路径的，而那种失败补 `--out` 一个都治不了。
4. code 只追加、不改名、不改含义，所以照旧行为写的调用方仍然能跑：它只是再也读不到一条假原因。

## 诊断码全表

只按 `code` 分支。码值只增不改名。

**`cli.*`（解析期，全部退出码 1）**
`cli.unknown_option` `cli.missing_value` `cli.switch_takes_no_value` `cli.invalid_number`
`cli.invalid_regex` `cli.invalid_value` `cli.invalid_format` `cli.unrecognized_extension`
`cli.unexpected_positional` `cli.duplicate_output` `cli.conflicting_options`
`cli.unknown_capture_method` `cli.unknown_language` `cli.monitor_conflict` `cli.internal_error`
`cli.query_conflict`（环境查询 `--capabilities` / `--diagnostics` / `--screens` 与截图选项或输出路径同时给出；`value` 一次列全冲突项，一张都不截也没一个文件被写）
`cli.monitor_selector_empty` / `cli.monitor_selector_kind`（`--monitor` 的标识写法：冒号后面是空的，或前缀不是 `device` / `id` 那两个词。都不退化成"用主屏"，也不去猜那个前缀想写什么）
`cli.window_query_conflict`（窗口查询 `--list` / `--inspect` 与截图那一级的选项、输出路径或环境查询同时给出；允许清单见
「只读的窗口查询」一节。两条查询各自的冲突各收一份，扫完之后按「这一次到底是哪一类查询」取对应那一条码报，
`value` 同样一次列全。位置参数报成 `--out` 而不回显用户那条路径本身）
`cli.stdout_multiple_targets`（stdout 一次只交付一张图，实际目标多于一个；整批没截也没写，也不弹框）
`cli.no_condition`（→ 文本帮助 + 2）
`cli.invalid_regex` 还有匹配期这一处：模式撞上正则引擎的回溯复杂度上限（`stage=match` + 1，消息说的就是回溯复杂度）——
**加大 `--timeout-ms` 没有用**，改写模式或换 `--title-contains`

**`match.*`**
`match.no_window`（4）`match.ambiguous_window`（5）`match.index_out_of_range`（1）`match.monitor_out_of_range`（1）`match.roi_out_of_range`（1，`--roi` 的矩形放不进选定那一刻那块窗口矩形；排在确认框与输出名规划之前，不弹框、不写文件，也不会被往里挪或裁到边上为止。一批里有一扇放不下就整批这张码，见「窗口内部裁剪」一节）
`match.monitor_unknown_id`（4，`--monitor` 的标识此刻不在桌面上：拔掉了、禁用了，或者是上一次 `--screens` 的旧值。
下一步是重新列一次，**不是**换编号碰碰运气，也不会被换成主屏）
`match.monitor_ambiguous_id`（5，同一标识命中多块屏；候选全列在 `hint` 里，本工具不替你挑一块）
`match.monitor_id_unverifiable`（7，屏幕身份这一问整条没答案，所以无法按标识点名；换 `--capture` 没有用——
这一路根本没选过通道）
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
`capture.monitor_unverifiable`（7，取帧之前的身份复核没能给出答案，而当初选定那块屏靠的是跨会话标识。
这时**不**按设备名退回去截：那个名字可能已经发给了另一块面板，截到的是没人批准过的画面）
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
`capture.cursor_unsupported`（**1**，要求的光标状态由这条通道结构上做不到：`--cursor include` 配 `printwindow` / `dwm` / `bitblt` / `duplication`，或整屏目标上配那几条没有开关的路径。解析期给出，`option`=`--cursor`、`value`=规范化取值、`message` 带实际通道与做得到的那一条（`wgc`）。**下一步是改用 `--capture wgc` 或去掉这条要求，而不是换一条会读桌面的通道试试**——工具自己绝不会换）
`capture.cursor_unverifiable`（7，明确要求过的光标状态在 `wgc` 那一次核实不了：那个开关取不到接口、设不下去，或设完读回来是相反的那一件。`message` 给要求的状态 + ASCII 原因名（`interface_unavailable` / `set_failed` / `read_failed` / `read_back_mismatch`）与实际读回的值；接口取不到时那一条就是 `E_NOINTERFACE`（这一问的下场确实是它），不拿 `E_FAIL` 顶。发生在 `StartCapture` **之前**，所以与要求相反的那一张根本不会交出去，一个像素都不落地。`--cursor default` 遇到同一情况不当失败，而是在结果里写 `cursorEffective: "unverified"`
`capture.hdr_unsupported`（**1**，要求的 HDR 处理由这条通道结构上做不到：`--hdr tonemap` / `refuse` 配 `printwindow` / `dwm` / `bitblt`（它们只带得回 8 位 SDR）。解析期给出，`option`=`--hdr`、`value`=规范化取值、`message` 带实际通道与带得回广色域的那两条（`wgc`、`duplication`）。**下一步是改用 `--capture wgc`/`duplication` 或去掉这条要求，而不是换一条会读桌面的通道试试**——工具自己绝不会换，也不摘 `auto` 链）
`capture.hdr_refused`（**7**，`--hdr refuse` 且这条路径核实回来的来源确是 HDR 帧（FP16 scRGB 或 10 位 PQ/HLG，`message` 给那个来源色彩空间名）。这正是用户要的"别把 HDR 硬压成发白图"，所以一个像素都不落地。下一步是改用 `--hdr tonemap` 拿那张映射后的 SDR，或换一条本来只交 8 位 SDR 的通道再问一次。不是换后端、也不是"那就整窗交出"的理由）
`capture.hdr_unverifiable`（**7**，这条路径带回一个本构建认不出的广色域像素格式（`message` 给那个 DXGI 格式编号）。认不出格式不等于硬按 BGRA8 解释（那正是发白图的成因），也不等于猜一个映射，所以这一张不落地。下一步是 `--hdr auto` 换一条通道或改要求，反复出现请把那个格式编号原样报给工具维护者）
`capture.roi_invalid`（7，交付的整窗图像比请求的裁剪矩形小：目标在选定之后改了尺寸、或被屏幕边缘裁短。这一张一个像素都不落地，既不往里挪，也不退回整窗交出；`message` 给图像实际尺寸与请求矩形的右下边 + ASCII 原因名）
`capture.roi_unmeasurable`（7，定位这块裁剪矩形所需要的那一问没有答案：客户区量不出来 （`client_unmeasurable`），或这块交付图像核实不出它对应屏幕上哪一块（`image_unmeasurable`）。与「放不下」分开给码：下一步是换一条窗口内容通道或整窗重取，而不是把请求往里挪挪）
`cap.scale_apply_failed`（7，形状判据放得下却缩不下来——帧在内存里的形状自相矛盾，这一张不落地；与 `cap.crop_apply_failed` 同一类：换一条 `--capture` 或重截一次，并把 `message` 里的尺寸报给本工具的维护者。缩放本身不会越界，所以没有第二种失败）
`capture.frame_invalid`（7，交回来的那帧像素自己说不通：宽高为 0、单边超过 16384 像素、行距装不下一行像素
（`< width*4`）或超过两倍行长、缓冲区比 `行距×高` 还短、整帧超过 1 GiB。裁剪 / 行重排 / 单色判定 / 编码之前都先核
这一道，所以坏帧不会被告知"成功"，也不会被读越界。上限与实际数字写在 `hint` 里，`stage` 是出问题那一步）
`capture.monitor_changed`（7，那块屏在本次请求里已经不在这台机器的桌面中，或它的画面（分辨率 / 旋转 / 位置）在人工
确认之后变了：下一步是**重新枚举显示器并重新确认**，不是换一条通道碰运气——另一块屏的画面谁都没批准过。本工具
绝不会静默改截别的屏幕，授权始终绑在人看过的那一块上。屏幕目标裁不出与该屏等大的图、以及桌面复制的
`AcquireNextFrame` 回 `DXGI_ERROR_ACCESS_LOST` / `NOT_FOUND` 都走这一条码）

**`env.*`（这一台机器的 Windows 版本给不出所要求的东西；两条都在枚举目标、弹框、读像素之前给出，`stage=capture`，退出码 7）**
`env.os_too_old`（7，本机 build 低于 10240：所有格式共用的那唯一一套 WinRT 编码器不在。`message` 给本机 build 与
要求的 build，`value` 是本机那个数字。**换 `--capture`、换目标、重试都不可能有别的下场**）
`env.channel_unsupported`（7，显式 `--capture` 指定的那条通道的下限高于本机 build。`message` 给通道名、要求的
build 与实际 build，`value` / `backend` 都是那条通道名。下一步是换一条通道或改用 `auto`，而工具不会自己把
指定的那条顶替掉）
`env.cursor_unsupported`（7，**这台机器**给不出这一次要求的光标状态：那个开关要 build 19041 而起，而本机更低；或 `auto` 链按光标要求筛完一条不剩。`message` 给要求的状态、本来要试的那几条与 ASCII 原因一览（含那道门槛的数字），`option`=`--cursor`、`value`=规范化取值、`backend`=本来要试的那些通道名，`stage=capture`。在枚举目标、确认框、输出名规划之前给出，一个像素都不取。下一步是 `--capabilities` 看 `cursor` 段或去掉这条要求，**不是**重试同一个目标）
判据、三条下限与实测范围见上面「运行环境与能力检查」一节。

**`io.*`**
`io.write_failed`（8，临时文件建不出来 / 写或刷新中断 / 提交为目标名失败）`io.file_exists`（8，配合 `--no-overwrite`）`io.output_collision`（8，整批输出名撞车，一张都没截也没写）`io.timeout`（8，`--timeout-ms` 在写文件 / 写 stdout 阶段耗尽，`stage=write`/`stdout`：已截好的那一张也不写）

**`note.*`（不是错误，`--quiet` 会去掉）**
`note.dry_run` `note.capture_channel`（`auto` 回退后实际用了哪条）`note.duplicate_value`
`note.extension_appended` `note.exe_path_looks_like_name`（`--exe` 传的像文件名不像完整路径）
`note.format_extension_mismatch` `note.format_defaulted_png` `note.output_defaulted_stdout`
`note.output_extension_appended` `note.quality_ignored` `note.all_without_placeholder`
`note.flag_overrides_quiet` `note.pipe_default_format` `note.json_flag_deprecated` `note.frame_uniform`（这一张整幅只有一个颜色：质量提示，图片照常交付）`note.crop_mapping_unavailable`（已按请求裁好，但这张交付图像核实不出它对应屏幕上哪一块，所以少了 `cropScreenRect` 那一行：`cropRect` 仍是图像自己的像素坐标，别拿它当桌面坐标用）`note.capture_clipped`（目标没被完整截下来：`message` 给"要截多大 / 只截到多大"，`hint` 给四边各少了几像素。图照常交付、退出码不变，配 `capturedRect` / `clipped` 一起看）
`note.cursor_channel_skipped`（`auto` 链里那一条**做不到这次要求的光标状态**、已从链中去掉：`message` 给通道名、要求的状态与一个 ASCII 原因 token（`os_below_min_build:19041` / `window_self_drawn` / `dwm_redirection_surface` / `screen_dc_has_no_pointer` / `pointer_shape_is_separate_metadata` / `not_registered`），`backend` 是被摘掉的那条，剩下的仍按顺序试。与 `note.channel_unavailable` 分开：那条说的是"本机版本用不了这条通道"，这条说的是"这条通道能用，但它兑现不了这次的光标要求"
`note.hdr_source_sdr`（写过 `--hdr tonemap` 或 `--hdr refuse`，而这一帧的来源核实是 8 位 SDR：那条处理是恒等的、没有改变任何一个像素。图照常交付、退出码不变，这条只是把"我要过 HDR 处理"与"其实这一帧没有 HDR"分开放在调用方眼前，免得把一次静默通过当成"HDR 已被正确映射"。`--hdr auto` 不发这条；`--quiet` 连同整段 notes 一起去掉）
`note.channel_unavailable`（`auto` 链里那一条被本机版本挡下、已从链中去掉：`message` 给通道名与两个 build
数字；图仍可能由别的那几条截到）`note.os_unverifiable`（本机 build 没问出来，所以这一次没有按版本筛通道 ——
问不出来不等于不支持，也不等于支持）
`note.help_ignored_arguments`
`note.window_query_stale`（每一次 `--list` / `--inspect` 都固定带这一条：交回的是**此刻的快照**，字段会过期，
`hwnd` / `pid` / 类名不是可以长期持有的凭证；真去截图时仍会在取帧之前复核身份。`--quiet` 会连同整段 notes
一起去掉，但 `policy` / `authorization` / `caveats` 里那三件自述不受 `--quiet` 影响）

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
# 0) 先只读问一次这台机器能走哪几条：不截图、不弹框、不写文件，也不需要窗口条件。
#    用它选 --capture、判断"这次失败该换通道还是这台机器不行"、以及确认这里弹框有没有人会答。
ECAPTURE.EXE --capabilities
ECAPTURE.EXE --diagnostics        # 要提交问题报告时用这份（构建标识 + 平台 + 后端状态）

# 0b) 要动哪一块屏？先列一次，然后按标识点名（别猜编号）。
#     交回的 selectors 就是能直接写回 --monitor 的那两条字符串。
ECAPTURE.EXE --screens
ECAPTURE.EXE --monitor device:DISPLAY1 --out D:\shots\m1.png     # 本次桌面连接的设备名
ECAPTURE.EXE --monitor "id:\\?\\DISPLAY#GSM41A2#5&…" --dry-run  # 跨会话那条，存档之后下次接着用
#     整屏拍的是桌面像素：一定弹确认框，--yes 跳不过。

# 1) 先只读发现：把候选列成结构化数据，不截图、不弹框、不写文件、也不需要 --out。
#    字段直接读（hwnd / pid / class / title / image / rect / visible / minimized / zOrder），
#    不要再去解析一句拼好的描述文本。命中总数看 pagination.matched，翻页用 --offset / --limit。
ECAPTURE.EXE --list --process notepad.exe
ECAPTURE.EXE --list --offset 50 --limit 50          # 上一页没列完时接着翻
ECAPTURE.EXE --list=all --class Notepad             # 把最小化窗口也并进同一根 Z 序轴
#    旧的探针入口照旧可用（挑中目标就返回，同样不落地、不打扰人）：
ECAPTURE.EXE --process notepad.exe --dry-run --out D:\shots\_probe.png

# 1b) 定一扇窗口看细节 + 拿身份约束：--inspect 用与截图同一套选择策略，定不出唯一一条就报歧义（+5），
#     不会替你挑一个。要归属映像完整路径写 --inspect=path（默认只写文件名，路径常含用户名）。
ECAPTURE.EXE --inspect --hwnd 0x001A0B4C
ECAPTURE.EXE --inspect=path --title LocalSend
#     identity 那一段是**下一次截图要带回去的约束**，不是许可证：processStartTicks 用来分辨 PID 有没有被复用，
#     selectionNeedsRecheck=true 说当初的条件含易变项（标题 / 按屏），截图前会拿条件重跑一次。
#     工具在取帧之前自己还会复核，所以不需要你手工传任何 token —— 这一份快照过期了就重新问一次。

# 2) 精确锁定一个窗口：默认 wgc 是窗口内容路径，带 --yes 才真的不弹框
ECAPTURE.EXE --title LocalSend --class UnityWndClass --yes --out D:\shots\game.png

# 3) 多匹配：从 --list 的 windows[].hwnd 里点名回来（这条没给 --yes，所以照样弹一次"是/否"框）
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite D:\shots\one.png

# 4) 每个命中窗口各一张（授权不跨请求缓存，所以每次调用都要重新带上 --yes）
ECAPTURE.EXE --pid 12345 --title-contains 报告 --all --yes --out "D:\shots\rpt_%i.png"

# 5) 图片进管道（JSON 于是在 stderr；stdout 一次只一张，多个目标请写到文件）
ECAPTURE.EXE --process notepad.exe --yes --out - > D:\shots\snap.png

# 6) 整屏 = 桌面像素：一定要人本人点"是"，--yes / --quiet / 环境变量都跳不过。
#    先把"会拍到什么范围"说清楚再启动，然后等用户点；被拒就是 capture.access_denied（+ stage=consent），
#    与有没有给 --out 无关（弹不出框则是 capture.consent_unavailable）
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"

# 7) 按屏过滤窗口（出的是窗口图字段，走窗口那一级：带 --yes 才不弹框，不给照样问一次）
ECAPTURE.EXE --monitor 2 --process chrome.exe --all --yes --out "D:\shots\m2_%i.png"

# 8) 只要屏幕上此刻的样子（连遮挡物一起要）：bitblt 整条都是桌面路径，一定弹框，--yes 在这里不起作用
ECAPTURE.EXE --class CabinetWClass --index 1 --capture bitblt --out D:\shots\visible.png

# 8b) 只要窗口图里的一块：--roi 的坐标是这张图像自己的像素（左上角 = (0,0)、物理像素），不是桌面坐标；
#     放不下的矩形是拒绝（match.roi_out_of_range+1 / capture.roi_invalid+7），不往里挪也不退回整窗交出。
#     它在取帧之后，所以桌面通道即使只截 8×8 也照样一定弹框，--yes 在这里不起作用。
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --roi 20,40,120,80 --out D:\shots\part.png
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --client-area --out D:\shots\client.png

# 9) auto 带 --yes：窗口内容那三条不问，一旦要迈进桌面路径照样弹框；拿到图看 images[].path / scope 才知道走了哪条
ECAPTURE.EXE --class CabinetWClass --index 1 --capture auto --yes --out D:\shots\auto.png

# 10) 给英文环境的人看诊断文字
ECAPTURE.EXE --process notepad.exe --yes --out D:\shots\a.png --lang en

# 11) 脚本里不许卡死：自动阶段一份总预算，耗尽给 match/capture/io.timeout（图不写、好图留着）；
#     等人工确认另计，超时算拒绝（capture.consent_timeout，退出码 6）
ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --out D:\shots\epad.png
ECAPTURE.EXE --monitor primary --consent-timeout-ms 60000 --out D:\shots\screen.png
```
