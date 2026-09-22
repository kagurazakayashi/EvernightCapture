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
| `--title-regex` | `-R` | ECMAScript 正则 | 正则**在匹配期编译并运行**（不在解析期，解析层根本不碰正则库），编译与求值整步跑在受期限与隔离控制的辅助进程里。三种失败分开：语法不合 → `cli.invalid_regex` + `1`（`stage=match`、`backend=match`）；撞到本机正则库的复杂度/回溯上限 → **同一个码、同样 `1`**，但那是"太复杂"那一条文案，加大 `--timeout-ms` 没有用；求值把预算花光 → `match.timeout` + `7`（跑它的那个辅助进程压根没回来则是 `capture.worker_failed` + `7`）。求值中途失败时**整份已收集命中作废**，绝不交回只求值了一半的清单。详见「期限与阻塞隔离」 |
| `--class` | `-c` | 窗口类名 | 忽略大小写，如 `Notepad` / `CabinetWClass` / `UnityWndClass` |
| `--index` | `-i` | 十进制，从 1 起（1..65535） | 多匹配消歧：取当下 Z 序（叠放次序）里的第 n 个。写法与区间见上面「取值写法」 |
| `--topmost-match` | | 开关 | 取当下 Z 序最靠前的命中窗口（此刻盖在最上面那一个） |
| `--bottommost-match` | | 开关 | 取当下 Z 序最靠后的命中窗口（此刻被压在最下面那一个） |
| `--newest` / `--oldest` | | 开关 | 上面两条的**旧名字**（兼容别名，行为完全相同）。它们选的一直是当下的 Z 序位置而不是创建时间 —— Windows 没有取窗口创建时间的公开 API，进程启动时间也不是窗口创建时间。写旧名字会多发一条 `note.deprecated_option`；同一条策略的新旧两种写法一起给（`--newest --topmost-match`）算一条策略，不是互斥冲突 |
| `--all` | `-a` | 开关 | 每个命中窗口各存一张；与 `--monitor all` 互斥 |
| `--capture` | `-C` | `wgc`（默认）/`dwm`/`printwindow`/`bitblt`/`duplication`/`auto` | 取图通道。取值写错解析期报 `cli.unknown_capture_method`，不会退化成默认值。**要不要问人，不看这里写的通道名，看实际走的那条内部路径**（`images[].path`，全表见「截图授权」一节）：`dwm` 的缩略图路径只取窗口画面，它那条"把宿主窗口盖到目标位置上再拷屏幕"的退路 `dwm.screen` 取的是桌面像素——这条退路只在缩略图那一步**真的失败**（PrintWindow 返回 FALSE、位图建不出来、宿主窗口量不出矩形）时才走，**不会因为画面正好是单色就走**（旧实现会，那等于把一扇本来就纯色的窗口升级到要另外授权的桌面取样）。`duplication` 取的是**某一块输出**的合成分：它会先枚举全部显卡适配器与输出定位目标、在该输出所属适配器上建设备（所以由第二块显卡驱动的屏也截得到，这条路上没有 WARP 兜底），并按该输出的显示方向把桌面帧顺时针转 0/90/180/270 度，交付图因此在虚拟屏幕坐标那一套系里（转了几度写在 `images[].rotation`）；一个目标只取与它重叠最多的那块输出，没截全时报 `capturedRect` / `clipped` 并发 `note.capture_clipped`，跨显卡拼图未实现。屏幕模式只支持 `wgc`/`duplication`/`bitblt`/`auto`，`dwm`/`printwindow` 报 `capture.unsupported`（整屏 `wgc` 也是桌面像素） |
| `--cursor` | | `default`（默认）/`include`/`exclude` | **画面里要不要鼠标指针**。判据是那条路径的**来源像素里有没有光标**，不是通道名字：`wgc` / `screen.wgc` 有一条真能设进去、也能读回来核实的开关（`IGraphicsCaptureSession2::IsCursorCaptureEnabled`，**要 build 19041**，比 `wgc` 通道自己的 18362 还高）；`printwindow` / `dwm.thumbnail` / `dwm.screen` / `bitblt.screen` / `screen.bitblt` 这几条的来源像素里根本没有光标（窗口自绘、DWM 重定向面、屏幕 DC），所以对它们 `exclude` 是来源那一层的事实、`include` 就是做不到。`duplication.frame` / `screen.duplication` 是另一种下场：登记为 `pointer_state_unverified` —— 桌面复制交回的那一幅桌面图像**可能已经把指针画在上面**（官方说明：指针要么已画进帧里、要么由显卡单独叠加），而这条路径没有可设也读得回来的开关，所以它**既保证不了画、也保证不了不画**。于是 `include` 配上面那几条、以及 `include` 或 `exclude` **两样**配 duplication，都在**解析期**报 `capture.cursor_unsupported`+1（`option`=`--cursor`、`value`=规范化取值，`message` 带实际通道与做得到的那一条；两种原因各有一种措辞，`include` 是"这条来源没有指针开关"，`exclude` 是"桌面帧里指针的状态证明不了"），**绝不改走去读桌面像素的通道**——那既加不回光标、也保证不了去掉一个，交回的还是一份没人批准过的画面；`--capture auto` 时兑现不了的那几条从链里摘掉并各留一条 `note.cursor_channel_skipped`（`backend`=被摘的那条，`message` 末尾是 ASCII 原因 token：`os_below_min_build:19041` / `window_self_drawn` / `dwm_redirection_surface` / `screen_dc_has_no_pointer` / `desktop_frame_pointer_state_unverified`（include 那一路摘 duplication）/ `duplication_cursor_exclusion_unprovable`（exclude 那一路摘 duplication）/ `not_registered`），摘到一条不剩就是 `env.cursor_unsupported`+7，一个像素都不取、也不弹框。取值只认那三个词（忽略大小写与首尾空白，内联 `--cursor=exclude` 也认），写别的报 `cli.invalid_value`+1 而不退化成 `default`；重复给出最后一个生效；`-v` 回显 `input.cursor` 与 `input.cursorGiven`。**默认值真的不动任何东西**：没写这条选项时不碰任何开关、结果里那三个键一个都不出现。写了 `--cursor default` 是"不要求改动，但把读到的状态报出来"。本工具**从不**用图像修补去加或去抹光标（不取指针形状来合成、不画光标、也不动使用者的鼠标），所以 `cursorEffective` 只断言到"这条会话被设成画/不画"或"这条路径的来源没有光标"那一层，**不**断言这一张图里此刻看得见或看不见指针；`--cursor default`（整条没写时那三个键根本不出现）配 duplication 时那张图照旧交付，而 `cursorEffective` 写 `unverified`、`cursorBasis` 写 `path_pointer_state_unverified` —— 这一格既不是"画了"也不是"没画"的证据。这条选项不改变授权：`exclude` 配 `bitblt` 的屏幕路径、`default` 配 `duplication` 与任何整屏目标照样一定弹框，`--yes` 照样管不着（而 `exclude` 配 `duplication` 在解析期就被拒，根本走不到弹框那一步）。 |
| `--hdr` | | `auto`（默认）/`tonemap`/`refuse` | **HDR 来源怎么处理**。显示器在 HDR 模式时采集回来的帧可能带超出 SDR 的亮度范围与另一种传递函数（WGC 可按 FP16 scRGB 线性交回；桌面复制的桌面纹理可能是 FP16 scRGB 或 10 位 ST.2084 (PQ) / HLG BT.2020）。把它硬按 8 位 BGRA 解释会得到一张发白、去饱和、亮部一团糊却"看着像正常图"的结果——本工具不把这当成正确的默认交付。这里要分两问，判据都是那条路径的**来源像素**而不是通道名字：一、**带不带得回广色域帧**（`capability` = `wide_gamut_capable` / `wide_gamut_unverified` / `sdr_source_only`）；二、**本构建兑现不兑现得了显式策略**（登记表里每行一条 `honorsExplicitPolicy`）。本构建只有 `wgc` / `screen.wgc` 两条是 `true`（来源跟随显示模式，且这条路径读回自己实际拿到的格式，所以既报得出也映射得了）；`duplication.frame` / `screen.duplication` 是 `wide_gamut_unverified` + `false`（那份桌面纹理确实可能是 FP16 scRGB 或 10 位 PQ/HLG，但本构建走 `DuplicateOutput()`、采集之前不问那块屏此刻的色彩空间，所以不敢替它声称兑现得了 `tonemap` / `refuse`）；`printwindow` / `dwm.thumbnail` / `dwm.screen` / `bitblt.screen` / `screen.bitblt` 是 `sdr_source_only` + `false`（结构上只有 8 位 SDR，处理对它们是恒等，不是"做不到就换通道"）。`tonemap` = 在编码之前把广色域帧经一份逐像素浮点中间量按固定曲线映射成 8 位 sRGB 交付（不分配整幅浮点帧；来源本就是 SDR 时是恒等透传）；`refuse` = 核实来源确是 HDR 就报错、一个像素都不落地；`auto` = 本工具对色彩一个字都不改（不探测显示、不改采集格式、不做映射）。`auto` 同时是**省略这条选项时的默认取值**，但"省略"与"明确写出"在**上报**上必须分开（见下面那一段），取图行为两者一致。**做不到的那条就拒绝、绝不改走去读桌面像素的通道**（与 `--cursor` 同一条原则）：显式 `--capture` 点了兑现不了这次要求的通道时，`tonemap` / `refuse` 在**解析期**报 `capture.hdr_unsupported`+1（`option`=`--hdr`、`value`=规范化取值、`message` 带实际通道与本构建兑现得了的那两条 `wgc` / `screen.wgc`），工具自己绝不换后端；`--capture auto` 时这条要求**筛那条回退链**——`FilterChainForHdr` 只留下真兑现得了的那几条（与 `-v` 回显的 `input.captureChain` 出自同一判据），被摘掉的每条各留一条 `note.hdr_channel_skipped`（`backend`=被摘的那条），摘到一条不剩就是 `env.hdr_unsupported`+7，一个像素都不取、不弹框、不写文件。**运行期出图之后才判出的策略结论同样是结论**：`refuse` 核实来源是 HDR = `capture.hdr_refused`+7，带回一个认不出的广色域格式 = `capture.hdr_unverifiable`+7，两种都一个像素不落地，而且**回退链到这里立刻停下、不会换一条后端重跑**——换一条只交 8 位的后端再出一张，等于把这次拒绝换成一次静默降级。探测显示 HDR 状态走只读的 `IDXGIOutput6::GetDesc1`，绝不改显示设置；问不出来是 `unknown`，既不折成"是 HDR"也不折成"是 SDR"（`refuse` 不因此拒绝，它拒的是确凿是 HDR）。取值只认那三个词（忽略大小写与空白，内联 `--hdr=tonemap` 也认），写别的报 `cli.invalid_value`+1 而不退化成 `auto`；重复给出最后一个生效；`-v` 回显 `input.hdr` 与 `input.hdrGiven`。**省略与明确写 `--hdr auto` 是两种上报**：整条没写时不改采集格式、结果里色彩那组键（`hdrRequested` / `hdrEffective` / `hdrBasis` / `sourceColorSpace` / `sourceBitDepth`）一个都不出现，输出与这条选项存在之前逐字节相同；明确写了 `--hdr auto` 时对色彩同样一个字都不改，**但那组键照常出现**，只被动报这一帧实际带回的样子——一张 8 位交付的 `wgc` / `screen.wgc` 帧因此是 `hdrEffective: unverified` + `hdrBasis: bgra8_source_unverified`，因为 `auto` 从没问过那块屏此刻是不是 HDR，而一张 8 位 surface 本身证明不了原始内容就是 SDR。`sdr_passthrough`（配 `delivered_bgra8_sdr`）只在确实核实过来源是 8 位 SDR 时才写，`tone_mapped` 说这一帧过了那条固定映射链路；`unverified` 两种都不折，**不**说色彩正确性被验过（本项目无 HDR 屏，`--capabilities` 的 `color.verifiedOnThisMachine` 恒 `no`）。`-v` 的 `input.hdrGiven` 就是"没写"与"写了 auto"那条区分。明确要过处理（`tonemap` / `refuse`）而这一张是按 8 位交付的，两条提示分开发：`note.hdr_source_sdr` 只有**采集之前真的问到那块屏此刻是 SDR** 时才发，那一问没答案时改发 `note.hdr_source_unverified`（图照常交付、退出码不变）——一张 8 位帧本身不证明原始内容是 SDR，所以"没核实"不会被写成"这一帧没有 HDR 可映射"；`auto` 两条都不发。这条选项不改变授权：会读桌面像素那几条照样一定弹框、`--yes` 照样管不着 |
| `--roi` | | `<x,y,w,h>` | **窗口内部裁剪**：从交付的整窗图像里裁出 x,y 起点、w×h 大小的一块。原点 `(0,0)` 是**这张图像自己的左上角像素**（图像对应的是用户看到的可见边框，`GetWindowRect` 还算在内的 DWM 透明 resize 边框不在里面），单位物理像素且**不按 DPI 缩放**（本进程 per-monitor v2，要按逻辑像素指定就自己乘缩放） —— 所以这四个数永远不会被当成桌面绝对坐标。四段都只认 `[0-9]+`、逗号分隔、不认空白/正负号/小数点/指数/下划线/`0x`与非 ASCII 数字；`x`/`y` 可为 0，`w`/`h` 至少 1，四条都不超过 16384（= 帧的单边上限，`--capabilities` 报成 `limits.roiMaxValue`）。写法不合 = `cli.invalid_value`+1。放不下 = 拒绝，绝不往里挪、裁到边上为止、也不退回整窗交出：取帧之前就看得出放不下报 `match.roi_out_of_range`+1（在确认框与输出名规划之前，不弹框、不写文件），取到帧之后才发现报 `capture.roi_invalid`+7（目标改了尺寸或被屏幕边缘裁短）。与 `--client-area` 互斥（`cli.crop_conflict`+1），与整块屏幕的目标说不通（`capture.unsupported`+1，**不会**改按桌面坐标去截），与只读查询一起给也算冲突。`--dry-run` 不取帧所以不判这条几何。结果里多带 `cropMode` / `cropRect`（图像坐标）/ `fullWidth` / `fullHeight` / `cropScreenRect`（屏幕坐标，只在图像原点核实得出来时才写，否则整个键不出现并留 `note.crop_mapping_unavailable`） |
| `--client-area` | | 开关 | 只交回窗口客户区那一块：在交付的整窗图像里再去掉标题栏与三边边框。这块矩形照目标此刻的几何量出来（`GetClientRect` + `ClientToScreen`），所以坐标系与单位跟 `--roi` 完全同一套。客户区问不出来 = `capture.roi_unmeasurable`+7，客户区有一边落在交付图像之外 = `capture.roi_invalid`+7（挂在屏外、或中途改了尺寸），两种都不退回整窗交出。`--client-area=false` 与普通开关同义 = 没写。与 `--roi` 互斥 |
| `--scale` | | `key=N[,...]` | **等比缩小**：把即将交付的这张图缩到天花板之内，**只缩不放**。三个键：`max-width=N` 限宽、`max-height=N` 限高、`max-pixels=N` 限总像素数（大小写不敏感；N 只认十进制，边长 1..16384（与 `--roi` 同一条线）、像素数 1..268435456）。可以只给一条，也可以一条里用逗号串几条；这一条写多次时每条天花板各记各的，重复给同一条时最后一个生效；一条都不给 = `cli.invalid_value`+1，**整条不生效**，不留下认得的那半。三条都按同一个比例缩（取最紧的那一条），宽高各自**向下取整**且各至少留 1 像素；本来就在天花板之内就原样交付（结果里 `scaleApplied: false`）。插值策略只有一种而且可预测：最近邻。顺序是**先裁（`--roi` / `--client-area`）后缩、再编码**，所以缩的是裁完的那一块；结果里 `scaleFromWidth` / `scaleFromHeight` 是缩之前的尺寸、`width` / `height` 是缩之后的。这一条**不改变授权与帧上限**：会从屏幕上取样的那几条照样一定弹框（`--yes` 不因为最后交的是小图而生效），`--roi` 的越界判据仍按未缩的那张图判。与任何环境查询（`cli.query_conflict`）或窗口查询（`cli.window_query_conflict`）同时给出都是冲突 |
| `--yes` | `-y` | 开关，可写 `=true/false` | **截图授权**：只免掉"只取所选窗口画面"那几条路径（`wgc` / `printwindow` / `dwm.thumbnail`）的确认框。裸写与 `=true/1/yes/y/on` = 开，`=false/0/no/n/off` = 关（它虽是正向开关，写 `=false` 却**有意义**：明确要问），重复给出时最后一个生效，最终结果由 `-v` 的 `input.yes` 回显；写成两头都不沾的取值（`--yes=maybe`）解析期就报 `cli.switch_takes_no_value`+1，不会当成"开了"。**其它一概不保证**：不保证目标真交出有效帧、不越过权限、不解除受保护内容、不吞掉任何错误，也不影响覆盖保护。凡是从屏幕上取像素的路径（`bitblt`、`duplication`、任何整屏、`dwm` 的屏幕退路）一定会弹框，这个开关跳不过 |
| `--timeout-ms` | | 毫秒，0–86400000 | **自动阶段的总预算**：从选定目标起，匹配（含 `--title-regex` 求值）、`auto` 的后端重试、等帧、编码、写文件 / 写 stdout 共用这一份剩余时间，整批只发一次，没有哪一步或哪个目标能另领一份。省略或 `0` = 不设总预算，此时被隔离进辅助进程执行的那几步（见「期限与阻塞隔离」）仍有内置 5000 ms 上限兜底，`--capture printwindow` / `dwm` 不再能无限期卡住；**给了 `--timeout-ms` 之后，隔离调用拿到的就是那一刻的剩余预算全额**——不再有第二道内置上限，辅助进程自己也不再挂一个与预算无关的固定秒表。预算耗尽时**还没开工的那一步被拒**，它那张图**不写**：按阶段报 `match.timeout`（`stage=match`）/ `capture.timeout`（等帧没等到是 `stage=capture`，预算死在编码器里是 `stage=encode`——同一个码，靠 `stage` 分这两种）/ `io.timeout`（`stage=write` / `stdout`，退出码 8）；剩下的目标不再开始，已经写好的图留着。**已经提交到一半的那个文件照样落地**（原子改名没有取消点），写完返回之后会**再复核一次期限**：这时若预算已经越过，就在交付记录之外**额外**记一条 `io.timeout`，而那张图仍在 `images` 里、`captured` 照旧计入，本次按部分成功报**退出码 `7`**（已交付 + 出错），不是那张都没落地时的 `8`。超时不回滚、也不删——盘上存在的文件是调用方自己要的，悄悄删掉等于再做一次没人要求的写。等人工确认**不计入**这条预算。只认十进制 `[0-9]+`（`0x…`、负号、下划线、指数、空白与非 ASCII 数字一律拒收），重复给出最后一个生效，最终结果由 `-v` 的 `input.timeoutMs` 回显 |
| `--consent-timeout-ms` | | 毫秒，0–86400000 | 确认框最多等人回答多久；省略或 `0` = 一直等。超时按**拒绝**处理而绝不当作同意：报 `capture.consent_timeout` + 退出码 6、`stage=consent`。这一段单独计时，**不消耗** `--timeout-ms` 的预算（等人在读那份确认文案不算"这台机器慢"，暂停也不会把已经烧掉的预算回填）；点「是」之后那约 1 秒的关框动画缓冲属于人工阶段，不会为了赶预算被跳过，超时也绝不削减它。这是**轮询**到的期限（约 50 ms 一个切片），到点后另有约 3 秒关框宽限，所以框可能比这个数字晚一小会儿消失；期限先到就 latch，同切片里紧接着出现的「是」照样算拒绝（细节见「确认框与诊断」）。取值写法与回显同上（`input.consentTimeoutMs`） |
| `--out` | `-o` | 路径或 `-` | `-` = 图片字节写标准输出。也可用位置参数；完全不给时等同 `--out -`。整批的最终绝对路径**在任何确认框与第一帧之前**一次算好（框上列的就是这些名字）：扩展名缺了就补，两个目标算出同一个名字就报 `io.output_collision`+8 且整批不作，绝不静默改名。`-` 不是路径，不参与展开与碰撞检测，而且**一次只交付一张图**：选中的目标多于一个而输出是 `-`（含没给输出路径）时，整批在确认框与取第一帧之前就报 `cli.stdout_multiple_targets`+1，一张都不截、一个文件都不写；判据是实际命中的目标数，所以 `--all` 只命中一个窗口时照样可以写 `-` |
| `--format` | `-f` | `png`/`jpg`/`jpeg`/`bmp`/`tiff`/`gif` | 不给则由扩展名判定；扩展名判不出时用 png 并发 `note.format_defaulted_png`（文件名不改）。**没有 `webp`、没有 `ico`、没有 `auto`** |
| `--quality` | | 十进制 1..100，默认 100 | 只对 jpeg 生效，给别的格式发 `note.quality_ignored`。只认 `[0-9]+`（`--quality 1e` 不会变成 30） |
| `--no-overwrite` | | 开关，可写 `=true/false` | 目标已存在时报 `io.file_exists` 且不覆盖。裸写与 `=true/1/yes/y/on` 同义（禁止覆盖），`=false/0/no/n/off` 取消禁令；重复给出时最后一个生效。**已存在与否由最后那次不许替换的改名原子判定**，没有「先查一下」那种竞态预检 |
| `--dry-run` | `-d` | 开关 | 选完窗口就返回，只给 `note.dry_run`；不取帧、不写文件、**在任何确认框之前就已返回**（注定什么都没截的调用不会先打扰人一次）。**不需要输出路径**：没给时等同 `--out -`，因为没有图片要交付，整份 JSON 走 stderr 并留一条 `note.output_defaulted_stdout`；给了输出路径也不会建那个文件。单独一条 `--dry-run` 而没有任何窗口条件仍然算"没给条件"→ 文本帮助 + 退出码 2 |
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
**条件永远不会跨两扇窗口拼起来**：`--process a.exe --title X` 找的是"同一扇窗口既是 a.exe 的窗口、标题又是 X"，
不是"a.exe 的窗口加上标题为 X 的窗口"。
**问不出来绝不等于命中，也绝不等于没命中之外的好消息**：读不到标题或进程信息（`denied` / `failed`）的那扇窗口
就是不满足需要这项信息的条件；`--title-regex` 求值失败时，这次已经收集到的所有命中**整份作废**并照实报错，
而不是交回一份"求值做了一半"的清单让人以为那就是全部结果。正则是**在匹配期编译并运行**的（解析层不构造正则，
所以语法这一判也发生在匹配期），跑在受期限与隔离控制的辅助进程里，它失败的几种方式各有下一步：
语法不合 → `cli.invalid_regex` + `1`（`stage=match`、`backend=match`）；撞到本机正则库的复杂度 / 回溯上限 →
同一个 `cli.invalid_regex` + `1`，换的是"太复杂"那句文案，其 `hint` 明说**加大 `--timeout-ms` 没有用**（那是有界的
资源停止，不是"慢但还能跑"）；求值把预算花光 → `match.timeout` + `7`；跑这一问的辅助进程自己没回来 →
`capture.worker_failed` + `7`。`--list` / `--inspect` 用的是同一套判据与同一条执行路线，所以查询里也不会有半评估的结果。

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
| `status` | `available` / `unavailable` / `unverified` —— 本机**现在**的判据（版本下限 + 屏幕拓扑）让不让走；`unverified` 是"该问的那一问答不出来"，**不等于** `unavailable` | 不是"某个窗口一定截得到"；驱动、受保护内容、HDR 都不在这层断言里 |
| `verifiedOnThisMachine` / `os.matchesTestedEnvironment` | 本机是否**匹配**项目记录的那一台实测环境（`os.build` 与架构对得上记下来的 19045 x64 就是 `yes`） | 不是"能用"也不是"不能用"，更不是"你这台显卡 / 驱动 / 显示器 / 这块 HDR 屏被实测过"：它比的是系统版本与架构，一台报 26100 的机器对每条路线都如实写 `no`，那是一次"没在这儿测过"，不是失败、也不需要你去放宽任何判据 |

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
| `cursor` | `option` / `default`（`default` = 不要求，本工具一个字都不改）/ `values` 三种取值；`switch` 那一条唯一的开关（`api` = `IGraphicsCaptureSession2::IsCursorCaptureEnabled`、`compiled` / `status` / `reason` / `minBuild` 19041 / `verifiedOnThisMachine`）；`paths[]` 每条已登记内部路径一行（`capability` 是 `settable` / `excludes_cursor` / `pointer_state_unverified` / `unregistered`，`reason` 是那条路径的根据，`include` / `exclude` 各是三值 `yes` / `no` / `unknown` —— 问不出来就是 `unknown`，不折成任何一边）；`duplication.frame` / `screen.duplication` 那两条是 `capability: pointer_state_unverified` 且 `include` 与 `exclude` **两个都是 `no`**（`reason` = `desktop_frame_pointer_state_unverified`：那一幅桌面图像可能已经把指针画在上面，而这条路径没有可读回也没有可设的开关，所以两头都保证不了）；未登记的路径读 `unknown` 而不是猜一个答案；末尾 `pointerShapeCompositing: "never"` 与 `pixelRetouching: "never"` 说本工具不动指针形状、也不修图 |
| `color` | `option` / `default`（`auto` = 不要求，本工具一个字都不改）/ `values` 三种取值（`auto` / `tonemap` / `refuse`）；`compiled` / `status` / `reason` / `verifiedOnThisMachine`。`status` 说的是这个构建带不带得回广色域帧 + tone mapping 怎么做，**不**去问那块屏此刻是不是 HDR 模式（`reason` = `hdr_display_mode_not_probed`，无可用显示拓扑时才是 `unavailable`）；`verifiedOnThisMachine` 恒 `no`（本项目没有能开 HDR 的显示器，不宣称色彩验收通过）。`paths[]` 每条已登记内部路径一行（`capability` 是 `wide_gamut_capable` / `wide_gamut_unverified` / `sdr_source_only` / `unregistered`，`reason` 是那条路径的根据，再加一条 `honorsExplicitPolicy`：本构建里只有 `wgc` 与 `screen.wgc` 为 `true`，`duplication` 那两条是 `wide_gamut_unverified` + `false`（采集之前不问色彩空间，所以不敢声称兑现得了显式 `tonemap` / `refuse`），其余是 `sdr_source_only` + `false`）。再加 `toneMapping`（那条固定曲线的名字）/ `floatIntermediateFrame: "per_pixel_registers"`（不分配整幅浮点帧）/ `encoderOutput: "sdr_bgra8"`（HDR 一律映射成 8 位 SDR 交付，不出 HDR 原生图）。caveats 恒含这四条 HDR token（`hdr_tone_mapping_not_verified_on_hdr_display`、`hdr_output_is_tone_mapped_to_sdr_bgra8`、`hdr_explicit_policy_only_fulfilled_by_wgc` 与 `hdr_pixel_layout_is_not_a_color_space`）|
| `autoChainWindow` / `autoChainScreen` | 本机现在能试的 `auto` 链。与真实截图那次 `-v` 回显的 `input.captureChain` 由**同一个** `GateChannels` 算出，`tests\capabilities.ps1` 判两处一致 |
| `limits` | `maxFrameSide` 16384、`maxFrameBytes` 1 GiB、`maxTimeoutMs` 86400000、`isolatedCallMs` 5000、`maxWgcRecreates` 4、`maxOrdinal` 65535、`maxPid` 4294967295、`stdoutTargetsMax` 1、`jpegQualityMin`/`Max` 1/100、`roiMaxValue` 16384（= `--roi` 与 `--scale` 那条边长的同一条线） |
| `privacy` | 自述：`capturesScreen` / `showsDialog` / `uploads` / `enumeratesUserFiles` / `readsEnvironmentVariables` / `includesUsernames` / `includesPaths` 全为 `false` |
| `caveats` | 稳定 ASCII token，列"这份报告没断言什么"：`no_capture_performed`、`no_consent_dialog_shown`、`available_is_not_a_guarantee`、`device_capability_not_predicted`、`encoder_state_not_probed`、`consent_dialog_state_inferred_not_probed`、`subsystem_version_is_linker_default`；光标那一段恒带 `cursor_effective_is_a_setting_not_a_pixel_check`、`pointer_shape_never_composited_nor_erased` 与 `duplication_desktop_frame_pointer_not_guaranteed`；色彩那一段恒带 `hdr_tone_mapping_not_verified_on_hdr_display`、`hdr_output_is_tone_mapped_to_sdr_bgra8`、`hdr_explicit_policy_only_fulfilled_by_wgc` 与 `hdr_pixel_layout_is_not_a_color_space`；按本机情况追加 `os_version_unavailable` / `display_topology_absent` / `display_topology_unavailable` / `remote_session_observed` / `desktop_paths_need_answerable_dialog` / `unelevated_process_may_miss_elevated_targets` / `build_identity_unavailable` / `this_environment_not_tested` / `tested_environment_unknown`。**这一段是 `-q` 会去掉的那一段**，所以要把"这份报告没说过什么"读全的时候就别加 `--quiet` |

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
事实只有这两样：这一问的读数状态，和那一条 API 的原样错误码。`denied` 说的是**这一问被拒绝了**
（`win32` 为 `5` 时就是 `ERROR_ACCESS_DENIED`）——对方的权限或完整性级别与本机不同只是**一种可能原因**，
这个字段既证明不了它，也证明不了目标受保护，更说明不了截图会不会成功（`unreadable_fields_are_not_a_prediction`
那条 caveat 钉的就是这点）。所以别把根因写成结论，也别因此自动提权或以管理员重试：**不因此要求你以管理员运行**。
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

