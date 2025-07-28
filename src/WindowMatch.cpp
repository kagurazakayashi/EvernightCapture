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

// 「被挡下」与「问不出来」是两件事：前者意味着换一个更高权限的调用方就能读到，
// 后者可能是那个进程刚刚已经没了。结构化窗口查询要把这两种下场分开写，所以这里只认
// 系统明确说不许你读的那两个码，其余一律按「问过而失败」处理。
bool IsDeniedError(DWORD gle) {
    return gle == ERROR_ACCESS_DENIED || gle == ERROR_PRIVILEGE_NOT_HELD;
}

// 一条候选窗口的归属进程问答结果（由 ProcessFactsOf 填，落到 WindowInfo 的同名诸项）。
struct WindowFacts {
    std::wstring path;
    uint64_t startTicks = 0;
    ReadState pathRead = ReadState::kFailed;
    ReadState startRead = ReadState::kFailed;
    uint32_t pathWin32 = 0;
    uint32_t startWin32 = 0;
};

// 一次 OpenProcess 问两件事：映像路径与进程创建时间。分成两次开句柄没有意义，
// 而"当场再问一次创建时间"更不行 —— 身份复核要比的是**枚举那一刻**的值
//（见 WindowIdentity.h：中间那次销毁重建会被记成基线，复核就成了自己跟自己对答案）。
// 读不到的一律留空 / 0：那是"这一条判据没做出来"，不是"它相同"。
//
// 每一问的下场都记进 WindowFacts（read + win32），因为读不到对调用方是有信息量的：
// 结构化窗口查询（--list / --inspect）要把「这一项问不出来」与「这一项是空的」分开写，
// 而不是拿空值冒充答案。原因码必须在失败点当场取走 —— 之后的任何 API 都会把它覆盖掉。
WindowFacts ProcessFactsOf(HWND hwnd) {
    WindowFacts f;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return f;   // 连归属进程都问不出来：两条问句都没答案
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        const DWORD gle = GetLastError();
        f.pathWin32 = gle;
        f.startWin32 = gle;
        const ReadState state = IsDeniedError(gle) ? ReadState::kDenied : ReadState::kFailed;
        f.pathRead = state;
        f.startRead = state;
        return f;
    }
    wchar_t buffer[8192];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (QueryFullProcessImageNameW(proc, 0, buffer, &size)) {
        f.path.assign(buffer, size);
        f.pathRead = ReadState::kReadable;
    } else {
        f.pathWin32 = GetLastError();
        f.pathRead = IsDeniedError(f.pathWin32) ? ReadState::kDenied : ReadState::kFailed;
    }
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(proc, &creation, &exit, &kernel, &user)) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = creation.dwLowDateTime;
        ticks.HighPart = creation.dwHighDateTime;
        // QuadPart 为 0 是"没拿到"的写法，不能当成一个真值传给复核去比。
        if (ticks.QuadPart != 0) {
            f.startTicks = ticks.QuadPart;
            f.startRead = ReadState::kReadable;
        }
    } else {
        f.startWin32 = GetLastError();
        f.startRead = IsDeniedError(f.startWin32) ? ReadState::kDenied : ReadState::kFailed;
    }
    CloseHandle(proc);
    return f;
}

// 异常 what() 是窄字符，只留可打印 ASCII：这段细节要穿过管道交给父进程。
std::string AsciiDetail(const char* what) {
    std::string out;
    for (const char* p = what ? what : ""; p && *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 32 && c < 127) out.push_back(*p);
    }
    return out;
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

// 正则求值时的失败要就地记下：异常不许穿过 EnumWindows 那条回调边界
// （回调是被 user32 调的，那条栈上没有 C++ 的展开信息，异常会绕到调用栈外面去）。
struct RegexFault {
    bool hit = false;
    BlockedStatus status = BlockedStatus::kOk;
    std::string detail;
};

bool Matches(const Compiled& c, const WindowInfo& w, RegexFault* fault) {
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
        for (const std::wregex& re : c.titleRegexes) {
            try {
                if (std::regex_search(w.title, re)) { any = true; break; }
            } catch (const std::regex_error& e) {
                // MSVC 的正则库对回溯复杂度有一道内置上限（error_complexity），失控的模式
                // 会在这里被挡下来，而不是永远跑下去 —— 那是"能中断"的那一类。
                if (fault && !fault->hit) {
                    fault->hit = true;
                    fault->status = e.code() == std::regex_constants::error_complexity
                                        ? BlockedStatus::kRegexTooComplex
                                        : BlockedStatus::kRegexInvalid;
                    fault->detail = AsciiDetail(e.what());
                }
                any = false;
                break;
            }
        }
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
    RegexFault fault;                  // 正则被上限挡下时的记录（不让异常穿出回调）
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

    w.imagePath.clear();
    const WindowFacts facts = ProcessFactsOf(hwnd);
    w.imagePath = facts.path;
    w.processStartTicks = facts.startTicks;
    w.pathRead = facts.pathRead;
    w.startRead = facts.startRead;
    w.pathWin32 = facts.pathWin32;
    w.startWin32 = facts.startWin32;
    if (!w.imagePath.empty()) w.imageName = FileNameOf(w.imagePath);

    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        w.x = rect.left;
        w.y = rect.top;
        w.width = rect.right - rect.left;
        w.height = rect.bottom - rect.top;
        w.rectRead = ReadState::kReadable;
    } else {
        // 量不出来与「量到了一个零尺寸的窗口」是两件事，两者都不能写成同一个值：
        // 前者是这一次问答没有答案，后者是窗口自己的形状。查询层据此写字段级 unknown。
        w.rectWin32 = GetLastError();
        w.rectRead = ReadState::kFailed;
    }
    w.zOrder = thisOrder;
    w.iconic = IsIconic(hwnd) != FALSE;

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (w.width <= 0 || w.height <= 0) return TRUE;

    if (state->fault.hit) return TRUE;   // 已经报废的求值不必再往下数
    if (!Matches(*state->compiled, w, &state->fault)) return TRUE;
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

