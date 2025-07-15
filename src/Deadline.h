#pragma once
// 自动处理阶段的执行期限（--timeout-ms）。
//
// 一次运行只建立**一个** Deadline：匹配、后端重试、取帧等待、编码、提交共用同一份剩余预算，
// 每一步都只拿到"还剩多少"，而不是每次重新领一份完整预算 —— 否则四个后端各等 2 秒就等于
// 期限根本没生效，旧实现里那条硬编码的 2000 ms 等帧上限正是这么被反复重置的。
//
// 时钟必须是单调的：GetTickCount64 只有 ~15.6 ms 粒度，而 QueryPerformanceCounter 不受系统
// 显示语言、时区、NTP 校时影响。这里用后者，且把频率取一次缓存起来。
//
// 两条边界（写在文档里，不藏在代码注释里）：
//   * 人工确认的等待**不占**这份预算（见 --consent-timeout-ms），确认框关闭后的动画缓冲也算在
//     人工那一级，绝不为了赶期限而省掉。
//   * 本结构只负责"还剩多少"。真正能在中途停下来的只有：辅助进程里那些能被结束的调用、
//     以及本来就有等待上限的消息泵 / 等帧循环。没有中断点的系统调用（磁盘写、写往被人堵住
//     的标准输出、WinRT 编码器内部的等待）只能在**开工之前**判一次、完工之后再核一次。

#include <cstdint>
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

class Deadline {
public:
    // 不限（--timeout-ms 0，也是默认）：各步骤沿用自己的等待上限。
    Deadline() = default;
    static Deadline FromTotalMs(uint64_t totalMs);   // 0 = 不限

    bool Enabled() const { return totalTicks_ != 0; }
    uint64_t TotalMs() const;
    uint64_t ElapsedMs() const;

    // 剩余预算（毫秒）。未启用时返回 kNoLimit，调用方据此走"不限"那条分支，
    // 而不是拿一个很大的数字去和 32 位的等待上限做减法。
    uint64_t RemainingMs() const;
    bool Spent() const;

    // 把一个"本来要等 wantMs"的等待压到剩余预算之内。未启用时原样返回。
    uint32_t ClampWait(uint32_t wantMs) const;

    // 未启用预算时的"无限"标记：给等待函数换成 INFINITE，别当成 0（那是"不等待"）。
    static constexpr uint64_t kNoLimit = 0xFFFFFFFFFFFFFFFFull;

private:
    int64_t start_ = 0;
    int64_t totalTicks_ = 0;
};

// 期限耗尽的统一诊断：code 按阶段选（匹配 / 取帧用 match.timeout、capture.timeout，
// 写文件与写标准输出用 io.timeout），message 里带上预算与实际耗时，调用方据此判断
// "是加大预算再来"还是"换一条能中断的路径"。target 与 backend 由调用点补。
Diagnostic BudgetSpent(const Deadline& dl, const wchar_t* code, const wchar_t* stage,
                       const wchar_t* backend);

// 把 Deadline 换算成等待毫秒数（未启用 = INFINITE）。
inline DWORD WaitTimeout(const Deadline& dl) {
    if (!dl.Enabled()) return INFINITE;
    const uint64_t left = dl.RemainingMs();
    return left > 0xFFFFFFFFull ? INFINITE : static_cast<DWORD>(left);
}

}  // namespace ecapture
