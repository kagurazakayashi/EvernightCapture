#pragma once
// 跨平台核心：一次截图授权的状态机（程序确认层）与系统授权层（Portal）的预留状态。
//
// 为什么要单独一份纯状态机：授权这件事最容易被写成"界面调用返回了 true 就算同意"。
// 本文件把两件事分开：
//   1. **界面返回的是一个回答**（同意 / 拒绝 / 关框 / 取消 / 超时 / 弹不出来）；
//   2. **授权是一份可以被撤销的状态**，只有走到 kGranted 才算有许可，而 kGranted 必须先经过
//      "人答了是" + "本次绑定仍然成立"两次事件；只有前一半时状态仍是 kWaiting。
// 于是"界面调用已返回"永远不会被简化为授权成功，测试也可以只对着这份转移表逐条判，
// 不需要真的弹框、不需要真的取像素。
//
// 八种状态（与任务口径一一对应）：
//   kNotRequested 还没有问过：既没有许可，也没有拒绝。
//   kWaiting      有人在回答这一步（弹框在世 / 已答是但绑定还没核实）。**不是许可**。
//   kGranted      只有这一档是有许可的（kProgramConfirm 这一层）。
//   kDenied       人明确拒绝，或人按 X / Esc 关掉了框（关闭归入拒绝，理由另记）。
//   kCancelled    程序主动取消（取消令牌），不是人的回答。
//   kTimedOut     等待预算到点没人答；迟到的"是"不会把它改回 kGranted。
//   kStale        曾经批准，但绑定它的目标 / 范围 / 布局 / 身份已经变了：这一份许可作废。
//                 kStale 不等于人拒绝，也不等于机制故障：调用方可以带着新事实重新问一次。
//   kError        机制层没有给出可信结论（GUI 初始化失败、驱动抛异常、系统授权层不可用）。
//                 kError 与 kDenied 一样会停掉本次请求剩下的采集，但诊断码不同。
//
// 系统授权层（Wayland 的 XDG Desktop Portal）是**独立的一层**，见下方的 SystemAuthState：
// 程序确认不能冒充系统授权，--yes 也不能改变"这一层需不需要"；本步只把状态与判据预留好，
// 真正的 Portal 驱动留给后端那一步。Windows / X11 上这一层是 kNotRequired。

#include <cstdint>

#include "core/Authorization.h"   // GrantLayer / RequiredGrantLayers
#include "core/Platform.h"        // PlatformKind / BuildPlatform（GateConfig 的默认要求层）