退出码用这六个：`0` 查询成功（包括命中 0 条）/ `1` 用法不合契约（含与截图那一级选项冲突、条件本身写坏、
`--index` / `--monitor` 编号越界，以及 `--title-regex` 在匹配期被判出的语法不合与复杂度上限 —— 那一条码是
`cli.invalid_regex`，退出码仍是 `1`，只是 `stage=match`）/ `4` 没有目标命中（`--inspect` 无匹配、
`match.monitor_unknown_id`）/ `5` 多匹配歧义（`--inspect` 定不出唯一一条、`match.monitor_ambiguous_id`）/
`7` 这一问没能问完：`match.timeout`（预算烧在条件求值上）、`capture.worker_failed`（跑这一问的辅助进程自己没回来）、
`capture.failed`（求值那一步的其它故障）、`match.monitor_id_unverifiable`（屏幕身份那一问整条没答案）——
说的都是「这一次问答没能问完」，与取帧无关；`9` 内部异常。
**`6` 与 `8` 不可能出现** —— 那两条说的是「没人批准」与「写文件失败」，而一条不弹框、不落地的
命令没有资格报它们。窗口查询那一条 `match.timeout` 的 `hint` 也是查询自己的说法：它明说
「换 `--capture` 没有用」，因为这一路根本没有通道可换。
**这两条码在这里照原样交出、不重新分类**：条件求值失败交回的是 `windowquery` / `windowinspect` 那一份契约文档
（`authorization` / `policy` / `pagination` 齐全，`windows: []`、`matched: 0`），所以"没命中"与"没问出来"只能靠
`errors[].code` 分开 —— 只数列表会把它当成"整机就这几个窗口"；而解析期那一级冲突仍交回与**截图结果同形**的失败
文档（`captured: 0`、`images: []`、`errors: [...]`），按 `errors[].code` 分支的那段代码两种形状都不用各写一份。

