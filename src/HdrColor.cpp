#include "HdrColor.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "CaptureCommon.h"   // CapturedFrame 的真身 + CaptureError
#include "Deadline.h"
#include "ImageOps.h"

namespace ecapture {
namespace {

// ---------------------------------------------------------------------------
// 一条固定的 tone mapping 曲线用到的常数。全都写死、不随内容变，所以同一张帧每一次映射
// 逐像素相同（这是"确定的 tone mapping"的字面意思），也全部可以被离线判据逐条核对。
//
// 归一化：每种来源都先解成"相对线性、SDR 参考白 = 1.0"的那一套量，再走同一条曲线。
// ---------------------------------------------------------------------------
constexpr double kToneMapRelativeWhite = 4.0;  // 假设的高光上限（相对 SDR 白的倍数）
constexpr double kPqPeakNits = 10000.0;        // ST 2084 的绝对峰值
constexpr double kPqSdrWhiteNits = 100.0;      // PQ 那一档里 SDR 参考白按 100 cd/m² 归一
constexpr double kHlgSdrWhiteLinear = 0.203;   // HLG 相对线性里 SDR 参考白所占的那一段
constexpr double kLumaR = 0.2126, kLumaG = 0.7152, kLumaB = 0.0722;  // BT.709 亮度系数

// BT.2020 -> BT.709 的线性原色转换（D65，行向量乘 3×3，取常用那一份近似值）。
// scRGB 与 sRGB 同原色，所以那条不用乘这份矩阵。
void Bt2020ToBt709(double r, double g, double b, double* o) {
    o[0] = 1.6604910 * r + -0.5876420 * g + -0.0728499 * b;
    o[1] = -0.1243836 * r + 1.1329030 * g + -0.0083825 * b;
    o[2] = -0.0181508 * r + -0.1005790 * g + 1.1187297 * b;
}

inline double ClampUnit(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

inline uint8_t ToU8(double v01) {
    // 就近取整并夹到 [0,255]；v01 期望已经在 [0,1]，这里再夹一次纯属兜底。
    double s = v01 * 255.0 + 0.5;
    if (s < 0.0) s = 0.0;
    if (s > 255.0) s = 255.0;
    return static_cast<uint8_t>(s);
}

// 把一个像素的相对线性 RGB（SDR 白=1.0，可能 >1 或 <0）映射成 8 位 sRGB BGRA 四字节。
// 亮度用一条固定的扩展 Reinhard 曲线压，再把同样的比例因子乘回三通道（保色相），
// 逐通道夹到 [0,1] 后过 sRGB 正 OETF。alpha 不参与 tone mapping（它不是亮度）。
void MapRelativeLinearToSdrBgra(double rLin, double gLin, double bLin, double alpha01,
                                uint8_t* bgra) {
    if (rLin < 0.0) rLin = 0.0;
    if (gLin < 0.0) gLin = 0.0;
    if (bLin < 0.0) bLin = 0.0;
    const double y = kLumaR * rLin + kLumaG * gLin + kLumaB * bLin;
    const double yt = ToneMapRelativeLuminance(y, kToneMapRelativeWhite);
    // Y=0 时没有比例可乘（三通道本来就都是 0），比例取 1，结果仍是黑。
    const double scale = y > 1e-9 ? (yt / y) : 1.0;
    const double r = ClampUnit(rLin * scale);
    const double g = ClampUnit(gLin * scale);
    const double b = ClampUnit(bLin * scale);
    bgra[0] = ToU8(SrgbOetf(b));
    bgra[1] = ToU8(SrgbOetf(g));
    bgra[2] = ToU8(SrgbOetf(r));
    bgra[3] = ToU8(ClampUnit(alpha01));
}

// 从半精度数组里读一像素（scRGB FP16，RGBA 顺序，各 2 字节）。
void DecodeScRgbPixel(const uint16_t* rgba, double* outLin, double* alpha) {
    outLin[0] = static_cast<double>(HalfToFloat(rgba[0]));  // R
    outLin[1] = static_cast<double>(HalfToFloat(rgba[1]));  // G
    outLin[2] = static_cast<double>(HalfToFloat(rgba[2]));  // B
    *alpha = static_cast<double>(HalfToFloat(rgba[3]));     // A（[0,1] 直通）
}

// 从 10 位打包（R10G10B10A2）里读一像素，PQ 或 HLG 各按传入的解码函数走。
void DecodeR10Pixel(uint32_t packed, bool hlg, double* outRel, double* alpha) {
    const double r = static_cast<double>((packed >> 0) & 0x3FFu) / 1023.0;
    const double g = static_cast<double>((packed >> 10) & 0x3FFu) / 1023.0;
    const double b = static_cast<double>((packed >> 20) & 0x3FFu) / 1023.0;
    // A2 只有两位，映射成 [0,1] 时用 /3。
    const double a = static_cast<double>((packed >> 30) & 0x3u) / 3.0;
    // 解到"相对线性、SDR 白 = 1.0"：PQ 先问绝对亮度再按参考白归一，HLG 直接是相对线性再除以白段。
    if (hlg) {
        outRel[0] = HlgInverseEotf(r) / kHlgSdrWhiteLinear;
        outRel[1] = HlgInverseEotf(g) / kHlgSdrWhiteLinear;
        outRel[2] = HlgInverseEotf(b) / kHlgSdrWhiteLinear;
    } else {
        outRel[0] = PqEotfToNits(r) / kPqSdrWhiteNits;
        outRel[1] = PqEotfToNits(g) / kPqSdrWhiteNits;
        outRel[2] = PqEotfToNits(b) / kPqSdrWhiteNits;
    }
    *alpha = a;
}

}  // namespace

// ---------------------------------------------------------------------------
// 单点数学（都是纯函数，tests\hdr_state.cpp 逐点判它们）
// ---------------------------------------------------------------------------

float HalfToFloat(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            bits = sign;  // ±0
        } else {
            // 半精度非规格化 → 规格化进单精度。先把最高位搬到隐式位，同时倒推指数。
            uint32_t e = 127u - 15u + 1u;
            while ((mantissa & 0x400u) == 0u) {
                mantissa <<= 1;
                --e;
            }
            mantissa &= 0x3FFu;
            bits = sign | (e << 23) | (mantissa << 13);
        }
    } else if (exponent == 31u) {
        bits = sign | 0x7F800000u | (mantissa << 23);  // inf / nan
    } else {
        const uint32_t e = exponent - 15u + 127u;
        bits = sign | (e << 23) | (mantissa << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

double PqEotfToNits(double encoded) {
    // ST 2084 反传递函数：编码 [0,1] → 绝对亮度 [0,10000] cd/m²。常数取 SMPTE ST 2084 定义值。
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double c3 = 2392.0 / 4096.0 * 32.0;
    double e = encoded < 0.0 ? 0.0 : (encoded > 1.0 ? 1.0 : encoded);
    const double pm = std::pow(e, 1.0 / m2);
    const double num = pm - c1;
    if (num <= 0.0) return 0.0;
    const double den = c2 - c3 * pm;
    if (den <= 0.0) return kPqPeakNits;  // 分母塌到 0：这一档顶到峰值，不返回一个无穷
    const double y = std::pow(num / den, 1.0 / m1);
    if (!(y >= 0.0)) return 0.0;
    return y * kPqPeakNits > kPqPeakNits ? kPqPeakNits : y * kPqPeakNits;
}

double HlgInverseEotf(double encoded) {
    // ITU-R BT.2102 HLG 反 OETF（相对线性 [0,1]，不含系统色那一步）。
    constexpr double a = 0.17883277;
    constexpr double b = 1.0 - 4.0 * a;
    constexpr double c = 0.55991073;
    double e = encoded < 0.0 ? 0.0 : (encoded > 1.0 ? 1.0 : encoded);
    if (e <= 0.5) return (e * e) / 3.0;
    return (std::exp((e - c) / a) + b) / 12.0;
}

double SrgbOetf(double linear) {
    // sRGB / BT.709 正传递函数。linear 期望在 [0,1]（>1 的先夹一次，避免编码出 >1）。
    const double v = linear < 0.0 ? 0.0 : (linear > 1.0 ? 1.0 : linear);
    if (v <= 0.0031308) return 12.92 * v;
    return 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

double ToneMapRelativeLuminance(double linearLuminance, double white) {
    // 扩展 Reinhard（ETwP）：L*(1+L/white^2)/(1+L)。黑进黑、white 进 1、全程单调不硬剪枝。
    // white<=0 没有意义（参考白必须是正数），按 1.0 处理成恒等；white=1 时该式退化为 L 本身。
    const double L = linearLuminance < 0.0 ? 0.0 : linearLuminance;
    const double w = white > 0.0 ? white : 1.0;
    const double out = L * (1.0 + L / (w * w)) / (1.0 + L);
    if (!(out >= 0.0)) return 0.0;
    return out > 1.0 ? 1.0 : out;
}

// ---------------------------------------------------------------------------
// 一帧广色域来源 -> 8 位 BGRA sRGB（就地换掉 frame->pixels）
// ---------------------------------------------------------------------------

bool ConvertWideFrameToSdrBgra8(CapturedFrame* frame, const Deadline* dl, Diagnostic* err) {
    if (!frame) return false;
    const FrameColorSpace src = frame->sourceColorSpace;
    // 只有三条确凿的广色域路线走这里；SDR 那条不需要映射，认不出的那条由调用方在更早处拦下。
    const bool isScRgb = src == FrameColorSpace::kScRgbFloat16;
    const bool isPq = src == FrameColorSpace::kPqBt2020;
    const bool isHlg = src == FrameColorSpace::kHlgBt2020;
    if (!isScRgb && !isPq && !isHlg) {
        CaptureError(err, frame->source.c_str(),
                     Msgf(L"cap.hdr_unverifiable", FrameColorSpaceName(src)),
                     Msg(L"cap.hdr_unverifiable_hint"), codes::kHdrUnverifiable);
        return false;
    }
    // 来源那一帧的形状必须先说得通（行距、缓冲区、单边与整帧上限），否则下面按行搬会读越界。
    const uint32_t srcBpp = FrameColorSpaceBytesPerPixel(src);
    {
        const FrameShapeInfo srcInfo{frame->width, frame->height, frame->stride,
                                     static_cast<uint64_t>(frame->pixels.size()), srcBpp};
        const FrameShape s = CheckFrameShape(srcInfo);
        if (s != FrameShape::kOk) {
            FrameShapeError(s, srcInfo, frame->source.c_str(), stages::kCapture, err);
            if (err) err->code = codes::kHdrUnverifiable;   // 广色域帧形状坏了：认不出，别硬映射
            return false;
        }
    }
    // 目标紧凑 BGRA8：分配之前先判它自身的形状与上限（守内存预算这条线）。
    const uint64_t dstStride = static_cast<uint64_t>(frame->width) * 4ull;
    const uint64_t dstBytes = dstStride * frame->height;
    const FrameShape dstShape =
        CheckFrameShape(FrameShapeInfo{frame->width, frame->height,
                                       static_cast<uint32_t>(dstStride), dstBytes, 4u});
    if (dstShape != FrameShape::kOk) {
        FrameShapeError(dstShape, FrameShapeInfo{frame->width, frame->height,
                                                 static_cast<uint32_t>(dstStride), dstBytes, 4u},
                        frame->source.c_str(), stages::kCapture, err);
        return false;
    }
    // 预算已经花光就别开工：这一趟是逐像素的线性扫描，一旦开始只有整帧粒度能停。
    if (dl && dl->Spent()) {
        if (err) *err = BudgetSpent(*dl, codes::kCaptureTimeout, stages::kCapture,
                                    frame->source.empty() ? nullptr : frame->source.c_str());
        return false;
    }

    std::vector<uint8_t> dst(static_cast<size_t>(dstBytes), 0);
    const uint8_t* in = frame->pixels.data();
    for (uint32_t y = 0; y < frame->height; ++y) {
        const uint8_t* rowIn = in + static_cast<size_t>(y) * frame->stride;
        uint8_t* rowOut = dst.data() + static_cast<size_t>(y) * dstStride;
        for (uint32_t x = 0; x < frame->width; ++x) {
            double rel[3] = {0.0, 0.0, 0.0};
            double alpha = 1.0;
            if (isScRgb) {
                const uint16_t* px = reinterpret_cast<const uint16_t*>(rowIn) + static_cast<size_t>(x) * 4;
                DecodeScRgbPixel(px, rel, &alpha);  // scRGB 已是相对线性，SDR 白=1.0
            } else {
                const uint32_t packed =
                    *reinterpret_cast<const uint32_t*>(rowIn + static_cast<size_t>(x) * 4);
                DecodeR10Pixel(packed, isHlg, rel, &alpha);
                double g709[3];
                Bt2020ToBt709(rel[0], rel[1], rel[2], g709);  // BT.2020 -> BT.709
                rel[0] = g709[0];
                rel[1] = g709[1];
                rel[2] = g709[2];
            }
            MapRelativeLinearToSdrBgra(rel[0], rel[1], rel[2], alpha,
                                       rowOut + static_cast<size_t>(x) * 4);
        }
    }

    frame->pixels = std::move(dst);
    frame->stride = static_cast<uint32_t>(dstStride);
    // sourceColorSpace 保留映射前那一份（供结果报告），只有交付的 pixels 变成了 BGRA8。
    frame->toneMapped = true;
    return true;
}

// ---------------------------------------------------------------------------
// 显示 HDR 状态：只读问答（绝不改显示设置）
// ---------------------------------------------------------------------------

DisplayHdrState DisplayHdrStateOfDxgiColorSpace(uint32_t cs) {
    // HDR 那几个 color space（ST 2084/PQ 与 HLG，含 narrow 与 xCCR 变体）→ kHdr。
    switch (cs) {
        case 2u:   // DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        case 5u:   // DXGI_COLOR_SPACE_RGB_NARROW_G2084_NONE_P2020
        case 12u:  // DXGI_COLOR_SPACE_RGB_FULL_G_HLG_NONE_P2020
        case 16u:  // DXGI_COLOR_SPACE_RGB_XCCR_G2084_NONE_P2020
        case 17u:  // DXGI_COLOR_SPACE_RGB_XCCR_G_HLG_NONE_P2020
            return DisplayHdrState::kHdr;
        case 0u:   // RGB_FULL_G22_NONE_P709（sRGB）
        case 1u:   // RGB_FULL_G10_NONE_P709（scRGB，SDR 配置里也用它）
        case 3u:   // RGB_NARROW_G22_NONE_P709
        case 4u:   // RGB_NARROW_G10_NONE_P709
        case 6u: case 7u: case 8u: case 9u: case 10u: case 11u:  // 各种 601/709 非 HDR
        case 13u: case 14u: case 15u:
            return DisplayHdrState::kSdr;
        default:
            return DisplayHdrState::kUnknown;  // 认不出就当没答案，不猜 HDR 也不猜 SDR
    }
}

}  // namespace ecapture
