#include "ScreenMatch.h"

#include <string>
#include <vector>

#include <windows.h>

#include "ScreenIdentity.h"   // 选择器判据只有一份，见那个头文件顶部那四条规矩

namespace ecapture {
namespace {

BOOL CALLBACK CollectMonitor(HMONITOR hmon, HDC /*dc*/, LPRECT /*rect*/, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<ScreenInfo>*>(lParam);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hmon, &info)) return TRUE;

    ScreenInfo s;
    s.monitor = reinterpret_cast<uint64_t>(hmon);
    s.ordinal = static_cast<uint32_t>(list->size() + 1);
    s.deviceName = info.szDevice;
    s.bounds = info.rcMonitor;
    s.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(std::move(s));
    return TRUE;
}

bool RectNonEmpty(const RECT& r) { return r.right > r.left && r.bottom > r.top; }

}  // namespace

std::vector<ScreenInfo> EnumScreens() {
    std::vector<ScreenInfo> screens;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&screens));
    return screens;
}

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

std::vector<ScreenInfo> SelectScreens(const Options& opt, std::vector<Diagnostic>* errors) {
    // 判据只有一份（ScreenIdentity.h 的 SelectScreenCandidates）：按屏过滤窗口这一路与
    // 真去截整屏那一路必须认出同一块屏，否则 --list 里点名的那块和实际截到的那块能各判一次。
    std::vector<ScreenInfo> out;
    for (const ScreenCandidate& c : SelectScreenCandidatesOf(opt.monitor, errors)) {
        out.push_back(c.screen);
    }
    return out;
}

std::vector<RECT> SelectedScreenRects(const Options& opt, std::vector<Diagnostic>* errors) {
    std::vector<RECT> rects;
    for (const auto& s : SelectScreens(opt, errors)) {
        if (RectNonEmpty(s.bounds)) rects.push_back(s.bounds);
    }
    return rects;
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
