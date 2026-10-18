// 【平台层 · Linux】--capabilities / --diagnostics 的 Linux 实现。
//
// 与 Windows 那一份（WinEnvReport.cpp）共享同一个 EnvReport 模型与同一套机器词，但探测事实
// 来自本平台的只读会话探针（PosixSessionProbe.cpp）。额外写下这一平台必须说清的三段：
//   * session：会话种类、X11 / Wayland / Portal / PipeWire 的连接结果与版本；
//   * runtime：每条能力五轴分开（compiled / reachable / versionSatisfied /
//     interactionRequired / implemented），status 与 canCapture 由纯判据算；
//   * diagnostics：缺少库、服务不存在、会话无法访问、连接失败、能力未实现各一条，
//     带稳定的 detail 与下一步 hint。
//
// 这份实现**没有**采集后端：backends 为空、autoChain 为空，canCapture 逐条为 false。
// 它不会因为"X11 连上了"就回答"可以截图"，也不会把计划能力写成运行事实。

#include "EnvReport.h"

#include <cstring>
#include <string>
#include <vector>

#include <sys/utsname.h>

#include "CaptureScope.h"
#include "CliOptions.h"
#include "core/CodecCapability.h"   // 格式支持矩阵：查询与实请求同源
#include "CursorControl.h"
#include "Deadline.h"
#include "HdrColor.h"
#include "Json.h"
#include "SessionProbe.h"
#include "WgcGeometry.h"
#include "core/Frame.h"

namespace ecapture {
namespace {

std::wstring W(const char* text) {
    std::wstring out;
    if (!text) return out;
    while (*text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    return out;
}

std::wstring TriWord(Tri value) {
    switch (value) {
        case Tri::kYes: return L"yes";
        case Tri::kNo: return L"no";
        case Tri::kUnknown: return L"unknown";
    }
    return L"unknown";
}

void WriteTri(Json& j, const wchar_t* key, Tri value) { j.Key(key).Value(TriWord(value)); }

void WriteStringArray(Json& j, const wchar_t* key, const std::vector<std::wstring>& items) {
    j.Key(key).Arr();
    for (const auto& s : items) j.Value(s);
    j.End();
}

std::wstring ArchName(const utsname& uts) {
    const std::string machine(uts.machine);
    if (machine == "x86_64" || machine == "amd64") return L"x64";
    if (machine == "aarch64" || machine == "arm64") return L"arm64";
    if (machine == "i386" || machine == "i486" || machine == "i586" || machine == "i686")
        return L"x86";
    if (machine.compare(0, 3, "arm") == 0) return L"arm";
    return std::wstring(machine.begin(), machine.end());
}

void AddUnique(std::vector<std::wstring>* out, const std::wstring& value) {
    for (const auto& existing : *out) {
        if (existing == value) return;
    }
    out->push_back(value);
}

void WriteBackendsEmpty(Json& j) {
    // 这个构建没有任何 Linux 采集后端。不写一个空数组之外的假路线：调用方读 runtime
    // 那一段就能看到每条能力卡在哪一轴（未编译 / 未实现 / 需要交互）。
    j.Key(L"backends").Arr().End();
}

void WriteFormats(Json& j) {
    constexpr ImageFormat kFormats[] = {ImageFormat::kPng, ImageFormat::kJpeg, ImageFormat::kBmp,
                                        ImageFormat::kTiff, ImageFormat::kGif};
    j.Key(L"formats").Arr();
    for (const ImageFormat format : kFormats) {
        const CodecSupport support = CodecSupportFor(BuildCodecPath(), format);
        bool compiled = support == CodecSupport::kSupported;
        const wchar_t* reason = L"none";
        if (format == ImageFormat::kPng) {
#if !defined(ECAPTURE_HAVE_PNG)
            compiled = false;
            reason = L"library_not_linked";
#endif
        } else if (format == ImageFormat::kJpeg) {
#if !defined(ECAPTURE_HAVE_JPEG)
            compiled = false;
            reason = L"library_not_linked";
#endif
        } else if (support == CodecSupport::kNoEncoder) {
            reason = L"no_encoder";
        }
        j.Obj();
        j.Key(L"name").Value(FormatName(format));
        j.Key(L"compiled").Value(compiled);
        j.Key(L"status").Value(compiled ? L"available" : L"unavailable");
        j.Key(L"reason").Value(reason);
        j.Key(L"minBuild").Value(0);
        // 这次没有实测编码器（与 Windows 那条规矩同源）：登记状态一律 unknown。
        j.Key(L"registered").Value(L"unknown");
        j.End();
    }
    j.End();
}

void WriteCursorEmpty(Json& j) {
    j.Key(L"cursor").Obj();
    j.Key(L"option").Value(L"--cursor");
    j.Key(L"default").Value(L"default");
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"cursor") WriteStringArray(j, L"values", info.allowedValues);
    }
    j.Key(L"switch").Obj();
    j.Key(L"api").Value(L"none");
    j.Key(L"compiled").Value(false);
    j.Key(L"status").Value(L"not_implemented");
    j.Key(L"reason").Value(L"linux_capture_backend_not_implemented");
    j.Key(L"minBuild").Value(0);
    j.Key(L"verifiedOnThisMachine").Value(L"no");
    j.End();
    j.Key(L"paths").Arr().End();
    j.Key(L"pointerShapeCompositing").Value(L"never");
    j.Key(L"pixelRetouching").Value(L"never");
    j.End();
}

void WriteColorEmpty(Json& j) {
    j.Key(L"color").Obj();
    j.Key(L"option").Value(L"--hdr");
    j.Key(L"default").Value(L"auto");
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"hdr") WriteStringArray(j, L"values", info.allowedValues);
    }
    j.Key(L"compiled").Value(false);
    j.Key(L"status").Value(L"not_implemented");
    j.Key(L"reason").Value(L"linux_capture_backend_not_implemented");
    j.Key(L"verifiedOnThisMachine").Value(L"no");
    j.Key(L"toneMapping").Value(L"not_implemented");
    j.Key(L"floatIntermediateFrame").Value(L"not_implemented");
    j.Key(L"encoderOutput").Value(L"not_implemented");
    j.Key(L"paths").Arr().End();
    j.End();
}

