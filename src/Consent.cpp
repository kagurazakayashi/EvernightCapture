#include "Consent.h"

#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"
#include "Deadline.h"
#include "ScreenMatch.h"

namespace ecapture {
namespace {

constexpr size_t kMaxListedTargets = 6;

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

// 问一次人：这次运行若配了自动预算（GateConfig::autoBudget），就**只在真人等待的这一段**
// 暂停它 —— 弹框阻塞、以及同意后关闭动画的缓冲（RunMonitoredDialog 里的 Sleep）都在这段
// 之内。暂停不回填预算：弹框之前已耗尽的预算恢复后照样是零。调用点只在真的要 Ask 时才走到
// 这里；--yes 直通、同批复用已给出的许可那些分支在本函数之前就已返回，不借道补时间。
// 人工那一级自己的 --consent-timeout-ms 走 GetTickCount64 秒表，不受这份暂停影响。
ConsentReply AskWithBudgetPause(const Deadline* budget, IConsentPrompt& prompt,
                                const ConsentQuestion& q) {
    std::optional<Deadline::PauseScope> pause;
    if (budget) pause.emplace(budget->PauseForHumanWait());
    return prompt.Ask(q);
}

// ---------------------------------------------------------------------------
// 弹框那条线程与生命周期状态机
// ---------------------------------------------------------------------------

// 到点关框之后，给弹框线程的收尾宽限。实测（2026-10-02，10.0.26100，最小 Win32 探针 +
// tests\consent.ps1 第 8 节判据）：这机器上的 #32770 会**处理** WM_CLOSE 却根本不关框 ——
// SendMessageTimeoutW 都返回成功、结果 0，框还在。所以关框不能只发一条消息然后无限等
// done（那正是旧实现在这台机器上的死法），而要：升级链反复送 + 有界宽限 +
// 宽限用尽后按"拒绝"收尾。
constexpr DWORD kDialogCloseGraceMs = 3000;
constexpr DWORD kDialogPollSliceMs = 50;

// 点"是"到真正取帧之间要等一下：对话框刚销毁时关闭动画还在 DWM 的画面上，
// 立刻截就会把半透明的残影拍进图里。1 秒足够动画放完，也不会让人觉得卡住。
// 它只在"真有个框刚被关掉、而且人答了是"那一条路上生效：超时与放弃的路径到不了这里，
// 但超时也**绝不削减**它 —— 答应留的缓冲就留满，这是文案对用户的承诺。
// 这个等待算在**人工确认那一级**，不算在 --timeout-ms 的自动处理预算里：它发生在
// ConsentGate 暂停自动预算的作用域之内（见上面 AskWithBudgetPause 套的 PauseScope），
// 所以这段 Sleep 既不烧自动预算，也不被自动预算削减。
constexpr DWORD kDialogSettleMs = 1000;

// 一次弹框的可变状态，由 shared_ptr 共享寿命：等待线程与弹框线程（乃至被放弃后
// 仍在 MessageBoxW 里的那条）可以同时在世，谁都不引用谁的栈。done 事件句柄由
// **最后一个**释放者关闭 —— 所以等待方放弃之后，弹框线程仍对着有效句柄 SetEvent。
struct DialogState {
    HANDLE done = nullptr;         // 弹框线程 Show 返回（或抛异常）后置起
    volatile LONG threadId = 0;    // 弹框线程 id：按它才能找到那个 #32770
    DialogResult result;           // 写：弹框线程，置 done 之前；读：等待线程，观察到 done 之后
    ~DialogState() {
        if (done) CloseHandle(done);
    }
};

struct DialogThreadCtx {
    std::shared_ptr<DialogState> state;
    std::shared_ptr<DialogDriver> driver;
};

DWORD CALLBACK RunDialogThread(LPVOID param) {
    // ctx 的所有权立刻归这条线程：它在自己的收尾（destructor）里才释放，
    // 等待方放弃与否都动不了它 —— 这就是"不能用 TerminateThread"仍然成立的代价与解法。
    std::unique_ptr<DialogThreadCtx> ctx(static_cast<DialogThreadCtx*>(param));
    DialogState& state = *ctx->state;
    InterlockedExchange(&state.threadId, static_cast<LONG>(GetCurrentThreadId()));
    try {
        state.result = ctx->driver->Show();
    } catch (...) {
        // 驱动把框弹崩了，对生命周期来说就是"结果不可靠地拿不到"：按 MessageBoxW
        // 的失败约定记 code 0，等待方按"弹不出框"处理。异常绝不许跑出线程入口 ——
        // 没人接的话 MSVC 会 terminate，把整个进程（连同刚拿到同意的那一笔交付）带走。
        state.result.code = 0;
        state.result.win32 = LastError();
    }
    SetEvent(state.done);
    return 0;
}

BOOL CALLBACK FindDialogProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[16] = {};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0) return TRUE;
    if (std::wcscmp(cls, L"#32770") != 0) return TRUE;
    *reinterpret_cast<HWND*>(lParam) = hwnd;
    return FALSE;
}

