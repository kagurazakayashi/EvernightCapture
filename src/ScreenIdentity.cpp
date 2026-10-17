// 屏幕身份的问答与判据。
//
// 判据本体（SelectScreenCandidates / CompareScreenIdentity / RecheckScreenSampling /
// MonitorSelectorLabel / ScreenIdEquals）是纯函数，留在这份共用文件里；碰 Win32 的那两半
//（EnumScreenCandidates 与 SelectScreenCandidatesOf）搬到了
// src/platform/windows/WinScreenSource.cpp —— 共用层只 include 平台无关的声明。
//
// 本文件不取一个像素、不弹确认框、不写文件，也**绝不**改任何显示设置（想"看清这块屏是不是
// 转过的"就去调 SetDisplayConfig，等于为了读答案把考卷改了）。

#include "ScreenIdentity.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "Lang.h"

namespace ecapture {
namespace {

// 一条选择器诊断：稳定的 code + 把人引向 --screens 的那句 hint + 候选一览。
// listed 给了就只列那几条并用它的条数，没给就列本机全部屏幕：多匹配那条要说的是
// "命中了这几块"，拿本机总数填进去会把它说成"本机有几块屏"。
Diagnostic SelectorError(const wchar_t* code, const wchar_t* messageKey, const wchar_t* hintKey,
                         const MonitorSelector& sel, const std::vector<ScreenCandidate>& all,
                         const std::vector<size_t>* listed) {
    Diagnostic d;
    d.code = code;
    d.message = Msg(messageKey);
    d.option = L"--monitor";
    d.value = sel.written.empty() ? MonitorSelectorLabel(sel) : sel.written;
    std::vector<ScreenInfo> infos;
    if (listed) {
        for (const size_t i : *listed) infos.push_back(all[i].screen);
    } else {
        infos.reserve(all.size());
        for (const ScreenCandidate& c : all) infos.push_back(c.screen);
    }
    d.hint = Msgf(hintKey, infos.size(), BriefScreenList(infos));
    d.stage = stages::kMatch;
    return d;
}

// 矩形/编号/主屏归属有没有变（判据与 ScreenMatch.h 的 CompareScreen 同源）。
bool SameGeometry(const ScreenInfo& a, const ScreenInfo& b) {
    return a.bounds.left == b.bounds.left && a.bounds.top == b.bounds.top &&
           a.bounds.right == b.bounds.right && a.bounds.bottom == b.bounds.bottom &&
           a.ordinal == b.ordinal && a.primary == b.primary;
}

bool PathReadable(const ScreenCandidate& c) {
    return c.facts.monitorPathQ.read == ReadState::kReadable && !c.facts.monitorPath.empty();
}

}  // namespace

bool ScreenIdEquals(const std::wstring& a, const std::wstring& b) {
    // 逐字符不区分大小写，与 Windows 自己看设备名的办法一致（区域敏感的比较规则会把
    // 土耳其语的 i 之类写成另一个名字，而"是不是同一个设备"不该由那种规则决定）。
    // 设备名 / devnode 路径都是系统用来认对象的键，原样交付、不做归一化；这里只折叠 ASCII
    // 与各语言字母的大小写（towlower），不裁空白、不换分隔符。
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

bool MonitorSelectorNeedsIdentity(const MonitorSelector& sel) {
    return sel.kind == MonitorSelector::Kind::kPath;
}

std::wstring MonitorSelectorLabel(const MonitorSelector& sel) {
    using Kind = MonitorSelector::Kind;
    switch (sel.kind) {
        case Kind::kAll: return L"all";
        case Kind::kOrdinal: return std::to_wstring(sel.ordinal);
        case Kind::kDevice: return L"device:" + sel.id;
        case Kind::kPath: return L"id:" + sel.id;
        case Kind::kPrimary: break;
    }
    return L"primary";
}

const wchar_t* MonitorSelectorKindName(const MonitorSelector& sel) {
    using Kind = MonitorSelector::Kind;
    switch (sel.kind) {
        case Kind::kAll: return L"all";
        case Kind::kOrdinal: return L"ordinal";
        case Kind::kDevice: return L"device";
        case Kind::kPath: return L"id";
        case Kind::kPrimary: break;
    }
    return L"primary";
}

ScreenSelectResult SelectScreenCandidates(const MonitorSelector& sel,
                                          const std::vector<ScreenCandidate>& all) {
    using Kind = MonitorSelector::Kind;
    ScreenSelectResult r;

    // 一块屏都没有：这与"编号写错了"同一类（调用方的下一步都是重新看一遍本机），
    // 也是旧行为 —— 不因这次加了标识写法就给它换一条码。
    if (all.empty()) {
        r.outcome = ScreenSelect::kOutOfRange;
        Diagnostic d;
        d.code = codes::kMonitorOutOfRange;
        d.message = Msg(L"match.monitor_out_of_range");
        d.option = L"--monitor";
        d.value = MonitorSelectorLabel(sel);
        d.hint = Msgf(L"match.monitor_out_of_range_hint", all.size(), std::wstring());
        d.stage = stages::kMatch;
        r.errors.push_back(std::move(d));
        return r;
    }

    switch (sel.kind) {
        case Kind::kAll:
            r.picked.resize(all.size());
            for (size_t i = 0; i < all.size(); ++i) r.picked[i] = i;
            r.outcome = ScreenSelect::kFound;
            return r;
        case Kind::kPrimary: {
            // 没有哪块被标成主屏（远程会话里见过）时照旧用第一块。这一条老行为不改：
            // 调用方说的是"主屏"，而本机对"哪块是主屏"这个问题的答复是"没有主屏可言"，
            // 在这台机器上"第一块"与"唯一那块"是同一件事（一块屏的会话里就是这么判的）。
            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].screen.primary) {
                    r.picked.push_back(i);
                    r.outcome = ScreenSelect::kFound;
                    return r;
                }
            }
            r.picked.push_back(0);
            r.outcome = ScreenSelect::kFound;
            return r;
        }
        case Kind::kOrdinal: {
            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].screen.ordinal == sel.ordinal) {
                    r.picked.push_back(i);
                    r.outcome = ScreenSelect::kFound;
                    return r;
                }
            }
            r.outcome = ScreenSelect::kOutOfRange;
            r.errors.push_back(SelectorError(codes::kMonitorOutOfRange,
                                             L"match.monitor_out_of_range",
                                             L"match.monitor_out_of_range_hint", sel, all, nullptr));
            return r;
        }
        case Kind::kDevice: {
            // 两边都先去前缀再比：解析层已经把用户写的那一条归一过一次，这里再判一次是幂等的
            // （同一个 inline 函数，形状只写一处），这样比对这一层收到的 id 带不带前缀都对得上，
            // 不必要求每一条调用路径都记得先剥。
            const std::wstring want = StripScreenDevicePrefix(sel.id);
            for (size_t i = 0; i < all.size(); ++i) {
                if (ScreenIdEquals(StripScreenDevicePrefix(all[i].screen.deviceName), want)) {
                    r.picked.push_back(i);
                }
            }
            break;
        }
        case Kind::kPath: {
            // 这一路只有显示配置答得上来。整张表里一条 devnode 都没有 = 这一问没答案，
            // 那不等于"没有这块屏"：报 unverifiable，让调用方去查显示拓扑，
            // 而不是拿"没找到"把人引向"那我改用编号试试"。
            bool anyAnswer = false;
            uint32_t why = 0;
            for (const ScreenCandidate& c : all) {
                if (PathReadable(c)) anyAnswer = true;
                if (why == 0 && c.facts.hasFacts) {
                    why = c.facts.config.win32 != 0 ? c.facts.config.win32
                                                    : c.facts.monitorPathQ.win32;
                }
            }
            if (!anyAnswer) {
                r.outcome = ScreenSelect::kUnverifiable;
                Diagnostic d = SelectorError(codes::kMonitorIdUnverifiable,
                                             L"match.monitor_id_unverifiable",
                                             L"match.monitor_id_unverifiable_hint", sel, all,
                                             nullptr);
                d.win32 = why;
                r.errors.push_back(std::move(d));
                return r;
            }
            for (size_t i = 0; i < all.size(); ++i) {
                if (PathReadable(all[i]) && ScreenIdEquals(all[i].facts.monitorPath, sel.id)) {
                    r.picked.push_back(i);
                }
            }
            break;
        }
    }

    if (r.picked.size() == 1) {
        r.outcome = ScreenSelect::kFound;
        return r;
    }
    if (r.picked.empty()) {
        r.outcome = ScreenSelect::kNoMatch;
        r.errors.push_back(SelectorError(codes::kMonitorUnknownId, L"match.monitor_unknown_id",
                                         L"match.monitor_unknown_id_hint", sel, all, nullptr));
        return r;
    }
    // 同一个标识命中多块屏：绝不替调用方挑一块（挑哪一块都没依据），把命中的那几块列全。
    r.outcome = ScreenSelect::kAmbiguous;
    r.errors.push_back(SelectorError(codes::kMonitorAmbiguousId, L"match.monitor_ambiguous_id",
                                     L"match.monitor_ambiguous_id_hint", sel, all, &r.picked));
    return r;
}