## 只读的屏幕枚举（`--screens`）

「要那一块屏」此前只有一种写法：`--monitor <n>`，而那个 n 是本次枚举顺序里的位置。猜错编号不会响亮地失败——
它会把另一块屏拍到磁盘上，而那块画面没人批准过。这条命令把屏幕身份当数据交回来，其中两种可以直接写回 `--monitor`。

```powershell
ECAPTURE.EXE --screens                                   # 契约名 screens：每块屏连同它的几种身份
ECAPTURE.EXE --monitor device:DISPLAY1 --out D:\shots\m1.png    # 按本次桌面连接的设备名点名
# 按跨会话的监视器设备路径点名：把 --screens 交回的 selectors.id **原样**抄过来并整体加引号
# （它长这样：id:\\?\DISPLAY#GSM41A2#5&2f186dd&0&UID8388688#{e6f07b5f-…}；Git Bash 要先
#  export MSYS2_ARG_CONV_EXCL='*' 并用单引号，否则反斜杠被吃掉会变成 match.monitor_unknown_id）
ECAPTURE.EXE --monitor 'id:\\?\DISPLAY#GSM41A2#5&2f186dd&0&UID8388688#{e6f07b5f-…}' --out D:\shots\m2.png
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
- **失败没有专门的码**：形状判据说放得下却实际缩不下来（帧在内存里的形状自相矛盾）时，`errors[].code` 就是
  `capture.frame_invalid`（7），这一张不落地；`message` 会写明"判据说过得去的那次裁剪 / 缩放没能落地"，
  裁剪那一条同形（也是 `capture.frame_invalid`）。缩放本身只会往小里挑尺寸、不会自己越界，所以没有第二种失败码。

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
- 一份授权的作用域写死了，而且它是一张**快照**：弹框之前先把这一轮的「框上列出的那批目标 + 各自的屏幕区域 +
  整批展开后的绝对输出名 + 屏幕拓扑指纹」冻结下来，框上写的就是这一份，凭证签的也是这一份。只覆盖这一次请求，
  不跨请求缓存、不扩大到没列出的目标、也不会从窗口范围自动升到桌面范围。**复核有两次**：签发之前重查一次拓扑，与
  **弹框前**那份快照比对——确认期间热插拔、拔线、改分辨率或挪了屏幕位置时，人在框上最后看到的还是旧布局，所以这一问
  一张都不采、不签发凭证，也不在同一次调用里按新布局再弹一次框（反复弹框等于无限重复问人）；每次真正要采样像素之前
  再复核一次那块区域是否还在批准的那一片里。基线取的是弹框**之前**那一次读数，不是应答之后第一次所见的新布局。
  两种过期都报 `capture.consent_stale`（7），批次不会因它像"有人答否"那样整批停住；调用方拿新的事实重来一遍，
  会在那份新快照上重新问一次。

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
- **脚本 / AI 调用给 `--timeout-ms`**（例如 5000）配 `--yes`（窗口内容路径），并且**照实向下层转述它的边界**：
  这份预算管的是可中断点与被隔离的那几步（匹配求值、`printwindow` / `dwm` 回读、等帧、开工前的编码与写），
  **不等于"这个调用永远不会卡住"** —— 原子写文件、没人抽走的 stdout 管道、无视取消的 WinRT 编码器都没有中途
  抢占点，只能在开工前拦、完工后再核；而**确认框不吃这份预算**，"到点有没有人答"要另外用
  `--consent-timeout-ms` 兜。上层调用方自己那层超时 / 看门狗仍然要留着，别把 `--timeout-ms` 当成对外承诺。
  `--yes` 照旧管不到桌面路径（`bitblt` / `duplication` / 任何整屏 / `dwm` 的屏幕退路）——那些一定弹框，
  设了人工期限才可能超时成 `capture.consent_timeout`。等人工确认不消耗 `--timeout-ms`，人答得慢不会把自动阶段预算吃光。
- **会拍到别家窗口时先向用户说明范围**（要 `bitblt` / `duplication`、要整屏、或 `auto` 有可能退到桌面路径），
  启动之后**等用户本人在框上点「是」**。
- **不得用脚本、`SendMessage`、UI 自动化代点**那个框——代点等于替人做了这个决定。
  本仓库自己"过框"有两套东西，别把一套当成另一套：`tests\consent_state.cpp` 的 `FakePrompt` 只喂给进程内的
  `ConsentGate`，既不弹真框也不采像素，而且只链接进测试二进制，`ECAPTURE.EXE` 里没有这条路；另一套是
  `tests\harness.psm1` 的 `Click-EcDialogButton`，它给**真的确认框**发 `BM_CLICK`，`-SimulateConsent` 之下
  `channels.ps1`、`screen.ps1`、`dup.ps1`、`identity.ps1` 会答 `IDYES`。后者的存在只是因为人自己挑了一台可以拍的
  专用桌面并显式要跑那几套判据——那是"测试在验证机制"，不是"有人同意了这一次请求"，调用方不能拿它当代答的依据，
  更不能据此以为所有自动确认都只发生在离线 fake 里。
- **用户拒绝不是技术故障，不得重试**：`capture.access_denied` 就停下来问用户怎么办；
  `capture.consent_timeout` 是"那一段时间里没有人应答"，不是"用户不同意"，要做的是确认有人在之后再开**一次新的请求**；
  `capture.consent_unavailable` 是"那个会话里根本没有人能答"（服务、计划任务、锁屏），要做的是换会话而不是再弹一遍。
  这三条工具自己都不会换后端再试，调用方也不许用"换一条通道再来一次"绕开这个决定。
- **没有授权的下一次截图不是重试能得到的东西**：`capture.consent_stale`（批准时看到的那份事实已经不在了）、
  身份那三条与屏幕那两条，都要**重新发现目标 + 由用户本人在新的确认框上答"是"**。同一次调用里不会重弹一次框，
  所以"自动重试"永远换不来一次新的授权；把重问写成"重试"就是把人没批准过的画面写进图里。
- 读到图先看 `images[].scope`：`desktop` 就意味着这张图里可能出现别人的窗口、文档、通知，
  转述与存档时按这个来说；别只看 `source` 就断定"截的是那个窗口自己"。
- **`capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable` 一律重新枚举、重新选目标**，
  不要换通道重试，也不要把条件放宽一点再试一次：这三条说的是"选定之后目标已经不是那一扇了"，而工具不会拿先前批准的
  许可去截一个后来的新对象。`capture.target_changed` 常常是因为目标改了标题、不再满足你给的 `--title*` 条件 ——
  这时该重新确认一次"要截哪个窗口"，而不是假定它还是同一个东西。
- **先确认手里那个二进制就是本文描述的这一版**：同印 `0.4.0` 的旧副本可能实现的是旧契约。只读地比
  `--capabilities`：`cursor.paths[]` 里 `duplication.frame` / `screen.duplication` 两条应为
  `capability: pointer_state_unverified`（旧副本写 `excludes_cursor`），`color.paths[]` 每行应带 `honorsExplicitPolicy`
  （旧副本没有这个键），`program.buildId` 也会不同。**旧副本"接受"了本文说会被拒的那一条，不等于要求被兑现**：
  它报的是当时那套已知的根据值，那一趟结果按未核实转述，并把差异报告给用户。重新构建并替换随包二进制属于改动
  发布产物，要用户决定，AI 不要自己动手构建、安装或覆盖。
- **屏幕那两条也是同一处理，而且换通道更不会有用**：`capture.monitor_changed`（那块屏不在桌面里了，或它的画面在确认之后变了）
  与 `capture.monitor_unverifiable`（复核身份的那一问没答案）都是 7、都停在换后端之前，绝不改截另一块屏；
  选屏阶段的 `match.monitor_id_unverifiable`（7，那一问整条没答案）与 `match.monitor_unknown_id`（4，这个标识此刻
  不在桌面上）是**两种结论**：一个是"没问出来"，一个是"没找到"，下一步都是重跑 `--screens`，不是换个编号碰运气。

### 确认框与诊断

- 框上写什么：默认焦点在"否"（回车不会误批）；列出目标及其屏幕区域、请求的通道**加实际走的那条内部路径**、
  展开后的绝对输出路径（或"标准输出"）、这一级会不会把别的窗口拍进图；桌面那一级还明确写着 `--yes` 对它不生效。
- 点"是"之后工具等约 1 秒才取帧——框的关闭动画还在 DWM 画面上时立刻截会拍到残影；框一定在第一帧之前就没了。
- 这是一个只有「是 / 否」两个按钮（`MB_YESNO`）、默认焦点落在**「否」**上的框：只有明确点「是」才算同意，
  **其余任何结果都算拒绝**，要拒绝就点「否」。**不要把 `X` 或 `Esc` 教成"拒绝"操作** —— `MB_YESNO` 下标题栏的 `X`
  虽然显示却被禁用，也没有可供 `Esc` 触发的 Cancel 按钮，两者都不是可靠的「否」；这个框终究得有人来答。
- 答"否" → `capture.access_denied` + 退出码 6、`stage=consent`。
  在 `--consent-timeout-ms` 之内没有人回答 → `capture.consent_timeout` + 退出码 6、`stage=consent`：
  **超时按"拒绝"处理，绝不当作同意**，之后剩下的采集同样停止。这段等待单独计时，不吃 `--timeout-ms` 的预算。
  期限是**轮询**到的（约 50 ms 一个切片）、不是抢占式的：到点之后给约 3 秒的关框宽限，所以框可能在数字之后
  还留一小会儿；不管框是没弹出来、发给它的关闭消息没送到、还是送到了没生效，到点都必须走一条**有界**的结束路径，
  线程与共享状态都不会被留在"还在等人"那一格（放弃收尾后弹框那条线程仍持有有效句柄，谁也不引用谁的栈）。
  **期限先到就 latch**：哪怕同一个切片里紧接着看见"是"，答案也是拒绝——没人应答就是没人同意，绝不读成"没人反对"。
  关框本身是一条升级链（实测某些构建上 `#32770` 会处理 `WM_CLOSE` 却不关框，于是下一切片给"否"按钮补一发点击）：
  这两步只改收场方式、不改答案，一律按拒绝收尾。点"是"之后那 1 秒缓冲算在人工那一级，既不烧自动预算，
  也不会被到点的超时削减（答应留的就留满）。
  框根本弹不出来 → `capture.consent_unavailable` + 退出码 6、`stage=consent`：**这不是人说了不**，
  下一步是换个有交互桌面的会话，而不是再问一遍。三条都带 `target`、`backend`（通道名）、`value`（实际那条路径名）。
