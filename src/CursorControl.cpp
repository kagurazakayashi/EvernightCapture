#include "CursorControl.h"

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureScope.h"   // WindowPathOf / ScreenPathOf：通道落到哪条内部路径，只有一份答案
#include "Lang.h"

namespace ecapture {
namespace {

// 一条通道被摘掉的原因。token 取自登记表（ASCII，不随 --lang 变，调用方按它分支），
// 版本不够那一种额外带上这道门槛的数字。
struct CursorDrop {
    std::wstring reason;
    uint32_t minBuild = 0;
};

std::wstring BriefChannels(const std::vector<CaptureMethod>& items) {
    std::wstring s;
    for (const CaptureMethod m : items) {
        if (!s.empty()) s += L", ";
        s += CaptureMethodName(m);
    }
    return s;
}

std::wstring BriefReasons(const std::vector<CursorDrop>& drops) {
    std::wstring s;
    for (const CursorDrop& d : drops) {
        if (!s.empty()) s += L", ";
        s += d.reason;
        if (d.minBuild != 0) s += L":" + std::to_wstring(d.minBuild);
    }
    return s;
}

// 这一条通道能不能兑现这一次的光标要求（结构 + 本机版本两层，都在这里判）。
// 落不下时 *drop 给出原因；判据一律走那张路径登记表，不在这里另写一份"哪条通道有光标开关"。
bool ChannelHonorsCursor(CaptureMethod method, bool screenMode, CursorMode mode,
                         const OsVersion& os, const Capability& control, CursorDrop* drop) {
    const wchar_t* path = screenMode ? ScreenPathOf(method) : WindowPathOf(method);
    drop->reason = CursorReasonOfPath(path);
    drop->minBuild = 0;

    switch (CursorCapabilityOfPath(path)) {
        case CursorCapability::kSettable:
            // 有开关，但开关本身还有一道版本门槛。版本问不出来时**不**按版本筛
            //（与 GateChannels 同一条规矩：问不出来不等于不支持）—— 这时照旧交给这条通道，
            // 由它在取帧之前把接口问一次，问不到就交回 capture.cursor_unverifiable。
            if (os.known && control.support == Support::kBelow) {
                drop->reason = cursor_reason::kOsBelowMin;
                drop->minBuild = control.minBuild;
                return false;
            }
            return true;

        case CursorCapability::kExcludesCursor:
            // 来源像素里根本没有光标：exclude 因此照实成立，include 因此做不到。
            // 做不到的那一条**不是**"那就换一条会读桌面的通道"—— 那几条同样没有光标，
            // 而换来的风险是多拍一份没人批准过的画面。
            return mode != CursorMode::kInclude;

        case CursorCapability::kUnregistered:
            // 没登记：include 不敢说做得到，exclude 也不敢说"这张图里真的没有光标"。
            // 新增一条通道而忘了在登记表里加一行，下场是被摘掉 + 一条写清楚原因的 note，
            // 而不是被当成"默认就符合要求"（默认从严，与 CaptureScope 那张表同一条规矩）。
            return false;
    }
    return false;
}

}  // namespace

CursorChainGate FilterChainForCursor(const std::vector<CaptureMethod>& chain,
                                     const CursorRequest& request, bool screenMode,
                                     const OsVersion& os) {
    CursorChainGate gate;
    gate.chain = chain;

    // 没写 --cursor，或写成 default：一条通道都不筛、一条 note 都不发。
    // 这条选项存在之前的行为就是"各通道照自己的默认交回"，默认值必须真的不动任何东西。
    if (!request.given || request.mode == CursorMode::kDefault) return gate;
    if (chain.empty()) return gate;   // 上一步（版本闸门）已经给过错误了，这里不再补一条

    const Capability control = AssessWgcCursorControl(os);
    std::vector<CaptureMethod> kept;
    std::vector<CaptureMethod> dropped;
    std::vector<CursorDrop> drops;

    for (const CaptureMethod m : chain) {
        CursorDrop drop;
        if (ChannelHonorsCursor(m, screenMode, request.mode, os, control, &drop)) {
            kept.push_back(m);
            continue;
        }
        dropped.push_back(m);
        drops.push_back(std::move(drop));
    }

    const std::wstring wanted = CursorModeName(request.mode);
    for (size_t i = 0; i < dropped.size(); ++i) {
        Diagnostic d;
        d.code = codes::kNoteCursorChannelSkipped;
        d.message = Msgf(L"note.cursor_channel_skipped", CaptureMethodName(dropped[i]), wanted,
                         drops[i].reason + (drops[i].minBuild != 0
                                                ? L":" + std::to_wstring(drops[i].minBuild)
                                                : std::wstring()));
        d.option = L"--cursor";
        d.value = wanted;
        d.backend = CaptureMethodName(dropped[i]);
        d.stage = stages::kCapture;
        gate.notes.push_back(std::move(d));
    }

    if (!kept.empty()) {
        gate.chain = std::move(kept);
        return gate;
    }

    // 一条都不剩：这一次一个像素都不取、确认框也不弹。绝不"那就照能截的那几条先交出再说"——
    // 那样交回去的那张图在光标这件事上根本不是用户要的那一种。
    Diagnostic d;
    d.code = codes::kEnvCursorUnsupported;
    d.message = Msgf(L"env.cursor_unsupported", wanted, BriefChannels(dropped),
                    BriefReasons(drops));
    d.option = L"--cursor";
    d.value = wanted;
    d.backend = BriefChannels(chain);
    d.hint = Msg(L"env.cursor_unsupported_hint");
    d.stage = stages::kCapture;
    gate.error = std::move(d);
    gate.chain.clear();
    return gate;
}

ChannelGate GateCaptureChain(CaptureMethod requested, bool screenMode, const OsVersion& os,
                             const CursorRequest& cursor) {
    // 版本那一道先走：它说的是"这条通道在这台机器上根本用不了"，与光标无关，
    // 而那条错误优先于这里（一次请求只交回一条最靠前能说清楚的下一步）。
    ChannelGate gate = GateChannels(requested, screenMode, os);
    if (!gate.error.code.empty()) return gate;

    const CursorChainGate cursorGate = FilterChainForCursor(gate.chain, cursor, screenMode, os);
    for (const Diagnostic& n : cursorGate.notes) gate.notes.push_back(n);
    if (!cursorGate.error.code.empty()) {
        gate.error = cursorGate.error;
        gate.chain.clear();
        return gate;
    }
    gate.chain = cursorGate.chain;
    return gate;
}

}  // namespace ecapture