void WriteHistory(Json& j) {
    j.Key(L"history").Obj();
    j.Key(L"supported").Value(true);
    j.Key(L"enabledByDefault").Value(true);
    j.Key(L"configKey").Value(L"history.enabled");
    j.Key(L"configOverride").Value(L"--history on|off");
    j.Key(L"relativeTo").Value(L"user-state-directory-or-executable-directory-when-portable");
    j.Key(L"location").Value(L"history/YYYY-MM-DD/");
    j.Key(L"naming").Value(L"YYYYMMDD-HHMMSS-<pid>-<token>-<seq>.<ext>");
    j.Key(L"source").Value(L"same-encoded-bytes");
    j.Key(L"commit").Value(L"exclusive-create");
    j.Key(L"created").Value(L"after-first-delivered-image");
    j.Key(L"retention").Value(L"never-pruned-automatically");
    j.Key(L"uploads").Value(false);
    j.Key(L"backgroundPruning").Value(false);
    j.Key(L"writabilityProbed").Value(false);
    j.Key(L"partialSuccessExit").Value(static_cast<long long>(EX_CAPTURE_FAILED));
    j.End();
}

void WriteLimits(Json& j, const EnvReport& r) {
    j.Key(L"limits").Obj();
    j.Key(L"maxFrameSide").Value(static_cast<long long>(r.limits.maxFrameSide));
    j.Key(L"maxFrameBytes").Value(static_cast<long long>(r.limits.maxFrameBytes));
    j.Key(L"maxTimeoutMs").Value(static_cast<long long>(r.limits.maxTimeoutMs));
    j.Key(L"isolatedCallMs").Value(static_cast<long long>(r.limits.isolatedCallMs));
    j.Key(L"maxWgcRecreates").Value(static_cast<long long>(r.limits.maxWgcRecreates));
    j.Key(L"maxOrdinal").Value(static_cast<long long>(r.limits.maxOrdinal));
    j.Key(L"maxPid").Value(static_cast<long long>(r.limits.maxPid));
    j.Key(L"stdoutTargetsMax").Value(static_cast<long long>(r.limits.stdoutTargetsMax));
    j.Key(L"jpegQualityMin").Value(static_cast<long long>(r.limits.jpegQualityMin));
    j.Key(L"jpegQualityMax").Value(static_cast<long long>(r.limits.jpegQualityMax));
    j.Key(L"roiMaxValue").Value(static_cast<long long>(r.limits.roiMaxValue));
    j.End();
}

