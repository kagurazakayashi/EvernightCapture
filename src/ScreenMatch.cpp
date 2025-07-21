#include "ScreenMatch.h"

#include <string>
#include <vector>

#include <windows.h>

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
    const std::wstring prefix = L"\\\\.\\";
    if (s.deviceName.size() > prefix.size() && s.deviceName.compare(0, prefix.size(), prefix) == 0)
        return s.deviceName.substr(prefix.size());
    return s.deviceName;
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
    const std::vector<ScreenInfo> all = EnumScreens();

    const auto outOfRange = [&](const std::wstring& value) {
        errors->push_back(Diagnostic{codes::kMonitorOutOfRange, Msg(L"match.monitor_out_of_range"),
                                     L"--monitor", value,
                                     Msgf(L"match.monitor_out_of_range_hint", all.size(),
                                          BriefScreenList(all))});
        return std::vector<ScreenInfo>{};
    };

    if (all.empty()) return outOfRange(std::to_wstring(opt.monitor.ordinal));
    if (opt.monitor.all) return all;
    if (opt.monitor.ordinal == 0) {
        for (const auto& s : all) {
            if (s.primary) return {s};
        }
        return {all.front()};  // 没有哪块被标成主屏（远程会话里见过），用第一块
    }
    for (const auto& s : all) {
        if (s.ordinal == static_cast<uint32_t>(opt.monitor.ordinal)) return {s};
    }
    return outOfRange(std::to_wstring(opt.monitor.ordinal));
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