- `capture.consent_stale`（退出码 7）说的是"人点头时看到的那份事实已经不在了"，两种发现处各带自己的 `stage`：
  确认这一关本身判出的（拓扑在框开着的时候变了、或要取的这一块根本不在人看过的清单里）是 `stage=consent`；
  批准之后目标又挪了位置或变了大小、要取样的矩形已经越出批准那一片时是 `stage=capture`。
  两种都一个像素不采、不签发凭证，也**不会**像"有人答否"那样把整批停住；下一步是重新选目标、重来一次请求，
  在新的快照上重新问人——**不是**换一条通道碰运气。
- **任何一次拒绝之后，这一次请求剩下的截图全部停止**：包括 `auto` 回退链里剩下的那几关，不只是剩下的 `--all` 目标；
  不换后端、不重试、不再弹第二遍，之前已经写好的图留着。同样"换后端没有意义"的还有身份那三条与色彩策略那两条
  （判据见 `src/FallbackChain.h` 的 `ClassifyChainStop`：`capture.access_denied` / `consent_unavailable` /
  `consent_timeout` / `consent_stale` / `target_gone` / `target_changed` / `target_unverifiable` /
  `hdr_refused` / `hdr_unverifiable`）——工具把这几条**原样**交出、绝不包装成 `capture.failed`，
  因为把它们混成一条就抹掉了调用方判断下一步所需的分别。
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
**还没开工的那一步被拒**，它那张图不写，按阶段给码：`match.timeout`（`stage=match`）/ `capture.timeout`
（等帧没来是 `stage=capture`，预算死在编码器里是同一个码配 `stage=encode`——没有 `encode.timeout` 这么一条码，
把两者分开的就是这个 stage）/ `io.timeout`（`stage=write` / `stdout`）。剩下的目标不再开始，已经写好的图留着
（同「部分成功」规矩）。省略或 `0` = 不设总预算，但即便如此，被隔离进辅助进程执行的那几步仍有内置 5000 ms 上限兜底；
**给了 `--timeout-ms` 时隔离调用拿到的就是剩余预算全额**，没有第二道内置上限压它。
**已经落地的图不会被一次超时抹掉**：原子写没有取消点，预算在提交过程中到期的那一张照样在盘上，写完返回之后再复核
一次期限，越线时**额外**记一条 `io.timeout`，而这张图仍在 `images` 里、`captured` 照旧计入，本次按部分成功报退出码
`7`（已交付 + 出错），只有"一张都没落地"才是 `8`。交付事实与期限合规是刻意分开的两件事，两头都不许折成另一头。

