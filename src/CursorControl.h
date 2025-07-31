#pragma once
// EvernightCapture - 光标包含与排除（--cursor）的判据本体
//
// 为什么这一层要单独存在，而且按**路径**登记而不是按通道名字：
// "画面里有没有鼠标指针"不是各条通道都能商量的事，它由那条路径的来源像素决定。
// 只有一条通道真的有一个可以设进去、还能读回来核实的开关（wgc 的
// IGraphicsCaptureSession2::IsCursorCaptureEnabled，内部版本 19041 起）；其余几条交回的像素里
// 本来就没有光标 —— PrintWindow 是让窗口自己画到 DC、dwm 读的是重定向位图、bitblt 拷的是屏幕 DC、
// duplication 的桌面帧更是明确把指针当**独立元数据**交回（本工具一次都没有去取它、也没有把它画进
// 帧里）。所以"我请求了排除"与"这张图里真的没有光标"是两件事，必须分开写：
//   requested = 用户要的那一种（default / include / exclude）
//   effective = 这条路径实际交回的那一种（include / exclude / unverified）
//   basis     = 这个结论凭什么（设过并读回 / 只读了当前值 / 这条路径的来源就没有光标 / 那一问没答案）
// 三条各占一个字段，谁也不冒充谁。
//
// 四条规矩（改代码前先对齐这里）：
//
// 1. **不支持就说做不到，绝不静默换后端。** --cursor include 配 printwindow / bitblt /
//    duplication / dwm 在解析期就报 capture.cursor_unsupported + 退出码 1：把一次"要光标"的
//    请求换成一条会读桌面像素的通道，既不会把光标加回来（那几条的来源里根本没有它），
//    又多拍了一份没人批准过的画面。--capture auto 时做不到的那几条从链里摘掉并各留一条
//    note.cursor_channel_skipped；摘到空了就是一条 env.cursor_unsupported，一张都不截。
// 2. **不拿图像修补冒充 API 能力。** 本工具既不画光标也不抹光标：不调
//    GetFramePointerShape / DrawIcon / DrawCursor，也不 SetCursorPoint 之后重画那块像素，
//    更不拿"整幅里找出像光标的形状然后涂掉"那种不可靠的办法交差。判据本体是这条登记表的
//    reason 字段，tests\cursor.ps1 里另有一条源码级守卫（src/ 里出现那几个 API 就红）。
// 3. **默认值不动任何东西。** 没写 --cursor 时（CursorRequest::given = false）一个开关都不碰、
//    结果里那三个键一个都不出现 —— 与这条选项存在之前逐字节相同。显式写 --cursor default 才是
//    "照通道默认交回，但把读到的状态报给我"：这时只读不写。
// 4. **问不出来 ≠ 照我要的办了。** 开关问不到、设不下去、或设完读回来跟所要求的不是同一件事，
//    都是 capture.cursor_unverifiable（7）而这一张**一个像素都不落地**；--cursor default 那一路
//    读不到就写 effective: "unverified" 并留 basis，绝不折成 include 或 exclude（与身份复核
//    "问不出来 ≠ 相同"、capture.roi_unmeasurable 与 capture.monitor_unverifiable 同源）。
//
// 登记表与 src/CaptureScope.cpp 那张"像素来源"表是同一类东西：新增一条通道忘了登记 =
// 更严格（按 kUnregistered 处理：include 做不到、exclude 不敢声称），而不是更松。
// 两份表一致性由 tests\cursor_state.cpp 逐条现场核对（遍历 RegisteredCapturePaths()，
// 每条路径都要在这张表里查得到；每个通道的 WindowPathOf / ScreenPathOf 也要对得上）。

#include <cstdint>
#include <string>
#include <vector>

#include "CaptureScope.h"   // paths:: 那些内部路径名（登记表按它登记，不按通道名）
#include "CliOptions.h"     // CursorMode / CursorRequest / CaptureMethod / Diagnostic / codes::
#include "SystemCompat.h"   // OsVersion / Capability / AssessWgcCursorControl

