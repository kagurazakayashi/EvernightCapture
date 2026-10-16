#include "Consent.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#include "Deadline.h"
#include "ScreenMatch.h"

namespace ecapture {
namespace {

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

// 问一次人：这次运行若配了自动预算（GateConfig::autoBudget），就**只在真人等待的这一段**
// 暂停它 —— 弹框阻塞、以及同意后关闭动画的缓冲（RunMonitoredDialog 里的 Sleep）都在这段
// 之内。暂停不回填预算：弹框之前已耗尽的预算恢复后照样是零。调用点只在真的要 Ask 时才走到
// 这里；--yes 直通、同批复用已给出的许可那些分支在本函数之前就已返回，不借道补时间。
// 人工那一级自己的 --consent-timeout-ms 走稳定时钟，不受这份暂停影响。
ConsentReply AskWithBudgetPause(const Deadline* budget, IConsentPrompt& prompt,
                                const ConsentQuestion& q) {
    std::optional<Deadline::PauseScope> pause;
    if (budget) pause.emplace(budget->PauseForHumanWait());
    return prompt.Ask(q);
}

// ---------------------------------------------------------------------------
// 到点 / 取消之后，给驱动关框的收尾宽限。窗口可能刚创建、事件队列还没转起来，
// 所以宽限期内每个切片重发一次关闭请求；宽限用尽仍没关掉时如实收尾，绝不改成同意。
constexpr std::chrono::milliseconds kDialogCloseGrace{3000};
constexpr std::chrono::milliseconds kDialogPollSlice{50};

// 点"是"到真正取帧之间要等一下：对话框刚销毁时关闭动画还在合成器画面上，
// 立刻截就会把半透明的残影拍进图里。1 秒足够动画放完，也不会让人觉得卡住。
// 它只在"真有个框刚被关掉、而且人答了是"那一条路上生效：超时与取消的路径到不了这里，
// 但超时也**绝不削减**它 —— 答应留的缓冲就留满，这是文案对用户的承诺。
// 这个等待算在**人工确认那一级**，不算在 --timeout-ms 的自动处理预算里：它发生在
// ConsentGate 暂停自动预算的作用域之内（见上面 AskWithBudgetPause 套的 PauseScope），
// 所以这段等待既不烧自动预算，也不被自动预算削减。
constexpr std::chrono::milliseconds kDialogSettle{1000};

}  // namespace

// ---------------------------------------------------------------------------
// 生命周期状态机本体
// ---------------------------------------------------------------------------

ConsentReply RunMonitoredDialog(std::shared_ptr<DialogDriver> driver, uint64_t timeoutMs,
                                ConsentCancelToken* cancelToken) {
    ConsentReply reply;
    if (!driver) {
        reply.answer = ConsentAnswer::kInitFailed;
        return reply;
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = timeoutMs > 0
                              ? started + std::chrono::milliseconds(timeoutMs)
                              : std::chrono::steady_clock::time_point::max();
    driver->SetDeadline(timeoutMs > 0 ? std::optional<std::chrono::steady_clock::time_point>(deadline)
                                      : std::nullopt);

    std::mutex mutex;
    std::condition_variable cv;
    bool showDone = false;
    DialogResult result;
    std::optional<ConsentAnswer> forced;   // 超时 / 取消：先到的那个理由生效
    std::optional<std::chrono::steady_clock::time_point> closeDeadline;

    // 看守线程只做三件事：计时、读取消令牌、把关闭请求交给驱动（驱动再投递到 GUI 主线程）。
    // 它不碰控件，也不阻塞 GUI 事件循环；50 ms 的等待是条件变量超时，不是忙等。
    std::thread watcher;
    try {
        watcher = std::thread([&] {
            std::unique_lock<std::mutex> lock(mutex);
            for (;;) {
                if (showDone) return;
                const auto now = std::chrono::steady_clock::now();

                // 期限优先于取消：两者同在时仍按"没人按时回答"这一档收尾。
                if (!forced && timeoutMs > 0 && now >= deadline) {
                    forced = ConsentAnswer::kTimedOut;
                    closeDeadline = now + kDialogCloseGrace;
                }
                if (!forced && cancelToken && cancelToken->Cancelled()) {
                    forced = ConsentAnswer::kCancelled;
                    closeDeadline = now + kDialogCloseGrace;
                }

                if (forced && closeDeadline && now < *closeDeadline) {
                    const DialogCloseReason reason = *forced == ConsentAnswer::kTimedOut
                                                         ? DialogCloseReason::kTimedOut
                                                         : DialogCloseReason::kCancelled;
                    // 宽限期内每个切片重试：窗口可能刚创建、事件队列还没转起来，
                    // 第一次投递不一定找得到可关的窗口。
                    lock.unlock();
                    driver->RequestClose(reason);
                    lock.lock();
                }

                cv.wait_for(lock, kDialogPollSlice, [&] { return showDone; });
            }
        });
    } catch (...) {
        reply.answer = ConsentAnswer::kInitFailed;
        return reply;
    }

    // Show() 在 GUI 主线程上阻塞到窗口关闭并销毁；这里拿到的是窗口真的收尾之后的结果。
    try {
        result = driver->Show();
    } catch (...) {
        // 驱动把框弹崩了：没有人能回答"是"，按初始化失败处理。异常绝不跑出本函数。
        result.answer = ConsentAnswer::kInitFailed;
        result.nativeError = kNativeErrorNone;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        showDone = true;
    }
    cv.notify_all();
    watcher.join();

    // 弹都没弹出去比"没人回答"更具体：即使期限也到了，仍报初始化失败。
    if (result.answer == ConsentAnswer::kInitFailed) {
        reply.answer = ConsentAnswer::kInitFailed;
        reply.nativeError = result.nativeError;
        return reply;
    }

    if (forced) {
        // 期限 / 取消先落地：窗口之后才带回来的"是"也改不了这个结局。
        reply.answer = *forced;
        return reply;
    }

    reply.answer = result.answer;
    if (reply.answer == ConsentAnswer::kAccepted) {
        // 关闭动画缓冲：见 kDialogSettle 的注释。此时对话框已经销毁，没有控件会被冻住。
        std::this_thread::sleep_for(kDialogSettle);
    }
    return reply;
}

// ---------------------------------------------------------------------------
// DesktopPermit
// ---------------------------------------------------------------------------

bool DesktopPermit::Covers(const PhysRect& area) const {
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
    // 调用方必须显式给真机枚举或假布局。空函数是调用点写错：按空拓扑处理，
    // 绝不在这里偷偷去问某个平台的屏幕接口。
    if (!topology_) topology_ = [] { return std::vector<ScreenInfo>{}; };
}

uint64_t ConsentGate::TopologyFingerprint() const { return Fingerprint(topology_()); }

void ConsentGate::SetTargetArea(const std::wstring& targetKey, const PhysRect& area) {
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
    // 历史副本这一句与目标清单、区域一起进同一轮问答：人点头批准的是"这些目标、这些输出、
    // 并且程序自己那份目录里还会留一份持久副本"，不是只批准了前两样。它不参与区域与拓扑的
    // 变化判定（一次运行里不会中途变），所以不必进快照那份。
    q.historyCopy = config_.historyCopy;
    return q;
}

Diagnostic ConsentGate::Denied(const wchar_t* path, const std::wstring& targetKey, PixelScope scope,
                               ConsentAnswer answer, NativeErrorCode nativeError) const {
    const bool desktop = scope == PixelScope::kDesktop;
    const bool timedOut = answer == ConsentAnswer::kTimedOut;
    const bool unavailable = answer == ConsentAnswer::kInitFailed;
    Diagnostic d;
    // 三种"没有得到同意"各有各的下一步：人答了否 / 关框 / 程序取消（要换条件或换人来答）、
    // 框根本弹不出来（要换一个能弹框的会话）、以及期限到点没人答（可能只是人暂时不在，
    // 再来一次也许就有人）。三者同为退出码 6，但 code 不同，调用方才不用猜。
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
    d.win32 = nativeError;
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

bool ConsentGate::DesktopGrantedFor(const std::wstring& targetKey, const PhysRect& area,
                                    PhysRect* approved) const {
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
        if (err) *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyAnswer_,
                               lastDenyNativeError_);
        return false;
    }
    if (!NeedsHumanConsent(path, config_.yes)) return true;  // 带 --yes 的可靠窗口路径
    if (windowLevel_ == Level::kGranted) return true;
    if (windowLevel_ == Level::kRefused) {
        if (err) *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyAnswer_,
                               lastDenyNativeError_);
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
    lastDenyAnswer_ = reply.answer;
    lastDenyNativeError_ = reply.nativeError;
    if (err) {
        *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyAnswer_,
                      lastDenyNativeError_);
    }
    return false;
}

bool ConsentGate::AuthorizeDesktop(const wchar_t* path, const std::wstring& targetKey,
                                   const PhysRect& area, std::optional<DesktopPermit>* out,
                                   Diagnostic* err) {
    if (Refused()) {   // 这一级或另一级被人否过（也包括弹不出框）：不再问第二遍，也不再截
        if (err) *err = Denied(path, targetKey, PixelScope::kDesktop, lastDenyAnswer_,
                               lastDenyNativeError_);
        return false;
    }
    PhysRect approved{};
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
        lastDenyAnswer_ = reply.answer;
        lastDenyNativeError_ = reply.nativeError;
        if (err) {
            *err = Denied(path, targetKey, PixelScope::kDesktop, lastDenyAnswer_,
                          lastDenyNativeError_);
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
        if (err) *err = Denied(path, targetKey, PixelScope::kDesktop, ConsentAnswer::kDeclined,
                               kNativeErrorNone);
        return false;
    }
    if (out) *out = DesktopPermit::Issue(targetKey, approved);
    return true;
}

}  // namespace ecapture