等人工确认**不计入**这份预算：`--consent-timeout-ms` 单独给确认框限时，超时按拒绝处理
（`capture.consent_timeout`，见上一节），绝不因为"没人反对"就当同意。

### 阻塞隔离（内部机制，不是公开选项）

- `printwindow`（这条通道本身，以及 `dwm` 回读的那次 `PrintWindow`）与**用了 `--title-regex` 或设了预算时的窗口匹配**，
  现在跑在一个隐藏的同 EXE 辅助进程里。期限到点，父进程只结束**它自己起的**那个辅助进程；
  目标应用的窗口从来不会被杀，也不会留下孤儿 worker（一个"关闭即结束"的作业对象 + 一次断管检查 + 辅助进程自己
  **按段**走的有界自退）。四类时钟各管一段，互不改写：父进程那一份 `--timeout-ms` 总预算（每步只拿"还剩多少"）；
  没有显式预算时那次隔离调用的内置 5000 ms；启动 / 握手段 30000 ms（父侧连管等待的上限，也压进剩余预算之内；
  子侧是"没人交任务就自己退出"）；辅助进程的执行段 = 随任务交下来的那笔剩余预算 + 1000 ms 交回宽限，
  应答交回之后不再挂任何自尽期限。**旧的固定 30 秒秒表已经删除**，所以用户明确接受的长预算不可能被辅助进程
  自己的时钟提前截短。另有两段不占预算的收尾宽限在父进程那侧：收尸等待约 2 秒、取消排干等待约 2 秒。
- 调用方要知道的：**没有公开的 `--worker` 入口**，它不能被用来绕开确认框，辅助进程永远不读桌面像素，
  辅助进程内部的退出码**不属于契约**（`0` 交回 / `60` 参数不合约定 / `61~68` 绑定校验第 N 条不成立 / `9` 管道或协议 /
  `10` 握手段内没人交任务 / `11` 超过交下来的预算 / `8` 由父进程收尸时写上）。
- **收尾边界和超时同等重要**（那条管道可能在不对的时刻回答）。连接时分三种情况判：辅助进程**已经连上**（那就
  没有挂起的 I/O 可取消）、写**同步完成**、以及返回 `ERROR_IO_PENDING`。`CancelIoEx` 只是**请求**取消，
  所以父进程会等到完成被观察到为止，并让每个 `OVERLAPPED` 和它的事件一直活到那一刻；完成迟迟不来时给辅助进程
  约 2 秒自己离开，之后才 `TerminateProcess`，而且这段宽限跑在请求预算**之外**。判决之后才到的那份回复
  **不会被读**：一张来晚了图既不能把已经报出的超时改写成成功，也不能当成图片交付出去。
- 调用方看到的分工：**预算耗尽把辅助进程中止**仍然是 `match.timeout` / `capture.timeout`（"这一步没在预算内跑完"），
  而**这套机制自己**跑不起来（辅助进程起不来 / 起来了却在握手段里没连上来 / 管道断了 / 消息对不上协议 / 任务不合法，
  文案是 `cap.worker.*`）统一以 `capture.worker_failed` + 退出码 7 报出来，带 `stage` / `backend`，`hint` 里附辅助进程
  最后那个退出码。"起来了没人交任务"这一条尤其要说清：它**不是** `capture.timeout`，加大 `--timeout-ms` 对一件根本没
  开始的事没有用。两条都不是"目标窗口拒绝对话"，也不该被读成受保护内容；`capture.worker_failed` 与 `capture.failed`
  分开给码，是因为下一步不同：这条要查的是这台机器的执行环境（权限、策略、杀软），而不是"目标是不是受保护"。
  一个形状上的分别照实记在这里：**窗口查询（`--list` / `--inspect`）保留条件求值那一步的原始码**
  （`match.timeout` / `cli.invalid_regex` / `capture.worker_failed`），而一次截图请求里"取帧之前那次匹配"的机制故障
  会被包成 `capture.failed` 交出（`message` 仍写明是辅助进程的问题）；取帧阶段（`printwindow` / `dwm` 回读）的机制
  故障照旧是 `capture.worker_failed`。
- `--title-regex` 的三种失败照实分开，**都发生在匹配期**（解析层不构造正则）：语法不合 → `cli.invalid_regex` + 1
  （`stage=match`、`backend=match`，弹框与取帧之前就判掉）；模式本身合法但撞到本机正则库的复杂度 / 回溯上限
  （`(a+)+$` 之类）→ **同一个码、同样 1**，换的是"太复杂"那条文案，其 `hint` 明说加大 `--timeout-ms` 没有用——
  改写模式，或者用 `--title-contains`；求值把预算花光 → `match.timeout` + 7（这才归预算管）。

### 诚实边界（这些是限制，不是保证）

- 预算在**可中断点**和"杀掉辅助进程"这两处生效。没有取消点的阻塞系统调用——原子写文件那几步、
  往堵住的标准输出管道里写、无视取消请求的 WinRT 编码器——是**开始前检查预算、结束后再计时**，
  不会在调用中途被抢占。
- **因此"给了 `--timeout-ms` 就绝不会卡"是过度承诺**，别这样向上层 AI / 脚本转述：`--timeout-ms` 不吃确认框那一段
  （不给 `--consent-timeout-ms` 时框可以一直等人回答），预算到点也追不进一次已经在跑的写或编码，只能等它返回后再判。
  调用方自己那一层超时 / 看门狗仍然要有；本工具能保证的是"到点之后不假装成功"，不是"任意时刻立刻返回"。
- 写到标准输出时"一个字节都没出去"与"半张图留在管道里"是分开的两种现场（后者的 `message` 给出已发字节数与总长），
  两种都**不算交付**、`images` 里都不许有它 —— 那条流已经脏了，调用方要按脏流处理。
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

`file` `bytes` `width` `height` `format` `source`（真正出图的那条通道）`path`（实际走的那条内部路径名）`scope`（`window` / `desktop`，由 `path` 算出）`rect`（`{"x","y","width","height"}`，那次授权允许采样的屏幕区域）`requestedRect` / `capturedRect` / `clipped` / `rotation`（只有从整幅桌面帧裁目标的通道会写，见下面那段）`hwnd`（`0x…` 字符串）`pid` `title` `class` `image`（映像文件名）`elapsedMs`；给了 `--roi` / `--client-area` 时再多 `cropMode` `cropRect` `fullWidth` `fullHeight`（以及图像原点核实得出来时的 `cropScreenRect`），见「窗口内部裁剪」一节；写过 `--scale` 时再多 `scaleMethod` `scaleApplied` `scaleFromWidth` `scaleFromHeight`（见「等比缩小」一节）；写过 `--cursor` 时再多 `cursorRequested` `cursorEffective` `cursorBasis`（见下面「光标那三个键」那段）；明确写过 `--hdr` 时再多 `hdrRequested` `hdrEffective` `hdrBasis` `sourceColorSpace` `sourceBitDepth`（整条省略时这组键一个都不出现，见下面「HDR 那一组键」那段）