// ---------------------------------------------------------------------------
// 条件求值：枚举 + AND/OR 判定（含正则）。这一半没有中断点，所以要限期限时得整半交给
// 辅助进程（Worker.h 的 kTaskMatchWindows），在这里加"检查点"是假的期限。
// 结果只带原因码，不带文案 —— 这条函数在另一个进程里也要能跑。
// ---------------------------------------------------------------------------
MatchOutcome EnumerateMatches(const MatchRequest& req) {
    MatchOutcome outcome;
    Compiled c;
    c.hwnds = req.match.hwnds;
    c.pids = req.match.pids;
    for (const auto& s : req.match.processes) {
        std::wstring v = ToLowerPlain(s);
        if (v.find(L'.') == std::wstring::npos) v += L".exe";
        c.processes.push_back(std::move(v));
    }
    for (const auto& s : req.match.exePaths) c.exePaths.push_back(ToLowerPlain(s));
    c.titles = req.match.titles;
    c.titleContains = req.match.titleContains;
    for (const auto& s : req.match.classes) c.classes.push_back(ToLowerPlain(s));
    for (const auto& expr : req.match.titleRegexes) {
        try {
            c.titleRegexes.emplace_back(expr, std::regex_constants::ECMAScript);
        } catch (const std::regex_error& e) {
            // 语法在解析期已经挡过一遍；走到这里说明这台机器的标准库拒绝编译它。
            // 只把机器码与 ASCII 细节交回去，本地化文案由父进程拼。
            outcome.status = BlockedStatus::kRegexInvalid;
            outcome.detail = AsciiDetail(e.what());
            return outcome;
        }
    }
    c.onScreens = req.onScreens;

    // 求值过程中的 regex_error：MSVC 的正则库对回溯复杂度有一道内置上限
    // （error_complexity），所以失控的模式会以异常中断，而不是永远跑下去。
    // 这是"能中断"的那一类，照实换成稳定诊断；时间上限另由期限那条路保证（Worker.h）。
    CollectState state{&c, &outcome.hits, &outcome.iconic, 0};
    try {
        EnumWindows(CollectCallback, reinterpret_cast<LPARAM>(&state));
    } catch (...) {
        outcome.status = BlockedStatus::kInternal;
        outcome.detail = "unknown";
        outcome.hits.clear();
        outcome.iconic.clear();
        return outcome;
    }
    if (state.fault.hit) {
        // 正则被本机正则库的上限挡下：状态码与细节照实交回去，
        // 半套命中列表不能交回调用方 —— 那看起来像"就这些窗口"，实际是"数到一半就停了"。
        outcome.status = state.fault.status;
        outcome.detail = state.fault.detail;
        outcome.hits.clear();
        outcome.iconic.clear();
        return outcome;
    }
    std::sort(outcome.hits.begin(), outcome.hits.end(),
              [](const WindowInfo& a, const WindowInfo& b) { return a.zOrder < b.zOrder; });
    return outcome;
}

std::wstring MonitorLabelOf(const Options& opt) {
    if (!opt.monitor.given) return std::wstring();
    if (opt.monitor.all) return L"all";
    if (opt.monitor.ordinal == 0) return L"primary";
    return std::to_wstring(opt.monitor.ordinal);
}

// ---------------------------------------------------------------------------
// 选择策略：只对着已经拿到手的列表做决定，不碰窗口也不碰正则，所以永远在父进程里跑。
// ---------------------------------------------------------------------------
std::vector<WindowInfo> SelectFromHits(const Options& opt, const std::vector<WindowInfo>& hits,
                                       const std::vector<WindowInfo>& iconic,
                                       const std::wstring& monitorLabel,
                                       std::vector<Diagnostic>* errors) {
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
        case MultiMatch::kTopmost:
            return {hits.front()};   // 当前 Z 序最前的那一个（不是"最后创建"）
        case MultiMatch::kBottommost:
            return {hits.back()};    // 当前 Z 序最后的那一个
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
