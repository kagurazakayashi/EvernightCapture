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
// 本构建只有一条采集通道：X11 单窗口取帧（xcomposite，读窗口自己的重定向存储）。
// 所以 backends 恰好一条、autoChainWindow 恰好一条、autoChainScreen 为空（没有桌面像素路径）。
// 那条通道的 status 由三问算出来（X11 连上没有、服务端有没有 Composite 与 RENDER），
// 不会因为"X11 连上了"就直接回答"可以截图"，也不会把计划能力写成运行事实。

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

// 服务端声明的扩展清单里有没有这一条。清单是 xcb_list_extensions 的整份回复（不截断），
// 所以"没有"是服务端真的没声明，而不是这次没问全。
bool HasServerExtension(const std::vector<std::wstring>& extensions, const wchar_t* name) {
    for (const auto& extension : extensions) {
        if (extension == name) return true;
    }
    return false;
}

void WriteConsentPaths(Json& j, const std::vector<ConsentPathReport>& paths) {
    j.Arr();
    for (const auto& p : paths) {
        j.Obj();
        j.Key(L"target").Value(p.target);
        j.Key(L"path").Value(p.path);
        j.Key(L"scope").Value(p.scope);
        j.Key(L"consentWithoutYes").Value(p.consentWithoutYes);
        j.Key(L"consentWithYes").Value(p.consentWithYes);
        j.End();
    }
    j.End();
}