**光标那三个键（写过 `--cursor` 才出现，`--quiet` 也不许藏）**：`cursorRequested` 是要求的那一种（`default` / `include` / `exclude`）；`cursorEffective` 是**这条路径实际**交回的那一种（`include` / `exclude` / `unverified`）；`cursorBasis` 说这个结论凭什么 —— `wgc_session_property_set`（按这次要求设过、再把读回来的值核对过）、`wgc_session_property_read`（没设过，只读当前值，即 `--cursor default`）、`path_excludes_cursor`（这条路径的来源像素里没有光标）、`path_pointer_state_unverified`（桌面复制那一帧：指针状态证明不了，见上面 `--cursor` 那一条）、`wgc_cursor_property_unavailable`（那一问没答案）、`path_capability_not_registered`（这条路径没进光标登记表，按严格处理而不是猜一个）；后三种与 `pointer_state_unverified` 那一类一样，`cursorEffective` 写的都是 `unverified`，不折成"画"或"不画"任何一边。三个键各说一件事，谁也不冒充谁：`effective` 说不到"这一张图里看得见或看不见指针"那一层（本 SDK 的会话接口没有 `IsCursorVisible` 那个只读属性，像素级的事本工具一条都不声称，而 `--capabilities` 把这条边界写成 `cursor_effective_is_a_setting_not_a_pixel_check`）。没写 `--cursor` 时三个键一个都不出现（那才是"默认不要求"与从前逐字节相同的保证）。

