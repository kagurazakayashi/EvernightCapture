#include "WindowMatch.h"

#include <algorithm>
#include <cwctype>
#include <regex>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>

#include "ScreenMatch.h"

namespace ecapture {
namespace {

std::wstring ToLowerPlain(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

std::wstring FileNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring ImagePathOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return std::wstring();
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return std::wstring();
    wchar_t buffer[8192];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buffer, &size)) result.assign(buffer, size);
    CloseHandle(proc);
    return result;
}

// 编译一次正则在枚举前，避免每个窗口重复构造
struct Compiled {
    std::vector<uint64_t> hwnds;
    std::vector<uint32_t> pids;
    std::vector<std::wstring> processes;     // 已小写
    std::vector<std::wstring> exePaths;      // 已小写
    std::vector<std::wstring> titles;
    std::vector<std::wstring> titleContains;
    std::vector<std::wstring> classes;       // 已小写
    std::vector<std::wregex> titleRegexes;
    std::vector<RECT> onScreens;             // --monitor 的屏幕限制；空 = 不限
};

// 窗口矩形与所选屏有重叠即算命中，所以跨屏窗口在两块屏上都找得到。
// 用完整窗口矩形（含 DWM 那圈透明边）而不是扩展边框：这里要的是"人把窗口放在哪"。
bool OnAnyScreen(const Compiled& c, const WindowInfo& w) {
    if (c.onScreens.empty()) return true;
    const RECT r{w.x, w.y, w.x + w.width, w.y + w.height};
    for (const RECT& m : c.onScreens) {
        if (r.left < m.right && r.right > m.left && r.top < m.bottom && r.bottom > m.top) return true;
    }
    return false;
}

bool Matches(const Compiled& c, const WindowInfo& w) {
    const std::wstring imageLower = ToLowerPlain(w.imageName);
    const std::wstring pathLower = ToLowerPlain(w.imagePath);
    const std::wstring classLower = ToLowerPlain(w.className);

    bool any = false;
    if (!c.hwnds.empty()) {
        any = std::find(c.hwnds.begin(), c.hwnds.end(), w.hwnd) != c.hwnds.end();
        if (!any) return false;
    }
    if (!c.pids.empty()) {
        any = std::find(c.pids.begin(), c.pids.end(), w.pid) != c.pids.end();
        if (!any) return false;
    }
    if (!c.processes.empty()) {
        any = std::find(c.processes.begin(), c.processes.end(), imageLower) != c.processes.end();
        if (!any) return false;
    }
    if (!c.exePaths.empty()) {
        any = std::find(c.exePaths.begin(), c.exePaths.end(), pathLower) != c.exePaths.end();
        if (!any) return false;
    }
    if (!c.titles.empty()) {
        any = std::find(c.titles.begin(), c.titles.end(), w.title) != c.titles.end();
        if (!any) return false;
    }
    if (!c.titleContains.empty()) {
        any = std::any_of(c.titleContains.begin(), c.titleContains.end(),
                          [&](const std::wstring& needle) { return w.title.find(needle) != std::wstring::npos; });
        if (!any) return false;
    }
    if (!c.titleRegexes.empty()) {
        any = std::any_of(c.titleRegexes.begin(), c.titleRegexes.end(), [&](const std::wregex& re) {
            return std::regex_search(w.title, re);
        });
        if (!any) return false;
    }
    if (!c.classes.empty()) {
        any = std::find(c.classes.begin(), c.classes.end(), classLower) != c.classes.end();
        if (!any) return false;
    }
    return true;
}

struct CollectState {
    const Compiled* compiled;
    std::vector<WindowInfo>* all;      // 命中且可见
    std::vector<WindowInfo>* iconic;   // 命中但最小化（用于给提示）
    int32_t order = 0;
};

BOOL CALLBACK CollectCallback(HWND hwnd, LPARAM lParam) {
    auto* state = reinterpret_cast<CollectState*>(lParam);
    const int32_t thisOrder = state->order++;
    if (!IsWindow(hwnd)) return TRUE;

    WindowInfo w;
    w.hwnd = reinterpret_cast<uint64_t>(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    w.pid = static_cast<uint32_t>(pid);

    const int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen > 0) {
        w.title.resize(static_cast<size_t>(titleLen) + 1);
        const int got = GetWindowTextW(hwnd, w.title.data(), titleLen + 1);
        w.title.resize(got > 0 ? static_cast<size_t>(got) : 0);
    }
    wchar_t cls[256];
    const int clsLen = GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    if (clsLen > 0) w.className.assign(cls, static_cast<size_t>(clsLen));

    w.imagePath = ImagePathOf(hwnd);
    if (!w.imagePath.empty()) w.imageName = FileNameOf(w.imagePath);

    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        w.x = rect.left;
        w.y = rect.top;
        w.width = rect.right - rect.left;
        w.height = rect.bottom - rect.top;
    }
    w.zOrder = thisOrder;
    w.iconic = IsIconic(hwnd) != FALSE;

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (w.width <= 0 || w.height <= 0) return TRUE;

    if (!Matches(*state->compiled, w)) return TRUE;
    // 最小化的窗口只为 hint 收集，屏幕限制只管真正的目标
    if (!w.iconic && !OnAnyScreen(*state->compiled, w)) return TRUE;
    if (w.iconic) state->iconic->push_back(w);
    else state->all->push_back(w);
    return TRUE;
}

