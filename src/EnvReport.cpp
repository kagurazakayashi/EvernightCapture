#include "EnvReport.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"   // kFrameMaxSide / kFrameMaxBytes
#include "CaptureScope.h"
#include "CliOptions.h"
#include "Deadline.h"        // kIsolatedCallMs
#include "Json.h"
#include "WgcGeometry.h"     // kMaxWgcRecreates

namespace ecapture {
namespace {

// 把 Support（版本下限那一层的判据）折成查询用的三态状态。
// 只翻译，不另写比较规则：那条线仍然只在 SystemCompat.cpp 里有一份。
CapStatus StatusOfSupport(Support support, std::wstring* reason) {
    switch (support) {
        case Support::kOk:
            *reason = cap_reason::kNone;
            return CapStatus::kAvailable;
        case Support::kBelow:
            *reason = cap_reason::kOsBelowMin;
            return CapStatus::kUnavailable;
        case Support::kUnknown:
            *reason = cap_reason::kOsUnverifiable;
            return CapStatus::kUnverified;
    }
    *reason = cap_reason::kOsUnverifiable;
    return CapStatus::kUnverified;
}

// 这条路线有没有"只走桌面像素"的那一面：桌面像素在这块桌面上取不到（会话里没有屏幕输出）
// 时，它整条路线都没有可用的目标。窗口内容路径不受屏幕拓扑影响，所以不因为"没屏"就判不可用。
bool OnlyDesktopPaths(const std::vector<ConsentPathReport>& paths) {
    if (paths.empty()) return false;
    for (const auto& p : paths) {
        if (p.scope == kScopeWindow) return false;
    }
    return true;
}

// 本项目实现了哪几条路线。名字用 --capture 的那个取值（机器名只增不改名）。
// kAuto 不在这里：它不是一条通道，而是一条按本机筛过的回退链，单独写成 autoChains。
constexpr CaptureMethod kImplemented[] = {
    CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail, CaptureMethod::kPrintWindow,
    CaptureMethod::kBitBlt, CaptureMethod::kDuplication};

// 每个格式各自的编码器：这一份顺序与 --format 的取值同源（ImageFormat 的全部枚举值）。
constexpr ImageFormat kImplementedFormats[] = {
    ImageFormat::kPng, ImageFormat::kJpeg, ImageFormat::kBmp, ImageFormat::kTiff,
    ImageFormat::kGif};

// 曾经列在取值里、后来因为"当前 SDK 没有对应编码器"而删掉的两个（见 AGENTS.md 的格式规则）。
// 继续列在查询结果里，为的是调用方（含看过旧文档的 AI）拿到一条"这个构建里没有这条路线"的
// 确定答案，而不是让它在两份文档之间猜。compiled=false 就是这个意思，与本机版本无关。
constexpr const wchar_t* kRemovedFormats[] = {L"webp", L"ico"};

// 一条路线在各类目标上实际走的那条内部路径。主路径由 CaptureScope 的
// WindowPathOf / ScreenPathOf 给（那两份函数就是通道实现自己用的同一套名字）；
// 这里额外列出的只有 dwm 那条"缩略图失败后改拷屏幕"的退路 —— 它不是用户能选的取值，
// 只在 dwm 内部走，但它的像素来源是桌面，调用方必须看得见，否则一条 --yes 就被误当成
// 覆盖 dwm 的全部行为。名字必须能在 CaptureScope 的登记表里查到，
// 这条不变量由 tests\capabilities_state.cpp 逐条判（改一处忘了另一处就会红）。
std::vector<ConsentPathReport> BackendPathsOf(CaptureMethod method) {
    std::vector<ConsentPathReport> out;
    auto add = [&](const wchar_t* target, const wchar_t* path) {
        ConsentPathReport r;
        r.target = target;
        r.path = path;
        r.scope = ScopeName(ScopeOf(path));
        r.consentWithoutYes = NeedsHumanConsent(path, false);
        r.consentWithYes = NeedsHumanConsent(path, true);
        out.push_back(std::move(r));
    };
    add(L"window", WindowPathOf(method));
    if (method == CaptureMethod::kDwmThumbnail) add(L"window", paths::kDwmScreen);
    const wchar_t* screen = ScreenPathOf(method);
    if (screen && std::wstring(screen) != paths::kUnknown) add(L"screen", screen);
    return out;
}

// 登记表里"某条内部路径的像素来源"那一份事实，整份交出去（--yes 的适用范围）。
// 表本体只在 CaptureScope.cpp 里有一份，这里只读它。
std::vector<ConsentPathReport> ConsentTable() {
    std::vector<ConsentPathReport> out;
    for (const PathScopeEntry& e : RegisteredCapturePaths()) {
        ConsentPathReport r;
        r.target = L"any";   // 登记表按路径名分类，与"窗口还是屏幕"这一层无关
        r.path = e.path;
        r.scope = ScopeName(e.scope);
        r.consentWithoutYes = NeedsHumanConsent(e.path, false);
        r.consentWithYes = NeedsHumanConsent(e.path, true);
        out.push_back(std::move(r));
    }
    return out;
}

std::wstring Hex(unsigned long long value, int digits) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%0*llX", digits, value);
    return buf;
}

// PE 的机器类型 -> 与 arch 同一套词表（别让查询里出现两种"64 位 x86"的写法）。
std::wstring PeMachineName(uint16_t machine) {
    switch (machine) {
        case IMAGE_FILE_MACHINE_AMD64: return L"x64";
        case IMAGE_FILE_MACHINE_ARM64: return L"arm64";
        case IMAGE_FILE_MACHINE_I386: return L"x86";
        case IMAGE_FILE_MACHINE_ARM: return L"arm";
        default: return std::wstring();
    }
}

// 子系统：本项目是控制台程序（wmain + 不画窗口）。这条只是"为什么 stdout 能用"的根据之一，
// 不是支持声明；PE 头里那个 subsystem version 是链接器默认值，见 caveat.kSubsystemIsLinkerDefault。
std::wstring SubsystemName(uint16_t subsystem) {
    switch (subsystem) {
        case IMAGE_SUBSYSTEM_WINDOWS_CUI: return L"cui";
        case IMAGE_SUBSYSTEM_WINDOWS_GUI: return L"gui";
        default: return std::wstring();
    }
}

// ---------------------------------------------------------------------------
// 探针用的几条廉价问答。每一条都不取帧、不弹框、不读文件、不读环境变量。
// ---------------------------------------------------------------------------

std::wstring NativeArch() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return L"x64";
        case PROCESSOR_ARCHITECTURE_ARM64: return L"arm64";
        case PROCESSOR_ARCHITECTURE_INTEL: return L"x86";
        case PROCESSOR_ARCHITECTURE_ARM: return L"arm";
        default: return std::wstring();   // 认不出来 = unknown，不猜
    }
}