**HDR 那一组键（明确写过 `--hdr` 才出现，`--quiet` 也不许藏）**：`hdrRequested` 是要求的策略（`auto` / `tonemap` / `refuse`）；`hdrEffective` 是**这一帧实际**经历的处理（`sdr_passthrough` = 来源被**核实**是 8 位 SDR、没做也不需要映射；`tone_mapped` = 来源是 HDR、已按固定的浮点曲线映射成 8 位 sRGB；`unverified` = 这一问没答案 —— 既包括带回一个认不出的广色域格式，也包括按 8 位交付却从没问过那块屏此刻是不是 HDR，两种都不折成"映射过了"也不折成"就是 SDR"）；`hdrBasis` 说这个结论凭什么（`delivered_bgra8_sdr` / `scrgb_float_tone_mapped` / `pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `path_sdr_source` / `format_unrecognized` / `transfer_function_unknown` / `bgra8_source_unverified` / `tone_map_not_applied`）；`sourceColorSpace` 是编码之前那份来源（`srgb_bgra8` / `scrgb_float` / `pq_bt2020` / `hlg_bt2020` / `rgb10a2_unverified` / `unknown`）；`sourceBitDepth` 是来源每通道位数（`8` / `10` / `16`，来源认不出时整个键不出现，绝不写 0）。**省略 `--hdr` 与明确写 `--hdr auto` 是两种上报**：前者这组键一个都不出现（与这条选项存在之前逐字节相同），后者取图行为一样而键照常出现，一张 8 位的 `wgc` / `screen.wgc` 帧读作 `unverified` + `bgra8_source_unverified`；`-v` 的 `input.hdrGiven` 分得开这两态。**像素布局不是色彩空间**：10 位打包帧登记为 `rgb10a2_unverified`，不会被默认按 PQ / HLG 解，也不会被折成 SDR（那种格式在本构建里是 `capture.hdr_unverifiable`）；`pq_bt2020_tone_mapped` / `hlg_bt2020_tone_mapped` / `transfer_function_unknown` 是枚举里成立而**真机截图走不到**的取值（这条构建里实际会映射的只有 FP16 scRGB 那一幅帧池），别把它们读成"本机有一块 PQ/HLG 面板被映射过"。明确要过处理（`tonemap` / `refuse`）而这一张是按 8 位交付时图照常交付，提示发哪一条取决于取图之前问到没有：问到且答的是 SDR → `note.hdr_source_sdr`；那一问没答案 → `note.hdr_source_unverified`（"按 8 位交付，但来源没被确认是 SDR"，它不是"这帧没有 HDR"的证据）；`--hdr auto` 这两条都不发，只靠上面那组被动字段。`hdrEffective: "tone_mapped"` 只说这台机器过了那条映射链路，**不**说色彩正确性被验过（本项目无 HDR 屏，`--capabilities` 的 `color.verifiedOnThisMachine` 恒 `no`）。

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

`file` `bytes` `width` `height` `format` `source`（同上）`path`（`screen.wgc` / `screen.bitblt` / `screen.duplication`，三条都是桌面）`scope`（`desktop`）`rect`（那次授权允许采样的屏幕区域，= 那块屏的矩形）`requestedRect` / `capturedRect` / `rotation`（`duplication` 这条会写，见上面那段；整屏本该 `capturedRect` 等于 `rect`，裁不全就直接报 `capture.monitor_changed` 而不是交一张偏小的图）`monitor`（编号）`device`（`\DISPLAY1` 之类）`primary`（布尔）`elapsedMs`；写过 `--cursor` 时同样多 `cursorRequested` / `cursorEffective` / `cursorBasis`；明确写过 `--hdr` 时同样多 `hdrRequested` / `hdrEffective` / `hdrBasis` / `sourceColorSpace` / `sourceBitDepth`（屏幕目标走 `screen.wgc` / `screen.duplication` 时也可能带回广色域帧，同窗口目标一套键；整条省略时这组键不出现）

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
| `stage` | 哪一步：`parse` / `match` / `plan` / `consent` / `capture` / `encode` / `write` / `stdout` / `report`。`match` = 目标匹配求值这一步（`match.timeout`，以及 `cli.invalid_regex` —— 语法与复杂度两种下场都在这里判，解析层不构造正则），`consent` = 人工确认这一关（答"否"、弹不出、`--consent-timeout-ms` 内没人答），`capture` 里也可能出"批了之后目标挪了位置"（`capture.consent_stale`） |
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
| 1 | 参数错（也含 `match.index_out_of_range` / `match.monitor_out_of_range` / `match.roi_out_of_range` 这些「编号或裁剪矩形对不上实际命中的目标」的越界用法）；也含 `--cursor include` 配那条结构上做不到的通道 `capture.cursor_unsupported`；也含 `--hdr tonemap`/`refuse` 配本构建兑现不了这次色彩要求的那条通道 `capture.hdr_unsupported` |
| 2 | 未给条件（输出文本帮助） |
| 3 | `--help` |
| 4 | 无匹配窗口 |
| 5 | 匹配多个窗口 |
| 6 | 这次截图没拿到人的同意：人在这个只有「是 / 否」的确认框上答了"否"（`capture.access_denied` —— 人能给出的唯一拒绝就是点"否"，而工具把任何非"是"的结果都当拒绝），那个会话根本没有可交互的桌面、框弹不出来（`capture.consent_unavailable`），或在 `--consent-timeout-ms` 之内没有人回答（`capture.consent_timeout`）；也包括目标受保护 |
| 7 | 截图失败（含 `--timeout-ms` 预算耗尽的 `match.timeout` / `capture.timeout`，含身份复核没过的 `capture.target_gone` / `capture.target_changed` / `capture.target_unverifiable`，含屏幕那三条 `capture.monitor_changed` / `capture.monitor_unverifiable` / `match.monitor_id_unverifiable`，也含本工具辅助进程的机制故障 `capture.worker_failed`），也含交付图像放不下请求的裁剪 `capture.roi_invalid` / 定位裁剪所需那一问答不出 `capture.roi_unmeasurable`；也含要求的光标状态核实不了 `capture.cursor_unverifiable` 与本机给不了那个开关 `env.cursor_unsupported`；也含 `--hdr refuse` 核实来源是 HDR `capture.hdr_refused`、带回认不出的广色域格式 `capture.hdr_unverifiable`，以及 `auto` 链被色彩要求筛到空 `env.hdr_unsupported`（都不落地） |
| 8 | 写文件失败（也含结果 JSON 没送到约定那条流，以及**那一步根本没开工**的 `io.timeout`）。注意：图**已经落地**、只是写完之后的期限复核越了线时，那张图仍留在 `images` 里、按部分成功报 `7`（已交付 + 出错），不是 `8` —— 见《期限与阻塞隔离》 |
| 9 | 内部异常 |

退出码与 body 是两套独立信号：先看 `errors`，再看 `captured`，最后才用退出码做粗分支。
只读查询那两条（`--capabilities` / `--diagnostics`）只用 `0` 与 `1`：`0` = 文档出完了（里面写"这台机器哪条都不行"也算成功，环境要看 `status` 而不是
退出码），`1` = `cli.query_conflict`。它们不产生 `4`/`5`/`6`/`7`/`8`，因为一次窗口都没枚举、一个框都没弹、
一个文件都没写。
窗口查询那两条（`--list` / `--inspect`）用 `0`/`1`/`4`/`5`/`7`/`9`：`1` = 用法不合契约（`cli.window_query_conflict`、
条件本身写坏、`--index` / `--monitor` 越界），也含 `--title-regex` 在**匹配期**被判出的语法不合与复杂度上限
（`cli.invalid_regex`，退出码仍是 `1`，只是带 `stage=match`）；`4` 与 `5` 在 `--inspect` 那条入口上是"定不出唯一目标"
（没命中 / 命中多扇），`match.monitor_unknown_id` 也归 `4`，而 `--list` 命中 0 条是正常答复、给 `0`；
`7` 是"这一问没能问完"（`match.timeout` / `capture.worker_failed` / `capture.failed` / `match.monitor_id_unverifiable`），
不是"截图失败"。同样**不会出现 `6` 与 `8`**：不弹框、不落地，那两条说的就是那两段的事。

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

**`cli.*`（用法族，除注明的两条外退出码都是 1）**：除 `cli.invalid_regex` 之外都在解析期发出；那一条的名字保留
`cli.` 前缀，但**发出点已在匹配执行层**（语法与复杂度都在匹配期判，退出码仍是 1，带 `stage=match`、`backend=match`）；
`cli.no_condition` 是文本帮助 + `2`。
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
`cli.invalid_regex`（1，`stage=match`、`backend=match`；**这条码只有匹配期这一处来源**——解析层不构造正则，枚举窗口、
弹框、取帧之前就判掉，一个像素都不取）。两种下场共用这一条码与这个退出码，靠 `message` 分：
**语法不合**（本机正则库编译不过这条模式，`hint` 里带 `regex_error code=N …` 那句原话）→ 改写模式；
**复杂度 / 资源上限**（模式合法但 `(a+)+$` 之类在长标题上撞到回溯上限）→ **加大 `--timeout-ms` 没有用**，
这是有界的资源停止而不是"慢但还能用"，改写模式或换 `--title-contains`。
求值把预算花光是另一条码：`match.timeout` + 7；跑这一问的辅助进程自己没回来也是另一条：`capture.worker_failed` + 7

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
`capture.access_denied`（6，确认框上的回答不是"是"——这个只有「是 / 否」两个按钮的框上，人能给出的唯一拒绝就是点"否"，
标题栏的 `X` 被禁用、也没有供 `Esc` 触发的 Cancel 按钮，所以别把那两个当成拒绝操作；窗口内容路径没带 `--yes` 时也要弹，
所以这一条不再只代表整屏。
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
起不来 / **起来了却在握手段里没连上来** / 管道断 / 协议不符 / 任务不合法，`hint` 里附辅助进程最后那个退出码。
"没人交任务"那一条不是 `capture.timeout`：加大 `--timeout-ms` 对一件根本没开始的事没有用。一次窗口查询里这一条码
原样保留；一次截图请求里"取帧之前那次匹配"的机制故障会被包成 `capture.failed`（`message` 仍写明是辅助进程的问题），
取帧阶段（`printwindow` / `dwm` 回读）的机制故障照旧是 `capture.worker_failed`）
`capture.timeout`（7，`--timeout-ms` 在取帧或编码阶段耗尽：等帧没来是 `stage=capture`，预算死在编码器里是同一个码配
`stage=encode`（没有 `encode.timeout` 这条码，分开两者的就是这个 stage）：`printwindow`/`dwm` 多半是目标
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
`capture.cursor_unsupported`（**1**，要求的光标状态由这条路径兑现不了：`--cursor include` 配 `printwindow` / `dwm` / `bitblt`（来源里根本没有指针可加），或 `include` **与** `exclude` 两样配 `duplication`（那条桌面帧的指针状态证明不了，两头都保证不了），以及整屏目标上配那几条没有开关的路径。解析期给出，`option`=`--cursor`、`value`=规范化取值、`message` 带实际通道与做得到的那一条（`wgc`），两种原因各有一种措辞。**下一步是改用 `--capture wgc` 或去掉这条要求，而不是换一条会读桌面的通道试试**——工具自己绝不会换）
`capture.cursor_unverifiable`（7，明确要求过的光标状态在 `wgc` 那一次核实不了：那个开关取不到接口、设不下去，或设完读回来是相反的那一件。`message` 给要求的状态 + ASCII 原因名（`interface_unavailable` / `set_failed` / `read_failed` / `read_back_mismatch`）与实际读回的值；接口取不到时那一条就是 `E_NOINTERFACE`（这一问的下场确实是它），不拿 `E_FAIL` 顶。发生在 `StartCapture` **之前**，所以与要求相反的那一张根本不会交出去，一个像素都不落地。`--cursor default` 遇到同一情况不当失败，而是在结果里写 `cursorEffective: "unverified"`
`capture.hdr_unsupported`（**1**，显式点名的那条通道兑现不了要求的 HDR 处理：`--hdr tonemap` / `refuse` 配 `printwindow` / `dwm` / `bitblt`（结构上只有 8 位 SDR），或配 `duplication`（本构建走 `DuplicateOutput()`、采集之前不问色彩空间，所以不敢声称兑现）。解析期给出，`option`=`--hdr`、`value`=规范化取值、`message` 带实际通道与本构建兑现得了的那两条（`wgc`、`screen.wgc`）。**下一步是改用 `--capture wgc` 或去掉这条要求，而不是换一条会读桌面的通道试试**——工具自己绝不会换；`--capture auto` 时同样的判据先筛链，被摘的每条留 `note.hdr_channel_skipped`）
`capture.hdr_refused`（**7**，`--hdr refuse` 且这条路径核实回来的来源确是 HDR 帧（FP16 scRGB 或 10 位 PQ/HLG，`message` 给那个来源色彩空间名）。这正是用户要的"别把 HDR 硬压成发白图"，所以一个像素都不落地。它是**用户策略的结论**而不是"这条通道不行"，所以 `auto` 回退链到这里立刻停下，**绝不换一条后端重跑**（换一条只交 8 位的后端再出一张，等于把拒绝偷换成一次静默降级）。下一步是改用 `--hdr tonemap` 拿那张映射后的 SDR，或明确去掉这条要求再问一次——不是换后端，也不是"那就整窗交出"的理由）
`capture.hdr_unverifiable`（**7**，这条路径带回一个本构建认不出的广色域像素格式（`message` 给那个 DXGI 格式编号）。认不出格式不等于硬按 BGRA8 解释（那正是发白图的成因），也不等于猜一个映射，所以这一张不落地，而回退链同样**到此停下、不换后端重跑**。下一步是改用 `--hdr tonemap` / `auto` 重新决定，反复出现请把那个格式编号原样报给工具维护者）
`capture.roi_invalid`（7，交付的整窗图像比请求的裁剪矩形小：目标在选定之后改了尺寸、或被屏幕边缘裁短。这一张一个像素都不落地，既不往里挪，也不退回整窗交出；`message` 给图像实际尺寸与请求矩形的右下边 + ASCII 原因名）
`capture.roi_unmeasurable`（7，定位这块裁剪矩形所需要的那一问没有答案：客户区量不出来 （`client_unmeasurable`），或这块交付图像核实不出它对应屏幕上哪一块（`image_unmeasurable`）。与「放不下」分开给码：下一步是换一条窗口内容通道或整窗重取，而不是把请求往里挪挪）
裁剪或缩放"判据说过得去却实际做不出来"（帧在内存里的形状自相矛盾）**没有专门的码**：`errors[].code` 就是下面那条
`capture.frame_invalid`（7，`stage=capture`、`option`=`--capture`、`value`/`backend`=那条通道），只有 `message`
文案区分是裁剪那次还是缩放那次（`cap.crop_apply_failed` / `cap.scale_apply_failed` 是**文案键**，不是 code，
`--lang` 会翻它，所以绝不能拿它们分支）。这一张不落地，下一步同 `capture.frame_invalid`。
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
`env.hdr_unsupported`（7，**这台机器给不出这一次要求的 HDR 处理**：过了版本与光标闸门之后，链里再没有一条真兑现得了 `tonemap` / `refuse`。`message` 给要求的策略、本来要试的那几条与 ASCII 原因一览，`option`=`--hdr`、`value`=规范化取值、`stage=capture`。在枚举目标、确认框、输出名规划之前给出，一个像素都不取、不弹框、不写文件。下一步是 `--capabilities` 看 `color.paths` 里每条的 `honorsExplicitPolicy`，或去掉这条要求；**换 `--capture` 变不出一条兑现得了的通道**，重试同一个目标也是同样的下场。它与 `env.channel_unsupported` 分开：那条说"这条通道在本机版本上不可用"，这条说"这条通道可用，但它兑现不了这次的色彩要求"）
判据、三条下限与实测范围见上面「运行环境与能力检查」一节。

**`io.*`**
`io.write_failed`（8，临时文件建不出来 / 写或刷新中断 / 提交为目标名失败；写标准输出时分两种现场，半张图留在管道里时 `message` 给已发字节数与总长，两种都**不算交付**、`images` 里没有它）`io.file_exists`（8，配合 `--no-overwrite`）`io.output_collision`（8，整批输出名撞车，一张都没截也没写）`io.timeout`（`stage=write` / `stdout`：①那一步**根本没开工**就发现预算已尽 → 这一张不写，一张都没落地时退出码 **8**；②写已经完成、只是**写完之后的期限复核**越了线 → 额外记这一条 `io.timeout`，而图仍在 `images` 里、`captured` 照计，本次按部分成功报 **7**（已交付 + 出错）。两头是刻意分开的：别对一个盘上存在的文件说"没写出来"，也别把越线那次超时藏起来）

**`note.*`（不是错误，`--quiet` 会去掉）**
`note.dry_run` `note.capture_channel`（`auto` 回退后实际用了哪条）`note.duplicate_value`
`note.extension_appended` `note.exe_path_looks_like_name`（`--exe` 传的像文件名不像完整路径）
`note.format_extension_mismatch` `note.format_defaulted_png` `note.output_defaulted_stdout`
`note.output_extension_appended` `note.quality_ignored` `note.all_without_placeholder`
`note.flag_overrides_quiet` `note.pipe_default_format` `note.json_flag_deprecated` `note.frame_uniform`（这一张整幅只有一个颜色：质量提示，图片照常交付）`note.crop_mapping_unavailable`（已按请求裁好，但这张交付图像核实不出它对应屏幕上哪一块，所以少了 `cropScreenRect` 那一行：`cropRect` 仍是图像自己的像素坐标，别拿它当桌面坐标用）`note.capture_clipped`（目标没被完整截下来：`message` 给"要截多大 / 只截到多大"，`hint` 给四边各少了几像素。图照常交付、退出码不变，配 `capturedRect` / `clipped` 一起看）
`note.cursor_channel_skipped`（`auto` 链里那一条**做不到这次要求的光标状态**、已从链中去掉：`message` 给通道名、要求的状态与一个 ASCII 原因 token（`os_below_min_build:19041` / `window_self_drawn` / `dwm_redirection_surface` / `screen_dc_has_no_pointer` / `desktop_frame_pointer_state_unverified`（include 那一路摘掉 duplication）/ `duplication_cursor_exclusion_unprovable`（exclude 那一路摘掉 duplication）/ `not_registered`），`backend` 是被摘掉的那条，剩下的仍按顺序试。与 `note.channel_unavailable` 分开：那条说的是"本机版本用不了这条通道"，这条说的是"这条通道能用，但它兑现不了这次的光标要求"
`note.hdr_channel_skipped`（`auto` 链里那一条**兑现不了这次要求的 HDR 处理**、已从链中去掉，剩下的照原顺序继续试：`message` 给通道名、要求的策略与一个 ASCII 原因 token（`window_self_drawn_8bit` / `dwm_redirection_surface_8bit` / `screen_dc_8bit` / `duplication_hdr_policy_not_implemented` / `not_registered`），`option`=`--hdr`、`value`=规范化取值、`backend`=被摘掉的那条，`stage=capture`。与 `note.channel_unavailable` 分开：那条说"本机版本用不了这条通道"，这条说"这条通道能用，但它兑现不了这次的色彩要求"）
`note.hdr_source_sdr`（写过 `--hdr tonemap` 或 `--hdr refuse`，而这一帧的来源核实是 8 位 SDR：那条处理是恒等的、没有改变任何一个像素。图照常交付、退出码不变，这条只是把"我要过 HDR 处理"与"其实这一帧没有 HDR"分开放在调用方眼前，免得把一次静默通过当成"HDR 已被正确映射"。**这一条只由采集之前真的问到那块屏此刻是 SDR 来支撑**；`--hdr auto` 不发这条；`--quiet` 连同整段 notes 一起去掉）
`note.hdr_source_unverified`（上面那条的替代：明确要过 HDR 处理、这一张是按 8 位交付的，而"这块屏此刻是不是 HDR"那一问**没有答案**。图照常交付、退出码不变，但它不是"这帧没有 HDR"的证据——合成器可能把一幅 HDR 画面压进一个 8 位帧池，所以一张 8 位帧本身证明不了来源是 SDR。下一步是查那块屏，或改用 `--capture wgc` 再来一次；绝不许把"没核实"读成"HDR 已经映射好了"）
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
会照实报错，不会截半张。`--scale` 排在这道检查**之后**，所以它不是"把超限的帧改小一点就能过"的路径：
过不了形状检查的那一帧根本到不了缩放这一步。

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
  **`--monitor id:` 那条反斜杠会被吃掉**：`\\?\DISPLAY#…` 传进去成了 `\?\DISPLAY#…`，而这条标识是逐字符比的，
  于是工具如实报 `match.monitor_unknown_id`+4 —— 看起来像"那块屏不在了"，实际是转义把标识改坏了。
  先 `export MSYS2_ARG_CONV_EXCL='*'`、用**单引号**原样包住从 `--screens` 抄回的那一条，并且拿 `-v` 回显的
  `input.monitor` 核对工具真正收到的字符串，再下"屏不存在"的结论。
