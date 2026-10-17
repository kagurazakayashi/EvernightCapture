// 条件求值那一步的运行时入口：枚举 + AND/OR 判定（含正则）。
//
// 为什么与 src/WindowMatch.cpp 分开：判据本体（CompileConditions / MatchesWindow /
// SelectFromHits / 渲染）是纯函数，两个平台链同一份；而这一步要真正枚举本机的顶层窗口
// （WindowSource.h 的平台入口），Linux 的窗口后端还没接进来。拆开之后，
// windows_state.cpp 那批离线判据只链纯函数那一份，不需要一个平台窗口后端才能编译。
//
// 这一半没有中断点，所以要限期限时得整半交给辅助进程（Isolation.h 的 IsolatedMatch）——
// 在这里加"检查点"是假的期限。结果只带原因码，不带文案：这条函数在另一个进程里也要能跑。
//
// 分工：平台层（EnumerateWindowCandidates）只把顶层窗口的事实问出来；可见性策略、
// --monitor 限缩、匹配语义与最小化分流全在这一层。于是"列出来的窗口"与"截图会选中的窗口"
// 不可能各按一套规则算。

#include <algorithm>
#include <string>
#include <vector>

#include "WindowMatch.h"
#include "WindowMatchInternal.h"   // CompileConditions / MatchesWindow（与离线判据共用同一份）
#include "WindowSource.h"

namespace ecapture {
namespace {

// 窗口矩形与所选屏有重叠即算命中，所以跨屏窗口在两块屏上都能找得到。
// 用完整窗口矩形（含 DWM 那圈透明边）而不是扩展边框：这里要的是"人把窗口放在哪"。
bool OnAnyScreen(const CompiledConditions& c, const WindowInfo& w) {
    if (c.onScreens.empty()) return true;
    const PhysRect r{w.x, w.y, w.x + w.width, w.y + w.height};
    for (const PhysRect& m : c.onScreens) {
        if (RectOverlaps(r, m)) return true;   // 判据与这条改动之前逐字相同，见 core/Geometry.h
    }
    return false;
}

}  // namespace

MatchOutcome EnumerateMatches(const MatchRequest& req) {
    MatchOutcome outcome;
    CompiledConditions c;
    // 编译一次在枚举前（含 --title-regex 那几条）：避免每个窗口重复构造正则。
    // 判据本体是 MatchesWindow，与离线判据共用同一份，这里不另立第二套匹配规则。
    BlockedStatus compileStatus = BlockedStatus::kOk;
    if (!CompileConditions(req, &c, &compileStatus, &outcome.detail)) {
        outcome.status = compileStatus;
        return outcome;
    }

    std::vector<WindowInfo> candidates;
    BlockedStatus enumStatus = BlockedStatus::kOk;
    std::string enumDetail;
    if (!EnumerateWindowCandidates(&candidates, &enumStatus, &enumDetail)) {
        outcome.status = enumStatus == BlockedStatus::kOk ? BlockedStatus::kInternal : enumStatus;
        outcome.detail = enumDetail;
        return outcome;
    }

    // 求值过程中的 regex_error：MSVC 的正则库对回溯复杂度有一道内置上限
    // （error_complexity），所以失控的模式会以异常中断，而不是永远跑下去。
    // 这是"能中断"的那一类，照实换成稳定诊断；时间上限另由期限那条路保证（Isolation.h）。
    RegexFault fault;
    try {
        for (const WindowInfo& w : candidates) {
            if (fault.hit) break;   // 已经报废的求值不必再往下数
            // 旧行为：不可见与零尺寸的顶层窗口根本不进候选；最小化的仍进（供 --list=all 与 hint）。
            if (!w.visible) continue;
            if (w.width <= 0 || w.height <= 0) continue;
            if (!MatchesWindow(c, w, &fault)) continue;
            // 最小化的窗口只为 hint 收集，屏幕限制只管真正的目标
            if (!w.iconic && !OnAnyScreen(c, w)) continue;
            if (w.iconic) outcome.iconic.push_back(w);
            else outcome.hits.push_back(w);
        }
    } catch (...) {
        outcome.status = BlockedStatus::kInternal;
        outcome.detail = "unknown";
        outcome.hits.clear();
        outcome.iconic.clear();
        return outcome;
    }
    if (fault.hit) {
        // 正则被本机正则库的上限挡下：状态码与细节照实交回去，
        // 半套命中列表不能交回调用方 —— 那看起来像"就这些窗口"，实际是"数到一半就停了"。
        outcome.status = fault.status;
        outcome.detail = fault.detail;
        outcome.hits.clear();
        outcome.iconic.clear();
        return outcome;
    }
    std::sort(outcome.hits.begin(), outcome.hits.end(),
              [](const WindowInfo& a, const WindowInfo& b) { return a.zOrder < b.zOrder; });
    return outcome;
}

}  // namespace ecapture
