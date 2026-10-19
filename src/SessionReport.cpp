#include "core/SessionCapability.h"

#include <string>
#include <vector>

#include "Json.h"

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

const wchar_t* HintFor(const std::wstring& reason) {
    if (reason == W(session_reason::kLibNotCompiled))
        return L"install the development package and rebuild; this build has no probe for it";
    if (reason == W(session_reason::kDisplayNotConfigured))
        return L"start a graphical session or point DISPLAY / WAYLAND_DISPLAY at the running server";
    if (reason == W(session_reason::kConnectFailed))
        return L"check that the display server / session bus / socket is running and reachable";
    if (reason == W(session_reason::kServiceNotFound))
        return L"start the session D-Bus and xdg-desktop-portal service; the query never starts it for you";
    if (reason == W(session_reason::kInterfaceMissing))
        return L"this portal implementation does not provide the interface; use a desktop that does";
    if (reason == W(session_reason::kVersionTooLow))
        return L"upgrade the portal or the compositor; the interface version is below the minimum";
    if (reason == W(session_reason::kSourceTypesUnavailable))
        return L"AvailableSourceTypes could not be read; do not guess which source types exist";
    if (reason == W(session_reason::kPrerequisiteMissing))
        return L"set XDG_RUNTIME_DIR and start PipeWire; a socket is a prerequisite, not a capture";
    if (reason == W(session_reason::kBackendNotImplemented))
        return L"this build has no Linux capture backend; this query only reports facts";
    if (reason == W(session_reason::kInteractionRequired))
        return L"a person must choose the portal source and approve the system grant";
    if (reason == W(session_reason::kProtocolError))
        return L"the server answered but the protocol exchange failed; retry after checking the session";
    if (reason == W(session_reason::kVersionUnverified))
        return L"the interface version was not readable; do not treat it as supported or unsupported";
    return L"the probe did not produce a usable answer; do not read it as yes or no";
}

RuntimeStatus StatusOf(const RuntimeCapabilityReport& r) {
    if (!r.compiled) return RuntimeStatus::kNotCompiled;
    if (r.reachable == Tri::kNo) return RuntimeStatus::kUnavailable;
    if (r.reachable == Tri::kUnknown) return RuntimeStatus::kUnverified;
    if (r.versionSatisfied == Tri::kNo) return RuntimeStatus::kUnavailable;
    if (r.implemented == Tri::kNo) return RuntimeStatus::kNotImplemented;
    if (r.implemented == Tri::kUnknown) return RuntimeStatus::kUnverified;
    if (r.versionSatisfied == Tri::kUnknown) return RuntimeStatus::kUnverified;
    if (r.interactionRequired == Tri::kYes) return RuntimeStatus::kNeedsInteraction;
    if (r.interactionRequired == Tri::kUnknown) return RuntimeStatus::kUnverified;
    return RuntimeStatus::kAvailable;
}

std::wstring ReasonOf(const RuntimeCapabilityReport& r) {
    if (!r.compiled) return W(session_reason::kLibNotCompiled);
    if (r.reachable == Tri::kNo)
        return r.reason.empty() ? W(session_reason::kConnectFailed) : r.reason;
    if (r.reachable == Tri::kUnknown) return W(session_reason::kProbeNotCompiled);
    if (r.versionSatisfied == Tri::kNo) return W(session_reason::kVersionTooLow);
    if (r.versionSatisfied == Tri::kUnknown) return W(session_reason::kVersionUnverified);
    if (r.implemented == Tri::kNo) return W(session_reason::kBackendNotImplemented);
    if (r.implemented == Tri::kUnknown) return W(session_reason::kProbeNotCompiled);
    if (r.interactionRequired == Tri::kYes) return W(session_reason::kInteractionRequired);
    if (r.interactionRequired == Tri::kUnknown) return W(session_reason::kVersionUnverified);
    return W(session_reason::kOk);
}

void Finalize(RuntimeCapabilityReport* r) {
    r->status = StatusOf(*r);
    r->reason = ReasonOf(*r);
    r->canCapture = r->status == RuntimeStatus::kAvailable;
}

