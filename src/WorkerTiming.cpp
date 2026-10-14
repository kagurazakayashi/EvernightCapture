#include "WorkerTiming.h"

#include <algorithm>
#include <cstdint>

namespace ecapture {

// 辅助进程自己的三段期限算式（JudgeHelperWatchdog 与 SaturatingAdd 那组帮手）已经搬到
// core/HelperTiming.cpp —— 两个平台接的就是同一个函数本体，这里不再重复一份。

Deadline IsolatedWaitFor(const Deadline& run) {
    // 给了 --timeout-ms 就照剩余预算等（用户明确要多少就给他多少，这里不再另加一道内置上限）；
    // 没给就用内置上限 —— 这条上限顶替的正是过去被忽略的那个等待参数。
    // 复制件指向同一份预算（Deadline 的 shared_ptr 约定），所以这一步不会重新领一份总额。
    return run.Enabled() ? run : Deadline::FromTotalMs(kIsolatedCallMs);
}

DWORD HandshakeWaitMs(const Deadline& wait) {
    // 未启用预算的调用方在这里也不退化成 INFINITE：隔离调用一律先经 IsolatedWaitFor 领过预算，
    // 真拿到一个"不限"就说明上游被改坏了，那就照握手段上限等，宁可报机制故障也不无限等下去。
    if (!wait.Enabled()) return kHandshakeMs;
    const uint64_t left = wait.RemainingMs();
    if (left >= kHandshakeMs) return kHandshakeMs;
    return static_cast<DWORD>(left);   // left 已判过 ≤ kHandshakeMs，落在 DWORD 之内
}

HandshakeFault ClassifyHandshakeTimeout(const Deadline& wait) {
    // 同一次"连管没等到"有两种完全不同的原因：预算真的花完了（调用方该考虑加大 --timeout-ms），
    // 与辅助进程压根没接上来（调用方该看的是权限、策略、杀软拦截）。混成一条码就会把后者
    // 教成"再等久一点"，而再等久一点永远等不来。
    return wait.Spent() ? HandshakeFault::kBudgetSpent : HandshakeFault::kHelperSilent;
}

uint32_t TaskBudgetMs(const Deadline& wait) {
    if (!wait.Enabled()) return static_cast<uint32_t>(kIsolatedCallMs);
    const uint64_t left = wait.RemainingMs();
    if (left == 0) return 1;   // 协议里 0 不合法；这一笔在连管那一步就会判成期限耗尽
    if (left > worker::kMaxWorkerBudgetMs) return worker::kMaxWorkerBudgetMs;
    return static_cast<uint32_t>(left);
}

}  // namespace ecapture
