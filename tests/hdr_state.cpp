// HDR 色彩处理（--hdr）的离线判据（由 tests\hdr.ps1 运行 build\ecapture-hdr-tests.exe）。
//
// 为什么在这一层测：本机没有一台能开 HDR 的显示器，真去带回一幅 FP16 / 10 位帧这件事在这里
// 造不出来 —— 而最要紧的两件判据（tone mapping 的数学对不对、认不认得来源格式与色彩空间）
// 全都是纯算术，可以拿"已知色块 + 亮度梯度"逐点判，不需要任何硬件。测的是生产函数本体
// （src/HdrColor.cpp、src/ImageOps.cpp、那张按路径登记的表），不是测试里另抄一份算法。
//
// 只用 C 风格 printf 汇报；任何一条不过就返回非 0。真机那一条（HDR 显示器上的实拍对照）
// 一律记未验证，不在这里伪造。
//
// 2026-10 起这里还判第二批：显式 HDR 要求的**策略筛选与回退控制**。要的现场是"wgc 交回
// capture.hdr_refused"与"这台机器的链里已经没有任何一条兑现得了 tonemap/refuse"—— 前者要有
// HDR 屏、后者要降版本，本机都给不出，所以判的是生产函数本体：src/HdrColor.cpp 的
// FilterChainForHdr、src/CursorControl.cpp 里串三道闸门的 GateCaptureChain、src/FallbackChain.h
// 那条链（假后端按通道交回什么由测试决定，被调用过哪几条由 called 记下），以及
// JudgeHdrPassiveNote 那一条"8 位帧不等于来源核实是 SDR"。
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

// 这两张分类表的期望值都从 SDK 的符号取，而不是测试里再手抄一份编号（判据本体在
// src/HdrColor.cpp 用的也是同一批符号，但期望值那一侧独立写着符号名）。DXGI_FORMAT_* 与
// DXGI_COLOR_SPACE_* 就是微软定义的那两个枚举；下面另外把几个关键编号（24 / 61 / 87 / 10）
// 也按数字钉一次，这样"符号还在而编号被换掉"那种 SDK 变化也会在这里现形，而不是悄悄把
// 一次分类改动带过去。
#include <dxgi.h>

#include "../src/CaptureCommon.h"
#include "../src/CaptureScope.h"
#include "../src/CliOptions.h"
#include "../src/CursorControl.h"   // GateCaptureChain：版本 / 光标 / HDR 三道闸门串成一份
#include "../src/FallbackChain.h"   // auto 回退链本体（假后端注入进来判它的走法）
#include "../src/HdrColor.h"
#include "../src/Lang.h"
#include "../src/SystemCompat.h"    // GateChannels（组合闸门那三道的第一道）
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
// 这里直接写 sourceColorSpace = kPqBt2020 是**合法但需要前提**的：那一个前提是调用方另外给出了
// 可靠的输出色彩空间（PQ / HLG 之分不在像素格式里，见 FrameColorSpaceFromDxgiFormat 那一段）。
// 这一份判据测的是映射数学本体，所以那份前提由这里代给；本构建的取帧路径自己给不出它，
// 因此那条路的下场是 rgb10a2_unverified + hdr_unverifiable，不是这里这一段。
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

// ---------------------------------------------------------------------------
// 策略筛选与回退控制那两批用的假现场（F07）
// ---------------------------------------------------------------------------
//
// 本机没有 HDR 显示器，也没有一条真通道允许测试去试，所以"后端交回 capture.hdr_refused 之后
// 其余后端一次都不调用"这一类判据只能在这里造：链是闸门筛完交出来的真链（生产函数本体），
// 每条通道"交回什么"由这张表给，而被调用过哪几条、按什么顺序，由 called 记下来 ——
// 那正是这些判据要看的东西，光看返回值看不出来。

HdrRequest Hdr(HdrPolicy policy, bool given) {
    HdrRequest r;
    r.policy = policy;
    r.given = given;
    return r;
}

OsVersion Os(uint32_t build, bool known = true) {
    OsVersion v;
    v.major = 10;
    v.minor = 0;
    v.build = build;
    v.known = known;
    return v;
}

CursorRequest Cur(CursorMode mode, bool given) {
    CursorRequest r;
    r.mode = mode;
    r.given = given;
    return r;
}

// 两条完整的 auto 链（未经任何闸门）：窗口四条、整屏三条，顺序就是回退顺序。
std::vector<CaptureMethod> WindowAll() {
    return {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail, CaptureMethod::kPrintWindow,
            CaptureMethod::kBitBlt};
}
std::vector<CaptureMethod> ScreenAll() {
    return {CaptureMethod::kWgc, CaptureMethod::kDuplication, CaptureMethod::kBitBlt};
}

std::string BriefChain(const std::vector<CaptureMethod>& chain) {
    std::string s;
    for (const CaptureMethod m : chain) {
        if (!s.empty()) s += ",";
        for (const wchar_t* p = CaptureMethodName(m); *p; ++p)
            s.push_back(static_cast<char>(*p));
    }
    return s;
}

std::string Ascii(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out.push_back(static_cast<char>(c));
    return out;
}

// 码 -> 这条失败对回退链意味着什么（判据本体在 src/FallbackChain.h）。
std::string StopName(ChainStop stop) {
    return stop == ChainStop::kStopWithVerdict ? "stop" : "next";
}

// 一次回退链的假后端集合。failCode 与 order 同长，空串 = 这一条成功出一帧。
struct FakeBackends {
    std::vector<CaptureMethod> order;
    std::vector<std::wstring> failCode;
    std::vector<std::wstring> failMessage;
    std::vector<bool> isFatal;
    std::vector<CaptureMethod> called;

    FakeBackends& Fails(CaptureMethod m, const wchar_t* code, const wchar_t* message = L"",
                        bool fatal = false) {
        order.push_back(m);
        failCode.push_back(code ? code : L"");
        failMessage.push_back(message ? message : L"");
        isFatal.push_back(fatal);
        return *this;
    }
    FakeBackends& Succeeds(CaptureMethod m) { return Fails(m, L""); }