// 本进程的会话号，与"接在控制台上的那个会话"是否同一个。
// 服务会话 / 计划任务 / 无人登录的会话里，确认框弹出去也没有人看得见 —— 这一问是
// capture.consent_unavailable 那次故障的**提前**答案，而且不必真去弹一次框。
void ProbeSessions(Tri* attached, bool* ownKnown, uint32_t* own, bool* consoleKnown,
                   uint32_t* consoleId) {
    *attached = Tri::kUnknown;
    *ownKnown = false;
    *consoleKnown = false;
    DWORD sid = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sid)) {
        *own = sid;
        *ownKnown = true;
    }
    // WTSGetActiveConsoleSessionId 在没有控制台会话时回 0xFFFFFFFF（例如整机无人登录）。
    const DWORD console = WTSGetActiveConsoleSessionId();
    if (console != 0xFFFFFFFFull) {
        *consoleId = console;
        *consoleKnown = true;
    }
    if (*ownKnown && *consoleKnown) *attached = (*own == *consoleId) ? Tri::kYes : Tri::kNo;
}

// 屏幕上有没有可用的输出，以及有几块。回调只累加计数，一条异常都不许抛出去
// （回调里抛出会直接终止进程，看起来就像"查询自己崩了"）。
BOOL CALLBACK CountMonitorCallback(HMONITOR, HDC, LPRECT, LPARAM param) {
    auto* counter = reinterpret_cast<uint64_t*>(param);
    if (counter) ++*counter;
    return TRUE;
}

void ProbeTopology(Tri* present, bool* countKnown, uint32_t* count) {
    uint64_t n = 0;
    const BOOL ok = EnumDisplayMonitors(nullptr, nullptr, CountMonitorCallback,
                                        reinterpret_cast<LPARAM>(&n));
    *countKnown = ok != FALSE;
    *count = ok ? static_cast<uint32_t>(n > 0xFFFFFFFFull ? 0xFFFFFFFFull : n) : 0;
    if (!ok) {
        *present = Tri::kUnknown;   // 枚举本身没跑成：不等于"一块屏都没有"
        return;
    }
    *present = n > 0 ? Tri::kYes : Tri::kNo;
}

