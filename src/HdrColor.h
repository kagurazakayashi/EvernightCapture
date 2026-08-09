#pragma once
// EvernightCapture - HDR 截图色彩处理（--hdr）的判据本体
//
// 为什么这一层要单独存在，而且按**内部路径**登记、把"来源色彩空间"与"我做了什么"分开写：
// 显示器处在 HDR 模式时，采集回来的帧可能带着超出 SDR 的亮度范围与另一种传递函数：
//   * WGC 可以按 FP16 scRGB 线性交回（DirectXPixelFormat::A16R16G16B16Float，
//     1.0 = SDR 参考白），这是 Windows 合成 HDR 内容时那条通用面；
//   * 桌面复制交回的桌面纹理跟随该输出的显示模式，HDR 时可能是 FP16 scRGB，
//     也可能是 10 位的 ST.2084 (PQ) / HLG BT.2020。
// 把那种帧硬按 8 位 BGRA 解释，得到的是一张发白、去饱和、亮部一团糊的图，而它"看着像一张
// 正常图"—— 那正是本工具不能默认为正确的结果。所以这里把三件事分开：
//   requested = 用户要的策略（auto / tonemap / refuse）
//   effective = 这一帧**实际**经历的处理（原样 SDR / 已映射 / 没能核实）
//   basis     = 这个结论凭什么（交付的帧本来就是 8 位 SDR / 来源那条路径结构上没有 HDR /
//               真的过了一遍浮点 tone mapping / 带回一个认不出的广色域格式）
// 三条各占一个字段，谁也不冒充谁；与 src/CursorControl.h 的 requested/effective/basis 同一种形状。
//
// 六条规矩（改代码前先对齐这里）：
//
// 1. **默认值一个字都不改。** 没写 --hdr（HdrRequest::given = false）时不探测显示状态、不改
//    采集格式、不做任何映射，结果里色彩那组键一个都不出现 —— 与这条选项存在之前逐字节相同。
//    auto 取值同样是"不启用新链路"，只是把来源色彩空间如实报出来。
// 2. **不支持/做不到就照实说，绝不"那就硬按 BGRA8 交一张发白图当成成功"。**
//    --hdr tonemap/refuse 配一条兑现不了这个要求的通道在解析期报 capture.hdr_unsupported +
//    退出码 1，**绝不换后端**（与 --cursor include 同源）；
//    --hdr refuse 且核实来源是 HDR 帧时一个像素都不落地（capture.hdr_refused）；
//    带回一个本构建认不出的广色域格式时 capture.hdr_unverifiable（认不出格式不等于猜一个映射）。
// 2b. **显式要求过的策略，回退链每一步都要继续兑现它（判据：FilterChainForHdr）。**
//    --capture auto 会换后端，而"换一条只带得回 8 位的后端"等于把用户的要求换成一张
//    可能被合成器压扁的图 —— 那正是 tonemap/refuse 要防的结果，所以：
//      * 没写 --hdr、或写成 auto     链原样交出，一条 note 都不发（规矩 1）；
//      * 显式 tonemap / refuse      链里只留**真能兑现**那几条（目前只有 wgc 的两条路径），
//                                   其余各摘一条并留 note.hdr_channel_skipped；
//                                   一条都不剩 = env.hdr_unsupported（退出码 7），一张都不取；
//      * 后端交回 capture.hdr_refused / capture.hdr_unverifiable 时，那是**用户策略的结论**
//        而不是"这条通道不行"，回退链立刻停下并把原码原样交出去（src/FallbackChain.h）。
//    这一条只**减**后端、绝不**加**后端：显式点名的那条通道仍然绝不自动换成别的，
//    auto 也不会因为 HDR 要求而开始绕开桌面像素那一级的人工确认。
// 3. **映射是确定的、可离线核对的。** FP16 scRGB 与 PQ/HLG BT.2020 到 8 位 BT.709 sRGB 的每一步
//    （解码传递函数 → 色域矩阵 → 按亮度做 tone mapping → sRGB 编码 → alpha 直通）都是纯算术，
//    写在 src/HdrColor.cpp，tests\hdr_state.cpp 用已知色块与亮度梯度逐条判它。浮点中间量只在
//    每像素的栈上寄存器里，**不分配整幅浮点帧**（那会把 1 GiB 的整帧预算乘四撑爆）。
// 4. **不改变授权。** 判据仍是"这条路径的像素从哪来"（src/CaptureScope.cpp 那张表），跟色彩无关：
//    会读到桌面像素的那几条照样一定弹框、--yes 照样管不着；色彩处理整个排在取帧之后、编码之前，
//    一个像素都不会因为"要映射 HDR"而多读、也不引入任何"映射过就算免确认"的旁路。
// 5. **问不出来 ≠ 没事。** 一条路径登记成 kWideGamutCapable 而这一帧的来源格式认不出时，
//    effective 只能写 unverified（basis=format_unrecognized），绝不折成 tone_mapped 或 sdr_passthrough
//    （与身份复核"问不出来 ≠ 相同"、cursor.roi_unmeasurable 与 monitor_unverifiable 分家同源）。
//    同一条规矩也管"这张交付的帧是 8 位"这一件事：它**不证明**来源本来是 SDR —— 合成器完全可能
//    把一幅 HDR 画面压成 8 位再交给一个 B8G8R8A8 的帧池。所以 note.hdr_source_sdr 只在采集之前
//    真的问到"这块屏此刻是 SDR"时才发；问不出来就发 note.hdr_source_unverified（图照常交付，
//    但不许把"没核实"说成"没有 HDR 可映射"），判据是 JudgeHdrPassiveNote 那一份。
//
// 登记表与 src/CaptureScope.cpp / src/CursorControl.h 那两张表是同一类东西：新增一条通道忘了登记
// = 按 kUnregistered 处理（tonemap/refuse 不敢声称带得回 HDR，更严而不是更松）。两份表的一致性
// 由 tests\hdr_state.cpp 现场核对（遍历 RegisteredCapturePaths()，每条都要在这张表里查得到）。