ScreenIdentityCheck CompareScreenIdentity(const ScreenCandidate& wanted,
                                          const std::vector<ScreenCandidate>& current,
                                          ScreenCandidate* fresh) {
    if (!PathReadable(wanted)) {
        // 当初就没问出跨会话标识：照旧按设备名核 —— 这一档不比旧实现更严，也不比它更松。
        std::vector<ScreenInfo> infos;
        infos.reserve(current.size());
        for (const ScreenCandidate& c : current) infos.push_back(c.screen);
        ScreenInfo freshInfo{};
        const ScreenCheck old = CompareScreen(wanted.screen, infos, &freshInfo);
        if (fresh && old != ScreenCheck::kGone) fresh->screen = freshInfo;
        return old == ScreenCheck::kSame   ? ScreenIdentityCheck::kSame
             : old == ScreenCheck::kMoved ? ScreenIdentityCheck::kMoved
                                          : ScreenIdentityCheck::kGone;
    }

    bool anyAnswer = false;
    for (const ScreenCandidate& c : current) {
        if (!PathReadable(c)) continue;
        anyAnswer = true;
        if (!ScreenIdEquals(c.facts.monitorPath, wanted.facts.monitorPath)) continue;
        if (fresh) *fresh = c;
        return SameGeometry(wanted.screen, c.screen) ? ScreenIdentityCheck::kSame
                                                    : ScreenIdentityCheck::kMoved;
    }
    // 这一次一个 devnode 都没问出来 —— 那不是"那块屏没了"，是"这一问没答案"。
    // 这时**不**退回按名字核：那个名字可能已经发给了另一块面板，照名字截就是截一块
    // 没人批准过的屏（与「身份变了不去找替代目标」那一条同源）。
    if (!anyAnswer) return ScreenIdentityCheck::kUnverifiable;
    return ScreenIdentityCheck::kGone;
}

