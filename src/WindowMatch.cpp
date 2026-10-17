#include "WindowMatch.h"

#include <algorithm>
#include <cwctype>
#include <regex>
#include <string>
#include <vector>

#include "Lang.h"
#include "ScreenIdentity.h"   // MonitorSelectorLabel：选择器标签只写一处
#include "WindowMatchInternal.h"
#include "WindowSource.h"     // EnumerateWindowCandidates：平台层只枚举，匹配语义留在这一层

namespace ecapture {
namespace {

std::wstring ToLowerPlain(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
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

// CompiledConditions 与 RegexFault 这两份形状住在 WindowMatchInternal.h：离线判据要直接
// 对着下面那两个函数注入假候选，而不是另抄一套匹配算法。

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

bool CompileConditions(const MatchRequest& req, CompiledConditions* out,
                       BlockedStatus* status, std::string* detail) {
    CompiledConditions c;
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
            // 语法就是在这里判的：解析层（CliOptions.cpp）不构造正则，这一步跑在受约束的
            // 匹配执行层里——有期限、要隔离时还在辅助进程，慢与拒都由那一道边界兜住。
            // 细节带上 regex_error 的机器码（ASCII），本地化文案由父进程拼。
            *status = BlockedStatus::kRegexInvalid;
            *detail = "regex_error code=" + std::to_string(static_cast<int>(e.code())) + " " +
                      AsciiDetail(e.what());
            return false;
        }
    }
    c.onScreens = req.onScreens;
    *out = std::move(c);
    return true;
}

// 每一类条件**各判各的**：跨类是 AND，同类多个取值是 OR。类与类之间不共享中间结果 ——
// 早先这里复用一个 bool any，前一类（hwnd/pid/process/exe/title/title-contains）命中之后
// 那个真值会一直留到正则那一段，于是"全部正则都不匹配"仍被前面的命中放行。
// 一条不成立的 --title-regex 因此在选择路径上形同消失，--list / --inspect / 截图 /
// 身份复核全都走这一份判据，所以修复只改这里。
bool MatchesWindow(const CompiledConditions& c, const WindowInfo& w, RegexFault* fault) {
    if (!c.hwnds.empty() &&
        std::find(c.hwnds.begin(), c.hwnds.end(), w.hwnd) == c.hwnds.end()) return false;
    if (!c.pids.empty() &&
        std::find(c.pids.begin(), c.pids.end(), w.pid) == c.pids.end()) return false;

    // 这三项是同类比较的写法（映像名/完整路径/类名忽略大小写），与"上一类有没有命中"无关。
    const std::wstring imageLower = ToLowerPlain(w.imageName);
    const std::wstring pathLower = ToLowerPlain(w.imagePath);
    const std::wstring classLower = ToLowerPlain(w.className);

    if (!c.processes.empty() &&
        std::find(c.processes.begin(), c.processes.end(), imageLower) == c.processes.end()) {
        return false;
    }
    if (!c.exePaths.empty() &&
        std::find(c.exePaths.begin(), c.exePaths.end(), pathLower) == c.exePaths.end()) {
        return false;
    }
    // 标题是用户自己写的那一串：精确与子串两条都按原样比，不做大小写折叠。
    if (!c.titles.empty() &&
        std::find(c.titles.begin(), c.titles.end(), w.title) == c.titles.end()) return false;
    const auto containsOne = [&](const std::wstring& needle) {
        return w.title.find(needle) != std::wstring::npos;
    };
    if (!c.titleContains.empty() &&
        !std::any_of(c.titleContains.begin(), c.titleContains.end(), containsOne)) return false;
    if (!c.titleRegexes.empty()) {
        bool anyRegexHit = false;
        for (const std::wregex& re : c.titleRegexes) {
            try {
                if (std::regex_search(w.title, re)) { anyRegexHit = true; break; }
            } catch (const std::regex_error& e) {
                // MSVC 的正则库对回溯复杂度有一道内置上限（error_complexity），失控的模式
                // 会在这里被挡下来，而不是永远跑下去 —— 那是"能中断"的那一类。
                // 这一类当场按"不成立"处理，同时把原因记下来让整次求值作废。
                if (fault && !fault->hit) {
                    fault->hit = true;
                    fault->status = e.code() == std::regex_constants::error_complexity
                                        ? BlockedStatus::kRegexTooComplex
                                        : BlockedStatus::kRegexInvalid;
                    fault->detail = AsciiDetail(e.what());
                }
                break;
            }
        }
        if (!anyRegexHit) return false;
    }
    if (!c.classes.empty() &&
        std::find(c.classes.begin(), c.classes.end(), classLower) == c.classes.end()) return false;
    return true;
}

std::wstring DescribeWindow(const WindowInfo& w) {
    wchar_t buf[160];
    swprintf(buf, 160, L"hwnd=0x%08X pid=%lu %dx%d+%d+%d", static_cast<unsigned>(w.hwnd),
             static_cast<unsigned long>(w.pid), w.width, w.height, w.x, w.y);
    std::wstring s = buf;
    s += L" class=" + w.className;
    if (!w.title.empty()) s += L" title=" + w.title;
    return s;
}

std::wstring MonitorLabelOf(const Options& opt) {
    if (!opt.monitor.given) return std::wstring();
    // 标签的写法只有一份（ScreenIdentity.h）：编号、主屏、all，外加两条标识写法
    // （device:… / id:…）。hint 里、-v 的 input 段里、窗口查询那份回显里都读这一条，
    // 免得"回显写的选择器"与"实际用的选择器"两处各拼一遍拼出两种形状。
    return MonitorSelectorLabel(opt.monitor);
}

const wchar_t* ReadStateName(ReadState state) {
    switch (state) {
        case ReadState::kReadable: return L"readable";
        case ReadState::kDenied: return L"denied";
        case ReadState::kFailed: return L"failed";
    }
    return L"failed";   // 没登记过的取值不当成「能读」：宁可说问不出来
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