#include <cstdint>
#include <string>
#include <vector>

#include "CaptureScope.h"   // paths:: 那些内部路径名（登记表按它登记，不按通道名）
#include "CliOptions.h"     // HdrPolicy / HdrRequest / CaptureMethod / Diagnostic / codes::

namespace ecapture {

// ConvertWideFrameToSdrBgra8 的两个参数类型。本头文件不 include CaptureCommon.h / Deadline.h
// （那会拖进 Consent / 取帧公共件，而解析层与只链 CliOptions 的离线判据不该被拖进去）：
// 前向声明只用于那一条函数的指针签名，标签要与定义处一致（CapturedFrame 是 struct、Deadline 是 class），
// 而且必须声明在 ecapture 里——那两个类型本来就在那个命名空间，声明到全局会得到另一个同名类型。
struct CapturedFrame;
class Deadline;

// ---------------------------------------------------------------------------
// 一条帧的来源色彩空间与像素格式（GPU 交回来、任何转换**之前**那一步的事实）。
// 取值只增不改名；它进结果里的 images[].sourceColorSpace。
// ---------------------------------------------------------------------------
enum class FrameColorSpace {
    kSrgbBgra8,     // 8 位 BGRA、sRGB/BT.709 —— 最常见那种，原样交付
    kScRgbFloat16,  // FP16 RGBA、scRGB 线性（1.0 = SDR 参考白），亮度可超过 1
    kPqBt2020,      // 10 位 R10G10B10A2、ST.2084 PQ、BT.2020 原色
    kHlgBt2020,     // 10 位 R10G10B10A2、HLG、BT.2020 原色
    kUnknown,       // 一个本构建认不出、因此无法正确映射的广色域格式
};

// FrameColorSpace 的机器名（进 images[].sourceColorSpace 与 --capabilities；只增不改名）。
inline const wchar_t* FrameColorSpaceName(FrameColorSpace cs) {
    switch (cs) {
        case FrameColorSpace::kSrgbBgra8: return L"srgb_bgra8";
        case FrameColorSpace::kScRgbFloat16: return L"scrgb_float";
        case FrameColorSpace::kPqBt2020: return L"pq_bt2020";
        case FrameColorSpace::kHlgBt2020: return L"hlg_bt2020";
        case FrameColorSpace::kUnknown: return L"unknown";
    }
    return L"unknown";
}

// 这个来源是不是 HDR（广色域或高亮范围）。kUnknown 不算已知 HDR，也不算 SDR —— 调用方靠它
// 分"照 SDR 交"与"认不出来不敢交"两种下一步，所以这里只回答"确凿是 HDR 吗"。
inline bool FrameColorSpaceIsHdr(FrameColorSpace cs) {
    return cs == FrameColorSpace::kScRgbFloat16 || cs == FrameColorSpace::kPqBt2020 ||
           cs == FrameColorSpace::kHlgBt2020;
}

// 一个 DXGI_FORMAT 的原值 → 本工具认得的来源色彩空间。写成收 uint32_t 是为了让这份头文件不必
// include <dxgiformat.h>（解析层与离线判据都被拖进 DXGI 头就麻烦了）。认得出的广色域格式以外，
// 一切非 BGRA8 的已知宽格式一律 kUnknown（而不是猜成某一种 HDR）：猜错就是拿错映射去解一幅图。
// 数值取自 DXGI_FORMAT 枚举：87=B8G8R8A8_UNORM，10=R16G16B16A16_FLOAT，
// 61=R10G10B10A2_UNORM。DXGI 不把 PQ 与 HLG 编码进像素格式里（那在输出的 color space 上），
// 所以 10 位包那条按调用方给出的输出色彩空间再细分成 PQ / HLG（见 ClassifyDuplicationSurface）。
inline FrameColorSpace FrameColorSpaceFromDxgiFormat(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case 87u: return FrameColorSpace::kSrgbBgra8;  // DXGI_FORMAT_B8G8R8A8_UNORM
        case 10u: return FrameColorSpace::kScRgbFloat16;  // DXGI_FORMAT_R16G16B16A16_FLOAT
        case 61u: return FrameColorSpace::kPqBt2020;  // DXGI_FORMAT_R10G10B10A2_UNORM（默认按 PQ）
        default: return FrameColorSpace::kUnknown;
    }
}

