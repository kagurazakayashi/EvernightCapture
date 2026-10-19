// 【平台层 · Linux】屏幕来源：X11 / XRandR 的监视器拓扑。
//
// 与 Windows 的 EnumDisplayMonitors 对应：编号是**这一次枚举里的位置**（1 起），不是
// 显示设置里那个可以重排的标识号。跨调用追踪同一块屏请认 deviceName（输出名，例如
// "HDMI-1"；拿不到输出名时是 "X11-<编号>"）。
//
// 三条边界：
//   * 只读：XRRGetMonitors / XGetWindowAttributes 都不改显示配置。
//   * 问不出来就说问不出来：没有 X 连接时交回空表，由选择器写 match.monitor_out_of_range
//     （而不是"那就用主屏"）。
//   * 没有跨会话的 devnode 标识：ScreenFacts.hasFacts 恒为 false。按 id: 选屏会因此
//     报 match.monitor_id_unverifiable —— 本构建不拿"看起来像"的设备名去冒充跨会话标识。

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ScreenIdentity.h"
#include "ScreenMatch.h"
#include "platform/linux/X11Capture.h"

namespace ecapture {
namespace {

std::wstring WideAscii(const char* text) {
    if (!text) return std::wstring();
    std::wstring out;
    for (const char* p = text; *p; ++p) out.push_back(static_cast<unsigned char>(*p));
    return out;
}

std::vector<ScreenInfo> EnumerateScreens(Display* display) {
    std::vector<ScreenInfo> screens;
    if (!display) return screens;
    const Window root = DefaultRootWindow(display);

    int eventBase = 0;
    int errorBase = 0;
    if (XRRQueryExtension(display, &eventBase, &errorBase) != 0) {
        int count = 0;
        XRRMonitorInfo* monitors = XRRGetMonitors(display, root, True, &count);
        if (monitors && count > 0) {
            const RROutput primary = XRRGetOutputPrimary(display, root);
            screens.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const XRRMonitorInfo& monitor = monitors[i];
                ScreenInfo info;
                info.monitor = static_cast<uint64_t>(i + 1);
                info.ordinal = static_cast<uint32_t>(i + 1);
                if (monitor.name != None) {
                    char* name = XGetAtomName(display, monitor.name);
                    if (name) {
                        info.deviceName = WideAscii(name);
                        XFree(name);
                    }
                }
                if (info.deviceName.empty()) info.deviceName = L"X11-" + std::to_wstring(i + 1);
                info.bounds.left = monitor.x;
                info.bounds.top = monitor.y;
                info.bounds.right = monitor.x + monitor.width;
                info.bounds.bottom = monitor.y + monitor.height;
                // XRandR 直接给了 primary 标志；没有它时再退回"第一个输出等于
                // XRRGetOutputPrimary"这条判据。
                info.primary = monitor.primary != 0 ||
                               (monitor.noutput > 0 && monitor.outputs != nullptr &&
                                monitor.outputs[0] == primary);
                screens.push_back(std::move(info));
            }
            XRRFreeMonitors(monitors);
            return screens;
        }
        if (monitors) XRRFreeMonitors(monitors);
    }

    // 没有 RandR（或它说没有监视器）：整个根窗口就是唯一一块屏。编号仍然从 1 起。
    XWindowAttributes attrs{};
    x11::ErrorTrap trap(display);
    const int ok = XGetWindowAttributes(display, root, &attrs);
    const int error = trap.Sync();
    if (ok == 0 || error != 0) return screens;
    ScreenInfo info;
    info.monitor = 1;
    info.ordinal = 1;
    info.deviceName = L"X11-1";
    info.bounds.left = 0;
    info.bounds.top = 0;
    info.bounds.right = attrs.width;
    info.bounds.bottom = attrs.height;
    info.primary = true;
    screens.push_back(std::move(info));
    return screens;
}

}  // namespace

std::vector<ScreenInfo> EnumScreens() {
    x11::DisplayConnection connection;
    return EnumerateScreens(connection.get());
}

std::vector<ScreenCandidate> EnumScreenCandidates(bool needFacts) {
    (void)needFacts;  // 没有显示配置那一路可问：事实恒为"没问过"
    std::vector<ScreenCandidate> result;
    const std::vector<ScreenInfo> screens = EnumScreens();
    result.reserve(screens.size());
    for (const ScreenInfo& screen : screens) {
        ScreenCandidate candidate;
        candidate.screen = screen;
        candidate.facts.hasFacts = false;
        result.push_back(std::move(candidate));
    }
    return result;
}

std::vector<ScreenCandidate> SelectScreenCandidatesOf(const MonitorSelector& sel,
                                                      std::vector<Diagnostic>* errors) {
    const std::vector<ScreenCandidate> all = EnumScreenCandidates(MonitorSelectorNeedsIdentity(sel));
    const ScreenSelectResult selected = SelectScreenCandidates(sel, all);
    std::vector<ScreenCandidate> result;
    if (!selected.errors.empty()) {
        if (errors) errors->insert(errors->end(), selected.errors.begin(), selected.errors.end());
        return result;
    }
    result.reserve(selected.picked.size());
    for (std::size_t index : selected.picked) {
        if (index < all.size()) result.push_back(all[index]);
    }
    return result;
}

std::vector<ScreenInfo> SelectScreens(const Options& opt, std::vector<Diagnostic>* errors) {
    std::vector<ScreenInfo> screens;
    for (const ScreenCandidate& candidate : SelectScreenCandidatesOf(opt.monitor, errors)) {
        screens.push_back(candidate.screen);
    }
    return screens;
}

std::vector<PhysRect> SelectedScreenRects(const Options& opt, std::vector<Diagnostic>* errors) {
    std::vector<PhysRect> rects;
    for (const ScreenInfo& screen : SelectScreens(opt, errors)) {
        rects.push_back(screen.bounds);
    }
    return rects;
}

}  // namespace ecapture
