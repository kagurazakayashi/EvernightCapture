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
#include "CursorControl.h"   // 光标能力登记表（cursor.paths 那一段逐条写它，不另判一次）
#include "HdrColor.h"        // HDR 能力登记表（color.paths 那一段逐条写它，不另判一次）
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
    // 本进程那一档强制完整性级别：与 TokenElevation 同一颗令牌上问出来的另一件事，
    // 只答级别本身，不判任何一条通道能不能用（见 src/ProcessIntegrity.h）。
    p.integrity = ProbeProcessIntegrity(&p.integrityProbeError);
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
    r.integrityLevel = IntegrityName(probe.integrity);
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
    // --roi 的上限与解析层同一份数字（EnvReport.h 的注释就钉着这条不变量）
    r.limits.roiMaxValue = cli_limits::kRoiMaxValue;

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

    // ---- 光标（--cursor）----
    // 这一段回答的是"这台机器上，把光标画进画面 / 不画进画面这件事，各条路线能不能说准"。
    // 三种取值从选项目录里读那一份（同一处数字与形状，这里不抄第二份表），默认值明确写出来。
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"cursor") r.cursor.values = info.allowedValues;
    }
    const Capability cursorControl = AssessWgcCursorControl(probe.os);
    r.cursor.control.api = L"IGraphicsCaptureSession2::IsCursorCaptureEnabled";
    // compiled 说的是"这个构建里真的在用这条接口"（src/CaptureWgc.cpp 的 ApplyCursorControl），
    // 与"这台机器让不让用"（下面那条 status）是两件事，各自一个字段。
    r.cursor.control.compiled = true;
    r.cursor.control.minBuild = cursorControl.minBuild;
    r.cursor.control.status = StatusOfSupport(cursorControl.support, &r.cursor.control.reason);
    r.cursor.control.verifiedOnThisMachine = r.matchesVerifiedEnv;
    for (const auto& e : RegisteredCursorPaths()) {
        CursorPathReport p;
        p.path = e.path;
        p.capability = CursorCapabilityName(e.capability);
        p.reason = e.reason;
        // 两个要求各自的**三值**下场。这里不重算任何判据，只是把那张表的三种登记翻译成
        // yes / no / unknown：问不出来就是 unknown，绝不折成任何一边。
        switch (e.capability) {
            case CursorCapability::kSettable: {
                // 有开关：能不能兑现全看这台机器的版本给不给得了那个开关。
                const Tri t = cursorControl.support == Support::kOk   ? Tri::kYes
                                : cursorControl.support == Support::kBelow ? Tri::kNo
                                                                        : Tri::kUnknown;
                p.includeState = TriName(t);
                p.excludeState = TriName(t);
                break;
            }
            case CursorCapability::kExcludesCursor:
                // 来源像素里没有光标：exclude 因此照实成立，include 因此是**做不到**
                //（本工具不画光标，也不拿桌面像素那条高风险路线去"碰运气"）。
                p.includeState = TriName(Tri::kNo);
                p.excludeState = TriName(Tri::kYes);
                break;
            case CursorCapability::kPointerStateUnverified:
                // 桌面复制那两条：两格都是 **no**，而且是查过来源之后说出口的 no ——
                // 这一问在这条路线上根本没有答案（来源可能已经把指针画在那幅桌面图像上，
                // 而它没有可读回的开关，本工具也不取指针元数据、不修图像）。
                // 与下面未登记那条的 unknown 分开：那一条是"没查过"，这一条是"已知保证不了"
                //（src/CursorControl.h 规矩 5；调用方按 capability 那一个字段分得更细）。
                p.includeState = TriName(Tri::kNo);
                p.excludeState = TriName(Tri::kNo);
                break;
            case CursorCapability::kUnregistered:
                // 没登记：两条都不敢声称。新增一条通道忘了登记就在这里现形，而不是被当成默认符合。
                p.includeState = TriName(Tri::kUnknown);
                p.excludeState = TriName(Tri::kUnknown);
                break;
        }
        r.cursor.paths.push_back(std::move(p));
    }

    // ---- HDR 色彩处理（--hdr）----
    // 这一段回答的是"这个构建带不带得回广色域帧、tone mapping 怎么做、在哪些路径上成立"。
    // 三种取值从选项目录里读那一份（不抄第二份表），默认值明确写出来。
    for (const auto& info : OptionCatalog()) {
        if (info.name == L"hdr") r.hdr.values = info.allowedValues;
    }
    // compiled：这个 exe 里真的实现了广色域采集 + 浮点 tone mapping（src/HdrColor.cpp），
    // 而**兑现得了显式 tonemap/refuse 的路径只有 WGC 那两条**：桌面复制那一条登记成
    // wide_gamut_unverified（它仍用 DuplicateOutput()，采集前不问显示色彩空间），逐条写在下面
    // paths 那一段的 honorsExplicitPolicy 里，另外配 caveat 那一条同源边界。
    r.hdr.compiled = true;
    // status：这份只读查询**不去问那块屏此刻是不是 HDR 模式**（那要开一次 DXGI 输出的 GetDesc1，
    // 与"不靠实际探测能力"这条规矩相抵）。所以只要本机有可用显示拓扑就是 unverified（不作断言），
    // 没有拓扑（无图形会话）时才是 unavailable。绝不因为"编译支持"就写 available。
    if (probe.displayTopology == Tri::kNo) {
        r.hdr.status = CapStatus::kUnavailable;
        r.hdr.reason = cap_reason::kNoDisplayTopology;
    } else if (probe.displayTopology == Tri::kUnknown) {
        r.hdr.status = CapStatus::kUnverified;
        r.hdr.reason = cap_reason::kDisplayTopologyUnknown;
    } else {
        r.hdr.status = CapStatus::kUnverified;
        r.hdr.reason = cap_reason::kHdrDisplayModeNotProbed;
    }
    // verifiedOnThisMachine 恒为 no：本项目没有能开 HDR 的显示器，tone mapping 的数学离线判过，
    // 但"真在一幅 HDR 帧上跑通"没有实测过——写 yes 或 unknown 都会把这件事说歪。
    r.hdr.verifiedOnThisMachine = Tri::kNo;
    // 那条曲线的名字与它适用的那三种来源。这一句**不是**"这种布局就按这种 HDR 解"的对照表：
    // PQ / HLG 那两条只在调用方拿得出可靠的输出色彩空间时才适用（DXGI 不把传递函数放进像素
    // 格式里），而那一句在本构建里没有人给得出，所以 10 位包一律是不可核实，见
    // caveat::kHdrLayoutIsNotColorSpace。
    r.hdr.toneMapping = L"fixed_extended_reinhard_scrgb_pq_hlg";
    r.hdr.floatIntermediateFrame = L"per_pixel_registers";
    r.hdr.encoderOutput = L"sdr_bgra8";
    for (const auto& e : RegisteredHdrPaths()) {
        HdrPathReport p;
        p.path = e.path;
        p.capability = HdrCapabilityName(e.capability);
        p.reason = e.reason;
        // 与闸门同一个判据（src/HdrColor.h 的 HdrPathHonorsPolicy），不在这里另写一份通道名单。
        p.honorsExplicitPolicy = HdrPathHonorsPolicy(e.path);
        r.hdr.paths.push_back(std::move(p));
    }

    // auto 的两条链：版本那一道判据与真去截图时用的**同一条**（GateChannels），所以"查询里给的链"
    // 与那次截图没有被版本筛歪的可能。这里**不**叠光标与 HDR 那两道闸门：这一份查询没有请求上下文
    //（它不收 --cursor / --hdr），报的是"这台机器给得出哪些路线"，绝不冒充"已经按某一次具体请求
    // 筛过了"。那两道闸门会不会再收窄这条链，逐条看得见：cursor.paths / color.paths 两段各自写着
    // 每条路径兑现得了哪一种要求，而真去截图那一次 -v 的 input.captureChain 才是三道闸门串起来的答案。
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

    // ---- 截图历史归档：这一段只说**规则** ----
    // 每一条取值都是这套实现自己写死的那几件事（判据本体在 src/HistoryArchive.h/.cpp），所以这里
    // 不去问环境、不去建目录、也不去试写：一次只读查询不能因为"想看看那里能不能写"就多出一个
    // 目录，更不能承诺某一次落盘必然成功。字符串里刻意不含反斜杠与盘符 —— 这份文档的隐私规矩是
    // "不输出任何绝对路径"，定位规则用相对程序目录的写法表达。
    r.history.enabledByDefault = true;
    r.history.relativeTo = L"executable-directory";
    r.history.location = L"history/YYYY-MM-DD/";
    r.history.naming = L"YYYYMMDD-HHMMSS-<pid>-<token>-<seq>.<ext>";
    r.history.source = L"same-encoded-bytes";
    r.history.commit = L"exclusive-create";
    r.history.created = L"after-first-delivered-image";
    r.history.retention = L"never-pruned-automatically";
    r.history.partialSuccessExit = EX_CAPTURE_FAILED;   // 主图已交付、副本没落地：已交付 + 有错误

    // ---- caveats：把"这份报告没说过什么"写出来，免得调用方把 available 读成保证 ----
    r.caveats.push_back(caveat::kNoCapture);
    r.caveats.push_back(caveat::kNoDialog);
    r.caveats.push_back(caveat::kNotAGuarantee);
    r.caveats.push_back(caveat::kDeviceNotPredicted);
    r.caveats.push_back(caveat::kEncoderUnprobed);
    r.caveats.push_back(caveat::kSessionInferred);
    // 光标那三条边界恒在：cursor 那一段说的是"设置与来源"这两层，不是像素；本工具
    // 从不动指针形状、也从不事后抹光标；而桌面复制那两条连"来源没有光标"这一层都撑不起
    //（官方说明允许指针已经画在那幅桌面图像上），所以那两条在 cursor.paths 里是
    // pointer_state_unverified + include/exclude 两个 no。写在这里是为了调用方不必读源码
    // 就看得见边界与各条路线的下场。
    r.caveats.push_back(caveat::kCursorSettingNotPixels);
    r.caveats.push_back(caveat::kPointerNeverComposited);
    r.caveats.push_back(caveat::kDuplicationPointerUnprovable);
    // HDR 那两条边界恒在：这一份查询没去问显示此刻是不是 HDR 模式，而本项目从没在 HDR 帧上实测过
    // tone mapping（只在离线用已知色块与梯度判过数学）；且 HDR 一律被映射成 8 位 SDR 再编码交付。
    r.caveats.push_back(caveat::kHdrToneMappingUnverified);
    r.caveats.push_back(caveat::kHdrOutputIsSdr);
    // 这一条与 color.paths 那一段逐路径的 honorsExplicitPolicy 同源：显式要过 tonemap/refuse 时，
    // 兑现得了的只有 WGC 那两条路径，桌面复制那两条本构建没实现那几步（所以会被摘出 auto 链）。
    r.caveats.push_back(caveat::kHdrPolicyWgcOnly);
    // 这一条守着 color.toneMapping 那一句被读歪的可能：那张曲线表只在**给了可靠输出色彩空间**
    // 的时候适用，本构建不从像素布局倒推色彩空间（10 位包不等于 PQ）。
    r.caveats.push_back(caveat::kHdrLayoutIsNotColorSpace);
    // 历史那一段（history）说的是规则，不是这一次的下场：这份查询没有去试过写那个位置，也没有
    // 为"看看能不能建目录"建过任何东西。某一张图的副本有没有落地，看的是截图结果里
    // images[].history 那一格与同码的那条 errors 记录。
    r.caveats.push_back(caveat::kHistoryWritabilityNotProbed);
    if (!probe.buildIdKnown) r.caveats.push_back(caveat::kBuildIdUnavailable);
    if (r.matchesVerifiedEnv == Tri::kNo) r.caveats.push_back(caveat::kNotTestedHere);
    if (r.matchesVerifiedEnv == Tri::kUnknown) r.caveats.push_back(caveat::kTestedEnvUnknown);
    if (probe.os.known == false) r.caveats.push_back(caveat::kOsUnverifiable);
    if (topologyAbsent) r.caveats.push_back(caveat::kTopologyAbsent);
    if (topologyUnknown) r.caveats.push_back(caveat::kTopologyUnverifiable);
    if (probe.remoteSession == Tri::kYes) r.caveats.push_back(caveat::kRemoteSession);
    if (r.consentDialogExpected == Tri::kNo) r.caveats.push_back(caveat::kDesktopNeedsDialog);
    if (probe.elevated == Tri::kNo) r.caveats.push_back(caveat::kNotElevated);
    // 低于中完整性时把这一档报出来：它解释的是"为什么这一步系统不让"，不是一条通道判决。
    // 问不出来（unknown）时一条都不加，免得把"没答案"写成"被降级了"。
    if (IntegrityBelowMedium(probe.integrity)) r.caveats.push_back(caveat::kLowIntegrity);
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

