#pragma once
// 跨平台核心：Linux 会话与运行能力的事实模型。
//
// 为什么单独一份：调用方（尤其是 AI）在截图之前必须先知道"这台机器现在到底是什么会话、
// 哪条路线真的连得上、版本够不够、要不要人点一下、这个构建有没有实现那条后端"。这四件事
// 彼此独立，折成一个 available/unavailable 会把下一步该做什么说没了：
//
//   1. **编译时包含**（compiled）：这个可执行文件里有没有那条后端的代码。没有就是没有，
//      "平台理论上做得到"不写成本工具支持。
//   2. **运行时可连接**（reachable）：这次真的去连了显示服务器 / D-Bus / 套接字。只有
//      连接结果算证据；DISPLAY、WAYLAND_DISPLAY、XDG_SESSION_TYPE、桌面名称都只是线索，
//      环境变量存在不等于服务可用，更不等于已经授权。
//   3. **功能版本满足**（versionSatisfied）：协议版本、Portal 接口版本、PipeWire 前提。
//      问不出来就是 unknown，不折成 yes 也不折成 no。
//   4. **需要用户交互**（interactionRequired）：Portal 的来源选择与系统授权必须由人完成。
//      只读查询本身**不**触发来源选择、不创建捕获会话、不读一个像素。
//   5. **当前尚未实现**（implemented）：查询层可以先把事实报出来，但后端还没接上时
//      status 只能是 not_implemented —— 绝不能因为"连接成功"就回答"可以截图"。
//
// 这份模型不 include 任何平台头，也不认识 X11 / Wayland / D-Bus 的原生句柄；平台层只交
// 已经收好的事实（连接成功与否、版本号、接口名），所有判断与机器名都在共用层一处完成。

#include <cstdint>
#include <string>
#include <vector>

#include "core/Error.h"       // Tri：一条问句的三值下场
#include "core/Platform.h"    // PlatformKind：平台族身份

