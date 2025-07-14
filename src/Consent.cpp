#include "Consent.h"

#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"
#include "ScreenMatch.h"

namespace ecapture {
namespace {

constexpr size_t kMaxListedTargets = 6;

// 点"是"到真正取帧之间要等一下：对话框刚销毁时关闭动画还在 DWM 的画面上，
// 立刻截就会把半透明的残影拍进图里。1 秒足够动画放完，也不会让人觉得卡住。
// 这个等待属于"真有个框刚被关掉"这件事，所以放在这里而不是授权判定器里：
// 测试注入的假应答器没有动画，也就不必等。
constexpr DWORD kDialogSettleMs = 1000;

std::wstring JoinLines(const std::vector<std::wstring>& lines, size_t maxListed,
                       const wchar_t* moreKey) {
    std::wstring text;
    for (size_t i = 0; i < lines.size() && i < maxListed; ++i) {
        if (i) text += L"\r\n  ";
        text += lines[i];
    }
    if (lines.size() > maxListed) {
        text += L"\r\n  " + Msgf(moreKey, lines.size() - maxListed);
    }
    return text;
}

// 只给 ConsentGate 用的拓扑指纹：屏幕数量、设备名、每块屏的矩形都算进去。
// 拔掉或挪动一台显示器，之前那次"我批准截这块屏幕"就不再成立了。
uint64_t Fingerprint(const std::vector<ScreenInfo>& screens) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFull;
            h *= 1099511628211ull;
        }
    };
    for (const ScreenInfo& s : screens) {
        mix(static_cast<uint64_t>(s.bounds.left));
        mix(static_cast<uint64_t>(s.bounds.top));
        mix(static_cast<uint64_t>(s.bounds.right));
        mix(static_cast<uint64_t>(s.bounds.bottom));
        mix(s.primary ? 1ull : 0ull);
        for (wchar_t c : s.deviceName) mix(static_cast<uint64_t>(static_cast<unsigned>(c)));
    }
    mix(screens.size());
    return h;
}

bool RectContains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

bool RectIsEmpty(const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }

}  // namespace

// ---------------------------------------------------------------------------
// 真机上的确认框
// ---------------------------------------------------------------------------

ConsentReply DialogConsentPrompt::Ask(const ConsentQuestion& q) {
    ConsentReply reply;
    const bool desktop = q.scope == PixelScope::kDesktop;

    std::wstring body;
    body += Msg(L"consent.head") + L"\r\n\r\n";
    body += Msgf(L"consent.targets", q.targets.size(),
                 JoinLines(q.targets, kMaxListedTargets, L"consent.more_targets")) + L"\r\n";
    body += Msgf(L"consent.plan", q.captureLabel, q.path, JoinLines(q.outputs, kMaxListedTargets,
                                                                   L"consent.more_targets")) +
            L"\r\n\r\n";
    body += Msg(desktop ? L"consent.scope_desktop" : L"consent.scope_window") + L"\r\n";
    // 桌面那一级必须把"--yes 在这儿没用"写在人眼前：不然调用方以为加了开关就等于授权。
    if (desktop && q.yesGiven) body += Msg(L"consent.yes_desktop") + L"\r\n";
    if (!desktop && !q.yesGiven) body += Msg(L"consent.yes_window") + L"\r\n";
    body += Msg(L"consent.dialog_closes") + L"\r\n\r\n";
    body += Msg(L"consent.ask");

    // 默认焦点在"否"上，回车不会误批；置顶是因为用户可能正全屏开着别的东西。
    const int answer = MessageBoxW(nullptr, body.c_str(), Msg(L"consent.title").c_str(),
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND |
                                       MB_TOPMOST);
    // MessageBoxW 返回 0 = 根本没弹出框（服务会话、没有交互桌面）。这时没有人能回答"是"，
    // 按"不可用"处理 —— 与"人答了否"分开，调用方才知道该做的是换会话而不是再问一次。
    if (answer == 0) {
        reply.answer = ConsentAnswer::kUnavailable;
        reply.win32 = LastError();   // 当场取码：拼文案要读资源，晚一步就不是这个码了
        return reply;
    }
    if (answer == IDYES) {
        Sleep(kDialogSettleMs);
        reply.answer = ConsentAnswer::kAccepted;
        return reply;
    }
    reply.answer = ConsentAnswer::kDeclined;
    return reply;
}

