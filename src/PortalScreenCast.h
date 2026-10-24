#pragma once
// 跨平台模型：XDG Desktop Portal ScreenCast 的授权 / 来源选择那一段。
//
// 本层只描述"要向系统门户请求哪一种来源、最后拿到了什么事实"，不 include libdbus、
// 不持有对象路径或文件描述符，也不读一个像素。真实 D-Bus 驱动在
// src/platform/linux/DBusPortalScreenCast.cpp；测试可注入 IPortalScreenCast 的假实现。
//
// auth-only 动作只做到 Start 成功、校验来源类型并关闭会话；--portal-capture 则在
// closeSessionOnSuccess=false 时保留同一会话，随后调用 OpenPipeWireRemote() 并把 fd
// 交给 PipeWire 后端。Portal 返回会话/流不等于取得图像：只有有效帧完成交付后才算成功。
//
// 关键约束（与 AGENTS.md《Wayland 提示与系统权限》同源）：
//   * 单窗口请求只申请 WINDOW 来源；屏幕请求只申请 MONITOR 来源，二者不互相冒充。
//   * multiple=false，只允许一个来源。Start 结果的 source_type 必须在 v3+ 可核实；
//     协议版本不足或属性缺失时停止，绝不把"拿到一个流"说成"条件已满足"。
//   * 不发送 restore_token，不设置 persist_mode，不保存 / 打印恢复令牌。
//   * parent_window 允许空字符串；没有可导出的父窗口时明确使用接口允许的无父窗口形式。

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ecapture {
namespace portal {

// org.freedesktop.portal.ScreenCast 的 AvailableSourceTypes / SelectSources.types 位值。
enum class SourceKind : uint32_t {
    kMonitor = 1u,
    kWindow = 2u,
    kVirtual = 4u,
};

inline uint32_t SourceKindMask(SourceKind kind) { return static_cast<uint32_t>(kind); }

inline const char* SourceKindName(SourceKind kind) {
    switch (kind) {
        case SourceKind::kMonitor: return "monitor";
        case SourceKind::kWindow: return "window";
        case SourceKind::kVirtual: return "virtual";
    }
    return "unknown";
}

// 一次授权 / 选择尝试的结局。只有 kGranted 表示系统授权与来源选择都已完成；
// 即使这样也不代表拿到了图像。
enum class Outcome : uint32_t {
    kGranted = 0,
    // Request.Response 的 response=1 只说明"用户取消了交互"；系统不区分拒绝与取消，
    // 所以这里保留为合并状态，绝不编造 kDenied。
    kCancelledOrDenied = 1,
    kTimedOut = 2,
    kCancelled = 3,
    kUnavailable = 4,
    // 当前门户/版本/来源能力不支持这次请求（例如版本低于可核实 source_type 的 3，
    // 或 AvailableSourceTypes 没有请求的那一位）。
    kUnsupported = 5,
    // 会话 / 服务在请求过程中消失（NameOwnerChanged、连接断开），不是用户拒绝。
    kServiceLost = 6,
    // 协议回复不完整、请求句柄对不上、stream 数量或来源类型无法核实。
    kProtocolError = 7,
    // Request.Response 的 response=2：交互以其它方式结束，但没有可核实的细分原因。
    kRequestFailed = 8,
};

inline const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::kGranted: return "granted";
        case Outcome::kCancelledOrDenied: return "cancelled_or_denied";
        case Outcome::kTimedOut: return "timed_out";
        case Outcome::kCancelled: return "cancelled";
        case Outcome::kUnavailable: return "unavailable";
        case Outcome::kUnsupported: return "unsupported";
        case Outcome::kServiceLost: return "service_lost";
        case Outcome::kProtocolError: return "protocol_error";
        case Outcome::kRequestFailed: return "request_failed";
    }
    return "protocol_error";
}

