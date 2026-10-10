#pragma once
// 跨平台核心：像素来源与授权层。
//
// 这一层只管"这张图的像素从哪来、要哪几层点头"，不弹窗、不取帧、不碰任何系统授权界面。
// 路径名到来源的那张登记表仍然在 CaptureScope.h/.cpp（那里是**通道名字**的归属地），
// 本文件只放三家平台共用的判据本体，免得 Windows 与将来的 X11/Wayland 各写一套"什么算桌面像素"。
//
// 三条隐私边界（与 Consent.h 的产品规则同源，这里只把最硬的那两条写成函数）：
//   1. 只有真正绑定所选窗口自己的画面、不从桌面采别的像素的那条路径，才可能被 --yes 免掉确认。
//      先取全屏再裁成窗口大小**仍然属于桌面截图** —— 判据看的是像素从哪来，不是最后那张图多大。
//   2. 桌面像素那一级一定要有人明确点头：--yes、--quiet、环境变量、调用身份都不能替代它。
//      漏登记的新的路径按桌面处理（默认更严格而不是更松），这条在 CaptureScope 的 ScopeOf 里。
//   3. 程序自己的确认框与操作系统的授权界面是**两层**，谁也不替代谁：
//      Windows 现在只有第一层（系统不提供每次调用的截屏授权），Wayland 两层都要 ——
//      Portal 那次授权（以及它要求的来源选择）不能拿程序确认框顶掉，反过来程序确认也不能因为
//      "Portal 已经记住了"而省掉。单窗口的画面只能来自满足窗口范围的来源，屏幕来源不算。
//      这一条不是注释里的愿望：RequiredGrantLayers 把它写成判据，tests\core_boundary_state.cpp
//      逐条钉住"Wayland 少一层就不放行"。

#include <cstdint>

#include "core/Platform.h"  // PlatformKind

namespace ecapture {

// 一条实际执行到的内部路径的像素来源。
enum class PixelScope {
    kWindowContent,  // 只有所选窗口自己的画面
    kDesktop,        // 可能含其它窗口 / 桌面像素
};

// --yes 在这一条路径上生不生效。写成一条函数是为了让它只有一个出处：
// CaptureScope 的 NeedsHumanConsent、能力查询里"给了 --yes 还要不要问人"那一格、
// 以及将来 Linux 后端的授权判定读的都是这一句，不在三处各判一遍。
inline bool YesGivenExempts(PixelScope scope, bool yesGiven) {
    return scope == PixelScope::kWindowContent && yesGiven;
}

// 取这一类像素需要哪几层点头。位或在一起：Wayland 的桌面像素两层都要。
// 这里说的是**要求**，不是"这台机器现在让不让" —— 后者由运行时探测回答（EnvReport 那一路）。
enum GrantLayer : uint32_t {
    kGrantNone = 0u,
    kGrantProgramConfirm = 1u,       // 本程序的确认框（wxWidgets；Windows 现在是 MessageBox）
    kGrantSystemAuthorization = 2u,  // 桌面环境的系统授权（Wayland 的 XDG Desktop Portal）
};

// 某个平台族上，取这一类像素**至少**要哪几层点头（上面那几个 GrantLayer 位或在一起）。
// 这里说的是**要求**，不是"这台机器现在让不让" —— 后者由运行时探测回答（EnvReport 那一路）。
// 三条依据：
//   * 桌面像素：程序确认这一级恒在（规矩 2），谁也免不掉。
//   * 系统授权：Wayland 恒在 —— 合成器不给没拿到 Portal 授权的进程读屏幕像素，而那一次授权
//     连着"由谁截"的来源选择，是系统界面的事，本程序的确认框替代不了它；反过来 Portal 记住了
//     会话、给了恢复令牌，也不能让本程序省掉那一次人工确认（AGENTS.md《Wayland 提示与系统权限》）。
//   * Windows 与 X11 现在没有系统授权这一级：那是这两家平台的现状而不是本工具的选择，
//     所以如实登记成"只有程序确认"，等真探测到了（例如某桌面另加了权限门户）再改这一格，
//     不提前把不存在的一层写进去充数。
inline uint32_t RequiredGrantLayers(PlatformKind platform) {
    uint32_t layers = kGrantProgramConfirm;
    if (platform == PlatformKind::kLinuxWayland) layers |= kGrantSystemAuthorization;
    return layers;
}

// 这一层要求缺没缺：拿到的层是否已经覆盖要求的层。缺任何一层就是没授权，
// 不许按"少弹一个框也算过"处理。
inline bool HasAllGrantLayers(uint32_t required, uint32_t granted) {
    return (required & granted) == required;
}

}  // namespace ecapture