namespace ecapture {

// 这次探测认定的会话种类。**只有实际连接结果能支撑它**：
//   kX11       只有 X11 连上了（没有并存的 Wayland 连接）
//   kWayland   只有 Wayland 连上了
//   kXWayland  Wayland 与 X11 都连上了，且 X11 那一侧是 XWayland（扩展或会话类型为证）
//   kHeadless  探测跑过了，但没有任何显示服务器连得上（DISPLAY 设了也一样）
//   kUnknown   探测本身没编译进来 / 没法给出结论；不拿它当 headless
enum class SessionKind : uint32_t {
    kX11 = 0,
    kWayland = 1,
    kXWayland = 2,
    kHeadless = 3,
    kUnknown = 4,
};

inline const char* SessionKindName(SessionKind kind) {
    switch (kind) {
        case SessionKind::kX11: return "x11";
        case SessionKind::kWayland: return "wayland";
        case SessionKind::kXWayland: return "xwayland";
        case SessionKind::kHeadless: return "headless";
        case SessionKind::kUnknown: return "unknown";
    }
    return "unknown";
}

// 一条运行能力的总体状态。与 EnvReport 的 CapStatus 同族，但多两档 Linux 必须分开说的：
//   kNotCompiled      这个构建没带那条后端（下一步：换一个构建）
//   kNotImplemented   代码有探测、但没有可用的采集实现（下一步：等后端；不是"换目标"）
//   kNeedsInteraction 已经能用，但必须先由人完成来源选择 / 系统授权（下一步：问人）
//   kUnverified       该问的判据没给出答案（不作任何断言）
enum class RuntimeStatus : uint32_t {
    kAvailable = 0,
    kUnavailable = 1,
    kUnverified = 2,
    kNotCompiled = 3,
    kNotImplemented = 4,
    kNeedsInteraction = 5,
};

inline const char* RuntimeStatusName(RuntimeStatus status) {
    switch (status) {
        case RuntimeStatus::kAvailable: return "available";
        case RuntimeStatus::kUnavailable: return "unavailable";
        case RuntimeStatus::kUnverified: return "unverified";
        case RuntimeStatus::kNotCompiled: return "not_compiled";
        case RuntimeStatus::kNotImplemented: return "not_implemented";
        case RuntimeStatus::kNeedsInteraction: return "needs_interaction";
    }
    return "unverified";
}

// 稳定的原因词（只增不改名）。**不随 --lang 变**，调用方按它分支。
namespace session_reason {
inline constexpr const char* kOk = "ok";
inline constexpr const char* kLibNotCompiled = "library_not_compiled";
inline constexpr const char* kDisplayNotConfigured = "display_not_configured";
inline constexpr const char* kConnectFailed = "connect_failed";
inline constexpr const char* kProtocolError = "protocol_error";
inline constexpr const char* kServiceNotFound = "service_not_found";
inline constexpr const char* kInterfaceMissing = "interface_missing";
inline constexpr const char* kVersionTooLow = "version_too_low";
inline constexpr const char* kVersionUnverified = "version_unverified";
inline constexpr const char* kSourceTypesUnavailable = "source_types_unavailable";
inline constexpr const char* kSourceTypeNotOffered = "source_type_not_offered";
inline constexpr const char* kPrerequisiteMissing = "prerequisite_missing";
inline constexpr const char* kBackendNotImplemented = "backend_not_implemented";
inline constexpr const char* kInteractionRequired = "interaction_required";
inline constexpr const char* kProbeNotCompiled = "probe_not_compiled";
}  // namespace session_reason

// caveat 的固定 token：这份报告**没有**断言什么。与 EnvReport 的 caveat 同一套用法。
// 这一份构建里 Linux 采集后端实现到哪一步。只在这里写一次：查询层与将来的后端共用
// 同一个事实；接上一条后端时改这里，不必重写 CLI 或 JSON 契约。
namespace session_backend {
inline constexpr bool kX11CaptureImplemented = false;
inline constexpr bool kWaylandCaptureImplemented = false;
inline constexpr bool kPortalCaptureImplemented = false;
inline constexpr bool kPipeWireCaptureImplemented = false;
}  // namespace session_backend

namespace session_caveat {
inline constexpr const char* kAvailableIsNotAGuarantee = "available_is_not_a_guarantee";
inline constexpr const char* kNoCapture = "no_capture_performed";
inline constexpr const char* kNoDialog = "no_consent_dialog_shown";
inline constexpr const char* kNoSourceSelection = "no_portal_source_selection_performed";
inline constexpr const char* kNoPortalSession = "no_portal_capture_session_created";
inline constexpr const char* kNoPixels = "no_screen_pixels_read";
inline constexpr const char* kNoAuthorization = "authorization_not_granted_by_query";
inline constexpr const char* kEnvironmentNotEvidence = "environment_variables_are_not_evidence";
inline constexpr const char* kDesktopNameNotEvidence = "desktop_name_is_not_evidence";
inline constexpr const char* kXwaylandScope = "xwayland_covers_x11_windows_only";
inline constexpr const char* kXwaylandNotNative = "xwayland_is_not_native_wayland";
inline constexpr const char* kWaylandNoWindowList = "wayland_has_no_global_window_list";
inline constexpr const char* kSshNotHeadless = "ssh_session_does_not_imply_headless";
inline constexpr const char* kUnknownIsNotNo = "unknown_is_not_no";
inline constexpr const char* kPipewirePrereqNotCapture = "pipewire_prerequisite_is_not_capture";
}  // namespace session_caveat

// 一条诊断：code 是稳定的 ASCII（例如 "session.portal.service_not_found"），detail 是
// 不随 --lang 变的原因 token（例如 "service_not_found"），hint 是给调用方的下一步
// （也是 ASCII token；人读文案在 README 与 resources 里）。severity 只区分信息与可执行。
enum class SessionSeverity : uint32_t {
    kInfo = 0,
    kActionable = 1,
};

struct SessionDiagnostic {
    std::wstring code;
    std::wstring detail;
    std::wstring hint;
    SessionSeverity severity = SessionSeverity::kInfo;
};

// ---- 环境线索：只报"设没设"，不报原值（错误消息不得泄露不必要的环境信息） ----
struct SessionEnvironmentFacts {
    bool xdgSessionTypeSet = false;
    // 只保留归一化后的枚举词：wayland / x11 / tty / unknown。原文不进报告。
    std::wstring xdgSessionType = L"unknown";
    bool displaySet = false;
    bool waylandDisplaySet = false;
    bool sshConnectionSet = false;
    bool sshTtySet = false;
    bool runtimeDirSet = false;
    bool dbusAddressSet = false;
    bool desktopNameSet = false;   // 读到了，但**不是**证据（报告里另有 caveat）
};

struct X11Facts {
    bool compiled = false;            // 这个构建带了 X11 探测（libxcb）
    bool displayConfigured = false;   // DISPLAY 非空
    bool attempted = false;           // 这次真的去连了
    bool connected = false;
    int connectError = 0;             // xcb 的连接错误（只作 detail，不作分支）
    uint32_t protocolMajor = 0;
    uint32_t protocolMinor = 0;
    uint32_t screenCount = 0;
    bool xwaylandExtension = false;
    std::vector<std::wstring> extensions;   // 服务端声明的扩展名（受上限）
};

struct WaylandFacts {
    bool compiled = false;
    bool displayConfigured = false;
    bool attempted = false;
    bool connected = false;
    int connectError = 0;
    bool registryRoundTripOk = false;
    std::vector<std::wstring> interfaces;   // 全局接口名（受上限）
    uint32_t outputCount = 0;
    bool hasCompositor = false;
    bool hasShm = false;
    bool hasScreencopy = false;
    bool hasImageCopyCapture = false;
};

struct PortalSourceFacts {
    std::wstring name;              // monitor / window / virtual
    Tri available = Tri::kUnknown;  // 这一种来源类型能不能用（位掩码问不出来就是 unknown）
};

struct PortalFacts {
    bool compiled = false;                   // 这个构建带了 D-Bus 探测（libdbus-1）
    bool addressConfigured = false;          // 有可用的会话总线地址（环境变量或 XDG_RUNTIME_DIR/bus）
    bool attempted = false;
    bool busConnected = false;
    int connectError = 0;
    bool serviceOwnerKnown = false;
    bool serviceOwner = false;
    bool screenCastInterfaceKnown = false;
    bool screenCastInterface = false;
    bool screenCastVersionKnown = false;
    uint32_t screenCastVersion = 0;
    bool screenshotInterfaceKnown = false;
    bool screenshotInterface = false;
    bool screenshotVersionKnown = false;
    uint32_t screenshotVersion = 0;
    bool sourceTypesKnown = false;
    uint32_t sourceTypesMask = 0;            // 1=monitor 2=window 4=virtual（Portal 位定义）
    bool cursorModesKnown = false;
    uint32_t cursorModesMask = 0;
};

struct PipeWireFacts {
    bool compiled = false;              // 这个构建带了 PipeWire 客户端
    bool runtimeDirConfigured = false;
    bool socketPresentKnown = false;
    bool socketPresent = false;
};

// 平台层交回的全部事实。测试可以逐项构造，不需要真的桌面。
struct SessionFacts {
    SessionEnvironmentFacts environment;
    X11Facts x11;
    WaylandFacts wayland;
    PortalFacts portal;
    PipeWireFacts pipewire;

