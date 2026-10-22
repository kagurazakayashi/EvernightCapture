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

// ---------------------------------------------------------------------------
// ConsentGate
// ---------------------------------------------------------------------------

ConsentGate::ConsentGate(GateConfig config, IConsentPrompt& prompt, TopologyProvider topology)
    : config_(std::move(config)), prompt_(prompt), topology_(std::move(topology)) {
    // 调用方必须显式给真机枚举或假布局。空函数是调用点写错：按空拓扑处理，
    // 绝不在这里偷偷去问某个平台的屏幕接口。
    if (!topology_) topology_ = [] { return std::vector<ScreenInfo>{}; };
    // 系统授权层：这个平台不需要它时是 kNotRequired；需要但本构建还没有驱动时是
    // kNotRequested —— 后者会让任何一次授权 fail-closed，绝不用程序确认冒充系统授权。
    systemState_ = SystemAuthorizationRequired() ? SystemAuthState::kNotRequested
                                                 : SystemAuthState::kNotRequired;
}

uint64_t ConsentGate::TopologyFingerprint() const {
    // 指纹本体只在 ScreenMatch.cpp 一处：根窗口范围、逻辑显示器、输出设备、镜像与旋转
    // 全部算进去。这样"人确认前后只是换了镜像输出 / 换了旋转但矩形碰巧一样"也会让旧授权失效。
    return ScreenTopologyFingerprint(topology_());
}

void ConsentGate::SetTargetArea(const std::wstring& targetKey, const PhysRect& area) {
    for (GateTarget& t : config_.targets) {
        if (t.key == targetKey) {
            t.area = area;
            return;
        }
    }
    // 清单外的目标 = 调用方扩大了范围，这里不接受：宁可让它因为"没有对应授权"而重新问一次。
}

