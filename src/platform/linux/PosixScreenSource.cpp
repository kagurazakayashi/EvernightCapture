// 【平台层 · Linux】屏幕来源：X11 / XRandR 的根窗口、逻辑显示器与输出设备拓扑。
//
// 三件事必须分开（调用方看到的字段也分开）：
//   * 输出设备（RROutput）：物理连接这一层，有输出名、连接状态、活动 CRTC 与旋转；
//   * 逻辑显示器（XRRMonitorInfo）：用户实际看到的一块桌面范围，镜像时可由多个输出共享；
//   * 整个桌面（根窗口）：XGetImage 的完整坐标空间，也是 --monitor desktop / --region 的范围。
//
// 边界：
//   * 只读。只调用 XGetWindowAttributes / XRRGetMonitors / XRRGetOutputInfo /
//     XRRGetCrtcInfo；不调用任何 XRRSet*，绝不为了看清旋转去改用户显示设置。
//   * 问不出来就说问不出来：没有 X 连接或根窗口读取失败时交回空表，由选择器报真实错误；
//     不拿"第一块屏"顶上。
//   * XRandR 的 rotation 已经在根窗口像素里体现；这里只把它作为**事实**报出，下游不重复旋转。
//   * 没有跨会话 devnode 标识：ScreenFacts.hasFacts 恒为 false；id: 在解析期就拒绝。

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

int RotationDegrees(Rotation rotation) {
    if (rotation & RR_Rotate_90) return 90;
    if (rotation & RR_Rotate_180) return 180;
    if (rotation & RR_Rotate_270) return 270;
    if (rotation & RR_Rotate_0) return 0;
    return -1;
}

// 一块逻辑显示器的输出事实。XRandR 的 monitor 列表可能只给 RROutput XID；资源表能拿到时
// 再补名字、连接状态、CRTC 范围与旋转，拿不到就保留 XID + "没问出来"，不编名字。
void FillOutputFacts(Display* display, XRRScreenResources* resources, RROutput primary,
                     const XRRMonitorInfo& monitor, std::vector<ScreenOutputInfo>* outputs) {
    if (!outputs) return;
    outputs->clear();
    // RandR 属性查询与热插拔并发时会返回 BadOutput / BadCrtc。用本工具的 trap 收住，
    // 然后把这一组的输出事实整体降级成"只有 XID、其余没问出来"，不交半真半假的名字。
    x11::ErrorTrap trap(display);
    for (int i = 0; i < monitor.noutput; ++i) {
        ScreenOutputInfo out;
        out.outputId = static_cast<uint64_t>(monitor.outputs[i]);
        out.active = true;
        out.primary = primary != None && monitor.outputs[i] == primary;
        out.bounds = PhysRect{monitor.x, monitor.y,
                              monitor.x + static_cast<int32_t>(monitor.width),
                              monitor.y + static_cast<int32_t>(monitor.height)};
        if (resources) {
            XRROutputInfo* info = XRRGetOutputInfo(display, resources, monitor.outputs[i]);
            if (info) {
                out.name = WideAscii(info->name);
                out.connected = info->connection == RR_Connected;
                out.crtcId = static_cast<uint64_t>(info->crtc);
                if (info->crtc != None) {
                    XRRCrtcInfo* crtc = XRRGetCrtcInfo(display, resources, info->crtc);
                    if (crtc) {
                        out.bounds = PhysRect{crtc->x, crtc->y,
                                              crtc->x + static_cast<int32_t>(crtc->width),
                                              crtc->y + static_cast<int32_t>(crtc->height)};
                        out.rotationDegrees = RotationDegrees(crtc->rotation);
                        XRRFreeCrtcInfo(crtc);
                    }
                }
                XRRFreeOutputInfo(info);
            }
        }
        outputs->push_back(std::move(out));
    }
    if (trap.Sync() != 0) {
        outputs->clear();
        for (int i = 0; i < monitor.noutput; ++i) {
            ScreenOutputInfo out;
            out.outputId = static_cast<uint64_t>(monitor.outputs[i]);
            out.active = true;
            outputs->push_back(std::move(out));
        }
    }
}

