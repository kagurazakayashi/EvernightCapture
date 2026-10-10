#pragma once
// 跨平台核心：能力与功能矩阵。
//
// 三件事必须分开写（与 src/EnvReport.h 那三条同源，这份矩阵只补上"平台族"这一根轴）：
//   1. **这个构建里有没有实现**：编译期的事。没实现就是没实现，不因为"平台理论上做得到"
//      就写成本工具支持。
//   2. **这台机器现在让不让走**：运行时的探测结果。版本门槛、合成器能力、Portal 接口版本、
//      有没有屏幕输出，全部走 --capabilities / --diagnostics 那一路的三值判据，不写进这份
//      编译期矩阵里 —— 两边各写各的，才有"报告与实探不同源"的风险。
//   3. **本项目有没有在那种环境实测过**：只有开发机那一台（EnvReport.h 的 verified_env）。
//      没实测过的格子写 kUnverified，既不写 kImplemented 也不蒙一个 kRuntimeProbe。
//
// 用法约束：这份表是给"人读的功能矩阵（README）+ 机器读的判据（tests\core_boundary_state.cpp）"
// 共用的同一份事实。--capabilities 现在仍然只报 Windows 那一路的探测结果（那份 JSON 的契约
// 版本号没变），把这张表也写进 --capabilities 是后续步骤，等 Linux 后端真的编译进构建时再做，
// 免得现在报一堆"计划支持"让调用方误判。
//
// 状态取值里 **kUnknown 是禁止出现的**：一格子要是说不清，就先写成 kPlanned（本构建没实现）
// 或 kNotSupported（这条路不做），并在那一句 note 里写清为什么；测试会逐格钉这一条。

#include <cstdint>

#include "core/Platform.h"