// ---------------------------------------------------------------------------
// DesktopPermit
// ---------------------------------------------------------------------------

bool DesktopPermit::Covers(const RECT& area) const {
    if (RectIsEmpty(area) || RectIsEmpty(approved_)) return false;
    // 允许一点测量误差：DWM 报告的外壳矩形、宿主窗口的实际尺寸与 GetWindowRect 之间可以差
    // 几像素（那圈不可见边框各处取整方式不同）。真被人挪走或换掉的窗口不会只差这么点。
    constexpr int kSlackPx = 16;
    RECT grown{approved_.left - kSlackPx, approved_.top - kSlackPx, approved_.right + kSlackPx,
               approved_.bottom + kSlackPx};
    return RectContains(grown, area);
}

// ---------------------------------------------------------------------------
// ConsentGate
// ---------------------------------------------------------------------------

ConsentGate::ConsentGate(GateConfig config, IConsentPrompt& prompt, TopologyProvider topology)
    : config_(std::move(config)), prompt_(prompt), topology_(std::move(topology)) {
    if (!topology_) topology_ = [] { return EnumScreens(); };
}

uint64_t ConsentGate::TopologyFingerprint() const { return Fingerprint(topology_()); }

void ConsentGate::SetTargetArea(const std::wstring& targetKey, const RECT& area) {
    for (GateTarget& t : config_.targets) {
        if (t.key == targetKey) {
            t.area = area;
            return;
        }
    }
    // 清单外的目标 = 调用方扩大了范围，这里不接受：宁可让它因为"没有对应授权"而重新问一次。
}

ConsentQuestion ConsentGate::MakeQuestion(const wchar_t* path, PixelScope scope,
                                          const std::vector<GateTarget>& listed) const {
    ConsentQuestion q;
    q.scope = scope;
    q.path = path ? path : paths::kUnknown;
    q.yesGiven = config_.yes;
    q.captureLabel = config_.captureLabel;
    for (const GateTarget& t : listed) q.targets.push_back(t.description);
    q.outputs = config_.outputs;
    return q;
}

Diagnostic ConsentGate::Denied(const wchar_t* path, const std::wstring& targetKey, PixelScope scope,
                               bool unavailable, DWORD gle) const {
    const bool desktop = scope == PixelScope::kDesktop;
    Diagnostic d;
    d.code = unavailable ? codes::kConsentUnavailable : codes::kAccessDenied;
    d.message = Msg(unavailable ? L"consent.unavailable" : L"consent.denied");
    d.option = L"--capture";
    d.value = path ? path : std::wstring();
    d.hint = Msg(unavailable
                     ? L"consent.unavailable_hint"
                     : (desktop ? L"consent.denied_hint_desktop" : L"consent.denied_hint_window"));
    d.target = targetKey;
    // backend 留给调用它的流水线去填通道名（bitblt / dwm / ...）：判定器只知道路径名，
    // 而路径名写在 value 里 —— 两者分开发，调用方既能按通道分支也能看见内部那条支路。
    d.stage = stages::kConsent;
    d.win32 = gle;
    return d;
}

void ConsentGate::GrantDesktop(const std::vector<GateTarget>& listed) {
    desktopLevel_ = Level::kGranted;
    desktopAreas_ = listed;  // 冻结：之后目标挪了位置就要重新问
    topologyAtAsk_ = TopologyFingerprint();
}