void WriteProbes(Json& j, const EnvReport& r) {
    j.Key(L"probes").Arr();
    j.Obj().Key(L"question").Value(L"sessionKind")
        .Key(L"known").Value(r.sessionReport.kind != SessionKind::kUnknown)
        .Key(L"answer").Value(W(SessionKindName(r.sessionReport.kind)))
        .Key(L"source").Value(L"socket connect (X11 / Wayland)")
        .End();
    j.Obj().Key(L"question").Value(L"x11")
        .Key(L"known").Value(r.sessionReport.x11.attempted)
        .Key(L"answer").Value(r.sessionReport.x11.connected      ? L"connected"
                                    : r.sessionReport.x11.attempted ? L"connect_failed"
                                                                    : L"not_attempted")
        .Key(L"source").Value(L"xcb_connect + xcb_list_extensions")
        .End();
    j.Obj().Key(L"question").Value(L"wayland")
        .Key(L"known").Value(r.sessionReport.wayland.attempted)
        .Key(L"answer").Value(r.sessionReport.wayland.connected ? L"connected"
                                    : r.sessionReport.wayland.attempted ? L"connect_failed"
                                                                        : L"not_attempted")
        .Key(L"source").Value(L"wl_display_connect + wl_registry roundtrip")
        .End();
    j.Obj().Key(L"question").Value(L"portal")
        .Key(L"known").Value(r.sessionReport.portal.attempted)
        .Key(L"answer").Value(r.sessionReport.portal.busConnected
                                    ? (r.sessionReport.portal.serviceOwner ? L"service_owner"
                                                                           : L"service_absent")
                                    : r.sessionReport.portal.attempted ? L"connect_failed"
                                                                       : L"not_attempted")
        .Key(L"source").Value(L"dbus_connection_open_private + Properties.Get")
        .End();
    j.Obj().Key(L"question").Value(L"pipewirePrerequisite")
        .Key(L"known").Value(r.sessionReport.pipewire.socketPresentKnown)
        .Key(L"answer").Value(r.sessionReport.pipewire.socketPresent ? L"socket_present"
                                                                     : L"socket_absent")
        .Key(L"source").Value(L"stat(XDG_RUNTIME_DIR/pipewire-0)")
        .End();
    j.Obj().Key(L"question").Value(L"environment")
        .Key(L"known").Value(true)
        .Key(L"answer").Value(L"names_only")
        .Key(L"source").Value(L"session environment allowlist (no values echoed)")
        .End();
    j.End();
}

void WriteBuildDetail(Json& j) {
    j.Key(L"build").Obj();
    j.Key(L"known").Value(false);
    j.Key(L"id").Value(L"unknown");
    // Linux 没有 PE 头：这一格不是"构建标识读失败"，而是这一套格式不适用。
    j.Key(L"reason").Value(L"no_pe_header_on_linux");
    j.End();
}

}  // namespace

const wchar_t* TriName(Tri value) {
    switch (value) {
        case Tri::kYes: return L"yes";
        case Tri::kNo: return L"no";
        case Tri::kUnknown: return L"unknown";
    }
    return L"unknown";
}

const wchar_t* CapStatusName(CapStatus status) {
    switch (status) {
        case CapStatus::kAvailable: return L"available";
        case CapStatus::kUnavailable: return L"unavailable";
        case CapStatus::kUnverified: return L"unverified";
    }
    return L"unverified";
}

const wchar_t* EnvContractName(EnvQueryKind kind) {
    return kind == EnvQueryKind::kDiagnostics ? L"diagnostics" : L"capabilities";
}

Tri EnvProbe::EncoderRegistered(ImageFormat format) const {
    for (const auto& e : encoders) {
        if (e.first == format) return e.second;
    }
    return Tri::kUnknown;
}

EnvProbe ProbeEnvFacts() {
    EnvProbe p;
    p.session = ProbeSessionFacts();
    p.osSystem = L"linux";
    struct utsname uts {};
    if (::uname(&uts) == 0) {
        p.kernelRelease.assign(uts.release, uts.release + std::strlen(uts.release));
        p.arch = ArchName(uts);
    }
    const bool display = p.session.x11.connected || p.session.wayland.connected;
    const bool probeRan = p.session.x11.compiled || p.session.wayland.compiled;
    p.consoleAttached = display ? Tri::kYes : (probeRan ? Tri::kNo : Tri::kUnknown);
    p.remoteSession = (p.session.environment.sshConnectionSet ||
                       p.session.environment.sshTtySet)
                          ? Tri::kYes
                          : Tri::kUnknown;
    p.displayTopology = display ? Tri::kYes : (probeRan ? Tri::kNo : Tri::kUnknown);
    p.monitorCountKnown = display;
    if (p.session.wayland.connected) {
        p.monitorCount = p.session.wayland.outputCount;
    } else if (p.session.x11.connected) {
        p.monitorCount = p.session.x11.screenCount;
    }
    p.elevated = Tri::kUnknown;
    p.integrity = Integrity::kUnknown;
    p.buildIdKnown = false;
    return p;
}