    bool operator()(CaptureMethod m, CapturedFrame* out, Diagnostic* err, bool* fat) {
        called.push_back(m);
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] != m) continue;
            if (!failCode[i].empty()) {
                if (err) {
                    err->code = failCode[i];
                    err->message = failMessage[i];
                }
                if (fat && isFatal[i]) *fat = true;
                return false;
            }
            out->width = out->height = 1;
            out->stride = 4;
            out->pixels.assign(4, 0xAB);
            out->source = CaptureMethodName(m);
            out->path = paths::kWgc;
            return true;
        }
        return false;   // 链里出现没登记的通道 = 这份判据自己写坏了
    }
};

// 用假后端跑一次链本体（判的是生产函数，不是这里重抄的走法）。
bool RunChain(FakeBackends* fake, const std::vector<CaptureMethod>& chain, CapturedFrame* out,
              Diagnostic* err, std::vector<Diagnostic>* notes) {
    const Deadline dl;   // 默认 = 不设预算：这批判据只管链的走法，不管预算
    bool fatal = false;
    return FallbackChain(
        chain, dl, out, err, notes, &fatal,
        [&](CaptureMethod m, CapturedFrame* f, Diagnostic* e, bool* fat) {
            return (*fake)(m, f, e, fat);
        });
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
        // 通道级那两句必须与"查这条通道的窗口路径"打表一致，而且问的是两件事：
        //   ChannelCarriesWideColorFrame = 来源带不带得回广色域这件事（含"本构建没兑现"那一条）
        //   ChannelHonorsHdrPolicy       = 显式 tonemap/refuse 兑不兑现得了（只有真做过那一步的）
        const CaptureMethod methods[] = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                         CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt,
                                         CaptureMethod::kDuplication};
        bool channelAgrees = true;
        bool honorsAgrees = true;
        for (CaptureMethod m : methods) {
            const HdrCapability capWin = HdrCapabilityOfPath(WindowPathOf(m));
            const bool wideViaTable = capWin == HdrCapability::kWideGamutCapable ||
                                      capWin == HdrCapability::kWideGamutUnverified;
            if (wideViaTable != ChannelCarriesWideColorFrame(m)) channelAgrees = false;
            const bool honorsViaTable = capWin == HdrCapability::kWideGamutCapable;
            if (honorsViaTable != ChannelHonorsHdrPolicy(m)) honorsAgrees = false;
            // 整屏那一路同类的也要一致：wgc 的两条都兑现得了，复制的两条都没兑现，那几条 8 位的
            // 仍只是 8 位。printwindow / dwm 在屏幕目标上根本没有对应路径（解析期就被
            // capture.unsupported 挡掉），所以那条未登记的 ScreenPathOf 不参与这一比。
            const HdrCapability capScreen = HdrCapabilityOfPath(ScreenPathOf(m));
            if (capScreen != HdrCapability::kUnregistered && capScreen != capWin)
                honorsAgrees = false;
        }
        Check(channelAgrees, "通道级『来源带不带得回广色域帧』与查窗口路径得到的登记一致");
        Check(honorsAgrees,
              "通道级『兑现得了显式 tonemap/refuse』与查表一致，且窗口/整屏两条登记同类");
        // 现场钉死那一条容易被"顺手放开"的差别：duplication 来源可能有广色域，但它兑现不了要求。
        Check(ChannelCarriesWideColorFrame(CaptureMethod::kDuplication) &&
                  !ChannelHonorsHdrPolicy(CaptureMethod::kDuplication),
              "duplication：来源可能带广色域 != 兑现得了显式策略（两句不是一句）");
        Check(ChannelHonorsHdrPolicy(CaptureMethod::kWgc), "wgc 兑现得了显式策略");
        // 屏幕那几条：wgc 在屏幕上仍然兑现得了，duplication 那一条登记成"没兑现"，bitblt 仍 sdr-only。
        Check(HdrCapabilityOfPath(ScreenPathOf(CaptureMethod::kWgc)) ==
                  HdrCapability::kWideGamutCapable,
              "screen.wgc 登记成带得回广色域且兑现得了策略");
        Check(HdrCapabilityOfPath(ScreenPathOf(CaptureMethod::kDuplication)) ==
                      HdrCapability::kWideGamutUnverified &&
                  HdrCapabilityOfPath(paths::kDuplicationFrame) ==
                      HdrCapability::kWideGamutUnverified,
              "桌面复制那两条登记成 wide_gamut_unverified（窗口与整屏两条都是）");
        Check(!HdrPathHonorsPolicy(ScreenPathOf(CaptureMethod::kDuplication)),
              "桌面复制不是显式策略的合格候选（不许拿未证实的『支持 HDR』继续放行）");
        Check(std::wstring(HdrCapabilityName(HdrCapability::kWideGamutUnverified)) ==
                      L"wide_gamut_unverified" &&
                  std::wstring(hdr_reason::kDupPolicyUnverified) ==
                      L"duplication_hdr_policy_not_implemented",
              "新增那个取值与那个原因 token 的机器名钉住（只增不改名）");
        Check(HdrCapabilityOfPath(ScreenPathOf(CaptureMethod::kBitBlt)) ==
                  HdrCapability::kSdrSourceOnly,
              "screen.bitblt 登记成只带 8 位 SDR");
        Check(HdrCapabilityOfPath(paths::kUnknown) == HdrCapability::kUnregistered,
              "未登记（含 unknown）一律 kUnregistered");
        // 漏登记 = 更严：一条没登记的路径既不是合格候选，也说不出"为什么被筛掉"以外的原因。
        Check(!HdrPathFulfilsPolicy(paths::kUnknown, HdrPolicy::kToneMap) &&
                  std::wstring(HdrPolicyBlockReason(paths::kUnknown)) == L"not_registered",
              "未登记 + 显式策略：不当候选，原因写 not_registered");
        Check(HdrPathFulfilsPolicy(paths::kUnknown, HdrPolicy::kAuto),
              "auto 不要求任何处理：连未登记的路径也照旧在链里（默认值不动任何东西）");
    }

    // ---- DXGI_FORMAT -> 来源色彩空间 / 位深 / 每像素字节 ----
    // 期望值的编号来自 SDK 符号本身，而"这个符号等于几"另外钉一次：两张表都不许再靠手抄的
    // 数字（上一版就是手抄错的：10 位包被写成 61，而 61 其实是 R8_UNORM）。
    Section("来源格式分类");
    {
        Check(static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM) == 87u &&
                  static_cast<uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT) == 10u &&
                  static_cast<uint32_t>(DXGI_FORMAT_R10G10B10A2_UNORM) == 24u &&
                  static_cast<uint32_t>(DXGI_FORMAT_R8_UNORM) == 61u,
              "SDK 编号对照：87=BGRA8、10=FP16 RGBA、24=10 位包、61=R8_UNORM（不是 10 位包）");
        Check(FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_B8G8R8A8_UNORM) ==
                  FrameColorSpace::kSrgbBgra8,
              "B8G8R8A8_UNORM -> srgb_bgra8");
        Check(FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_R16G16B16A16_FLOAT) ==
                  FrameColorSpace::kScRgbFloat16,
              "R16G16B16A16_FLOAT -> scrgb_float");
        // 这一条就是本轮修的那个错：布局认得，色彩空间说不出，所以绝不默认成 PQ。
        Check(FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_R10G10B10A2_UNORM) ==
                  FrameColorSpace::kRgb10A2Unverified,
              "R10G10B10A2_UNORM -> rgb10a2_unverified（布局认得出，不冒充某一种 HDR）");
        Check(FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_R10G10B10A2_UNORM) !=
                  FrameColorSpace::kPqBt2020,
              "负向对照：10 位包不等于 PQ（那一问的答案在输出的 color space 上）");
        Check(FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_R8_UNORM) == FrameColorSpace::kUnknown,
              "R8_UNORM（61，上一版被当成 10 位包的那个数）-> unknown");
        Check(FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_R10G10B10A2_UINT) ==
                  FrameColorSpace::kUnknown &&
                  FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM) ==
                      FrameColorSpace::kUnknown,
              "10 位包的 UINT / XR_BIAS 变体也不叫名字（不是那种无符号颜色数据）");
        Check(FrameColorSpaceFromDxgiFormat(DXGI_FORMAT_UNKNOWN) == FrameColorSpace::kUnknown &&
                  FrameColorSpaceFromDxgiFormat(999u) == FrameColorSpace::kUnknown,
              "认不出的格式 -> unknown（不猜某一种 HDR）");
        Check(FrameColorSpaceBitDepth(FrameColorSpace::kScRgbFloat16) == 16 &&
                  FrameColorSpaceBitDepth(FrameColorSpace::kPqBt2020) == 10 &&
                  FrameColorSpaceBitDepth(FrameColorSpace::kSrgbBgra8) == 8 &&
                  FrameColorSpaceBitDepth(FrameColorSpace::kUnknown) == 0,
              "位深：16 / 10 / 8 / 连布局都认不出=0");
        Check(FrameColorSpaceBitDepth(FrameColorSpace::kRgb10A2Unverified) == 10,
              "位深那一条事实与色彩空间那句是分开的：10 位包报得出 10 位");
        Check(FrameColorSpaceBytesPerPixel(FrameColorSpace::kScRgbFloat16) == 8 &&
                  FrameColorSpaceBytesPerPixel(FrameColorSpace::kPqBt2020) == 4 &&
                  FrameColorSpaceBytesPerPixel(FrameColorSpace::kRgb10A2Unverified) == 4,
              "每像素字节：scRGB=8、8 位 BGRA 与 10 位包=4");
        Check(FrameColorSpaceIsHdr(FrameColorSpace::kScRgbFloat16) &&
                  FrameColorSpaceIsHdr(FrameColorSpace::kPqBt2020) &&
                  FrameColorSpaceIsHdr(FrameColorSpace::kHlgBt2020) &&
                  !FrameColorSpaceIsHdr(FrameColorSpace::kSrgbBgra8) &&
                  !FrameColorSpaceIsHdr(FrameColorSpace::kUnknown) &&
                  !FrameColorSpaceIsHdr(FrameColorSpace::kRgb10A2Unverified),
              "IsHdr 只对确凿带得回 HDR 的三种为真（两种说不清的都不为真，也不为 SDR）");
        // 那一条"说不清就不许按 4 字节搬"的下场：映射函数自己拒它，返回前不动像素。
        {
            auto f = R10Frame({{0, 0, 0, 3}, {1023, 1023, 1023, 3}});
            f.sourceColorSpace = FrameColorSpace::kRgb10A2Unverified;
            const std::vector<uint8_t> before = f.pixels;
            Diagnostic err;
            Check(!ConvertWideFrameToSdrBgra8(&f, nullptr, &err) &&
                      err.code == codes::kHdrUnverifiable && f.pixels == before,
                  "10 位包说不清色彩空间 -> hdr_unverifiable，一个字节都不按猜的布局去解释");
        }
    }

    // ---- 显示 color space -> HDR 状态（三值，认不出不猜）----
    // 期望值同样从 SDK 符号取。这一张表上一版也是手抄编号，而且抄错得更实用：它把 2 / 5 / 17
    // 当成 HDR（那三条是 G22 的 studio-range 与 BT.2020 宽色域 SDR），又把 13 / 14 两条真 PQ
    // 的当成 SDR。一幅"HDR 关掉、wide colour 打开"的屏就会答 HDR，让 --hdr refuse 拒掉正常图。
    Section("显示 HDR 状态分类");
    {
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ==
                  DisplayHdrState::kHdr,
              "RGB full G2084 (PQ) BT.2020 -> HDR");
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020) ==
                      DisplayHdrState::kHdr &&
                  DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020) ==
                      DisplayHdrState::kHdr &&
                  DisplayHdrStateOfDxgiColorSpace(
                      DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020) == DisplayHdrState::kHdr,
              "studio/left/topleft 那几条 G2084 变体也都是 HDR（上一版把其中两条记成 SDR）");
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020) ==
                      DisplayHdrState::kHdr &&
                  DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020) ==
                      DisplayHdrState::kHdr,
              "HLG 那两条 -> HDR");
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) ==
                      DisplayHdrState::kSdr &&
                  DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) ==
                      DisplayHdrState::kSdr,
              "sRGB(G22 full) 与 scRGB(G10) -> SDR");
        // 负向对照：原色是 BT.2020 不等于 HDR，传递函数才是那一问的答案。
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020) ==
                      DisplayHdrState::kSdr &&
                  DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020) ==
                      DisplayHdrState::kSdr,
              "BT.2020 原色配 G22（宽色域 SDR 面板那种）-> SDR，不是 HDR");
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709) ==
                      DisplayHdrState::kSdr &&
                  DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601) ==
                      DisplayHdrState::kSdr,
              "G22 的 studio-range 与 YCbCr 601 那些 -> SDR（上一版把这两条记成 HDR）");
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709) ==
                      DisplayHdrState::kSdr &&
                  DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020) ==
                      DisplayHdrState::kSdr,
              "G24 那两条（display-referred，没有 PQ/HLG）-> SDR");
        Check(DisplayHdrStateOfDxgiColorSpace(DXGI_COLOR_SPACE_RESERVED) ==
                      DisplayHdrState::kUnknown,
              "RESERVED -> unknown（占位值不是一句「这块屏此刻是什么」）");
        // 那一个编号是 0xFFFFFFFF，比这个枚举其余取值都大，所以递出去时显式转一次无符号，
        // 免得 /W4 把这一次符号转换报成告警（判的是同一个 SDK 符号，不是另抄的数）。
        Check(DisplayHdrStateOfDxgiColorSpace(static_cast<uint32_t>(DXGI_COLOR_SPACE_CUSTOM)) ==
                      DisplayHdrState::kUnknown,
              "CUSTOM -> unknown（厂商自定义那一句本构建读不懂，既不猜 HDR 也不猜 SDR）");
        Check(DisplayHdrStateOfDxgiColorSpace(9999u) == DisplayHdrState::kUnknown &&
                  DisplayHdrStateOfDxgiColorSpace(0xFFFFFFFEu) == DisplayHdrState::kUnknown,
              "表外编号 -> unknown（认不出就当没答案）");
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
    // 这一批判的是"三份事实各有一个来源，而对外那一句只能由它们合成"：内存布局（source）、
    // 真过了映射的那一份记录（toneMapped）、采集之前那次只读问答（displayState），加上这条路径
    // 在登记表上的那一句（path）。任何一份缺席都不许被另一份顶上。
    Section("MakeHdrReport 合成");
    {
        const DisplayHdrState unk = DisplayHdrState::kUnknown;
        Check(!MakeHdrReport(Hdr(HdrPolicy::kAuto, false), paths::kWgc,
                             FrameColorSpace::kScRgbFloat16, true, unk)
                    .written,
              "没写 --hdr：written=false（那组键一个都不出现）");
        // 真过了映射：tone_mapped 那一句唯一的根据就是那份记录。
        auto a = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kWgc,
                               FrameColorSpace::kScRgbFloat16, true, DisplayHdrState::kHdr);
        Check(a.written && a.requested == L"tonemap" && a.effective == hdr_effective::kToneMapped &&
                  a.basis == hdr_basis::kScRgbToneMapped && a.sourceColorSpace == L"scrgb_float" &&
                  a.bitDepthKnown && a.bitDepth == 16,
              "tonemap + scRGB + 映射记录：tone_mapped / scrgb basis / 位深 16");
        auto pq = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kWgc,
                               FrameColorSpace::kPqBt2020, true, DisplayHdrState::kHdr);
        Check(pq.effective == hdr_effective::kToneMapped && pq.basis == hdr_basis::kPqToneMapped &&
                  pq.bitDepth == 10,
              "tonemap + 一个给出可靠色彩空间的 PQ 帧：tone_mapped / pq basis / 位深 10");
        // 广色域来源而那份映射记录没立起来：不许写 tone_mapped（也不许照 8 位说成透传）。
        auto notApplied = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kWgc,
                                        FrameColorSpace::kScRgbFloat16, false,
                                        DisplayHdrState::kHdr);
        Check(notApplied.effective == hdr_effective::kUnverified &&
                  notApplied.basis == hdr_basis::kToneMapNotApplied,
              "宽格式而那份映射记录没立起来：unverified / tone_map_not_applied（来源那一句不替映射作保）");
        // 10 位包：布局那一条照报，色彩空间那一句留空。
        auto tenBit = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kDuplicationFrame,
                                    FrameColorSpace::kRgb10A2Unverified, false, unk);
        Check(tenBit.effective == hdr_effective::kUnverified &&
                  tenBit.basis == hdr_basis::kTransferFunctionUnknown &&
                  tenBit.sourceColorSpace == L"rgb10a2_unverified" && tenBit.bitDepthKnown &&
                  tenBit.bitDepth == 10,
              "10 位包：unverified / transfer_function_unknown，而位深 10 仍然报得出");
        auto unkFormat = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kWgc,
                                       FrameColorSpace::kUnknown, false, unk);
        Check(unkFormat.effective == hdr_effective::kUnverified &&
                  unkFormat.basis == hdr_basis::kFormatUnrecognized && !unkFormat.bitDepthKnown,
              "连布局都认不出：unverified，且不写位深（问不出来 ≠ 某一位深）");
        // 8 位交付的三条下场：登记表 / 那次问答 / 两者都没有。
        auto sdrOnWide = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kWgc,
                                       FrameColorSpace::kSrgbBgra8, false, DisplayHdrState::kSdr);
        Check(sdrOnWide.effective == hdr_effective::kSdrPassthrough &&
                  sdrOnWide.basis == hdr_basis::kDeliveredBgra8Sdr && sdrOnWide.bitDepth == 8,
              "问过、答案是 SDR：passthrough + delivered_bgra8_sdr（那句核实由那次问答撑着）");
        auto sdrOnSdr = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kPrintWindow,
                                      FrameColorSpace::kSrgbBgra8, false, unk);
        Check(sdrOnSdr.effective == hdr_effective::kSdrPassthrough &&
                  sdrOnSdr.basis == hdr_basis::kPathSdrSource,
              "printwindow 上 8 位交付：basis 说这条路径结构上带不回 HDR，所以不需要那次问答");
        // 本轮修掉的那一格：没问过（--hdr auto 那一路根本不发那次问答）与答非 SDR，
        // 都不许被写成 sdr_passthrough —— 而 --quiet 会把 notes 整段去掉，所以这一格必须自己说话。
        auto neverAsked = MakeHdrReport(Hdr(HdrPolicy::kAuto, true), paths::kWgc,
                                        FrameColorSpace::kSrgbBgra8, false, unk);
        Check(neverAsked.written && neverAsked.requested == L"auto" &&
                  neverAsked.effective == hdr_effective::kUnverified &&
                  neverAsked.basis == hdr_basis::kBgra8SourceUnverified &&
                  neverAsked.sourceColorSpace == L"srgb_bgra8" && neverAsked.bitDepth == 8,
              "--hdr auto（没做过那次问答）：键仍然写，而 effective 自己就是 unverified");
        auto displayHdr = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), paths::kWgc,
                                        FrameColorSpace::kSrgbBgra8, false, DisplayHdrState::kHdr);
        Check(displayHdr.effective == hdr_effective::kUnverified &&
                  displayHdr.basis == hdr_basis::kBgra8SourceUnverified,
              "屏在 HDR 模式而这一张按 8 位交付：同样不许说成 SDR 来源");
        auto probeUnknown = MakeHdrReport(Hdr(HdrPolicy::kRefuse, true), paths::kDuplicationFrame,
                                          FrameColorSpace::kSrgbBgra8, false, unk);
        Check(probeUnknown.effective == hdr_effective::kUnverified,
              "问不出来既不折成 SDR 也不折成 HDR（规矩 5），图的下场由调用方那条提示去说给人听");
        auto refuse = MakeHdrReport(Hdr(HdrPolicy::kRefuse, true), paths::kWgc,
                                    FrameColorSpace::kSrgbBgra8, false, DisplayHdrState::kSdr);
        Check(refuse.requested == L"refuse", "requested 写的是规范化后的策略名");
        // 阴性对照：这一格里没有任何一种"8 位交付"的下场会写 sdr_passthrough 而不带两条根据之一。
        bool onlyEvidence = true;
        const DisplayHdrState states[] = {DisplayHdrState::kSdr, DisplayHdrState::kHdr,
                                          DisplayHdrState::kUnknown};
        const wchar_t* paths2[] = {paths::kWgc, paths::kDuplicationFrame, paths::kPrintWindow,
                                   paths::kDwmThumbnail, paths::kBitBltScreen};
        for (DisplayHdrState st : states) {
            for (const wchar_t* p : paths2) {
                const HdrReport r = MakeHdrReport(Hdr(HdrPolicy::kToneMap, true), p,
                                                  FrameColorSpace::kSrgbBgra8, false, st);
                if (r.effective != hdr_effective::kSdrPassthrough) continue;
                const bool structural = HdrCapabilityOfPath(p) == HdrCapability::kSdrSourceOnly;
                if (!structural && st != DisplayHdrState::kSdr) onlyEvidence = false;
            }
        }
        Check(onlyEvidence, "sdr_passthrough 只在有两条根据之一时才出现（8 位帧自己不算证据）");
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
                  HdrRequestPossible(CaptureMethod::kWgc, HdrPolicy::kRefuse),
              "tonemap/refuse 配 wgc 可行");
        Check(!HdrRequestPossible(CaptureMethod::kDuplication, HdrPolicy::kToneMap) &&
                  !HdrRequestPossible(CaptureMethod::kDuplication, HdrPolicy::kRefuse),
              "tonemap/refuse 配 duplication：本构建没兑现那几步，解析期就不放行（F07 收紧）");
        Check(HdrRequestPossible(CaptureMethod::kDuplication, HdrPolicy::kAuto),
              "duplication 配 auto 照旧放行（auto 不要求任何处理，与这条选项存在之前相同）");
        Check(HdrRequestPossible(CaptureMethod::kAuto, HdrPolicy::kToneMap),
              "tonemap 配 auto 放行（落到哪条通道要到运行期才知道，判据是 FilterChainForHdr）");
    }

    // ---- FilterChainForHdr：显式要求过的策略，回退链每一步都要继续兑现它 ----
    Section("FilterChainForHdr：没要求与 auto 一条都不动");
    {
        const HdrChainGate none = FilterChainForHdr(WindowAll(), Hdr(HdrPolicy::kAuto, false), false);
        Check(BriefChain(none.chain) == "wgc,dwm,printwindow,bitblt" && none.notes.empty() &&
                  none.error.code.empty(),
              "没写 --hdr：窗口链原样、不发 note、不报错（默认值真的不动任何东西）");
        const HdrChainGate autoG = FilterChainForHdr(ScreenAll(), Hdr(HdrPolicy::kAuto, true), true);
        Check(BriefChain(autoG.chain) == "wgc,duplication,bitblt" && autoG.notes.empty() &&
                  autoG.error.code.empty(),
              "写了 --hdr auto：同样一条都不动（auto 只是被动上报，不要求任何处理）");
        const HdrChainGate empty =
            FilterChainForHdr({}, Hdr(HdrPolicy::kToneMap, true), false);
        Check(empty.chain.empty() && empty.error.code.empty() && empty.notes.empty(),
              "上一步（版本/光标闸门）已经给过错误时这里不补第二条（一次请求一条下一步）");
    }

    Section("FilterChainForHdr：tonemap/refuse 只留兑现得了的那几条，各留一条 note");
    {
        const HdrChainGate tm = FilterChainForHdr(WindowAll(), Hdr(HdrPolicy::kToneMap, true), false);
        Check(BriefChain(tm.chain) == "wgc" && tm.error.code.empty(),
              "窗口链 + tonemap：只剩 wgc（回退不再交出兑现不了的那几种）");
        Check(tm.notes.size() == 3, "被摘掉的三条各留一条 note.hdr_channel_skipped");
        bool allSkipped = true, reasons = true, shape = true;
        for (const Diagnostic& n : tm.notes) {
            if (n.code != codes::kNoteHdrChannelSkipped) allSkipped = false;
            if (n.option != L"--hdr" || n.value != L"tonemap" || n.stage != stages::kCapture)
                shape = false;
            if (Ascii(n.message).find("window_self_drawn_8bit") != std::string::npos ||
                Ascii(n.message).find("dwm_redirection_surface_8bit") != std::string::npos ||
                Ascii(n.message).find("screen_dc_8bit") != std::string::npos)
                reasons = true;
        }
        Check(allSkipped && shape, "note 的 code/option/value/stage 都按契约填");
        Check(reasons, "note 里那条原因 token 看得见（8 位那三条各说自己的根据）");

        const HdrChainGate sc = FilterChainForHdr(ScreenAll(), Hdr(HdrPolicy::kRefuse, true), true);
        Check(BriefChain(sc.chain) == "wgc", "整屏链 + refuse：同样只剩 wgc");
        bool dupReason = false, bitbltReason = false;
        for (const Diagnostic& n : sc.notes) {
            if (Ascii(n.message).find("duplication_hdr_policy_not_implemented") != std::string::npos)
                dupReason = true;
            if (Ascii(n.message).find("screen_dc_8bit") != std::string::npos) bitbltReason = true;
        }
        Check(sc.notes.size() == 2 && dupReason && bitbltReason,
              "整屏那两条被摘掉：复制那条的原因写『本构建没实现』而不是『结构上带不回』");

        const HdrChainGate rf = FilterChainForHdr(WindowAll(), Hdr(HdrPolicy::kRefuse, true), false);
        Check(BriefChain(rf.chain) == "wgc" && rf.notes.size() == 3,
              "refuse 与 tonemap 用的是同一条筛选（两条都是显式要求）");
    }

    Section("FilterChainForHdr：一条都不剩就是 env.hdr_unsupported，一张都不取");
    {
        const std::vector<CaptureMethod> sdrOnly = {CaptureMethod::kDwmThumbnail,
                                                    CaptureMethod::kPrintWindow,
                                                    CaptureMethod::kBitBlt};
        const HdrChainGate g = FilterChainForHdr(sdrOnly, Hdr(HdrPolicy::kToneMap, true), false);
        Check(g.chain.empty() && g.error.code == codes::kEnvHdrUnsupported,
              "没有合格候选：链清空 + 一条 env.hdr_unsupported（绝不照能截的那几条先交一张）");
        Check(g.error.option == L"--hdr" && g.error.value == L"tonemap" &&
                  g.error.backend == L"dwm, printwindow, bitblt" && !g.error.hint.empty() &&
                  g.error.stage == stages::kCapture,
              "那条错误把要求与本来要试的哪几条都写出来，且带 hint");
        Check(Ascii(g.error.message).find("window_self_drawn_8bit") != std::string::npos,
              "错误里逐条原因 token 也看得见（调用方不必读源码就知道为什么）");
    }

    Section("GateCaptureChain：三道闸门串起来（版本 → 光标 → HDR），HDR 与光标取交集");
    {
        // 默认值那两条：没写 --cursor / --hdr 时组合结果与 GateChannels 逐字相同。
        const ChannelGate base = GateChannels(CaptureMethod::kAuto, false, Os(19045));
        const ChannelGate plain =
            GateCaptureChain(CaptureMethod::kAuto, false, Os(19045), Cur(CursorMode::kDefault, false),
                             Hdr(HdrPolicy::kAuto, false));
        Check(BriefChain(plain.chain) == BriefChain(base.chain) && plain.notes.size() ==
                  base.notes.size() && plain.error.code.empty(),
              "两个要求都没写时与只过版本闸门的结果逐字相同");

        // 光标 exclude（八条都做得到）+ HDR tonemap：只有 HDR 那一道摘人。
        const ChannelGate ex =
            GateCaptureChain(CaptureMethod::kAuto, false, Os(19045), Cur(CursorMode::kExclude, true),
                             Hdr(HdrPolicy::kToneMap, true));
        Check(BriefChain(ex.chain) == "wgc" && ex.error.code.empty(),
              "exclude + tonemap：交集是 wgc（exclude 谁都能满足，收窄只来自 HDR）");
        bool onlyHdrNotes = !ex.notes.empty();
        for (const Diagnostic& n : ex.notes)
            if (n.code != codes::kNoteHdrChannelSkipped) onlyHdrNotes = false;
        Check(onlyHdrNotes, "这一份的 note 全来自 HDR 那一道（光标那道一条都没摘）");

        // 光标 include（只有 wgc 做得到）+ HDR refuse：两道都只剩 wgc，note 只有一份。
        const ChannelGate inc =
            GateCaptureChain(CaptureMethod::kAuto, false, Os(19045), Cur(CursorMode::kInclude, true),
                             Hdr(HdrPolicy::kRefuse, true));
        Check(BriefChain(inc.chain) == "wgc" && inc.error.code.empty() &&
                  !inc.notes.empty(),
              "include + refuse：交集同样是 wgc，且不会被任何一道换成别家");
        bool noHdrNotes = true;
        for (const Diagnostic& n : inc.notes)
            if (n.code == codes::kNoteHdrChannelSkipped) noHdrNotes = false;
        Check(noHdrNotes, "光标那道已经把链收到 wgc，HDR 这道不再补摘除 note（交集不重复报）");

        // 整屏那一条也一样：auto + tonemap 绝不因为"HDR 要复制那条"而放行桌面路径。
        const ChannelGate screen =
            GateCaptureChain(CaptureMethod::kAuto, true, Os(19045), Cur(CursorMode::kDefault, false),
                             Hdr(HdrPolicy::kToneMap, true));
        Check(BriefChain(screen.chain) == "wgc", "整屏目标 + tonemap：链只剩 wgc（仍然一定问人）");

        // 顺序那两条：版本错误优先；版本筛过之后光标筛空时，不再给 HDR 那条错误。
        const ChannelGate old =
            GateCaptureChain(CaptureMethod::kAuto, false, Os(10240), Cur(CursorMode::kInclude, true),
                             Hdr(HdrPolicy::kToneMap, true));
        Check(old.error.code == codes::kEnvCursorUnsupported,
              "10240 上 include 先筛空 -> 交回光标那条，不再叠一条 HDR 的（一次请求一条下一步）");
        bool bothNotes = false, hasCursorNote = false, hasHdrNote = false;
        for (const Diagnostic& n : old.notes) {
            if (n.code == codes::kNoteCursorChannelSkipped) hasCursorNote = true;
            if (n.code == codes::kNoteHdrChannelSkipped) hasHdrNote = true;
        }
        bothNotes = hasCursorNote;   // HDR 那一道这时链已空，不该有 note
        Check(bothNotes && !hasHdrNote, "光标筛空时 HDR 那道不再发摘除 note");

        const ChannelGate explicitDup =
            GateCaptureChain(CaptureMethod::kDuplication, false, Os(19045),
                             Cur(CursorMode::kDefault, false), Hdr(HdrPolicy::kToneMap, true));
        Check(explicitDup.chain.empty() && explicitDup.error.code == codes::kEnvHdrUnsupported,
              "运行期这一层也绝不把用户点名的 duplication 换成别家（解析期那条 capture.hdr_unsupported "
              "才是调用方实际看到的）");

        const ChannelGate unknownOs =
            GateCaptureChain(CaptureMethod::kAuto, false, Os(0, false),
                             Cur(CursorMode::kDefault, false), Hdr(HdrPolicy::kToneMap, true));
        Check(unknownOs.error.code.empty() && BriefChain(unknownOs.chain) == "wgc",
              "版本问不出来时不按版本筛，但 HDR 那道照旧筛（问不出来不等于合格）");
    }

    Section("ClassifyChainStop：策略结论与授权/身份/致命同一条线");
    {
        Check(StopName(ClassifyChainStop(codes::kHdrRefused)) == "stop" &&
                  StopName(ClassifyChainStop(codes::kHdrUnverifiable)) == "stop",
              "hdr_refused / hdr_unverifiable 是策略结论：整条链就此停下");
        Check(StopName(ClassifyChainStop(codes::kAccessDenied)) == "stop" &&
                  StopName(ClassifyChainStop(codes::kConsentUnavailable)) == "stop" &&
                  StopName(ClassifyChainStop(codes::kConsentTimeout)) == "stop" &&
                  StopName(ClassifyChainStop(codes::kConsentStale)) == "stop",
              "授权那四条仍然终止（保留既有规则）");
        Check(StopName(ClassifyChainStop(codes::kTargetGone)) == "stop" &&
                  StopName(ClassifyChainStop(codes::kTargetChanged)) == "stop" &&
                  StopName(ClassifyChainStop(codes::kTargetUnverifiable)) == "stop",
              "身份那三条仍然终止（保留既有规则）");
        // 负向对照：普通"这条通道不行"的下场绝不因为这次改动被误判成终止。
        Check(StopName(ClassifyChainStop(codes::kCaptureFailed)) == "next" &&
                  StopName(ClassifyChainStop(codes::kFrameTimeout)) == "next" &&
                  StopName(ClassifyChainStop(codes::kWindowGone)) == "next" &&
                  StopName(ClassifyChainStop(codes::kCursorUnverifiable)) == "next" &&
                  StopName(ClassifyChainStop(codes::kHdrUnsupported)) == "next" &&
                  StopName(ClassifyChainStop(std::wstring())) == "next",
              "capture.failed / frame.timeout / window.gone / cursor_unverifiable 仍可换后端");
    }

    Section("FallbackChain + 假后端：hdr_refused 之后其余后端一次都不调用");
    {
        // 注意这里给的是一条"没被 HDR 闸门筛过"的完整链（wgc 之外还剩三条），因为这一判要看的是
        // 链本体遇到策略结论时的走法：闸门那条路（只留 wgc）另有一批判据判。两者任一条漏掉，
        // 结果都会是"用户拒绝过的东西被另一家后端交出来了"。
        FakeBackends fb;
        fb.Fails(CaptureMethod::kWgc, codes::kHdrRefused, L"REFUSED-ORIGINAL")
            .Succeeds(CaptureMethod::kDwmThumbnail)
            .Succeeds(CaptureMethod::kPrintWindow)
            .Succeeds(CaptureMethod::kBitBlt);
        CapturedFrame out;
        Diagnostic err;
        std::vector<Diagnostic> notes;
        const bool ok = RunChain(&fb, WindowAll(), &out, &err, &notes);
        Check(!ok, "refuse 那一路不成立");
        Check(BriefChain(fb.called) == "wgc", "wgc 交回 hdr_refused 后，其余三条一次都没被调用");
        Check(out.pixels.empty() && out.width == 0, "一张图都没交出（没有半张被硬压成的 8 位帧）");
        Check(err.code == codes::kHdrRefused && err.message == L"REFUSED-ORIGINAL",
              "原码原文交回，没有被统一包装成 capture.failed（调用方还看得见原因）");
        Check(notes.empty(), "没有 note.capture_channel：根本没走到第二条后端");

        FakeBackends fb2;
        fb2.Fails(CaptureMethod::kWgc, codes::kHdrUnverifiable, L"UNVERIFIABLE-ORIGINAL")
            .Succeeds(CaptureMethod::kDuplication)
            .Succeeds(CaptureMethod::kBitBlt);
        CapturedFrame out2;
        Diagnostic err2;
        std::vector<Diagnostic> notes2;
        Check(!RunChain(&fb2, ScreenAll(), &out2, &err2, &notes2) &&
                  BriefChain(fb2.called) == "wgc" && err2.code == codes::kHdrUnverifiable &&
                  out2.pixels.empty(),
              "来源认不出时同样终止整条整屏链，原码交出（不许换一家按 8 位猜着交）");
    }

    Section("FallbackChain + 假后端：普通失败照旧往下试，且只试合格的那几条");
    {
        // tonemap 筛完之后链里只有 wgc：它普通失败时不会有第二条后端被试到，包装那条仍说"试过 wgc"。
        const std::vector<CaptureMethod> filtered =
            FilterChainForHdr(WindowAll(), Hdr(HdrPolicy::kToneMap, true), false).chain;
        FakeBackends fb;
        fb.Fails(CaptureMethod::kWgc, codes::kFrameTimeout, L"FRAME-TIMEOUT");
        CapturedFrame out;
        Diagnostic err;
        std::vector<Diagnostic> notes;
        Check(!RunChain(&fb, filtered, &out, &err, &notes), "唯一的合格后端失败：这一张不落地");
        Check(BriefChain(fb.called) == "wgc",
              "tonemap 下首后端失败后没有再动 dwm/printwindow/bitblt（合格路径已经只有它）");
        Check(err.code == codes::kCaptureFailed && err.backend == L"wgc" &&
                  err.hint == L"FRAME-TIMEOUT",
              "链耗尽时包成 capture.failed，但仍写着真实试过的那几条与最后那条原因");
        Check(out.pixels.empty(), "包装那条不伴随任何交付");

        // 负向对照：没要求 HDR 时，链还是四条，第一条普通失败要照旧往下试并留来路提示。
        FakeBackends fb2;
        fb2.Fails(CaptureMethod::kWgc, codes::kFrameTimeout, L"FRAME-TIMEOUT")
            .Succeeds(CaptureMethod::kDwmThumbnail);
        CapturedFrame out2;
        Diagnostic err2;
        std::vector<Diagnostic> notes2;
        Check(RunChain(&fb2, WindowAll(), &out2, &err2, &notes2), "没有 HDR 要求时回退照旧成功");
        Check(BriefChain(fb2.called) == "wgc,dwm", "首后端普通失败后试了第二条");
        Check(notes2.size() == 1 && notes2[0].code == codes::kCaptureChannel,
              "实际用的不是链首时留一条来路提示（这条既有规则没被动到）");

        // 致命错误仍然终止整条链，且原样交出。
        FakeBackends fb3;
        fb3.Fails(CaptureMethod::kWgc, codes::kCaptureFailed, L"E_OUTOFMEMORY", true)
            .Succeeds(CaptureMethod::kDwmThumbnail);
        CapturedFrame out3;
        Diagnostic err3;
        std::vector<Diagnostic> notes3;
        Check(!RunChain(&fb3, WindowAll(), &out3, &err3, &notes3) &&
                  BriefChain(fb3.called) == "wgc" && err3.message == L"E_OUTOFMEMORY",
              "致命（资源/设备没了）立刻终止，换后端不会有区别");
    }

    Section("FallbackChain + 假后端：授权拒绝仍然终止");
    {
        const wchar_t* denied[] = {codes::kAccessDenied, codes::kConsentTimeout,
                                   codes::kConsentStale, codes::kConsentUnavailable};
        bool allStop = true;
        for (const wchar_t* code : denied) {
            FakeBackends fb;
            fb.Fails(CaptureMethod::kWgc, code, L"D").Succeeds(CaptureMethod::kDwmThumbnail);
            CapturedFrame out;
            Diagnostic err;
            std::vector<Diagnostic> notes;
            if (RunChain(&fb, WindowAll(), &out, &err, &notes) || BriefChain(fb.called) != "wgc" ||
                err.code != code || !out.pixels.empty()) {
                allStop = false;
            }
        }
        Check(allStop, "四条授权结论各自都终止整条链、原码交出、一张都不落地");
    }

    Section("JudgeHdrPassiveNote：一张 8 位帧不证明来源是 SDR");
    {
        Check(JudgeHdrPassiveNote(Hdr(HdrPolicy::kToneMap, false), FrameColorSpace::kSrgbBgra8,
                                  DisplayHdrState::kUnknown) == HdrPassiveNote::kNone,
              "没写 --hdr：一条提示都不发（那组键也整个不出现）");
        Check(JudgeHdrPassiveNote(Hdr(HdrPolicy::kAuto, true), FrameColorSpace::kSrgbBgra8,
                                  DisplayHdrState::kSdr) == HdrPassiveNote::kNone,
              "--hdr auto：只被动上报，不发提示（既有语义不变）");
        Check(JudgeHdrPassiveNote(Hdr(HdrPolicy::kToneMap, true), FrameColorSpace::kScRgbFloat16,
                                  DisplayHdrState::kHdr) == HdrPassiveNote::kNone,
              "真带回广色域并映射过：不发这两条里的任何一条");
        Check(JudgeHdrPassiveNote(Hdr(HdrPolicy::kToneMap, true), FrameColorSpace::kSrgbBgra8,
                                  DisplayHdrState::kSdr) == HdrPassiveNote::kSourceSdr,
              "问过且答案是 SDR：note.hdr_source_sdr（那句恒等映射才有根据）");
        Check(JudgeHdrPassiveNote(Hdr(HdrPolicy::kRefuse, true), FrameColorSpace::kSrgbBgra8,
                                  DisplayHdrState::kUnknown) == HdrPassiveNote::kSourceUnverified &&
                  JudgeHdrPassiveNote(Hdr(HdrPolicy::kToneMap, true), FrameColorSpace::kSrgbBgra8,
                                      DisplayHdrState::kHdr) == HdrPassiveNote::kSourceUnverified,
              "没问到答案（或按 8 位帧池交付而屏是 HDR）：改发 note.hdr_source_unverified，不说成 SDR");
    }

    std::printf("\n共 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
