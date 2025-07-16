#include "ImageOps.h"

#include <cstring>
#include <string>

namespace ecapture {
namespace {

uint64_t RowBytesOf(uint64_t width) { return width * 4ull; }

}  // namespace

FrameShape CheckFrameShape(const FrameShapeInfo& info) {
    if (info.width == 0 || info.height == 0) return FrameShape::kEmpty;
    if (info.width > kFrameMaxSide || info.height > kFrameMaxSide) return FrameShape::kSideTooLarge;

    // 到这一步 width 已经在 uint32 里，乘以 4 只会溢出在 32 位上，所以全程用 64 位算
    const uint64_t rowBytes = RowBytesOf(info.width);
    if (info.stride < rowBytes) return FrameShape::kStrideTooSmall;
    if (info.stride > rowBytes * 2ull) return FrameShape::kStrideTooLarge;

    const uint64_t needed = static_cast<uint64_t>(info.stride) * info.height;
    if (needed > kFrameMaxBytes) return FrameShape::kTooManyBytes;
    if (info.size < needed) return FrameShape::kBufferShort;
    return FrameShape::kOk;
}

const char* FrameShapeName(FrameShape shape) {
    switch (shape) {
        case FrameShape::kOk: return "ok";
        case FrameShape::kEmpty: return "empty";
        case FrameShape::kSideTooLarge: return "side_too_large";
        case FrameShape::kStrideTooSmall: return "stride_too_small";
        case FrameShape::kStrideTooLarge: return "stride_too_large";
        case FrameShape::kTooManyBytes: return "too_many_bytes";
        case FrameShape::kBufferShort: return "buffer_short";
    }
    return "unknown";
}

void FrameShapeError(FrameShape shape, const FrameShapeInfo& info, const wchar_t* channel,
                     const wchar_t* stage, Diagnostic* err) {
    if (!err) return;
    std::wstring token;
    for (const char* p = FrameShapeName(shape); p && *p; ++p) {
        token.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    // channel 可以为空（编码那一步不知道是哪条通道出的这帧），空就是诊断里不写这个键
    const std::wstring backend = channel ? std::wstring(channel) : std::wstring();
    *err = Diagnostic{codes::kFrameInvalid, Msgf(L"cap.frame_invalid", token), L"--capture",
                      backend, std::wstring(), std::wstring(), backend, stage};
    err->hint = Msgf(L"cap.frame_invalid_hint", static_cast<uint64_t>(kFrameMaxSide),
                     static_cast<uint64_t>(kFrameMaxBytes), static_cast<uint64_t>(info.width),
                     static_cast<uint64_t>(info.height), static_cast<uint64_t>(info.stride),
                     info.size);
}

bool FrameShapeOk(const CapturedFrame& frame, const wchar_t* channel, const wchar_t* stage,
                  Diagnostic* err) {
    const FrameShape shape = InspectFrameShape(frame);
    if (shape == FrameShape::kOk) return true;
    FrameShapeError(shape, FrameShapeInfo{frame.width, frame.height, frame.stride,
                                          static_cast<uint64_t>(frame.pixels.size())},
                    channel, stage, err);
    return false;
}

bool FrameIsUniform(const CapturedFrame& frame, FrameColor* out) {
    if (InspectFrameShape(frame) != FrameShape::kOk) return false;
    const uint8_t* base = frame.pixels.data();
    const FrameColor ref{base[0], base[1], base[2], base[3]};
    if (out) *out = ref;

    // 参考像素铺满一行，然后整行整行地比：行末的填充不参与（那不是画面内容），
    // 而"每行都比同一张参考行"也顺带把旧实现按行首像素比的错误排除掉了。
    const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
    std::vector<uint8_t> pattern(rowBytes);
    for (size_t off = 0; off < rowBytes; off += 4u) {
        pattern[off] = ref.b;
        pattern[off + 1] = ref.g;
        pattern[off + 2] = ref.r;
        pattern[off + 3] = ref.a;
    }
    for (uint32_t row = 0; row < frame.height; ++row) {
        const uint8_t* p = base + static_cast<size_t>(row) * frame.stride;
        if (std::memcmp(p, pattern.data(), rowBytes) != 0) return false;
    }
    return true;
}

bool CropFrame(CapturedFrame* frame, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!frame) return false;
    if (InspectFrameShape(*frame) != FrameShape::kOk) return false;
    if (width == 0 || height == 0) return false;
    if (width > kFrameMaxSide || height > kFrameMaxSide) return false;

    // 相加与相乘都在 64 位里判，判过之后才转回 uint32 用
    if (static_cast<uint64_t>(x) + width > frame->width ||
        static_cast<uint64_t>(y) + height > frame->height) {
        return false;
    }
    const uint64_t srcRowBytes = frame->stride;
    const uint64_t lastRow = static_cast<uint64_t>(y) + height - 1ull;
    const uint64_t rowBytes = RowBytesOf(width);
    // 最后一行要读的字节必须还在缓冲区里（上面的形状检查只保证整帧自洽）
    if (lastRow * srcRowBytes + static_cast<uint64_t>(x) * 4ull + rowBytes > frame->pixels.size()) {
        return false;
    }
    const uint64_t croppedBytes = rowBytes * height;
    if (croppedBytes > kFrameMaxBytes) return false;

    std::vector<uint8_t> cropped(static_cast<size_t>(croppedBytes));
    const uint8_t* srcBase = frame->pixels.data();
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = srcBase + static_cast<size_t>(y + row) * frame->stride +
                             static_cast<size_t>(x) * 4u;
        std::memcpy(cropped.data() + static_cast<size_t>(row) * rowBytes, src,
                    static_cast<size_t>(rowBytes));
    }
    frame->pixels.swap(cropped);
    frame->width = width;
    frame->height = height;
    frame->stride = static_cast<uint32_t>(rowBytes);
    return true;
}

bool PackTight(const CapturedFrame& frame, std::vector<uint8_t>* out) {
    if (!out) return false;
    if (InspectFrameShape(frame) != FrameShape::kOk) return false;

    const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
    const size_t tightBytes = rowBytes * static_cast<size_t>(frame.height);
    if (tightBytes > kFrameMaxBytes) return false;
    out->assign(tightBytes, 0);
    for (uint32_t row = 0; row < frame.height; ++row) {
        std::memcpy(out->data() + static_cast<size_t>(row) * rowBytes,
                    frame.pixels.data() + static_cast<size_t>(row) * frame.stride, rowBytes);
    }
    return true;
}

}  // namespace ecapture
