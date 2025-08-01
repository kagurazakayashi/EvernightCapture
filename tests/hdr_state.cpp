// HDR 色彩处理（--hdr）的离线判据（由 tests\hdr.ps1 运行 build\ecapture-hdr-tests.exe）。
//
// 为什么在这一层测：本机没有一台能开 HDR 的显示器，真去带回一幅 FP16 / 10 位帧这件事在这里
// 造不出来 —— 而最要紧的两件判据（tone mapping 的数学对不对、认不认得来源格式与色彩空间）
// 全都是纯算术，可以拿"已知色块 + 亮度梯度"逐点判，不需要任何硬件。测的是生产函数本体
// （src/HdrColor.cpp、src/ImageOps.cpp、那张按路径登记的表），不是测试里另抄一份算法。
//
// 只用 C 风格 printf 汇报；任何一条不过就返回非 0。真机那一条（HDR 显示器上的实拍对照）
// 一律记未验证，不在这里伪造。
#include <array>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../src/CaptureCommon.h"
#include "../src/CaptureScope.h"
#include "../src/CliOptions.h"
#include "../src/HdrColor.h"
#include "../src/Lang.h"
#include "../src/ImageOps.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}
void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

// 单精度 -> 半精度（就近取整）。只测正常范围与几个已知值，够摆出色块与梯度。
uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t e = static_cast<int32_t>((x >> 23) & 0xFFu) - 127;
    uint32_t m = x & 0x7FFFFFu;
    if (e > 15) return static_cast<uint16_t>(sign | 0x7C00u);
    if (e >= -14) {
        uint32_t hm = m >> 13;
        uint32_t rem = m & 0x1FFFu;
        uint32_t half = sign | (static_cast<uint32_t>(e + 15) << 10) | hm;
        if (rem >= 0x1000u) ++half;
        return static_cast<uint16_t>(half);
    }
    if (e >= -24) {
        m |= (1u << 23);
        const uint32_t shift = static_cast<uint32_t>(-e) + 13u;
        uint32_t half = m >> shift;
        if ((m >> (shift - 1)) & 1u) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    return static_cast<uint16_t>(sign);
}

// 造一帧 scRGB FP16：每像素 4 个 half（RGBA），行距 = width*8。
CapturedFrame ScRgbFrame(std::vector<std::array<float, 4>> rgba) {
    CapturedFrame f;
    const uint32_t w = static_cast<uint32_t>(rgba.size());
    f.width = w;
    f.height = 1;
    f.stride = w * 8u;
    f.pixels.assign(static_cast<size_t>(f.stride), 0);
    auto* h = reinterpret_cast<uint16_t*>(f.pixels.data());
    for (uint32_t i = 0; i < w; ++i) {
        for (int c = 0; c < 4; ++c) h[i * 4 + c] = FloatToHalf(rgba[i][c]);
    }
    f.source = L"wgc";
    f.path = paths::kWgc;
    f.sourceColorSpace = FrameColorSpace::kScRgbFloat16;
    return f;
}

// 造一帧 R10G10B10A2：每像素一个 uint32（R/G/B 各 10 位 + A 2 位），行距 = width*4。
CapturedFrame R10Frame(const std::vector<std::array<uint32_t, 4>>& rgba10) {
    CapturedFrame f;
    const uint32_t w = static_cast<uint32_t>(rgba10.size());
    f.width = w;
    f.height = 1;
    f.stride = w * 4u;
    f.pixels.assign(static_cast<size_t>(f.stride), 0);
    auto* p = reinterpret_cast<uint32_t*>(f.pixels.data());
    for (uint32_t i = 0; i < w; ++i) {
        const auto& v = rgba10[i];
        p[i] = (v[0] & 0x3FFu) | ((v[1] & 0x3FFu) << 10) | ((v[2] & 0x3FFu) << 20) |
               ((v[3] & 0x3u) << 30);
    }
    f.source = L"duplication";
    f.path = paths::kDuplicationFrame;
    f.sourceColorSpace = FrameColorSpace::kPqBt2020;
    return f;
}

// 读一帧交付 BGRA8 的某个像素。
void ReadBgra(const CapturedFrame& f, uint32_t x, uint32_t y, uint8_t* out) {
    const size_t off = static_cast<size_t>(y) * f.stride + static_cast<size_t>(x) * 4;
    out[0] = f.pixels[off];
    out[1] = f.pixels[off + 1];
    out[2] = f.pixels[off + 2];
    out[3] = f.pixels[off + 3];
}

