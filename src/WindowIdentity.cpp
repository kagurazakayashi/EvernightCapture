#include "WindowIdentity.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>

#include "Lang.h"
#include "WindowMatch.h"   // WindowInfo 的完整定义（MakeWindowIdentity 读它的字段）

// 目标身份复核：确认「手上这个窗口现在还是当初选中的那一扇」。
//
// 这一份只放**判据**：快照怎么做、四问（句柄/归属进程/进程创建时间/类名）按什么顺序判、
// 以及"问不出来"如何折成 unverifiable。四问的真机实现（IsWindow / GetWindowThreadProcessId /
// GetProcessTimes / GetClassNameW）搬到了 src/platform/windows/WinWindowSource.cpp 的
// SystemWindowQueryLayer()：共用层只对着 WindowQueryLayer 里的函数指针判，注入假答案就能把
// 「句柄被另一个进程复用」「PID 相同但那是另一个进程」这类现场逐条判完。
//
// 三条边界（完整版见 WindowIdentity.h 顶部）：
//   1. 复核只降低竞态窗口，不声称消除；判据与取帧不是原子的。
//   2. 查不到就是查不到：任何一问没给出答案就按 capture.target_unverifiable 拒绝取帧。
//   3. 身份变了不去找替代目标：用户针对旧对象给出的许可不转移给新对象。

namespace ecapture {
namespace {

std::wstring ToLowerPlain(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

template <typename T>
std::string ToAscii(T value) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
    return buf;
}

// 类名 / 标题里可能本来就有非 ASCII（窗口类名基本都是 ASCII，但第三方窗口不保证）。
// 这段细节要进 message 的 %1，而 message 是人看的：把无法显示的字符换成 '?' 而不是丢掉，
// 长度也压住，免得一条诊断被一扇标题几千字符的窗口顶爆。
std::string WideToAscii(const std::wstring& s, size_t limit = 96) {
    std::string out;
    for (size_t i = 0; i < s.size() && i < limit; ++i) {
        const wchar_t ch = s[i];
        out.push_back(ch < 32 || ch > 126 ? '?' : static_cast<char>(ch));
    }
    if (s.size() > limit) out += "...";
    return out;
}

std::string HwndText(uint64_t hwnd) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08llX", static_cast<unsigned long long>(hwnd));
    return buf;
}

}  // namespace

WindowIdentity MakeWindowIdentity(const WindowInfo& selected, const MatchOptions& match,
                                  bool monitorGiven) {
    WindowIdentity id;
    id.hwnd = selected.hwnd;
    id.pid = selected.pid;
    id.processStartTicks = selected.processStartTicks;
    id.className = selected.className;
    id.title = selected.title;
    // 看标题的那一类条件、以及"按屏过滤"那一条，都是会随时间变的：窗口改了标题、
    // 或者被人挪到另一块屏上，当初挑中它的理由就不再成立了。这类条件不逐字比快照，
    // 而是在 kFull 那一档里拿同一份条件重新求值一次（WindowIdentity.h 顶部）。
    id.selectionNeedsRecheck = !match.titles.empty() || !match.titleContains.empty() ||
                               !match.titleRegexes.empty() || monitorGiven;
    return id;
}