void ConsentGate::ApplyAnswer(ConsentStateMachine* machine, ConsentAnswer answer) {
    switch (answer) {
        case ConsentAnswer::kAccepted: machine->Resolve(ConsentEvent::kAccepted); break;
        case ConsentAnswer::kDeclined: machine->Resolve(ConsentEvent::kDeclined); break;
        case ConsentAnswer::kClosed: machine->Resolve(ConsentEvent::kClosed); break;
        case ConsentAnswer::kTimedOut: machine->Resolve(ConsentEvent::kTimedOut); break;
        case ConsentAnswer::kCancelled: machine->Resolve(ConsentEvent::kCancelled); break;
        case ConsentAnswer::kInitFailed: machine->Resolve(ConsentEvent::kFailed); break;
    }
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
    // 来源：调用方自述（不可信）与程序核实的事实（可信）分两路交出去，渲染层分别带标签。
    q.requestSource = config_.requestSource;
    q.requestSourceGiven = config_.requestSourceGiven;
    q.programPath = config_.programPath;
    q.programVersion = config_.programVersion;
    q.programPid = config_.programPid;
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

Diagnostic ConsentGate::SystemLayerUnavailable(const wchar_t* path,
                                               const std::wstring& targetKey) const {
    // 这一档不是"人拒绝"，也不是"对话框弹不出来"：这个平台要求系统授权层，而本构建还没有
    // 那一层驱动（Portal 接入之前）。宁可一张都不截，也不拿程序确认冒充系统授权。
    Diagnostic d;
    d.code = codes::kConsentUnavailable;
    d.message = Msg(L"consent.system_layer_required");
    d.option = L"--capture";
    d.value = path ? path : std::wstring();
    d.hint = Msg(L"consent.system_layer_hint");
    d.target = targetKey;
    d.stage = stages::kConsent;
    return d;
}

bool ConsentGate::EnsureSystemLayer(PixelScope scope, const wchar_t* path,
                                    const std::wstring& targetKey, Diagnostic* err) {
    (void)scope;
    if (!SystemAuthorizationRequired()) {
        systemState_ = SystemAuthState::kNotRequired;
        return true;
    }
    if (systemState_ == SystemAuthState::kGranted) return true;
    if (systemState_ == SystemAuthState::kNotRequired) systemState_ = SystemAuthState::kNotRequested;
    if (systemState_ == SystemAuthState::kUnavailable || systemState_ == SystemAuthState::kError ||
        SystemAuthBlocks(systemState_)) {
        if (err) *err = SystemLayerUnavailable(path, targetKey);
        return false;
    }
    // 需要系统授权层但本构建 / 本调用还没有把它取得：fail-closed，绝不把程序确认当两层。
    // Portal 驱动接进来之后会在同一个状态字段上推进 kWaiting -> kGranted / kDenied / ...
    systemState_ = SystemAuthState::kUnavailable;
    lastDenyAnswer_ = ConsentAnswer::kInitFailed;
    lastDenyNativeError_ = kNativeErrorNone;
    if (err) *err = SystemLayerUnavailable(path, targetKey);
    return false;
}

bool ConsentGate::ListedTarget(const std::wstring& targetKey) const {
    for (const GateTarget& t : config_.targets) {
        if (t.key == targetKey) return true;
    }
    return false;
}

void ConsentGate::GrantSnapshot(const ConsentSnapshot& snap) {
    // 冻结的就是人刚看过的那一份（或 --yes 那一次已经交来的清单）：
    //   * desktopAreas_ 带区域，之后目标挪了位置 / 矩形变大要重新问；
    //   * windowKeys_ 只记"哪些目标在清单里"，窗口内容路径不绑定屏幕区域，但批量范围仍限定于此。
    desktopAreas_.clear();
    windowKeys_.clear();
    for (const GateTarget& t : snap.listed) {
        desktopAreas_.push_back(t);
        windowKeys_.push_back(t.key);
    }
    topologyAtAsk_ = snap.topology;  // 基线是弹框**之前**那一次取值，不是应答之后首次所见
}

bool ConsentGate::WindowGrantedFor(const std::wstring& targetKey) const {
    if (!window_.granted()) return false;
    for (const std::wstring& key : windowKeys_) {
        if (key == targetKey) return true;
    }
    return false;   // 清单里没有这个目标 = 范围被扩大，重新问
}

bool ConsentGate::DesktopGrantedFor(const std::wstring& targetKey, const PhysRect& area,
                                    PhysRect* approved) const {
    if (!desktop_.granted()) return false;
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
                                  Diagnostic* err, const YesEligibility& yes,
                                  const ConsentBindingVerifier& verify) {
    Diagnostic scratch;   // 调用方可以不要诊断，但本函数每一档都要写一条
    if (!err) err = &scratch;
    // 1) 系统授权层先判：需要而不可得时，程序确认做得再好也不授权。
    if (!EnsureSystemLayer(PixelScope::kWindowContent, path, targetKey, err)) return false;
    // 2) 已经有人答过否 / 取消 / 超时 / 机制错误：整批停在这里，不重复骚扰。
    if (Refused()) {
        *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyAnswer_,
                      lastDenyNativeError_);
        return false;
    }
    // 3) 这一入口只接受窗口内容路径。拿窗口那一级的许可去放行桌面像素是越界，直接拒绝，
    //    并把这一级推进到错误档（调用点写错时不能留下一个"看起来成功"的状态）。
    if (ScopeOf(path) != PixelScope::kWindowContent) {
        window_.Reset();
        window_.Request();
        window_.Resolve(ConsentEvent::kFailed);
        *err = Denied(path, targetKey, PixelScope::kWindowContent, ConsentAnswer::kDeclined,
                      kNativeErrorNone);
        return false;
    }
    // 4) 已经批准过：只有清单里列过的目标才能复用；清单外 = 范围扩大，重新走确认。
    if (window_.granted() && WindowGrantedFor(targetKey)) {
        if (ConsentLayersSatisfy(config_.requiredGrantLayers, window_.state(), systemState_)) {
            return true;
        }
    }
    if (window_.granted() && !WindowGrantedFor(targetKey)) window_.Resolve(ConsentEvent::kInvalidated);

    // 5) --yes 例外：必须同时满足"目标是单扇窗口 / 能力已确认 / 身份已复核"，而且目标在本次
    //    清单里、绑定核实也通过。缺任何一条都退回正常确认，绝不因为给了 --yes 就放宽。
    if (config_.yes && yes.Eligible() && ListedTarget(targetKey)) {
        window_.Request();
        if (!verify || !verify(err)) {
            window_.Resolve(ConsentEvent::kInvalidated);
            if (!err || err->code.empty()) *err = Stale(path, targetKey);
            return false;
        }
        // 这里不是"人答了是"：按 yes_window_content 这一种非人来源授予（状态机只接受这一种）。
        window_.GrantWithoutHuman(ConsentBasis::kYesWindowContent);
        GrantSnapshot(TakeSnapshot());
        if (!WindowGrantedFor(targetKey)) {
            *err = Denied(path, targetKey, PixelScope::kWindowContent, ConsentAnswer::kDeclined,
                          kNativeErrorNone);
            return false;
        }
        return true;
    }

    // 6) 正常确认：从弹框前的快照问起（清单与文案同源）。窗口内容路径不绑定屏幕拓扑，
    //    但"点完是之后目标还是不是那一扇"仍要在签发之前核实一次（verify），
    //    流水线随后每次真实取帧之前还会再做（见 CaptureOneChannel）。
    if (!window_.can_request()) {
        *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyAnswer_,
                      lastDenyNativeError_);
        return false;
    }
    window_.Request();
    ++windowAsks_;
    const ConsentSnapshot snap = TakeSnapshot();
    const ConsentQuestion q = MakeQuestion(path, PixelScope::kWindowContent, snap);
    ConsentReply reply;
    try {
        reply = AskWithBudgetPause(config_.autoBudget, prompt_, q);
    } catch (...) {
        // 应答器抛异常：异常照旧穿出去（调用方要看见），但这一级不能停在"等待"——
        // 否则下一次调用会被 can_request() 拦下，像是一次永远答不完的弹框。
        window_.Reset();
        throw;
    }
    ApplyAnswer(&window_, reply.answer);
    if (!window_.waiting() || !window_.accepted_pending()) {
        lastDenyAnswer_ = reply.answer;
        lastDenyNativeError_ = reply.nativeError;
        *err = Denied(path, targetKey, PixelScope::kWindowContent, lastDenyAnswer_,
                      lastDenyNativeError_);
        return false;
    }
    // 人答了是，但**还不是授权**：先做这一次绑定核实。核实问不出来 / 目标变了 / 没有可用的
    // 核实手段时，这一问以 stale 收尾，一张都不截。
    if (!verify || !verify(err)) {
        window_.Resolve(ConsentEvent::kInvalidated);
        if (!err || err->code.empty()) *err = Stale(path, targetKey);
        return false;
    }
    window_.Resolve(ConsentEvent::kVerified);
    GrantSnapshot(snap);
    if (!WindowGrantedFor(targetKey)) {
        // 人在框上看到的清单里根本没有这次要取的这个目标：不给自己放行。
        *err = Denied(path, targetKey, PixelScope::kWindowContent, ConsentAnswer::kDeclined,
                      kNativeErrorNone);
        return false;
    }
    return ConsentLayersSatisfy(config_.requiredGrantLayers, window_.state(), systemState_);
}