// 本进程是不是被提升过（管理员）。这条与"别人的窗口截不到"有关：UIPI 不会把跨进程的
// 绘制请求送进更高完整性级别的进程，所以未提升的这一次对提升过的目标本来就没把握。
// 只回答"是/否"，不读用户名、不读 SID 归属。
Tri ProbeElevation() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return Tri::kUnknown;
    TOKEN_ELEVATION elev{};
    DWORD returned = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &returned);
    CloseHandle(token);
    if (!ok || returned < sizeof(elev)) return Tri::kUnknown;
    return elev.TokenIsElevated ? Tri::kYes : Tri::kNo;
}

// 读本进程自己那份已经在内存里的 PE 头：不 CreateFile、不枚举目录，所以既不会碰到
// 别人的文件，也不会因为路径里有用户名而漏出身份。这是"可核对的构建标识"的来源。
// 判据只有形状这一层：MSVC（以及别的链接器）都把 NT 头放在映像的第一页里，所以 e_lfanew
// 落在这一页之内才算说得通；不成立就当这一问没答出来，不去读一个来路不明的偏移。
void ProbePeIdentity(EnvProbe* p) {
    const HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return;
    auto* image = reinterpret_cast<uint8_t*>(base);
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (dos->e_lfanew <= static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
        dos->e_lfanew >= static_cast<LONG>(4096)) {
        return;
    }
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    p->buildIdKnown = true;
    p->linkTimestampKnown = true;
    p->linkTimestamp = nt->FileHeader.TimeDateStamp;
    p->peMachineKnown = true;
    p->peMachine = nt->FileHeader.Machine;
    p->imageSizeKnown = true;
    p->imageSize = nt->OptionalHeader.SizeOfImage;
    p->subsystemKnown = true;
    p->subsystem = nt->OptionalHeader.Subsystem;
    p->subsystemVersionKnown = true;
    p->subsystemMajor = nt->OptionalHeader.MajorSubsystemVersion;
    p->subsystemMinor = nt->OptionalHeader.MinorSubsystemVersion;
    wchar_t buf[32];
    swprintf(buf, 32, L"%u.%u", static_cast<unsigned>(nt->OptionalHeader.MajorLinkerVersion),
             static_cast<unsigned>(nt->OptionalHeader.MinorLinkerVersion));
    p->linkerVersion = buf;
}

const wchar_t* TriOrNull(Tri value) { return TriName(value); }

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
    return Tri::kUnknown;   // 没列出的 = 这一问没有答案，不等于"能用"
}