namespace ecapture {

// 一条能力相对**这个构建**的状态。取值只增不改名。
enum class MatrixStatus : uint32_t {
    kImplemented = 0,   // 已实现，且本项目在登记过的环境里实测过
    kUnverified = 1,    // 已实现，但本项目没有在那种环境实测过（不声称跑得通）
    kRuntimeProbe = 2,  // 已实现，但能不能兑现要看运行时探测（版本门槛、接口能力）
    kPlanned = 3,       // 计划支持：这个构建里还没有这条实现
    kNotSupported = 4,  // 明确不做：平台本身不提供这条路，或设计上决定不走
    kUnknown = 5,       // 没登记。矩阵里出现它就是缺陷（测试会红），不许当兜底值用
};

inline const char* MatrixStatusName(MatrixStatus status) {
    switch (status) {
        case MatrixStatus::kImplemented: return "implemented";
        case MatrixStatus::kUnverified: return "unverified";
        case MatrixStatus::kRuntimeProbe: return "runtime_probe";
        case MatrixStatus::kPlanned: return "planned";
        case MatrixStatus::kNotSupported: return "not_supported";
        case MatrixStatus::kUnknown: return "unknown";
    }
    return "unknown";
}

// 矩阵的功能轴。每一条都是"能不能对某个目标做成这件事"，不是某个 CLI 选项。
enum class MatrixFeature : uint32_t {
    kTargetListing = 0,     // 目标选择：列出/inspect 可选的窗口与屏幕，并按条件命中
    kWindowCapture = 1,     // 窗口截图：只含所选窗口自己的画面
    kScreenCapture = 2,     // 屏幕截图：整块输出的桌面像素
    kInteractiveGrant = 3,  // 交互授权：程序确认框，以及（Wayland）系统授权那一层
    kMinimizedWindow = 4,   // 最小化窗口的画面
    kCursorControl = 5,     // 光标：--cursor include/exclude 能否兑现
    kHdr = 6,               // HDR：广色域来源帧的取回与 tone mapping
    kHeadlessSession = 7,   // 无图形会话：不依赖桌面那部分命令照常可跑
    kCount = 8,
};

inline const char* MatrixFeatureName(MatrixFeature feature) {
    switch (feature) {
        case MatrixFeature::kTargetListing: return "target_listing";
        case MatrixFeature::kWindowCapture: return "window_capture";
        case MatrixFeature::kScreenCapture: return "screen_capture";
        case MatrixFeature::kInteractiveGrant: return "interactive_grant";
        case MatrixFeature::kMinimizedWindow: return "minimized_window";
        case MatrixFeature::kCursorControl: return "cursor_control";
        case MatrixFeature::kHdr: return "hdr";
        case MatrixFeature::kHeadlessSession: return "headless_session";
        case MatrixFeature::kCount: break;
    }
    return "unknown";
}

struct MatrixCell {
    PlatformKind platform;
    MatrixFeature feature;
    MatrixStatus status;
    const char* note;  // 恒 ASCII：这一格为什么是这个状态，人类读、测试也读
};

// ---------------------------------------------------------------------------
// 矩阵本体。README《跨平台功能矩阵》那一节写的是这张表，两处必须一起改。
//
// Windows 那一列对着当前实现与 EnvReport 的登记逐格填；两条 Linux 列**一律不能出现
// kImplemented / kUnverified / kRuntimeProbe**，因为这个构建里既没有 X11 代码也没有
// Portal 代码 —— 写了就是"伪造 Linux 成功后端"（AGENTS.md《构建、验证与文档》：
// 跨平台编译成功不等于平台运行通过，这里连编译都没有）。
// ---------------------------------------------------------------------------
inline const MatrixCell* PlatformMatrixCells(uint32_t* count) {
    static const MatrixCell kCells[] = {
        // ---- Windows：现状（本项目唯一实测过的环境是 EnvReport 里登记的那一台）----
        {PlatformKind::kWindows, MatrixFeature::kTargetListing, MatrixStatus::kImplemented,
         "EnumDisplayMonitors + EnumWindows; --list/--inspect as structured JSON"},
        {PlatformKind::kWindows, MatrixFeature::kWindowCapture, MatrixStatus::kImplemented,
         "wgc / dwm.thumbnail / printwindow; these paths bind to the window surface"},
        {PlatformKind::kWindows, MatrixFeature::kScreenCapture, MatrixStatus::kImplemented,
         "screen.wgc / screen.bitblt / screen.duplication; always behind a human confirm"},
        {PlatformKind::kWindows, MatrixFeature::kInteractiveGrant, MatrixStatus::kImplemented,
         "program confirm only (MessageBox now, wxWidgets planned); Windows has no per-call system screen grant"},
        {PlatformKind::kWindows, MatrixFeature::kMinimizedWindow, MatrixStatus::kNotSupported,
         "iconic windows are listed with --list=all and only collected for the hint; never captured"},
        {PlatformKind::kWindows, MatrixFeature::kCursorControl, MatrixStatus::kRuntimeProbe,
         "wgc cursor switch has a build floor; duplication frames are registered pointer_state_unverified"},
        {PlatformKind::kWindows, MatrixFeature::kHdr, MatrixStatus::kUnverified,
         "tonemap is implemented and judged offline, but this project has no HDR display to verify on"},
        {PlatformKind::kWindows, MatrixFeature::kHeadlessSession, MatrixStatus::kImplemented,
         "help/version/config/capabilities run with no desktop; desktop pixel paths report unavailable"},

        // ---- Linux X11：本构建一行代码都没有，全部按"计划支持"登记 ----
        {PlatformKind::kLinuxX11, MatrixFeature::kTargetListing, MatrixStatus::kPlanned,
         "planned via X11 window queries; XWayland only sees X11 windows, native Wayland ones are invisible"},
        {PlatformKind::kLinuxX11, MatrixFeature::kWindowCapture, MatrixStatus::kPlanned,
         "planned; no X11 backend is compiled into this build"},
        {PlatformKind::kLinuxX11, MatrixFeature::kScreenCapture, MatrixStatus::kPlanned,
         "planned; needs the same human confirm as Windows desktop pixels"},
        {PlatformKind::kLinuxX11, MatrixFeature::kInteractiveGrant, MatrixStatus::kPlanned,
         "planned: program confirm only (X11 itself has no per-call screen grant) until probed otherwise"},
        {PlatformKind::kLinuxX11, MatrixFeature::kMinimizedWindow, MatrixStatus::kPlanned,
         "planned; an iconic X11 window is unmapped, so whatever is offered must say it is a stale pixmap"},
        {PlatformKind::kLinuxX11, MatrixFeature::kCursorControl, MatrixStatus::kPlanned,
         "planned; not probed, not claimed"},
        {PlatformKind::kLinuxX11, MatrixFeature::kHdr, MatrixStatus::kPlanned,
         "planned; no X11 wide-gamut path is implemented here, so nothing is asserted about HDR"},
        {PlatformKind::kLinuxX11, MatrixFeature::kHeadlessSession, MatrixStatus::kPlanned,
         "planned: help/version/config/capabilities must run without DISPLAY; capture must report the real limit"},

        // ---- Linux Wayland：同上，外加两条平台本身不给的路 ----
        {PlatformKind::kLinuxWayland, MatrixFeature::kTargetListing, MatrixStatus::kNotSupported,
         "compositors do not give clients a global window list; this tool will not fake one"},
        {PlatformKind::kLinuxWayland, MatrixFeature::kWindowCapture, MatrixStatus::kPlanned,
         "planned via Portal; whether a window-scoped source can be honoured is a runtime probe of the portal implementation"},
        {PlatformKind::kLinuxWayland, MatrixFeature::kScreenCapture, MatrixStatus::kPlanned,
         "planned via XDG Desktop Portal ScreenShot / Screen Capture plus PipeWire"},
        {PlatformKind::kLinuxWayland, MatrixFeature::kInteractiveGrant, MatrixStatus::kPlanned,
         "planned: BOTH layers required (program confirm plus the system Portal grant); neither replaces the other"},
        {PlatformKind::kLinuxWayland, MatrixFeature::kMinimizedWindow, MatrixStatus::kNotSupported,
         "no window list and no window handles means there is nothing to point a minimized surface at"},
        {PlatformKind::kLinuxWayland, MatrixFeature::kCursorControl, MatrixStatus::kPlanned,
         "planned; Portal cursor metadata is an interface-capability probe, not assumed"},
        {PlatformKind::kLinuxWayland, MatrixFeature::kHdr, MatrixStatus::kPlanned,
         "planned; depends on the portal reporting the source colour space, which is not probed today"},
        {PlatformKind::kLinuxWayland, MatrixFeature::kHeadlessSession, MatrixStatus::kPlanned,
         "planned: non-desktop commands must still run; Portal requests report the real session limit"},
    };
    *count = sizeof(kCells) / sizeof(kCells[0]);
    return kCells;
}

inline uint32_t PlatformMatrixSize() {
    uint32_t n = 0;
    PlatformMatrixCells(&n);
    return n;
}

// 取某一格。没有登记的那一格返回 kUnknown —— 调用方据此知道"这一格还没填"，
// 而不是拿到一个看起来可用的默认状态。
inline MatrixStatus MatrixStatusOf(PlatformKind platform, MatrixFeature feature) {
    uint32_t n = 0;
    const MatrixCell* cells = PlatformMatrixCells(&n);
    for (uint32_t i = 0; i < n; ++i) {
        if (cells[i].platform == platform && cells[i].feature == feature) return cells[i].status;
    }
    return MatrixStatus::kUnknown;
}

// 这一格里"这个构建到底有没有实现"。矩阵状态里三种已实现档
//（implemented / unverified / runtime_probe）都算有，planned / not_supported / unknown 都算没有。
// 分开问是因为"没实现"与"实现了但没验证"对调用方是不同的下一步。
inline bool MatrixIsCompiled(MatrixStatus status) {
    return status == MatrixStatus::kImplemented || status == MatrixStatus::kUnverified ||
           status == MatrixStatus::kRuntimeProbe;
}

}  // namespace ecapture
