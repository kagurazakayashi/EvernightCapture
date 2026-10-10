# MEMORY.md — EvernightCapture 会话交接

本文件记录当前进度与证据。长期约束看 `AGENTS.md`，看真实状态以仓库实际文件与 `git log` 为准。

## 当前步骤与状态

步骤：**划分跨平台核心与平台实现边界**（架构基础，单步交付）。

- 状态：代码已实现；**Linux 离线验证通过**；**Windows 编译未验证**（见《环境限制》）。
- 本步**没有**实现任何 Linux 截图能力，也**没有**改动任何对外行为：CLI 参数、配置优先级、
  JSON 键与取值、错误码、退出码、`--capabilities` 契约全部不动（新增的判据只在测试里）。
- 建议正式提交标题：`划分跨平台核心与平台实现边界`（由用户决定提交，AI 不自动提交）。

## 实际目录与接口（本步落地的那一份）

新增目录：

- `src/core/` —— 平台无关模型与判据，**一条都不 include `windows.h`**：
  | 文件 | 承载 | 关键点 |
  | ---- | ---- | ------ |
  | `Platform.h` | `PlatformKind`（windows / linux-x11 / linux-wayland / unknown）、`BuildPlatform()`、`PlatformName()` | 全仓**第一处**按平台条件编译（只在 `BuildPlatform()` 内）；非 Windows 构建如实返回 `kUnknown`，不猜 X11 |
  | `Geometry.h` | `PhysRect`（四条 `int32_t` 边）、`MaybePhysRect`（`known` + 值）、`RectIsEmpty` / `RectContains` / `RectWidth` / `RectHeight` / `RectOverlaps` | 判据从 `Consent.cpp` 与 `WindowMatch.cpp` 搬来，**语义逐字保持**：空矩形不是特例；零宽窗口矩形落在屏内仍算命中（改了会让同一条命令行忽然找不到窗口） |
  | `Status.h` | `BlockedStatus`（从 `CaptureCommon.h` 搬出） | 条件求值与取帧共用同一份线上词表；枚举名带 Windows 通道痕迹，本步**不重命名**（改名等于同时动管道协议与四语判据表） |
  | `Frame.h` | `CapturedFrame` + `kFrameMaxSide` / `kFrameMaxBytes` + `RenderOutcome`（从 `CaptureCommon.h` 搬出） | 帧不挂任何句柄；`NativeErrorCode` 承载 win32 / hresult，默认 0 = 不适用；未问过的读数保持"没有答案"那一档 |
  | `Error.h` | `NativeErrorCode`（`uint32_t`）、`kNativeErrorNone`、`Tri`（从 `EnvReport.h` 搬出） | 字段名负责说明来源（win32 / hresult）；**不建**平行的"来源枚举"与第二份 ASCII 词表 |
  | `Target.h` | `TargetDomain`、`NativeIdForm`、`SessionRef`、`TargetRef`、`SameTargetAs()`（三值） | Wayland 的 toplevel 句柄用**字符串**形状表达，不压成假整数；跨平台 / 跨会话 / 无标识 → `kUnknown`，不回 `kNo` |
  | `Authorization.h` | `PixelScope`（从 `CaptureScope.h` 搬出）、`YesGivenExempts()`、`GrantLayer`、`RequiredGrantLayers()`、`HasAllGrantLayers()` | `--yes` 只豁免窗口内容路径这一条现在**只有一个出处**；Wayland 两层都要（程序确认 + 系统 Portal 授权），谁也不替代谁 |
  | `Capability.h` | `MatrixStatus`、`MatrixFeature`、`MatrixCell`、`PlatformMatrixCells()`、`MatrixStatusOf()`、`MatrixIsCompiled()` | 3 平台族 × 8 项功能 = 24 格满表；`kUnknown` 在表里是缺陷值（测试钉住）；非 Windows 列一律不许声称已实现 |

- `src/platform/windows/WinBridge.h` —— 核心 ↔ Win32 的唯一转换处：`ToNativeRect` / `FromNativeRect` /
  `ToNativeRects` / `FromNativeRects`（逐字段赋值，不做 memcpy、不做别名），以及从 `Deadline.h` 搬来的
  `WaitTimeout()`（`INFINITE` / `DWORD` 换算不再污染共用的截止时间头）。

