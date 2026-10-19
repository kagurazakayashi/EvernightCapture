// 【平台层 · Linux】条件求值的"隔离"入口：本进程执行 + 期限前后核对。
//
// Windows 那一份把整步交给辅助进程（Worker），因为那里没有可中断点。Linux 本构建
// 还没有辅助进程协议（core/WorkerFraming 只有外层帧，载荷仍只在 Windows 的
// WorkerProtocol.h），所以这里在本进程里跑 EnumerateMatches，并在**开工前**与**完工后**
// 各核一次期限：
//   * 开工前已耗尽 -> 一个窗口都不枚举，交回 capture.frame_timeout；
//   * 完工后已耗尽 -> 这一批结果不交给调用方（半套命中列表看起来像"就这些窗口"）。
//
// 这条边界的代价写在文档里：正则回溯本身没有中断点，失控的模式只能靠正则库自己的复杂度
// 上限挡下；要在第 N 毫秒真的掐断，需要 Linux 辅助进程实现之后再接。这里不假装已经做到。

#include "Isolation.h"

#include <string>
#include <vector>

#include "Deadline.h"
#include "WindowMatch.h"

namespace ecapture {

bool IsolatedMatch(const MatchOptions& match, const std::vector<PhysRect>& onScreens,
                   const Deadline& dl, std::vector<WindowInfo>* hits,
                   std::vector<WindowInfo>* iconic, Diagnostic* err) {
    if (!hits || !iconic) {
        if (err) *err = BlockedToDiagnostic(BlockedStatus::kInternal, kNativeErrorNone,
                                             kNativeErrorNone, "null output", L"xcomposite",
                                             stages::kMatch);
        return false;
    }
    if (dl.Spent()) {
        if (err) *err = BudgetSpent(dl, codes::kFrameTimeout, stages::kMatch, L"xcomposite");
        return false;
    }

    MatchRequest request;
    request.match = match;
    request.onScreens = onScreens;
    const MatchOutcome outcome = EnumerateMatches(request);
    if (outcome.status != BlockedStatus::kOk) {
        if (err) {
            *err = BlockedToDiagnostic(outcome.status, kNativeErrorNone, kNativeErrorNone,
                                       outcome.detail, L"xcomposite", stages::kMatch);
        }
        return false;
    }
    if (dl.Spent()) {
        // 结果已经算出来了，但预算在算的过程中用光：不交半套，按超时失败。
        if (err) *err = BudgetSpent(dl, codes::kFrameTimeout, stages::kMatch, L"xcomposite");
        return false;
    }
    *hits = outcome.hits;
    *iconic = outcome.iconic;
    return true;
}

}  // namespace ecapture