// 每个来源对应的位深（进 images[].sourceBitDepth）。kUnknown 给 0 = 认不出来，调用方据此不写这个键。
inline uint32_t FrameColorSpaceBitDepth(FrameColorSpace cs) {
    switch (cs) {
        case FrameColorSpace::kSrgbBgra8: return 8;
        case FrameColorSpace::kScRgbFloat16: return 16;
        case FrameColorSpace::kPqBt2020: return 10;
        case FrameColorSpace::kHlgBt2020: return 10;
        case FrameColorSpace::kUnknown: return 0;
    }
    return 0;
}

// 一个来源一帧占几个字节（GPU 拷回 CPU 那一步按它判行距与整帧上限）。
// scRGB FP16 = RGBA16F = 8；R10G10B10A2 与 BGRA8 都是 4。kUnknown 保守按 4（拷回那一步会先
// 按字节搬、认不出的宽格式在编码之前就被判成 hdr_unverifiable，不会真按这个 bpp 去解释像素）。
inline uint32_t FrameColorSpaceBytesPerPixel(FrameColorSpace cs) {
    return cs == FrameColorSpace::kScRgbFloat16 ? 8u : 4u;
}

// ---------------------------------------------------------------------------
// 每条路径"带不带得回广色域帧"的登记表（与 CaptureScope / CursorControl 同一种形状）。
// ---------------------------------------------------------------------------
enum class HdrCapability {
    // 这条路径交回的帧可能带广色域 / 高亮范围，**而且本构建真的按显式 HDR 策略处理它**：
    // 开始采集之前只读地问一次那块屏此刻的色彩空间，据此决定帧池格式（B8G8R8A8 还是 FP16 scRGB），
    // 问出来是 HDR 而策略是 refuse 就在一个像素都不读之前停下。目前只有 wgc 那两条做得到。
    kWideGamutCapable,
    // 这条路径的来源在 Windows 那一侧**可能**跟显示模式走（桌面复制的桌面纹理），但本构建没有
    // 实现兑现显式 HDR 策略所需的那几步：它仍用 IDXGIOutput1::DuplicateOutput()，既不选广色域
    // 格式、也不问那块屏此刻是不是 HDR 模式，而 10 位那一条的 PQ/HLG 之分还要靠输出的色彩空间
    // 才能判（见 FrameColorSpaceFromDxgiFormat 那一段）。所以它交回一张 8 位 BGRA 时说不出
    // "原始内容到底是不是 HDR"。tonemap / refuse 因此不许落到它身上 —— 保守拒绝一条没能核实的
    // "支持 HDR"声明，而不是拿它继续放行。完整的广色域采集是独立后续任务（README/Skill 同记）。
    kWideGamutUnverified,
    // 结构上只会带回 8 位 SDR：printwindow 让窗口自绘进 8 位 DIB、dwm 读的是 8 位重定向位图、
    // bitblt 拷的是 8 位屏幕 DC。HDR 处理对它们没有对象，所以是恒等而非"做不到就换一条"。
    kSdrSourceOnly,
    // 没登记。tonemap/refuse 因此不敢声称带得回 HDR（新增通道忘登记 = 更严而不是更松）。
    kUnregistered,
};