std::vector<ScreenInfo> EnumerateScreens(Display* display) {
    std::vector<ScreenInfo> screens;
    if (!display) return screens;
    const Window root = DefaultRootWindow(display);

    XWindowAttributes rootAttrs{};
    x11::ErrorTrap rootTrap(display);
    const int rootOk = XGetWindowAttributes(display, root, &rootAttrs);
    const int rootError = rootTrap.Sync();
    if (rootOk == 0 || rootError != 0 || rootAttrs.width <= 0 || rootAttrs.height <= 0) {
        return screens;
    }

    x11::DisplaySessionFacts session;
    x11::QueryDisplaySessionFacts(display, &session);

    const PhysRect desktop{0, 0, rootAttrs.width, rootAttrs.height};
    const auto decorate = [&](ScreenInfo* info) {
        info->hasScreenIdentity = session.known;
        info->rootWindow = static_cast<uint64_t>(root);
        info->displayScope = session.scope;
        info->desktopKnown = true;
        info->desktopBounds = desktop;
    };

    int eventBase = 0;
    int errorBase = 0;
    if (XRRQueryExtension(display, &eventBase, &errorBase) != 0) {
        int major = 0;
        int minor = 0;
        const bool monitorsApi = XRRQueryVersion(display, &major, &minor) != 0 &&
                                 (major > 1 || (major == 1 && minor >= 5));
        int count = 0;
        XRRMonitorInfo* monitors = nullptr;
        int monitorsError = 0;
        if (monitorsApi) {
            x11::ErrorTrap trap(display);
            monitors = XRRGetMonitors(display, root, True, &count);
            monitorsError = trap.Sync();
            if (monitorsError != 0) {
                if (monitors) XRRFreeMonitors(monitors);
                monitors = nullptr;
                count = 0;
            }
        }
        if (monitors && count > 0) {
            const RROutput primary = XRRGetOutputPrimary(display, root);
            XRRScreenResources* resources = XRRGetScreenResourcesCurrent(display, root);
            screens.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const XRRMonitorInfo& monitor = monitors[i];
                ScreenInfo info;
                info.monitor = static_cast<uint64_t>(i + 1);
                info.ordinal = static_cast<uint32_t>(i + 1);
                if (monitor.name != None) {
                    char* name = XGetAtomName(display, monitor.name);
                    if (name) {
                        info.logicalName = WideAscii(name);
                        XFree(name);
                    }
                }
                FillOutputFacts(display, resources, primary, monitor, &info.outputs);
                info.deviceName = info.logicalName;
                if (info.deviceName.empty() && !info.outputs.empty()) {
                    info.deviceName = info.outputs.front().name;
                }
                if (info.deviceName.empty()) info.deviceName = L"X11-" + std::to_wstring(i + 1);
                info.bounds.left = monitor.x;
                info.bounds.top = monitor.y;
                info.bounds.right = monitor.x + static_cast<int32_t>(monitor.width);
                info.bounds.bottom = monitor.y + static_cast<int32_t>(monitor.height);
                info.primary = monitor.primary != 0 ||
                               (monitor.noutput > 0 && monitor.outputs != nullptr &&
                                monitor.outputs[0] == primary);
                info.automatic = monitor.automatic != 0;
                info.mirrored = monitor.noutput > 1;
                decorate(&info);
                screens.push_back(std::move(info));
            }
            if (resources) XRRFreeScreenResources(resources);
            XRRFreeMonitors(monitors);
            return screens;
        }
        if (monitors) XRRFreeMonitors(monitors);
    }

    // 没有 RandR（或它说没有监视器）：整个根窗口就是唯一一块逻辑显示器。输出设备这一层
    // 没有答案，outputs 留空；编号仍然从 1 起，设备名明确标成本工具合成，而不是输出名。
    ScreenInfo info;
    info.monitor = 1;
    info.ordinal = 1;
    info.deviceName = L"X11-1";
    info.logicalName = info.deviceName;
    info.bounds = desktop;
    info.primary = true;
    decorate(&info);
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
