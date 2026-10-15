// 【平台层 · Windows】DXGI 编号 -> 共用层的色彩空间与显示 HDR 状态这两张分类表。
//
// 为什么这两条要单独成一份平台源文件（跨平台图像处理与编码这一步）：它们问的是"DXGI 这个编号
// 在微软那套枚举里叫什么"，编号的唯一来源就是 SDK 符号本身（dxgiformat.h / dxgicommon.h）。
// 判据本体（tone mapping 那趟算术、HDR 闸门、结果合成）是纯算术，已经留在共用层 src/HdrColor.cpp；
// 让那一份为了两句编号分类去 include DXGI 头，等于要求 Linux 也有一张 DXGI 表 —— 那是把一家平台
// 的模型强加给另外两家（AGENTS.md《结构与依赖边界》）。
//
// 声明仍然在 src/HdrColor.h（FrameColorSpace 与 DisplayHdrState 是跨平台取值）：一个构建里
// 只用得到自己那一家平台的实现，与 CliArgs.h 那两条平台入口同一类做法。
//
// 两条表的历史错误（照搬过来，别在搬家时改语义）：上一版按手抄编号写过一次 ——
//   * R10G10B10A2_UNORM 是 24，而那份表把 61（R8_UNORM，单通道 8 位）当成了 10 位包，
//     于是每像素字节数与映射双双错位；
//   * 显示色彩空间那一张把 2 / 5 / 17 当成 HDR（那是 G22 的 studio-range 与 BT.2020 宽色域
//     **SDR**），又把 13 / 14 两条真 PQ 的当成 SDR。宽色域 SDR 面板答 HDR 会让 --hdr refuse
//     在没有 HDR 的机器上拒掉一张正常图。
// 现在编号只从 SDK 符号来，判据见 tests/hdr_state.cpp（遍历两张表的每一个编号）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dxgi.h>

#include "HdrColor.h"

namespace ecapture {

// ---------------------------------------------------------------------------
// 一帧的内存格式 -> 叫得出名字的来源色彩空间（唯一一份分类；编号只从 SDK 符号来）
// ---------------------------------------------------------------------------

FrameColorSpace FrameColorSpaceFromDxgiFormat(uint32_t dxgiFormat) {
    switch (dxgiFormat) {
        case static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM):
            return FrameColorSpace::kSrgbBgra8;
        case static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT):
            return FrameColorSpace::kScRgbFloat16;
        // 10 位打包：布局是这一句能给的全部事实。PQ / HLG / 还是某种宽色域 SDR 写在输出的
        // color space 上，不写在像素格式里，所以这里说不出色彩空间，也绝不默认按某一种 HDR 交
        // （那个默认值本身就是"拿错的映射去解一幅图"，而按 4 字节搬一个不是 10 位包的格式
        // 还会读越界 —— 上一版就是这么错的）。
        case static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM):
            return FrameColorSpace::kRgb10A2Unverified;
        // 其余一切都不叫名字：DXGI_FORMAT_R8_UNORM（61，单通道 8 位）、那三种 10 位包的
        // UINT / XR_BIAS 变体、各种 YUV 与类型无格式，以及认不出的任何编号。
        default:
            return FrameColorSpace::kUnknown;
    }
}


// ---------------------------------------------------------------------------
// 显示 HDR 状态：只读问答（绝不改显示设置）
// ---------------------------------------------------------------------------

DisplayHdrState DisplayHdrStateOfDxgiColorSpace(uint32_t cs) {
    // 只有带 ST.2084(PQ) 或 HLG 那一条传递函数的才是 HDR（编号同样只从 SDK 符号来）。
    // 上一版这一条也是手抄编号，而且抄错得比格式那张更实用：它把 2 / 5 / 17 当成 HDR（那三条
    // 其实是 G22 的 studio-range 与 BT.2020 宽色域 **SDR**），又把 13 / 14 那两条真 PQ 的当成
    // SDR。宽色域 SDR 面板答 HDR 会让 --hdr refuse 在没有 HDR 的机器上拒掉一张正常图。
    switch (cs) {
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020):
            return DisplayHdrState::kHdr;
        // SDK 里有名字的其余显示色彩空间：G22（sRGB / BT.709 / BT.601 / BT.2020 的 full 与
        // studio range，RGB 与 YCbCr 两种取向）、G10（scRGB 的那种配置）与 G24。原色是
        // BT.2020 不等于 HDR —— 传递函数才是那一问的答案。
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020):
        case static_cast<uint32_t>(DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020):
            return DisplayHdrState::kSdr;
        default:
            // 认不出就当没答案（规矩 5）：DXGI_COLOR_SPACE_RESERVED 与 DXGI_COLOR_SPACE_CUSTOM
            // 都不是一句"这块屏此刻是什么"，既不折成 HDR 也不折成 SDR。
            return DisplayHdrState::kUnknown;
    }
}

}  // namespace ecapture