- **cmd.exe**：同一个值用双引号包住就原样送达（`--monitor "id:\\?\DISPLAY#…#{…}"`），里面的 `&`、`#`、`{}` 都不会被
  当成命令解析；不带引号的 `&` 会当场断成两条命令。
- **PowerShell**：单引号字符串里的反斜杠是字面量，`'id:\\?\DISPLAY#…'` 原样送达；`--monitor` 的取值整体加引号即可。
- **图片字节走 stdout 时的保真度**（`--out -` 出的是**二进制 PNG 字节**，整份 JSON 同时在 stderr，两条流不混）：
  `cmd` 与 PowerShell **7.4+** 逐字节无损；PowerShell **7.0–7.3** 会把 stdout 按文本解码；**Windows PowerShell 5.1 会弄坏**
  （字节先按文本解码再以 UTF-16LE 写出，NUL 与所有 ≥ 0x80 的字节在落文件之前就没了，连 `2>` 那份 stderr 文件也要套上
  它自己的错误记录格式）。`2>&1` 与 `*>` 在任何一种 shell 里都不是答案：一合并 shell 就当字符串处理，图片字节必坏。
  所以：要么直接 `--out <绝对路径>`（JSON 就在 stdout，stderr 为空），要么 `cmd /c` 包一层，要么
  `Start-Process -RedirectStandardOutput … -RedirectStandardError …`（句柄由系统接上，逐字节无损，退出码要从
  `-PassThru` 那个对象上读，只加 `-Wait` 不会把退出码回报出来）。
  5.1 下 `2>&1` 还会把原生 stderr 包成错误记录文字，"JSON 走 stderr"那种调用因此解析不出 JSON。
  想复核"是不是 shell 把字节改了"不必真截图：重定向一段固定字节流（例如 `ECAPTURE.EXE --version`）再比对即可。
  ```cmd
  :: cmd.exe：两条流分开重定向，逐字节无损
  D:\tools\ECAPTURE.EXE --process notepad.exe --yes --out - 1> D:\shots\snap.png 2> D:\shots\result.json
  ```
- 输出目录必须**已存在**，工具不建目录；路径不存在时报 `io.write_failed`+8，那不是"目标截不到"，先去建目录或改名。
- 控制台代码页不是 65001 时中文照样正常（工具直接写 UTF-8 字节 + CRLF），但**别用 `Write-Host` 之外的
  管道去二次编码**。
- 这个工具是 Windows 原生 exe：换到别的操作系统上跑不了，`wine` / 跨平台脚本里的探测结果不能当作 Windows 上的验收。

## 常用配方

下面一律写成 `ECAPTURE.EXE ...` 只是为了一行看得下：**真调用时给绝对路径**（见 `SKILL.md`「Where the
executable is」），别假设 `ECAPTURE.EXE` 在 `PATH` 里，也别在交互输入时使用 `$PSScriptRoot`（它只在 `.ps1`
脚本文件里才有值）。

```powershell
# 0) 先只读问一次这台机器能走哪几条：不截图、不弹框、不写文件，也不需要窗口条件。
#    用它选 --capture、判断"这次失败该换通道还是这台机器不行"、以及确认这里弹框有没有人会答。
ECAPTURE.EXE --capabilities
ECAPTURE.EXE --diagnostics        # 要提交问题报告时用这份（构建标识 + 平台 + 后端状态）

# 0b) 要动哪一块屏？先列一次，然后按标识点名（别猜编号）。
#     交回的 selectors 就是能直接写回 --monitor 的那两条字符串。
ECAPTURE.EXE --screens
ECAPTURE.EXE --monitor device:DISPLAY1 --out D:\shots\m1.png     # 本次桌面连接的设备名
#     跨会话那条要**原样抄回**并整体加引号：'id:\\?\DISPLAY#GSM41A2#5&…#{…}'（Git Bash 先
#     export MSYS2_ARG_CONV_EXCL='*'，否则反斜杠被吃掉会变成 match.monitor_unknown_id+4，看着像"屏不在了"）
ECAPTURE.EXE --monitor 'id:\\?\DISPLAY#GSM41A2#5&…#{…}' --dry-run
#     整屏拍的是桌面像素：一定弹确认框，--yes 跳不过。--dry-run 不取帧也不弹框，只把这次的理解写出来。

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

# 3) 从 --list 的 windows[].hwnd 里点名回来（--hwnd 已经把目标钉死成一条；这条没给 --yes，
#    所以是窗口内容那一级也要问一次的现场——要人不被打扰就带 --yes）
ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite --out D:\shots\one.png

# 4) 每个命中窗口各一张（授权不跨请求缓存，所以每次调用都要重新带上 --yes；一张一个名字）
ECAPTURE.EXE --pid 12345 --title-contains 报告 --all --yes --out "D:\shots\rpt_%i.png"

# 5) 图片进管道（JSON 于是在 stderr；stdout 一次只一张，多个目标请写到文件）
#    这条只在 cmd 与 PowerShell 7.4+ 里无损；5.1 与 7.0-7.3 会把字节按文本重编码（见「各 shell 的坑」），
#    那种环境要么直接 --out <文件>，要么 cmd /c 包一层 / 用 Start-Process -RedirectStandardOutput。
#    两条流要分开接，别用 2>&1 合并：
ECAPTURE.EXE --process notepad.exe --yes --out - 1> D:\shots\snap.png 2> D:\shots\result.json

# 6) 整屏 = 桌面像素：一定要人本人点"是"，--yes / --quiet / 环境变量都跳不过。
#    先把"会拍到什么范围"说清楚再启动，然后等用户点；被拒就是 capture.access_denied（+ stage=consent），
#    与有没有给 --out 无关（弹不出框则是 capture.consent_unavailable）
ECAPTURE.EXE --monitor primary --out D:\shots\screen.png
ECAPTURE.EXE --monitor all --out "D:\shots\screen_%i.png"

# 7) 按屏过滤窗口（出的是窗口图字段，走窗口那一级：带 --yes 才不弹框，不给照样问一次）
#    编号只是**本次枚举顺序里的位置**，跨调用要用 --screens 交回的标识点名
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

# 9b) 光标：唯一那条能设进去也读回来的开关在 wgc / screen.wgc（要 build 19041）
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --cursor include --yes --out D:\shots\c.png
#     duplication 那两条登记为 pointer_state_unverified：include 与 exclude **两样**都在解析期报
#     capture.cursor_unsupported+1（不换后端、也走不到弹框）。下面这条是"照着办会被拒"的对照，不是推荐用法：
ECAPTURE.EXE --hwnd 0x001A0B4C --capture duplication --cursor exclude --out D:\shots\x.png
#     不要求改动（--cursor default）时那张桌面复制的图照旧交付，只是 cursorEffective 写 unverified

# 9c) 色彩：省略 --hdr 与明确 --hdr auto 的分别是"色彩那组键出现不出现"，用 -v 的 input.hdrGiven 对照
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --out D:\shots\h.png -v      # hdrGiven: false，没有色彩键
ECAPTURE.EXE --hwnd 0x001A0B4C --capture wgc --yes --hdr auto --out D:\shots\h2.png -v   # 键出现，8 位帧读 unverified

# 10) 给英文环境的人看诊断文字
ECAPTURE.EXE --process notepad.exe --yes --out D:\shots\a.png --lang en

# 11) 脚本里要有期限意识：自动阶段一份总预算，耗尽给 match/capture/io.timeout（还没开工的图不写、好图留着；
#     已经落地的图不会因为超时而消失，那时是"已交付 + 出错"的 7）。**这条预算不等于"绝不卡住"**：
#     确认框不吃它，人工那一级要单独用 --consent-timeout-ms（到点算拒绝，capture.consent_timeout，退出码 6），
#     调用方自己那层超时仍然要留着。
ECAPTURE.EXE --process notepad.exe --yes --timeout-ms 5000 --out D:\shots\epad.png
ECAPTURE.EXE --monitor primary --consent-timeout-ms 60000 --out D:\shots\screen.png
```
