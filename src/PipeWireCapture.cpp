#include "PipeWireCapture.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "ImageOps.h"

namespace ecapture {
namespace pipewire {
namespace {

bool MulFits(uint64_t a, uint64_t b) {
    return b == 0 || a <= std::numeric_limits<uint64_t>::max() / b;
}

}  // namespace

const char* CopyResultName(CopyResult result) {
    switch (result) {
        case CopyResult::kOk: return "ok";
        case CopyResult::kZeroSize: return "zero_size";
        case CopyResult::kBadStride: return "bad_stride";
        case CopyResult::kPayloadShort: return "payload_short";
        case CopyResult::kEmptyChunk: return "empty_chunk";
        case CopyResult::kCorruptedChunk: return "corrupted_chunk";
        case CopyResult::kUnsupportedFormat: return "unsupported_format";
        case CopyResult::kUnsupportedMemory: return "unsupported_memory";
        case CopyResult::kTooLarge: return "too_large";
        case CopyResult::kOverflow: return "overflow";
    }
    return "overflow";
}

int ClockwiseDegrees(VideoTransform transform) {
    switch (transform) {
        case VideoTransform::kNone: return 0;
        // SPA 的 90/270 是逆时针语义；交付的 RotateCropFrame 用顺时针角度。
        case VideoTransform::kRotate90Ccw: return 270;
        case VideoTransform::kRotate180: return 180;
        case VideoTransform::kRotate270Ccw: return 90;
        case VideoTransform::kFlipped:
        case VideoTransform::kFlipped90:
        case VideoTransform::kFlipped180:
        case VideoTransform::kFlipped270:
            return -1;
    }
    return -1;
}

bool SelectAuthorizedStream(const portal::Selection& selection,
                            portal::SourceKind expectedSource,
                            AuthorizedStreamTarget* out,
                            const char** reasonOut) {
    if (reasonOut) *reasonOut = reason::kProtocolError;
    if (!out) return false;
    if (!selection.exactlyOneStream || selection.streams.size() != 1) {
        if (reasonOut) *reasonOut = reason::kTargetNotUnique;
        return false;
    }
    const portal::StreamInfo& stream = selection.streams.front();
    if (!stream.sourceTypeKnown || !selection.sourceTypeVerified) {
        if (reasonOut) *reasonOut = reason::kSourceUnverified;
        return false;
    }
    if (stream.sourceType != expectedSource) {
        if (reasonOut) *reasonOut = reason::kSourceMismatch;
        return false;
    }
    if (stream.nodeId == 0) {
        if (reasonOut) *reasonOut = reason::kTargetMissing;
        return false;
    }
    out->nodeId = stream.nodeId;
    out->serialKnown = stream.pipewireSerialKnown;
    out->serial = stream.pipewireSerial;
    out->sourceType = stream.sourceType;
    if (reasonOut) *reasonOut = reason::kOk;
    return true;
}

uint64_t RequiredPayloadBytes(uint32_t width, uint32_t height, int32_t stride) {
    if (width == 0 || height == 0 || stride <= 0) return 0;
    const uint64_t rowBytes = static_cast<uint64_t>(width) * 4u;
    if (static_cast<uint64_t>(stride) < rowBytes) return 0;
    const uint64_t rows = static_cast<uint64_t>(height);
    if (!MulFits(static_cast<uint64_t>(stride), rows - 1u)) return 0;
    const uint64_t body = static_cast<uint64_t>(stride) * (rows - 1u);
    if (body > std::numeric_limits<uint64_t>::max() - rowBytes) return 0;
    return body + rowBytes;
}

CopyResult CopyFrameToBgra(const FrameBufferView& view, CapturedFrame* out) {
    if (!out) return CopyResult::kOverflow;
    if (!IsSupportedMemoryType(view.memoryType)) return CopyResult::kUnsupportedMemory;
    if (!IsSupportedPixelFormat(view.format)) return CopyResult::kUnsupportedFormat;
    if (view.chunkFlags & (1u << 0)) return CopyResult::kCorruptedChunk;
    if (view.chunkFlags & (1u << 1)) return CopyResult::kEmptyChunk;
    if (view.width == 0 || view.height == 0) return CopyResult::kZeroSize;
    if (view.width > kFrameMaxSide || view.height > kFrameMaxSide) return CopyResult::kTooLarge;
    if (!view.data || view.dataMaxSize == 0) return CopyResult::kPayloadShort;
    if (view.chunkSize == 0) return CopyResult::kEmptyChunk;

    const uint64_t rowBytes = static_cast<uint64_t>(view.width) * 4u;
    if (view.stride <= 0) return CopyResult::kBadStride;
    if (static_cast<uint64_t>(view.stride) < rowBytes ||
        static_cast<uint64_t>(view.stride) > rowBytes * 2u) {
        return CopyResult::kBadStride;
    }
    const uint64_t required = RequiredPayloadBytes(view.width, view.height, view.stride);
    if (required == 0) return CopyResult::kOverflow;
    if (!MulFits(rowBytes, view.height)) return CopyResult::kOverflow;
    const uint64_t outBytes = rowBytes * view.height;
    if (outBytes > kFrameMaxBytes) return CopyResult::kTooLarge;

    // spa_chunk.offset 的文档要求按 data.maxsize 取模；size 超出剩余空间时按文档 clamp。
    const uint64_t offset = static_cast<uint64_t>(view.chunkOffset) % view.dataMaxSize;
    const uint64_t available = view.dataMaxSize - offset;
    const uint64_t payload = std::min<uint64_t>(view.chunkSize, available);
    if (payload < required) return CopyResult::kPayloadShort;

    std::vector<uint8_t> dst(static_cast<size_t>(outBytes));
    const uint8_t* src = view.data + offset;
    for (uint32_t y = 0; y < view.height; ++y) {
        const uint8_t* s = src + static_cast<uint64_t>(y) * static_cast<uint64_t>(view.stride);
        uint8_t* d = dst.data() + static_cast<uint64_t>(y) * rowBytes;
        switch (view.format) {
            case PixelFormat::kBgra:
                std::memcpy(d, s, static_cast<size_t>(rowBytes));
                break;
            case PixelFormat::kRgba:
                for (uint32_t x = 0; x < view.width; ++x, s += 4, d += 4) {
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                }
                break;
            case PixelFormat::kBgrx:
                for (uint32_t x = 0; x < view.width; ++x, s += 4, d += 4) {
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
                }
                break;
            case PixelFormat::kRgbx:
                for (uint32_t x = 0; x < view.width; ++x, s += 4, d += 4) {
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
                }
                break;
            case PixelFormat::kXrgb:
                for (uint32_t x = 0; x < view.width; ++x, s += 4, d += 4) {
                    d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = 255;
                }
                break;
            case PixelFormat::kXbgr:
                for (uint32_t x = 0; x < view.width; ++x, s += 4, d += 4) {
                    d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = 255;
                }
                break;
            case PixelFormat::kArgb:
                for (uint32_t x = 0; x < view.width; ++x, s += 4, d += 4) {
                    d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];
                }
                break;
            case PixelFormat::kAbgr:
                for (uint32_t x = 0; x < view.width; ++x, s += 4, d += 4) {
                    d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
                }
                break;
            case PixelFormat::kUnknown:
            default:
                return CopyResult::kUnsupportedFormat;
        }
    }

