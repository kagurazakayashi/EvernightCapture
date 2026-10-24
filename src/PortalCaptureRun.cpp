#include "PortalCaptureRun.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "CaptureDiag.h"
#include "CaptureScope.h"
#include "Consent.h"
#include "CursorControl.h"
#include "Deadline.h"
#include "Delivery.h"
#include "Encoder.h"
#include "FileSave.h"
#include "HdrColor.h"
#include "HistoryArchive.h"
#include "HostPath.h"
#include "HostProcess.h"
#include "ImageOps.h"
#include "Json.h"
#include "Lang.h"
#include "OutputPlan.h"
#include "OutputStream.h"
#include "PipeWireCapture.h"
#include "PortalSystemAuth.h"
#include "Version.h"
#include "core/ConsentState.h"
#include "platform/linux/DBusPortalScreenCast.h"
#include "platform/linux/PipeWireCaptureLinux.h"

namespace ecapture {
namespace {

constexpr uint32_t kPortalFrameTimeoutMs = 2000;
constexpr uint64_t kPortalRemoteOpenTimeoutMs = 5000;

std::wstring W(const char* text) {
    std::wstring out;
    if (!text) return out;
    while (*text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    return out;
}

Diagnostic PortalDiag(const wchar_t* code, const std::wstring& message,
                      const std::wstring& hint, const std::wstring& target,
                      const std::wstring& detail = std::wstring(),
                      const wchar_t* option = L"--portal-capture") {
    Diagnostic d;
    d.code = code;
    d.message = message;
    d.hint = hint;
    d.target = target;
    d.detail = detail;
    d.option = option;
    d.value = L"portal";
    d.backend = L"portal";
    d.stage = stages::kPortal;
    return d;
}

Diagnostic AuthFailure(const portal::AuthorizationResult& result, const std::wstring& target) {
    const wchar_t* code = codes::kPortalProtocolError;
    switch (result.outcome) {
        case portal::Outcome::kCancelledOrDenied:
            code = codes::kPortalCancelledOrDenied;
            break;
        case portal::Outcome::kTimedOut:
            code = codes::kPortalTimeout;
            break;
        case portal::Outcome::kCancelled:
            code = codes::kPortalCancelled;
            break;
        case portal::Outcome::kUnavailable:
            code = codes::kPortalUnavailable;
            break;
        case portal::Outcome::kUnsupported:
            code = codes::kPortalUnsupported;
            break;
        case portal::Outcome::kServiceLost:
            code = codes::kPortalServiceLost;
            break;
        case portal::Outcome::kProtocolError:
        case portal::Outcome::kRequestFailed:
        case portal::Outcome::kGranted:
            code = codes::kPortalProtocolError;
            break;
    }
    Diagnostic d = PortalDiag(code, Msg(L"cap.portal_capture_failed"),
                              Msg(L"cap.portal_capture_failed_hint"), target,
                              W(result.reason.c_str()));
    d.stage = stages::kPortal;
    return d;
}

int ExitCodeForPortalDiagnostic(const Diagnostic& d) {
    if (d.code == codes::kPortalCancelledOrDenied || d.code == codes::kPortalTimeout ||
        d.code == codes::kPortalCancelled) {
        return EX_DENIED;
    }
    return EX_CAPTURE_FAILED;
}

Diagnostic PipeWireFailure(const pipewire::CaptureResult& capture, const std::wstring& target) {
    const wchar_t* code = codes::kPortalPipeWireNoFrame;
    const wchar_t* message = L"cap.portal_no_frame";
    const wchar_t* hint = L"cap.portal_no_frame_hint";
    switch (capture.outcome) {
        case pipewire::Outcome::kUnavailable:
            code = codes::kPortalPipeWireUnavailable;
            message = L"cap.portal_pipewire_unavailable";
            hint = L"cap.portal_pipewire_unavailable_hint";
            break;
        case pipewire::Outcome::kUnsupported:
            if (capture.reason == pipewire::reason::kMemoryUnsupported) {
                code = codes::kPortalMemoryUnsupported;
                message = L"cap.portal_memory_unsupported";
                hint = L"cap.portal_memory_unsupported_hint";
            } else if (capture.reason == pipewire::reason::kHdrUnverified) {
                code = codes::kPortalHdrUnsupported;
                message = L"cap.portal_hdr_unsupported";
                hint = L"cap.portal_hdr_unsupported_hint";
            } else {
                code = codes::kPortalFormatUnsupported;
                message = L"cap.portal_format_unsupported";
                hint = L"cap.portal_format_unsupported_hint";
            }
            break;
        case pipewire::Outcome::kTimeout:
            code = codes::kCaptureTimeout;
            message = L"cap.portal_no_frame";
            hint = L"cap.portal_no_frame_hint";
            break;
        case pipewire::Outcome::kCancelled:
            code = codes::kPortalCancelled;
            message = L"cap.portal_capture_failed";
            hint = L"cap.portal_capture_failed_hint";
            break;
        case pipewire::Outcome::kDisconnected:
            code = codes::kPortalStreamLost;
            message = L"cap.portal_stream_lost";
            hint = L"cap.portal_stream_lost_hint";
            break;
        case pipewire::Outcome::kFrameInvalid:
            code = codes::kPortalFrameInvalid;
            message = L"cap.portal_frame_invalid";
            hint = L"cap.portal_frame_invalid_hint";
            break;
        case pipewire::Outcome::kSourceMismatch:
        case pipewire::Outcome::kProtocolError:
            code = codes::kPortalSourceMismatch;
            message = L"cap.portal_capture_failed";
            hint = L"cap.portal_capture_failed_hint";
            break;
        case pipewire::Outcome::kNoFrame:
            code = codes::kPortalPipeWireNoFrame;
            message = L"cap.portal_no_frame";
            hint = L"cap.portal_no_frame_hint";
            break;
        case pipewire::Outcome::kHdrUnverified:
            code = codes::kPortalHdrUnsupported;
            message = L"cap.portal_hdr_unsupported";
            hint = L"cap.portal_hdr_unsupported_hint";
            break;
        case pipewire::Outcome::kOk:
            break;
    }
    std::wstring detail = W(capture.reason.c_str());
    if (capture.emptyChunks || capture.corruptedChunks || capture.staleFrames ||
        capture.invalidFrames) {
        detail += L"; dropped empty=" + std::to_wstring(capture.emptyChunks) +
                  L" corrupted=" + std::to_wstring(capture.corruptedChunks) +
                  L" stale=" + std::to_wstring(capture.staleFrames) +
                  L" invalid=" + std::to_wstring(capture.invalidFrames);
    }
    Diagnostic d = PortalDiag(code, Msg(message), Msg(hint), target, detail);
    d.stage = stages::kCapture;
    return d;
}

portal::CursorMode PortalCursorModeFor(const Options& opt) {
    if (!opt.cursor.given) return portal::CursorMode::kDefault;
    if (opt.cursor.mode == CursorMode::kInclude) return portal::CursorMode::kEmbedded;
    if (opt.cursor.mode == CursorMode::kExclude) return portal::CursorMode::kHidden;
    return portal::CursorMode::kDefault;   // Portal 官方默认即 Hidden
}

bool ApplyRoi(const Options& opt, CapturedFrame* frame, CapturedImage* image,
              CaptureOutcome* outcome) {
    if (opt.crop.mode != CropMode::kRoi) return true;
    const uint32_t fullW = frame->width;
    const uint32_t fullH = frame->height;
    if (opt.crop.x > fullW || opt.crop.y > fullH ||
        opt.crop.width > fullW - opt.crop.x || opt.crop.height > fullH - opt.crop.y) {
        Diagnostic d;
        d.code = codes::kRoiInvalid;
        d.message = Msgf(L"cap.roi_invalid", fullW, fullH,
                         static_cast<uint64_t>(opt.crop.x) + opt.crop.width,
                         static_cast<uint64_t>(opt.crop.y) + opt.crop.height,
                         L"portal_frame_does_not_contain_requested_roi");
        d.option = L"--roi";
        d.value = opt.crop.written;
        d.hint = Msg(L"cap.roi_invalid_hint");
        d.stage = stages::kCapture;
        d.target = L"portal";
        outcome->errors.push_back(std::move(d));
        outcome->exitCode = EX_USAGE;
        return false;
    }
    if (!CropFrame(frame, opt.crop.x, opt.crop.y, opt.crop.width, opt.crop.height)) {
        Diagnostic d;
        d.code = codes::kFrameInvalid;
        d.message = Msg(L"cap.crop_apply_failed");
        d.option = L"--roi";
        d.value = opt.crop.written;
        d.hint = Msg(L"cap.crop_apply_failed_hint");
        d.stage = stages::kCapture;
        d.target = L"portal";
        outcome->errors.push_back(std::move(d));
        outcome->exitCode = EX_CAPTURE_FAILED;
        return false;
    }
    image->cropped = true;
    image->cropMode = L"roi";
    image->crop.x = opt.crop.x;
    image->crop.y = opt.crop.y;
    image->crop.width = opt.crop.width;
    image->crop.height = opt.crop.height;
    image->fullWidth = fullW;
    image->fullHeight = fullH;
    // Portal stream 的窗口坐标原点与客户区几何不由本工具核实；不猜 cropScreenRect。
    Diagnostic note;
    note.code = codes::kCropMappingUnavailable;
    note.message = Msgf(L"note.crop_mapping_unavailable", fullW, fullH);
    note.option = L"--roi";
    note.hint = Msg(L"note.crop_mapping_unavailable_hint");
    note.stage = stages::kCapture;
    note.target = L"portal";
    outcome->notes.push_back(std::move(note));
    return true;
}

bool ApplyScale(const Options& opt, CapturedFrame* frame, CapturedImage* image,
                CaptureOutcome* outcome) {
    if (!opt.scale.given) return true;
    const ScaleResolution scale = ResolveScale(opt.scale, frame->width, frame->height);
    if (scale.status == ScaleStatus::kRejected) {
        Diagnostic d;
        d.code = codes::kFrameInvalid;
        d.message = Msg(L"cap.scale_apply_failed");
        d.option = L"--scale";
        d.hint = Msg(L"cap.scale_apply_failed_hint");
        d.stage = stages::kCapture;
        d.target = L"portal";
        outcome->errors.push_back(std::move(d));
        outcome->exitCode = EX_CAPTURE_FAILED;
        return false;
    }
    image->scaled = true;
    image->scaleFromWidth = frame->width;
    image->scaleFromHeight = frame->height;
    image->scaleApplied = scale.status == ScaleStatus::kScaled;
    if (image->scaleApplied && !ScaleFrame(frame, scale.width, scale.height)) {
        Diagnostic d;
        d.code = codes::kFrameInvalid;
        d.message = Msg(L"cap.scale_apply_failed");
        d.option = L"--scale";
        d.hint = Msg(L"cap.scale_apply_failed_hint");
        d.stage = stages::kCapture;
        d.target = L"portal";
        outcome->errors.push_back(std::move(d));
        outcome->exitCode = EX_CAPTURE_FAILED;
        return false;
    }
    return true;
}

class PortalOutputSink final : public OutputSink {
public:
    bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                  Diagnostic* err) override {
        return SaveFileAtomic(path, bytes, overwrite, err);
    }
    bool EmitBytes(const std::vector<uint8_t>& bytes, uint64_t* emitted,
                   NativeErrorCode* ioError) override {
        return EmitStdoutBytes(bytes, ioError, emitted);
    }
};

struct SessionCloser {
    portal::IPortalScreenCast* driver = nullptr;
    ~SessionCloser() {
        if (driver) driver->CloseSession();
    }
};

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() {
        if (fd >= 0) close(fd);
    }
    int Release() {
        const int value = fd;
        fd = -1;
        return value;
    }
};

}  // namespace