namespace ecapture {

// 程序确认层的状态。取值只增不改名（测试与将来的诊断会按机器名分支）。
enum class ConsentState : uint32_t {
    kNotRequested = 0,
    kWaiting = 1,
    kGranted = 2,
    kDenied = 3,
    kCancelled = 4,
    kTimedOut = 5,
    kStale = 6,
    kError = 7,
};

// 机器名（ASCII，不随 --lang 变）。
inline const char* ConsentStateName(ConsentState state) {
    switch (state) {
        case ConsentState::kNotRequested: return "not_requested";
        case ConsentState::kWaiting: return "waiting";
        case ConsentState::kGranted: return "granted";
        case ConsentState::kDenied: return "denied";
        case ConsentState::kCancelled: return "cancelled";
        case ConsentState::kTimedOut: return "timed_out";
        case ConsentState::kStale: return "stale";
        case ConsentState::kError: return "error";
    }
    return "error";
}

// 这份许可凭什么给出。kYesWindowContent 是 --yes 那条例外，它只可能出现在窗口内容路径上；
// 其余情况一律是真人答过"是"。留成字段是为了让结果与诊断能说清"这次没弹框不等于没授权"。
enum class ConsentBasis : uint32_t {
    kNone = 0,
    kHumanAccepted = 1,
    kYesWindowContent = 2,
};

inline const char* ConsentBasisName(ConsentBasis basis) {
    switch (basis) {
        case ConsentBasis::kNone: return "none";
        case ConsentBasis::kHumanAccepted: return "human_accepted";
        case ConsentBasis::kYesWindowContent: return "yes_window_content";
    }
    return "none";
}

// 驱动状态机的事件。kAccepted 只是"人答了是"——它把状态推进到"已接受、待核实"，
// 仍然停在 kWaiting；必须再来一个 kVerified（本次绑定核实通过）才到 kGranted。
enum class ConsentEvent : uint32_t {
    kRequested = 0,
    kAccepted = 1,
    kDeclined = 2,
    kClosed = 3,
    kCancelled = 4,
    kTimedOut = 5,
    kFailed = 6,
    kVerified = 7,
    kInvalidated = 8,
};

inline const char* ConsentEventName(ConsentEvent event) {
    switch (event) {
        case ConsentEvent::kRequested: return "requested";
        case ConsentEvent::kAccepted: return "accepted";
        case ConsentEvent::kDeclined: return "declined";
        case ConsentEvent::kClosed: return "closed";
        case ConsentEvent::kCancelled: return "cancelled";
        case ConsentEvent::kTimedOut: return "timed_out";
        case ConsentEvent::kFailed: return "failed";
        case ConsentEvent::kVerified: return "verified";
        case ConsentEvent::kInvalidated: return "invalidated";
    }
    return "failed";
}

// 一档状态对"本次请求还能不能继续"意味着什么。只有这三种会被拒绝语义粘住并停掉整批：
// 人拒绝（含关框）、程序取消、等待超时、机制错误。kStale 不在此列 —— 它要求重新确认，
// 而不是把整批判死。
inline bool ConsentStateBlocks(ConsentState state) {
    return state == ConsentState::kDenied || state == ConsentState::kCancelled ||
           state == ConsentState::kTimedOut || state == ConsentState::kError;
}

// 程序确认这一层有没有许可。kWaiting（含"已答是但尚未核实"）一律不算。
inline bool ConsentStateGrants(ConsentState state) { return state == ConsentState::kGranted; }

// 一次授权的纯状态机。不弹框、不碰系统、不持有任何资源；ConsentGate 为窗口与桌面两级各用一台。
class ConsentStateMachine {
public:
    ConsentState state() const { return state_; }
    ConsentBasis basis() const { return basis_; }

    // 已经收到"人答了是"，但尚未收到绑定核实：这正是"界面调用已返回"的那一档。
    bool accepted_pending() const { return acceptedPending_; }
    bool waiting() const { return state_ == ConsentState::kWaiting; }
    bool granted() const { return state_ == ConsentState::kGranted; }
    bool blocks() const { return ConsentStateBlocks(state_); }
    bool can_request() const {
        return state_ == ConsentState::kNotRequested || state_ == ConsentState::kStale;
    }

    // 开始问人。只有"没问过"或"曾经批准但已失效"两档可以重新开始；
    // 被拒绝 / 取消 / 超时 / 机制错误的请求不会被一句"再问一次"洗成新的等待。
    void Request() {
        if (!can_request()) return;
        state_ = ConsentState::kWaiting;
        acceptedPending_ = false;
        basis_ = ConsentBasis::kNone;
    }

