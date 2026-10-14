#include "HelperTiming.h"

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
            // pollMs 留 0，调用方据此收线程（看门狗循环见各平台的辅助进程入口）。
            return v;
    }
}

}  // namespace ecapture
