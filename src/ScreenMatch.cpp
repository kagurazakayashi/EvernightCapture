// 屏幕（显示器）目标：目标屏的枚举与选择判据。
//
// 编号是 **本次进程这一次 EnumDisplayMonitors 的顺序**，1 起。它是"这次枚举里的位置"，
// 不是 Windows「显示设置」里那个标识号：系统那边给每块屏编的号存在注册表里、可以按设备重新
// 排，而 EnumDisplayMonitors 的顺序由驱动枚举决定，两者常常一致但不保证一致，热插拔或改分辨率
// 之后也会变。所以这里不声称"--monitor 2 就是设置里写着 2 的那块屏"，要跨调用追踪同一块屏
// 请认 images[].device（"\.\DISPLAY1" 这个形状的设备名）。
// 矩形用的是虚拟屏幕坐标系的物理像素（进程已声明 per-monitor DPI v2）。
//
// 分工：EnumDisplayMonitors 那一条真机问答（EnumScreens）与按选择器取屏的运行时入口
// （SelectScreens / SelectedScreenRects）搬到了 src/platform/windows/WinScreenSource.cpp；
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
    return t;
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
    // 只按设备名认。名字相同而矩形或编号不同 = 同一块屏改了样子；名字不见了 = 那块屏没了。
    for (const ScreenInfo& s : current) {
        if (s.deviceName != wanted.deviceName) continue;
        if (fresh) *fresh = s;
        if (s.bounds.left == wanted.bounds.left && s.bounds.top == wanted.bounds.top &&
            s.bounds.right == wanted.bounds.right && s.bounds.bottom == wanted.bounds.bottom &&
            s.ordinal == wanted.ordinal && s.primary == wanted.primary) {
            return ScreenCheck::kSame;
        }
        return ScreenCheck::kMoved;
    }
    return ScreenCheck::kGone;
}

}  // namespace ecapture