    // 把一次回答 / 核实结果 / 失效通知推进状态机。
    void Resolve(ConsentEvent event) {
        switch (event) {
            case ConsentEvent::kRequested:
                Request();
                return;
            case ConsentEvent::kAccepted:
                // 关键的一步：只记录"已答是"，状态**仍是等待**。
                if (state_ == ConsentState::kWaiting) acceptedPending_ = true;
                return;
            case ConsentEvent::kVerified:
                if (state_ == ConsentState::kWaiting && acceptedPending_) {
                    state_ = ConsentState::kGranted;
                    basis_ = ConsentBasis::kHumanAccepted;
                    acceptedPending_ = false;
                }
                return;
            case ConsentEvent::kInvalidated:
                if (state_ == ConsentState::kGranted || state_ == ConsentState::kWaiting) {
                    state_ = ConsentState::kStale;
                    acceptedPending_ = false;
                    basis_ = ConsentBasis::kNone;
                }
                return;
            case ConsentEvent::kDeclined:
            case ConsentEvent::kClosed:
                if (state_ == ConsentState::kWaiting || state_ == ConsentState::kNotRequested) {
                    state_ = ConsentState::kDenied;
                    acceptedPending_ = false;
                    basis_ = ConsentBasis::kNone;
                }
                return;
            case ConsentEvent::kCancelled:
                if (state_ == ConsentState::kWaiting) {
                    state_ = ConsentState::kCancelled;
                    acceptedPending_ = false;
                    basis_ = ConsentBasis::kNone;
                }
                return;
            case ConsentEvent::kTimedOut:
                if (state_ == ConsentState::kWaiting) {
                    state_ = ConsentState::kTimedOut;
                    acceptedPending_ = false;
                    basis_ = ConsentBasis::kNone;
                }
                return;
            case ConsentEvent::kFailed:
                // 机制错误也只在等待期成立；已经批准过的一次不会因为后来一次失败被抹掉
                //（那次失败属于新的调用点，由新的请求重新开始）。
                if (state_ == ConsentState::kWaiting) {
                    state_ = ConsentState::kError;
                    acceptedPending_ = false;
                    basis_ = ConsentBasis::kNone;
                }
                return;
        }
    }

    // --yes 那条例外：不经过人，但**必须**由调用方先核实能力与目标。状态机只接受这一种
    // 非人的来源；kNone 或 Human 都不该走这里（防止"忘了传事实就默认放行"）。
    void GrantWithoutHuman(ConsentBasis basis) {
        if (basis != ConsentBasis::kYesWindowContent) return;
        if (state_ == ConsentState::kGranted) return;
        // 调用方在走 --yes 例外之前会先 Request() 一次（于是这里是 waiting）；
        // 也允许从未请求 / 已失效两档直接授予。仍不接受"人答了是但未核实"那一档。
        if (!can_request() && state_ != ConsentState::kWaiting) return;
        if (acceptedPending_) return;
        state_ = ConsentState::kGranted;
        acceptedPending_ = false;
        basis_ = basis;
    }