HWND FindThreadDialog(LONG threadId) {
    if (threadId == 0) return nullptr;
    HWND found = nullptr;
    EnumThreadWindows(static_cast<DWORD>(threadId), FindDialogProc,
                      reinterpret_cast<LPARAM>(&found));
    return found;
}

LONG ReadThreadId(const DialogState& state) {
    // 跨线程读那个 volatile LONG：走原子操作，别指望 volatile 在两个线程之间给出一致的值
    return InterlockedCompareExchange(&const_cast<DialogState&>(state).threadId, 0, 0);
}

// 真机驱动。Show 就是 MessageBoxW；关框是一条升级链，理由全部来自实测：
//   1) WM_CLOSE —— "人按 X"的消息。本机实测被 #32770 无视（处理了，框不关）。
//      保留它：换了系统它可能就管用，而且它语义最正。
//   2) 下一切片发现框还在，就给"否"按钮补一发 BM_CLICK —— 本机实测这一发必关，
//      语义与按 X 同（人没同意）。等待方在宽限期内每个切片重发一次 RequestClose，
//      所以"框弹得晚、这一发还没窗口"也会被后面的切片补上。
// 两步都只是"等价于人自己关框"，改的是收场方式，不改答案：一律按拒绝。
class MessageBoxDriver final : public DialogDriver {
public:
    MessageBoxDriver(std::wstring body, std::wstring title, UINT flags)
        : body_(std::move(body)), title_(std::move(title)), flags_(flags) {}

    DialogResult Show() override {
        DialogResult r;
        r.code = MessageBoxW(nullptr, body_.c_str(), title_.c_str(), flags_);
        if (r.code == 0) r.win32 = LastError();
        return r;
    }

    bool RequestClose(DWORD dialogThreadId) override {
        const HWND dialog = FindThreadDialog(static_cast<LONG>(dialogThreadId));
        if (!dialog) return false;
        PostMessageW(dialog, WM_CLOSE, 0, 0);
        if (++closeSlices_ >= 2) {
            // GetDlgItem/PostMessage 对已消失的按钮返回假值，无害；框一关，后续切片
            // 连 dialog 都找不到，自然停下。
            PostMessageW(GetDlgItem(dialog, IDNO), BM_CLICK, 0, 0);
        }
        return true;
    }

private:
    std::wstring body_;
    std::wstring title_;
    const UINT flags_;
    int closeSlices_ = 0;   // 只有等待线程会碰 RequestClose
};

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期状态机本体 + 生产入口
// ---------------------------------------------------------------------------

