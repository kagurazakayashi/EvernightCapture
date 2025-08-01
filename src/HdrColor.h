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
// 五条规矩（改代码前先对齐这里）：
//
// 1. **默认值一个字都不改。** 没写 --hdr（HdrRequest::given = false）时不探测显示状态、不改
//    采集格式、不做任何映射，结果里色彩那组键一个都不出现 —— 与这条选项存在之前逐字节相同。
//    auto 取值同样是"不启用新链路"，只是把来源色彩空间如实报出来。
// 2. **不支持/做不到就照实说，绝不"那就硬按 BGRA8 交一张发白图当成成功"。**
//    --hdr tonemap/refuse 配一条结构上带不回广色域帧的通道（printwindow / dwm / bitblt）在解析期
//    报 capture.hdr_unsupported + 退出码 1，**绝不换后端**（与 --cursor include 同源）；
//    --hdr refuse 且核实来源是 HDR 帧时一个像素都不落地（capture.hdr_refused）；
//    带回一个本构建认不出的广色域格式时 capture.hdr_unverifiable（认不出格式不等于猜一个映射）。
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
    // 这条路径交回的帧可能带广色域 / 高亮范围（WGC 合成面、桌面复制的桌面纹理都跟显示模式走）。
    kWideGamutCapable,
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
}  // namespace hdr_reason

// capability 的机器名（--capabilities 用它；截图那份 JSON 写的是 effective/basis 那一组，不写它）。
inline const wchar_t* HdrCapabilityName(HdrCapability capability) {
    switch (capability) {
        case HdrCapability::kWideGamutCapable: return L"wide_gamut_capable";
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
    {paths::kDuplicationFrame, HdrCapability::kWideGamutCapable, hdr_reason::kDupDesktop},
    {paths::kScreenDuplication, HdrCapability::kWideGamutCapable, hdr_reason::kDupDesktop},
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

// 这条**通道**（结构层面，不看本机版本、不看目标）带不带得回广色域帧。auto 不算通道。
// 与上面那张表必须一致，判据写在 tests\hdr_state.cpp（对每个通道取 WindowPathOf/ScreenPathOf 查表核对）。
inline constexpr bool ChannelCarriesWideColorFrame(CaptureMethod method) {
    return method == CaptureMethod::kWgc || method == CaptureMethod::kDuplication;
}

// 这一次的 HDR 策略由这条通道（结构层面）做不做得到。auto 恒成立；tonemap / refuse 只有那条
// 真带得回广色域帧的通道才有意义，配 printwindow / dwm / bitblt 在解析期就该说做不到（退出码 1，
// 不换后端）。--capture auto 不在这里判：它最终落到哪条通道要到运行期才知道，而那两条带得回广色域
// 帧的通道（wgc / duplication）都在回退链里，所以 auto 放行（与 --cursor include 配 auto 同源）。
// 解析层只判这一条：它与本机版本、与目标窗口都无关，所以在 --dry-run 下也成立。
inline bool HdrRequestPossible(CaptureMethod method, HdrPolicy policy) {
    if (policy == HdrPolicy::kAuto) return true;
    return ChannelCarriesWideColorFrame(method) || method == CaptureMethod::kAuto;
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
            // 来源就是 8 位 SDR。分两种根据：这条路径结构上带不回 HDR（printwindow 那几条），
            // 还是这条路径带得回 HDR 而这一帧恰好是 SDR（wgc / duplication 在 SDR 显示上）。
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
