#pragma once
// EvernightCapture - 光标包含与排除（--cursor）的判据本体
//
// 为什么这一层要单独存在，而且按**路径**登记而不是按通道名字：
// "画面里有没有鼠标指针"不是各条通道都能商量的事，它由那条路径的来源像素决定。
// 只有一条通道真的有一个可以设进去、还能读回来核实的开关（wgc 的
// IGraphicsCaptureSession2::IsCursorCaptureEnabled，内部版本 19041 起）；其余交回的像素里
// 本来就没有光标 —— PrintWindow 是让窗口自己画到 DC、dwm 读的是重定向位图、bitblt 拷的是屏幕 DC。
// 桌面复制那两条**不能**算在"本来就没有"里面：官方的 Desktop Duplication 说明写得很清楚，
// 指针要么**已经画在** AcquireNextFrame 交回的那幅桌面图像上，要么由显卡作为独立覆盖层叠加
//（见 direct3ddxgi/desktop-dup-api 的 "Updating the desktop pointer"）。所以"我没有去取那份
// 指针元数据、也没有把它合成进帧"这件事**证明不了**"帧里没有指针像素"。
// 因此"我请求了排除"与"这张图里真的没有光标"是两件事，必须分开写：
//   requested = 用户要的那一种（default / include / exclude）
//   effective = 这条路径实际交回的那一种（include / exclude / unverified）
//   basis     = 这个结论凭什么（设过并读回 / 只读了当前值 / 这条路径的来源就没有光标 /
//               这条路线对光标这一问根本没有答案 / 那一问没答案 / 这条路径压根没登记）
// 三条各占一个字段，谁也不冒充谁。
//
// 六条规矩（改代码前先对齐这里）：
//
// 1. **不支持就说做不到，绝不静默换后端。** 敢声称"这张图里没有光标"的只有两种来源：有开关
//    且这次真的设进去并读回来的那条（wgc），以及来源像素里根本没有光标的 printwindow / dwm /
//    bitblt。桌面复制那两条两种都不是，所以 include 与 exclude 配它都在解析期就报
//    capture.cursor_unsupported + 退出码 1（同一个码、两句文案：那几条"来源根本没有光标"与
//    桌面复制"这条路线保证不了"是两件不同的事，调用方下一步也不一样）：把一次"要光标"的
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
// 5. **"没登记"与"已知保证不了"是两件事，各自一个状态。** 前者是这条表的空白（新增通道忘了加
//    一行，什么都不知道），后者是查过来源之后**能说出口的下场**（桌面复制那两条：既没有开关，
//    来源又可能已经把指针画进帧里）。前者在三值报告里写 unknown，后者写 no —— 把后者删掉或折成
//    unknown 都会把"这条路线确实保证不了"这件事藏起来，把前者写成 no 又是在声称一个没查过的结论。
// 6. **不拿逐帧指针元数据冒充保证。** DXGI_OUTDUPL_FRAME_INFO 的 PointerPosition 说的是**硬件**
//    指针，而且只在 LastMouseUpdateTime 非零时才有意义（那一条文档写明"否则 this value is
//    ignored"）：Visible = FALSE 有两种完全不同的来路 —— 指针此刻根本不可见，以及指针已经画在
//    交回的那幅桌面图像上。这个字段分不开这两种，所以"没有独立可见的指针"推不出"帧里没有指针像素"。
//    要拿它作断言得先有一套逐帧采集 + 跨帧合并指针形状的机制，那是独立后续任务，不在这一层顺手放宽。
//
// 登记表与 src/CaptureScope.cpp 那张"像素来源"表是同一类东西：新增一条通道忘了登记 =
// 更严格（按 kUnregistered 处理：include 做不到、exclude 不敢声称），而不是更松。
// 两份表一致性由 tests\cursor_state.cpp 逐条现场核对（遍历 RegisteredCapturePaths()，
// 每条路径都要在这张表里查得到；每个通道的 WindowPathOf / ScreenPathOf 也要对得上，
// 通道级那两句判断与路径表的取值也要逐条相同）。

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
    // 来源**可能已经把指针画在这幅画面里**，而这条路径没有任何能设进去也读得回来的开关：
    // 桌面复制交回的是显示器合成分，按官方说明指针要么已经画在那幅桌面图像上、要么由显卡单独
    // 叠加（规矩 6），本工具不去取那份元数据也不合成它，于是**两种下场都排除不了**。
    // 所以这一格说的是一件查过之后的**否定**结论：include 与 exclude 都保证不了（三值报告写 no），
    // 而 default / 没写这条选项时图照旧交，只在结果里把这一问答 unverified。
    // 它与下面 kUnregistered 的区别就是"知道保证不了"与"根本没查过"（规矩 5）。
    kPointerStateUnverified,
    // 没登记的名字。include 做不到，exclude **不敢声称**（宁可报无法核实，也不把没查过的事说成查过）。
    kUnregistered,
};