ConsentReply RunMonitoredDialog(std::shared_ptr<DialogDriver> driver, uint64_t timeoutMs) {
    ConsentReply reply;
    auto state = std::make_shared<DialogState>();
    state->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!state->done) {
        const DWORD gle = LastError();
        reply.answer = ConsentAnswer::kUnavailable;
        reply.win32 = gle;
        return reply;
    }
    // 等待方自己留一份驱动引用：放弃收尾之后、弹框线程归还之前，对象不会半路析构。
    auto* ctx = new DialogThreadCtx{state, driver};
    HANDLE thread = CreateThread(nullptr, 0, RunDialogThread, ctx, 0, nullptr);
    if (!thread) {
        const DWORD gle = LastError();
        delete ctx;
        reply.answer = ConsentAnswer::kUnavailable;
        reply.win32 = gle;
        return reply;
    }

    const ULONGLONG started = GetTickCount64();
    bool answered = false;   // 观察到 done（弹框线程已写出结果）
    bool timedOut = false;   // 一旦 latch 永不解除：超时与"是"同刻发生时的保守优先级
    for (;;) {
        answered = WaitForSingleObject(state->done, kDialogPollSliceMs) == WAIT_OBJECT_0;
        const ULONGLONG elapsed = GetTickCount64() - started;
        if (timeoutMs > 0 && !timedOut && elapsed >= timeoutMs) {
            // 期限先到就 latch —— 哪怕同一个切片里紧接着观察到"是"，答案也是拒绝。
            // 没人应答就是没人同意，绝不读成"没人反对"。
            timedOut = true;
        }
        if (answered) break;
        if (timedOut) {
            if (elapsed >= timeoutMs + kDialogCloseGraceMs) break;   // 放弃收尾（见下）
            // 每个切片重试：框弹得晚、上一条消息被无视、瞬时的查找/投递失败，
            // 都在这条有界循环里被补上。
            driver->RequestClose(static_cast<DWORD>(ReadThreadId(*state)));
        }
        // timeoutMs == 0 到这里就继续等 —— 那是"等人回答"的文档语义，不是没兑现的上限。
    }
    CloseHandle(thread);   // 观察到 done 之后线程只是收尾，句柄不再需要

    if (!answered) {
        // 宽限期用尽仍没能把这条框关到回话：到点了没人答，按拒绝 + timedOut 收尾。
        // 安全收尾方案：结果、事件句柄、驱动都活在这条线程自己持有的 shared_ptr 里，
        // 本函数返回不落空任何东西；绝不 TerminateThread。框可能仍在屏幕上，但流水线上线
        // 已经拿到"拒绝"，不会再采一个像素；人之后就算点了"是"也没人再听（那条线程写完
        // 结果就自己收尾）。窗口和线程随本进程退出消失 —— 边界是极端系统故障把进程
        // 退出本身也拖住：那时宁可留下一个没人应答的框，也不留下一次没人同意的截图。
        reply.answer = ConsentAnswer::kDeclined;
        reply.timedOut = true;
        return reply;
    }
    // 能安全读 result：它在弹框线程 SetEvent 之前写好，而 done 的观察到读取之间
    // 由事件等待建立先后关系。
    const DialogResult res = state->result;
    if (res.code == 0) {
        // 根本没弹出框（服务会话、没有交互桌面）或驱动抛了异常：没有人能回答"是"，
        // 按"不可用"处理 —— 与"人答了否"分开，调用方才知道该换会话而不是再问一次。
        reply.answer = ConsentAnswer::kUnavailable;
        reply.win32 = res.win32;
        return reply;
    }
    if (timedOut) {
        // 期限先到过就是拒绝：哪怕"是"在同一瞬间被点下去、关框的点击恰好比人慢一步，
        // 也不改成同意。
        reply.answer = ConsentAnswer::kDeclined;
        reply.timedOut = true;
        return reply;
    }
    if (res.code == IDYES) {
        Sleep(kDialogSettleMs);   // 关闭动画缓冲：见 kDialogSettleMs 的注释
        reply.answer = ConsentAnswer::kAccepted;
        return reply;
    }
    reply.answer = ConsentAnswer::kDeclined;   // 人点了"否"，或自己按 X 关了框
    return reply;
}

ConsentReply ShowConsentDialog(const std::wstring& body, const std::wstring& title, UINT flags,
                               uint64_t timeoutMs) {
    return RunMonitoredDialog(
        std::make_shared<MessageBoxDriver>(body, title, flags), timeoutMs);
}

// ---------------------------------------------------------------------------
// 真机上的确认框
// ---------------------------------------------------------------------------

ConsentReply DialogConsentPrompt::Ask(const ConsentQuestion& q) {
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
    if (timeoutMs_ > 0) {
        // 等人回答的那段时间是有上限的，这件事要写在框上：不然人以为"放着不管以后还能截"，
        // 而实际上到点就按拒绝处理了。
        body += Msgf(L"consent.deadline_line", timeoutMs_) + L"\r\n";
    }

    // 默认焦点在"否"上，回车不会误批；置顶是因为用户可能正全屏开着别的东西。
    // MB_YESNO 是关框升级链的前提（要点的是那块 IDNO 按钮），换按钮组合前先回去读
    // MessageBoxDriver 的注释。
    const UINT flags = MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST;
    return ShowConsentDialog(body, Msg(L"consent.title"), flags, timeoutMs_);
}

// ---------------------------------------------------------------------------
// DesktopPermit
// ---------------------------------------------------------------------------