EnvProbe ProbeEnvFacts() {
    EnvProbe p;
    p.os = ProbeOsVersion();
    p.arch = NativeArch();
    ProbeSessions(&p.consoleAttached, &p.processSessionKnown, &p.processSessionId,
                  &p.consoleSessionKnown, &p.consoleSessionId);
    // GetSystemMetrics 在两种情况下都回 0（"不是远程会话"与"这个会话里量不出任何东西"），
    // 所以这里只报"是不是远程"，把"量不出来"那一面留给 displayTopology 那条独立的问答。
    p.remoteSession = GetSystemMetrics(SM_REMOTESESSION) != 0 ? Tri::kYes : Tri::kNo;
    ProbeTopology(&p.displayTopology, &p.monitorCountKnown, &p.monitorCount);
    p.elevated = ProbeElevation();
    ProbePeIdentity(&p);
    wchar_t buf[64];
    if (p.buildIdKnown) {
        // 构建标识 = 版本号 + 架构 + 链接时间戳。三者都能和发布产物逐字节对上：
        // 版本号在 --version 与文件属性里，后两条在 PE 头里（dumpbin /headers 读得到）。
        std::wstring archText = p.arch.empty() ? L"unknown" : p.arch;
        swprintf(buf, 64, L"%s-%s-%08lX", kVersion, archText.c_str(),
                 static_cast<unsigned long>(p.linkTimestamp));
        p.buildId = buf;
    }
    // 编码器一律不实测（理由见 EnvProbe::encoders 的注释），所以这里一条都不填 = 全 unknown。
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

    // 这三条是机器标识，按规矩不翻译（进 JSON 的取值与 --capture / --format 那一类同级）。
    r.programName = L"EvernightCapture";
    r.binaryName = L"ECAPTURE.EXE";
    r.version = kVersion;
    r.arch = probe.arch.empty() ? L"unknown" : probe.arch;
    r.buildId = probe.buildIdKnown && !probe.buildId.empty() ? probe.buildId : L"unknown";

    r.os = probe.os;
    r.declaredMinBuild = os_floor::kSupportedMinBuild;
    r.encoderMinBuild = os_floor::kEncoder;
    r.verifiedOsBuild = verified_env::kOsBuild;
    r.verifiedArch = verified_env::kArch;

    // "这台机器是不是本项目实测过的那一种"：只有版本与架构都问出来才谈得上匹配。
    // 问不出来就是 unknown —— 不因为"看起来像 19045"就宣称实测过。
    if (!probe.os.known || probe.arch.empty()) {
        r.matchesVerifiedEnv = Tri::kUnknown;
    } else {
        r.matchesVerifiedEnv = (probe.os.build == verified_env::kOsBuild &&
                                probe.arch == verified_env::kArch)
                                   ? Tri::kYes
                                   : Tri::kNo;
    }

    r.consoleAttached = probe.consoleAttached;
    r.remoteSession = probe.remoteSession;
    r.elevated = probe.elevated;
    r.displayTopology = probe.displayTopology;
    r.monitorCountKnown = probe.monitorCountKnown;
    r.monitorCount = probe.monitorCount;
    r.processSessionKnown = probe.processSessionKnown;
    r.processSessionId = probe.processSessionId;
    r.consoleSessionKnown = probe.consoleSessionKnown;
    r.consoleSessionId = probe.consoleSessionId;

    // 确认框能不能弹：这一条是**推出来的**（会话号 + 屏幕拓扑），本查询没有真去弹一个框。
    // 任一条问答不出来就是 unknown，不猜"应该能弹"。
    if (probe.consoleAttached == Tri::kNo) {
        r.consentDialogExpected = Tri::kNo;
    } else if (probe.consoleAttached == Tri::kYes && probe.displayTopology == Tri::kYes) {
        r.consentDialogExpected = Tri::kYes;
    } else if (probe.consoleAttached == Tri::kUnknown || probe.displayTopology == Tri::kUnknown) {
        r.consentDialogExpected = Tri::kUnknown;
    } else {
        // 接在控制台上但一块屏都没有：桌面像素取不到，弹给谁看也说不准，不作断言。
        r.consentDialogExpected = Tri::kUnknown;
    }

    r.limits.maxFrameSide = kFrameMaxSide;
    r.limits.maxFrameBytes = kFrameMaxBytes;
    r.limits.maxTimeoutMs = cli_limits::kMaxTimeoutMs;
    r.limits.isolatedCallMs = kIsolatedCallMs;
    r.limits.maxWgcRecreates = kMaxWgcRecreates;
    r.limits.maxOrdinal = cli_limits::kMaxOrdinal;
    r.limits.maxPid = cli_limits::kMaxPid;
    r.limits.stdoutTargetsMax = 1;   // 标准输出一次只交付一张图（见「输出契约」第 6 条）
    r.limits.jpegQualityMin = cli_limits::kJpegQualityMin;
    r.limits.jpegQualityMax = cli_limits::kJpegQualityMax;

    // ---- 后端 ----
    const bool topologyAbsent = probe.displayTopology == Tri::kNo;
    const bool topologyUnknown = probe.displayTopology == Tri::kUnknown;
    for (const CaptureMethod method : kImplemented) {
        BackendReport b;
        b.method = method;
        b.name = CaptureMethodName(method);
        b.compiled = true;   // 这一组里的每一条都已实现并接进分派表
        b.paths = BackendPathsOf(method);

        const Capability cap = AssessChannel(method, false, probe.os);
        b.minBuild = cap.minBuild;
        b.status = StatusOfSupport(cap.support, &b.reason);
        if (b.status == CapStatus::kAvailable) {
            // 版本这一关过了，再看屏幕拓扑：只走桌面像素的那两条（bitblt / duplication）
            // 在没有屏幕输出的会话里根本没有可取的地方。
            if (topologyAbsent && OnlyDesktopPaths(b.paths)) {
                b.status = CapStatus::kUnavailable;
                b.reason = cap_reason::kNoDisplayTopology;
            } else if (topologyUnknown && OnlyDesktopPaths(b.paths)) {
                // 拓扑那一问没答出来 ≠ "有一条屏"：这条状态只能是 unverified，不能冒充可用。
                b.status = CapStatus::kUnverified;
                b.reason = cap_reason::kDisplayTopologyUnknown;
            }
        }
        b.verifiedOnThisMachine = r.matchesVerifiedEnv;
        r.backends.push_back(std::move(b));
    }

    // auto 的两条链：与真去截图时用的同一个 GateChannels，所以"查询里给的链"与
    // "那次实际会试的链"不可能各写一份顺序而互相打脸。
    for (const bool screenMode : {false, true}) {
        ChannelGate gate = GateChannels(CaptureMethod::kAuto, screenMode, probe.os);
        std::vector<std::wstring> chain;
        chain.reserve(gate.chain.size());
        for (const CaptureMethod m : gate.chain) chain.push_back(CaptureMethodName(m));
        if (screenMode) r.autoChainScreen = std::move(chain);
        else r.autoChainWindow = std::move(chain);
    }

    // ---- 格式 ----
    // 六条通道共用同一个 BitmapEncoder，所以整工具那一道下限（AssessRuntime）就是格式的
    // 版本判据；再叠上"某个编码器确实没登记"这一问（真机上没去实测，所以是 unknown）。
    const Capability runtime = AssessRuntime(probe.os);
    for (const ImageFormat format : kImplementedFormats) {
        FormatReport f;
        f.name = FormatName(format);
        f.compiled = true;
        f.minBuild = runtime.minBuild;
        f.status = StatusOfSupport(runtime.support, &f.reason);
        f.registered = probe.EncoderRegistered(format);
        if (f.registered == Tri::kNo) {
            f.status = CapStatus::kUnavailable;
            f.reason = cap_reason::kEncoderNotRegistered;
        }
        r.formats.push_back(std::move(f));
    }
    for (const wchar_t* removed : kRemovedFormats) {
        FormatReport f;
        f.name = removed;
        f.compiled = false;
        f.status = CapStatus::kUnavailable;
        f.reason = cap_reason::kNotCompiled;
        f.registered = Tri::kUnknown;
        r.formats.push_back(std::move(f));
    }

    // ---- --yes 的适用范围 ----
    r.consentPaths = ConsentTable();

    // ---- caveats：把"这份报告没说过什么"写出来，免得调用方把 available 读成保证 ----
    r.caveats.push_back(caveat::kNoCapture);
    r.caveats.push_back(caveat::kNoDialog);
    r.caveats.push_back(caveat::kNotAGuarantee);
    r.caveats.push_back(caveat::kDeviceNotPredicted);
    r.caveats.push_back(caveat::kEncoderUnprobed);
    r.caveats.push_back(caveat::kSessionInferred);
    if (!probe.buildIdKnown) r.caveats.push_back(caveat::kBuildIdUnavailable);
    if (r.matchesVerifiedEnv == Tri::kNo) r.caveats.push_back(caveat::kNotTestedHere);
    if (r.matchesVerifiedEnv == Tri::kUnknown) r.caveats.push_back(caveat::kTestedEnvUnknown);
    if (probe.os.known == false) r.caveats.push_back(caveat::kOsUnverifiable);
    if (topologyAbsent) r.caveats.push_back(caveat::kTopologyAbsent);
    if (topologyUnknown) r.caveats.push_back(caveat::kTopologyUnverifiable);
    if (probe.remoteSession == Tri::kYes) r.caveats.push_back(caveat::kRemoteSession);
    if (r.consentDialogExpected == Tri::kNo) r.caveats.push_back(caveat::kDesktopNeedsDialog);
    if (probe.elevated == Tri::kNo) r.caveats.push_back(caveat::kNotElevated);
    // PE 头里那个 subsystem version 是链接器默认值，不是本工具的支持声明（AGENTS.md
    // 《系统兼容性与能力检查》里"别拿它当声明"那一条同源），所以只要把它报出来就要配这一句。
    if (probe.subsystemVersionKnown) r.caveats.push_back(caveat::kSubsystemIsLinkerDefault);

    return r;
}

