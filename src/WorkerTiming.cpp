#include "WorkerTiming.h"

#include <algorithm>
#include <cstdint>

namespace ecapture {
namespace {

uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

// 本段还剩多久：bound - elapsed，负数一律按 0 处理（那是"已经到点"，不是绕回成一个巨大的数）。
uint64_t RemainingIn(uint64_t boundMs, uint64_t elapsedMs) {
    return elapsedMs >= boundMs ? 0 : boundMs - elapsedMs;
}

// 下一次复查之前睡多久：绝不超过本段的剩余，也绝不超过 kWatchdogPollMs。
uint32_t PollSlice(uint64_t remainingMs) {
    return remainingMs > kWatchdogPollMs ? kWatchdogPollMs : static_cast<uint32_t>(remainingMs);
}

}  // namespace

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

HelperVerdict JudgeHelperWatchdog(HelperPhase phase, const HelperLimits& limits,
                                  uint64_t elapsedInPhaseMs) {
    HelperVerdict v;
    switch (phase) {
        case HelperPhase::kAwaitTask: {
            const uint64_t left = RemainingIn(limits.handshakeMs, elapsedInPhaseMs);
            if (left == 0) {
                v.exitNow = true;
                v.exitCode = kHelperExitNoTask;
            } else {
                v.pollMs = PollSlice(left);
            }
            return v;
        }
        case HelperPhase::kRunning: {
            // 执行段的期限 = 父进程交出这条任务时还剩的预算 + 交回宽限。
            // 这一段与第 1 类时钟（父进程总预算）不是竞争关系：辅助进程从"收到任务"才开始数，
            // 而那个数取的是更早一刻的剩余，所以它只会晚于父进程自己放弃的那一刻。
            const uint64_t bound = SaturatingAdd(limits.taskBudgetMs, limits.deliverGraceMs);
            const uint64_t left = RemainingIn(bound, elapsedInPhaseMs);
            if (left == 0) {
                v.exitNow = true;
                v.exitCode = kHelperExitOverBudget;
            } else {
                v.pollMs = PollSlice(left);
            }
            return v;
        }
        case HelperPhase::kDelivered:
        default:
            // 应答已经交出去：进程正在自己退出，这里不再挂着第四个"迟早要把谁杀掉"的时钟。
            // pollMs 留 0，调用方据此收线程（见 Worker.cpp 的看门狗循环）。
            return v;
    }
}

}  // namespace ecapture
