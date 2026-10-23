#include "PortalAuthRun.h"

#include <memory>
#include <string>
#include <vector>

#include "CaptureScope.h"
#include "Consent.h"
#include "Deadline.h"
#include "HostPath.h"
#include "HostProcess.h"
#include "Json.h"
#include "Lang.h"
#include "SystemCompat.h"
#include "Version.h"
#include "core/ConsentState.h"
#include "PortalSystemAuth.h"

#if defined(__linux__) && !defined(_WIN32)
#include "platform/linux/DBusPortalScreenCast.h"
#endif

namespace ecapture {
namespace {

std::wstring W(const char* text) {
    std::wstring out;
    if (!text) return out;
    while (*text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    return out;
}

void WriteDiagnostics(Json& j, const wchar_t* key, const std::vector<Diagnostic>& items) {
    if (items.empty()) return;
    j.Key(key).Arr();
    for (const auto& d : items) {
        j.Obj();
        j.Key(L"code").Value(d.code);
        OptString(j, L"message", d.message);
        OptString(j, L"option", d.option);
        OptString(j, L"value", d.value);
        OptString(j, L"hint", d.hint);
        OptString(j, L"target", d.target);
        OptString(j, L"detail", d.detail);
        OptString(j, L"stage", d.stage);
        if (d.win32 != 0) j.Key(L"win32").Value(static_cast<long long>(d.win32));
        j.End();
    }
    j.End();
}

void WritePortalObject(Json& j, PortalAuthorizeMode mode,
                       const portal::AuthorizationResult& result, ConsentState programState,
                       SystemAuthState systemState, bool granted) {
    j.Key(L"portal").Obj();
    j.Key(L"action").Value(L"authorize_and_select");
    j.Key(L"contractVersion").Value(1);
    j.Key(L"requestedSource").Value(PortalAuthorizeModeName(mode));
    j.Key(L"scope").Value(mode == PortalAuthorizeMode::kMonitor ? L"desktop" : L"window");
    j.Key(L"programConfirmation").Value(W(ConsentStateName(programState)));
    j.Key(L"systemAuthorization").Value(W(SystemAuthStateName(systemState)));
    j.Key(L"systemAuthorizationGranted").Value(granted);
    j.Key(L"selectionCompleted").Value(granted);
    j.Key(L"parentWindow").Obj()
        .Key(L"provided").Value(false)
        .Key(L"reason").Value(L"no_exportable_cli_parent_window")
        .Key(L"apiAllowsEmpty").Value(true)
        .End();
    j.Key(L"requestedSourceMask").Value(static_cast<long long>(result.requestedSourceMask));
    j.Key(L"multipleSourcesRequested").Value(false);
    j.Key(L"restoreTokenRequested").Value(false);
    j.Key(L"restoreTokenStored").Value(result.restoreTokenPersisted);
    j.Key(L"restoreTokenReported").Value(false);
    if (result.versionKnown) {
        j.Key(L"portalVersion").Value(static_cast<long long>(result.portalVersion));
    }
    j.Key(L"availableSourceTypes").Obj();
    if (result.sourceTypesKnown) {
        j.Key(L"known").Value(true);
        j.Key(L"mask").Value(static_cast<long long>(result.availableSourceTypes));
        j.Key(L"monitor").Value((result.availableSourceTypes & 1u) != 0);
        j.Key(L"window").Value((result.availableSourceTypes & 2u) != 0);
        j.Key(L"virtual").Value((result.availableSourceTypes & 4u) != 0);
    } else {
        j.Key(L"known").Value(false);
        j.Key(L"mask").Value(L"unknown");
    }
    j.End();
    j.Key(L"selection").Obj();
    j.Key(L"sourceTypeVerified").Value(result.selection.sourceTypeVerified);
    j.Key(L"exactlyOneStream").Value(result.selection.exactlyOneStream);
    j.Key(L"streamCount").Value(static_cast<long long>(result.selection.streams.size()));
    j.Key(L"streams").Arr();
    for (const auto& stream : result.selection.streams) {
        j.Obj();
        j.Key(L"nodeId").Value(static_cast<long long>(stream.nodeId));
        j.Key(L"nodeIdScope").Value(L"portal_session");
        if (stream.sourceTypeKnown) {
            j.Key(L"sourceType").Value(W(portal::SourceKindName(stream.sourceType)));
        } else {
            j.Key(L"sourceType").Value(L"unknown");
        }
        if (stream.positionKnown) {
            j.Key(L"position").Obj().Key(L"x").Value(stream.x).Key(L"y").Value(stream.y).End();
        }
        if (stream.sizeKnown) {
            j.Key(L"size").Obj().Key(L"width").Value(stream.width).Key(L"height").Value(stream.height)
                .End();
        }
        if (stream.pipewireSerialKnown) {
            j.Key(L"pipewireSerial").Value(static_cast<long long>(stream.pipewireSerial));
        }
        j.End();
    }
    j.End();   // streams array
    j.End();   // selection object
    j.Key(L"request").Obj()
        .Key(L"responseCompleted").Value(result.requestCompleted)
        .Key(L"closedByApplication").Value(result.requestClosed)
        .End();
    j.Key(L"session").Obj()
        .Key(L"created").Value(result.sessionCreated)
        .Key(L"closed").Value(result.sessionClosed)
        .Key(L"closedByPortal").Value(result.sessionClosedByPortal)
        .End();
    j.Key(L"lateSuccessIgnored").Value(result.lateResponseIgnored);
    // 这三条是本步骤的边界：授权与来源选择可用，不等于截到图。
    j.Key(L"pixelsRead").Value(false);
    j.Key(L"pipeWireRemoteOpened").Value(false);
    j.Key(L"imageDelivered").Value(false);
    j.Key(L"authorizationAndSelectionAvailable").Value(granted);
    j.Key(L"nextStep").Value(granted ? L"pipewire_frame_delivery_not_implemented"
                                     : L"resolve_portal_failure");
    j.End();
}

std::vector<Diagnostic> BuildNotes(const Options& opt) {
    std::vector<Diagnostic> notes;
    Diagnostic only;
    only.code = codes::kPortalImageDeliveryNotImplemented;
    only.message = Msg(L"note.portal_authorization_only");
    only.hint = Msg(L"note.portal_authorization_only_hint");
    only.stage = stages::kPortal;
    if (opt.yes) {
        Diagnostic yes;
        yes.code = codes::kPortalYesNotApplicable;
        yes.message = Msg(L"note.portal_yes_not_applicable");
        yes.option = L"--yes";
        yes.value = L"ignored_for_interactive_portal_source";
        yes.stage = stages::kConsent;
        // 不把两个 note 混成一个：一个说 --yes 为什么不生效，一个说本步骤没有图像。
        notes.push_back(std::move(yes));
    }
    notes.push_back(std::move(only));
    return notes;
}

int ExitCodeFor(const Diagnostic& err) {
    if (err.code == codes::kPortalUnavailable || err.code == codes::kPortalUnsupported ||
        err.code == codes::kPortalServiceLost || err.code == codes::kPortalProtocolError ||
        err.code == codes::kPortalSourceMismatch || err.code == codes::kPortalMultipleStreams ||
        err.code == codes::kPortalImageDeliveryNotImplemented) {
        return EX_CAPTURE_FAILED;
    }
    // 人拒绝 / 系统合并拒绝或取消 / 程序取消 / 超时 / 确认框失败：都没有取得授权。
    return EX_DENIED;
}

std::wstring RenderFailure(const Options& opt, PortalAuthorizeMode mode,
                           const std::vector<Diagnostic>& errors, ConsentState programState,
                           SystemAuthState systemState,
                           const portal::AuthorizationResult& result) {
    Json j;
    j.Obj();
    j.Key(L"captured").Value(0);
    j.Key(L"images").Arr().End();
    WritePortalObject(j, mode, result, programState, systemState, false);
    WriteDiagnostics(j, L"errors", errors);
    const std::vector<Diagnostic> notes = BuildNotes(opt);
    if (!opt.quiet) WriteDiagnostics(j, L"notes", notes);
    j.End();
    return j.Str();
}

std::wstring RenderSuccess(const Options& opt, PortalAuthorizeMode mode,
                           ConsentState programState, SystemAuthState systemState,
                           const portal::AuthorizationResult& result) {
    Json j;
    j.Obj();
    j.Key(L"captured").Value(0);
    j.Key(L"images").Arr().End();
    WritePortalObject(j, mode, result, programState, systemState, true);
    const std::vector<Diagnostic> notes = BuildNotes(opt);
    if (!opt.quiet) WriteDiagnostics(j, L"notes", notes);
    j.End();
    return j.Str();
}

std::vector<Diagnostic> BuildProgramFailure(const Diagnostic& err) {
    if (!err.code.empty()) return {err};
    Diagnostic fallback;
    fallback.code = codes::kAccessDenied;
    fallback.message = Msg(L"consent.denied");
    fallback.stage = stages::kConsent;
    return {fallback};
}

}  // namespace

PortalAuthRunResult RunPortalAuthorization(const Options& opt) {
    PortalAuthRunResult out;
    const PortalAuthorizeMode mode = opt.portalAuthorize;
    const PixelScope scope =
        mode == PortalAuthorizeMode::kMonitor ? PixelScope::kDesktop : PixelScope::kWindowContent;
    const wchar_t* path =
        mode == PortalAuthorizeMode::kMonitor ? paths::kPortalMonitor : paths::kPortalWindow;
    const wchar_t* targetKey = mode == PortalAuthorizeMode::kMonitor ? L"portal.monitor"
                                                                   : L"portal.window";

    if (mode == PortalAuthorizeMode::kNone) {
        Diagnostic d;
        d.code = codes::kPortalAuthorizeConflict;
        d.message = Msg(L"cli.portal_authorize_conflict");
        d.stage = stages::kParse;
        out.body = RenderFailure(opt, mode, {d}, ConsentState::kNotRequested,
                                 SystemAuthState::kNotRequested, portal::AuthorizationResult{});
        out.exitCode = EX_USAGE;
        return out;
    }

#if defined(__linux__) && !defined(_WIN32)
    std::unique_ptr<portal::IPortalScreenCast> driver = portal::CreateDBusPortalScreenCast();
#else
    std::unique_ptr<portal::IPortalScreenCast> driver;
#endif
    if (!driver) {
        Diagnostic d;
        d.code = codes::kPortalUnavailable;
        d.message = Msg(L"consent.system_unavailable");
        d.hint = Msg(L"consent.system_unavailable_hint");
        d.stage = stages::kPortal;
        out.body = RenderFailure(opt, mode, {d}, ConsentState::kNotRequested,
                                 SystemAuthState::kUnavailable, portal::AuthorizationResult{});
        out.exitCode = EX_CAPTURE_FAILED;
        return out;
    }

    const portal::SourceKind source =
        mode == PortalAuthorizeMode::kMonitor ? portal::SourceKind::kMonitor
                                              : portal::SourceKind::kWindow;
    PortalSystemAuthProvider portalAuth(
        *driver, source, opt.timeoutMs == 0 ? kIsolatedCallMs : opt.timeoutMs,
        opt.portalTimeoutMs);

    // 程序确认与系统授权是两层。这里显式要求两层，不用编译期平台默认值：即使当前跑在
    // XWayland / X11 会话，只要用户点名走 Portal，系统授权层就必须真实取得。
    const uint32_t required = kGrantProgramConfirm | kGrantSystemAuthorization;
    GateConfig gateCfg;
    gateCfg.yes = opt.yes;
    gateCfg.requiredGrantLayers = required;
    gateCfg.captureLabel = mode == PortalAuthorizeMode::kMonitor ? L"portal.monitor"
                                                               : L"portal.window";
    gateCfg.consentTimeoutMs = opt.consentTimeoutMs;
    gateCfg.systemAuth = &portalAuth;
    gateCfg.systemInteractiveSource = true;
    gateCfg.systemSourceLabel =
        Msg(mode == PortalAuthorizeMode::kMonitor ? L"portal.source_monitor"
                                                   : L"portal.source_window");
    gateCfg.requestSource = opt.purpose;
    gateCfg.requestSourceGiven = !opt.purpose.empty();
    {
        std::wstring selfPath;
        if (hostproc::SelfImagePath(&selfPath)) gateCfg.programPath = std::move(selfPath);
    }
    gateCfg.programVersion = kVersion;
    gateCfg.programPid = CurrentProcessId();

    GateTarget target;
    target.screen = mode == PortalAuthorizeMode::kMonitor;
    target.interactiveSystemSource = true;
    target.key = targetKey;
    target.subject = Msg(mode == PortalAuthorizeMode::kMonitor ? L"portal.target_monitor"
                                                               : L"portal.target_window");
    target.area = PhysRect{0, 0, 0, 0};
    gateCfg.targets.push_back(std::move(target));

    DialogConsentPrompt prompt(opt.consentTimeoutMs);
    // 本动作没有预先枚举的屏幕拓扑，也不靠拓扑给任何许可；空拓扑只让状态机有一个稳定的
    // 内部指纹。真正的系统授权与来源选择由 Portal 窗口完成。
    ConsentGate gate(std::move(gateCfg), prompt,
                     [] { return std::vector<ScreenInfo>{}; });

    Diagnostic err;
    const bool granted = gate.AuthorizePortalSource(scope, path, targetKey, &err);
    const portal::AuthorizationResult result = portalAuth.LastResult();
    const ConsentState programState =
        scope == PixelScope::kDesktop ? gate.DesktopState() : gate.WindowState();
    const SystemAuthState systemState = gate.SystemState();

    if (!granted) {
        const std::vector<Diagnostic> errors = BuildProgramFailure(err);
        out.exitCode = ExitCodeFor(errors.front());
    } else {
        out.exitCode = EX_OK;
    }
    out.body = granted ? RenderSuccess(opt, mode, programState, systemState, result)
                       : RenderFailure(opt, mode, BuildProgramFailure(err), programState,
                                       systemState, result);
    return out;
}

}  // namespace ecapture