// capability 的机器名（--capabilities 的 cursor.paths 段用它；截图那份 JSON 写的是
// effective/basis 那一组，不写它）。只增不改名。
inline const wchar_t* CursorCapabilityName(CursorCapability capability) {
    switch (capability) {
        case CursorCapability::kSettable: return L"settable";
        case CursorCapability::kExcludesCursor: return L"excludes_cursor";
        case CursorCapability::kPointerStateUnverified: return L"pointer_state_unverified";
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
// 不是 wgc 的那几条：来源像素本身就没有光标这件事（dwm.screen 是屏幕 DC、printwindow 是窗口自绘
// 到 DC、dwm 主路径读的是 8 位重定向位图）。桌面复制那两条**不写这一句** —— 见下面那一条。
inline constexpr const wchar_t* kPathExcludes = L"path_excludes_cursor";
// 桌面复制那两条：来源可能已经把指针画在那幅桌面图像上，而这条路径没有可读回的开关，
// 所以这一问根本没有答案。effective 恒为 unverified，与"那次问答没答案"是两件不同的事
//（那一条是 wgc 特有的属性问不到，这一条是路线本身保证不了）。
inline constexpr const wchar_t* kPathPointerUnverified = L"path_pointer_state_unverified";
// wgc 但那个开关问不到（接口没实现、调用失败）：这时 effective 只能是 unverified。
inline constexpr const wchar_t* kPropertyUnavailable = L"wgc_cursor_property_unavailable";
// 这条内部路径压根没登记在表上（新增通道忘了加一行）：连"该按哪一种来源判"都不知道，
// 所以同样不作任何断言。它与 kPathPointerUnverified（"查过来源，这一问没有答案"）的区别
// 就是规矩 5："没查过"不等于"已知保证不了"，两者也给调用方不同的下一步。
inline constexpr const wchar_t* kPathNotRegistered = L"path_capability_not_registered";
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
// 桌面复制的合成分：按官方说明指针要么**已经画在那幅桌面图像上**，要么由显卡单独叠加，
// 那两种下场在这一问上分不开（规矩 6）。它说的是"这条路线保证不了"，不是"来源没有光标"。
inline constexpr const wchar_t* kPointerUnverified = L"desktop_frame_pointer_state_unverified";
// 被显式 exclude 要求筛掉时，桌面复制那两条写的**这一条**原因：既没有开关可设，来源又可能
// 已经把指针画进帧里，所以不敢声称交回的图没有光标。与上一条同样说的是"保证不了"，
// 而它专门用在筛链那一步，免得调用方把"来源根本没有光标"与"这一问没有答案"读成同一件事
//（与 src/HdrColor.h 的 duplication_hdr_policy_not_implemented 同一种分工）。
inline constexpr const wchar_t* kExcludeUnprovable = L"duplication_cursor_exclusion_unprovable";
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
    // 这两条过去登记成 kExcludesCursor，凭的是"指针是独立元数据而本工具从不合成它"——
    // 那句话证明不了帧里没有指针像素（官方说明允许指针已经画在桌面图像上），现在改登记成
    // 这一问根本没有答案的那一种。见上面规矩 1、5、6。
    {paths::kDuplicationFrame, CursorCapability::kPointerStateUnverified, cursor_reason::kPointerUnverified},
    {paths::kScreenDuplication, CursorCapability::kPointerStateUnverified, cursor_reason::kPointerUnverified},
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

// 这条**通道**的来源可不可能已经把指针画在交回的画面里（结构层面，不看本机版本；auto 不算通道）。
// 只有桌面复制那两条：它读的是显示器的合成分，而按官方说明那一幅桌面图像里指针要么已经画在上面、
// 要么由显卡单独叠加，这一问在本工具手里分不开。与上面那张表的 kPointerStateUnverified 必须逐条
// 相同（判据在 tests\cursor_state.cpp，两种目标各核一遍）。
inline constexpr bool ChannelPointerMayBeInImage(CaptureMethod method) {
    return method == CaptureMethod::kDuplication;
}

// 这条**通道**敢不敢声称"交回的画面里没有光标"（结构层面 + 本构建的实现，auto 不算通道）。
// 撑得起这一句的只有两种：那条真设得进去也读得回来的开关（wgc），以及来源像素里根本没有光标的
// printwindow / dwm / bitblt。桌面复制那两条在这里是 false —— 不是"结构上一定没有光标"，
// 而是"这一问没有答案，所以不许拿它兑现一个用户显式要过的 exclude"。
// 新增一条通道时这里默认落到 false（少写 = 更严而不是更松，与 kUnregistered 同一条规矩）；
// 与路径表的一致性同样由 tests\cursor_state.cpp 逐通道、两种目标核对。
inline constexpr bool ChannelGuaranteesCursorExclusion(CaptureMethod method) {
    switch (method) {
        case CaptureMethod::kWgc:          // 靠把开关设成"不画"并读回来
        case CaptureMethod::kPrintWindow:  // 窗口自绘到 DC，来源没有光标
        case CaptureMethod::kDwmThumbnail: // 读 DWM 重定向位图，来源没有光标
        case CaptureMethod::kBitBlt:       // 拷屏幕 DC，系统指针画在 DC 内容之外
            return true;
        case CaptureMethod::kDuplication:
        case CaptureMethod::kAuto:
            return false;   // auto 不在这里下结论：它看闸门筛完之后剩什么
    }
    return false;
}

// 这一次的光标要求由这条通道（结构层面，不看本机版本）做不做得到。
//   include  —— 只有那个开关设得进去的通道能做到（auto 看闸门筛完之后剩什么）。
//   exclude  —— 靠两样东西之一：那条有开关的去设开关，或来源像素里根本没有光标的那几条。
//               桌面复制那两条**不敢声称**（来源可能已经含指针而没有开关），没登记的路径同样不敢。
//               这里与运行期筛链用的是同一个判据：解析期放行 auto，运行期把不合格的那几条摘掉
//              （src/CursorControl.cpp 的 FilterChainForCursor 查那张路径表）。
//   default  —— 什么都不要求，恒成立。
// 解析层只在显式点名某条通道时下结论（auto 交给闸门）：这一条判断与本机版本、与目标窗口都无关，
// 所以它在 --dry-run 下也成立。
inline bool CursorRequestPossible(CaptureMethod method, CursorMode mode) {
    if (mode == CursorMode::kInclude) return ChannelHasCursorSwitch(method) ||
                                           method == CaptureMethod::kAuto;
    if (mode == CursorMode::kExclude) return ChannelGuaranteesCursorExclusion(method) ||
                                            method == CaptureMethod::kAuto;
    return true;   // default：不构成任何要求
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
    if (capability == CursorCapability::kPointerStateUnverified) {
        // 桌面复制那两条：这一问在这条路线上根本没有答案（来源可能已经把指针画在画面里，
        // 而它没有可读回的开关），所以 effective 只能是 unverified。
        // 正常情况下显式要求（include / exclude）早在解析期或筛链那一步被挡掉了，走不到这里；
        // 留这一格是为了"报告本体自己不兜底"—— 渲染层绝不再把没答案折成一种达成
        //（与规矩 4 同一条理由）。
        report.effective = cursor_effective::kUnverified;
        report.basis = cursor_basis::kPathPointerUnverified;
        return report;
    }
    // 没登记：不敢对光标这件事作任何断言。闸门那一步已经拒绝过"明确要求配未登记的路径"，
    // 剩下的只有 --cursor default —— 这时照实写"问不出来"，并把"根本没查过"与上面那条
    // "查过而保证不了"分开的 basis 交出去（规矩 5）。
    report.effective = cursor_effective::kUnverified;
    report.basis = cursor_basis::kPathNotRegistered;
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

// 三条闸门串成一份：先按本机版本筛（GateChannels），再按这一次的光标要求筛，最后按这一次显式
// 要过的 HDR 处理要求筛（src/HdrColor.h 的 FilterChainForHdr）。
// 版本那一条的错误优先（那条说的是"这条通道在这台机器上根本用不了"，与光标、色彩都无关），
// 光标那一条次之，HDR 那一条最后 —— 一次请求只交回一条最靠前能说清楚的下一步。
// 截图链路与 -v 的 input.captureChain 走这一份，所以"这次能试哪几条"只有一个答案。
// --capabilities 没有请求上下文（它不收 --cursor / --hdr），所以那份 autoChains 走的是只按版本筛的
// GateChannels，并在 color.paths / cursor.paths 两段逐条写明每条路径兑现得了哪一种要求 —— 它说
// "这台机器给得出哪些路线"，绝不冒充"已经按某一次具体请求筛过了"。
ChannelGate GateCaptureChain(CaptureMethod requested, bool screenMode, const OsVersion& os,
                             const CursorRequest& cursor, const HdrRequest& hdr);

}  // namespace ecapture