bool DesktopPermit::Covers(const RECT& area) const {
    if (RectIsEmpty(area) || RectIsEmpty(approved_)) return false;
    // 严格包含、零容差：向外扩任何像素去采样都是把人没批准过的画面带进图里，
    // "测量误差"不能成为扩大隐私范围的理由。而正常路径本来就量在批准矩形之内：
    // 批准的是整窗外框（GetWindowRect 那圈含不可见边框的大矩形），从屏幕实际读走的
    // 是 DWM 的扩展框架矩形（WindowScreenRect），它是前者**内缩**的一块；DWM 升级路径
    // 授权与采样的都是宿主窗口矩形本身，逐像素相等。真被人挪走或换大的目标会撞在
    // 这条线上判成 consent_stale —— 那正是要它做的事。
    return RectContains(approved_, area);
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

ConsentGate::ConsentSnapshot ConsentGate::TakeSnapshot() const {
    ConsentSnapshot snap;
    snap.listed = config_.targets;
    snap.outputs = config_.outputs;
    snap.topology = TopologyFingerprint();   // 弹框**之前**取的基线
    return snap;
}

ConsentQuestion ConsentGate::MakeQuestion(const wchar_t* path, PixelScope scope,
                                          const ConsentSnapshot& snap) const {
    ConsentQuestion q;
    q.scope = scope;
    q.path = path ? path : paths::kUnknown;
    q.yesGiven = config_.yes;
    q.captureLabel = config_.captureLabel;
    // 框上的每一行从快照即时渲染：给人看的坐标与将要冻结进授权的坐标是同一份数，
    // 不存在"文案拼好之后区域又刷新了"的平行文本。
    for (const GateTarget& t : snap.listed) {
        q.targets.push_back(Msgf(L"consent.target_line", t.subject,
                                 static_cast<long long>(t.area.left),
                                 static_cast<long long>(t.area.top),
                                 static_cast<long long>(t.area.right - t.area.left),
                                 static_cast<long long>(t.area.bottom - t.area.top)));
    }
    q.outputs = snap.outputs;
    return q;
}

Diagnostic ConsentGate::Denied(const wchar_t* path, const std::wstring& targetKey, PixelScope scope,
                               bool unavailable, DWORD gle, bool timedOut) const {
    const bool desktop = scope == PixelScope::kDesktop;
    Diagnostic d;
    // 三种"没有得到同意"各有各的下一步：人答了否（要换条件或换人来答）、框根本弹不出来
    // （要换一个能弹框的会话）、以及期限到点没人答（可能只是人暂时不在，再来一次也许就有人）。
    // 三者同为退出码 6，但 code 不同，调用方才不用猜。
    d.code = timedOut   ? codes::kConsentTimeout
           : unavailable ? codes::kConsentUnavailable
                         : codes::kAccessDenied;
    d.message = Msg(timedOut    ? L"consent.timeout"
                    : unavailable ? L"consent.unavailable"
                                  : L"consent.denied");
    d.option = L"--capture";
    d.value = path ? path : std::wstring();
    d.hint = Msg(timedOut      ? L"consent.timeout_hint"
                 : unavailable ? L"consent.unavailable_hint"
                               : (desktop ? L"consent.denied_hint_desktop"
                                          : L"consent.denied_hint_window"));
    d.target = targetKey;
    // backend 留给调用它的流水线去填通道名（bitblt / dwm / ...）：判定器只知道路径名，
    // 而路径名写在 value 里 —— 两者分开发，调用方既能按通道分支也能看见内部那条支路。
    d.stage = stages::kConsent;
    d.win32 = gle;
    return d;
}

Diagnostic ConsentGate::Stale(const wchar_t* path, const std::wstring& targetKey) const {
    // 确认基线在弹框期间失效：这不是"人不同意"（那是 access_denied，整批要停），也不是
    // "弹不出框"。它说的是"人点头时看到的那份事实已经不在了"—— 这一问不签发任何授权，
    // 也不换一个新基线把这一问蒙过去；调用方拿新的事实重来一遍，会再问一次。
    Diagnostic d;
    d.code = codes::kConsentStale;
    d.message = Msg(L"cap.consent.stale");
    d.option = L"--capture";
    d.value = path ? path : std::wstring();
    d.hint = Msg(L"cap.consent.stale_hint");
    d.target = targetKey;
    d.stage = stages::kConsent;
    return d;
}

void ConsentGate::GrantDesktop(const ConsentSnapshot& snap) {
    desktopLevel_ = Level::kGranted;
    desktopAreas_ = snap.listed;   // 冻结的就是人刚看过的那一份：之后目标挪了位置要重新问
    topologyAtAsk_ = snap.topology;  // 基线是弹框**之前**那一次取值，不是应答之后首次所见
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
        if (err) *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyUnavailable_, 0,
                               lastDenyTimedOut_);
        return false;
    }
    if (!NeedsHumanConsent(path, config_.yes)) return true;  // 带 --yes 的可靠窗口路径
    if (windowLevel_ == Level::kGranted) return true;
    if (windowLevel_ == Level::kRefused) {
        if (err) *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyUnavailable_, 0,
                               lastDenyTimedOut_);
        return false;
    }

    ++windowAsks_;
    // 同样从弹框前的快照问起（清单与文案同源）。这一级不做确认后的拓扑复核：窗口内容
    // 路径的像素来自目标窗口自己，屏幕布局在确认期间怎么变都不改变"人批准了什么"；
    // "点完'是'之后目标还是不是那一扇"由流水线在授权之后按身份复核（CaptureOneChannel
    // 与 DWM 升级路径各有一次），不是这一关的职责。
    const ConsentQuestion q = MakeQuestion(path, PixelScope::kWindowContent, TakeSnapshot());
    const ConsentReply reply = AskWithBudgetPause(config_.autoBudget, prompt_, q);
    if (reply.answer == ConsentAnswer::kAccepted) {
        windowLevel_ = Level::kGranted;
        return true;
    }
    windowLevel_ = Level::kRefused;
    lastDenyUnavailable_ = reply.answer == ConsentAnswer::kUnavailable;
    lastDenyTimedOut_ = reply.timedOut;
    if (err) {
        *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyUnavailable_,
                      reply.win32, lastDenyTimedOut_);
    }
    return false;
}