const BackendReport* EnvReport::BackendOf(CaptureMethod method) const {
    for (const auto& b : backends) {
        if (b.method == method) return &b;
    }
    return nullptr;
}

EnvReport BuildEnvReport(const EnvProbe& probe, EnvQueryKind kind) {
    EnvReport r;
    r.probe = probe;
    r.kind = kind;
    r.contract = EnvContractName(kind);
    r.contractVersion = kEnvContractVersion;
    r.programName = L"EvernightCapture";
    r.binaryName = L"ECAPTURE";
    r.version = kVersion;
    r.arch = probe.arch.empty() ? L"unknown" : probe.arch;
    r.buildId = L"unknown";
    r.os = probe.os;   // Linux 上 known=false：Windows 内部版本不是本平台的版本
    r.osSystem = probe.osSystem;
    r.kernelRelease = probe.kernelRelease;
    r.declaredMinBuild = 0;
    r.encoderMinBuild = 0;
    r.verifiedOsBuild = 0;
    r.verifiedArch = L"";
    // 本项目从未在任何 Linux 桌面 / 合成器上做过端到端实测：这是"未验证"，不是"不支持"。
    r.matchesVerifiedEnv = Tri::kNo;
    r.consoleAttached = probe.consoleAttached;
    r.remoteSession = probe.remoteSession;
    r.elevated = Tri::kUnknown;
    r.integrityLevel = L"unknown";
    r.displayTopology = probe.displayTopology;
    r.monitorCountKnown = probe.monitorCountKnown;
    r.monitorCount = probe.monitorCount;
    r.processSessionKnown = false;
    r.consoleSessionKnown = false;
    // 只读查询不弹框，也不预测"这里有没有人能看见"。
    r.consentDialogExpected = Tri::kUnknown;

    r.limits.maxFrameSide = kFrameMaxSide;
    r.limits.maxFrameBytes = kFrameMaxBytes;
    r.limits.maxTimeoutMs = cli_limits::kMaxTimeoutMs;
    r.limits.isolatedCallMs = kIsolatedCallMs;
    r.limits.maxWgcRecreates = kMaxWgcRecreates;
    r.limits.maxOrdinal = cli_limits::kMaxOrdinal;
    r.limits.maxPid = cli_limits::kMaxPid;
    r.limits.stdoutTargetsMax = 1;
    r.limits.jpegQualityMin = cli_limits::kJpegQualityMin;
    r.limits.jpegQualityMax = cli_limits::kJpegQualityMax;
    r.limits.roiMaxValue = cli_limits::kRoiMaxValue;

    // 会话与运行能力：纯判据本体在 SessionReport.cpp，平台只交事实。
    r.sessionReport = BuildSessionReport(probe.session);
    r.hasSessionReport = true;
    r.environmentAllowlist = {L"XDG_SESSION_TYPE", L"DISPLAY",  L"WAYLAND_DISPLAY",
                             L"SSH_CONNECTION",  L"SSH_TTY", L"XDG_RUNTIME_DIR",
                             L"DBUS_SESSION_BUS_ADDRESS", L"XDG_CURRENT_DESKTOP",
                             L"DESKTOP_SESSION"};

    // 这个构建没有 Linux 采集后端：backends / autoChain 为空，cursor / color 只有"未实现"。
    r.cursor.control.compiled = false;
    r.cursor.control.status = CapStatus::kUnavailable;
    r.cursor.control.reason = L"linux_capture_backend_not_implemented";
    r.cursor.control.verifiedOnThisMachine = Tri::kNo;
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"cursor") r.cursor.values = info.allowedValues;
    }
    r.hdr.compiled = false;
    r.hdr.status = CapStatus::kUnavailable;
    r.hdr.reason = L"linux_capture_backend_not_implemented";
    r.hdr.verifiedOnThisMachine = Tri::kNo;
    r.hdr.toneMapping = L"not_implemented";
    r.hdr.floatIntermediateFrame = L"not_implemented";
    r.hdr.encoderOutput = L"not_implemented";
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"hdr") r.hdr.values = info.allowedValues;
    }

    // 隐私自述：Linux 只读查询确实读了会话环境，但只读白名单里的**名字**、不读值；
    // 不取像素、不弹框、不上传、不枚举用户文件、不输出用户名与路径。
    r.privacyCapturesScreen = false;
    r.privacyShowsDialog = false;
    r.privacyUploads = false;
    r.privacyEnumeratesUserFiles = false;
    r.privacyReadsEnvVars = true;
    r.privacyIncludesUsernames = false;
    r.privacyIncludesPaths = false;

    for (const auto& c : r.sessionReport.caveats) AddUnique(&r.caveats, c);
    AddUnique(&r.caveats, caveat::kNoCapture);
    AddUnique(&r.caveats, caveat::kNoDialog);
    AddUnique(&r.caveats, caveat::kNotAGuarantee);
    AddUnique(&r.caveats, caveat::kEncoderUnprobed);
    AddUnique(&r.caveats, caveat::kLinuxBackendNotImplemented);
    AddUnique(&r.caveats, caveat::kSessionQueryReadOnly);
    AddUnique(&r.caveats, caveat::kEnvironmentNotEvidence);
    AddUnique(&r.caveats, caveat::kDesktopNameNotEvidence);
    AddUnique(&r.caveats, caveat::kXwaylandNotNative);
    AddUnique(&r.caveats, caveat::kWaylandNoWindowList);
    AddUnique(&r.caveats, caveat::kPortalNeedsInteraction);
    AddUnique(&r.caveats, caveat::kSshNotHeadless);
    AddUnique(&r.caveats, caveat::kUnknownIsNotNo);
    AddUnique(&r.caveats, caveat::kHistoryWritabilityNotProbed);
    AddUnique(&r.caveats, caveat::kHistoryRunStateNotReported);
    AddUnique(&r.caveats, caveat::kNotTestedHere);
    return r;
}

