#include "Deadline.h"

#include <algorithm>
#include <climits>
#include <string>

namespace ecapture {
namespace {

// QPC 的频率整机恒定，取一次缓存起来即可；拿不到（极老的虚拟化环境）时按 1000 处理，
// 于是换算退化成"每个计数就是一毫秒"，宁可不准确也不返回一个负数预算。
int64_t PerfFrequency() {
    static const int64_t freq = [] {
        LARGE_INTEGER li{};
        if (!QueryPerformanceFrequency(&li) || li.QuadPart <= 0) return static_cast<int64_t>(1000);
        return li.QuadPart;
    }();
    return freq;
}

// 生产单调时钟：QPC 换算成毫秒。先除后乘拆开，避免 ticks*1000 溢出。
int64_t QpcNowMs() {
    LARGE_INTEGER li{};
    if (!QueryPerformanceCounter(&li)) return 0;
    const int64_t f = PerfFrequency();
    const int64_t t = li.QuadPart > 0 ? li.QuadPart : 0;
    return (t / f) * 1000 + ((t % f) * 1000) / f;
}

int64_t ClampTotalMs(uint64_t totalMs) {
    return totalMs > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(totalMs);
}

}  // namespace

Deadline Deadline::FromTotalMs(uint64_t totalMs) { return FromTotalMs(totalMs, QpcNowMs); }

Deadline Deadline::FromTotalMs(uint64_t totalMs, MonotonicNowMs now) {
    Deadline dl;
    if (totalMs == 0) return dl;   // 0 = 不限
    auto state = std::make_shared<State>();
    state->now = now ? std::move(now) : MonotonicNowMs(QpcNowMs);
    state->totalMs = ClampTotalMs(totalMs);   // 换算会溢出：按"实际上永远不会耗尽"处理
    state->start = state->now();
    dl.state_ = std::move(state);
    return dl;
}

int64_t Deadline::ActiveElapsedMs() const {
    if (!state_) return 0;
    const int64_t nowMs = state_->now();
    int64_t elapsed = nowMs - state_->start - state_->pausedMs;
    // 在世暂停的进行部分同样不算流逝。深度只在最外层记 pauseStart，内层进出不重复扣。
    if (state_->depth > 0) elapsed -= (nowMs - state_->pauseStart);
    return elapsed > 0 ? elapsed : 0;
}

uint64_t Deadline::TotalMs() const {
    return state_ && state_->totalMs > 0 ? static_cast<uint64_t>(state_->totalMs) : 0;
}

uint64_t Deadline::ElapsedMs() const {
    if (!Enabled()) return 0;
    return static_cast<uint64_t>(ActiveElapsedMs());
}

uint64_t Deadline::RemainingMs() const {
    if (!Enabled()) return kNoLimit;
    const int64_t left = state_->totalMs - ActiveElapsedMs();
    return left > 0 ? static_cast<uint64_t>(left) : 0;
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

Deadline::PauseScope Deadline::PauseForHumanWait() const { return PauseScope(state_); }

void Deadline::PauseScope::Start() {
    if (!state_) return;
    // 深度由 0 变 1 才记进入时刻：外层已在世时，内层只是"再挂一个引用"，不另起一段。
    if (state_->depth++ == 0) state_->pauseStart = state_->now();
}

void Deadline::PauseScope::End() {
    if (!state_) return;
    // 深度归零才结算这一段：嵌套作用域（外层已把窗口盖住时又进一层）不重复扣，
    // 时间也不可能凭空多出。异常退栈走的是同一个析构，恢复路径与正常路径同源。
    if (state_->depth > 0 && --state_->depth == 0) {
        state_->pausedMs += state_->now() - state_->pauseStart;
    }
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
