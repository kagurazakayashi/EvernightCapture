#pragma once
// 自动处理阶段的执行期限（--timeout-ms）。
//
// 一次运行只建立**一个** Deadline：匹配、后端重试、取帧等待、编码、提交共用同一份剩余预算，
// 每一步都只拿到"还剩多少"，而不是每次重新领一份完整预算 —— 否则四个后端各等 2 秒就等于
// 期限根本没生效，旧实现里那条硬编码的 2000 ms 等帧上限正是这么被反复重置的。
//
// 时钟必须是单调的：GetTickCount64 只有 ~15.6 ms 粒度，而 QueryPerformanceCounter 不受系统
// 显示语言、时区、NTP 校时影响。生产换算取自 QPC；离线判据可注入假时钟踩精确毫秒数。
//
// 预算的本体放在一份共享状态里（shared_ptr）：Deadline 被值复制进查询闭包、隔离调用或
// lambda 之后，**仍然指向同一份预算** —— 复制件看不见暂停、各自独立流逝的计时副本正是这一
// 版要消灭的缺陷。所有读写都发生在单线程流水线上（与 ConsentGate 同一约定）；共享只是
// 为了让"同一份预算的不同引用"一致，不是跨线程无锁计时的许可。
//
// 人工确认的等待**不占**这份预算：PauseForHumanWait() 建立一个 RAII 暂停作用域，覆盖真人
// 等待与确认框关闭后的动画缓冲（见 Consent.cpp 的 kDialogSettleMs）。语义钉死四条：
//   * 嵌套暂停只按最外层扣一次（深度计数，内层进出不动账）；
//   * 恢复后不会凭空多出预算 —— 暂停只是**不扣**，不是**回填**；
//   * 弹框之前就已耗尽的预算不会借一次确认复活；
//   * --consent-timeout-ms 走它自己的 GetTickCount64 秒表（RunMonitoredDialog），暂停冻结
//     的是这份自动预算的流逝读数，冻结不了人工那一级计时。
//
// 剩下的一条边界（写在文档里，不藏在代码注释里）：
//   * 本结构只负责"还剩多少"。真正能在中途停下来的只有：辅助进程里那些能被结束的调用、
//     以及本来就有等待上限的消息泵 / 等帧循环。没有中断点的系统调用（磁盘写、写往被人堵住
//     的标准输出、WinRT 编码器内部的等待）只能在**开工之前**判一次、完工之后再核一次。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CliOptions.h"

namespace ecapture {

// 没有 --timeout-ms 时，一次隔离调用最多等多久。这条内置上限顶替的就是过去被 PrintWindow
// 忽略掉的那个等待参数：给了 --timeout-ms 就按剩余预算等，没给也绝不无限等（见 Worker.h）。
inline constexpr uint64_t kIsolatedCallMs = 5000;

// 单调毫秒时钟：给出"从某一起点算来"单调不减的毫秒数，只用于差值。
// 生产用 QueryPerformanceCounter 换算；离线判据注入假时钟，逐毫秒踩出确定的预算算术。
using MonotonicNowMs = std::function<int64_t()>;

class Deadline {
private:
    // 一份预算的全部可变状态（完整定义在类尾）。单线程流水线内读写；shared_ptr 让
    // 复制出的 Deadline 引用同一份。
    struct State;

public:
    // 不限（--timeout-ms 0，也是默认）：各步骤沿用自己的等待上限。
    Deadline() = default;
    static Deadline FromTotalMs(uint64_t totalMs);   // 0 = 不限（生产时钟）
    // 注入时钟的重载只供离线判据使用：生产调用点一律走上面那个单参版本。
    static Deadline FromTotalMs(uint64_t totalMs, MonotonicNowMs now);

    bool Enabled() const { return state_ != nullptr; }
    uint64_t TotalMs() const;
    // 已经流逝的**自动处理**时间：人工确认暂停的区间不计入。
    uint64_t ElapsedMs() const;

    // 剩余预算（毫秒）。未启用时返回 kNoLimit，调用方据此走"不限"那条分支，
    // 而不是拿一个很大的数字去和 32 位的等待上限做减法。
    uint64_t RemainingMs() const;
    bool Spent() const;

    // 把一个"本来要等 wantMs"的等待压到剩余预算之内。未启用时原样返回。
    uint32_t ClampWait(uint32_t wantMs) const;

    // 未启用预算时的"无限"标记：给等待函数换成 INFINITE，别当成 0（那是"不等待"）。
    static constexpr uint64_t kNoLimit = 0xFFFFFFFFFFFFFFFFull;

    // 自动预算的 RAII 暂停作用域：构造即开始暂停，析构（正常离开、return、异常退栈）即
    // 恢复。作用域存活期间，同一份预算的所有引用（含被复制进闭包的那些）读到的剩余时间
    // 冻结在进入暂停的那一刻；嵌套作用域只由最外层进出账一次。对未启用的预算是惰性对象。
    class PauseScope {
    public:
        PauseScope(const PauseScope&) = delete;
        PauseScope& operator=(const PauseScope&) = delete;
        PauseScope(PauseScope&&) noexcept = default;
        PauseScope& operator=(PauseScope&& other) noexcept {
            if (this != &other) {
                End();
                state_ = std::move(other.state_);
            }
            return *this;
        }
        ~PauseScope() { End(); }

    private:
        friend class Deadline;
        explicit PauseScope(std::shared_ptr<State> state) : state_(std::move(state)) { Start(); }
        void Start();
        void End();
        std::shared_ptr<State> state_;   // 空 = 惰性（预算未启用）
    };

    // 只在**真的在等人**的区间调用（弹确认框，含同意后的关闭动画缓冲）；--yes 直通或
    // 复用已有许可时调用方根本没走到这里，不许无故补时间。
    [[nodiscard]] PauseScope PauseForHumanWait() const;

private:
    // 一份预算的全部可变状态。
    struct State {
        MonotonicNowMs now;
        int64_t start = 0;
        int64_t totalMs = 0;
        int64_t pausedMs = 0;     // 已结算（暂停作用域已退出）的累计暂停
        int depth = 0;            // 当前在世的暂停作用域嵌套深度
        int64_t pauseStart = 0;   // 最外层暂停进入时刻（depth 由 0 变 1 时记下）
    };

    // 流逝的自动处理毫秒：now - start - 已结算暂停 - 在世暂停的进行部分。
    int64_t ActiveElapsedMs() const;
    std::shared_ptr<State> state_;
};

// 期限耗尽的统一诊断：code 按阶段选（匹配 / 取帧用 match.timeout、capture.timeout，
// 写文件与写标准输出用 io.timeout），message 里带上预算与实际耗时（人工确认暂停不计入），
// 调用方据此判断"是加大预算再来"还是"换一条能中断的路径"。target 与 backend 由调用点补。
Diagnostic BudgetSpent(const Deadline& dl, const wchar_t* code, const wchar_t* stage,
                       const wchar_t* backend);

// 把 Deadline 换算成等待毫秒数（未启用 = INFINITE）。
inline DWORD WaitTimeout(const Deadline& dl) {
    if (!dl.Enabled()) return INFINITE;
    const uint64_t left = dl.RemainingMs();
    return left > 0xFFFFFFFFull ? INFINITE : static_cast<DWORD>(left);
}

}  // namespace ecapture
