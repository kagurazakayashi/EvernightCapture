#pragma once
// 隔离执行这条路上"每个时钟各管哪一段"的算式，集中在这里。
//
// 这里只有算式：不起线程、不碰句柄、不调任何系统等待，所以每一条都能用注入的数值逐条判
// （判的是函数本体，见 tests\isolation_state.cpp，不是测试里另抄的一份规则）。
// Worker.cpp 只负责把这些算式接到真时钟、真线程与真管道上。
//
// 四类期限，职责互不重叠，也不许互相改写：
//   1. 父进程那一份自动处理总预算（--timeout-ms，见 Deadline.h）：一次运行只有这一份，
//      每一步只拿"还剩多少"。本模块不读它也不重置它，只做换算。
//   2. 默认隔离调用上限 kIsolatedCallMs（Deadline.h）：只在**没有**显式预算时代替用户决定
//      "这一次最多等多久"。给了 --timeout-ms 就不再有第二道内置上限压它。
//   3. 启动/握手段 kHandshakeMs：等的是"我起的那个辅助进程有没有把一条任务交到我手上"。
//      父进程侧表现为连管的等待上限，辅助进程侧表现为"没人交任务就自己退出"的上限 ——
//      同一段含义、两个观察者、同一个常数。它不会缩短第 1 条：健康机器上远远用不完，
//      而它到点时报的是"机制故障"，不是"你的期限太短"。
//   4. 辅助进程的执行段 = 父进程随任务交下来的那一笔剩余预算（协议里已判过界，非 0 且有界）
//      + 交回宽限 kDeliverGraceMs。这一条只会**晚于**父进程自己放弃的那一刻（预算是交出任务
//      那一刻的剩余，而辅助进程从收到任务才开始数），所以用户显式接受的期限不可能被
//      辅助进程自己的时钟截断；它挡的是"父进程已经卡死、没人来收尸"的那种遗留。
// 另外两段有界的收尾宽限不在这里，它们是父进程自己的事：收尸等待与取消排干等待（Worker.cpp）。

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Deadline.h"
#include "WorkerProtocol.h"   // kMaxWorkerBudgetMs：交下去的那一笔预算的上限与公开期限上限同源

namespace ecapture {

// 启动/握手段的上限（父进程连管与辅助进程等任务共用这一个数，含义是同一段）。
inline constexpr uint32_t kHandshakeMs = 30000u;
// 执行段末尾的交回宽限：抵消父进程（QPC）与辅助进程（GetTickCount64）两套单调时钟的粒度差，
// 并给"已经算完、正在把应答写进管道"那几步留出收尾时间。它只延长兜底，不缩短任何预算。
inline constexpr uint32_t kDeliverGraceMs = 1000u;
// 辅助进程看门狗复查自己阶段的间隔上限。这不是期限，只是"多久看一眼阶段变了没有"：
// 没有它，一条 Sleep(整个上限) 会在阶段切换之后继续睡，最长把退出误延一整个上限。
inline constexpr uint32_t kWatchdogPollMs = 200u;

// 辅助进程的内部退出码。只用于诊断：父进程把它写进"机制故障"那条 hint 里给人看，
// 它不是对外契约的一部分，也不进任何 JSON 字段，随时可以改（见 Worker.h 第 2 条边界）。
inline constexpr int kHelperExitProtocol = 9;      // 没能交回任何东西：参数 / 管道 / 协议不成立
inline constexpr int kHelperExitNoTask = 10;       // 启动/握手段到点仍没有一条合法任务交到手
inline constexpr int kHelperExitOverBudget = 11;   // 执行段超过交下来的预算 + 交回宽限
// 这两个由**父进程**动手（父进程按自己的期限或收尾流程结束它），不是辅助进程自己的判断。
// 与上面几条分开才有诊断价值：看见它就知道"没人把消息送回来"这一路是谁先做的决定。
inline constexpr int kHelperExitReaped = 8;        // 父进程收尸时结束的

// ---------------------------------------------------------------------------
// 父进程侧的换算
// ---------------------------------------------------------------------------

// 把"这一次运行还剩的预算"换算成一次隔离调用实际能等多久：给了 --timeout-ms 就照剩余预算
// （用户明确要多少就给他多少，不再另加一道内置上限），没给就用内置上限。
// 定义放在 WorkerTiming.cpp，是为了让离线判据直接判这条换算本身，而不是判它的抄本。
Deadline IsolatedWaitFor(const Deadline& run);

// 父进程肯为"辅助进程连上管道"等多久：绝不超过这一笔交易还剩的预算，也不超过 kHandshakeMs。
// 把整份剩余预算压在连管上，会让一个根本没起来的辅助进程把用户的期限烧光，还能报成
// "期限耗尽"——那是把机制故障说成期限太短。
DWORD HandshakeWaitMs(const Deadline& wait);

// 连管等待到点之后，这一笔交易算哪一种失败：预算真的烧光了才是期限耗尽，
// 否则是"辅助进程没接上来"这条机制故障。两条给调用方的是不同的稳定码。
enum class HandshakeFault { kBudgetSpent, kHelperSilent };
HandshakeFault ClassifyHandshakeTimeout(const Deadline& wait);

// 随任务交下去的那一笔预算：非 0、有界，而且只是"还剩多少"的换算，不重新领一份总额。
uint32_t TaskBudgetMs(const Deadline& wait);

// ---------------------------------------------------------------------------
// 辅助进程侧的三段期限模型
// ---------------------------------------------------------------------------

// 辅助进程自己走到哪一段。段的划分依据是"这段时间里谁欠谁什么"：
//   kAwaitTask  —— 还没拿到任务。父进程欠我一条任务，这段由 kHandshakeMs 兜底。
//   kRunning    —— 任务已到手（正在算，或正在把应答写进管道）。这段由父进程交下来的预算兜底。
//   kDelivered  —— 应答已经交出去/交回收尾完毕。此后不再有自尽的期限，进程正自己退出。
enum class HelperPhase : uint32_t { kAwaitTask = 0, kRunning = 1, kDelivered = 2 };

// 三段各自的上限。taskBudgetMs 必须是**已经过协议判界**的那个值（DecodeTask 判过
// [1, kMaxWorkerBudgetMs]），本模块只按加法算，不再重复当它是外部输入。
struct HelperLimits {
    uint64_t handshakeMs = kHandshakeMs;
    uint64_t taskBudgetMs = 0;
    uint64_t deliverGraceMs = kDeliverGraceMs;
};

struct HelperVerdict {
    bool exitNow = false;   // 这一段到点了：辅助进程该当场结束自己（只结束自己）
    int exitCode = 0;       // 仅供诊断的内部退出码，exitNow 为真时才有意义
    uint32_t pollMs = 0;    // 下一次复查之前该睡多久（有界，且不超过本段剩余）
};

// elapsedInPhaseMs 是"进入当前这一段以来"流逝的毫秒数（不是进程启动以来）：
// 每段的计时起点由调用方在进入该段时重新锚一次，所以前一段花掉的时间不会侵占后一段，
// 而后一段也不会拿到一份新的预算 —— 它用的仍然是父进程交出任务那一刻的剩余。
HelperVerdict JudgeHelperWatchdog(HelperPhase phase, const HelperLimits& limits,
                                  uint64_t elapsedInPhaseMs);

}  // namespace ecapture
