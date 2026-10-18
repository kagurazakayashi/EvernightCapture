// 会话识别与运行能力查询的离线判据 + 一次真实只读探测。
//
// 为什么这一份要能把事实逐项注入：本机只有一个会话（X11 + XWayland），没有第二块屏、
// 没有 Portal 版本过低的服务、也没有"没有会话总线"的机器。判据本体是纯函数
// （SessionReport.cpp 的 BuildSessionReport），注入一份假的 SessionFacts 就能把
// X11 / Wayland / XWayland / SSH / headless / 缺库 / 服务不存在 / 版本过低 / 需要交互 /
// 后端未实现这些分支逐条走完；最后一节再对**当前机器**做一次真实只读探测。
//
// 真实探测那一节只判三件事：不崩、没有一条 canCapture=true（本构建没有 Linux 采集后端）、
// 报告里没有回显任何环境变量原值。它不弹框、不选来源、不建会话、不读像素。
//
// 用法：
//   ecapture-session-tests          跑全部判据，全部通过返回 0
//   ecapture-session-tests --json   只做一次真实探测并打印 JSON（给 AI / 人工核对）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "SessionProbe.h"
#include "core/SessionCapability.h"
#include "core/TextCoding.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

std::wstring W(const char* text) {
    std::wstring out;
    if (!text) return out;
    while (*text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    return out;
}

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

bool Has(const std::vector<std::wstring>& items, const wchar_t* needle) {
    for (const auto& s : items) {
        if (s == needle) return true;
    }
    return false;
}

bool HasDiag(const SessionReport& r, const wchar_t* code) {
    for (const auto& d : r.diagnostics) {
        if (d.code == code) return true;
    }
    return false;
}

SessionFacts X11Only() {
    SessionFacts f;
    f.environment.displaySet = true;
    f.x11.compiled = true;
    f.x11.displayConfigured = true;
    f.x11.attempted = true;
    f.x11.connected = true;
    f.x11.protocolMajor = 11;
    f.x11.protocolMinor = 0;
    f.x11.screenCount = 1;
    f.x11.extensions = {L"RANDR", L"SHM"};
    return f;
}

SessionFacts XWaylandSession() {
    SessionFacts f;
    f.environment.xdgSessionTypeSet = true;
    f.environment.xdgSessionType = L"wayland";
    f.environment.displaySet = true;
    f.environment.waylandDisplaySet = true;
    f.x11.compiled = true;
    f.x11.displayConfigured = true;
    f.x11.attempted = true;
    f.x11.connected = true;
    f.x11.protocolMajor = 11;
    f.x11.protocolMinor = 0;
    f.x11.screenCount = 1;
    f.x11.extensions = {L"XWAYLAND", L"RANDR"};
    f.x11.xwaylandExtension = true;
    f.wayland.compiled = true;
    f.wayland.displayConfigured = true;
    f.wayland.attempted = true;
    f.wayland.connected = true;
    f.wayland.registryRoundTripOk = true;
    f.wayland.outputCount = 1;
    f.wayland.interfaces = {L"wl_compositor", L"wl_shm", L"wl_output"};
    f.wayland.hasCompositor = true;
    f.wayland.hasShm = true;
    return f;
}

void TestMachineWords() {
    Section("1) 机器词稳定：会话种类 / 运行状态 / 原因");
    Check(W(SessionKindName(SessionKind::kX11)) == L"x11", "kind x11");
    Check(W(SessionKindName(SessionKind::kWayland)) == L"wayland", "kind wayland");
    Check(W(SessionKindName(SessionKind::kXWayland)) == L"xwayland", "kind xwayland");
    Check(W(SessionKindName(SessionKind::kHeadless)) == L"headless", "kind headless");
    Check(W(SessionKindName(SessionKind::kUnknown)) == L"unknown", "kind unknown");
    Check(W(RuntimeStatusName(RuntimeStatus::kNotCompiled)) == L"not_compiled",
          "status not_compiled");
    Check(W(RuntimeStatusName(RuntimeStatus::kNotImplemented)) == L"not_implemented",
          "status not_implemented");
    Check(W(RuntimeStatusName(RuntimeStatus::kNeedsInteraction)) ==
              L"needs_interaction",
          "status needs_interaction");
}

void TestX11Only() {
    Section("2) 只有 X11 连上：不是 Wayland，也不是可截图");
    const SessionReport r = BuildSessionReport(X11Only());
    Check(r.kind == SessionKind::kX11, "会话种类是 x11");
    Check(r.platform == PlatformKind::kLinuxX11, "平台族是 linux-x11");
    const auto* x11 = r.RuntimeOf(L"x11");
    Check(x11 && x11->reachable == Tri::kYes, "x11 reachable=yes（真的连上了）");
    Check(x11 && x11->implemented == Tri::kNo, "x11 implemented=no（没有采集后端）");
    Check(x11 && x11->status == RuntimeStatus::kNotImplemented, "x11 status=not_implemented");
    Check(x11 && !x11->canCapture, "x11 不回答可以截图");
    const auto* wayland = r.RuntimeOf(L"wayland");
    Check(wayland && wayland->compiled == false, "wayland 没编译进来");
    Check(wayland && wayland->status == RuntimeStatus::kNotCompiled, "wayland=not_compiled");
    const auto* portal = r.RuntimeOf(L"xdg-portal");
    Check(portal && portal->status == RuntimeStatus::kNotCompiled, "portal=not_compiled");
    Check(!r.authorizationGranted, "只读查询不授权");
    Check(r.nativeWaylandWindowQuerySupported == false, "没有伪造原生 Wayland 窗口查询");
}

void TestXWayland() {
    Section("3) XWayland：X11 只覆盖 X11 窗口，不能当原生 Wayland 的替代");
    const SessionReport r = BuildSessionReport(XWaylandSession());
    Check(r.kind == SessionKind::kXWayland, "会话种类是 xwayland");
    Check(r.platform == PlatformKind::kLinuxWayland, "平台族是 linux-wayland");
    Check(r.xwaylandCoversOnlyX11Windows, "XWayland 只覆盖 X11 窗口");
    Check(r.xwaylandAuthorizationSubstitute == false, "XWayland 不是原生 Wayland 授权的替代");
    Check(r.nativeWaylandWindowQuerySupported == false, "没有全局窗口列表");
    Check(Has(r.caveats, L"xwayland_covers_x11_windows_only"), "有 XWayland 作用域 caveat");
    Check(Has(r.caveats, L"xwayland_is_not_native_wayland"), "有 XWayland 非原生 caveat");
    Check(HasDiag(r, L"session.xwayland.scope"), "诊断里写明 XWayland 作用域");
}

void TestWaylandPortal() {
    Section("4) 原生 Wayland + Portal：版本/来源类型分开报，仍不可截图");
    SessionFacts f = XWaylandSession();
    f.x11 = X11Facts{};   // 只有 Wayland
    f.environment.displaySet = false;
    f.environment.xdgSessionType = L"wayland";
    f.portal.compiled = true;
    f.portal.addressConfigured = true;
    f.portal.attempted = true;
    f.portal.busConnected = true;
    f.portal.serviceOwnerKnown = true;
    f.portal.serviceOwner = true;
    f.portal.screenCastInterfaceKnown = true;
    f.portal.screenCastInterface = true;
    f.portal.screenCastVersionKnown = true;
    f.portal.screenCastVersion = 5;
    f.portal.screenshotInterfaceKnown = true;
    f.portal.screenshotInterface = true;
    f.portal.screenshotVersionKnown = true;
    f.portal.screenshotVersion = 2;
    f.portal.sourceTypesKnown = true;
    f.portal.sourceTypesMask = 3;   // monitor + window
    const SessionReport r = BuildSessionReport(f);
    Check(r.kind == SessionKind::kWayland, "会话种类是 wayland");
    Check(r.portalSources.size() == 3, "三种来源类型都报出来");
    Check(r.portalSources[0].available == Tri::kYes, "monitor 来源可用");
    Check(r.portalSources[1].available == Tri::kYes, "window 来源可用");
    Check(r.portalSources[2].available == Tri::kNo, "virtual 来源不可用");
    for (const auto& s : r.portalSources) {
        Check(s.interactionRequired, "每种来源选择都需要人");
    }
    const auto* portal = r.RuntimeOf(L"xdg-portal");
    Check(portal && portal->reachable == Tri::kYes, "portal reachable=yes");
    Check(portal && portal->versionSatisfied == Tri::kYes, "portal 版本满足");
    Check(portal && portal->interactionRequired == Tri::kYes, "portal 需要用户交互");
    Check(portal && portal->implemented == Tri::kNo, "portal 未实现");
    Check(portal && portal->status == RuntimeStatus::kNotImplemented, "portal=not_implemented");
    Check(portal && !portal->canCapture, "portal 不回答可以截图");
    Check(Has(r.caveats, L"no_portal_source_selection_performed"), "没有触发来源选择");
    Check(Has(r.caveats, L"no_portal_capture_session_created"), "没有创建捕获会话");
}

void TestHeadlessAndSsh() {
    Section("5) 无桌面 / SSH：环境变量不是服务可用或已授权的证据");
    SessionFacts f;
    f.environment.displaySet = true;          // 设了 DISPLAY
    f.environment.waylandDisplaySet = false;
    f.environment.sshConnectionSet = true;    // 也在 SSH 里
    f.x11.compiled = true;
    f.x11.displayConfigured = true;
    f.x11.attempted = true;
    f.x11.connected = false;
    f.x11.connectError = 11;
    f.wayland.compiled = true;
    f.wayland.displayConfigured = false;
    const SessionReport r = BuildSessionReport(f);
    Check(r.kind == SessionKind::kHeadless, "DISPLAY 设了但连不上 = headless");
    Check(r.remoteSession == Tri::kYes, "SSH 会话如实报 remote=yes");
    Check(r.ssh, "ssh 标记为真");
    const auto* x11 = r.RuntimeOf(L"x11");
    Check(x11 && x11->reachable == Tri::kNo, "x11 reachable=no");
    Check(x11 && x11->status == RuntimeStatus::kUnavailable, "x11 unavailable");
    Check(x11 && x11->reason == L"connect_failed", "原因是 connect_failed");
    Check(HasDiag(r, L"session.headless"), "有无桌面诊断");
    Check(HasDiag(r, L"session.ssh.remote"), "有 SSH 诊断");
    Check(Has(r.caveats, L"ssh_session_does_not_imply_headless"), "SSH 不等于无桌面");
    Check(Has(r.caveats, L"environment_variables_are_not_evidence"), "环境变量不是证据");
}

void TestNotCompiled() {
    Section("6) 缺库：compiled=no 与 unknown 分开，不折成 no");
    SessionFacts f;
    f.environment.displaySet = true;   // 只有环境线索
    const SessionReport r = BuildSessionReport(f);
    Check(r.kind == SessionKind::kUnknown, "探测没编译进来时会话种类 unknown");
    for (const auto& runtime : r.runtime) {
        Check(!runtime.compiled, "每一条都报未编译");
        Check(runtime.reachable == Tri::kUnknown, "未编译时 reachable=unknown");
        Check(runtime.status == RuntimeStatus::kNotCompiled, "status=not_compiled");
        Check(!runtime.canCapture, "未编译不回答可以截图");
    }
    Check(Has(r.caveats, L"unknown_is_not_no"), "unknown 不折成 no");
}

void TestPortalFailures() {
    Section("7) Portal：服务不存在 / 版本过低 / 来源类型未知");
    {
        SessionFacts f = XWaylandSession();
        f.x11 = X11Facts{};
        f.portal.compiled = true;
        f.portal.attempted = true;
        f.portal.busConnected = true;
        f.portal.serviceOwnerKnown = true;
        f.portal.serviceOwner = false;
        const SessionReport r = BuildSessionReport(f);
        const auto* p = r.RuntimeOf(L"xdg-portal");
        Check(p && p->status == RuntimeStatus::kUnavailable, "服务不存在 = unavailable");
        Check(p && p->reason == L"service_not_found", "原因是 service_not_found");
        Check(p && p->versionSatisfied == Tri::kUnknown, "没连上接口时版本 unknown");
        Check(HasDiag(r, L"session.xdg-portal.service_not_found"), "有服务不存在诊断");
    }
    {
        SessionFacts f = XWaylandSession();
        f.x11 = X11Facts{};
        f.portal.compiled = true;
        f.portal.attempted = true;
        f.portal.busConnected = true;
        f.portal.serviceOwnerKnown = true;
        f.portal.serviceOwner = true;
        f.portal.screenCastInterfaceKnown = true;
        f.portal.screenCastInterface = true;
        f.portal.screenCastVersionKnown = true;
        f.portal.screenCastVersion = 0;
        const SessionReport r = BuildSessionReport(f);
        const auto* p = r.RuntimeOf(L"xdg-portal");
        Check(p && p->versionSatisfied == Tri::kNo, "版本 0 低于下限");
        Check(p && p->status == RuntimeStatus::kUnavailable, "版本过低 = unavailable");
        Check(p && p->reason == L"version_too_low", "原因是 version_too_low");
    }
    {
        SessionFacts f = XWaylandSession();
        f.x11 = X11Facts{};
        f.portal.compiled = true;
        f.portal.attempted = true;
        f.portal.busConnected = true;
        f.portal.serviceOwnerKnown = true;
        f.portal.serviceOwner = true;
        f.portal.screenCastInterfaceKnown = true;
        f.portal.screenCastInterface = true;
        f.portal.screenCastVersionKnown = true;
        f.portal.screenCastVersion = 5;
        f.portal.sourceTypesKnown = false;
        const SessionReport r = BuildSessionReport(f);
        for (const auto& s : r.portalSources) {
            Check(s.available == Tri::kUnknown, "来源类型问不出来时保持 unknown");
        }
        Check(HasDiag(r, L"session.portal.source_types_unavailable") ||
                  HasDiag(r, L"session.xdg-portal.version_unverified"),
              "来源类型未知有诊断");
    }
}

void TestPipeWirePrerequisite() {
    Section("8) PipeWire：套接字是接入前提，不是采集能力");
    SessionFacts f;
    f.pipewire.compiled = true;
    f.pipewire.runtimeDirConfigured = true;
    f.pipewire.socketPresentKnown = true;
    f.pipewire.socketPresent = false;
    const SessionReport r = BuildSessionReport(f);
    const auto* p = r.RuntimeOf(L"pipewire");
    Check(p && p->reachable == Tri::kNo, "套接字不存在 = reachable=no");
    Check(p && p->status == RuntimeStatus::kUnavailable, "pipewire unavailable");
    Check(p && p->reason == L"prerequisite_missing", "原因是 prerequisite_missing");
    Check(Has(r.caveats, L"pipewire_prerequisite_is_not_capture"), "前提不等于采集");
}

void TestInvariants(const SessionReport& r, const char* label) {
    Section(label);
    for (const auto& runtime : r.runtime) {
        Check(!runtime.canCapture, "任何一条运行能力都不回答可以截图");
        if (runtime.status == RuntimeStatus::kAvailable) {
            Check(false, "本构建没有 Linux 采集后端，不应出现 available");
        }
    }
    Check(!r.authorizationGranted, "报告从不授权");
    Check(!r.xwaylandAuthorizationSubstitute, "XWayland 从不作为授权替代");
    Check(Has(r.caveats, L"no_capture_performed"), "没有取帧");
    Check(Has(r.caveats, L"no_consent_dialog_shown"), "没有弹框");
    Check(Has(r.caveats, L"no_screen_pixels_read"), "没有读像素");
}

void TestRealProbe(bool printJson) {
    const SessionFacts facts = ProbeSessionFacts();
    const SessionReport report = BuildSessionReport(facts);
    const std::wstring json = RenderSessionJson(report, true);
    const std::string utf8 = EncodeUtf8(json);
    if (printJson) {
        std::printf("%s\n", utf8.c_str());
        return;
    }
    Section("9) 真实只读探测（当前机器）");
    Check(!json.empty(), "真实探测能渲染出 JSON");
    Check(json.find(L"\"runtime\"") != std::wstring::npos, "JSON 含 runtime 段");
    Check(json.find(L"\"canCapture\": false") != std::wstring::npos,
          "JSON 里没有 canCapture=true");
    Check(json.find(L"\"pixelsRead\": false") != std::wstring::npos, "JSON 明写没有读像素");
    Check(json.find(L"\"sourceSelectionRequested\": false") != std::wstring::npos,
          "JSON 明写没有选来源");
    Check(json.find(L"\"captureSessionCreated\": false") != std::wstring::npos,
          "JSON 明写没有建会话");

    // 隐私：报告不得回显任何环境变量原值。键名可以出现（displaySet 等），值不行。
    const char* names[] = {"DISPLAY", "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "HOME", "USER",
                           "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"};
    for (const char* name : names) {
        const char* raw = std::getenv(name);
        if (!raw || !*raw || std::strlen(raw) < 3) continue;
        const std::string value(raw);
        // 会话类型是归一化后的枚举词（wayland/x11/tty），单独放行；其余原值一个都不许出现。
        if (std::strcmp(name, "XDG_SESSION_TYPE") == 0) continue;
        Check(utf8.find(value) == std::string::npos, "报告没有回显环境变量原值");
    }
    TestInvariants(report, "10) 真实探测的不变量");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--json") == 0) {
        TestRealProbe(true);
        return 0;
    }
    std::printf("ecapture-session-tests：会话识别与运行能力查询判据\n");
    TestMachineWords();
    TestX11Only();
    TestXWayland();
    TestWaylandPortal();
    TestHeadlessAndSsh();
    TestNotCompiled();
    TestPortalFailures();
    TestPipeWirePrerequisite();
    TestRealProbe(false);
    std::printf("\n%d 条判据，%d 条失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