    out->pixels = std::move(dst);
    out->width = view.width;
    out->height = view.height;
    out->stride = static_cast<uint32_t>(rowBytes);
    return CopyResult::kOk;
}

FrameDecision DecideFrame(const FrameBufferView& view, uint64_t minGraphNs,
                          bool havePreviousSize, uint32_t previousWidth,
                          uint32_t previousHeight) {
    if (view.chunkFlags & (1u << 1)) return FrameDecision::kEmpty;
    if (view.chunkFlags & (1u << 0)) return FrameDecision::kCorrupted;
    if (!IsSupportedMemoryType(view.memoryType)) return FrameDecision::kUnsupportedMemory;
    if (!IsSupportedPixelFormat(view.format)) return FrameDecision::kUnsupportedFormat;
    if (view.chunkSize == 0) return FrameDecision::kEmpty;
    if (view.width == 0 || view.height == 0 || view.width > kFrameMaxSide ||
        view.height > kFrameMaxSide || RequiredPayloadBytes(view.width, view.height,
                                                            view.stride) == 0) {
        return FrameDecision::kInvalidShape;
    }
    if (!view.data || view.dataMaxSize == 0) return FrameDecision::kInvalidShape;
    const uint64_t offset = static_cast<uint64_t>(view.chunkOffset) % view.dataMaxSize;
    const uint64_t available = view.dataMaxSize - offset;
    const uint64_t payload = std::min<uint64_t>(view.chunkSize, available);
    if (payload < RequiredPayloadBytes(view.width, view.height, view.stride)) {
        return FrameDecision::kInvalidShape;
    }
    if (havePreviousSize &&
        (view.width != previousWidth || view.height != previousHeight)) {
        return FrameDecision::kAcceptSizeChanged;
    }
    if (view.bufferTimeKnown && minGraphNs != 0 && view.bufferTimeNs < minGraphNs) {
        return FrameDecision::kStale;
    }
    return FrameDecision::kAccept;
}

WaitDecision DecideWait(bool gotFrame, bool fatal, bool disconnected, bool cancelRequested,
                        bool deadlineEnabled, uint64_t nowNs, uint64_t deadlineNs) {
    if (gotFrame) return WaitDecision::kFrameReady;
    if (cancelRequested) return WaitDecision::kCancelled;
    if (disconnected) return WaitDecision::kDisconnected;
    if (fatal) return WaitDecision::kFatal;
    if (deadlineEnabled && nowNs >= deadlineNs) return WaitDecision::kTimedOut;
    return WaitDecision::kContinue;
}

bool RotateFrameClockwise(CapturedFrame* frame, uint32_t clockwiseDegrees) {
    if (!frame) return false;
    if (clockwiseDegrees == 0) return true;
    if (clockwiseDegrees != 90 && clockwiseDegrees != 180 && clockwiseDegrees != 270) return false;
    if (frame->width == 0 || frame->height == 0) return false;
    if (frame->width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        frame->height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    CapturedFrame rotated;
    const PhysRect all{0, 0, static_cast<int32_t>(frame->width),
                       static_cast<int32_t>(frame->height)};
    if (!RotateCropFrame(*frame, all, clockwiseDegrees, &rotated)) return false;
    rotated.source = frame->source;
    rotated.path = frame->path;
    rotated.reportsCrop = frame->reportsCrop;
    rotated.requestedRect = frame->requestedRect;
    rotated.capturedRect = frame->capturedRect;
    rotated.clipped = frame->clipped;
    rotated.rotation = (frame->rotation + clockwiseDegrees) % 360u;
    rotated.cursorStateKnown = frame->cursorStateKnown;
    rotated.cursorInFrame = frame->cursorInFrame;
    rotated.sourceColorSpace = frame->sourceColorSpace;
    rotated.toneMapped = frame->toneMapped;
    rotated.displayHdrState = frame->displayHdrState;
    *frame = std::move(rotated);
    return true;
}

}  // namespace pipewire
}  // namespace ecapture
