// 屏幕（显示器）目标：目标屏的枚举与选择判据。
//
// 编号是 **本次进程这一次屏幕枚举里的位置**，1 起。它是"这次枚举里的位置"，
// 不是系统「显示设置」里那个标识号：系统那边给每块屏编的号存在配置里、可以按设备重新
// 排，而枚举顺序由驱动 / 显示服务决定，两者常常一致但不保证一致，热插拔或改分辨率
// 之后也会变。所以这里不声称"--monitor 2 就是设置里写着 2 的那块屏"，要跨调用追踪同一块屏
// 请认 --screens 交回的设备名（Windows 是 `\\.\DISPLAY1`；X11 是逻辑显示器名前缀，
// 物理输出名另用 `output:` 点）。给出这个顺序的枚举本身按平台分（core/Platform.h 的
// MonitorOrdinalKind）：Windows 是 EnumDisplayMonitors，X11 是 XRRGetMonitors。
// 矩形用的是虚拟屏幕坐标系的物理像素（进程已声明 per-monitor DPI v2）。
//
// 分工：屏幕枚举那条真机问答（EnumScreens）与按选择器取屏的运行时入口
// （SelectScreens / SelectedScreenRects）搬到了平台层（src/platform/）；
// 这一份只留"给两份屏幕清单，判是不是同一块屏"的纯判据。

#include "ScreenMatch.h"

#include <string>
#include <vector>

namespace ecapture {

std::wstring ScreenDisplayName(const ScreenInfo& s) {
    return StripScreenDevicePrefix(s.deviceName);
}

std::wstring DescribeScreen(const ScreenInfo& s) {
    std::wstring t = L"#" + std::to_wstring(s.ordinal) + L" " + s.deviceName + L" " +
                     std::to_wstring(s.bounds.right - s.bounds.left) + L"x" +
                     std::to_wstring(s.bounds.bottom - s.bounds.top) + L"+" +
                     std::to_wstring(s.bounds.left) + L"+" + std::to_wstring(s.bounds.top);
    if (s.primary) t += L" primary";
    if (s.mirrored) t += L" mirrored(" + std::to_wstring(s.outputs.size()) + L")";
    return t;
}

uint64_t ScreenTopologyFingerprint(const std::vector<ScreenInfo>& screens) {
    // FNV-1a。比较的是布局事实，不是名称的展示形式：设备名、输出名逐字符进入，
    // 因为系统就是把它们当作本次连接里的键；数字字段按固定 8 字节混入，避免拼接歧义。
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFull;
            h *= 1099511628211ull;
        }
    };
    auto mixText = [&](const std::wstring& text, uint64_t tag) {
        mix(tag);
        for (wchar_t c : text) mix(static_cast<uint64_t>(static_cast<unsigned>(c)));
        mix(0xffffffffffffffffull);
    };
    mix(static_cast<uint64_t>(screens.size()));
    for (const ScreenInfo& s : screens) {
        mix(static_cast<uint64_t>(s.ordinal));
        mix(static_cast<uint64_t>(s.monitor));
        mix(static_cast<uint64_t>(s.bounds.left));
        mix(static_cast<uint64_t>(s.bounds.top));
        mix(static_cast<uint64_t>(s.bounds.right));
        mix(static_cast<uint64_t>(s.bounds.bottom));
        mix(s.primary ? 1ull : 0ull);
        mix(s.automatic ? 1ull : 0ull);
        mix(s.mirrored ? 1ull : 0ull);
        mix(s.hasScreenIdentity ? 1ull : 0ull);
        mix(s.rootWindow);
        mixText(s.deviceName, 0x1001);
        mixText(s.logicalName, 0x1002);
        mixText(s.displayScope, 0x1003);
        mix(s.desktopKnown ? 1ull : 0ull);
        if (s.desktopKnown) {
            mix(static_cast<uint64_t>(s.desktopBounds.left));
            mix(static_cast<uint64_t>(s.desktopBounds.top));
            mix(static_cast<uint64_t>(s.desktopBounds.right));
            mix(static_cast<uint64_t>(s.desktopBounds.bottom));
        }
        mix(static_cast<uint64_t>(s.outputs.size()));
        for (const ScreenOutputInfo& o : s.outputs) {
            mix(o.outputId);
            mix(o.crtcId);
            mix(o.connected ? 1ull : 0ull);
            mix(o.active ? 1ull : 0ull);
            mix(o.primary ? 1ull : 0ull);
            mix(static_cast<uint64_t>(static_cast<int64_t>(o.rotationDegrees)));
            mix(static_cast<uint64_t>(o.bounds.left));
            mix(static_cast<uint64_t>(o.bounds.top));
            mix(static_cast<uint64_t>(o.bounds.right));
            mix(static_cast<uint64_t>(o.bounds.bottom));
            mixText(o.name, 0x2001);
        }
    }
    return h;
}

std::wstring BriefScreenList(const std::vector<ScreenInfo>& screens) {
    std::wstring s;
    for (const auto& one : screens) {
        if (!s.empty()) s += L" | ";
        s += DescribeScreen(one);
    }
    return s;
}

ScreenCheck CompareScreen(const ScreenInfo& wanted, const std::vector<ScreenInfo>& current,
                          ScreenInfo* fresh) {
    // 只按逻辑设备名认。名字相同而布局、编号、主屏归属或输出/镜像/旋转事实不同 =
    // 同一块屏改了样子；名字不见了 = 那块屏没了。输出名与 XID 不拿来跨平台挑替代屏，
    // 但一旦名字对上，它们的变化必须能让旧授权失效。
    for (const ScreenInfo& s : current) {
        if (s.deviceName != wanted.deviceName) continue;
        if (fresh) *fresh = s;
        const bool sameOutputs = [&] {
            if (s.outputs.size() != wanted.outputs.size()) return false;
            for (size_t i = 0; i < s.outputs.size(); ++i) {
                const ScreenOutputInfo& a = wanted.outputs[i];
                const ScreenOutputInfo& b = s.outputs[i];
                if (a.outputId != b.outputId || a.crtcId != b.crtcId || a.name != b.name ||
                    a.connected != b.connected || a.active != b.active || a.primary != b.primary ||
                    a.rotationDegrees != b.rotationDegrees ||
                    a.bounds.left != b.bounds.left || a.bounds.top != b.bounds.top ||
                    a.bounds.right != b.bounds.right || a.bounds.bottom != b.bounds.bottom) {
                    return false;
                }
            }
            return true;
        }();
        if (s.bounds.left == wanted.bounds.left && s.bounds.top == wanted.bounds.top &&
            s.bounds.right == wanted.bounds.right && s.bounds.bottom == wanted.bounds.bottom &&
            s.ordinal == wanted.ordinal && s.primary == wanted.primary &&
            s.automatic == wanted.automatic && s.mirrored == wanted.mirrored &&
            s.logicalName == wanted.logicalName && sameOutputs) {
            return ScreenCheck::kSame;
        }
        return ScreenCheck::kMoved;
    }
    return ScreenCheck::kGone;
}

}  // namespace ecapture