// 一条路径"这件事的根据"那一个 ASCII token（进 --capabilities 的 color.paths 段；不随 --lang 变）。
namespace hdr_reason {
inline constexpr const wchar_t* kWgcComposed = L"wgc_composition_surface";       // WGC 读 DWM 合成面
inline constexpr const wchar_t* kDupDesktop = L"duplication_desktop_surface";    // 桌面纹理跟显示模式
inline constexpr const wchar_t* kSelfDrawn8 = L"window_self_drawn_8bit";         // printwindow 自绘
inline constexpr const wchar_t* kDwmSurface8 = L"dwm_redirection_surface_8bit";  // dwm 读重定向位图
inline constexpr const wchar_t* kScreenDc8 = L"screen_dc_8bit";                  // bitblt 拷屏幕 DC
inline constexpr const wchar_t* kNotRegistered = L"not_registered";
// 被显式 HDR 策略筛掉时，桌面复制那两条写的**这一条**原因：来源可能有广色域，但本构建没实现
// 兑现策略那几步（见 kWideGamutUnverified）。它说的是"没做/没核实"，不是"结构上不可能"。
inline constexpr const wchar_t* kDupPolicyUnverified = L"duplication_hdr_policy_not_implemented";
}  // namespace hdr_reason

// capability 的机器名（--capabilities 用它；截图那份 JSON 写的是 effective/basis 那一组，不写它）。
inline const wchar_t* HdrCapabilityName(HdrCapability capability) {
    switch (capability) {
        case HdrCapability::kWideGamutCapable: return L"wide_gamut_capable";
        case HdrCapability::kWideGamutUnverified: return L"wide_gamut_unverified";
        case HdrCapability::kSdrSourceOnly: return L"sdr_source_only";
        case HdrCapability::kUnregistered: return L"unregistered";
    }
    return L"unregistered";
}

// 登记表的一行。
struct HdrPathEntry {
    const wchar_t* path;
    HdrCapability capability;
    const wchar_t* reason;
};

// 登记表本体（唯一出处）。写成 inline 而不是放 .cpp，与光标那张表同一个理由：解析层
// （CliOptions.cpp）也要问它，而那些只链 CliOptions.cpp 的离线判据目标不该被拖进取帧公共件。
inline constexpr HdrPathEntry kHdrTable[] = {
    {paths::kWgc, HdrCapability::kWideGamutCapable, hdr_reason::kWgcComposed},
    {paths::kScreenWgc, HdrCapability::kWideGamutCapable, hdr_reason::kWgcComposed},
    {paths::kDuplicationFrame, HdrCapability::kWideGamutUnverified, hdr_reason::kDupDesktop},
    {paths::kScreenDuplication, HdrCapability::kWideGamutUnverified, hdr_reason::kDupDesktop},
    {paths::kPrintWindow, HdrCapability::kSdrSourceOnly, hdr_reason::kSelfDrawn8},
    {paths::kDwmThumbnail, HdrCapability::kSdrSourceOnly, hdr_reason::kDwmSurface8},
    {paths::kDwmScreen, HdrCapability::kSdrSourceOnly, hdr_reason::kScreenDc8},
    {paths::kBitBltScreen, HdrCapability::kSdrSourceOnly, hdr_reason::kScreenDc8},
    {paths::kScreenBitBlt, HdrCapability::kSdrSourceOnly, hdr_reason::kScreenDc8},
};

// 整张登记表（只读视图）：--capabilities 的 color.paths 段与离线判据都遍历这一份。
inline const std::vector<HdrPathEntry>& RegisteredHdrPaths() {
    static const std::vector<HdrPathEntry> table(std::begin(kHdrTable), std::end(kHdrTable));
    return table;
}