double LumaOf(const uint8_t* bgra) {
    return 0.2126 * (bgra[2] / 255.0) + 0.7152 * (bgra[1] / 255.0) + 0.0722 * (bgra[0] / 255.0);
}

}  // namespace

int main() {
    SetLanguage(Language::kEn);
    // 判据本体里的 message 只用得着 Msg/Msgf（诊断文案），取值全走 ASCII；固定成英文即可，
    // 四种语言的一致性由 check-lang.ps1 现场判，不在这里逐语跑。

    // ---- 登记表一致性：每张 CaptureScope 路径都要在 HDR 表里查得到，通道级判断也要对得上 ----
    Section("登记表与通道级判断一致");
    {
        bool allCovered = true;
        for (const auto& e : RegisteredCapturePaths()) {
            if (e.path != paths::kUnknown) {
                bool found = false;
                for (const auto& h : RegisteredHdrPaths())
                    if (std::wstring(h.path) == e.path) found = true;
                if (!found) allCovered = false;
            }
        }
        Check(allCovered, "每条已登记的内部路径都在 HDR 表里有一行");
        // 通道级那句 ChannelCarriesWideColorFrame 必须与"查这条通道的窗口路径"打表一致。
        const CaptureMethod methods[] = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                         CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt,
                                         CaptureMethod::kDuplication};
        bool channelAgrees = true;
        for (CaptureMethod m : methods) {
            const HdrCapability capWin = HdrCapabilityOfPath(WindowPathOf(m));
            const bool wideViaTable = capWin == HdrCapability::kWideGamutCapable;
            if (wideViaTable != ChannelCarriesWideColorFrame(m)) channelAgrees = false;
        }
        Check(channelAgrees, "通道级『带不带广色域帧』与查窗口路径得到的登记一致");
        // 屏幕那两条：wgc / duplication 在屏幕上仍是 wide，bitblt 仍 sdr-only。
        Check(HdrCapabilityOfPath(ScreenPathOf(CaptureMethod::kWgc)) ==
                  HdrCapability::kWideGamutCapable,
              "screen.wgc 登记成带得回广色域");
        Check(HdrCapabilityOfPath(ScreenPathOf(CaptureMethod::kBitBlt)) ==
                  HdrCapability::kSdrSourceOnly,
              "screen.bitblt 登记成只带 8 位 SDR");
        Check(HdrCapabilityOfPath(paths::kUnknown) == HdrCapability::kUnregistered,
              "未登记（含 unknown）一律 kUnregistered");
    }

    // ---- DXGI_FORMAT -> 来源色彩空间 / 位深 / 每像素字节 ----
    Section("来源格式分类");
    {
        Check(FrameColorSpaceFromDxgiFormat(87u) == FrameColorSpace::kSrgbBgra8,
              "87=B8G8R8A8_UNORM -> srgb_bgra8");
        Check(FrameColorSpaceFromDxgiFormat(10u) == FrameColorSpace::kScRgbFloat16,
              "10=R16G16B16A16_FLOAT -> scrgb_float");
        Check(FrameColorSpaceFromDxgiFormat(61u) == FrameColorSpace::kPqBt2020,
              "61=R10G10B10A2_UNORM -> pq_bt2020");
        Check(FrameColorSpaceFromDxgiFormat(999u) == FrameColorSpace::kUnknown,
              "认不出的格式 -> unknown（不猜某一种 HDR）");
        Check(FrameColorSpaceBitDepth(FrameColorSpace::kScRgbFloat16) == 16 &&
                  FrameColorSpaceBitDepth(FrameColorSpace::kPqBt2020) == 10 &&
                  FrameColorSpaceBitDepth(FrameColorSpace::kSrgbBgra8) == 8 &&
                  FrameColorSpaceBitDepth(FrameColorSpace::kUnknown) == 0,
              "位深：16 / 10 / 8 / unknown=0");
        Check(FrameColorSpaceBytesPerPixel(FrameColorSpace::kScRgbFloat16) == 8 &&
                  FrameColorSpaceBytesPerPixel(FrameColorSpace::kPqBt2020) == 4,
              "每像素字节：scRGB=8、10 位包=4");
        Check(FrameColorSpaceIsHdr(FrameColorSpace::kScRgbFloat16) &&
                  !FrameColorSpaceIsHdr(FrameColorSpace::kSrgbBgra8) &&
                  !FrameColorSpaceIsHdr(FrameColorSpace::kUnknown),
              "IsHdr 只对确凿的广色域为真（unknown 不为真也不为 SDR）");
    }

    // ---- 显示 color space -> HDR 状态（三值，认不出不猜）----
    Section("显示 HDR 状态分类");
    {
        Check(DisplayHdrStateOfDxgiColorSpace(2u) == DisplayHdrState::kHdr &&
                  DisplayHdrStateOfDxgiColorSpace(12u) == DisplayHdrState::kHdr &&
                  DisplayHdrStateOfDxgiColorSpace(17u) == DisplayHdrState::kHdr,
              "G2084 / HLG / xCCR 那几条判成 HDR");
        Check(DisplayHdrStateOfDxgiColorSpace(0u) == DisplayHdrState::kSdr &&
                  DisplayHdrStateOfDxgiColorSpace(1u) == DisplayHdrState::kSdr,
              "sRGB / scRGB 判成 SDR");
        Check(DisplayHdrStateOfDxgiColorSpace(9999u) == DisplayHdrState::kUnknown,
              "认不出的 color space -> unknown（不猜 HDR 也不猜 SDR）");
    }

    // ---- 单点数学：half 解码 + 传递函数 + tone 曲线的性质 ----
    Section("单点数学");
    {
        Check(HalfToFloat(0x0000) == 0.0f, "half 0x0000 -> 0");
        Check(HalfToFloat(0x3C00) == 1.0f, "half 0x3C00 -> 1.0");
        Check(HalfToFloat(0x3800) == 0.5f, "half 0x3800 -> 0.5");
        Check(HalfToFloat(0xBC00) == -1.0f, "half 0xBC00 -> -1.0");
        Check(std::fabs(HalfToFloat(0x0001) - std::ldexp(1.0f, -24)) < 1e-12f,
              "half 最小非规格化 0x0001 -> 2^-24");
        // 往返：几个正常值 FloatToHalf->HalfToFloat 一致。
        bool rt = true;
        for (float v : {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f})
            if (std::fabs(HalfToFloat(FloatToHalf(v)) - v) > 1e-3f) rt = false;
        Check(rt, "FloatToHalf/HalfToFloat 在正常范围往返一致");
        Check(SrgbOetf(0.0) == 0.0 && std::fabs(SrgbOetf(1.0) - 1.0) < 1e-9, "sRGB OETF 端点 0/1");
        Check(SrgbOetf(0.18) > SrgbOetf(0.05) && SrgbOetf(0.18) < SrgbOetf(0.5),
              "sRGB OETF 单调");
        Check(PqEotfToNits(0.0) == 0.0, "PQ EOTF：0 -> 0 nits");
        Check(std::fabs(PqEotfToNits(1.0) - 10000.0) < 1.0, "PQ EOTF：1 -> 10000 nits");
        Check(PqEotfToNits(0.5) > PqEotfToNits(0.25) && PqEotfToNits(0.75) > PqEotfToNits(0.5),
              "PQ EOTF 单调");
        Check(HlgInverseEotf(0.0) == 0.0 && HlgInverseEotf(1.0) > 0.99 &&
                  HlgInverseEotf(1.0) <= 1.0001,
              "HLG 反 OETF：0->0、1->~1");
        // tone 曲线：黑进黑、white 进 1、white=1 时退化为恒等、单调不越界。
        Check(ToneMapRelativeLuminance(0.0, 4.0) == 0.0, "tone：0 进 0 出");
        Check(std::fabs(ToneMapRelativeLuminance(4.0, 4.0) - 1.0) < 1e-9, "tone：white 进 1 出");
        bool identity = true;
        for (double L = 0.0; L <= 1.0; L += 0.1)
            if (std::fabs(ToneMapRelativeLuminance(L, 1.0) - L) > 1e-9) identity = false;
        Check(identity, "tone：white=1 时该曲线是恒等（这正是 SDR 不被映射歪的理由）");
        Check(ToneMapRelativeLuminance(3.0, 4.0) > ToneMapRelativeLuminance(2.0, 4.0), "tone 单调");
        Check(ToneMapRelativeLuminance(1000.0, 4.0) <= 1.0 + 1e-9, "tone：任何输入都不越过 1");
    }

    // ---- 广色域帧 -> 8 位 BGRA：已知色块 + 亮度梯度 + 形状与来源守卫 ----
    Section("ConvertWideFrameToSdrBgra8：已知色块");
    {
        // scRGB：黑、白、纯红三块。
        auto f = ScRgbFrame({{0, 0, 0, 1}, {1, 1, 1, 1}, {1, 0, 0, 1}});
        Diagnostic err;
        const bool ok = ConvertWideFrameToSdrBgra8(&f, nullptr, &err);
        Check(ok, "scRGB 三块帧映射成功");
        if (ok) {
            Check(f.width == 3 && f.height == 1 && f.stride == 12, "交付仍是同尺寸、紧凑 BGRA8");
            Check(f.sourceColorSpace == FrameColorSpace::kScRgbFloat16 && f.toneMapped,
                  "保留映射前来源为 scRGB、标记 toneMapped");
            uint8_t blk[4], wht[4], red[4];
            ReadBgra(f, 0, 0, blk);
            ReadBgra(f, 1, 0, wht);
            ReadBgra(f, 2, 0, red);
            Check(blk[0] + blk[1] + blk[2] == 0, "scRGB 黑 -> 交付黑");
            Check(LumaOf(wht) > LumaOf(blk), "scRGB 白比黑亮");
            Check(LumaOf(wht) < 1.0, "SDR 白被固定曲线压到峰值之下（不是硬顶 255，而是映射过）");
            Check(red[2] > red[0] && red[2] > red[1], "scRGB 纯红 -> 交付里红通道最高（色相保住了）");
            Check(blk[3] == 255 && wht[3] == 255, "alpha 直通为不透明 255");
        }
    }
    Section("ConvertWideFrameToSdrBgra8：亮度梯度单调");
    {
        std::vector<std::array<float, 4>> ramp;
        for (int i = 0; i <= 20; ++i) {
            const float v = static_cast<float>(i) / 10.0f;  // 0..2，跨过 SDR 白
            ramp.push_back({v, v, v, 1.0f});
        }
        auto f = ScRgbFrame(ramp);
        Diagnostic err;
        const bool ok = ConvertWideFrameToSdrBgra8(&f, nullptr, &err);
        Check(ok, "灰阶梯度帧映射成功");
        if (ok) {
            bool mono = true;
            double prev = -1.0;
            for (uint32_t x = 0; x < f.width; ++x) {
                uint8_t px[4];
                ReadBgra(f, x, 0, px);
                const double l = LumaOf(px);
                if (l + 1e-9 < prev) mono = false;  // 允许相等，不允许回头
                prev = l;
            }
            Check(mono, "输入越亮 -> 交付亮度非递减（tone mapping 不制造反转）");
            uint8_t lo[4], hi[4];
            ReadBgra(f, 0, 0, lo);
            ReadBgra(f, f.width - 1, 0, hi);
            Check(LumaOf(hi) > LumaOf(lo), "梯度首尾确有明暗差");
        }
    }
    Section("ConvertWideFrameToSdrBgra8：10 位 PQ");
    {
        // 0 = 黑；(600,600,600) 中等；峰值用满量程；alpha 用两位。
        auto f = R10Frame({{0, 0, 0, 3}, {600, 600, 600, 3}, {1023, 1023, 1023, 3}});
        Diagnostic err;
        const bool ok = ConvertWideFrameToSdrBgra8(&f, nullptr, &err);
        Check(ok, "PQ 10 位帧映射成功");
        if (ok) {
            Check(f.stride == f.width * 4u && f.sourceColorSpace == FrameColorSpace::kPqBt2020 &&
                      f.toneMapped,
                  "交付紧凑 BGRA8、保留 PQ 来源、标记映射");
            uint8_t blk[4], mid[4], hi[4];
            ReadBgra(f, 0, 0, blk);
            ReadBgra(f, 1, 0, mid);
            ReadBgra(f, 2, 0, hi);
            Check(blk[0] + blk[1] + blk[2] == 0, "PQ 编码 0 -> 交付黑");
            Check(LumaOf(mid) > LumaOf(blk) && LumaOf(hi) >= LumaOf(mid), "PQ 梯度非递减");
            Check(blk[3] == 255, "2 位 alpha 满值 -> 255");
        }
    }
    Section("ConvertWideFrameToSdrBgra8：来源与形状守卫");
    {
        // 来源写 SDR：不该走这里，返回 false 且不动像素。
        auto f = ScRgbFrame({{1, 1, 1, 1}});
        f.sourceColorSpace = FrameColorSpace::kSrgbBgra8;
        Diagnostic err;
        Check(!ConvertWideFrameToSdrBgra8(&f, nullptr, &err) &&
                  err.code == codes::kHdrUnverifiable,
              "来源不是广色域 -> hdr_unverifiable（映射函数不被误用）");
        // 来源写 unknown：同样拒。
        auto g = ScRgbFrame({{1, 1, 1, 1}});
        g.sourceColorSpace = FrameColorSpace::kUnknown;
        Diagnostic err2;
        Check(!ConvertWideFrameToSdrBgra8(&g, nullptr, &err2), "来源认不出 -> 不映射");
        // 行距太小（装不下一行 FP16）：形状不合法，不越界读。
        auto h = ScRgbFrame({{1, 1, 1, 1}, {1, 1, 1, 1}});
        h.stride = 4u;  // 2 像素 FP16 要 16 字节，这里给 4 -> kStrideTooSmall
        Diagnostic err3;
        Check(!ConvertWideFrameToSdrBgra8(&h, nullptr, &err3),
              "来源形状说不通 -> 拒绝，不读越界");
    }

    // ---- MakeHdrReport：三个键 + 来源色彩空间 + 位深的合成 ----
    Section("MakeHdrReport 合成");
    {
        HdrRequest none;  // given=false
        Check(!MakeHdrReport(none, paths::kWgc, FrameColorSpace::kScRgbFloat16).written,
              "没写 --hdr：written=false（那组键一个都不出现）");
        HdrRequest tonemap;
        tonemap.given = true;
        tonemap.policy = HdrPolicy::kToneMap;
        auto a = MakeHdrReport(tonemap, paths::kWgc, FrameColorSpace::kScRgbFloat16);
        Check(a.written && a.requested == L"tonemap" && a.effective == hdr_effective::kToneMapped &&
                  a.basis == hdr_basis::kScRgbToneMapped && a.sourceColorSpace == L"scrgb_float" &&
                  a.bitDepthKnown && a.bitDepth == 16,
              "tonemap + scRGB：tone_mapped / scrgb basis / 位深 16");
        auto pq = MakeHdrReport(tonemap, paths::kDuplicationFrame, FrameColorSpace::kPqBt2020);
        Check(pq.effective == hdr_effective::kToneMapped && pq.basis == hdr_basis::kPqToneMapped &&
                  pq.bitDepth == 10,
              "tonemap + PQ：位深 10");
        auto sdrOnWide = MakeHdrReport(tonemap, paths::kWgc, FrameColorSpace::kSrgbBgra8);
        Check(sdrOnWide.effective == hdr_effective::kSdrPassthrough &&
                  sdrOnWide.basis == hdr_basis::kDeliveredBgra8Sdr && sdrOnWide.bitDepth == 8,
              "wgc 上带回 SDR：passthrough + delivered_bgra8_sdr");
        auto sdrOnSdr = MakeHdrReport(tonemap, paths::kPrintWindow, FrameColorSpace::kSrgbBgra8);
        Check(sdrOnSdr.basis == hdr_basis::kPathSdrSource,
              "printwindow 上 SDR 来源：basis 说这条路径结构上没有 HDR");
        auto unk = MakeHdrReport(tonemap, paths::kWgc, FrameColorSpace::kUnknown);
        Check(unk.effective == hdr_effective::kUnverified && unk.basis == hdr_basis::kFormatUnrecognized &&
                  !unk.bitDepthKnown,
              "认不出的格式：unverified，且不写位深（问不出来 ≠ 某一位深）");
        HdrRequest refuse;
        refuse.given = true;
        refuse.policy = HdrPolicy::kRefuse;
        auto r = MakeHdrReport(refuse, paths::kWgc, FrameColorSpace::kSrgbBgra8);
        Check(r.requested == L"refuse", "requested 写的是规范化后的策略名");
    }

    // ---- HdrRequestPossible：结构与策略的组合 ----
    Section("HdrRequestPossible");
    {
        Check(HdrRequestPossible(CaptureMethod::kPrintWindow, HdrPolicy::kAuto),
              "auto 恒可行（与本机版本、目标无关）");
        Check(!HdrRequestPossible(CaptureMethod::kPrintWindow, HdrPolicy::kToneMap) &&
                  !HdrRequestPossible(CaptureMethod::kDwmThumbnail, HdrPolicy::kRefuse) &&
                  !HdrRequestPossible(CaptureMethod::kBitBlt, HdrPolicy::kToneMap),
              "tonemap/refuse 配 printwindow/dwm/bitblt 结构上做不到");
        Check(HdrRequestPossible(CaptureMethod::kWgc, HdrPolicy::kToneMap) &&
                  HdrRequestPossible(CaptureMethod::kDuplication, HdrPolicy::kRefuse),
              "tonemap/refuse 配 wgc/duplication 可行");
        Check(HdrRequestPossible(CaptureMethod::kAuto, HdrPolicy::kToneMap),
              "tonemap 配 auto 放行（落到哪条通道要到运行期才知道）");
    }

    std::printf("\n共 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