std::wstring BriefList(const std::vector<WindowInfo>& items, size_t limit) {
    std::wstring s;
    for (size_t i = 0; i < items.size() && i < limit; ++i) {
        if (i) s += L" | ";
        wchar_t buf[96];
        swprintf(buf, 96, L"0x%08X", static_cast<unsigned>(items[i].hwnd));
        s += buf;
        s += L" ";
        s += items[i].title.empty() ? Msg(L"match.untitled") : items[i].title;
        if (!items[i].imageName.empty()) s += L" [" + items[i].imageName + L"]";
    }
    if (items.size() > limit) s += L" …";
    return s;
}

}  // namespace

std::wstring DescribeWindow(const WindowInfo& w) {
    wchar_t buf[160];
    swprintf(buf, 160, L"hwnd=0x%08X pid=%lu %dx%d+%d+%d", static_cast<unsigned>(w.hwnd),
             static_cast<unsigned long>(w.pid), w.width, w.height, w.x, w.y);
    std::wstring s = buf;
    s += L" class=" + w.className;
    if (!w.title.empty()) s += L" title=" + w.title;
    return s;
}

std::vector<WindowInfo> SelectWindows(const Options& opt, std::vector<Diagnostic>* errors,
                                      std::vector<Diagnostic>* notes) {
    (void)notes;  // 预留：以后放"命中但被过滤"的提示
    Compiled c;
    c.hwnds = opt.match.hwnds;
    c.pids = opt.match.pids;
    for (const auto& s : opt.match.processes) {
        std::wstring v = ToLowerPlain(s);
        if (v.find(L'.') == std::wstring::npos) v += L".exe";
        c.processes.push_back(std::move(v));
    }
    for (const auto& s : opt.match.exePaths) c.exePaths.push_back(ToLowerPlain(s));
    c.titles = opt.match.titles;
    c.titleContains = opt.match.titleContains;
    for (const auto& s : opt.match.classes) c.classes.push_back(ToLowerPlain(s));
    for (const auto& expr : opt.match.titleRegexes) {
        try {
            c.titleRegexes.emplace_back(expr, std::regex_constants::ECMAScript);
        } catch (const std::regex_error&) {
            Diagnostic d{codes::kInvalidRegex, Msg(L"cli.regex_late"), L"--title-regex", expr, L""};
            errors->push_back(std::move(d));
            return {};
        }
    }

    // --monitor 与窗口条件同时给出：条件照旧，只是只在所选那块屏上找
    std::wstring monitorLabel;
    if (opt.monitor.given) {
        if (opt.monitor.all) monitorLabel = L"all";
        else if (opt.monitor.ordinal == 0) monitorLabel = L"primary";
        else monitorLabel = std::to_wstring(opt.monitor.ordinal);
        c.onScreens = SelectedScreenRects(opt, errors);
        if (!errors->empty()) return {};
    }

    std::vector<WindowInfo> hits;
    std::vector<WindowInfo> iconic;
    CollectState state{&c, &hits, &iconic, 0};
    EnumWindows(CollectCallback, reinterpret_cast<LPARAM>(&state));

    std::sort(hits.begin(), hits.end(),
              [](const WindowInfo& a, const WindowInfo& b) { return a.zOrder < b.zOrder; });

    const auto fail = [&](Diagnostic d) {
        errors->push_back(std::move(d));
        return std::vector<WindowInfo>{};
    };

    if (hits.empty()) {
        std::wstring hint =
            monitorLabel.empty() ? Msg(L"match.no_window_hint")
                                 : Msgf(L"match.no_window_monitor_hint", monitorLabel);
        if (!iconic.empty()) hint = Msgf(L"match.iconic_hint", iconic.size(), BriefList(iconic, 3));
        return fail(Diagnostic{codes::kNoWindow, Msg(L"match.no_window"), L"", L"", std::move(hint)});
    }

    switch (opt.multi) {
        case MultiMatch::kAll:
            return hits;
        case MultiMatch::kNewest:
            return {hits.front()};              // Z 序最前，近似"最后激活/创建"
        case MultiMatch::kOldest:
            return {hits.back()};
        case MultiMatch::kIndex: {
            const size_t n = static_cast<size_t>(opt.index);
            if (n > hits.size()) {
                return fail(Diagnostic{codes::kIndexOutOfRange, Msg(L"match.index_out_of_range"),
                                       L"--index", std::to_wstring(opt.index),
                                       Msgf(L"match.index_hint", hits.size(), BriefList(hits, 8))});
            }
            return {hits[n - 1]};
        }
        case MultiMatch::kAsk:
        default:
            if (hits.size() == 1) return hits;
            return fail(Diagnostic{codes::kAmbiguousWindow, Msg(L"match.ambiguous"), L"",
                                   std::to_wstring(hits.size()),
                                   Msgf(L"match.ambiguous_hint", BriefList(hits, 8))});
    }
}

}  // namespace ecapture