// 这条内部路径能不能带广色域帧。没登记（含空串与 paths::kUnknown）一律 kUnregistered。
inline HdrCapability HdrCapabilityOfPath(const wchar_t* path) {
    if (path) {
        for (const auto& e : kHdrTable)
            if (std::wstring(e.path) == path) return e.capability;
    }
    return HdrCapability::kUnregistered;
}
inline HdrCapability HdrCapabilityOfPath(const std::wstring& path) {
    return HdrCapabilityOfPath(path.c_str());
}
inline const wchar_t* HdrReasonOfPath(const wchar_t* path) {
    if (path) {
        for (const auto& e : kHdrTable)
            if (std::wstring(e.path) == path) return e.reason;
    }
    return hdr_reason::kNotRegistered;
}

// 这条**通道**（结构层面，不看本机版本、不看目标）的来源带不带得回广色域帧这件事。auto 不算通道。
// 它说的是 Windows 那一侧的可能性，**不是**"本工具真的按显式策略处理它"（那一条见
// ChannelHonorsHdrPolicy）。与上面那张表必须一致：判据写在 tests\hdr_state.cpp，对每个通道取
// WindowPathOf / ScreenPathOf 查表核对，两种取值（capable / unverified）都算"带得回"。
inline constexpr bool ChannelCarriesWideColorFrame(CaptureMethod method) {
    return method == CaptureMethod::kWgc || method == CaptureMethod::kDuplication;
}

// 这条**通道**在显式 tonemap / refuse 下真兑现得了要求吗（结构 + 本构建实现两层；auto 不算通道）。
// 只有 wgc 那两条：它在开始采集之前问过那块屏的色彩空间，也真的据此建过广色域帧池。
// 桌面复制那两条在这里是 false —— 不是"来源不可能带广色域"，而是"这一步本构建没做、没核实"，
// 所以不许拿它兑现一个用户显式要过的要求（登记表 kWideGamutUnverified 那条注释写了差在哪几步）。
inline constexpr bool ChannelHonorsHdrPolicy(CaptureMethod method) {
    return method == CaptureMethod::kWgc;
}

// 这条**内部路径**在显式策略下是不是合格候选（唯一判据，登记表读出来，不在调用方另写通道名单）。
// auto 恒成立：它不要求任何处理，所以任何一条都算兑现（规矩 1）。
inline bool HdrPathHonorsPolicy(const wchar_t* path) {
    return HdrCapabilityOfPath(path) == HdrCapability::kWideGamutCapable;
}
inline bool HdrPathHonorsPolicy(const std::wstring& path) {
    return HdrPathHonorsPolicy(path.c_str());
}
inline bool HdrPathFulfilsPolicy(const wchar_t* path, HdrPolicy policy) {
    if (policy == HdrPolicy::kAuto) return true;
    return HdrPathHonorsPolicy(path);
}

// 这一条路径被显式策略筛掉时那一个稳定的原因 token（ASCII，进 note.hdr_channel_skipped 的 message
// 与调用方分支；与 message 不同，它不随 --lang 变）。只有被筛掉的路径才会走到这里，所以合格的
// 那两种取值都落进"说不清"（新增取值忘了登记 = 更严而不是更松，与 kUnregistered 同一条规矩）。
inline const wchar_t* HdrPolicyBlockReason(const wchar_t* path) {
    switch (HdrCapabilityOfPath(path)) {
        case HdrCapability::kSdrSourceOnly:
            // 结构上只带得回 8 位 SDR，原因就用登记表那一句（window_self_drawn_8bit 那种）。
            return HdrReasonOfPath(path);
        case HdrCapability::kWideGamutUnverified:
            return hdr_reason::kDupPolicyUnverified;
        case HdrCapability::kWideGamutCapable:
        case HdrCapability::kUnregistered:
            return hdr_reason::kNotRegistered;
    }
    return hdr_reason::kNotRegistered;
}

// 这一次的 HDR 策略由这条通道做不做得到（解析期那一道，与本机版本、与目标都无关，所以
// --dry-run 下也成立）。auto 恒成立；tonemap / refuse 只有**真兑现得了**的那条通道（wgc）放行，
// 配 printwindow / dwm / bitblt / duplication 在解析期就该说做不到（退出码 1，不换后端）。
// --capture auto 不在这里判：它最终落到哪条通道要到运行期才知道，判据是 FilterChainForHdr 那一份
//（规矩 2b）—— 解析期放行 auto，运行期把不合格的那几条摘掉，两边共用同一张登记表。
inline bool HdrRequestPossible(CaptureMethod method, HdrPolicy policy) {
    if (policy == HdrPolicy::kAuto) return true;
    if (method == CaptureMethod::kAuto) return true;
    return ChannelHonorsHdrPolicy(method);
}