namespace {

// ---------------------------------------------------------------------------
// 渲染：JSON 里所有取值都是 ASCII 机器词，不读文案资源。
// 所以同一台机器上换 --lang，这份文档逐字节相同（tests\capabilities.ps1 就按这条判）。
// 三值事实一律写成 "yes" / "no" / "unknown"，不会因为"不知道"而整个键消失。
// ---------------------------------------------------------------------------
void WriteTriState(Json& j, const wchar_t* key, Tri value) {
    j.Key(key).Value(TriOrNull(value));
}

void WriteConsentPaths(Json& j, const std::vector<ConsentPathReport>& paths) {
    j.Arr();
    for (const auto& p : paths) {
        j.Obj();
        j.Key(L"target").Value(p.target);
        j.Key(L"path").Value(p.path);
        j.Key(L"scope").Value(p.scope);
        // 这两条一起说清了 --yes 的边界：桌面那几条 consentWithYes 恒为 true，
        // 也就是说"给了 --yes 还要问人"，与 AGENTS.md《截图授权与 --yes》那张表同源。
        j.Key(L"consentWithoutYes").Value(p.consentWithoutYes);
        j.Key(L"consentWithYes").Value(p.consentWithYes);
        j.End();
    }
    j.End();
}

void WriteBackends(Json& j, const std::vector<BackendReport>& backends) {
    j.Arr();
    for (const auto& b : backends) {
        j.Obj();
        j.Key(L"name").Value(b.name);
        j.Key(L"compiled").Value(b.compiled);   // 这个构建里有没有这条路线
        j.Key(L"status").Value(CapStatusName(b.status));  // 这台机器现在让不让走
        j.Key(L"reason").Value(b.reason);
        j.Key(L"minBuild").Value(static_cast<long long>(b.minBuild));  // 0 = 没有版本门槛
        j.Key(L"verifiedOnThisMachine").Value(TriOrNull(b.verifiedOnThisMachine));
        j.Key(L"paths");
        WriteConsentPaths(j, b.paths);
        j.End();
    }
    j.End();
}

void WriteFormats(Json& j, const std::vector<FormatReport>& formats) {
    j.Arr();
    for (const auto& f : formats) {
        j.Obj();
        j.Key(L"name").Value(f.name);
        j.Key(L"compiled").Value(f.compiled);
        j.Key(L"status").Value(CapStatusName(f.status));
        j.Key(L"reason").Value(f.reason);
        j.Key(L"minBuild").Value(static_cast<long long>(f.minBuild));
        // registered 是"这次有没有真去问编码器组件"的答案。没问就是 unknown，
        // 不拿 status 冒充它（status 里那条 available 只由版本下限与编译支持支撑）。
        j.Key(L"registered").Value(TriOrNull(f.registered));
        j.End();
    }
    j.End();
}

void WriteStringArray(Json& j, const wchar_t* key, const std::vector<std::wstring>& items) {
    j.Key(key).Arr();
    for (const auto& s : items) j.Value(s);
    j.End();
}

// --verbose 才展开：每一问的原始答案，让用户不必猜"这个 available 是怎么来的"就能提交。
void WriteProbes(Json& j, const EnvReport& r) {
    j.Key(L"probes").Arr();
    {
        j.Obj().Key(L"question").Value(L"osVersion");
        j.Key(L"known").Value(r.os.known);
        if (r.os.known) {
            wchar_t buf[40];
            swprintf(buf, 40, L"%u.%u.%u", r.os.major, r.os.minor, r.os.build);
            j.Key(L"answer").Value(buf);
            j.Key(L"source").Value(L"ntdll!RtlGetVersion");
        } else {
            j.Key(L"answer").Value(L"unknown");
            j.Key(L"source").Value(L"ntdll!RtlGetVersion");
        }
        j.End();
    }
    {
        j.Obj().Key(L"question").Value(L"architecture")
            .Key(L"known").Value(r.arch != L"unknown")
            .Key(L"answer").Value(r.arch)
            .Key(L"source").Value(L"GetNativeSystemInfo").End();
    }
    {
        j.Obj().Key(L"question").Value(L"consoleSession")
            .Key(L"known").Value(r.consoleAttached != Tri::kUnknown)
            .Key(L"answer").Value(TriOrNull(r.consoleAttached))
            .Key(L"source").Value(L"ProcessIdToSessionId/WTSGetActiveConsoleSessionId");
        if (r.processSessionKnown) {
            j.Key(L"processSession").Value(static_cast<long long>(r.processSessionId));
        }
        if (r.consoleSessionKnown) {
            j.Key(L"consoleSession").Value(static_cast<long long>(r.consoleSessionId));
        }
        j.End();
    }
    {
        j.Obj().Key(L"question").Value(L"remoteSession")
            .Key(L"known").Value(r.remoteSession != Tri::kUnknown)
            .Key(L"answer").Value(TriOrNull(r.remoteSession))
            .Key(L"source").Value(L"GetSystemMetrics(SM_REMOTESESSION)").End();
    }
    {
        j.Obj().Key(L"question").Value(L"displayTopology")
            .Key(L"known").Value(r.displayTopology != Tri::kUnknown)
            .Key(L"answer").Value(TriOrNull(r.displayTopology))
            .Key(L"source").Value(L"EnumDisplayMonitors");
        if (r.monitorCountKnown) j.Key(L"monitors").Value(static_cast<long long>(r.monitorCount));
        j.End();
    }
    {
        j.Obj().Key(L"question").Value(L"tokenElevation")
            .Key(L"known").Value(r.elevated != Tri::kUnknown)
            .Key(L"answer").Value(TriOrNull(r.elevated))
            .Key(L"source").Value(L"GetTokenInformation(TokenElevation)").End();
    }
    {
        j.Obj().Key(L"question").Value(L"peBuildIdentity")
            .Key(L"known").Value(r.probe.buildIdKnown)
            .Key(L"answer").Value(r.buildId)
            .Key(L"source").Value(L"in-memory PE headers (no file access)");
        if (r.probe.linkTimestampKnown) {
            j.Key(L"linkTimestamp").Value(Hex(r.probe.linkTimestamp, 8));
        }
        if (r.probe.peMachineKnown) {
            j.Key(L"peMachine").Value(Hex(r.probe.peMachine, 4));
            const std::wstring machine = PeMachineName(static_cast<uint16_t>(r.probe.peMachine));
            if (!machine.empty()) j.Key(L"peMachineName").Value(machine);
        }
        if (r.probe.imageSizeKnown) {
            j.Key(L"imageSize").Value(static_cast<long long>(r.probe.imageSize));
        }
        if (r.probe.subsystemKnown) {
            j.Key(L"subsystem").Value(Hex(r.probe.subsystem, 2));
            const std::wstring name = SubsystemName(static_cast<uint16_t>(r.probe.subsystem));
            if (!name.empty()) j.Key(L"subsystemName").Value(name);
        }
        if (r.probe.subsystemVersionKnown) {
            // 这一条是 MSVC 链接器的默认值，不是本工具的支持声明（AGENTS.md《系统兼容性》第二条硬事实）
            wchar_t buf[16];
            swprintf(buf, 16, L"%u.%u", r.probe.subsystemMajor, r.probe.subsystemMinor);
            j.Key(L"subsystemVersion").Value(buf);
        }
        if (!r.probe.linkerVersion.empty()) j.Key(L"linkerVersion").Value(r.probe.linkerVersion);
        j.End();
    }
    {
        j.Obj().Key(L"question").Value(L"encoders")
            .Key(L"known").Value(false)
            .Key(L"answer").Value(L"not_probed")
            .Key(L"source").Value(L"Windows.Graphics.Imaging.BitmapEncoder").End();
    }
    j.End();
}

void WriteBuildDetail(Json& j, const EnvReport& r) {
    j.Key(L"build").Obj();
    j.Key(L"known").Value(r.probe.buildIdKnown);
    j.Key(L"id").Value(r.buildId);
    if (r.probe.linkTimestampKnown) {
        j.Key(L"linkTimestamp").Value(Hex(r.probe.linkTimestamp, 8));
    }
    if (r.probe.peMachineKnown) {
        j.Key(L"peMachine").Value(Hex(r.probe.peMachine, 4));
        const std::wstring machine = PeMachineName(static_cast<uint16_t>(r.probe.peMachine));
        if (!machine.empty()) j.Key(L"peMachineName").Value(machine);
    }
    if (r.probe.imageSizeKnown) {
        j.Key(L"imageSize").Value(static_cast<long long>(r.probe.imageSize));
    }
    if (r.probe.subsystemKnown) {
        const std::wstring name = SubsystemName(static_cast<uint16_t>(r.probe.subsystem));
        j.Key(L"subsystem").Value(name.empty() ? L"unknown" : name);
    }
    if (r.probe.subsystemVersionKnown) {
        wchar_t buf[16];
        swprintf(buf, 16, L"%u.%u", r.probe.subsystemMajor, r.probe.subsystemMinor);
        j.Key(L"subsystemVersion").Value(buf);
        j.Key(L"subsystemVersionIsSupportClaim").Value(false);
    }
    j.Key(L"linkerVersion").Value(r.probe.linkerVersion.empty() ? L"unknown"
                                                                : r.probe.linkerVersion);
    j.End();
}

}  // namespace