// ---------------------------------------------------------------------------
// 逐条复核。顺序按"便宜且严"排在前面：句柄有效性 -> 归属进程 -> 进程创建时间 -> 类名 ->
//（kFull 才有）重跑一次选择条件。任何一条不成立就立刻交出去，不再问后面那些 ——
// 后面的答案不会改变结论，而 kFull 那一问是这里最贵的一问，更不该白问。
// ---------------------------------------------------------------------------
IdentityVerdict CheckWindowIdentity(const WindowIdentity& id, const WindowQueryLayer& query,
                                    IdentityScope scope, IdentityFault* fault) {
    const auto fail = [&](IdentityVerdict verdict, std::string detail) {
        if (fault) {
            fault->verdict = verdict;
            fault->detail = std::move(detail);
        }
        return verdict;
    };

    const bool recheckSelection = scope == IdentityScope::kFull && id.selectionNeedsRecheck;
    if (!query.alive || !query.windowPid || !query.windowClass || !query.processStart ||
        (recheckSelection && !query.selectionStillMatches)) {
        return fail(IdentityVerdict::kUnverifiable, "query layer is incomplete");
    }

    if (!query.alive(id.hwnd)) {
        return fail(IdentityVerdict::kGone, "IsWindow(" + HwndText(id.hwnd) + ") = FALSE");
    }

    uint32_t pid = 0;
    if (!query.windowPid(id.hwnd, &pid)) {
        // IsWindow 说还在、却问不到归属进程：正被销毁的那一瞬间，或者查询层被换掉了。
        return fail(IdentityVerdict::kUnverifiable, "GetWindowThreadProcessId failed");
    }
    if (pid != id.pid) {
        return fail(IdentityVerdict::kChanged,
                    "pid " + ToAscii(id.pid) + " -> " + ToAscii(pid) + " (handle reused)");
    }

    // PID 会被系统复用，"同一个 PID"不等于"同一个进程" —— 这一条就是用来分开这两件事的。
    // 基线当时就没有（0）就整条跳过：那是一次没做出来的判定，不能反过来当成失败。
    if (id.processStartTicks != 0) {
        uint64_t ticks = 0;
        if (!query.processStart(pid, &ticks)) {
            return fail(IdentityVerdict::kUnverifiable,
                        "process " + ToAscii(pid) + " creation time unreadable");
        }
        if (ticks != id.processStartTicks) {
            return fail(IdentityVerdict::kChanged,
                        "process creation time of pid " + ToAscii(pid) + " is " + ToAscii(ticks) +
                            ", the selected process started at " + ToAscii(id.processStartTicks) +
                            " (pid reused)");
        }
    }

    std::wstring className;
    if (!query.windowClass(id.hwnd, &className)) {
        return fail(IdentityVerdict::kUnverifiable, "GetClassNameW failed");
    }
    // 忽略大小写：类名在注册时不区分大小写，而 --class 这条条件本来就是这么比的。
    if (ToLowerPlain(className) != ToLowerPlain(id.className)) {
        return fail(IdentityVerdict::kChanged,
                    "class '" + WideToAscii(id.className) + "' -> '" + WideToAscii(className) + "'");
    }

    if (!recheckSelection) return IdentityVerdict::kSame;

    // 易变属性（标题、窗口在哪块屏上）就在这里判：**问的是当初那条条件现在还成立吗**，
    // 而不是"标题有没有变"。应用刷新标题是正常现象，逐字比较会把每一次进度条式的标题变化
    // 都当成换了目标；而条件不再成立，才说明当初把它挑出来的理由已经不属于它了。
    bool matched = false;
    if (!query.selectionStillMatches(id.hwnd, &matched)) {
        return fail(IdentityVerdict::kUnverifiable,
                    "the original conditions could not be re-evaluated");
    }
    if (!matched) {
        return fail(IdentityVerdict::kChanged,
                    "hwnd " + HwndText(id.hwnd) +
                        " no longer satisfies the conditions it was selected by (title, or the "
                        "--monitor screen it was matched on)");
    }
    return IdentityVerdict::kSame;
}

Diagnostic IdentityDiagnostic(IdentityVerdict verdict, const std::string& detail, uint64_t hwnd) {
    std::wstring reason;
    reason.reserve(detail.size());
    for (char ch : detail) reason.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));

    Diagnostic d;
    // 与 images[].hwnd / 诊断里的 target 同一个写法（0x + 至少 8 位大写十六进制）
    wchar_t tag[24];
    std::swprintf(tag, std::size(tag), L"0x%08X", static_cast<unsigned>(hwnd));
    d.target = tag;
    d.stage = stages::kCapture;
    switch (verdict) {
        case IdentityVerdict::kGone:
            d.code = codes::kTargetGone;
            d.message = Msg(L"cap.target_gone");
            d.hint = Msg(L"cap.target_gone_hint");
            break;
        case IdentityVerdict::kChanged:
            d.code = codes::kTargetChanged;
            d.message = Msgf(L"cap.target_changed", reason);
            d.hint = Msg(L"cap.target_changed_hint");
            break;
        case IdentityVerdict::kUnverifiable:
            d.code = codes::kTargetUnverifiable;
            d.message = Msgf(L"cap.target_unverifiable", reason);
            d.hint = Msg(L"cap.target_unverifiable_hint");
            break;
        case IdentityVerdict::kSame:
        default:
            break;
    }
    return d;
}

bool VerifyWindowIdentity(const WindowIdentity& id, const WindowQueryLayer& query,
                          IdentityScope scope, Diagnostic* err) {
    IdentityFault fault;
    const IdentityVerdict verdict = CheckWindowIdentity(id, query, scope, &fault);
    if (verdict == IdentityVerdict::kSame) return true;
    if (err) *err = IdentityDiagnostic(verdict, fault.detail, id.hwnd);
    return false;
}

const char* IdentityVerdictName(IdentityVerdict verdict) {
    switch (verdict) {
        case IdentityVerdict::kGone: return "gone";
        case IdentityVerdict::kChanged: return "changed";
        case IdentityVerdict::kUnverifiable: return "unverifiable";
        case IdentityVerdict::kSame: break;
    }
    return "same";   // 返回字面量，不是临时对象的 c_str()
}

}  // namespace ecapture