std::wstring RenderEnvJson(const EnvReport& r, bool verbose, bool quiet) {
    Json j;
    j.Obj();
    j.Key(L"contract").Value(r.contract);
    j.Key(L"contractVersion").Value(static_cast<long long>(r.contractVersion));

    j.Key(L"program").Obj();
    j.Key(L"name").Value(r.programName);
    j.Key(L"binary").Value(r.binaryName);
    j.Key(L"version").Value(r.version);
    j.Key(L"arch").Value(r.arch);
    j.Key(L"buildId").Value(r.buildId);
    j.End();

    j.Key(L"platform").Value(W(PlatformName(r.sessionReport.platform)));

    j.Key(L"os").Obj();
    j.Key(L"system").Value(r.osSystem);
    j.Key(L"kernelRelease").Value(r.kernelRelease);
    j.Key(L"arch").Value(r.arch);
    j.Key(L"known").Value(!r.kernelRelease.empty());
    j.End();

    // 会话与运行能力（含 diagnostics）；这一节是既有 capabilities/diagnostics 文档的
    // 兼容扩展：老字段一个不少，新事实全部追加在这里。
    WriteSessionObject(j, r.sessionReport, verbose);

    j.Key(L"authorization").Obj();
    j.Key(L"yesFlag").Value(L"--yes");
    j.Key(L"yesSkips").Value(L"window-content");
    j.Key(L"desktopPixelsAlwaysAsk").Value(true);
    j.Key(L"authorizationGranted").Value(false);
    j.Key(L"sourceSelectionRequested").Value(false);
    j.Key(L"captureSessionCreated").Value(false);
    j.End();

    WriteBackendsEmpty(j);
    WriteCursorEmpty(j);
    WriteColorEmpty(j);
    WriteFormats(j);
    WriteStringArray(j, L"autoChainWindow", {});
    WriteStringArray(j, L"autoChainScreen", {});
    WriteHistory(j);
    WriteLimits(j, r);

    j.Key(L"privacy").Obj();
    j.Key(L"capturesScreen").Value(r.privacyCapturesScreen);
    j.Key(L"showsDialog").Value(r.privacyShowsDialog);
    j.Key(L"uploads").Value(r.privacyUploads);
    j.Key(L"enumeratesUserFiles").Value(r.privacyEnumeratesUserFiles);
    j.Key(L"readsEnvironmentVariables").Value(r.privacyReadsEnvVars);
    j.Key(L"includesUsernames").Value(r.privacyIncludesUsernames);
    j.Key(L"includesPaths").Value(r.privacyIncludesPaths);
    j.Key(L"environmentValuesEchoed").Value(false);
    WriteStringArray(j, L"environmentAllowlist", r.environmentAllowlist);
    j.End();

    if (r.kind == EnvQueryKind::kDiagnostics) WriteBuildDetail(j);
    if (verbose) WriteProbes(j, r);
    if (!quiet) WriteStringArray(j, L"caveats", r.caveats);
    j.End();
    return j.Str();
}

}  // namespace ecapture