SamplingRecheck RecheckScreenSampling(const ScreenCandidate& approvedTarget,
                                      const PhysRect& approvedArea,
                                      const std::vector<ScreenCandidate>& current,
                                      ScreenCandidate* fresh) {
    ScreenCandidate now{};
    switch (CompareScreenIdentity(approvedTarget, current, &now)) {
        case ScreenIdentityCheck::kUnverifiable: return SamplingRecheck::kUnverifiable;
        case ScreenIdentityCheck::kGone: return SamplingRecheck::kGone;
        // "还是那块屏，但形状/编号/主屏归属变了"落在采样这一问里就是确认之后的漂移：
        // 人在框上批准的是旧样子。不采、也不把旧授权追认到新布局上 —— 重新确认由
        // 调用方带着新的事实重来一遍，而不是在这里悄悄换一块矩形继续。
        case ScreenIdentityCheck::kMoved: return SamplingRecheck::kStale;
        case ScreenIdentityCheck::kSame: break;
    }
    // 身份说形状没变，还要与"刚批准的那一片"再对一次：批准区域是人看过的那份事实，
    // 采样矩形越出它以外就不是被批准的画面。零容差 —— 与 DesktopPermit::Covers 同一判据。
    const PhysRect& b = now.screen.bounds;
    const PhysRect& a = approvedArea;
    if (b.left < a.left || b.top < a.top || b.right > a.right || b.bottom > a.bottom) {
        return SamplingRecheck::kStale;
    }
    if (fresh) *fresh = now;
    return SamplingRecheck::kOk;
}

}  // namespace ecapture