// 字符串数组那段在本文件更靠后的位置定义，这里先声明（WriteCursor 要写 values 那三条取值）。
// WriteStringArray 在本文件更靠后的位置定义，这里先声明一次（WriteCursor 要写 values 那三条取值）。
void WriteStringArray(Json& j, const wchar_t* key, const std::vector<std::wstring>& items);

// --cursor 那一段：默认值、三种取值、那条开关在本机的三态、以及每条路径各能做到什么。
// 三个字段各说一件事（compiled / status / verifiedOnThisMachine），与 backends 那一段同一种写法；
// includeState / excludeState 是三值的，问不出来就写 unknown，不折成 yes 也不折成 no。
void WriteCursor(Json& j, const EnvCursorReport& cursor) {
    j.Obj();
    j.Key(L"option").Value(cursor.option);
    // 默认值明确写在这里：不给这条选项 = default = 本工具对光标一个字都不改，
    // 而显式写 --cursor default 是"照通道默认交回，但把读到的状态报给我"。
    j.Key(L"default").Value(cursor.defaultValue);
    WriteStringArray(j, L"values", cursor.values);
    j.Key(L"switch").Obj();
    j.Key(L"api").Value(cursor.control.api);
    j.Key(L"compiled").Value(cursor.control.compiled);
    j.Key(L"status").Value(CapStatusName(cursor.control.status));
    j.Key(L"reason").Value(cursor.control.reason);
    j.Key(L"minBuild").Value(static_cast<long long>(cursor.control.minBuild));
    j.Key(L"verifiedOnThisMachine").Value(TriOrNull(cursor.control.verifiedOnThisMachine));
    j.End();
    j.Key(L"paths");
    j.Arr();
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
    // 本工具对指针动手的两问恒为 never：既不把桌面复制那份独立的指针元数据合成进帧，
    // 也不事后抹掉已经画进帧里的光标。这两条要与 cursor.paths 那一段一起读才完整：
    // "exclude" 在有开关的那条说的是那次设置，在来源没有光标的那几条说的是来源，
    // 而桌面复制那两条两样都不是（所以那里 include/exclude 两格都是 no）。
    j.Key(L"pointerShapeCompositing").Value(cursor.pointerCompositing);
    j.Key(L"pixelRetouching").Value(cursor.pixelRetouching);
    j.End();
}