// 稳定原因 token（只增不改名）。它解释 Outcome，不随 --lang 变。
namespace reason {
inline constexpr const char* kGranted = "portal_granted";
inline constexpr const char* kCancelledOrDenied = "portal_user_cancelled_or_denied";
inline constexpr const char* kAutomaticTimeout = "automatic_budget_expired";
inline constexpr const char* kInteractionTimeout = "interaction_timeout";
inline constexpr const char* kAppCancelled = "application_cancelled";
inline constexpr const char* kBusUnavailable = "session_bus_unavailable";
inline constexpr const char* kServiceUnavailable = "portal_service_unavailable";
inline constexpr const char* kServiceLost = "portal_service_lost";
inline constexpr const char* kInterfaceMissing = "screencast_interface_missing";
inline constexpr const char* kVersionUnverified = "screencast_version_unverified";
inline constexpr const char* kVersionTooLow = "screencast_version_too_low_for_source_verification";
inline constexpr const char* kSourceTypesUnavailable = "available_source_types_unavailable";
inline constexpr const char* kSourceTypeNotOffered = "requested_source_type_not_offered";
inline constexpr const char* kProtocolError = "portal_protocol_error";
inline constexpr const char* kRequestFailed = "portal_request_ended_without_reason";
inline constexpr const char* kSourceMismatch = "selected_source_type_mismatch";
inline constexpr const char* kMultipleStreams = "multiple_streams_not_requested";
inline constexpr const char* kStreamSourceUnverifiable = "stream_source_type_unverifiable";
inline constexpr const char* kSessionMissing = "session_handle_missing";
inline constexpr const char* kCursorModeUnavailable = "portal_cursor_mode_not_offered";
inline constexpr const char* kPipeWireRemoteUnavailable = "portal_open_pipewire_remote_failed";
}  // namespace reason

// Start 返回的元数据。不保存恢复令牌；nodeId 只在本会话内有效。
struct StreamInfo {
    uint32_t nodeId = 0;
    bool sourceTypeKnown = false;
    SourceKind sourceType = SourceKind::kWindow;
    bool positionKnown = false;
    int32_t x = 0;
    int32_t y = 0;
    bool sizeKnown = false;
    int32_t width = 0;
    int32_t height = 0;
    bool pipewireSerialKnown = false;
    uint64_t pipewireSerial = 0;   // v6：给后续 PipeWire 步骤定位节点，不是授权令牌
};

struct Selection {
    SourceKind requested = SourceKind::kWindow;
    bool exactlyOneStream = false;
    bool sourceTypeVerified = false;
    std::vector<StreamInfo> streams;
};

// SelectSources 的 cursor_mode 位值。kDefault 表示不设置该选项（Portal 官方默认 Hidden）。
enum class CursorMode : uint32_t {
    kDefault = 0u,
    kHidden = 1u,
    kEmbedded = 2u,
    kMetadata = 4u,
};

inline const char* CursorModeName(CursorMode mode) {
    switch (mode) {
        case CursorMode::kHidden: return "hidden";
        case CursorMode::kEmbedded: return "embedded";
        case CursorMode::kMetadata: return "metadata";
        case CursorMode::kDefault: return "default";
    }
    return "default";
}

struct AuthorizationResult {
    Outcome outcome = Outcome::kUnavailable;
    std::string reason = reason::kServiceUnavailable;

    uint32_t portalVersion = 0;
    bool versionKnown = false;
    uint32_t availableSourceTypes = 0;
    bool sourceTypesKnown = false;
    uint32_t availableCursorModes = 0;
    bool cursorModesKnown = false;
    uint32_t requestedSourceMask = 0;

    bool sessionCreated = false;
    bool requestCompleted = false;
    // 取消 / 超时 / 早期失败时由本程序发送 Request.Close()；成功响应本身已经结束请求。
    bool requestClosed = false;
    bool sessionClosed = false;
    bool sessionClosedByPortal = false;
    bool pipeWireRemoteOpened = false;
    CursorMode requestedCursorMode = CursorMode::kDefault;
    bool cursorModeRequested = false;
    // 只在驱动内部记账：超时 / 取消之后到达的成功响应一律忽略，不改变 outcome。
    bool lateResponseIgnored = false;

    // 本步骤的硬边界：只做授权与选择，不交付图像，也不持久化令牌。
    bool pixelsRead = false;
    bool imageDelivered = false;
    bool restoreTokenPersisted = false;

    Selection selection;
};

// 与 ConsentCancelToken 同形但独立：Portal 驱动不依赖 GUI / 授权状态机头。
class CancelToken {
public:
    void Cancel() noexcept { cancelled_.store(true); }
    bool Cancelled() const noexcept { return cancelled_.load(); }

private:
    std::atomic<bool> cancelled_{false};
};

struct Request {
    SourceKind source = SourceKind::kWindow;
    // xdg-desktop-portal 允许无父窗口时传空字符串。本项目是终端 CLI，没有可导出的
    // xdg_foreign handle 时只能传空；绝不伪造一个 Wayland / X11 标识。
    std::string parentWindow;
    // 自动处理预算（连接、Properties.Get、CreateSession、SelectSources、Start 的方法回复）。
    uint64_t automaticTimeoutMs = 5000;
    // 系统来源选择 / 授权窗口的人工等待预算。0 = 不限。它与 automaticTimeoutMs 分开计时。
    uint64_t interactionTimeoutMs = 0;
    CancelToken* cancelToken = nullptr;
    // 交互来源选择的 cursor_mode。kDefault 时不发送该选项，由 Portal 官方默认（Hidden）决定。
    CursorMode cursorMode = CursorMode::kDefault;
    // 本步骤结束即关闭会话。后续 PipeWire 交付步骤应传 false 并在流建立后自行关闭。
    bool closeSessionOnSuccess = true;
};