    // 这三条是这套实现自己的保证，恒为 false：只读查询不选来源、不建会话、不取像素。
    // 留成字段是为了让判据能逐条钉住，而不是靠注释。
    bool sourceSelectionRequested = false;
    bool captureSessionCreated = false;
    bool screenPixelsRead = false;
};

// 一条运行能力的报告（每行五轴分开写，再给一个总体 status 与 canCapture）。
struct RuntimeCapabilityReport {
    std::wstring name;                 // x11 / wayland / xdg-portal / pipewire
    bool compiled = false;
    Tri reachable = Tri::kUnknown;
    Tri versionSatisfied = Tri::kUnknown;
    Tri interactionRequired = Tri::kUnknown;
    Tri implemented = Tri::kNo;
    RuntimeStatus status = RuntimeStatus::kUnverified;
    std::wstring reason = L"probe_not_compiled";
    std::wstring observedVersion;      // 认得出来才写
    std::wstring minimumVersion;       // 这一条要求的版本，0 = 没有门槛时留空
    // 只有 status == available 才是 true：连接成功 + 版本满足 + 已实现 + 不需要交互。
    // not_implemented / not_compiled / needs_interaction 一律 false —— 绝不回答"可以截图"。
    bool canCapture = false;
};

struct PortalSourceReport {
    std::wstring name;
    Tri available = Tri::kUnknown;
    bool interactionRequired = true;   // 来源选择永远需要人
    std::wstring reason = L"source_types_unavailable";
};

struct SessionReport {
    SessionKind kind = SessionKind::kUnknown;
    PlatformKind platform = PlatformKind::kUnknown;

    SessionEnvironmentFacts environment;
    X11Facts x11;
    WaylandFacts wayland;
    PortalFacts portal;
    PipeWireFacts pipewire;

    Tri remoteSession = Tri::kUnknown;
    bool ssh = false;
    bool authorizationGranted = false;          // 只读查询恒 false
    bool xwaylandCoversOnlyX11Windows = true;
    bool nativeWaylandWindowQuerySupported = false;   // 合成器不提供全局窗口列表
    bool xwaylandAuthorizationSubstitute = false;

    std::vector<RuntimeCapabilityReport> runtime;
    std::vector<PortalSourceReport> portalSources;
    std::vector<SessionDiagnostic> diagnostics;
    std::vector<std::wstring> caveats;

    // 按名字取一条运行能力；没有就返回 nullptr（不是拿一个默认值冒充）。
    const RuntimeCapabilityReport* RuntimeOf(const wchar_t* name) const;
};

// 纯判据：从事实算出整份报告。不连任何服务、不读环境、不弹框、不取像素。
SessionReport BuildSessionReport(const SessionFacts& facts);

// 机器可读 JSON 的一段（session 对象本体 + runtime 数组 + diagnostics）。调用方负责外层
// 文档与键名。所有取值都是 ASCII 机器词，同一台机器上换 --lang 逐字节相同。
std::wstring RenderSessionJson(const SessionReport& report, bool verbose);

// 只渲染 session 对象（不含外层键），给 EnvReport 的 capabilities/diagnostics 文档复用。
void WriteSessionObject(class Json& json, const SessionReport& report, bool verbose);

}  // namespace ecapture