bool ConsentGate::AuthorizeDesktop(const wchar_t* path, const std::wstring& targetKey,
                                   const RECT& area, std::optional<DesktopPermit>* out,
                                   Diagnostic* err) {
    if (Refused()) {   // 这一级或另一级被人否过（也包括弹不出框）：不再问第二遍，也不再截
        if (err) *err = Denied(path, targetKey, PixelScope::kDesktop, lastDenyUnavailable_, 0,
                               lastDenyTimedOut_);
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
    // 弹框前冻结这一轮的授权快照：框上写的目标、区域、输出与拓扑基线全部出自这一次取值。
    const ConsentSnapshot snap = TakeSnapshot();
    const ConsentQuestion q = MakeQuestion(path, PixelScope::kDesktop, snap);
    const ConsentReply reply = AskWithBudgetPause(config_.autoBudget, prompt_, q);
    if (reply.answer != ConsentAnswer::kAccepted) {
        desktopLevel_ = Level::kRefused;
        lastDenyUnavailable_ = reply.answer == ConsentAnswer::kUnavailable;
        lastDenyTimedOut_ = reply.timedOut;
        if (err) {
            *err = Denied(path, targetKey, PixelScope::kDesktop, lastDenyUnavailable_, reply.win32,
                          lastDenyTimedOut_);
        }
        return false;
    }
    // 人点头（含关闭动画缓冲）之后、签发授权之前，重新查询一遍拓扑，与**弹框前**那份快照比。
    // 这一步是 F06 的要害：确认期间热插拔、拔线、改分辨率或挪了屏幕位置时，人在框上最后
    // 看到的还是旧布局 —— 拿应答之后第一次所见的新布局当基线，等于替旧请求追认了新拓扑。
    // 判据是停，不是重试：这一问一张都不采、不签发凭证，也不自动按新布局再弹一次框
    // （同一次调用里反复弹框就是无限重复问人）；调用方重来一遍时会在新的快照上重新问。
    if (TopologyFingerprint() != snap.topology) {
        desktopLevel_ = Level::kNotAsked;   // 没答"否"：批次不因这个失败而整批停止
        if (err) *err = Stale(path, targetKey);
        return false;
    }
    GrantDesktop(snap);
    if (!DesktopGrantedFor(targetKey, area, &approved)) {
        // 人在框上看到的清单里根本没有这次要取的这一块：不给自己放行。
        // 真机流水线的目标矩形在问之前刚量过，正常走不到这里；走到就是调用点写错了。
        if (err) *err = Denied(path, targetKey, PixelScope::kDesktop, false, 0, false);
        return false;
    }
    if (out) *out = DesktopPermit::Issue(targetKey, approved);
    return true;
}

}  // namespace ecapture