    // 只在测试与"换一个全新请求"时使用：把这一级清回未请求。
    void Reset() {
        state_ = ConsentState::kNotRequested;
        acceptedPending_ = false;
        basis_ = ConsentBasis::kNone;
    }

private:
    ConsentState state_ = ConsentState::kNotRequested;
    ConsentBasis basis_ = ConsentBasis::kNone;
    bool acceptedPending_ = false;
};

// ---------------------------------------------------------------------------
// 系统授权层（Wayland / XDG Desktop Portal）。
//
// 这一层与程序确认并列，不是它的替代：Wayland 的 RequiredGrantLayers 同时含两层，
// 任何一层缺失都不能授权。Windows 与 X11 默认是 kNotRequired；显式走 Portal 授权流程时
// 则两层都要求。Portal 驱动由平台层注入，状态只在本进程、本次请求内有效。
//
// kCancelledOrDenied 是**合并状态**：XDG Desktop Portal 的 Request.Response 只用
// response=1 报"用户取消了交互"，系统并不告诉客户端那是拒绝还是取消。驱动不能把
// 它拆成 kDenied / kCancelled 再编一个原因。
enum class SystemAuthState : uint32_t {
    kNotRequired = 0,       // 这个平台 / 这次请求不需要系统授权
    kNotRequested = 1,      // 需要，但还没有开始
    kWaiting = 2,           // 系统授权 / 来源选择界面在世
    kGranted = 3,           // 系统授权与来源选择已取得，且绑定本次目标
    kDenied = 4,            // 系统明确报告拒绝（接口能区分时才使用）
    kCancelledOrDenied = 5, // 系统无法区分拒绝与取消：准确的合并状态
    kCancelled = 6,         // 程序主动取消（不是人的回答）
    kTimedOut = 7,          // 等待系统授权到点
    kUnavailable = 8,       // 会话总线 / 门户服务不可用
    kUnsupported = 9,       // 当前门户接口、版本或来源能力不支持本次请求
    kServiceLost = 10,      // 门户服务在请求中途消失
    kStale = 11,            // 系统授权绑定失效（来源 / 会话 / 目标变了）
    kError = 12,            // 协议或机制错误（不等同于用户拒绝）
};

inline const char* SystemAuthStateName(SystemAuthState state) {
    switch (state) {
        case SystemAuthState::kNotRequired: return "not_required";
        case SystemAuthState::kNotRequested: return "not_requested";
        case SystemAuthState::kWaiting: return "waiting";
        case SystemAuthState::kGranted: return "granted";
        case SystemAuthState::kDenied: return "denied";
        case SystemAuthState::kCancelledOrDenied: return "cancelled_or_denied";
        case SystemAuthState::kCancelled: return "cancelled";
        case SystemAuthState::kTimedOut: return "timed_out";
        case SystemAuthState::kUnavailable: return "unavailable";
        case SystemAuthState::kUnsupported: return "unsupported";
        case SystemAuthState::kServiceLost: return "service_lost";
        case SystemAuthState::kStale: return "stale";
        case SystemAuthState::kError: return "error";
    }
    return "error";
}

// 这一层本身有没有**取得**：只有 kGranted。kNotRequired 是"这个平台不需要它"，由
// ConsentLayersSatisfy 按 required 决定要不要看这一眼——它不能被读成"需要却也满足了"。
inline bool SystemAuthSatisfied(SystemAuthState state) {
    return state == SystemAuthState::kGranted;
}

// 系统授权层这一档会不会停掉整批请求。kStale 与 kNotRequested 不停（前者要重新取，后者是
// "本构建没有驱动"——由调用点立刻判失败，见 ConsentGate）。
inline bool SystemAuthBlocks(SystemAuthState state) {
    return state == SystemAuthState::kDenied ||
           state == SystemAuthState::kCancelledOrDenied ||
           state == SystemAuthState::kCancelled ||
           state == SystemAuthState::kTimedOut ||
           state == SystemAuthState::kUnavailable ||
           state == SystemAuthState::kUnsupported ||
           state == SystemAuthState::kServiceLost ||
           state == SystemAuthState::kError;
}

// 两层授权的合并判据（**唯一一份**）：required 来自 RequiredGrantLayers，
// 程序确认层必须真的 kGranted，系统授权层必须满足（不需要 / kGranted）。
// 注意：--yes 只可能把程序确认层从"要问人"降成"窗口内容例外"，它不改变 required，
// 也不能让系统授权层被满足 —— 这条判据因此对 --yes 一无所知。
inline bool ConsentLayersSatisfy(uint32_t required, ConsentState program,
                                 SystemAuthState system) {
    const bool needProgram = (required & kGrantProgramConfirm) != 0;
    const bool needSystem = (required & kGrantSystemAuthorization) != 0;
    if (needProgram && program != ConsentState::kGranted) return false;
    if (needSystem && !SystemAuthSatisfied(system)) return false;
    return true;
}

// 这个平台族要求的授权层（GateConfig 的默认值；测试可以显式覆盖）。
inline uint32_t DefaultRequiredGrantLayers() {
    const PlatformKind platform = BuildPlatform();
    if (platform == PlatformKind::kWindows || platform == PlatformKind::kLinuxX11) {
        return RequiredGrantLayers(platform);
    }
    if (platform == PlatformKind::kLinuxWayland) return RequiredGrantLayers(platform);
    // 认不出来的平台不凭空添加系统授权层，但也不把已有要求抹掉：按"只有程序确认"登记，
    // 平台身份未知这件事由能力那一路如实报 unknown，不在这里猜。
    return kGrantProgramConfirm;
}

}  // namespace ecapture