CaptureOutcome RunPortalCapture(const Options& opt) {
    CaptureOutcome outcome;
    if (opt.portalCapture == PortalAuthorizeMode::kNone) {
        Diagnostic d;
        d.code = codes::kPortalCaptureConflict;
        d.message = Msg(L"cli.portal_capture_conflict");
        d.stage = stages::kParse;
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_USAGE;
        return outcome;
    }

    if (opt.yes) {
        Diagnostic note;
        note.code = codes::kPortalYesNotApplicable;
        note.message = Msg(L"note.portal_yes_not_applicable");
        note.option = L"--yes";
        note.value = L"ignored_for_interactive_portal_source";
        note.stage = stages::kConsent;
        outcome.notes.push_back(std::move(note));
    }

    const bool window = opt.portalCapture == PortalAuthorizeMode::kWindow;
    const portal::SourceKind source =
        window ? portal::SourceKind::kWindow : portal::SourceKind::kMonitor;
    const PixelScope scope = window ? PixelScope::kWindowContent : PixelScope::kDesktop;
    const wchar_t* path = window ? paths::kPortalWindow : paths::kPortalMonitor;
    const std::wstring targetKey = window ? L"portal.window" : L"portal.monitor";
    const std::wstring sourceLabel =
        Msg(window ? L"portal.source_window" : L"portal.source_monitor");

    if (opt.crop.mode == CropMode::kClientArea) {
        Diagnostic d = PortalDiag(codes::kPortalClientAreaUnsupported,
                                  Msg(L"cap.portal_client_area_unsupported"),
                                  Msg(L"cap.portal_client_area_unsupported_hint"), targetKey);
        d.stage = stages::kParse;
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_USAGE;
        return outcome;
    }

    const char* formatReason = nullptr;
    if (!EncodeFormatSupported(opt.format, &formatReason)) {
        Diagnostic d;
        d.code = codes::kEncoderUnavailable;
        d.message = Msg(L"enc.format_unsupported");
        d.option = L"--format";
        d.value = FormatName(opt.format);
        d.detail = W(formatReason);
        d.stage = stages::kEncode;
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }

    // 输出名与历史位置在弹框与系统授权之前判：注定写不下来的目标不该先打扰人。
    std::wstring outputPath;
    if (opt.output == L"-") {
        outputPath = L"-";
    } else {
        std::vector<OutputTarget> targets;
        OutputTarget outputTarget;
        outputTarget.name = targetKey;
        targets.push_back(std::move(outputTarget));
        std::vector<std::wstring> planned;
        std::vector<Diagnostic> planNotes;
        Diagnostic planErr;
        if (!PlanOutputPaths(opt, targets, &planned, &planNotes, &planErr)) {
            planErr.stage = stages::kPlan;
            outcome.errors.push_back(std::move(planErr));
            outcome.exitCode = EX_IO_FAILED;
            return outcome;
        }
        outputPath = planned.front();
        for (Diagnostic& note : planNotes) outcome.notes.push_back(std::move(note));
    }

    std::optional<HistoryArchive> historyArchive;
    if (opt.historyEnabled) historyArchive.emplace(opt.portable);

    const Deadline dl = Deadline::FromTotalMs(opt.timeoutMs);
    std::unique_ptr<portal::IPortalScreenCast> driver =
        portal::CreateDBusPortalScreenCast();
    if (!driver) {
        Diagnostic d = PortalDiag(codes::kPortalUnavailable, Msg(L"consent.system_unavailable"),
                                  Msg(L"consent.system_unavailable_hint"), targetKey);
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }
    SessionCloser sessionCloser{driver.get()};
    portal::CancelToken cancelToken;
    PortalSystemAuthProvider portalAuth(
        *driver, source, dl.ClampWait(kIsolatedCallMs), opt.portalTimeoutMs, &cancelToken,
        /*closeSessionOnSuccess=*/false, PortalCursorModeFor(opt));

    GateConfig gateCfg;
    gateCfg.yes = opt.yes;
    gateCfg.requiredGrantLayers = kGrantProgramConfirm | kGrantSystemAuthorization;
    gateCfg.captureLabel = path;
    gateCfg.consentTimeoutMs = opt.consentTimeoutMs;
    gateCfg.autoBudget = &dl;
    gateCfg.systemAuth = &portalAuth;
    gateCfg.systemInteractiveSource = true;
    gateCfg.systemSourceLabel = sourceLabel;
    gateCfg.requestSource = opt.purpose;
    gateCfg.requestSourceGiven = !opt.purpose.empty();
    gateCfg.historyCopy = historyArchive && historyArchive->root().usable;
    {
        std::wstring selfPath;
        if (hostproc::SelfImagePath(&selfPath)) gateCfg.programPath = std::move(selfPath);
    }
    gateCfg.programVersion = kVersion;
    gateCfg.programPid = CurrentProcessId();

    GateTarget target;
    target.screen = !window;
    target.interactiveSystemSource = true;
    target.key = targetKey;
    target.subject = sourceLabel;
    target.area = PhysRect{0, 0, 0, 0};
    gateCfg.targets.push_back(std::move(target));
    gateCfg.outputs.push_back(outputPath == L"-" ? Msg(L"consent.output_stdout") : outputPath);

    DialogConsentPrompt prompt(opt.consentTimeoutMs);
    ConsentGate gate(std::move(gateCfg), prompt, [] { return std::vector<ScreenInfo>{}; });

    Diagnostic authErr;
    if (!gate.AuthorizePortalSource(scope, path, targetKey, &authErr)) {
        if (authErr.code.empty()) authErr = AuthFailure(portalAuth.LastResult(), targetKey);
        outcome.exitCode = ExitCodeForPortalDiagnostic(authErr);
        outcome.errors.push_back(std::move(authErr));
        return outcome;
    }

    const portal::AuthorizationResult auth = portalAuth.LastResult();
    pipewire::AuthorizedStreamTarget authorized;
    const char* targetReason = nullptr;
    if (!pipewire::SelectAuthorizedStream(auth.selection, source, &authorized,
                                          &targetReason)) {
        const bool multiple = targetReason &&
                              std::string(targetReason) == pipewire::reason::kTargetNotUnique;
        Diagnostic d = PortalDiag(multiple ? codes::kPortalMultipleStreams
                                           : codes::kPortalSourceMismatch,
                                  Msg(L"cap.portal_capture_failed"),
                                  Msg(L"cap.portal_capture_failed_hint"), targetKey,
                                  targetReason ? W(targetReason) : std::wstring());
        outcome.exitCode = ExitCodeForPortalDiagnostic(d);
        outcome.errors.push_back(std::move(d));
        return outcome;
    }

    if (dl.Enabled() && dl.Spent()) {
        Diagnostic d = PortalDiag(codes::kCaptureTimeout, Msg(L"cap.portal_no_frame"),
                                  Msg(L"cap.portal_no_frame_hint"), targetKey,
                                  L"automatic_budget_expired_before_open_pipewire_remote");
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }
    const uint64_t remoteWaitMs =
        dl.Enabled() ? std::min<uint64_t>(dl.RemainingMs(), kPortalRemoteOpenTimeoutMs)
                     : kPortalRemoteOpenTimeoutMs;
    ScopedFd remoteFd;
    std::string openReason;
    if (!driver->OpenPipeWireRemote(&remoteFd.fd, remoteWaitMs, &openReason)) {
        Diagnostic d = PortalDiag(codes::kPortalPipeWireUnavailable,
                                  Msg(L"cap.portal_pipewire_unavailable"),
                                  Msg(L"cap.portal_pipewire_unavailable_hint"), targetKey,
                                  W(openReason.c_str()));
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }

    if (dl.Enabled() && dl.Spent()) {
        Diagnostic d = PortalDiag(codes::kCaptureTimeout, Msg(L"cap.portal_no_frame"),
                                  Msg(L"cap.portal_no_frame_hint"), targetKey,
                                  L"automatic_budget_expired_before_frame_wait");
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }
    pipewire::CaptureRequest captureRequest;
    captureRequest.expectedSource = source;
    captureRequest.selection = auth.selection;
    captureRequest.timeoutMs = dl.ClampWait(kPortalFrameTimeoutMs);
    captureRequest.cancelToken = &cancelToken;

    std::unique_ptr<pipewire::IPipeWireCapture> captureBackend =
        pipewire::CreatePipeWireCapture();
    if (!captureBackend) {
        Diagnostic d = PortalDiag(codes::kPortalPipeWireUnavailable,
                                  Msg(L"cap.portal_pipewire_unavailable"),
                                  Msg(L"cap.portal_pipewire_unavailable_hint"), targetKey);
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }

    // Capture 成功后 remote fd 已交给 PipeWire context；成功与失败路径都不再由 ScopedFd 重复关闭。
    const pipewire::CaptureResult captured =
        captureBackend->Capture(remoteFd.Release(), captureRequest);
    if (captured.outcome != pipewire::Outcome::kOk) {
        Diagnostic d = PipeWireFailure(captured, targetKey);
        outcome.exitCode = ExitCodeForPortalDiagnostic(d);
        outcome.errors.push_back(std::move(d));
        return outcome;
    }

    CapturedFrame frame = captured.frame;
    frame.source = L"portal";
    frame.path = path;
    frame.sourceColorSpace =
        captured.colorSpaceVerified ? FrameColorSpace::kSrgbBgra8 : FrameColorSpace::kUnknown;
    frame.toneMapped = false;
    frame.displayHdrState = DisplayHdrState::kUnknown;

    if (opt.hdr.given && opt.hdr.policy == HdrPolicy::kRefuse &&
        !captured.colorSpaceVerified) {
        Diagnostic d = PortalDiag(codes::kHdrUnverifiable, Msg(L"cap.hdr_unverifiable"),
                                  Msg(L"cap.hdr_unverifiable_hint"), targetKey,
                                  W(pipewire::reason::kHdrUnverified));
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }
    if (opt.hdr.given && opt.hdr.policy == HdrPolicy::kRefuse &&
        FrameColorSpaceIsHdr(frame.sourceColorSpace)) {
        Diagnostic d = PortalDiag(codes::kHdrRefused, Msg(L"cap.hdr_refused"),
                                  Msg(L"cap.hdr_refused_hint"), targetKey);
        outcome.errors.push_back(std::move(d));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }

    // Portal 的 cursor_mode 是来源选择契约：本工具不自行合成光标。Embedded 表示合成器
    // 已嵌入；Hidden / 默认 Hidden 表示帧里没有光标。两者都不再二次合成。
    if (opt.cursor.given) {
        frame.cursorStateKnown = true;
        frame.cursorInFrame = auth.requestedCursorMode == portal::CursorMode::kEmbedded;
    }

    CapturedImage image;
    image.file = outputPath;
    image.format = FormatName(opt.format);
    image.source = L"portal";
    image.path = path;
    image.scope = ScopeName(scope);
    image.screen = !window;
    if (!window) {
        // 交互式选择的 monitor 没有可核实的设备名/枚举序号；不拿内部目标 key 冒充设备。
        image.screenKind = L"portal-monitor";
    }
    image.cursor = MakeCursorReport(opt.cursor, image.path, frame.cursorStateKnown,
                                    frame.cursorInFrame);
    image.hdr = MakeHdrReport(opt.hdr, image.path, frame.sourceColorSpace,
                              frame.toneMapped, DisplayHdrState::kUnknown);

    image.portal.written = true;
    image.portal.sourceType = window ? L"window" : L"monitor";
    image.portal.targetBasis =
        authorized.serialKnown ? L"pipewire_serial" : L"portal_session_node_id_fallback";
    image.portal.serialKnown = authorized.serialKnown;
    image.portal.serial = authorized.serial;
    image.portal.nodeId = authorized.nodeId;
    image.portal.logicalSizeKnown = !auth.selection.streams.empty() &&
                                    auth.selection.streams.front().sizeKnown;
    if (image.portal.logicalSizeKnown) {
        const portal::StreamInfo& stream = auth.selection.streams.front();
        image.portal.logicalPositionKnown = stream.positionKnown;
        if (stream.positionKnown) {
            image.portal.logicalX = stream.x;
            image.portal.logicalY = stream.y;
        }
        image.portal.logicalWidth = stream.width;
        image.portal.logicalHeight = stream.height;
    }
    image.portal.bufferWidth = captured.sourceWidth;
    image.portal.bufferHeight = captured.sourceHeight;
    image.portal.rotationCw = captured.rotationCw;
    image.portal.sizeChangedWhileWaiting = captured.sizeChangedWhileWaiting;
    image.portal.sizeChanges = captured.sizeChanges;
    image.portal.frameTimestampKnown = captured.frameTimestampKnown;
    image.portal.frameTimestampVerified = captured.frameTimestampVerified;
    image.portal.memoryType = W(pipewire::MemoryTypeName(captured.memoryType));
    image.portal.pixelFormat = W(pipewire::PixelFormatName(captured.pixelFormat));
    image.portal.colorSpaceVerified = captured.colorSpaceVerified;
    image.portal.emptyChunks = captured.emptyChunks;
    image.portal.corruptedChunks = captured.corruptedChunks;
    image.portal.staleFrames = captured.staleFrames;
    image.portal.invalidFrames = captured.invalidFrames;

    if (!ApplyRoi(opt, &frame, &image, &outcome)) return outcome;
    if (!ApplyScale(opt, &frame, &image, &outcome)) return outcome;

    if (!FrameShapeOk(frame, L"portal", stages::kCapture, &outcome.errors.emplace_back())) {
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }

    std::vector<uint8_t> encoded;
    Diagnostic encodeErr;
    if (!EncodeFrame(frame, opt.format, opt.jpegQuality, dl, &encoded, &encodeErr)) {
        outcome.errors.push_back(std::move(encodeErr));
        outcome.exitCode = EX_CAPTURE_FAILED;
        return outcome;
    }

    image.width = frame.width;
    image.height = frame.height;

    PortalOutputSink sink;
    if (outputPath == L"-") ClaimStdout();
    const auto clock = [] { return static_cast<uint64_t>(MonotonicClockMs()); };
    const DeliveryRun delivery{&dl, &sink, clock,
                               historyArchive ? &*historyArchive : nullptr, opt.format,
                               !opt.historyEnabled};
    DeliveryTarget deliveryTarget;
    deliveryTarget.file = outputPath;
    deliveryTarget.tag = targetKey;
    deliveryTarget.backend = L"portal";
    deliveryTarget.overwrite = opt.overwrite;
    deliveryTarget.implicitStdout = opt.outputImplicitStdout;

    PendingImage pending;
    pending.image = std::move(image);
    pending.startedClockMs = clock();
    DeliveryStep step;
    DeliverImage(delivery, deliveryTarget, std::move(pending), encoded, &outcome, &step);
    if (!step.delivered && outcome.exitCode == EX_OK) outcome.exitCode = EX_IO_FAILED;
    if (step.delivered) {
        outcome.exitCode = OutcomeExitCode(outcome);
    }
    return outcome;
}

}  // namespace ecapture