改造后**已经平台无关**的既有头（本步去掉 `windows.h`，Linux 侧编得过）：
`Capture.h`、`CaptureScope.h`、`Consent.h`、`CropGeometry.h`、`CursorControl.h`、`Deadline.h`、`Delivery.h`、
`DupGeometry.h`、`EnvReport.h`、`FileSave.h`、`HdrColor.h`、`HistoryArchive.h`、`ImageOps.h`、`Json.h`、
`Notification.h`、`OutputPlan.h`、`ProcessIntegrity.h`、`Report.h`、`ScreenIdentity.h`、`ScreenMatch.h`、
`ScreenQuery.h`、`SystemCompat.h`、`WindowIdentity.h`、`WindowMatch.h`、`WindowQuery.h`、`WgcGeometry.h`、
`CliOptions.h`、`Config.h`。

仍属【平台层 · Windows】的文件在开头加了 `【平台层` 归属标记（14 个：`CaptureCommon.h`、五条通道头、
`D3dDevice.h`、`WinrtApartment.h`、`Worker.h`、`WorkerIo.h`、`WorkerProtocol.h`、`WorkerTiming.h`、
`NotificationToast.cpp`、`Lang.cpp`、`SystemCompat.cpp` 等），`scripts/core-portability-check.sh` 第 2 关会列缺标记的文件。

类型替换清单（共用结构里的平台类型 → 中立承载）：
`RECT` → `PhysRect`（`CapturedFrame.requestedRect/capturedRect`、`CapturedImage.rect/requestedRect/capturedRect/cropScreen`、
`ScreenInfo.bounds`、`GateTarget.area`、`DesktopPermit.approved_`、`DeliveredImage.screenRect`、
`ClientAreaProbe.screenRect`、`CropResolution.cropScreen`、`MatchRequest.onScreens`、
`DupCropPlan.src/captured/requested`、`DupOutputInfo.desktopRect`）；
`DWORD win32` / `HRESULT hresult` / `HRESULT lastError` → `NativeErrorCode`
（`RenderOutcome`、`ConsentReply`、`DialogResult`、`NotifyOutcome`）；
`UINT flags` → `uint32_t`、`DWORD threadId` → `uint32_t`（`ShowConsentDialog`、`DialogDriver::RequestClose`）。

## 明确保留的过渡桥接（下一步要拆的东西）

1. `WorkerProtocol.h` / `Worker.h`：辅助进程管道上的 `Task.onScreens` 仍是 `std::vector<RECT>`、
   `Reply.win32/hresult` 仍是 `DWORD/HRESULT`。改它要同步 `kProtocolVersion` 并两端同改，本步只在
   `Capture.cpp`、`WindowQueryRun.cpp`、`Worker.cpp` 三处边界做 `ToNativeRects` / `FromNativeRects`。
2. `Notification.h`：`ShowToastViaWinrt()` 与 `ToastSender` 仍是 WinRT toast 的入口声明（该文件已不含
   `windows.h`，但声明本身属平台实现）。wxWidgets 通知接口接进来时替换它。
3. `Consent.h`：`DialogConsentPrompt` / `DialogDriver` / `RunMonitoredDialog` / `ShowConsentDialog`
   是 MessageBox 那一侧的实现与它的超时状态机；`IConsentPrompt` 是给 wxWidgets 留的接缝。
4. `CaptureCommon.h`：`Dib`（HDC / HBITMAP）、`CopyTextureToFrame`（ID3D11）、`WindowFullRect` /
   `WindowScreenRect` / `ClientScreenRect` / `WindowIsOnTopAt` 返回或接收**原生 RECT**，调用方要搬进
   共用结构时必须显式转换。它现在的中性部分已经在 `core/Frame.h`。
5. `Report.h` 一份头同时承载 JSON 渲染（核心）与标准输出写入（平台服务），`EmitStdoutBytes` 的
   `ioError` 已换成 `NativeErrorCode`，但这一对职责还没分家。
6. 文本层仍是 `std::wstring`（Windows UTF-16）。Linux 上 `wchar_t` 是 UTF-32，所以**共用模型的字符串编码
   约定还没定**：新增核心接口只放 ASCII 机器名（`const char*`），本地化文案继续留在平台/UI 侧。
   把 `std::wstring` 换成带编码声明的类型是独立步骤。
