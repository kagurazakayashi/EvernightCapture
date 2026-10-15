#include "Encoder.h"

#include <string>

#include "HdrColor.h"    // FrameColorSpace 那一份来源事实与它的机器名
#include "ImageOps.h"    // CheckFrameShape / PackTight / FrameShapeError
#include "Lang.h"

namespace ecapture {
namespace {

// 把一条恒 ASCII 的细节（登记表给的那句理由、形状状态名）搬进 std::wstring。
// 结论的 ASCII 名与现场只作 diagnostics 的细节用，调用方分支靠的是稳定码。
std::wstring AsciiToken(const char* text) {
    std::wstring out;
    for (const char* p = text; p && *p; ++p) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    return out;
}

}  // namespace

bool EncodeFailure(Diagnostic* out, const wchar_t* code, std::wstring message,
                   const std::wstring& format, const std::wstring& detail,
                   const std::wstring& hresult, const wchar_t* backend) {
    if (!out) return false;
    const std::wstring hint = Msgf(L"enc.available", AvailableFormats());
    *out = Diagnostic{code, std::move(message), L"--format", format,
                      detail.empty() ? hint : Msgf(L"enc.detail", hint, detail),
                      std::wstring(), backend ? std::wstring(backend) : std::wstring(),
                      stages::kEncode};
    // hresult 只在真拿得到那一次返回值的那条文路上出现，别填个 0 进去。
    out->hresult = hresult;
    return false;
}

const char* EncodeInputName(EncodeInput verdict) {
    switch (verdict) {
        case EncodeInput::kOk: return "ok";
        case EncodeInput::kZeroSize: return "zero_size";
        case EncodeInput::kBadShape: return "bad_shape";
        case EncodeInput::kTooWideForCodec: return "too_wide_for_codec";
        case EncodeInput::kHdrNotMapped: return "hdr_not_mapped";
        case EncodeInput::kFormatUnsupported: return "format_unsupported";
        case EncodeInput::kBudgetSpent: return "budget_spent";
    }
    return "unknown";
}

CodecPath EncoderCurrentPath() { return BuildCodecPath(); }

bool EncodeFormatSupported(ImageFormat format, const char** reason) {
    const CodecPath path = BuildCodecPath();
    CodecSupport support = CodecSupportFor(path, format);
    if (support == CodecSupport::kSupported && !CodecLibraryLinked(format)) {
        // 登记表说"这条路径有这一格式"，而这次构建真没链上那家库：说成缺依赖，
        // 不说成"本工具不支持"——前者的下一步是装库再配置一次，后者的下一步是换格式。
        support = CodecSupport::kLibraryNotLinked;
    }
    if (reason) {
        *reason = support == CodecSupport::kLibraryNotLinked
                      ? "this build did not link the encoder library for this format"
                      : CodecSupportReason(path, format);
    }
    return CodecIsUsable(support);
}

bool CheckEncodeInput(const CapturedFrame& frame, ImageFormat format, const Deadline& dl,
                      Diagnostic* err) {
    const std::wstring label = FormatName(format);

    if (frame.width == 0 || frame.height == 0) {
        // backend 留空：与这条判据搬进共用层之前那条诊断的形状逐字节相同
        //（Windows 上这些文路当初就没写 errors[].backend，填上它是改契约而不是搬动）。
        return EncodeFailure(err, codes::kCaptureFailed, Msg(L"enc.size_zero"), label,
                             std::wstring(), std::wstring(), nullptr);
    }

    // 形状与上限排在任何分配之前判完（下面 PackTight 要按 stride*height 分配，平台编码器要把
    // 字节数当成一个 32 位长度交出去，两处都依赖这些数字还说得通）。判据是 CheckFrameShape，
    // 不靠"分配失败抛异常"来发现坏形状，也不让一个外部声明的尺寸换来一次无界分配。
    const FrameShapeInfo info{frame.width, frame.height, frame.stride,
                              static_cast<uint64_t>(frame.pixels.size())};
    const FrameShape shape = CheckFrameShape(info);
    auto failShape = [&](FrameShape bad) {
        if (err) {
            FrameShapeError(bad, info, nullptr, stages::kEncode, err);
            err->option = L"--format";
            err->value = label;
        }
        return false;
    };
    if (shape != FrameShape::kOk) return failShape(shape);

    const uint64_t tightBytes = static_cast<uint64_t>(frame.width) * 4ull * frame.height;
    if (tightBytes > 0xFFFFFFFFull) {
        // 两条路径的长度参数都是 32 位（WinRT 的 SetPixelData、libpng 的行数与 libjpeg 的
        // 图像尺寸）：紧凑像素超过 4 GiB 就交不出去。上面那条整帧 1 GiB 的上限本来已经把它
        // 挡在外面，这条判据留着是因为转成 32 位之前必须有人判一次。
        return failShape(FrameShape::kTooManyBytes);
    }

    // 来源说不出是 8 位 sRGB，而这一帧又没过一遍映射：不能硬按 8 位 SDR 解释。
    // 那两种下场各自都要拦：确凿是 HDR（FP16 scRGB / PQ / HLG）时那张图发白、去饱和、亮部一团糊，
    // "看着像一张正常图"，正是不能默认为正确的结果；而 kRgb10A2Unverified 与 kUnknown 连每像素
    // 字节数与传递函数都说不清（判据本体在 HdrColor.h 的规矩 2 与规矩 5）。
    // 已经映射过的帧照编：toneMapped 只在真过了一遍浮点映射时为 true，映射前的来源留在
    // sourceColorSpace 里作为一份事实，不由这里再判第二遍。
    const bool sdrSource = frame.sourceColorSpace == FrameColorSpace::kSrgbBgra8;
    if (!sdrSource && !frame.toneMapped) {
        if (!err) return false;
        *err = Diagnostic{codes::kHdrUnverifiable,
                          Msgf(L"cap.hdr_unverifiable", FrameColorSpaceName(frame.sourceColorSpace)),
                          L"--hdr", std::wstring(), Msg(L"cap.hdr_unverifiable_hint"),
                          std::wstring(), frame.source, stages::kEncode};
        return false;
    }

    // 格式支持问的是登记表 + 本次真链上的库，与"调用方以为自己能编"无关：
    // 不支持就一个字节都不产，并把现在编得出的那几种一并报出去。
    const char* reason = nullptr;
    if (!EncodeFormatSupported(format, &reason)) {
        return EncodeFailure(err, codes::kEncoderUnavailable, Msg(L"enc.format_unsupported"),
                             label, reason ? AsciiToken(reason) : std::wstring(), std::wstring(),
                             nullptr);
    }

    // 预算已经用尽就别开工：这一步一旦开始，能中断的点比它自己写完一趟少得多。
    if (dl.Spent()) {
        if (!err) return false;
        Diagnostic d = BudgetSpent(dl, codes::kCaptureTimeout, stages::kEncode, nullptr);
        d.option = L"--format";
        d.value = label;
        *err = d;
        return false;
    }
    return true;
}

bool EncodeFrame(const CapturedFrame& frame, ImageFormat fmt, int jpegQuality, const Deadline& dl,
                 std::vector<uint8_t>* bytes, Diagnostic* err) {
    if (!bytes) return false;
    const std::wstring label = FormatName(fmt);
    // 一次失败的编码不留下任何东西：调用方复用的那份缓冲（辅助进程那一路也是同一形状）
    // 不许带着上一次的字节被当成这一张的产物交出去，所以判据之前先清。
    bytes->clear();
    if (!CheckEncodeInput(frame, fmt, dl, err)) return false;

    // 行距补齐重排成紧凑行：两条路径都只收紧凑行，行末那几字节填充不进图片。
    // 上面 CheckFrameShape 已经过了才会到这里，那唯一的可能就是缓冲区自己与形状对不上：
    // 照 buffer_short 说，不说"形状没问题却失败了"这种读不懂的话。
    std::vector<uint8_t> tight;
    if (!PackTight(frame, &tight)) {
        const FrameShapeInfo info{frame.width, frame.height, frame.stride,
                                  static_cast<uint64_t>(frame.pixels.size())};
        if (err) {
            FrameShapeError(FrameShape::kBufferShort, info, nullptr, stages::kEncode, err);
            err->option = L"--format";
            err->value = label;
        }
        return false;
    }

    bytes->clear();   // 不让一次失败的编码留下上一次的旧字节
    if (!EncodeFrameNative(frame, tight, fmt, jpegQuality, dl, bytes, err)) {
        bytes->clear();
        return false;
    }

    // 空产物是一条失败，而不是"零字节也算编完了"：否则调用方拿到的 errors 里会出现一条
    // 没有 code 的条目，而磁盘上什么也没有。这一条只在这里判一次，两个平台共用。
    if (bytes->empty()) {
        return EncodeFailure(err, codes::kEncoderUnavailable, Msg(L"enc.empty"), label,
                             std::wstring(), std::wstring(), nullptr);
    }
    return true;
}

std::wstring AvailableFormats() {
    // 顺序 = ImageFormat 的取值顺序，别名 = FormatDisplayAlias，可用性 = 本次的真实答复。
    // Windows 上这一句仍然是 "png, jpg, bmp, tiff, gif"，与这条判据搬进共用层之前逐字节相同。
    std::wstring out;
    const ImageFormat all[] = {ImageFormat::kPng, ImageFormat::kJpeg, ImageFormat::kBmp,
                               ImageFormat::kTiff, ImageFormat::kGif};
    for (const ImageFormat fmt : all) {
        if (!EncodeFormatSupported(fmt, nullptr)) continue;
        if (!out.empty()) out += L", ";
        out += FormatDisplayAlias(fmt);
    }
    return out;
}

}  // namespace ecapture