void AddDiagnostic(SessionReport* report, const wchar_t* code, const wchar_t* detail,
                   const wchar_t* hint, SessionSeverity severity) {
    SessionDiagnostic d;
    d.code = code;
    d.detail = detail;
    d.hint = hint;
    d.severity = severity;
    report->diagnostics.push_back(std::move(d));
}

std::wstring VersionText(uint32_t major, uint32_t minor) {
    std::wstring out = std::to_wstring(major);
    out.push_back(L'.');
    out += std::to_wstring(minor);
    return out;
}

Tri VersionAtLeast(bool known, uint32_t major, uint32_t minor, uint32_t minMajor,
                   uint32_t minMinor) {
    if (!known) return Tri::kUnknown;
    if (major > minMajor) return Tri::kYes;
    if (major < minMajor) return Tri::kNo;
    return minor >= minMinor ? Tri::kYes : Tri::kNo;
}

}  // namespace

const RuntimeCapabilityReport* SessionReport::RuntimeOf(const wchar_t* name) const {
    if (!name) return nullptr;
    for (const auto& r : runtime) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

SessionReport BuildSessionReport(const SessionFacts& facts) {
    SessionReport report;
    report.environment = facts.environment;
    report.x11 = facts.x11;
    report.wayland = facts.wayland;
    report.portal = facts.portal;
    report.pipewire = facts.pipewire;
    report.ssh = facts.environment.sshConnectionSet || facts.environment.sshTtySet;
    // 只有"确实连上了"才是 yes；没有 SSH 线索也不是"确定不是远程"，所以 else 是 unknown。
    report.remoteSession = report.ssh ? Tri::kYes : Tri::kUnknown;
    report.authorizationGranted = false;
    report.xwaylandCoversOnlyX11Windows = true;
    report.nativeWaylandWindowQuerySupported = false;
    report.xwaylandAuthorizationSubstitute = false;

    // ---- 会话种类：只由实际连接结果支撑 ----
    const bool x11 = facts.x11.connected;
    const bool wayland = facts.wayland.connected;
    const bool xwayland =
        x11 && wayland &&
        (facts.x11.xwaylandExtension || facts.environment.xdgSessionType == L"wayland");
    if (xwayland) {
        report.kind = SessionKind::kXWayland;
        report.platform = PlatformKind::kLinuxWayland;
    } else if (wayland) {
        report.kind = SessionKind::kWayland;
        report.platform = PlatformKind::kLinuxWayland;
    } else if (x11) {
        report.kind = SessionKind::kX11;
        report.platform = PlatformKind::kLinuxX11;
    } else if (facts.x11.compiled || facts.wayland.compiled || facts.portal.compiled ||
               facts.pipewire.compiled) {
        // 探测跑过了，没有任何显示服务器连得上：headless。DISPLAY 设了也一样。
        report.kind = SessionKind::kHeadless;
        report.platform = PlatformKind::kUnknown;
    } else {
        report.kind = SessionKind::kUnknown;
        report.platform = PlatformKind::kUnknown;
    }

    // ---- X11 ----
    {
        RuntimeCapabilityReport r;
        r.name = L"x11";
        r.compiled = facts.x11.compiled;
        r.implemented = session_backend::kX11CaptureImplemented ? Tri::kYes : Tri::kNo;
        r.interactionRequired = Tri::kNo;   // X11 没有每次调用的系统截屏授权
        r.minimumVersion = L"11.0";
        if (facts.x11.compiled) {
            r.reachable = facts.x11.attempted ? (facts.x11.connected ? Tri::kYes : Tri::kNo)
                                              : Tri::kNo;
            if (facts.x11.connected) r.observedVersion = VersionText(11, 0);   // 协议主版本
            r.versionSatisfied =
                facts.x11.connected
                    ? VersionAtLeast(true, facts.x11.protocolMajor, facts.x11.protocolMinor, 11, 0)
                    : Tri::kUnknown;
            if (!facts.x11.attempted) r.reason = W(session_reason::kDisplayNotConfigured);
            else if (!facts.x11.connected) r.reason = W(session_reason::kConnectFailed);
        } else {
            r.reachable = Tri::kUnknown;
            r.versionSatisfied = Tri::kUnknown;
            r.reason = W(session_reason::kLibNotCompiled);
        }
        Finalize(&r);
        report.runtime.push_back(std::move(r));
    }

    // ---- Wayland ----
    {
        RuntimeCapabilityReport r;
        r.name = L"wayland";
        r.compiled = facts.wayland.compiled;
        r.implemented = session_backend::kWaylandCaptureImplemented ? Tri::kYes : Tri::kNo;
        r.interactionRequired = Tri::kNo;
        if (facts.wayland.compiled) {
            r.reachable = facts.wayland.attempted
                              ? (facts.wayland.connected ? Tri::kYes : Tri::kNo)
                              : Tri::kNo;
            r.versionSatisfied = facts.wayland.connected
                                     ? (facts.wayland.registryRoundTripOk ? Tri::kYes : Tri::kNo)
                                     : Tri::kUnknown;
            if (!facts.wayland.attempted) r.reason = W(session_reason::kDisplayNotConfigured);
            else if (!facts.wayland.connected) r.reason = W(session_reason::kConnectFailed);
            else if (!facts.wayland.registryRoundTripOk)
                r.reason = W(session_reason::kProtocolError);
        } else {
            r.reachable = Tri::kUnknown;
            r.versionSatisfied = Tri::kUnknown;
            r.reason = W(session_reason::kLibNotCompiled);
        }
        Finalize(&r);
        report.runtime.push_back(std::move(r));
    }

    // ---- XDG Desktop Portal（D-Bus）----
    {
        RuntimeCapabilityReport r;
        r.name = L"xdg-portal";
        r.compiled = facts.portal.compiled;
        r.implemented = session_backend::kPortalCaptureImplemented ? Tri::kYes : Tri::kNo;
        r.minimumVersion = L"1";
        if (facts.portal.compiled) {
            const bool bus = facts.portal.busConnected;
            const bool service = bus && facts.portal.serviceOwner;
            const bool ifaceKnown = facts.portal.screenCastInterfaceKnown;
            const bool iface = facts.portal.screenCastInterface;
            r.reachable = facts.portal.attempted
                              ? ((service && (!ifaceKnown || iface)) ? Tri::kYes : Tri::kNo)
                              : Tri::kNo;
            if (service && ifaceKnown && !iface) {
                r.versionSatisfied = Tri::kNo;   // 接口不存在，版本无从谈起
            } else if (service && iface) {
                r.versionSatisfied = VersionAtLeast(facts.portal.screenCastVersionKnown,
                                                    facts.portal.screenCastVersion, 0, 1, 0);
            } else {
                r.versionSatisfied = Tri::kUnknown;
            }
            if (facts.portal.screenCastVersionKnown) {
                r.observedVersion = std::to_wstring(facts.portal.screenCastVersion);
            }
            // 来源选择与系统授权永远要人；界面本身不在这份查询里出现。
            r.interactionRequired = service ? Tri::kYes : Tri::kUnknown;
            if (!facts.portal.attempted) r.reason = W(session_reason::kServiceNotFound);
            else if (!bus) r.reason = W(session_reason::kConnectFailed);
            else if (!facts.portal.serviceOwner) r.reason = W(session_reason::kServiceNotFound);
            else if (ifaceKnown && !iface) r.reason = W(session_reason::kInterfaceMissing);
            else if (!facts.portal.screenCastVersionKnown)
                r.reason = W(session_reason::kVersionUnverified);
        } else {
            r.reachable = Tri::kUnknown;
            r.versionSatisfied = Tri::kUnknown;
            r.reason = W(session_reason::kLibNotCompiled);
        }
        Finalize(&r);
        report.runtime.push_back(std::move(r));
    }

    // ---- PipeWire 接入前提 ----
    {
        RuntimeCapabilityReport r;
        r.name = L"pipewire";
        r.compiled = facts.pipewire.compiled;
        r.implemented = session_backend::kPipeWireCaptureImplemented ? Tri::kYes : Tri::kNo;
        r.interactionRequired = Tri::kNo;
        if (facts.pipewire.compiled) {
            const bool known = facts.pipewire.socketPresentKnown;
            if (!facts.pipewire.runtimeDirConfigured ||
                (known && !facts.pipewire.socketPresent)) {
                r.reachable = Tri::kNo;   // 前提不成立 = 确定连不上
            } else if (!known) {
                r.reachable = Tri::kUnknown;
            } else {
                r.reachable = Tri::kYes;
            }
            r.versionSatisfied = Tri::kUnknown;   // 没连协议就没有版本可谈
            if (!facts.pipewire.runtimeDirConfigured ||
                (known && !facts.pipewire.socketPresent)) {
                r.reason = W(session_reason::kPrerequisiteMissing);
            }
        } else {
            r.reachable = Tri::kUnknown;
            r.versionSatisfied = Tri::kUnknown;
            r.reason = W(session_reason::kLibNotCompiled);
        }
        Finalize(&r);
        report.runtime.push_back(std::move(r));
    }

    // ---- Portal 来源类型：只读 AvailableSourceTypes，不选来源 ----
    if (facts.portal.sourceTypesKnown) {
        const struct {
            const wchar_t* name;
            uint32_t bit;
        } kSources[] = {{L"monitor", 1u}, {L"window", 2u}, {L"virtual", 4u}};
        for (const auto& s : kSources) {
            PortalSourceReport source;
            source.name = s.name;
            const bool offered = (facts.portal.sourceTypesMask & s.bit) != 0;
            source.available = offered ? Tri::kYes : Tri::kNo;
            source.interactionRequired = true;
            source.reason = offered ? W(session_reason::kInteractionRequired)
                                    : W(session_reason::kSourceTypeNotOffered);
            report.portalSources.push_back(std::move(source));
        }
    } else {
        const wchar_t* kSources[] = {L"monitor", L"window", L"virtual"};
        for (const wchar_t* name : kSources) {
            PortalSourceReport source;
            source.name = name;
            source.available = Tri::kUnknown;
            source.interactionRequired = true;
            source.reason = W(session_reason::kSourceTypesUnavailable);
            report.portalSources.push_back(std::move(source));
        }
    }

    // ---- 诊断：每一条不可用的能力一条，另加会话层面的边界说明 ----
    for (const auto& r : report.runtime) {
        if (r.status == RuntimeStatus::kAvailable) continue;
        const std::wstring code = L"session." + r.name + L"." + r.reason;
        AddDiagnostic(&report, code.c_str(), r.reason.c_str(), HintFor(r.reason),
                      r.status == RuntimeStatus::kUnverified ? SessionSeverity::kInfo
                                                             : SessionSeverity::kActionable);
    }
    if (report.kind == SessionKind::kHeadless) {
        AddDiagnostic(&report, L"session.headless",
                      L"no reachable display server",
                      L"start a graphical session; DISPLAY or WAYLAND_DISPLAY alone is not a server",
                      SessionSeverity::kActionable);
    }
    if (report.kind == SessionKind::kXWayland) {
        AddDiagnostic(&report, L"session.xwayland.scope",
                      L"x11_windows_only",
                      L"XWayland covers X11 windows only; native Wayland windows and their authorization are not a substitute",
                      SessionSeverity::kInfo);
    }
    if (report.kind == SessionKind::kWayland || report.kind == SessionKind::kXWayland) {
        AddDiagnostic(&report, L"session.wayland.no_global_window_list",
                      L"compositor_does_not_expose_window_list",
                      L"do not fake a window list; use the portal's interactive source selection",
                      SessionSeverity::kInfo);
    }
    if (report.ssh) {
        AddDiagnostic(&report, L"session.ssh.remote",
                      L"ssh_connection_present",
                      L"SSH does not imply headless; a forwarded or local display may still be reachable",
                      SessionSeverity::kInfo);
    }
    if (facts.portal.compiled && facts.portal.screenCastInterface &&
        !facts.portal.sourceTypesKnown) {
        AddDiagnostic(&report, L"session.portal.source_types_unavailable",
                      L"AvailableSourceTypes was not readable",
                      L"do not guess which source types exist; the portal version may be too old or the property absent",
                      SessionSeverity::kInfo);
    }
    if (facts.environment.desktopNameSet) {
        AddDiagnostic(&report, L"session.desktop_name.not_evidence",
                      L"desktop_name_was_read_but_not_used",
                      L"a desktop name is not a service probe and not authorization",
                      SessionSeverity::kInfo);
    }
    if (!facts.sourceSelectionRequested) {
        AddDiagnostic(&report, L"session.portal.source_selection_not_requested",
                      L"read_only_query",
                      L"this query never opens the portal source picker",
                      SessionSeverity::kInfo);
    }
    if (!facts.captureSessionCreated) {
        AddDiagnostic(&report, L"session.portal.capture_session_not_created",
                      L"read_only_query",
                      L"this query never creates a ScreenCast session",
                      SessionSeverity::kInfo);
    }
    if (!facts.screenPixelsRead) {
        AddDiagnostic(&report, L"session.pixels.not_read",
                      L"no_screen_pixels_read",
                      L"this query reads no pixel; a frame is only obtained by a real capture",
                      SessionSeverity::kInfo);
    }
    if (!report.authorizationGranted) {
        AddDiagnostic(&report, L"session.authorization.not_granted",
                      L"authorization_not_granted_by_query",
                      L"a capability report never authorizes a capture; ask the person",
                      SessionSeverity::kInfo);
    }

    // ---- caveats ----
    report.caveats.push_back(W(session_caveat::kAvailableIsNotAGuarantee));
    report.caveats.push_back(W(session_caveat::kNoCapture));
    report.caveats.push_back(W(session_caveat::kNoDialog));
    report.caveats.push_back(W(session_caveat::kNoSourceSelection));
    report.caveats.push_back(W(session_caveat::kNoPortalSession));
    report.caveats.push_back(W(session_caveat::kNoPixels));
    report.caveats.push_back(W(session_caveat::kNoAuthorization));
    report.caveats.push_back(W(session_caveat::kEnvironmentNotEvidence));
    report.caveats.push_back(W(session_caveat::kDesktopNameNotEvidence));
    report.caveats.push_back(W(session_caveat::kXwaylandScope));
    report.caveats.push_back(W(session_caveat::kXwaylandNotNative));
    report.caveats.push_back(W(session_caveat::kWaylandNoWindowList));
    report.caveats.push_back(W(session_caveat::kSshNotHeadless));
    report.caveats.push_back(W(session_caveat::kUnknownIsNotNo));
    report.caveats.push_back(W(session_caveat::kPipewirePrereqNotCapture));
    // 本构建的 X11 后端只交付窗口自己的存储：这一句是"这条 available 不覆盖什么"的边界，
    // 与 --capabilities 里 backends[].paths 那一段（xcomposite 的 scope=window）同源。
    if (session_backend::kX11CaptureImplemented) {
        report.caveats.push_back(W(session_caveat::kX11WindowStorageOnly));
    }

    return report;
}

void WriteSessionObject(Json& j, const SessionReport& report, bool verbose) {
    j.Key(L"session").Obj();
    j.Key(L"kind").Value(W(SessionKindName(report.kind)));
    j.Key(L"platform").Value(W(PlatformName(report.platform)));
    WriteTri(j, L"remoteSession", report.remoteSession);
    j.Key(L"ssh").Value(report.ssh);

    j.Key(L"environment").Obj();
    j.Key(L"xdgSessionType").Value(report.environment.xdgSessionType);
    j.Key(L"xdgSessionTypeSet").Value(report.environment.xdgSessionTypeSet);
    j.Key(L"displaySet").Value(report.environment.displaySet);
    j.Key(L"waylandDisplaySet").Value(report.environment.waylandDisplaySet);
    j.Key(L"sshConnectionSet").Value(report.environment.sshConnectionSet);
    j.Key(L"sshTtySet").Value(report.environment.sshTtySet);
    j.Key(L"runtimeDirSet").Value(report.environment.runtimeDirSet);
    j.Key(L"dbusAddressSet").Value(report.environment.dbusAddressSet);
    j.Key(L"desktopNameSet").Value(report.environment.desktopNameSet);
    j.Key(L"desktopNameIsEvidence").Value(false);
    j.Key(L"environmentValuesNotEchoed").Value(true);
    j.End();

    j.Key(L"x11").Obj();
    j.Key(L"compiled").Value(report.x11.compiled);
    j.Key(L"configured").Value(report.x11.displayConfigured);
    j.Key(L"attempted").Value(report.x11.attempted);
    WriteTri(j, L"reachable", report.x11.attempted
                                   ? (report.x11.connected ? Tri::kYes : Tri::kNo)
                                   : (report.x11.compiled ? Tri::kNo : Tri::kUnknown));
    if (report.x11.connected) {
        j.Key(L"protocolVersion").Value(VersionText(report.x11.protocolMajor,
                                                    report.x11.protocolMinor));
    }
    j.Key(L"screens").Value(static_cast<long long>(report.x11.screenCount));
    j.Key(L"xwaylandExtension").Value(report.x11.xwaylandExtension);
    if (verbose) WriteStringArray(j, L"extensions", report.x11.extensions);
    j.End();

    j.Key(L"wayland").Obj();
    j.Key(L"compiled").Value(report.wayland.compiled);
    j.Key(L"configured").Value(report.wayland.displayConfigured);
    j.Key(L"attempted").Value(report.wayland.attempted);
    WriteTri(j, L"reachable", report.wayland.attempted
                                   ? (report.wayland.connected ? Tri::kYes : Tri::kNo)
                                   : (report.wayland.compiled ? Tri::kNo : Tri::kUnknown));
    WriteTri(j, L"registryRoundTrip",
             report.wayland.connected
                 ? (report.wayland.registryRoundTripOk ? Tri::kYes : Tri::kNo)
                 : Tri::kUnknown);
    j.Key(L"outputs").Value(static_cast<long long>(report.wayland.outputCount));
    j.Key(L"compositor").Value(report.wayland.hasCompositor);
    j.Key(L"shm").Value(report.wayland.hasShm);
    j.Key(L"screencopy").Value(report.wayland.hasScreencopy);
    j.Key(L"imageCopyCapture").Value(report.wayland.hasImageCopyCapture);
    if (verbose) WriteStringArray(j, L"interfaces", report.wayland.interfaces);
    j.End();

    j.Key(L"portal").Obj();
    j.Key(L"compiled").Value(report.portal.compiled);
    j.Key(L"busAddressConfigured").Value(report.portal.addressConfigured);
    j.Key(L"attempted").Value(report.portal.attempted);
    WriteTri(j, L"busReachable", report.portal.attempted
                                      ? (report.portal.busConnected ? Tri::kYes : Tri::kNo)
                                      : Tri::kUnknown);
    WriteTri(j, L"serviceOwner", report.portal.serviceOwnerKnown
                                      ? (report.portal.serviceOwner ? Tri::kYes : Tri::kNo)
                                      : Tri::kUnknown);
    j.Key(L"screencast").Obj();
    WriteTri(j, L"present", report.portal.screenCastInterfaceKnown
                                 ? (report.portal.screenCastInterface ? Tri::kYes : Tri::kNo)
                                 : Tri::kUnknown);
    if (report.portal.screenCastVersionKnown) {
        j.Key(L"version").Value(static_cast<long long>(report.portal.screenCastVersion));
    } else {
        j.Key(L"version").Value(L"unknown");
    }
    j.End();
    j.Key(L"screenshot").Obj();
    WriteTri(j, L"present", report.portal.screenshotInterfaceKnown
                                 ? (report.portal.screenshotInterface ? Tri::kYes : Tri::kNo)
                                 : Tri::kUnknown);
    if (report.portal.screenshotVersionKnown) {
        j.Key(L"version").Value(static_cast<long long>(report.portal.screenshotVersion));
    } else {
        j.Key(L"version").Value(L"unknown");
    }
    j.End();
    j.Key(L"sourceTypes").Obj();
    j.Key(L"known").Value(report.portal.sourceTypesKnown);
    if (report.portal.sourceTypesKnown) {
        j.Key(L"mask").Value(static_cast<long long>(report.portal.sourceTypesMask));
        for (const auto& s : report.portalSources) {
            WriteTri(j, s.name.c_str(), s.available);
        }
    } else {
        j.Key(L"mask").Value(L"unknown");
        WriteTri(j, L"monitor", Tri::kUnknown);
        WriteTri(j, L"window", Tri::kUnknown);
        WriteTri(j, L"virtual", Tri::kUnknown);
    }
    j.End();
    j.Key(L"cursorModes").Obj();
    j.Key(L"known").Value(report.portal.cursorModesKnown);
    if (report.portal.cursorModesKnown) {
        j.Key(L"mask").Value(static_cast<long long>(report.portal.cursorModesMask));
    } else {
        j.Key(L"mask").Value(L"unknown");
    }
    j.End();
    j.Key(L"sourceSelectionRequested").Value(false);
    j.Key(L"captureSessionCreated").Value(false);
    j.End();

    j.Key(L"pipewire").Obj();
    j.Key(L"compiled").Value(report.pipewire.compiled);
    j.Key(L"runtimeDirConfigured").Value(report.pipewire.runtimeDirConfigured);
    WriteTri(j, L"socketPresent", report.pipewire.socketPresentKnown
                                       ? (report.pipewire.socketPresent ? Tri::kYes : Tri::kNo)
                                       : Tri::kUnknown);
    j.End();

    j.Key(L"xwayland").Obj();
    j.Key(L"covers").Value(L"x11-windows-only");
    j.Key(L"nativeWaylandWindowQuery").Value(L"not_supported");
    j.Key(L"authorizationSubstitute").Value(false);
    j.End();

    j.Key(L"authorization").Value(L"not_requested");
    j.Key(L"consentDialogExpected").Value(L"unknown");
    j.Key(L"pixelsRead").Value(false);
    j.End();

    j.Key(L"runtime").Arr();
    for (const auto& r : report.runtime) {
        j.Obj();
        j.Key(L"name").Value(r.name);
        j.Key(L"compiled").Value(r.compiled);
        WriteTri(j, L"reachable", r.reachable);
        WriteTri(j, L"versionSatisfied", r.versionSatisfied);
        WriteTri(j, L"interactionRequired", r.interactionRequired);
        WriteTri(j, L"implemented", r.implemented);
        j.Key(L"status").Value(W(RuntimeStatusName(r.status)));
        j.Key(L"reason").Value(r.reason);
        if (!r.minimumVersion.empty()) j.Key(L"minimumVersion").Value(r.minimumVersion);
        if (!r.observedVersion.empty()) j.Key(L"observedVersion").Value(r.observedVersion);
        j.Key(L"canCapture").Value(r.canCapture);
        j.End();
    }
    j.End();

    j.Key(L"diagnostics").Arr();
    for (const auto& d : report.diagnostics) {
        j.Obj();
        j.Key(L"code").Value(d.code);
        j.Key(L"detail").Value(d.detail);
        j.Key(L"hint").Value(d.hint);
        j.Key(L"severity").Value(d.severity == SessionSeverity::kActionable
                                     ? L"actionable"
                                     : L"info");
        j.End();
    }
    j.End();
}

std::wstring RenderSessionJson(const SessionReport& report, bool verbose) {
    Json j;
    j.Obj();
    WriteSessionObject(j, report, verbose);
    WriteStringArray(j, L"caveats", report.caveats);
    j.End();
    return j.Str();
}

}  // namespace ecapture
