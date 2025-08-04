#include "ImageOps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace ecapture {
namespace {

uint64_t RowBytesOf(uint64_t width, uint32_t bytesPerPixel = 4) {
    return width * static_cast<uint64_t>(bytesPerPixel == 0 ? 4u : bytesPerPixel);
}

}  // namespace

FrameShape CheckFrameShape(const FrameShapeInfo& info) {
    if (info.width == 0 || info.height == 0) return FrameShape::kEmpty;
    if (info.width > kFrameMaxSide || info.height > kFrameMaxSide) return FrameShape::kSideTooLarge;

    // 到这一步 width 已经在 uint32 里，乘以每像素字节数只会溢出在 32 位上，所以全程用 64 位算。
    // bytesPerPixel 默认 4（BGRA8 那一条，也是既有全部调用点），只有广色域帧那条传 8。
    const uint64_t rowBytes = RowBytesOf(info.width, info.bytesPerPixel);
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

bool RotateCropFrame(const CapturedFrame& frame, const RECT& srcRect, uint32_t angle,
                     CapturedFrame* out) {
    if (!out) return false;
    if (InspectFrameShape(frame) != FrameShape::kOk) return false;
    if (angle != 0u && angle != 90u && angle != 180u && angle != 270u) return false;

    const int64_t left = srcRect.left;
    const int64_t top = srcRect.top;
    const int64_t sw = static_cast<int64_t>(srcRect.right) - left;
    const int64_t sh = static_cast<int64_t>(srcRect.bottom) - top;
    if (left < 0 || top < 0 || sw <= 0 || sh <= 0) return false;
    if (srcRect.right > static_cast<int64_t>(frame.width) ||
        srcRect.bottom > static_cast<int64_t>(frame.height)) {
        return false;
    }

    const bool swapSides = angle == 90u || angle == 270u;
    const uint64_t outWidth = swapSides ? static_cast<uint64_t>(sh) : static_cast<uint64_t>(sw);
    const uint64_t outHeight = swapSides ? static_cast<uint64_t>(sw) : static_cast<uint64_t>(sh);
    if (outWidth > kFrameMaxSide || outHeight > kFrameMaxSide) return false;
    const uint64_t rowBytes = RowBytesOf(outWidth);
    if (rowBytes * outHeight > kFrameMaxBytes) return false;

    std::vector<uint8_t> rotated(static_cast<size_t>(rowBytes * outHeight));
    const uint8_t* srcBase = frame.pixels.data();
    for (uint64_t y = 0; y < outHeight; ++y) {
        uint8_t* dstRow = rotated.data() + static_cast<size_t>(y) * rowBytes;
        for (uint64_t x = 0; x < outWidth; ++x) {
            // 目标像素 (x,y) 来自那块矩形里的哪一个源像素。四种角度都写成"矩形内坐标"的
            // 形式，再叠加矩形左上角 —— 越界的可能性已经被上面的 srcRect 检查挡掉了。
            uint64_t cx = x;
            uint64_t cy = y;
            switch (angle) {
                case 90u:   cx = y;  cy = static_cast<uint64_t>(sh) - 1u - x; break;
                case 180u:  cx = static_cast<uint64_t>(sw) - 1u - x;
                            cy = static_cast<uint64_t>(sh) - 1u - y; break;
                case 270u:  cx = static_cast<uint64_t>(sw) - 1u - y;
                            cy = x; break;
                default: break;   // 0 度：不转
            }
            const uint8_t* src = srcBase + (static_cast<uint64_t>(top) + cy) * frame.stride +
                                 (static_cast<uint64_t>(left) + cx) * 4ull;
            std::memcpy(dstRow + static_cast<size_t>(x) * 4u, src, 4u);
        }
    }
    out->pixels.swap(rotated);
    out->width = static_cast<uint32_t>(outWidth);
    out->height = static_cast<uint32_t>(outHeight);
    out->stride = static_cast<uint32_t>(rowBytes);
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

ScaleResolution ResolveScale(const ScaleRequest& request, uint32_t srcWidth, uint32_t srcHeight) {
    ScaleResolution r;
    if (!request.given) {
        r.status = ScaleStatus::kNoScale;
        return r;
    }
    // 输入形状说不通（零尺寸 / 超单边上限）：与帧形状那一关同一组边界，不猜
    if (srcWidth == 0 || srcHeight == 0 || srcWidth > kFrameMaxSide || srcHeight > kFrameMaxSide) {
        r.status = ScaleStatus::kRejected;
        return r;
    }
    // 一条天花板都没给（解析期拦得住，这里只是不把"空请求"当成一次"缩到 0"）
    if (!request.hasMaxWidth && !request.hasMaxHeight && !request.hasMaxPixels) {
        r.status = ScaleStatus::kUnchanged;
        r.width = srcWidth;
        r.height = srcHeight;
        return r;
    }

    // 比例只减不增：三条天花板各算一个比例，取最小的；没有一条比图更紧时 ratio 留在 1
    //（这就是"默认不放大"：比例永远不会大于 1，所以输出永远不会比源大）。
    double ratio = 1.0;
    if (request.hasMaxWidth && request.maxWidth < srcWidth) {
        ratio = std::min(ratio, static_cast<double>(request.maxWidth) / static_cast<double>(srcWidth));
    }
    if (request.hasMaxHeight && request.maxHeight < srcHeight) {
        ratio = std::min(ratio,
                         static_cast<double>(request.maxHeight) / static_cast<double>(srcHeight));
    }
    if (request.hasMaxPixels) {
        const double area = static_cast<double>(srcWidth) * static_cast<double>(srcHeight);
        if (static_cast<double>(request.maxPixels) < area) {
            ratio = std::min(ratio,
                             std::sqrt(static_cast<double>(request.maxPixels) / area));
        }
    }
    if (ratio >= 1.0) {
        r.status = ScaleStatus::kUnchanged;
        r.width = srcWidth;
        r.height = srcHeight;
        return r;
    }

    // 向下取整：floor 只会更小，所以按比例算出来的宽高不会越过任何一条天花板；
    // 再各自至少留 1 像素（空图不是一张图）。
    uint32_t w = static_cast<uint32_t>(std::floor(static_cast<double>(srcWidth) * ratio));
    uint32_t h = static_cast<uint32_t>(std::floor(static_cast<double>(srcHeight) * ratio));
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    // 两道**整数**核对（不靠上面那次开方的精度）：把三条天花板真的钉死，
    // 一条不成立就再缩一像素（正常情况下一次都不会转，这里只是不把"差一像素"留给运气）。
    if (request.hasMaxWidth && w > request.maxWidth) w = request.maxWidth;
    if (request.hasMaxHeight && h > request.maxHeight) h = request.maxHeight;
    if (request.hasMaxPixels) {
        while (static_cast<uint64_t>(w) * h > request.maxPixels && (w > 1 || h > 1)) {
            if (w >= h && w > 1) {
                --w;
            } else if (h > 1) {
                --h;
            } else {
                break;
            }
        }
    }

    if (w >= srcWidth && h >= srcHeight) {   // 核对之后又不比源小了（只是可能）：按原样
        r.status = ScaleStatus::kUnchanged;
        r.width = srcWidth;
        r.height = srcHeight;
        return r;
    }
    r.status = ScaleStatus::kScaled;
    r.width = w;
    r.height = h;
    return r;
}

bool ScaleFrame(CapturedFrame* frame, uint32_t outWidth, uint32_t outHeight) {
    if (!frame) return false;
    if (InspectFrameShape(*frame) != FrameShape::kOk) return false;
    if (outWidth == 0 || outHeight == 0) return false;
    if (outWidth > frame->width || outHeight > frame->height) return false;   // 只缩不放

    const uint64_t rowBytes = RowBytesOf(outWidth);
    const uint64_t outBytes = rowBytes * outHeight;
    // 分配之前先判形状与上限（与 CropFrame 同一道规矩：不靠"分配失败抛异常"当检查）
    if (CheckFrameShape(FrameShapeInfo{outWidth, outHeight, static_cast<uint32_t>(rowBytes), outBytes}) !=
        FrameShape::kOk) {
        return false;
    }

    std::vector<uint8_t> scaled(static_cast<size_t>(outBytes));
    const uint8_t* srcBase = frame->pixels.data();
    const uint64_t srcW = frame->width;
    const uint64_t srcH = frame->height;
    for (uint32_t y = 0; y < outHeight; ++y) {
        // 最近邻：源行 = floor(y * srcH / outHeight)。恒等尺寸下它正好就是它自己那一行，
        // 所以"给了 --scale 但天花板不生效"不会被这一层搬成另一张图。
        const uint64_t sy = static_cast<uint64_t>(y) * srcH / outHeight;
        const uint8_t* srcRow = srcBase + static_cast<size_t>(sy) * frame->stride;
        uint8_t* dstRow = scaled.data() + static_cast<size_t>(y) * rowBytes;
        for (uint32_t x = 0; x < outWidth; ++x) {
            const uint64_t sx = static_cast<uint64_t>(x) * srcW / outWidth;
            std::memcpy(dstRow + static_cast<size_t>(x) * 4u,
                        srcRow + static_cast<size_t>(sx) * 4u, 4u);
        }
    }
    frame->pixels.swap(scaled);
    frame->width = outWidth;
    frame->height = outHeight;
    frame->stride = static_cast<uint32_t>(rowBytes);
    return true;
}

}  // namespace ecapture