// ---------------------------------------------------------------------------
// images[].hdrEffective / hdrBasis 的取值（机器名，不随 --lang 变，只增不改名）。
// ---------------------------------------------------------------------------
namespace hdr_effective {
// 这一帧按 8 位 SDR 交付，且来源核实本来就是 SDR：没做（也不需要）任何色彩转换。
inline constexpr const wchar_t* kSdrPassthrough = L"sdr_passthrough";
// 来源是 HDR，本工具已按固定的浮点链路把它映射到 8 位 BT.709 sRGB 再交付。
inline constexpr const wchar_t* kToneMapped = L"tone_mapped";
// 没能核实这一帧的来源色彩空间（带回一个认不出的广色域格式）：不声称映射对，也不声称就是 SDR。
inline constexpr const wchar_t* kUnverified = L"unverified";
}  // namespace hdr_effective

namespace hdr_basis {
// 交付的帧按 BGRA8 处理且来源核实为 SDR（含"这条路径结构上没有 HDR"与"这条 WGC/复制帧是 SDR"）。
inline constexpr const wchar_t* kDeliveredBgra8Sdr = L"delivered_bgra8_sdr";
// 真的过了一遍 FP16 scRGB → sRGB 的浮点 tone mapping。
inline constexpr const wchar_t* kScRgbToneMapped = L"scrgb_float_tone_mapped";
// 真的过了一遍 PQ BT.2020 → sRGB 的浮点 tone mapping。
inline constexpr const wchar_t* kPqToneMapped = L"pq_bt2020_tone_mapped";
// 真的过了一遍 HLG BT.2020 → sRGB 的浮点 tone mapping。
inline constexpr const wchar_t* kHlgToneMapped = L"hlg_bt2020_tone_mapped";
// 这条路径结构上带不回广色域帧（printwindow / dwm / bitblt），tonemap 因此是恒等透传。
inline constexpr const wchar_t* kPathSdrSource = L"path_sdr_source";
// 带回一个本构建认不出的广色域像素格式 —— 既不敢映射，也不敢说它就是 SDR。
inline constexpr const wchar_t* kFormatUnrecognized = L"format_unrecognized";
}  // namespace hdr_basis

// 一次截图交回来的 HDR 报告（images[] 里那组色彩键的本体）。written=false 时渲染层一个键都不写。
struct HdrReport {
    bool written = false;
    std::wstring requested;            // HdrPolicyName(request.policy)
    std::wstring effective;            // hdr_effective::
    std::wstring basis;                // hdr_basis::
    std::wstring sourceColorSpace;     // FrameColorSpaceName(source)
    bool bitDepthKnown = false;
    uint32_t bitDepth = 0;             // 8 / 10 / 16；kUnknown 时 bitDepthKnown=false
};

// 把"要求的策略"与"这条路径这一帧实际带回的来源色彩空间"合成结果里那组键。
// source 是编码之前那份帧的**来源**色彩空间（wide 帧被映射过也保留映射前那一份），
// path 是这条路径的内部路径名（用来分辨"来源本来就是 SDR"与"这条路径带不回 HDR"）。
// 判据只看两件事：来源核实成哪一种、这条路径登记成哪一种能力。绝不重跑映射，也不改退出码。
inline HdrReport MakeHdrReport(const HdrRequest& request, const std::wstring& path,
                               FrameColorSpace source) {
    HdrReport report;
    if (!request.given) return report;   // 没写过 --hdr：那组键都不出现（兼容行为）
    report.written = true;
    report.requested = HdrPolicyName(request.policy);
    report.sourceColorSpace = FrameColorSpaceName(source);
    const uint32_t bd = FrameColorSpaceBitDepth(source);
    report.bitDepthKnown = bd != 0;
    report.bitDepth = bd;

    switch (source) {
        case FrameColorSpace::kScRgbFloat16:
            report.effective = hdr_effective::kToneMapped;
            report.basis = hdr_basis::kScRgbToneMapped;
            return report;
        case FrameColorSpace::kPqBt2020:
            report.effective = hdr_effective::kToneMapped;
            report.basis = hdr_basis::kPqToneMapped;
            return report;
        case FrameColorSpace::kHlgBt2020:
            report.effective = hdr_effective::kToneMapped;
            report.basis = hdr_basis::kHlgToneMapped;
            return report;
        case FrameColorSpace::kUnknown:
            // 认不出的广色域格式：不声称映射对，也不声称是 SDR。
            report.effective = hdr_effective::kUnverified;
            report.basis = hdr_basis::kFormatUnrecognized;
            return report;
        case FrameColorSpace::kSrgbBgra8:
        default:
            // 这一帧是按 8 位 BGRA sRGB 交付的。两种根据分开写：这条路径结构上带不回 HDR
            //（printwindow 那几条，来源那一级由登记表断言），还是这条路径按 8 位帧池取回了
            // 一张（wgc 与桌面复制）。后者**不**等于"核实过原始内容是 SDR"—— 那一问的答案在
            // CapturedFrame::displayHdrState，由 JudgeHdrPassiveNote 拿去决定留哪一条提示；
            // 这个键只说交付那一份的形状与这条路径的登记根据，不替那次问答背书。
            report.effective = hdr_effective::kSdrPassthrough;
            report.basis = HdrCapabilityOfPath(path) == HdrCapability::kSdrSourceOnly
                               ? hdr_basis::kPathSdrSource
                               : hdr_basis::kDeliveredBgra8Sdr;
            return report;
    }
}

