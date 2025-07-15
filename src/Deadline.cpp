#include "Deadline.h"

#include <algorithm>
#include <string>

namespace ecapture {
namespace {

// QPC 的频率整机恒定，取一次缓存起来即可；拿不到（极老的虚拟化环境）时按 1 处理，
// 于是所有换算都退化成"每个计数就是一毫秒量级"，宁可不准确也不返回一个负数预算。
int64_t PerfFrequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER li{};
        if (!QueryPerformanceFrequency(&li) || li.QuadPart <= 0) return static_cast<int64_t>(1000);
        return li.QuadPart;
    }();
    return freq;
}

int64_t NowTicks() {
    LARGE_INTEGER li{};
    if (!QueryPerformanceCounter(&li)) return 0;
    return li.QuadPart;
}

int64_t MsToTicks(uint64_t ms) {
    const int64_t f = PerfFrequency();
    if (ms > static_cast<uint64_t>(0x7FFFFFFFFFFFFFFFll) / static_cast<uint64_t>(f)) {
        return 0x7FFFFFFFFFFFFFFFll;   // 换算会溢出：按"实际上永远不会耗尽"处理
    }
    return static_cast<int64_t>(ms) * f / 1000;
}

uint64_t TicksToMs(int64_t ticks) {
    if (ticks <= 0) return 0;
    const int64_t f = PerfFrequency();
    return static_cast<uint64_t>(ticks) * 1000ull / static_cast<uint64_t>(f);
}

}  // namespace

Deadline Deadline::FromTotalMs(uint64_t totalMs) {
    Deadline dl;
    if (totalMs == 0) return dl;   // 0 = 不限
    dl.start_ = NowTicks();
    dl.totalTicks_ = MsToTicks(totalMs);
    return dl;
}

uint64_t Deadline::TotalMs() const { return TicksToMs(totalTicks_); }

uint64_t Deadline::ElapsedMs() const {
    if (!Enabled()) return 0;
    return TicksToMs(NowTicks() - start_);
}

uint64_t Deadline::RemainingMs() const {
    if (!Enabled()) return kNoLimit;
    const int64_t left = totalTicks_ - (NowTicks() - start_);
    return TicksToMs(left > 0 ? left : 0);
}

bool Deadline::Spent() const {
    if (!Enabled()) return false;
    return RemainingMs() == 0;
}

uint32_t Deadline::ClampWait(uint32_t wantMs) const {
    if (!Enabled()) return wantMs;
    const uint64_t left = RemainingMs();
    if (left == 0) return 0;
    return static_cast<uint32_t>(std::min<uint64_t>(left, wantMs));
}

Diagnostic BudgetSpent(const Deadline& dl, const wchar_t* code, const wchar_t* stage,
                       const wchar_t* backend) {
    Diagnostic d;
    d.code = code;
    d.message = Msgf(L"cap.timeout", dl.TotalMs(), dl.ElapsedMs());
    d.option = L"--timeout-ms";
    d.value = std::to_wstring(dl.TotalMs());
    d.hint = Msg(L"cap.timeout_hint");
    d.backend = backend ? backend : std::wstring();
    d.stage = stage;
    return d;
}

}  // namespace ecapture