class IPortalScreenCast {
public:
    virtual ~IPortalScreenCast() = default;
    virtual AuthorizationResult AuthorizeAndSelect(const Request& request) = 0;

    // 取得当前授权会话的 PipeWire remote fd（>= 0，调用方拥有并负责 close）。
    // 默认实现用于注入替身 / 不支持这条路的驱动：不伪装成功。
    virtual bool OpenPipeWireRemote(int* fd, uint64_t timeoutMs, std::string* reason) {
        (void)timeoutMs;
        if (fd) *fd = -1;
        if (reason) *reason = reason::kPipeWireRemoteUnavailable;
        return false;
    }

    // 关闭 AuthorizeAndSelect 成功且 closeSessionOnSuccess=false 时保留的会话。
    // 默认空操作，保持旧替身与 auth-only 路径的编译/语义不变。
    virtual void CloseSession() {}
};

// ---------------------------------------------------------------------------
// Request / Response 关联器（纯逻辑，可直接离线逐条判）。
//
// xdg-desktop-portal 的 Request.Response 可能早于 D-Bus 方法回复到达；因此真实驱动必须
// **先安装信号匹配与过滤器**，再发方法调用。本类把"预期的 handle_token 路径、方法实际
// 返回的路径、提前到达的 Response、取消 / 超时之后迟到的成功"分开记：
//   * Response 先到：按信号自己的 object path 缓存，等方法回复确认实际请求路径后再配对；
//   * 方法实际返回的路径与预期不同：仍按实际路径配对（兼容旧版；调用方必须在发请求前
//     已经安装宽匹配过滤器）；
//   * 已经标记超时 / 取消：迟到的 Response 只记 lateResponseIgnored，绝不把结局改回成功。
// ---------------------------------------------------------------------------
class RequestTracker {
public:
    enum class State : uint32_t {
        kPending = 0,
        kSuccess = 1,
        kCancelledOrDenied = 2,
        kRequestFailed = 3,
        kTimedOut = 4,
        kCancelled = 5,
        kProtocolError = 6,
    };

    // 在发方法调用之前登记 handle_token 预期路径。
    void Expect(std::string expectedPath);
    // 方法回复给出实际请求路径。若此前该路径的 Response 已缓存，则立即完成配对。
    void OnMethodReply(std::string actualPath);
    // 过滤器收到 Request.Response(u response, a{sv} results)；调用方只应转交属于本请求的路径。
    void OnResponse(std::string path, uint32_t response);
    void MarkTimedOut();
    void MarkCancelled();
    void MarkCancelledOrDenied();
    void MarkProtocolError();

    State state() const { return state_; }
    bool pending() const { return state_ == State::kPending; }
    bool done() const { return state_ != State::kPending; }
    bool ResponseArrivedBeforeMethodReply() const { return responseBeforeReply_; }
    bool MethodReplied() const { return methodReplied_; }
    bool HandleChanged() const { return handleChanged_; }
    bool LateResponseIgnored() const { return lateResponseIgnored_; }
    const std::string& ExpectedPath() const { return expectedPath_; }
    const std::string& ActualPath() const { return actualPath_; }
    const std::string& MatchedResponsePath() const { return matchedResponsePath_; }

    // 过滤器用来判断一条 Response 是否可能属于本请求：预期路径，或已确认的实际路径。
    // 旧版在方法回复前用不可预测路径发信号时，驱动只能靠宽匹配器收到后按实际路径配对；
    // 本方法不把不相关客户端的 Response 写进缓存。
    bool AcceptsPath(const std::string& path) const;

    // 仅供测试 / 诊断：收到过多少条 Response 信号（包括迟到的）。
    int responsesSeen() const { return responsesSeen_; }

private:
    struct BufferedResponse {
        uint32_t code = 2;
    };
    void ApplyResponse(uint32_t code, const std::string& path);

    std::string expectedPath_;
    std::string actualPath_;
    std::string matchedResponsePath_;
    bool methodReplied_ = false;
    bool responseBeforeReply_ = false;
    bool handleChanged_ = false;
    bool lateResponseIgnored_ = false;
    int responsesSeen_ = 0;
    State state_ = State::kPending;
    std::vector<std::pair<std::string, BufferedResponse>> buffered_;
};

}  // namespace portal
}  // namespace ecapture
