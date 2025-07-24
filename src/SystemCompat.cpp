#include "SystemCompat.h"

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Lang.h"

namespace ecapture {
namespace {

// RtlGetVersion 的函数原型。ntdll.dll 本来就在进程里（它是最初装载的那批之一），
// 所以这里只取函数地址，不 LoadLibrary；那个结构是纯 POD，不依赖任何 SDK 版本宏。
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);

// 那个函数的成功返回值是 NTSTATUS 的 0。不引 STATUS_SUCCESS：它在 ntstatus.h 里，
// 而这里只需要这一个数。
constexpr LONG kNtSuccess = 0;

// 版本够不够这一道门槛。minBuild=0 表示这一条没有版本门槛，永远够。
Support Judge(const OsVersion& os, uint32_t minBuild) {
    if (minBuild == 0) return Support::kOk;
    if (!os.known) return Support::kUnknown;
    // 只比内部版本：本工具声明的是 x64 桌面版 Windows，那条序列上主版本号与内部版本
    // 一起单调递增（6.3.9600 < 10.0.10240 < 10.0.18362），不存在"主版本更高、内部版本更低"
    // 的机型，所以不必拿 major/minor 再判一次，也就不会在两处各写一套比较规则。
    return os.build >= minBuild ? Support::kOk : Support::kBelow;
}

std::wstring BuildText(const OsVersion& os) {
    if (!os.known) return std::wstring(L"?");
    return std::to_wstring(os.build);
}

// 环境类诊断的公共形状：option 指出这件事与哪个选项有关，stage 一律 capture
// （"取图这一步在这台机器上做不了"）。数字只出现在 message 里，是 ASCII，四语同一份。
Diagnostic EnvBase(const wchar_t* code, const wchar_t* hintKey) {
    Diagnostic d;
    d.code = code;
    d.option = L"--capture";
    d.stage = stages::kCapture;
    if (hintKey) d.hint = Msg(hintKey);
    return d;
}

}  // namespace

OsVersion ProbeOsVersion() {
    // 只问一次：一次运行里版本号不会变，而通道闸门与 -v 的回显两处都要读它。
    static const OsVersion cached = [] {
        OsVersion v;
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return v;
        const RtlGetVersionFn proc = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (!proc) return v;
        OSVERSIONINFOEXW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (proc(&info) != kNtSuccess) return v;
        v.major = static_cast<uint32_t>(info.dwMajorVersion);
        v.minor = static_cast<uint32_t>(info.dwMinorVersion);
        v.build = static_cast<uint32_t>(info.dwBuildNumber);
        v.known = v.build != 0;
        return v;
    }();
    return cached;
}

Capability AssessRuntime(const OsVersion& os) {
    return {Judge(os, os_floor::kEncoder), os_floor::kEncoder};
}

Capability AssessChannel(CaptureMethod method, bool screenMode, const OsVersion& os) {
    (void)screenMode;  // 按窗口与按屏那两条 WGC 路线的下限是同一个数字，暂不分档
    uint32_t minBuild = 0;
    switch (method) {
        case CaptureMethod::kWgc: minBuild = os_floor::kWgc; break;
        case CaptureMethod::kDuplication: minBuild = os_floor::kDuplication; break;
        case CaptureMethod::kPrintWindow:
        case CaptureMethod::kDwmThumbnail: minBuild = os_floor::kPrintWindow; break;
        case CaptureMethod::kBitBlt:
        case CaptureMethod::kAuto: minBuild = 0; break;  // GDI 与"整条链"本身没有版本门槛
    }
    return {Judge(os, minBuild), minBuild};
}

const std::vector<CaptureMethod>& AutoChain(bool screenMode) {
    // 屏幕目标没有"某个窗口自己的画面"可截，所以链与窗口模式不同（见 --capture 的说明）。
    static const std::vector<CaptureMethod> kWindow = {
        CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail, CaptureMethod::kPrintWindow,
        CaptureMethod::kBitBlt};
    static const std::vector<CaptureMethod> kScreen = {
        CaptureMethod::kWgc, CaptureMethod::kDuplication, CaptureMethod::kBitBlt};
    return screenMode ? kScreen : kWindow;
}

Diagnostic EnvironmentError(const OsVersion& os) {
    const Capability cap = AssessRuntime(os);
    if (cap.support != Support::kBelow) return Diagnostic{};

    Diagnostic d = EnvBase(codes::kEnvOsTooOld, L"env.os_too_old_hint");
    d.message = Msgf(L"env.os_too_old", BuildText(os), cap.minBuild);
    d.value = BuildText(os);
    return d;
}

ChannelGate GateChannels(CaptureMethod requested, bool screenMode, const OsVersion& os) {
    ChannelGate gate;
    const std::vector<CaptureMethod>& full = AutoChain(screenMode);

    // 版本问不出来：不按版本筛。整条链照旧交出去，让那一步自己交回真实的错误码，
    // 同时说明白"这一次没有按版本判过"，别让调用方以为判过了（问不出来 ≠ 不支持）。
    if (!os.known) {
        gate.chain =
            requested == CaptureMethod::kAuto ? full : std::vector<CaptureMethod>{requested};
        Diagnostic d = EnvBase(codes::kNoteOsUnverifiable, nullptr);
        d.message = Msg(L"note.os_unverifiable");
        d.value = CaptureMethodName(requested);
        gate.notes.push_back(std::move(d));
        return gate;
    }

    if (requested != CaptureMethod::kAuto) {
        const Capability cap = AssessChannel(requested, screenMode, os);
        if (cap.support == Support::kBelow) {
            Diagnostic d = EnvBase(codes::kEnvChannelUnsupported, L"env.channel_unsupported_hint");
            d.message = Msgf(L"env.channel_unsupported", CaptureMethodName(requested), cap.minBuild,
                             BuildText(os));
            d.value = CaptureMethodName(requested);
            d.backend = CaptureMethodName(requested);
            gate.error = std::move(d);
            return gate;  // chain 为空：一条都不试，也不替用户换成别的通道
        }
        gate.chain = {requested};
        return gate;
    }

    // auto：被版本挡掉的那几条从链里去掉，其余照旧回退。bitblt 那条没有版本门槛，
    // 所以只要整工具那一道下限过了（EnvironmentError 已经判过），链不可能为空。
    for (const CaptureMethod m : full) {
        const Capability cap = AssessChannel(m, screenMode, os);
        if (cap.support == Support::kBelow) {
            Diagnostic d = EnvBase(codes::kNoteChannelUnavailable, nullptr);
            d.message = Msgf(L"note.channel_unavailable", CaptureMethodName(m), cap.minBuild,
                             BuildText(os));
            d.value = CaptureMethodName(m);
            d.backend = CaptureMethodName(m);
            gate.notes.push_back(std::move(d));
            continue;
        }
        gate.chain.push_back(m);
    }
    return gate;
}

}  // namespace ecapture
