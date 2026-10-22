// 本机版本门槛的判据本体（平台无关）。
//
// 这一份只回答"给定一个 OsVersion，这条通道 / 这个构建够不够门槛"以及"哪几条通道组成
// auto 链"。真机版本探测（RtlGetVersion）搬到了 src/platform/windows/WinSystemCompat.cpp：
// 共用层不再 include windows.h，注入一个假 OsVersion 就能离线逐条判边界（tests/compat_state.cpp）。
#define WIN32_LEAN_AND_MEAN
#include "SystemCompat.h"

#include <cstdint>
#include <string>
#include <vector>

#include "Lang.h"

namespace ecapture {
namespace {

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
        // X11 的窗口存储与 Windows 版本无关：那一列门槛在这里不适用（0 = 没有门槛）。
        case CaptureMethod::kXComposite:
        case CaptureMethod::kX11Desktop:
        case CaptureMethod::kAuto: minBuild = 0; break;  // X11 后端与"整条链"本身没有版本门槛
    }
    return {Judge(os, minBuild), minBuild};
}

Capability AssessWgcCursorControl(const OsVersion& os) {
    return {Judge(os, os_floor::kWgcCursor), os_floor::kWgcCursor};
}

const std::vector<CaptureMethod>& AutoChain(bool screenMode) {
    // 屏幕目标没有"某个窗口自己的画面"可截，所以链与窗口模式不同（见 --capture 的说明）。
#if defined(_WIN32)
    static const std::vector<CaptureMethod> kWindow = {
        CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail, CaptureMethod::kPrintWindow,
        CaptureMethod::kBitBlt};
    static const std::vector<CaptureMethod> kScreen = {
        CaptureMethod::kWgc, CaptureMethod::kDuplication, CaptureMethod::kBitBlt};
    return screenMode ? kScreen : kWindow;
#else
    // Linux 的 auto 按**目标种类**分开：窗口目标只读窗口自己的重定向存储；屏幕 / 全桌面 /
    // 明确区域目标只走 X11 根窗口后端。两条链不互相回退 —— 尤其不允许窗口内容失败后
    // 悄悄改读桌面像素。
    static const std::vector<CaptureMethod> kWindow = {CaptureMethod::kXComposite};
    static const std::vector<CaptureMethod> kScreen = {CaptureMethod::kX11Desktop};
    return screenMode ? kScreen : kWindow;
#endif
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

#if !defined(_WIN32)
    // Windows 版本门槛是 Windows 那一列的判据：Linux 上问不出、也不该问。这里不写
    // "问不出来"的 note（那会让每一次 Linux 截图都带一条与本机无关的提示），直接按
    // 本构建编进来的那一条通道给链。显式点名的通道若没有实现，运行期会交回
    // capture.unsupported —— 不在这里替它换一条。
    (void)os;
    // 显式点名的通道不能跨目标种类：xcomposite 只拍窗口，x11desktop 只拍根窗口。
    // auto 已经按 screenMode 选好链；显式错配在取帧 / 弹框之前就报，绝不偷换后端。
    if (requested != CaptureMethod::kAuto) {
        const bool windowOnly = requested == CaptureMethod::kXComposite;
        const bool screenOnly = requested == CaptureMethod::kX11Desktop;
        if ((windowOnly && screenMode) || (screenOnly && !screenMode)) {
            Diagnostic d = EnvBase(codes::kUnsupported, L"cap.unsupported_for_screen_hint");
            d.message = windowOnly
                            ? Msgf(L"cap.unsupported_for_screen", CaptureMethodName(requested))
                            : Msg(L"cap.x11.desktop_window_unsupported");
            d.value = CaptureMethodName(requested);
            d.backend = CaptureMethodName(requested);
            gate.error = std::move(d);
            return gate;
        }
    }
    gate.chain = requested == CaptureMethod::kAuto ? full
                                                   : std::vector<CaptureMethod>{requested};
    return gate;
#else
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
#endif
}

}  // namespace ecapture