7. `EnvReport` 的 `--capabilities` JSON 仍只报 Windows 探测；把功能矩阵也写进那份 JSON 是后续步骤
   （现在报一堆"计划支持"只会误导调用方）。

## 功能矩阵（与 `src/core/Capability.h` 同源，改一处要改三处：头 / README / MEMORY）

| 能力 | Windows | Linux X11 | Linux Wayland |
| ---- | ------- | --------- | ------------- |
| 目标选择 | 已实现 | 计划支持（XWayland 只见 X11 窗口） | 明确不做（没有全局窗口列表，不虚构） |
| 窗口截图 | 已实现 | 计划支持 | 计划支持（Portal，窗口级来源要探测实现） |
| 屏幕截图 | 已实现，必人工确认 | 计划支持 | 计划支持（Portal + PipeWire） |
| 交互授权 | 已实现（只有程序确认；Windows 无每次调用的系统截屏授权） | 计划支持（只有程序确认） | 计划支持（程序确认 + 系统授权两层，缺一不可） |
| 最小化窗口画面 | 明确不做（`--list=all` 只列不截） | 计划支持（只有旧 pixmap，要如实说明） | 明确不做 |
| 光标 | 运行时探测（WGC 开关有版本门槛；duplication 登记不可核实） | 计划支持 | 计划支持 |
| HDR | 未验证（实现有，本项目无 HDR 显示器实测） | 计划支持 | 计划支持 |
| 无图形会话 | 已实现（帮助/版本/配置/能力查询照跑，桌面路径报 unavailable） | 计划支持 | 计划支持 |

## 已完成 / 未完成

已完成（本步范围内）：调用链与平台类型扩散范围的梳理、`src/core/` 六类中立模型、共用头去平台类型、
`platform/windows/WinBridge.h` 适配层、平台层归属标记、功能矩阵、隐私判据单点化、离线边界测试与
两条验证入口、README 四语新增《跨平台分层与功能矩阵》一节、AGENTS.md 补已落地边界规则。

未完成 / 本步刻意不做：Linux 后端任何实现；wxWidgets 界面与 Portal/PipeWire 接入；`std::wstring` →
带编码声明的文字类型；`WorkerProtocol` 去平台类型；`Report.h` 职责分家；把矩阵写进 `--capabilities`；
把 `WindowIdentity` / `ScreenInfo` 正式映射成 `TargetRef`（类型与判据已就位，接线待后端步骤）。

面向调用方的文档本步**不需要**改，也没有改：CLI 参数、选项含义、JSON 键与取值、错误码、退出码
一条都没动，所以四份 README 里被 `<!-- BEGIN ECAPTURE-HELP -->` 夹住的 `--help` 段仍然新鲜
（`.\\scripts\\mkreadme.ps1 -Check` 应该照旧通过），`.agents\\skills\\yashi-evernight-capture\\SKILL.md`
与 `references/cli-contract.md` 描述的是对外契约，也不受这次内部划分影响。新增的那一节是维护者视角的分层说明。

## 修改文件（本步）

新增：`src/core/{Platform,Geometry,Error,Status,Frame,Target,Authorization,Capability}.h`、
`src/platform/windows/WinBridge.h`、`tests/core_boundary_state.cpp`、`tests/core-boundary.ps1`、
`scripts/core-portability-check.sh`（已 `chmod +x`，LF）。

改动：`src/` 下 39 个头与 .cpp（模型迁移 + 边界转换 + 归属标记）、`CMakeLists.txt`（新增
`ecapture-core-tests` target：只链 `tests/core_boundary_state.cpp` + `src/CaptureScope.cpp`，**不链任何
Windows 库**）、`test-all.ps1`（`$suites` 登记 `core-boundary`，`Real='none'`）、
`installer/payload.manifest.json`（`ecapture-core-tests.exe` 进 test-binary 条目 + `offlineTests` +
`offlineSuiteDeps`，JSON 已用 `python3 -m json` 校验可解析）、四份 README、`AGENTS.md`、`MEMORY.md`。

## 已执行的验证与结果