// ---------------------------------------------------------------------------
// 显示是否处在 HDR 模式的只读问答 + 通道链的 HDR 闸门（实现在 src/HdrColor.cpp）
// ---------------------------------------------------------------------------

// 一次探测某块屏此刻是不是在 HDR 模式的结果。三值：yes / no / 问不出来（unknown）。
// unknown 既不折成 yes 也不折成 no（规矩 5）：问不出来时 tonemap 走 SDR 透传并留
// basis，refuse 不因此拒绝（它拒的是"确凿是 HDR"），而不是猜一个决定。
enum class DisplayHdrState { kSdr, kHdr, kUnknown };

// 一个 DXGI 输出的 color space 原值（DXGI_COLOR_SPACE_TYPE）→ 是不是 HDR。写成收 uint32_t 让
// 这份头文件不 include dxgi1_6.h；离线判据就能逐条注入。判据：G2084(PQ) 与 HLG 那几种是 HDR。
DisplayHdrState DisplayHdrStateOfDxgiColorSpace(uint32_t dxgiColorSpaceType);

// 按 HWND / HMONITOR 只读地问一次它所在那块屏此刻是不是 HDR 模式。
// 全程只读：QueryInterface 一条 DXGI 输出问它的 color space，绝不改任何显示设置
// （不调 ChangeDisplaySettings / SetDisplayConfig）。任何一步问不出来就返回 kUnknown。
DisplayHdrState ProbeDisplayHdrForHwnd(uint64_t hwnd);
DisplayHdrState ProbeDisplayHdrForMonitor(void* hmonitor);

// ---------------------------------------------------------------------------
// 显式要求过 HDR 处理、而这一张交回的是 8 位 SDR：该留哪一条提示（判据只有这一份）
// ---------------------------------------------------------------------------
//
// "我要过 HDR 处理"与"这一张其实没有 HDR 可处理"是两件事，得摆在调用方眼前（规矩 5 那一条的
// 延伸）。第三种下场是这件更重要的差别：**没核实过**。一张 8 位 BGRA 帧并不证明原始内容是 SDR
// —— 合成器完全可能把一幅 HDR 画面压成 8 位再交给一个 B8G8R8A8 的帧池，所以那句"来源是 SDR"
// 只能在采集之前真的问到"这块屏此刻是 SDR"时才算，否则只能说"这件事没核实出来"。
// 两种都是提示（图照常交付、退出码不变），不是错误，也不是静默通过。
enum class HdrPassiveNote {
    kNone,             // 不该发提示：没要求过、要求的是 auto（只被动上报），或这一张真带回了广色域来源
    kSourceSdr,        // note.hdr_source_sdr：核实过这块屏是 SDR，所以那个处理确实是恒等的
    kSourceUnverified, // note.hdr_source_unverified：这一张是 8 位交付，而来源是不是 HDR 没核实出来
};

inline HdrPassiveNote JudgeHdrPassiveNote(const HdrRequest& request, FrameColorSpace source,
                                          DisplayHdrState displayState) {
    if (!request.given || request.policy == HdrPolicy::kAuto) return HdrPassiveNote::kNone;
    if (source != FrameColorSpace::kSrgbBgra8) return HdrPassiveNote::kNone;
    return displayState == DisplayHdrState::kSdr ? HdrPassiveNote::kSourceSdr
                                                 : HdrPassiveNote::kSourceUnverified;
}