void WriteBackends(Json& j, const std::vector<BackendReport>& backends) {
    j.Key(L"backends").Arr();
    for (const auto& b : backends) {
        j.Obj();
        j.Key(L"name").Value(b.name);
        j.Key(L"compiled").Value(b.compiled);
        j.Key(L"status").Value(CapStatusName(b.status));
        j.Key(L"reason").Value(b.reason);
        j.Key(L"minBuild").Value(static_cast<long long>(b.minBuild));
        j.Key(L"verifiedOnThisMachine").Value(TriWord(b.verifiedOnThisMachine));
        j.Key(L"paths");
        WriteConsentPaths(j, b.paths);
        j.End();
    }
    j.End();
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

void WriteCursor(Json& j, const EnvCursorReport& cursor) {
    j.Key(L"cursor").Obj();
    j.Key(L"option").Value(cursor.option);
    j.Key(L"default").Value(cursor.defaultValue);
    WriteStringArray(j, L"values", cursor.values);
    j.Key(L"switch").Obj();
    j.Key(L"api").Value(cursor.control.api);
    j.Key(L"compiled").Value(cursor.control.compiled);
    j.Key(L"status").Value(CapStatusName(cursor.control.status));
    j.Key(L"reason").Value(cursor.control.reason);
    j.Key(L"minBuild").Value(static_cast<long long>(cursor.control.minBuild));
    j.Key(L"verifiedOnThisMachine").Value(TriWord(cursor.control.verifiedOnThisMachine));
    j.End();
    j.Key(L"paths").Arr();
    for (const auto& p : cursor.paths) {
        j.Obj();
        j.Key(L"path").Value(p.path);
        j.Key(L"capability").Value(p.capability);
        j.Key(L"reason").Value(p.reason);
        j.Key(L"include").Value(p.includeState);
        j.Key(L"exclude").Value(p.excludeState);
        j.End();
    }
    j.End();
    j.Key(L"pointerShapeCompositing").Value(cursor.pointerCompositing);
    j.Key(L"pixelRetouching").Value(cursor.pixelRetouching);
    j.End();
}

void WriteColor(Json& j, const EnvHdrReport& hdr) {
    j.Key(L"color").Obj();
    j.Key(L"option").Value(hdr.option);
    j.Key(L"default").Value(hdr.defaultValue);
    WriteStringArray(j, L"values", hdr.values);
    j.Key(L"compiled").Value(hdr.compiled);
    j.Key(L"status").Value(CapStatusName(hdr.status));
    j.Key(L"reason").Value(hdr.reason);
    j.Key(L"verifiedOnThisMachine").Value(TriWord(hdr.verifiedOnThisMachine));
    j.Key(L"toneMapping").Value(hdr.toneMapping);
    j.Key(L"floatIntermediateFrame").Value(hdr.floatIntermediateFrame);
    j.Key(L"encoderOutput").Value(hdr.encoderOutput);
    j.Key(L"paths").Arr();
    for (const auto& p : hdr.paths) {
        j.Obj();
        j.Key(L"path").Value(p.path);
        j.Key(L"capability").Value(p.capability);
        j.Key(L"reason").Value(p.reason);
        j.Key(L"honorsExplicitPolicy").Value(p.honorsExplicitPolicy);
        j.End();
    }
    j.End();
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

    // ---- 后端：本构建只有一条 X11 单窗口通道（xcomposite）----
    // 它读目标窗口（及子窗口）自己的重定向存储，一个像素都不从屏幕上取，所以是窗口内容路径。
    // 能不能走要三问：X11 连上了没有、服务端声明了 Composite 与 RENDER 没有。
    {
        BackendReport b;
        b.method = CaptureMethod::kXComposite;
        b.name = CaptureMethodName(b.method);
        b.compiled = true;
        // 授权路径只有"窗口"这一条：本构建没有屏幕目标路径，不虚构 screen 那一条。
        const wchar_t* const winPath = WindowPathOf(b.method);
        ConsentPathReport path;
        path.target = L"window";
        path.path = winPath;
        path.scope = ScopeName(ScopeOf(winPath));
        path.consentWithoutYes = NeedsHumanConsent(winPath, false);
        path.consentWithYes = NeedsHumanConsent(winPath, true);
        b.paths.push_back(std::move(path));

        const X11Facts& x11 = probe.session.x11;
        if (!x11.compiled) {
            b.status = CapStatus::kUnavailable;
            b.reason = cap_reason::kNotCompiled;
        } else if (!x11.attempted) {
            // DISPLAY 没设：这一会话没有可用的显示输出，不是"后端坏了"。
            b.status = CapStatus::kUnavailable;
            b.reason = cap_reason::kNoDisplayTopology;
        } else if (!x11.connected) {
            b.status = CapStatus::kUnavailable;
            b.reason = cap_reason::kDisplayUnavailable;
        } else if (!HasServerExtension(x11.extensions, L"Composite")) {
            b.status = CapStatus::kUnavailable;
            b.reason = cap_reason::kCompositeUnavailable;
        } else if (!HasServerExtension(x11.extensions, L"RENDER")) {
            b.status = CapStatus::kUnavailable;
            b.reason = cap_reason::kRenderUnavailable;
        } else {
            b.status = CapStatus::kAvailable;
            b.reason = cap_reason::kNone;
        }
        // 这一格里"本项目在这台机器上实测过没有"：Linux 没有可核对的版本登记，
        // 所以写 unknown（这一问没答案），不冒充 yes。
        b.verifiedOnThisMachine = Tri::kUnknown;
        r.autoChainWindow.push_back(b.name);
        r.backends.push_back(std::move(b));
        // autoChainScreen 留空：本构建没有桌面像素路径，--monitor 在解析期就报
        // capture.unsupported，这里不把窗口通道冒充成屏幕链。
    }

    // ---- 光标（--cursor）----
    r.cursor.option = L"--cursor";
    r.cursor.defaultValue = L"default";
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"cursor") r.cursor.values = info.allowedValues;
    }
    // 本构建里没有任何可读回的光标开关：X11 的指针由服务器合成到屏幕上，不写进窗口存储。
    // 所以 compiled=false（这条接口没实现），而 xcomposite 那条路径的来源本来就没有指针：
    // exclude 照实成立，include 做不到（解析期就报 cursor.include_unsupported）。
    r.cursor.control.api = L"none";
    r.cursor.control.compiled = false;
    r.cursor.control.status = CapStatus::kUnavailable;
    r.cursor.control.reason = cap_reason::kNotCompiled;
    r.cursor.control.verifiedOnThisMachine = Tri::kUnknown;
    {
        CursorPathReport p;
        p.path = paths::kXComposite;
        p.capability = CursorCapabilityName(CursorCapabilityOfPath(p.path.c_str()));
        p.reason = CursorReasonOfPath(p.path.c_str());
        p.includeState = TriWord(Tri::kNo);
        p.excludeState = TriWord(Tri::kYes);
        r.cursor.paths.push_back(std::move(p));
    }

    // ---- HDR 色彩处理（--hdr）----
    r.hdr.option = L"--hdr";
    r.hdr.defaultValue = L"auto";
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"hdr") r.hdr.values = info.allowedValues;
    }
    // xcomposite 交回的是窗口存储里的 8 位 SDR 像素：没有广色域来源帧、没有浮点中间帧、
    // 也没有 PQ/HLG 解码。所以这一条路径兑现不了显式 tonemap/refuse，解析期就报做不到。
    r.hdr.compiled = false;
    r.hdr.status = CapStatus::kUnavailable;
    r.hdr.reason = cap_reason::kNotCompiled;
    r.hdr.verifiedOnThisMachine = Tri::kNo;
    r.hdr.toneMapping = L"not_implemented";
    r.hdr.floatIntermediateFrame = L"not_implemented";
    r.hdr.encoderOutput = L"not_implemented";
    {
        HdrPathReport p;
        p.path = paths::kXComposite;
        p.capability = HdrCapabilityName(HdrCapabilityOfPath(p.path.c_str()));
        p.reason = HdrReasonOfPath(p.path.c_str());
        p.honorsExplicitPolicy = HdrPathHonorsPolicy(p.path.c_str());
        r.hdr.paths.push_back(std::move(p));
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

    WriteBackends(j, r.backends);
    WriteCursor(j, r.cursor);
    WriteColor(j, r.hdr);
    WriteFormats(j);
    WriteStringArray(j, L"autoChainWindow", r.autoChainWindow);
    WriteStringArray(j, L"autoChainScreen", r.autoChainScreen);
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