1. **Linux 编译 + 判据**（本机 Debian，g++ 14.2.0，`cmake 3.31.6`，**没有 MSVC / Windows SDK / mingw-w64**）：
   `./scripts/core-portability-check.sh` → 第 1 关静态清单 37 份共用头全部 ok；第 2 关平台层标记无缺；
   第 3 关 g++ `-std=c++20 -Wall -Wextra` 编译**零警告零错误**；第 4 关运行
   `core-boundary: 141 条通过，0 条失败` → 脚本以"边界成立"退出 0。
2. 逐头 include 检查：`src/core/*.h` 与上列共用头单独成 TU 用 g++ 语法检查全部通过。
3. Windows 类型扩散的收尾审计：`grep` 确认 `src/*.cpp` 里剩下的 `RECT` 全部只在平台层
   （Win32 问答、DWM 宿主、DXGI、管道线上格式），`PhysRect` 出现在边界与模型侧；
   另用脚本核对"用到 Win32 符号却没有 `windows.h` 可达"的文件，5 处命中全是误报（注释或 `<Windows.h>` 大写、`<dxgi.h>` 带入）。
4. JSON 合法性：`installer/payload.manifest.json` 改后用 `json.load` 解析通过。

**未验证（照实记，不算通过）**：

- Windows 侧构建与运行：本机没有 MSVC 与 Windows SDK，也没有 mingw-w64，**无法编译 ECAPTURE.EXE 与任何
  `*-tests.exe`**。`.\build.ps1`、`.\tests\core-boundary.ps1`、`.\test-all.ps1` 都待在 Windows 上首跑。
  风险点集中在：MSVC x64 下 `DWORD(unsigned long)` 与 `uint32_t(unsigned int)` 的**指针**类型不匹配
  （已改的：`ThumbHost::Start`、`DialogDriver::RequestClose`、`OutputSink::EmitBytes`、`EmitStdoutBytes`、
  `ToastSender`/`ShowToastViaWinrt`），以及去掉 `windows.h` 后是否还有 `.cpp` 依赖它传递包含
  （已用脚本扫过，未发现）。若报错，优先看这两类。
- 真机截图行为：本步不取任何像素，既有行为未做回归（改动仅是类型与转换，不改算式与流程）。
- X11 / Wayland：无实现，矩阵那一列是计划，不是能力证明。
- PowerShell 套件的 AST/登记表一致性：`test-all.ps1` 新增条目与 `tests/core-boundary.ps1` 的参数
  （只判到 `-SkipState`，`Real='none'` 所以不需要 `-SkipReal`/`OfflineLayer`）在本机无法解析验证。

## 环境与工具限制

- 开发机：Linux（Debian 13，kernel 6.12），zsh；仓库内 Windows 专用工具链（PowerShell、MSVC、Inno）不可用。
- 本步只新增一条 Linux 入口（`scripts/core-portability-check.sh`），不改任何 `.ps1` 的换行与既有逻辑。
- `AGENTS.md` 被 `.gitignore` 的 `/A*.md` 忽略（仓库既有选择），本文件的修改只留在本地工作树。

## 下一步建议（依赖关系）

1. 先由用户在 Windows 上跑一次 `.\build.ps1` 与 `.\tests\core-boundary.ps1`，把上面那条"未验证"关掉；
   编译错误按两类看（指针类型 / 缺失 `windows.h`）。这一步不做，后面所有 Linux 工作都缺地基。
2. 文本编码约定（`std::wstring` → 声明了编码的类型）宜在接 Linux 后端**之前**单独一步定下来，
   否则每个后端都会各写一份 UTF-16/UTF-8 转换。
3. 之后才轮到：wxWidgets 的 `IConsentPrompt` / 通知实现替换（保留 `ConsentGate` 判据不动）、
   X11 后端（先做目标选择 + 窗口/屏幕取帧，帧仍填 `CapturedFrame`）、Wayland 后端（Portal + PipeWire，
   `RequiredGrantLayers` 已经写明两层都要，程序不代答系统授权界面）。
4. `WindowIdentity` / `ScreenInfo` → `TargetRef` 的正式映射与 `--capabilities` 增加平台列，放在有第二条
   平台路线之后再做，避免只报"计划支持"。
