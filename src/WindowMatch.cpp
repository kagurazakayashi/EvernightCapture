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
};

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
        s += items[i].title.empty() ? L"(无标题)" : items[i].title;
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
            Diagnostic d{codes::kInvalidRegex, L"正则在该阶段重新编译失败", L"--title-regex", expr, L""};
            errors->push_back(std::move(d));
            return {};
        }
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
        std::wstring hint = L"确认窗口已显示且没被别的进程独占；最小化的窗口无法采集";
        if (!iconic.empty()) {
            hint = L"有 " + std::to_wstring(iconic.size()) + L" 个窗口命中条件但处于最小化，无法采集：" +
                   BriefList(iconic, 3);
        }
        return fail(Diagnostic{codes::kNoWindow, L"没有窗口满足全部条件", L"", L"", hint});
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
                return fail(Diagnostic{codes::kIndexOutOfRange,
                                       L"--index 超出匹配窗口数量", L"--index",
                                       std::to_wstring(opt.index),
                                       L"共 " + std::to_wstring(hits.size()) + L" 个候选：" +
                                           BriefList(hits, 8)});
            }
            return {hits[n - 1]};
        }
        case MultiMatch::kAsk:
        default:
            if (hits.size() == 1) return hits;
            return fail(Diagnostic{codes::kAmbiguousWindow,
                                   L"匹配到多个窗口，需要消歧", L"",
                                   std::to_wstring(hits.size()),
                                   L"用 --index/--newest/--oldest/--all 或补充条件。候选（按 Z 序）：" +
                                       BriefList(hits, 8)});
    }
}

}  // namespace ecapture
