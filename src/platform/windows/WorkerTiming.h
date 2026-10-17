#pragma once
// 【平台层 · Windows】隔离调用的父进程侧期限换算（依赖上面那套管道时序；换算结果都是毫秒时长）。
// 三段期限模型本体、握手/交回宽限常数与辅助进程的内部退出码都在 core/HelperTiming.h ——
// 那一份是纯算式，Linux 侧接线的就是它。这里只留**换算成 Win32 等待参数**的那些函数，
// 它们的返回值（DWORD / INFINITE 语义）本来就属于平台侧。
//
// 四类期限，职责互不重叠，也不许互相改写（本体见 core/HelperTiming.h 与 Deadline.h）：
//   1. 父进程那一份自动处理总预算（--timeout-ms，见 Deadline.h）：一次运行只有这一份，
//      每一步只拿"还剩多少"。本模块不读它也不重置它，只做换算。
//   2. 默认隔离调用上限 kIsolatedCallMs（Deadline.h）：只在**没有**显式预算时代替用户决定
//      "这一次最多等多久"。给了 --timeout-ms 就不再有第二道内置上限压它。
//   3. 启动/握手段 kHandshakeMs：父进程侧表现为连管的等待上限，辅助进程侧表现为
//      "没人交任务就自己退出"的上限 —— 同一段含义、两个观察者、同一个常数。
//   4. 辅助进程的执行段 = 随任务交下来的那一笔剩余预算 + 交回宽限 kDeliverGraceMs。
// 另外两段有界的收尾宽限不在这里，它们是父进程自己的事：收尸等待与取消排干等待（Worker.cpp）。

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Deadline.h"
#include "core/HelperTiming.h"      // 三段期限模型本体（kHandshakeMs 等常数也在那一份里）
#include "core/WorkerFraming.h"     // kMaxWorkerBudgetMs：交下去的那一笔预算的上限与公开期限上限同源

namespace ecapture {

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
// 交下去的是**时长**（毫秒），不是任何一个进程自己的时钟读数 —— 两个时钟域之间
// 永远不做绝对值相减（见 core/HelperTiming.h 第 4 条）。
uint32_t TaskBudgetMs(const Deadline& wait);

}  // namespace ecapture