// --hdr 那一段：默认值、三种取值、这条路线在本机的三态、每条路径带不带得回广色域帧，
// 以及 tone mapping / 浮点中间帧 / 编码输出三件"做法"的自述。三件事照旧分开写
// （compiled / status / verifiedOnThisMachine）；verifiedOnThisMachine 恒 no（本项目没有 HDR 屏）。
void WriteHdr(Json& j, const EnvHdrReport& hdr) {
    j.Obj();
    j.Key(L"option").Value(hdr.option);
    j.Key(L"default").Value(hdr.defaultValue);
    WriteStringArray(j, L"values", hdr.values);
    j.Key(L"compiled").Value(hdr.compiled);
    j.Key(L"status").Value(CapStatusName(hdr.status));
    j.Key(L"reason").Value(hdr.reason);
    j.Key(L"verifiedOnThisMachine").Value(TriOrNull(hdr.verifiedOnThisMachine));
    // 做法的自述：映射曲线叫什么、有没有分配整幅浮点帧、编码器拿到的是什么。写出来让调用方
    // 不必读源码就知道"HDR 处理"的产物永远是一张 8 位 SDR 图，而不是把 FP16 硬塞进编码器。
    j.Key(L"toneMapping").Value(hdr.toneMapping);
    j.Key(L"floatIntermediateFrame").Value(hdr.floatIntermediateFrame);
    j.Key(L"encoderOutput").Value(hdr.encoderOutput);
    j.Key(L"paths");
    j.Arr();
    for (const auto& p : hdr.paths) {
        j.Obj();
        j.Key(L"path").Value(p.path);
        j.Key(L"capability").Value(p.capability);
        j.Key(L"reason").Value(p.reason);
        // 这一条路径兑不兑现得了**显式**的 tonemap/refuse。写成一个布尔而不是靠调用方猜 capability
        // 的语义：false 有两种原因（结构上只带 8 位 / 本构建没实现那几步），capability 那一个键
        // 分得开，而这个键直接回答"我这次的要求会不会被回退链丢掉"。
        j.Key(L"honorsExplicitPolicy").Value(p.honorsExplicitPolicy);
        j.End();
    }
    j.End();
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

// 截图历史归档那一段。每个键说的都是"这套实现是怎么规定的"，没有一个键是本次落盘的断言；
// 字符串里不出现反斜杠与盘符（这份文档的隐私规矩：不输出任何绝对路径）。
void WriteHistory(Json& j, const EnvHistoryReport& h) {
    j.Obj();
    j.Key(L"enabledByDefault").Value(h.enabledByDefault);
    j.Key(L"relativeTo").Value(h.relativeTo);
    j.Key(L"location").Value(h.location);
    j.Key(L"naming").Value(h.naming);
    // 副本的字节来源：与主交付**同一份**已编码缓冲。这一条要说得出口，因为"另存一份"如果靠
    // 重拍一次、重新编码一次或去读主输出文件，就不是同一张图了（而且硬链接根本不独立）。
    j.Key(L"source").Value(h.source);
    j.Key(L"commit").Value(h.commit);
    j.Key(L"created").Value(h.created);
    j.Key(L"retention").Value(h.retention);
    j.Key(L"uploads").Value(h.uploads);
    j.Key(L"backgroundPruning").Value(h.backgroundPruning);
    // 这一条恒 false：这份只读查询没去试过写那个位置，也不预测某一次落盘必然成功。
    j.Key(L"writabilityProbed").Value(h.writabilityProbed);
    // 主图已交付而副本没落地时的那一种部分成功给哪一个退出码（不是 0，也不是"什么都没写"）。
    j.Key(L"partialSuccessExit").Value(static_cast<long long>(h.partialSuccessExit));
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
        j.Obj().Key(L"question").Value(L"tokenIntegrityLevel")
            .Key(L"known").Value(r.probe.integrity != Integrity::kUnknown)
            .Key(L"answer").Value(r.integrityLevel)
            .Key(L"belowMedium").Value(IntegrityBelowMedium(r.probe.integrity))
            .Key(L"source").Value(L"GetTokenInformation(TokenIntegrityLevel)");
        // 问不出来时把那一步的 GetLastError 一起交回：0 而答案仍是 unknown，意味着系统答了
        // 一个本工具没登记过的标签取值，与"那两步调用本身失败"是两件不同的事。
        if (r.probe.integrity == Integrity::kUnknown) {
            j.Key(L"win32").Value(static_cast<long long>(r.probe.integrityProbeError));
        }
        j.End();
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
    // 本进程的强制完整性级别（ASCII token，不随 --lang 变）。低于 medium 时 caveats 里
    // 另有一条 process_integrity_below_medium 说明这一档影响的是哪几件事。
    j.Key(L"integrityLevel").Value(r.integrityLevel);
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
    // 光标这一段紧跟 backends：它说的就是"这几条路线各自对光标这件事说得到哪一层"，
    // 与 autoChain 那两条一起读，调用方在决定 --capture / --cursor 之前就能问清楚，
    // 不必先截一张再去猜"怎么图里没光标 / 怎么多了光标"。
    j.Key(L"cursor");
    WriteCursor(j, r.cursor);
    // HDR 这一段紧跟 cursor：它说的就是"这几条路线各自带不带得回广色域帧、HDR 被怎么处理"。
    j.Key(L"color");
    WriteHdr(j, r.hdr);
    j.Key(L"formats");
    WriteFormats(j, r.formats);
    WriteStringArray(j, L"autoChainWindow", r.autoChainWindow);
    WriteStringArray(j, L"autoChainScreen", r.autoChainScreen);
    // 历史归档这一段紧跟 autoChain：调用方（含 AI）在决定"这一张要写到哪个名字"之前就该知道
    // 程序自己那份目录里还会另存一份持久副本，以及它的定位规则与保留策略。这一段全是**规则**，
    // 没有一个字段是"这一次写成功了"那种断言（那一句在截图结果的 images[].history 里）。
    j.Key(L"history");
    WriteHistory(j, r.history);

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
