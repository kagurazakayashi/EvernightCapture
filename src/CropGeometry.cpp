#include "CropGeometry.h"

#include <cstdint>
#include <limits>

namespace ecapture {
namespace {

constexpr int64_t kLongMin = std::numeric_limits<LONG>::min();
constexpr int64_t kLongMax = std::numeric_limits<LONG>::max();

// 落在 LONG 范围内才敢写成 RECT（屏幕坐标那套系就是 LONG）。
bool FitsLong(int64_t v) { return v >= kLongMin && v <= kLongMax; }

// 一条矩形，屏幕坐标，判它是不是装得进 image 那块交付图像，并算出图像坐标里的那一块。
// 返回 false = 装不进去（含负偏移：那一条边根本没被交付）。
bool MapScreenRectIntoImage(const RECT& screen, const DeliveredImage& image, ImageRect* out,
                            uint64_t* right, uint64_t* bottom) {
    const int64_t w = static_cast<int64_t>(screen.right) - screen.left;
    const int64_t h = static_cast<int64_t>(screen.bottom) - screen.top;
    if (w <= 0 || h <= 0) return false;
    const int64_t offX = static_cast<int64_t>(screen.left) - image.screenRect.left;
    const int64_t offY = static_cast<int64_t>(screen.top) - image.screenRect.top;
    if (offX < 0 || offY < 0) return false;
    const uint64_t edgeX = static_cast<uint64_t>(offX) + static_cast<uint64_t>(w);
    const uint64_t edgeY = static_cast<uint64_t>(offY) + static_cast<uint64_t>(h);
    if (right) *right = edgeX;
    if (bottom) *bottom = edgeY;
    if (edgeX > image.width || edgeY > image.height) return false;
    if (out) {
        out->x = static_cast<uint32_t>(offX);
        out->y = static_cast<uint32_t>(offY);
        out->width = static_cast<uint32_t>(w);
        out->height = static_cast<uint32_t>(h);
    }
    return true;
}

}  // namespace

bool RoiFitsTarget(const CropRequest& request, uint32_t rectWidth, uint32_t rectHeight) {
    if (request.mode != CropMode::kRoi) return true;   // --client-area 由取帧之后那一关判
    if (rectWidth == 0 || rectHeight == 0) return false;
    if (request.width == 0 || request.height == 0) return false;
    // 相加在 64 位里判：解析层已经限死每条都不超过 kRoiMaxValue，所以这里不可能绕回，
    // 但判据不依赖那个前提（ResolveWindowCrop 走的是同一条算式，两处都得自洽）。
    const uint64_t right = static_cast<uint64_t>(request.x) + request.width;
    const uint64_t bottom = static_cast<uint64_t>(request.y) + request.height;
    return right <= rectWidth && bottom <= rectHeight;
}

DeliveredImage DescribeDeliveredImage(uint32_t frameWidth, uint32_t frameHeight,
                                      const RECT& measuredWindowRect, bool measuredOk,
                                      const RECT& sampledScreenRect, bool sampledRectOk) {
    DeliveredImage image;
    image.width = frameWidth;
    image.height = frameHeight;
    // 桌面裁切那一条：实际截到的那一块就是这块图像的屏幕位置，尺寸也必然等于帧尺寸
    //（帧是从它那么大的一块拷出来的）。优先用它，因为它是这条通道自己报出来的事实。
    if (sampledRectOk) {
        const int64_t w = static_cast<int64_t>(sampledScreenRect.right) - sampledScreenRect.left;
        const int64_t h = static_cast<int64_t>(sampledScreenRect.bottom) - sampledScreenRect.top;
        if (w > 0 && h > 0 && static_cast<uint64_t>(w) == frameWidth &&
            static_cast<uint64_t>(h) == frameHeight) {
            image.hasScreenRect = true;
            image.screenRect = sampledScreenRect;
            return image;
        }
    }
    // 窗口内容那一条：此刻量到的可见矩形尺寸与交付尺寸完全相同，才承认它就是这块图像。
    if (measuredOk) {
        const int64_t w = static_cast<int64_t>(measuredWindowRect.right) - measuredWindowRect.left;
        const int64_t h = static_cast<int64_t>(measuredWindowRect.bottom) - measuredWindowRect.top;
        if (w > 0 && h > 0 && static_cast<uint64_t>(w) == frameWidth &&
            static_cast<uint64_t>(h) == frameHeight) {
            image.hasScreenRect = true;
            image.screenRect = measuredWindowRect;
        }
    }
    return image;
}

CropResolution ResolveWindowCrop(const CropRequest& request, const DeliveredImage& image,
                                 const ClientAreaProbe& client) {
    CropResolution res;
    res.imageWidth = image.width;
    res.imageHeight = image.height;
    if (request.mode == CropMode::kNone) return res;   // status 保持 kNoCrop

    if (image.width == 0 || image.height == 0) {
        res.status = CropStatus::kOutOfRange;
        res.fail = CropFail::kEmptyImage;
        return res;
    }

    if (request.mode == CropMode::kClientArea) {
        // 两问都要有答案：客户区本身、以及这块交付图像在屏幕上落在哪儿。
        // 少了后者就不知道客户区相对于图像左上角偏移多少 —— 那是"问不出来"，不是"放不下"。
        if (!client.readable) {
            res.status = CropStatus::kUnmeasurable;
            res.fail = CropFail::kClientUnmeasurable;
            return res;
        }
        if (!image.hasScreenRect) {
            res.status = CropStatus::kUnmeasurable;
            res.fail = CropFail::kImageUnmeasurable;
            return res;
        }
        const int64_t w = static_cast<int64_t>(client.screenRect.right) - client.screenRect.left;
        const int64_t h = static_cast<int64_t>(client.screenRect.bottom) - client.screenRect.top;
        if (w <= 0 || h <= 0) {
            res.status = CropStatus::kOutOfRange;
            res.fail = CropFail::kEmptyClientArea;
            return res;
        }
        ImageRect crop{};
        if (!MapScreenRectIntoImage(client.screenRect, image, &crop, &res.requestRight,
                                    &res.requestBottom)) {
            res.status = CropStatus::kOutOfRange;
            res.fail = CropFail::kClientBeyondImage;
            return res;
        }
        res.crop = crop;
        res.status = CropStatus::kCropped;
        res.hasCropScreen = true;
        res.cropScreen = client.screenRect;
        return res;
    }

    // --roi：只需要图像尺寸就裁得下来；屏幕坐标那一行要等图像原点核实得出来才写。
    res.requestRight = static_cast<uint64_t>(request.x) + request.width;
    res.requestBottom = static_cast<uint64_t>(request.y) + request.height;
    if (request.width == 0 || request.height == 0 || res.requestRight > image.width ||
        res.requestBottom > image.height) {
        res.status = CropStatus::kOutOfRange;
        res.fail = CropFail::kRoiBeyondImage;
        return res;
    }
    res.crop.x = request.x;
    res.crop.y = request.y;
    res.crop.width = request.width;
    res.crop.height = request.height;
    res.status = CropStatus::kCropped;
    if (image.hasScreenRect) {
        const int64_t left = static_cast<int64_t>(image.screenRect.left) + request.x;
        const int64_t top = static_cast<int64_t>(image.screenRect.top) + request.y;
        const int64_t right = left + static_cast<int64_t>(request.width);
        const int64_t bottom = top + static_cast<int64_t>(request.height);
        if (FitsLong(left) && FitsLong(top) && FitsLong(right) && FitsLong(bottom)) {
            res.hasCropScreen = true;
            res.cropScreen = RECT{static_cast<LONG>(left), static_cast<LONG>(top),
                                  static_cast<LONG>(right), static_cast<LONG>(bottom)};
        }
    }
    return res;
}

const char* CropFailName(CropFail fail) {
    switch (fail) {
        case CropFail::kNone: return "none";
        case CropFail::kRoiBeyondImage: return "roi_beyond_image";
        case CropFail::kEmptyImage: return "empty_image";
        case CropFail::kClientUnmeasurable: return "client_unmeasurable";
        case CropFail::kImageUnmeasurable: return "image_unmeasurable";
        case CropFail::kClientBeyondImage: return "client_beyond_image";
        case CropFail::kEmptyClientArea: return "empty_client_area";
    }
    return "unknown";
}

}  // namespace ecapture