bool ConsentGate::DesktopGrantedFor(const std::wstring& targetKey, const RECT& area,
                                    RECT* approved) const {
    if (desktopLevel_ != Level::kGranted) return false;
    if (TopologyFingerprint() != topologyAtAsk_) return false;  // 拓扑变了 = 授权作废
    for (const GateTarget& t : desktopAreas_) {
        if (t.key != targetKey) continue;
        if (!RectContains(t.area, area)) return false;  // 超出当时批准的那一片
        if (approved) *approved = t.area;
        return true;
    }
    return false;  // 清单里没有这个目标 = 范围被扩大，重新问
}

bool ConsentGate::AuthorizeWindow(const wchar_t* path, const std::wstring& targetKey,
                                 Diagnostic* err) {
    if (Refused()) {
        // 已经有人答过"否"（或根本没有桌面可弹）：这一次请求里后面都不再问、也不再截。
        // 这条规矩放在判定器自己身上，不依赖调用方记得去查。
        if (err) *err = Denied(path, targetKey, PixelScope::kWindowContent, false, 0);
        return false;
    }
    if (!NeedsHumanConsent(path, config_.yes)) return true;  // 带 --yes 的可靠窗口路径
    if (windowLevel_ == Level::kGranted) return true;
    if (windowLevel_ == Level::kRefused) {
        if (err) *err = Denied(path, targetKey, PixelScope::kWindowContent, false, 0);
        return false;
    }

    ++windowAsks_;
    const ConsentQuestion q = MakeQuestion(path, PixelScope::kWindowContent, config_.targets);
    const ConsentReply reply = prompt_.Ask(q);
    if (reply.answer == ConsentAnswer::kAccepted) {
        windowLevel_ = Level::kGranted;
        return true;
    }
    windowLevel_ = Level::kRefused;
    if (err) {
        *err = Denied(path, targetKey, PixelScope::kWindowContent,
                      reply.answer == ConsentAnswer::kUnavailable, reply.win32);
    }
    return false;
}

bool ConsentGate::AuthorizeDesktop(const wchar_t* path, const std::wstring& targetKey,
                                   const RECT& area, std::optional<DesktopPermit>* out,
                                   Diagnostic* err) {
    if (Refused()) {   // 这一级或另一级被人否过（也包括弹不出框）：不再问第二遍，也不再截
        if (err) *err = Denied(path, targetKey, PixelScope::kDesktop, false, 0);
        return false;
    }
    RECT approved{};
    if (DesktopGrantedFor(targetKey, area, &approved)) {
        if (out) *out = DesktopPermit::Issue(targetKey, approved);
        return true;
    }
    if (desktopLevel_ == Level::kGranted) {
        // 之前批过，但这次的目标或矩形不在当时那份清单里 —— 范围升级，重新问一次。
        desktopLevel_ = Level::kNotAsked;
        desktopAreas_.clear();
    }

    ++desktopAsks_;
    const ConsentQuestion q = MakeQuestion(path, PixelScope::kDesktop, config_.targets);
    const ConsentReply reply = prompt_.Ask(q);
    if (reply.answer != ConsentAnswer::kAccepted) {
        desktopLevel_ = Level::kRefused;
        if (err) {
            *err = Denied(path, targetKey, PixelScope::kDesktop,
                          reply.answer == ConsentAnswer::kUnavailable, reply.win32);
        }
        return false;
    }
    GrantDesktop(config_.targets);
    if (!DesktopGrantedFor(targetKey, area, &approved)) {
        // 人在框上看到的清单里根本没有这次要取的这一块：不给自己放行。
        // 真机流水线的目标矩形在问之前刚量过，正常走不到这里；走到就是调用点写错了。
        if (err) *err = Denied(path, targetKey, PixelScope::kDesktop, false, 0);
        return false;
    }
    if (out) *out = DesktopPermit::Issue(targetKey, approved);
    return true;
}

}  // namespace ecapture
