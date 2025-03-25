#include "ImageOps.h"

#include <cstring>

namespace ecapture {

bool CropFrame(CapturedFrame* frame, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (!frame || width == 0 || height == 0) return false;
    if (static_cast<uint64_t>(x) + width > frame->width ||
        static_cast<uint64_t>(y) + height > frame->height) {
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    std::vector<uint8_t> cropped(rowBytes * height);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = frame->pixels.data() + static_cast<size_t>(y + row) * frame->stride +
                             static_cast<size_t>(x) * 4u;
        std::memcpy(cropped.data() + static_cast<size_t>(row) * rowBytes, src, rowBytes);
    }
    frame->pixels.swap(cropped);
    frame->width = width;
    frame->height = height;
    frame->stride = static_cast<uint32_t>(rowBytes);
    return true;
}

bool FrameIsFlat(const CapturedFrame& frame) {
    if (frame.pixels.empty() || frame.width == 0 || frame.height == 0) return true;
    const uint8_t* first = frame.pixels.data();
    // 跨行跨列抽样比较，遇到第一个不同就判定"有内容"：步长取质数，避免撞上周期性图案
    for (uint32_t y = 0; y < frame.height; y += 3) {
        const uint8_t* row = frame.pixels.data() + static_cast<size_t>(y) * frame.stride;
        for (uint32_t x = 0; x + 3 < frame.width * 4u; x += 13) {
            if (row[x] != first[x] || row[x + 1] != first[x + 1] || row[x + 2] != first[x + 2] ||
                row[x + 3] != first[x + 3]) {
                return false;
            }
        }
    }
    return true;
}

std::vector<uint8_t> PackTight(const CapturedFrame& frame) {
    const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
    std::vector<uint8_t> tight(rowBytes * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(tight.data() + y * rowBytes,
                    frame.pixels.data() + static_cast<size_t>(y) * frame.stride, rowBytes);
    }
    return tight;
}

}  // namespace ecapture