std::wstring RenderEnvJson(const EnvReport& r, bool verbose, bool quiet) {
    Json j;
    j.Obj();
    // 契约标识只属于这两份文档。普通截图 JSON 不写这些，也不因为这里加了版本号而变。
    j.Key(L"contract").Value(r.contract);
    j.Key(L"contractVersion").Value(static_cast<long long>(r.contractVersion));

    j.Key(L"program").Obj();
    j.Key(L"name").Value(r.programName);
    j.Key(L"binary").Value(r.binaryName);   // 只有文件名，不含目录
    j.Key(L"version").Value(r.version);
    j.Key(L"arch").Value(r.arch);
    j.Key(L"buildId").Value(r.buildId);
    j.End();

    j.Key(L"os").Obj();
    j.Key(L"known").Value(r.os.known);
    if (r.os.known) {
        j.Key(L"major").Value(static_cast<long long>(r.os.major));
        j.Key(L"minor").Value(static_cast<long long>(r.os.minor));
        j.Key(L"build").Value(static_cast<long long>(r.os.build));
    } else {
        j.Key(L"major").Value(L"unknown");
        j.Key(L"minor").Value(L"unknown");
        j.Key(L"build").Value(L"unknown");
    }
    j.Key(L"declaredMinBuild").Value(static_cast<long long>(r.declaredMinBuild));
    j.Key(L"encoderMinBuild").Value(static_cast<long long>(r.encoderMinBuild));
    j.Key(L"testedMinBuild").Value(static_cast<long long>(r.verifiedOsBuild));
    j.Key(L"testedArch").Value(r.verifiedArch);
    j.Key(L"matchesTestedEnvironment").Value(TriOrNull(r.matchesVerifiedEnv));
    j.End();

    j.Key(L"session").Obj();
    WriteTriState(j, L"attachedToConsoleSession", r.consoleAttached);
    WriteTriState(j, L"remoteSession", r.remoteSession);
    WriteTriState(j, L"displayTopology", r.displayTopology);
    if (r.monitorCountKnown) j.Key(L"monitors").Value(static_cast<long long>(r.monitorCount));
    else j.Key(L"monitors").Value(L"unknown");
    WriteTriState(j, L"elevated", r.elevated);
    // 这一条是推出来的（会话号 + 屏幕拓扑），本查询没有真去弹一个框：名字里就写着 expected，
    // 免得调用方读成"已经确认过那里有个人"。
    WriteTriState(j, L"consentDialogExpected", r.consentDialogExpected);
    j.Key(L"consentDialogProbed").Value(false);
    j.End();

    j.Key(L"authorization").Obj();
    j.Key(L"yesFlag").Value(L"--yes");
    // 一句话把 --yes 的适用范围写死：只免"窗口自己的画面"那一级，桌面像素那一级永远问人。
    j.Key(L"yesSkips").Value(L"window-content");
    j.Key(L"desktopPixelsAlwaysAsk").Value(true);
    j.Key(L"unregisteredPathScope").Value(kScopeDesktop);   // 说不清来路 = 按桌面处理，宁可多问
    j.Key(L"paths");
    WriteConsentPaths(j, r.consentPaths);
    j.End();

    j.Key(L"backends");
    WriteBackends(j, r.backends);
    j.Key(L"formats");
    WriteFormats(j, r.formats);
    WriteStringArray(j, L"autoChainWindow", r.autoChainWindow);
    WriteStringArray(j, L"autoChainScreen", r.autoChainScreen);

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
    j.End();

    // 隐私自述：这几条是本查询真实遵守的规矩，写出来让调用方不必读源码就能核对；
    // 下面那一段 probes 与构建标识里也没有用户名、环境变量与绝对路径。
    j.Key(L"privacy").Obj();
    j.Key(L"capturesScreen").Value(r.privacyCapturesScreen);
    j.Key(L"showsDialog").Value(r.privacyShowsDialog);
    j.Key(L"uploads").Value(r.privacyUploads);
    j.Key(L"enumeratesUserFiles").Value(r.privacyEnumeratesUserFiles);
    j.Key(L"readsEnvironmentVariables").Value(r.privacyReadsEnvVars);
    j.Key(L"includesUsernames").Value(r.privacyIncludesUsernames);
    j.Key(L"includesPaths").Value(r.privacyIncludesPaths);
    j.End();

    // --diagnostics 的本职是"出问题后要提交的那份东西"，所以构建标识明细是默认段；
    // --capabilities 只在 --verbose 时展开它，平时只给 program.buildId 那一个数。
    if (r.kind == EnvQueryKind::kDiagnostics || verbose) WriteBuildDetail(j, r);

    if (verbose) WriteProbes(j, r);

    if (!quiet) WriteStringArray(j, L"caveats", r.caveats);
    j.End();
    return j.Str();
}

}  // namespace ecapture