// ---------------------------------------------------------------------------
// 通道链的 HDR 闸门（实现同样在 src/HdrColor.cpp：它要产出诊断文案，所以读 Lang）
// ---------------------------------------------------------------------------

struct HdrChainGate {
    std::vector<CaptureMethod> chain;   // 还能兑现这一次 HDR 要求的那几条，按原顺序
    Diagnostic error;                   // 非空 = 一条都不试、不弹框、不落地
    std::vector<Diagnostic> notes;      // 被摘掉的那几条，各一条 note.hdr_channel_skipped
};

// 按这一次的显式 HDR 要求筛通道链（规矩 2b）。判据与 FilterChainForCursor 同源：
//   * 只**减**不加：这里从不把一条通道换进链里，也不引入任何"映射过就算免确认"的旁路 ——
//     会读桌面像素的那几条照样一定问人（--capture auto + --hdr tonemap 落在整屏那条 wgc 路径上
//     时仍然要人点头，ScreenPathOf(kWgc) 是桌面路径）。
//   * 没写 --hdr、或写成 auto -> 链原样交出、一条 note 都不发（规矩 1：默认值不动任何东西）。
//   * 链里做不到的那几条 -> 摘掉并各留一条 note，剩下照旧回退；一条都不剩 = env.hdr_unsupported，
//     一张都不取、确认框也不弹。绝不"那就照能截的那几条先交出再说"。
//   * 显式指定的那条通道不经过这里（GateChannels 给它一条 env 错误、解析层给它
//     capture.hdr_unsupported），所以"要哪个就要哪个"这条规矩不被筛选取代。
// screenMode 必须传进来：同一个通道在两种目标上走的是不同的内部路径，而这张表登记的是路径。
HdrChainGate FilterChainForHdr(const std::vector<CaptureMethod>& chain, const HdrRequest& request,
                               bool screenMode);

// ---------------------------------------------------------------------------
// 浮点中间量与 tone mapping 的纯算术（实现与逐点判据在 src/HdrColor.cpp / tests\hdr_state.cpp）
// ---------------------------------------------------------------------------

// IEEE 754 binary16（半精度）→ 单精度 float。DXGI 那条 FP16 面交回的每个通道就是这个编码。
float HalfToFloat(uint16_t half);

// ST.2084 (PQ) 解码：编码值 [0,1] → 绝对亮度 [0,10000] cd/m²。
double PqEotfToNits(double encoded);
// HLG (ARIB STD-B67 / ITU-R BT.2102) 反 OETF：编码 [0,1] → 相对线性 [0,1]（系统色需要的那一步另加）。
double HlgInverseEotf(double encoded);
// sRGB / BT.709 的正 OETF：线性 [0,1] → 编码 [0,1]（超过 1 的先由 tone mapping 压回，这里再夹一次）。
double SrgbOetf(double linear);

// 一条固定的亮度 tone mapping 曲线：把线性相对亮度 [0, white] 单调映到 [0, 1]，
// 0 进 0 出、white 进 1 出，中间不上色带、不抖。white 是这次映射的参考峰值（相对 SDR 白）。
// 用扩展 Reinhard（ETwP）：L*(1+L/white^2)/(1+L)。它是确定、单调、不产生硬剪枝的，
// 且在 white=1 时退化成恒等（所以一张本来就 ≤ 参考白的帧被映射后与不映射逐像素相同）。
double ToneMapRelativeLuminance(double linearLuminance, double white);

// 把一帧**广色域来源**（FP16 scRGB 或 10 位 PQ/HLG）就地映射成 8 位 BGRA sRGB。
// 读 frame->pixels（按 frame->sourceColorSpace 那份布局）与 frame->width/height/stride，
// 写出紧凑 BGRA8（stride = width*4），把 frame->sourceColorSpace 保留为映射前那一份、
// 设 frame->toneMapped=true。分配之前先过整帧上限那道线（CheckFrameShape），内存预算与超时都守。
// dl 非空时按剩余预算判：预算已经花光就交回 capture.timeout 而不动 frame。
// 失败（形状不合法 / 认不出的来源 / 预算用尽）时不写坏 frame，交回诊断。
bool ConvertWideFrameToSdrBgra8(CapturedFrame* frame, const Deadline* dl, Diagnostic* err);

}  // namespace ecapture
