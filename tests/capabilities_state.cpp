// 能力与诊断查询的离线判据（由 tests\capabilities.ps1 运行 build\ecapture-capabilities-tests.exe）。
//
// 为什么单独一个可执行文件：这一批判据要的是"这台机器不是那一台开发机"的现场——
// 一块屏都没有的会话、版本正好低于某条通道的下限、版本根本问不出来、某个编码器没登记。
// 本机降不了级，也没有第二块显卡，更不能为了看"没有屏幕拓扑会怎么判"而去拔显示器。
// 判据本体（src/EnvReport.cpp 的 BuildEnvReport）是纯函数，注入一份假的 EnvProbe 就能
// 逐条判；真机那层（tests\capabilities.ps1 的第二层）判的是另一件事：这一次查询确实
// 一个像素都没取、一个框都没弹、一个文件都没写，而且四语与 --lang 无关。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "../src/CaptureCommon.h"
#include "../src/CaptureScope.h"
#include "../src/Deadline.h"
#include "../src/EnvReport.h"
#include "../src/CliOptions.h"
#include "../src/Lang.h"
#include "../src/SystemCompat.h"
#include "../src/WgcGeometry.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

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

std::string Narrow(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out.push_back(static_cast<char>(c > 0 && c < 128 ? c : '?'));
    return out;
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// 一台"什么都问得出来、而且样样齐"的机器：开发机那一台的样子（内部版本 19045、x64、
// 接在控制台上、有一块屏）。每条用例只改自己那一问，其余照这份，失败时才知道是谁动的。
EnvProbe HealthyProbe() {
    EnvProbe p;
    p.os.major = 10;
    p.os.minor = 0;
    p.os.build = 19045;
    p.os.known = true;
    p.arch = L"x64";
    p.consoleAttached = Tri::kYes;
    p.processSessionKnown = true;
    p.processSessionId = 1;
    p.consoleSessionKnown = true;
    p.consoleSessionId = 1;
    p.remoteSession = Tri::kNo;
    p.elevated = Tri::kNo;
    p.displayTopology = Tri::kYes;
    p.monitorCountKnown = true;
    p.monitorCount = 1;
    p.buildIdKnown = true;
    p.buildId = L"0.4.0-x64-665f1a2b";
    p.linkTimestampKnown = true;
    p.linkTimestamp = 0x665f1a2b;
    p.peMachineKnown = true;
    p.peMachine = 0x8664;
    p.imageSizeKnown = true;
    p.imageSize = 1081344;
    p.subsystemKnown = true;
    p.subsystem = 3;   // IMAGE_SUBSYSTEM_WINDOWS_CUI
    p.subsystemVersionKnown = true;
    p.subsystemMajor = 6;
    p.subsystemMinor = 0;
    p.linkerVersion = L"14.51";
    return p;
}

// 把版本号改成 X，其余照健康那份。
EnvProbe ProbeWithBuild(uint32_t build) {
    EnvProbe p = HealthyProbe();
    p.os.build = build;
    return p;
}

const BackendReport* Find(EnvReport& r, CaptureMethod method) { return r.BackendOf(method); }

bool HasCaveat(const EnvReport& r, const wchar_t* token) {
    for (const auto& c : r.caveats) {
        if (c == token) return true;
    }
    return false;
}

const ConsentPathReport* FindPath(const std::vector<ConsentPathReport>& paths, const wchar_t* name) {
    for (const auto& p : paths) {
        if (p.path == name) return &p;
    }
    return nullptr;
}

const FormatReport* FindFormat(EnvReport& r, const wchar_t* name) {
    for (const auto& f : r.formats) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 1) 机器名本身：这三组词是契约的一部分，改名等于破坏调用方
// ---------------------------------------------------------------------------
void TestMachineWords() {
    Section("1) 状态词的机器名稳定");
    Check(std::wstring(TriName(Tri::kYes)) == L"yes", "Tri yes");
    Check(std::wstring(TriName(Tri::kNo)) == L"no", "Tri no");
    Check(std::wstring(TriName(Tri::kUnknown)) == L"unknown", "Tri unknown");
    Check(std::wstring(CapStatusName(CapStatus::kAvailable)) == L"available", "status available");
    Check(std::wstring(CapStatusName(CapStatus::kUnavailable)) == L"unavailable", "status unavailable");
    Check(std::wstring(CapStatusName(CapStatus::kUnverified)) == L"unverified", "status unverified");
    Check(std::wstring(EnvContractName(EnvQueryKind::kCapabilities)) == L"capabilities",
          "contract name capabilities");
    Check(std::wstring(EnvContractName(EnvQueryKind::kDiagnostics)) == L"diagnostics",
          "contract name diagnostics");
    Check(kEnvContractVersion == 1, "contract version starts at 1");
}

// ---------------------------------------------------------------------------
// 2) 编译支持 vs 本机可用 vs 本项目实测过：三件事必须各占一个字段
// ---------------------------------------------------------------------------
void TestThreeLayers() {
    Section("2) 编译支持、本机可用、实测过是三条");
    EnvReport r = BuildEnvReport(HealthyProbe(), EnvQueryKind::kCapabilities);

    // 五条通道都实现了：compiled 恒真，与本机版本无关
    int compiled = 0;
    for (const auto& b : r.backends) compiled += b.compiled ? 1 : 0;
    Check(r.backends.size() == 5, "five backends listed");
    Check(compiled == 5, "all five backends are compiled in");

    // 健康机器（19045 x64）上：五条都可用，且这一台正是实测过的那一种
    for (const auto& b : r.backends) {
        Check(b.status == CapStatus::kAvailable, ("healthy machine: " + Narrow(b.name) + " available").c_str());
        Check(b.reason == cap_reason::kNone, ("healthy machine: " + Narrow(b.name) + " reason none").c_str());
        Check(b.verifiedOnThisMachine == Tri::kYes,
              ("healthy machine: " + Narrow(b.name) + " verified here").c_str());
    }
    Check(r.matchesVerifiedEnv == Tri::kYes, "19045/x64 matches the recorded tested environment");

    // 换一台版本齐但本项目没实测过的机器：可用照旧，实测过那条必须翻成 no
    EnvReport r2 = BuildEnvReport(ProbeWithBuild(22621), EnvQueryKind::kCapabilities);
    Check(r2.matchesVerifiedEnv == Tri::kNo, "22621 is not the recorded tested environment");
    for (const auto& b : r2.backends) {
        Check(b.status == CapStatus::kAvailable,
              ("22621: " + Narrow(b.name) + " still available by the version floor").c_str());
        Check(b.verifiedOnThisMachine == Tri::kNo,
              ("22621: " + Narrow(b.name) + " not tested by this project").c_str());
    }
    Check(HasCaveat(r2, caveat::kNotTestedHere), "untested environment leaves its own caveat");
}

// ---------------------------------------------------------------------------
// 3) 每一条下限两侧各判一次（数字的出处见 src/SystemCompat.h）
// ---------------------------------------------------------------------------
void TestVersionFloors() {
    Section("3) 各条路线的下限两侧各判一次");
    struct FloorCase {
        CaptureMethod method;
        uint32_t minBuild;
    };
    const FloorCase cases[] = {
        {CaptureMethod::kWgc, os_floor::kWgc},
        {CaptureMethod::kDuplication, os_floor::kDuplication},
        {CaptureMethod::kPrintWindow, os_floor::kPrintWindow},
        {CaptureMethod::kDwmThumbnail, os_floor::kDwmThumbnail},
    };
    for (const auto& c : cases) {
        const std::wstring name = CaptureMethodName(c.method);
        EnvReport below = BuildEnvReport(ProbeWithBuild(c.minBuild - 1), EnvQueryKind::kCapabilities);
        EnvReport at = BuildEnvReport(ProbeWithBuild(c.minBuild), EnvQueryKind::kCapabilities);
        const BackendReport* bb = Find(below, c.method);
        const BackendReport* ab = Find(at, c.method);
        Check(bb != nullptr && ab != nullptr, (Narrow(name) + " present in both reports").c_str());
        if (!bb || !ab) continue;
        Check(bb->status == CapStatus::kUnavailable, (Narrow(name) + " below floor is unavailable").c_str());
        Check(bb->reason == cap_reason::kOsBelowMin, (Narrow(name) + " below floor reason").c_str());
        Check(ab->status == CapStatus::kAvailable, (Narrow(name) + " at floor is available").c_str());
        Check(ab->reason == cap_reason::kNone, (Narrow(name) + " at floor reason none").c_str());
        Check(bb->minBuild == c.minBuild && ab->minBuild == c.minBuild,
              (Narrow(name) + " reports its own floor").c_str());
    }
    // bitblt 自己没有版本门槛：低到 10.0.10240 也仍然可用（但整工具的编码器那一道另算）
    EnvReport early = BuildEnvReport(ProbeWithBuild(os_floor::kEncoder), EnvQueryKind::kCapabilities);
    const BackendReport* bitblt = Find(early, CaptureMethod::kBitBlt);
    Check(bitblt && bitblt->status == CapStatus::kAvailable, "bitblt has no version floor");
    Check(bitblt && bitblt->minBuild == 0, "bitblt reports minBuild 0");
    const BackendReport* wgcEarly = Find(early, CaptureMethod::kWgc);
    Check(wgcEarly && wgcEarly->status == CapStatus::kUnavailable, "10240 does not offer wgc");
}

// ---------------------------------------------------------------------------
// 4) 无图形环境：一块屏都没有的会话里，只走桌面像素的那两条没有可取的地方
// ---------------------------------------------------------------------------
void TestNoGraphicsEnvironment() {
    Section("4) 无图形环境（一块屏都没有）");
    EnvProbe p = HealthyProbe();
    p.displayTopology = Tri::kNo;
    p.monitorCount = 0;
    p.monitorCountKnown = true;
    // 没有交互桌面：会话号也对不上（服务会话就是这一形状）
    p.consoleAttached = Tri::kNo;
    p.processSessionId = 0;
    p.consoleSessionId = 1;
    EnvReport r = BuildEnvReport(p, EnvQueryKind::kCapabilities);

    const BackendReport* bitblt = Find(r, CaptureMethod::kBitBlt);
    const BackendReport* dup = Find(r, CaptureMethod::kDuplication);
    Check(bitblt && bitblt->status == CapStatus::kUnavailable, "bitblt without topology is unavailable");
    Check(bitblt && bitblt->reason == cap_reason::kNoDisplayTopology, "bitblt reason is the topology");
    Check(dup && dup->status == CapStatus::kUnavailable, "duplication without topology is unavailable");
    Check(dup && dup->reason == cap_reason::kNoDisplayTopology, "duplication reason is the topology");

    // 窗口内容那三条不读屏幕，没有屏也照样是实现得好的路线（只是这一次可能截给谁看都成问题）
    for (const CaptureMethod m : {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                  CaptureMethod::kPrintWindow}) {
        const BackendReport* b = Find(r, m);
        Check(b && b->status == CapStatus::kAvailable,
              ("window-content route survives no topology: " + Narrow(b ? b->name : L"?")).c_str());
    }
    // 弹不出框那一面要单独看得见：桌面像素那一级非要人点头不可
    Check(r.consentDialogExpected == Tri::kNo, "dialog expected to be unshowable in a foreign session");
    Check(HasCaveat(r, caveat::kDesktopNeedsDialog), "desktop-needs-dialog caveat present");
    Check(HasCaveat(r, caveat::kTopologyAbsent), "topology-absent caveat present");
    Check(Contains(RenderEnvJson(r, false, false), L"\"consentDialogProbed\": false"),
          "the dialog question is marked as not probed");
}

// ---------------------------------------------------------------------------
// 5) 拓扑那一问没答出来：不能当成"有一块屏"
// ---------------------------------------------------------------------------
void TestTopologyUnknown() {
    Section("5) 屏幕拓扑问不出来");
    EnvProbe p = HealthyProbe();
    p.displayTopology = Tri::kUnknown;
    p.monitorCountKnown = false;
    p.monitorCount = 0;
    EnvReport r = BuildEnvReport(p, EnvQueryKind::kCapabilities);
    const BackendReport* bitblt = Find(r, CaptureMethod::kBitBlt);
    Check(bitblt && bitblt->status == CapStatus::kUnverified, "unknown topology leaves bitblt unverified");
    Check(bitblt && bitblt->reason == cap_reason::kDisplayTopologyUnknown, "and says which question failed");
    const BackendReport* wgc = Find(r, CaptureMethod::kWgc);
    Check(wgc && wgc->status == CapStatus::kAvailable, "window-content route is not gated by topology");
    Check(HasCaveat(r, caveat::kTopologyUnverifiable), "topology-unverifiable caveat present");
    Check(!HasCaveat(r, caveat::kTopologyAbsent), "unknown is not reported as absent");

    // 会话号问不出来时，"能不能弹框"也只能是 unknown，不许猜"应该能弹"
    EnvProbe p2 = HealthyProbe();
    p2.consoleAttached = Tri::kUnknown;
    p2.processSessionKnown = false;
    p2.consoleSessionKnown = false;
    EnvReport r2 = BuildEnvReport(p2, EnvQueryKind::kCapabilities);
    Check(r2.consentDialogExpected == Tri::kUnknown, "unknown session does not claim a dialog can show");
}

// ---------------------------------------------------------------------------
// 6) 版本问不出来：整份报告一律 unverified，且不按版本筛掉任何一条链
// ---------------------------------------------------------------------------
void TestOsUnverifiable() {
    Section("6) 本机版本问不出来");
    EnvProbe p = HealthyProbe();
    p.os = OsVersion{};   // known=false
    EnvReport r = BuildEnvReport(p, EnvQueryKind::kCapabilities);
    Check(r.os.known == false, "report says the version was not obtained");
    for (const auto& b : r.backends) {
        // bitblt 是唯一那条**没有版本门槛**的路线：版本问不出来也挡不到它，所以它照旧 available。
        // 这一条特意分开判，免得把"版本没问到"写成"所有路线都未知"那种说过头话的断言。
        if (b.method == CaptureMethod::kBitBlt) {
            Check(b.status == CapStatus::kAvailable, "bitblt has no floor to be unsure about");
            continue;
        }
        Check(b.status == CapStatus::kUnverified,
              ("unverifiable os: " + Narrow(b.name) + " unverified").c_str());
        Check(b.reason == cap_reason::kOsUnverifiable,
              ("unverifiable os: " + Narrow(b.name) + " names the question").c_str());
    }
    for (const auto& f : r.formats) {
        if (!f.compiled) continue;
        Check(f.status == CapStatus::kUnverified, ("unverifiable os: format " + Narrow(f.name)).c_str());
    }
    Check(r.matchesVerifiedEnv == Tri::kUnknown, "unverifiable os cannot claim a tested environment");
    Check(HasCaveat(r, caveat::kOsUnverifiable), "os-unverifiable caveat present");
    // 与 GateChannels 同源：版本问不出来时链一条都不筛，链里还是完整的四条
    Check(r.autoChainWindow.size() == 4, "unverifiable os leaves the whole window chain in place");
    Check(r.autoChainScreen.size() == 3, "and the whole screen chain");
}

// ---------------------------------------------------------------------------
// 7) 某个编码器不可用：只有那一个格式变 unavailable，别的不跟着倒
// ---------------------------------------------------------------------------
void TestEncoderUnavailable() {
    Section("7) 某个编码器不可用");
    EnvProbe p = HealthyProbe();
    p.encoders.emplace_back(ImageFormat::kJpeg, Tri::kNo);
    EnvReport r = BuildEnvReport(p, EnvQueryKind::kCapabilities);
    const FormatReport* jpeg = FindFormat(r, L"jpeg");
    const FormatReport* png = FindFormat(r, L"png");
    Check(jpeg && jpeg->status == CapStatus::kUnavailable, "the unregistered encoder is unavailable");
    Check(jpeg && jpeg->reason == cap_reason::kEncoderNotRegistered, "and names the encoder as the reason");
    Check(jpeg && jpeg->registered == Tri::kNo, "registered field carries the answer");
    Check(png && png->status == CapStatus::kAvailable, "the other encoder is not dragged down");
    Check(png && png->registered == Tri::kUnknown, "an encoder nobody asked about is unknown, not yes");
    Check(HasCaveat(r, caveat::kEncoderUnprobed), "not-probing-encoders is stated as a caveat");

    // 整工具那一道下限（任何一张图都要编码器组件）：10240 以下全部格式都不可用
    EnvReport early = BuildEnvReport(ProbeWithBuild(os_floor::kEncoder - 1), EnvQueryKind::kCapabilities);
    for (const auto& f : early.formats) {
        if (!f.compiled) continue;
        Check(f.status == CapStatus::kUnavailable, ("below the encoder floor: " + Narrow(f.name)).c_str());
        Check(f.reason == cap_reason::kOsBelowMin, ("below the encoder floor reason: " + Narrow(f.name)).c_str());
    }

    // 曾经列在取值里、后来因为"没有编码器"删掉的两个：明确回答"这个构建里没有"
    const FormatReport* webp = FindFormat(early, L"webp");
    Check(webp && webp->compiled == false, "webp is answered as not compiled");
    Check(webp && webp->status == CapStatus::kUnavailable, "webp is unavailable");
    Check(webp && webp->reason == cap_reason::kNotCompiled, "and the reason is the build, not the machine");
    // 这一条与本机版本无关：健康机器上也是 not_compiled
    EnvReport healthy = BuildEnvReport(HealthyProbe(), EnvQueryKind::kCapabilities);
    const FormatReport* webpHere = FindFormat(healthy, L"webp");
    Check(webpHere && webpHere->reason == cap_reason::kNotCompiled, "webp stays not_compiled here");
}

// ---------------------------------------------------------------------------
// 8) --yes 的适用范围：桌面那一级永远问人，窗口内容那一级才免
// ---------------------------------------------------------------------------
void TestYesScope() {
    Section("8) --yes 的适用范围与登记表一致");
    EnvReport r = BuildEnvReport(HealthyProbe(), EnvQueryKind::kCapabilities);
    Check(!r.consentPaths.empty(), "authorization section lists paths");
    for (const auto& p : r.consentPaths) {
        const PixelScope scope = ScopeOf(p.path.c_str());
        Check(p.scope == ScopeName(scope), ("scope matches the registry: " + Narrow(p.path)).c_str());
        // 没有一条路径能让 --yes 免掉"要不要问人"里的"窗口路径没给 --yes 时必须问"
        Check(p.consentWithoutYes == true, ("without --yes asks a human: " + Narrow(p.path)).c_str());
        if (p.scope == kScopeDesktop) {
            Check(p.consentWithYes == true, ("desktop path still asks with --yes: " + Narrow(p.path)).c_str());
        } else {
            Check(p.consentWithYes == false,
                  ("window-content path is skipped by --yes: " + Narrow(p.path)).c_str());
        }
    }
    // 登记不上的名字一律按桌面处理（漏登记 = 更严格），这一条也要在查询里看得见
    const ConsentPathReport* unknown = FindPath(r.consentPaths, paths::kUnknown);
    Check(unknown != nullptr && unknown->scope == kScopeDesktop, "unregistered name is desktop");
    Check(unknown && unknown->consentWithYes == true, "unregistered name still asks with --yes");

    // 后端那一段里出现的每一条路径名都必须能在登记表里查到：
    // 这条不变量挡的是"通道改了路径名而查询这边还在写旧名字"，也就是两份信息互相打脸。
    for (const auto& b : r.backends) {
        Check(!b.paths.empty(), ("backend has paths: " + Narrow(b.name)).c_str());
        for (const auto& p : b.paths) {
            const bool registered = FindPath(r.consentPaths, p.path.c_str()) != nullptr;
            Check(registered, ("backend path is registered: " + Narrow(b.name) + "/" + Narrow(p.path)).c_str());
        }
    }
    // dwm 内部那条屏幕退路必须看得见，否则 --yes 会被误当成覆盖 dwm 的全部行为
    const BackendReport* dwm = Find(r, CaptureMethod::kDwmThumbnail);
    Check(dwm && FindPath(dwm->paths, paths::kDwmScreen) != nullptr, "dwm shows its screen fallback");
    Check(dwm && FindPath(dwm->paths, paths::kDwmScreen)->scope == kScopeDesktop,
          "dwm's fallback is a desktop path");
    // bitblt / duplication 的窗口目标取的也是桌面像素
    const BackendReport* dup = Find(r, CaptureMethod::kDuplication);
    Check(dup && FindPath(dup->paths, paths::kDuplicationFrame) != nullptr, "duplication lists its frame path");
    Check(dup && FindPath(dup->paths, paths::kDuplicationFrame)->scope == kScopeDesktop,
          "duplication path is desktop scope");
}

// ---------------------------------------------------------------------------
// 9) 两份查询共享同一批判据：除了契约名，字段不许各写一份
// ---------------------------------------------------------------------------
void TestTwoCommandsAgree() {
    Section("9) --capabilities 与 --diagnostics 不打脸");
    const EnvProbe p = HealthyProbe();
    EnvReport caps = BuildEnvReport(p, EnvQueryKind::kCapabilities);
    EnvReport diag = BuildEnvReport(p, EnvQueryKind::kDiagnostics);

    Check(caps.contract == L"capabilities" && diag.contract == L"diagnostics", "contract names differ");
    Check(caps.contractVersion == diag.contractVersion, "same contract version");
    Check(caps.version == diag.version && caps.version == kVersion, "version comes from Version.h");
    Check(caps.buildId == diag.buildId, "same build id");
    Check(caps.arch == diag.arch, "same architecture");
    Check(caps.os.build == diag.os.build && caps.os.known == diag.os.known, "same os");
    Check(caps.declaredMinBuild == diag.declaredMinBuild, "same declared floor");
    Check(caps.monitorCount == diag.monitorCount, "same topology");
    Check(caps.backends.size() == diag.backends.size(), "same backend count");
    for (size_t i = 0; i < caps.backends.size(); ++i) {
        Check(caps.backends[i].name == diag.backends[i].name, "same backend order");
        Check(caps.backends[i].status == diag.backends[i].status, "same backend status");
        Check(caps.backends[i].reason == diag.backends[i].reason, "same backend reason");
    }
    Check(caps.formats.size() == diag.formats.size(), "same format list");
    Check(caps.consentPaths.size() == diag.consentPaths.size(), "same authorization table");
    Check(caps.limits.maxFrameSide == diag.limits.maxFrameSide, "same limits");
    Check(caps.caveats == diag.caveats, "same caveats");

    // 渲染上的差别只有段落取舍：契约名、构建明细、每一问的原始答案
    const std::wstring capsJson = RenderEnvJson(caps, false, false);
    const std::wstring diagJson = RenderEnvJson(diag, false, false);
    Check(Contains(capsJson, L"\"capabilities\""), "capabilities contract in its own document");
    Check(Contains(diagJson, L"\"diagnostics\""), "diagnostics contract in its own document");
    Check(!Contains(capsJson, L"\"linkTimestamp\""), "capabilities keeps build detail out");
    Check(Contains(diagJson, L"\"linkTimestamp\""), "diagnostics carries the checkable build id fields");
    Check(Contains(diagJson, L"\"buildId\""), "both documents state the build id");
    Check(Contains(capsJson, L"\"buildId\""), "capabilities states buildId too");
    Check(Contains(RenderEnvJson(caps, true, false), L"\"linkTimestamp\""),
          "--verbose expands the build detail for capabilities as well");
}

// ---------------------------------------------------------------------------
// 10) 渲染：全部 ASCII（所以 --lang 改不动它）、无路径与身份、未知值写成 unknown
// ---------------------------------------------------------------------------
void TestRenderedShape() {
    Section("10) 渲染出来的那份东西");
    EnvReport r = BuildEnvReport(HealthyProbe(), EnvQueryKind::kCapabilities);
    const std::wstring json = RenderEnvJson(r, false, false);

    bool asciiOnly = true;
    for (wchar_t c : json) {
        // 缩进用的换行不算"非 ASCII"，除此之外这份文档一个多字节字符都不许出现：
        // 那才是"--lang 换任何一种语言，这份文档逐字节相同"的真正依据。
        if (c == L'\n' || c == L'\r') continue;
        if (c < 0x20 || c > 0x7E) { asciiOnly = false; break; }
    }
    Check(asciiOnly, "the whole document is ASCII: no language can change it");
    Check(!Contains(json, L"\\\\"), "no Windows path in the document");
    Check(!Contains(json, L"C:"), "no drive-qualified path");
    Check(!Contains(json, L"%USERPROFILE%") && !Contains(json, L"COMPUTERNAME"), "no environment variable leak");
    Check(Contains(json, L"\"binary\": \"ECAPTURE.EXE\""), "the binary name is there, without a directory");
    Check(json.size() > 1500, "the document is a real report, not a stub");
    Check(json.front() == L'{' && json.back() == L'}', "one JSON document");

    // 三值事实一律写出 unknown，不整个键消失：调用方要能分辨"没这回事"与"没问到"
    EnvProbe p = HealthyProbe();
    p.arch.clear();
    p.buildId.clear();
    p.buildIdKnown = false;
    p.remoteSession = Tri::kUnknown;
    EnvReport blind = BuildEnvReport(p, EnvQueryKind::kCapabilities);
    const std::wstring blindJson = RenderEnvJson(blind, false, false);
    Check(Contains(blindJson, L"\"arch\": \"unknown\""), "unknown architecture is written as unknown");
    Check(Contains(blindJson, L"\"buildId\": \"unknown\""), "unknown build id is written as unknown");
    Check(Contains(blindJson, L"\"remoteSession\": \"unknown\""), "unknown remote session stays unknown");
    Check(Contains(blindJson, L"\"known\": false") || Contains(blindJson, L"\"buildId\": \"unknown\""),
          "the unknown-ness is reported as data");
    Check(HasCaveat(blind, caveat::kBuildIdUnavailable), "missing build identity says so");

    // --verbose 才有 probes；--quiet 只去掉 caveats
    Check(!Contains(json, L"\"probes\""), "probes are off by default");
    Check(Contains(RenderEnvJson(r, true, false), L"\"probes\""), "--verbose adds probes");
    Check(Contains(RenderEnvJson(r, true, false), L"RtlGetVersion"),
          "probes name the API each answer came from");
    Check(Contains(json, L"caveats"), "caveats on by default");
    Check(!Contains(RenderEnvJson(r, false, true), L"caveats"), "--quiet drops only caveats");
    Check(Contains(RenderEnvJson(r, false, true), L"\"backends\""), "--quiet keeps the answers");
}

// ---------------------------------------------------------------------------
// 11) 隐私自述：默认不上传、不采集画面、不枚举用户文件、不输出身份
// ---------------------------------------------------------------------------
void TestPrivacyFlags() {
    Section("11) 隐私自述");
    EnvReport r = BuildEnvReport(HealthyProbe(), EnvQueryKind::kCapabilities);
    Check(r.privacyCapturesScreen == false, "self-declared: no screen captured");
    Check(r.privacyShowsDialog == false, "self-declared: no dialog shown");
    Check(r.privacyUploads == false, "self-declared: nothing uploaded");
    Check(r.privacyEnumeratesUserFiles == false, "self-declared: no user files enumerated");
    Check(r.privacyReadsEnvVars == false, "self-declared: no environment variables read");
    Check(r.privacyIncludesUsernames == false, "self-declared: no usernames");
    Check(r.privacyIncludesPaths == false, "self-declared: no paths");
    const std::wstring json = RenderEnvJson(r, false, false);
    Check(Contains(json, L"\"capturesScreen\": false"), "the self-declaration is in the document");
    Check(Contains(json, L"\"includesPaths\": false"), "and so is the no-paths one");
}

// ---------------------------------------------------------------------------
// 12) limits 与解析判据同源：数字必须在别处只写了一次
// ---------------------------------------------------------------------------
void TestLimitsAreSingleSourced() {
    Section("12) limits 与实现同源");
    EnvReport r = BuildEnvReport(HealthyProbe(), EnvQueryKind::kCapabilities);
    Check(r.limits.maxFrameSide == kFrameMaxSide, "maxFrameSide is the frame limit itself");
    Check(r.limits.maxFrameBytes == kFrameMaxBytes, "maxFrameBytes too");
    Check(r.limits.maxTimeoutMs == cli_limits::kMaxTimeoutMs, "maxTimeoutMs is the parse bound");
    Check(r.limits.isolatedCallMs == kIsolatedCallMs, "isolatedCallMs is the worker bound");
    Check(r.limits.maxWgcRecreates == kMaxWgcRecreates, "maxWgcRecreates is the WGC bound");
    Check(r.limits.maxOrdinal == cli_limits::kMaxOrdinal, "maxOrdinal is the ordinal bound");
    Check(r.limits.maxPid == cli_limits::kMaxPid, "maxPid is the PID bound");
    Check(r.limits.stdoutTargetsMax == 1, "stdout really is one image at a time");
    Check(r.limits.jpegQualityMin == static_cast<uint64_t>(cli_limits::kJpegQualityMin), "quality floor");
    Check(r.limits.jpegQualityMax == static_cast<uint64_t>(cli_limits::kJpegQualityMax), "quality ceiling");
    // --roi 的上限也必须是解析层那一个数（帮助、解析、查询三处读同一份，不在这里另写一个）
    Check(r.limits.roiMaxValue == cli_limits::kRoiMaxValue, "roi bound is the parse bound");
    Check(r.declaredMinBuild == os_floor::kSupportedMinBuild, "declared floor is SystemCompat's own");
    Check(r.verifiedOsBuild == verified_env::kOsBuild && r.verifiedArch == verified_env::kArch,
          "the recorded tested environment is one place only");
}

// ---------------------------------------------------------------------------
// 13) auto 链与 GateChannels 同源：查询里给的链就是那次截图会试的链
// ---------------------------------------------------------------------------
void TestChainsMatchTheGate() {
    Section("13) auto 链就是截图时那条链");
    const EnvProbe cases[] = {HealthyProbe(), ProbeWithBuild(10240), ProbeWithBuild(17134), [] {
                                  EnvProbe p = HealthyProbe();
                                  p.os = OsVersion{};
                                  return p;
                              }()};
    for (const EnvProbe& p : cases) {
        const EnvReport r = BuildEnvReport(p, EnvQueryKind::kCapabilities);
        const ChannelGate window = GateChannels(CaptureMethod::kAuto, false, p.os);
        const ChannelGate screen = GateChannels(CaptureMethod::kAuto, true, p.os);
        Check(r.autoChainWindow.size() == window.chain.size(),
              "window chain length equals GateChannels for that version");
        Check(r.autoChainScreen.size() == screen.chain.size(),
              "screen chain length equals GateChannels for that version");
        for (size_t i = 0; i < r.autoChainWindow.size() && i < window.chain.size(); ++i) {
            Check(r.autoChainWindow[i] == CaptureMethodName(window.chain[i]),
                  "window chain entries are the same ones");
        }
        for (size_t i = 0; i < r.autoChainScreen.size() && i < screen.chain.size(); ++i) {
            Check(r.autoChainScreen[i] == CaptureMethodName(screen.chain[i]),
                  "screen chain entries are the same ones");
        }
    }
    // 低到 10240：链里少了 wgc 那一条，剩下 dwm / printwindow / bitblt 三条（后两条下限是 9600）
    EnvReport low = BuildEnvReport(ProbeWithBuild(10240), EnvQueryKind::kCapabilities);
    Check(low.autoChainWindow.size() == 3, "10240 keeps the pre-WGC routes and bitblt");
    bool wgcInChain = false;
    for (const auto& n : low.autoChainWindow) wgcInChain = wgcInChain || n == L"wgc";
    Check(!wgcInChain, "wgc is not in the 10240 chain");
    const BackendReport* wgc = Find(low, CaptureMethod::kWgc);
    Check(wgc && wgc->status == CapStatus::kUnavailable, "and is reported unavailable");
}

}  // namespace

int main() {
    // 查询的 JSON 不读文案资源，但 BuildEnvReport 的邻居（CaptureScope 的诊断那一路）会，
    // 所以整份判据仍按英文文案跑，与其他 state 测试同一约定。
    SetLanguage(Language::kEn);

    TestMachineWords();
    TestThreeLayers();
    TestVersionFloors();
    TestNoGraphicsEnvironment();
    TestTopologyUnknown();
    TestOsUnverifiable();
    TestEncoderUnavailable();
    TestYesScope();
    TestTwoCommandsAgree();
    TestRenderedShape();
    TestPrivacyFlags();
    TestLimitsAreSingleSourced();
    TestChainsMatchTheGate();

    std::printf("\ncapabilities-state: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