namespace ecapture {

// 一条取帧路径对"光标在不在画面里"能做到什么。取值只增不改名。
enum class CursorCapability {
    // 有一个真设得进去、也读得回来的开关（目前只有 wgc 那两条：按窗口与按整屏共用同一条会话接口）。
    // 开关本身还有一道版本门槛：见 os_floor::kWgcCursor 与 AssessWgcCursorControl。
    kSettable,
    // 这条路径的来源像素里根本没有光标：没有开关可设，也不需要。
    // exclude 因此是照实成立（basis 写 path_excludes_cursor），include 因此是做不到（不是"那就换一条"）。
    kExcludesCursor,
    // 没登记的名字。include 做不到，exclude **不敢声称**（宁可报无法核实，也不把没查过的事说成查过）。
    kUnregistered,
};

// capability 的机器名（--capabilities 的 cursor.paths 段用它；截图那份 JSON 写的是
// effective/basis 那一组，不写它）。只增不改名。
inline const wchar_t* CursorCapabilityName(CursorCapability capability) {
    switch (capability) {
        case CursorCapability::kSettable: return L"settable";
        case CursorCapability::kExcludesCursor: return L"excludes_cursor";
        case CursorCapability::kUnregistered: return L"unregistered";
    }
    return L"unregistered";
}

// images[].cursorEffective 的取值（机器名，不随 --lang 变）。
namespace cursor_effective {
inline constexpr const wchar_t* kInclude = L"include";        // 这一帧里有光标（开关读过/设过，值是"要画"）
inline constexpr const wchar_t* kExclude = L"exclude";        // 这一帧里没有光标
inline constexpr const wchar_t* kUnverified = L"unverified";  // 那一问没答案；不等于上面任何一种
}  // namespace cursor_effective

// images[].cursorBasis 的取值：上面那个结论**凭什么**。与 effective 一样只增不改名。
namespace cursor_basis {
// wgc：本次按 --cursor 的要求 put 过 IsCursorCaptureEnabled，并且 get 回来与所要求的一致。
// 断言到这一层为止：它说的是"这条会话被设置为把光标画进帧里/不画进帧里"，
// 而**不是**"此刻光标正停在目标窗口上所以图里一定看得见它"（本 SDK 的会话接口没有
// IsCursorVisible 那个只读属性，所以像素级的事本工具不作断言）。
inline constexpr const wchar_t* kSessionSet = L"wgc_session_property_set";
// wgc + --cursor default：一个字节都没改过，只是把这条会话当前的开关值读回来报给你。
inline constexpr const wchar_t* kSessionRead = L"wgc_session_property_read";
// 不是 wgc 的那几条：来源像素本身就没有光标这件事（dwm.screen 是屏幕 DC、duplication 的桌面帧
// 明确不含指针，指针形状是独立元数据而本工具从不合成它）。
inline constexpr const wchar_t* kPathExcludes = L"path_excludes_cursor";
// wgc 但那个开关问不到（接口没实现、调用失败）：这时 effective 只能是 unverified。
inline constexpr const wchar_t* kPropertyUnavailable = L"wgc_cursor_property_unavailable";
}  // namespace cursor_basis

// 每条路径"光标这件事的根据"那一个 ASCII token（进 note.cursor_channel_skipped 的 message
// 与 --capabilities 的 cursor.paths 段；与 message 不同，它不随 --lang 变，调用方按它分支）。
namespace cursor_reason {
// 这条路径有开关可设（版本够不够是另一问，见 os_below_min_build）
inline constexpr const wchar_t* kSessionProperty = L"wgc_session_property";
// 窗口自绘到 DC（printwindow，以及 dwm 主路径读的重定向位图）
inline constexpr const wchar_t* kSelfDrawnSurface = L"window_self_drawn";
// 读的是 DWM 重定向位图那一面（dwm.thumbnail）
inline constexpr const wchar_t* kDwmSurface = L"dwm_redirection_surface";
// 拷屏幕 DC（bitblt 与 dwm 的屏幕退路）：系统指针画在 DC 内容之外
inline constexpr const wchar_t* kScreenDc = L"screen_dc_has_no_pointer";
// 桌面复制的合成分：指针是**独立元数据**，本工具不取它也不合成进帧
inline constexpr const wchar_t* kPointerMetadata = L"pointer_shape_is_separate_metadata";
// 没登记（新增通道忘了登记 = 更严而不是更松）
inline constexpr const wchar_t* kNotRegistered = L"not_registered";
// 本机内部版本低于 os_floor::kWgcCursor，那个开关问不到
inline constexpr const wchar_t* kOsBelowMin = L"os_below_min_build";
}  // namespace cursor_reason

// 登记表的一行。
struct CursorPathEntry {
    const wchar_t* path;
    CursorCapability capability;
    const wchar_t* reason;
};

// 登记表本体（唯一出处）。写在这里而不是 .cpp，是因为解析层（CliOptions.cpp）也要问它，
// 而那些只链 CliOptions.cpp 的离线判据目标不该被拖进取帧公共件。
// 每一项对应 src/CaptureScope.cpp 里那条已登记的内部路径；漏一项 = 该项按 kUnregistered 判。
inline constexpr CursorPathEntry kCursorTable[] = {
    {paths::kWgc, CursorCapability::kSettable, cursor_reason::kSessionProperty},
    {paths::kScreenWgc, CursorCapability::kSettable, cursor_reason::kSessionProperty},
    {paths::kPrintWindow, CursorCapability::kExcludesCursor, cursor_reason::kSelfDrawnSurface},
    {paths::kDwmThumbnail, CursorCapability::kExcludesCursor, cursor_reason::kDwmSurface},
    {paths::kDwmScreen, CursorCapability::kExcludesCursor, cursor_reason::kScreenDc},
    {paths::kBitBltScreen, CursorCapability::kExcludesCursor, cursor_reason::kScreenDc},
    {paths::kScreenBitBlt, CursorCapability::kExcludesCursor, cursor_reason::kScreenDc},
    {paths::kDuplicationFrame, CursorCapability::kExcludesCursor, cursor_reason::kPointerMetadata},
    {paths::kScreenDuplication, CursorCapability::kExcludesCursor, cursor_reason::kPointerMetadata},
};

// 整张登记表（只读视图）：--capabilities 的 cursor.paths 段与离线判据都遍历这一份，
// 不再各自抄一份表。
inline const std::vector<CursorPathEntry>& RegisteredCursorPaths() {
    static const std::vector<CursorPathEntry> table(std::begin(kCursorTable), std::end(kCursorTable));
    return table;
}

// 这条内部路径的光标能力。没登记的名字（含空串与 paths::kUnknown）一律 kUnregistered。
// 逐字符比较，不做任何"看起来像"的猜测 —— 表里写的是通道自己填进 CapturedFrame::path
// 那个机器名，对不上就是没登记，要人去补那一行而不是在这里凑。
inline CursorCapability CursorCapabilityOfPath(const wchar_t* path) {
    if (path) {
        for (const auto& e : kCursorTable)
            if (std::wstring(e.path) == path) return e.capability;
    }
    return CursorCapability::kUnregistered;
}

inline CursorCapability CursorCapabilityOfPath(const std::wstring& path) {
    return CursorCapabilityOfPath(path.c_str());
}

// 这条路径"光标这件事的根据"那一个 token（未登记的写 not_registered）。
inline const wchar_t* CursorReasonOfPath(const wchar_t* path) {
    if (path) {
        for (const auto& e : kCursorTable)
            if (std::wstring(e.path) == path) return e.reason;
    }
    return cursor_reason::kNotRegistered;
}

// 这条**通道**有没有那个可设的开关（结构层面，不看本机版本；auto 不算通道）。这一条与上面那张
// 表必须一致，判据写在 tests\cursor_state.cpp：它对每个通道取 WindowPathOf / ScreenPathOf 再查表
// 核对，所以这里只保留解析层需要的那一句判断，不另立第二套登记表。
inline constexpr bool ChannelHasCursorSwitch(CaptureMethod method) {
    return method == CaptureMethod::kWgc;
}

// 这一次的光标要求由这条通道（结构层面，不看本机版本）做不做得到。
//   include  —— 只有那个开关设得进去的通道能做到（auto 看闸门筛完之后剩什么）。
//   exclude  —— 有开关的那条靠设开关，没开关的那几条靠"来源像素本来就没有光标"，两条都成立；
//               唯一不敢声称的是**没登记**的路径，而那一条由 FilterChainForCursor 挡（它才查得到表）。
//   default  —— 什么都不要求，恒成立。
// 解析层只判 include 那一条：这条判断与本机版本、与目标窗口都无关，所以它在 --dry-run 下也成立。
inline bool CursorRequestPossible(CaptureMethod method, CursorMode mode) {
    if (mode != CursorMode::kInclude) return true;
    return method == CaptureMethod::kWgc || method == CaptureMethod::kAuto;
}

// 一次截图交回来的光标报告（images[] 里那三个键的本体）。written=false 时渲染层一个键都不写。
struct CursorReport {
    bool written = false;
    std::wstring requested;   // CursorModeName(opt.cursor.mode)
    std::wstring effective;   // cursor_effective::
    std::wstring basis;       // cursor_basis::
};

// 把"请求的那一种"与"这条路径实际交回的那一种"拼成结果里那三个键。
// frameStateKnown / frameCursorIn 是 wgc 那条通道**设完再读回来**的那两个答案（其它通道给
// false/false，因为它们的结论不来自那条会话，而来自登记表）。
// 判据只看两件事：这条路径登记成哪一种能力、以及这一次用户是不是真写过 --cursor。
inline CursorReport MakeCursorReport(const CursorRequest& request, const std::wstring& path,
                                    bool frameStateKnown, bool frameCursorIn) {
    CursorReport report;
    if (!request.given) return report;   // 没写过 --cursor：三个键都不出现（兼容行为）
    report.written = true;
    report.requested = CursorModeName(request.mode);

    const CursorCapability capability = CursorCapabilityOfPath(path);
    if (capability == CursorCapability::kSettable) {
        if (!frameStateKnown) {
            report.effective = cursor_effective::kUnverified;
            report.basis = cursor_basis::kPropertyUnavailable;
            return report;
        }
        report.effective = frameCursorIn ? cursor_effective::kInclude : cursor_effective::kExclude;
        report.basis = request.mode == CursorMode::kDefault ? cursor_basis::kSessionRead
                                                           : cursor_basis::kSessionSet;
        return report;
    }
    if (capability == CursorCapability::kExcludesCursor) {
        // 闸门已经挡掉"include 配这一类路径"（解析期）与"链筛空"（运行期），所以走到这里
        // 只可能是 default 或 exclude 两种请求，而两种都成立在来源这件事上。
        report.effective = cursor_effective::kExclude;
        report.basis = cursor_basis::kPathExcludes;
        return report;
    }
    // 没登记：不敢对光标这件事作任何断言。闸门那一步已经拒绝过"明确要求配未登记的路径"，
    // 剩下的只有 --cursor default —— 这时照实写"问不出来"。
    report.effective = cursor_effective::kUnverified;
    report.basis = cursor_basis::kPropertyUnavailable;
    return report;
}

// ---------------------------------------------------------------------------
// 通道链的光标闸门（实现在 src/CursorControl.cpp：它要产出诊断文案，所以读 Lang）
// ---------------------------------------------------------------------------

struct CursorChainGate {
    std::vector<CaptureMethod> chain;   // 还能兑现这一次光标要求的那几条，按原顺序
    Diagnostic error;                   // 非空 = 一条都不试、不弹框、不落地
    std::vector<Diagnostic> notes;      // 被摘掉的那几条，各一条 note.cursor_channel_skipped
};

// 按这一次的光标要求筛通道链。规矩与 GateChannels 同源：
//   * 显式指定的那条做不到 -> 一条错误，**绝不**替用户换成另一条（也不换成会读桌面像素的那几条）。
//   * auto 链里做不到的那几条 -> 摘掉并各留一条 note，剩下的照旧回退。
//   * 本机版本问不出来 -> 不按版本筛（只按结构筛），让那一步自己交回真码（同 note.os_unverifiable）。
//   * --cursor 没写、或写成 default -> 链原样交出、一条 note 都不发：默认值不动任何东西。
// screenMode 必须传进来：同一个通道在"按窗口"与"按整屏"两种目标上走的是**不同的内部路径**，
// 而这张表登记的是路径（两条 wgc 都有开关，dwm 那条退路读的是屏幕 DC）。
CursorChainGate FilterChainForCursor(const std::vector<CaptureMethod>& chain,
                                     const CursorRequest& request, bool screenMode,
                                     const OsVersion& os);

// 两条闸门串成一份：先按本机版本筛（GateChannels），再按这一次的光标要求筛。
// 版本那一条的错误优先（那条说的是"这条通道在这台机器上根本用不了"，与光标无关）。
// 截图链路、-v 的 input.captureChain、以及 --capabilities 的 autoChains 都走这一份，
// 所以"这次能试哪几条"只有一个答案。
ChannelGate GateCaptureChain(CaptureMethod requested, bool screenMode, const OsVersion& os,
                             const CursorRequest& cursor);

}  // namespace ecapture