bool ConsentGate::AuthorizeDesktop(const wchar_t* path, const std::wstring& targetKey,
                                   const PhysRect& area, std::optional<DesktopPermit>* out,
                                   Diagnostic* err) {
    Diagnostic scratch;   // 同上：诊断指针为空也不在内部解引用空指针
    if (!err) err = &scratch;
    // 桌面路径永远要人确认：--yes 在这里不参与任何分支。
    if (!EnsureSystemLayer(PixelScope::kDesktop, path, targetKey, err)) return false;
    if (Refused()) {   // 这一级或另一级被人否过（也包括弹不出框）：不再问第二遍，也不再截
        *err = Denied(path, targetKey, PixelScope::kDesktop, lastDenyAnswer_,
                      lastDenyNativeError_);
        return false;
    }
    PhysRect approved{};
    if (desktop_.granted() && DesktopGrantedFor(targetKey, area, &approved)) {
        if (out) *out = DesktopPermit::Issue(targetKey, approved);
        return true;
    }
    if (desktop_.granted()) {
        // 之前批过，但这次的目标 / 区域 / 拓扑不在当时那份清单里 —— 授权作废，重新问一次。
        desktop_.Resolve(ConsentEvent::kInvalidated);
    }
    if (!desktop_.can_request()) {
        *err = Denied(path, targetKey, PixelScope::kDesktop, lastDenyAnswer_,
                      lastDenyNativeError_);
        return false;
    }

    desktop_.Request();
    ++desktopAsks_;
    // 弹框前冻结这一轮的授权快照：框上写的目标、区域、输出与拓扑基线全部出自这一次取值。
    const ConsentSnapshot snap = TakeSnapshot();
    const ConsentQuestion q = MakeQuestion(path, PixelScope::kDesktop, snap);
    ConsentReply reply;
    try {
        reply = AskWithBudgetPause(config_.autoBudget, prompt_, q);
    } catch (...) {
        desktop_.Reset();
        throw;
    }
    ApplyAnswer(&desktop_, reply.answer);
    if (!desktop_.waiting() || !desktop_.accepted_pending()) {
        lastDenyAnswer_ = reply.answer;
        lastDenyNativeError_ = reply.nativeError;
        *err = Denied(path, targetKey, PixelScope::kDesktop, lastDenyAnswer_,
                      lastDenyNativeError_);
        return false;
    }
    // 人点头（含关闭动画缓冲）之后、签发授权之前，重新查询一遍拓扑，与**弹框前**那份快照比。
    // 这一步是 F06 的要害：确认期间热插拔、拔线、改分辨率或挪了屏幕位置时，人在框上最后
    // 看到的还是旧布局 —— 拿应答之后第一次所见的新布局当基线，等于替旧请求追认了新拓扑。
    // 判据是停，不是重试：这一问一张都不采、不签发凭证，也不自动按新布局再弹一次框
    // （同一次调用里反复弹框就是无限重复问人）；调用方重来一遍时会在新的快照上重新问。
    if (TopologyFingerprint() != snap.topology) {
        desktop_.Resolve(ConsentEvent::kInvalidated);   // 没答"否"：批次不因这个失败而整批停止
        *err = Stale(path, targetKey);
        return false;
    }
    desktop_.Resolve(ConsentEvent::kVerified);
    GrantSnapshot(snap);
    if (!DesktopGrantedFor(targetKey, area, &approved)) {
        // 人在框上看到的清单里根本没有这次要取的这一块：不给自己放行。
        // 真机流水线的目标矩形在问之前刚量过，正常走不到这里；走到就是调用点写错了。
        *err = Denied(path, targetKey, PixelScope::kDesktop, ConsentAnswer::kDeclined,
                      kNativeErrorNone);
        return false;
    }
    if (out) *out = DesktopPermit::Issue(targetKey, approved);
    return ConsentLayersSatisfy(config_.requiredGrantLayers, desktop_.state(), systemState_);
}

}  // namespace ecapture
