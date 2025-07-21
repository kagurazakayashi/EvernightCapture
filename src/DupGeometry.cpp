#include "DupGeometry.h"

#include <algorithm>
#include <cstdint>

namespace ecapture {
namespace {

int64_t W(const RECT& r) { return static_cast<int64_t>(r.right) - r.left; }
int64_t H(const RECT& r) { return static_cast<int64_t>(r.bottom) - r.top; }

bool Empty(const RECT& r) { return W(r) <= 0 || H(r) <= 0; }

RECT Intersect(const RECT& a, const RECT& b) {
    RECT r{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
           std::min(a.bottom, b.bottom)};
    if (Empty(r)) return RECT{};
    return r;
}

int64_t Area(const RECT& r) { return W(r) * H(r); }

// 纹理坐标的四个边界，全部用 int64 算：desktop / target 是有符号的虚拟屏幕坐标，
// 相减之后还要与无符号的纹理宽高比大小，混着比会把负数当成巨大的无符号数。
struct SrcBox {
    int64_t left = 0;
    int64_t top = 0;
    int64_t right = 0;
    int64_t bottom = 0;
};

}  // namespace

DupTransformDecision DecideDupTransform(DupRotation rotation, uint32_t desktopWidth,
                                        uint32_t desktopHeight, uint32_t textureWidth,
                                        uint32_t textureHeight) {
    DupTransformDecision d;
    d.desktopWidth = desktopWidth;
    d.desktopHeight = desktopHeight;
    d.textureWidth = textureWidth;
    d.textureHeight = textureHeight;

    if (desktopWidth == 0 || desktopHeight == 0 || textureWidth == 0 || textureHeight == 0) {
        d.reason = DupTransformReason::kDesktopRectEmpty;
        return d;
    }

    const bool sameAsDesktop = textureWidth == desktopWidth && textureHeight == desktopHeight;
    const bool swapped = textureWidth == desktopHeight && textureHeight == desktopWidth;
    // 只有 90/270 会让两种尺寸不同；正方形屏幕上"交换"与"不交换"是同一个尺寸，
    // 那时候没有尺寸证据可判，只能照报出的旋转处理。
    const bool orientationSwapped = desktopWidth != desktopHeight;

    switch (rotation) {
        case DupRotation::kIdentity:
            // 没有旋转：纹理就该和桌面一样大。对不上说明这张帧不是这块输出的当前画面。
            if (!sameAsDesktop) {
                d.reason = DupTransformReason::kFrameMismatch;
                return d;
            }
            d.angle = 0;
            d.ok = true;
            return d;
        case DupRotation::kRotate90:
        case DupRotation::kRotate270: {
            if (orientationSwapped && sameAsDesktop && !swapped) {
                // 驱动已经把画面转好了（尺寸就是用户看到的尺寸）：再转一次就转回去了。
                d.angle = 0;
                d.ok = true;
                return d;
            }
            if (swapped) {
                d.angle = rotation == DupRotation::kRotate90 ? 90u : 270u;
                d.ok = true;
                return d;
            }
            if (sameAsDesktop) {   // 正方形桌面：两种判据分不开，按报出的旋转走
                d.angle = rotation == DupRotation::kRotate90 ? 90u : 270u;
                d.ok = true;
                return d;
            }
            d.reason = DupTransformReason::kFrameMismatch;
            return d;
        }
        case DupRotation::kRotate180:
            // 180 度不交换宽高，尺寸给不出"转好了还是没转"的证据，只能照文档按未转处理。
            // 这条的现场由 images[].rotation 交付：真机把屏设成 180 度时一眼能核对方向。
            if (!sameAsDesktop) {
                d.reason = DupTransformReason::kFrameMismatch;
                return d;
            }
            d.angle = 180u;
            d.ok = true;
            return d;
        case DupRotation::kUnspecified:
        default:
            // 旋转值没报到（老驱动 / 远程会话）。这时唯一的线索是尺寸：
            // 与桌面一致就当作不用转；与桌面交换过则不敢猜方向，判不合法而不是蒙一次。
            if (sameAsDesktop) {
                d.angle = 0;
                d.ok = true;
                return d;
            }
            d.reason = DupTransformReason::kFrameMismatch;
            return d;
    }
}

DupCropPlan PlanDupCrop(const RECT& desktop, const RECT& target, uint32_t textureWidth,
                        uint32_t textureHeight, uint32_t angle) {
    DupCropPlan p;
    p.requested = target;
    if (Empty(target)) {
        p.reason = DupCropReason::kTargetEmpty;
        return p;
    }
    if (Empty(desktop)) {
        p.reason = DupCropReason::kNotOnOutput;
        return p;
    }
    if (angle != 0u && angle != 90u && angle != 180u && angle != 270u) {
        p.reason = DupCropReason::kBadAngle;
        return p;
    }

    const RECT visible = Intersect(desktop, target);
    if (Empty(visible)) {
        p.reason = DupCropReason::kNotOnOutput;
        return p;
    }
    p.captured = visible;
    p.lostLeft = visible.left - target.left;
    p.lostTop = visible.top - target.top;
    p.lostRight = target.right - visible.right;
    p.lostBottom = target.bottom - visible.bottom;
    p.clipped = p.lostLeft != 0 || p.lostTop != 0 || p.lostRight != 0 || p.lostBottom != 0;

    // 有效区域相对该输出左上角的位置（0 起，非负）
    const int64_t nx = static_cast<int64_t>(visible.left) - desktop.left;
    const int64_t ny = static_cast<int64_t>(visible.top) - desktop.top;
    const int64_t nw = W(visible);
    const int64_t nh = H(visible);
    const int64_t dw = W(desktop);
    const int64_t dh = H(desktop);

    SrcBox src{};
    switch (angle) {
        case 0u:
            src = SrcBox{nx, ny, nx + nw, ny + nh};
            break;
        case 90u:
            // 桌面(nx,ny) = 纹理(ny, dw-1-nx)：顺时针转 90 度才与用户看到的一致
            src = SrcBox{ny, dw - nx - nw, ny + nh, dw - nx};
            break;
        case 180u:
            src = SrcBox{dw - nx - nw, dh - ny - nh, dw - nx, dh - ny};
            break;
        default:   // 270
            // 桌面(nx,ny) = 纹理(dh-1-ny, nx)
            src = SrcBox{dh - ny - nh, nx, dh - ny, nx + nw};
            break;
    }
    if (src.left < 0 || src.top < 0 || src.right <= src.left || src.bottom <= src.top) {
        p.reason = DupCropReason::kOutsideTexture;
        return p;
    }
    if (src.right > static_cast<int64_t>(textureWidth) ||
        src.bottom > static_cast<int64_t>(textureHeight)) {
        p.reason = DupCropReason::kOutsideTexture;
        return p;
    }

    p.src = RECT{static_cast<LONG>(src.left), static_cast<LONG>(src.top),
                 static_cast<LONG>(src.right), static_cast<LONG>(src.bottom)};
    p.outWidth = static_cast<uint32_t>(nw);
    p.outHeight = static_cast<uint32_t>(nh);
    p.ok = true;
    p.reason = DupCropReason::kNone;
    return p;
}

namespace {

// 可当目标用的输出：必须已经接进桌面，且自己的矩形非空
bool Usable(const DupOutputInfo& o) {
    return o.attachedToDesktop && !Empty(o.desktopRect);
}

DupPickResult Found(const DupOutputInfo& o, bool byName) {
    DupPickResult r;
    r.status = DupPickStatus::kFound;
    r.adapterIndex = o.adapterIndex;
    r.outputIndex = o.outputIndex;
    r.output = o;
    r.matchedByDeviceName = byName;
    return r;
}

}  // namespace

DupPickResult PickDupOutputForScreen(const std::vector<DupOutputInfo>& outputs,
                                     const std::wstring& deviceName, const RECT& bounds) {
    bool usable = false;
    for (const DupOutputInfo& o : outputs) {
        if (!Usable(o)) continue;
        usable = true;
        if (!deviceName.empty() && o.deviceName == deviceName) return Found(o, true);
    }
    // 设备名对不上时退而比矩形：虚拟显卡与远程会话里两边的写法偶尔不一致。
    // 仍然要求"完全相同"——近似相等就换一块屏截，等于截了没人批准过的画面。
    for (const DupOutputInfo& o : outputs) {
        if (!Usable(o)) continue;
        if (o.desktopRect.left == bounds.left && o.desktopRect.top == bounds.top &&
            o.desktopRect.right == bounds.right && o.desktopRect.bottom == bounds.bottom) {
            return Found(o, false);
        }
    }
    DupPickResult r;
    r.status = usable ? DupPickStatus::kNoMatch : DupPickStatus::kNoOutputs;
    return r;
}

DupPickResult PickDupOutputForRect(const std::vector<DupOutputInfo>& outputs, const RECT& window) {
    const DupOutputInfo* best = nullptr;
    int64_t bestArea = 0;
    for (const DupOutputInfo& o : outputs) {
        if (!Usable(o)) continue;
        const int64_t overlap = Area(Intersect(window, o.desktopRect));
        if (overlap <= 0) continue;
        // 严格大者胜出；并列时留下先枚举到的那个（适配器/输出序号有序，不随驱动顺序抖）
        if (overlap > bestArea) {
            bestArea = overlap;
            best = &o;
        }
    }
    DupPickResult r;
    if (best) return Found(*best, false);
    r.status = DupPickStatus::kNoOutputs;
    for (const DupOutputInfo& o : outputs) {
        if (Usable(o)) {
            r.status = DupPickStatus::kNoMatch;
            break;
        }
    }
    return r;
}

std::wstring BriefDupOutputs(const std::vector<DupOutputInfo>& outputs) {
    std::wstring s;
    for (const DupOutputInfo& o : outputs) {
        if (!s.empty()) s += L" | ";
        s += L"a" + std::to_wstring(o.adapterIndex) + L"/o" + std::to_wstring(o.outputIndex);
        s += L" ";
        s += o.deviceName.empty() ? L"(unnamed)" : o.deviceName;
        s += L" " + std::to_wstring(W(o.desktopRect)) + L"x" + std::to_wstring(H(o.desktopRect)) +
             L"+" + std::to_wstring(o.desktopRect.left) + L"+" + std::to_wstring(o.desktopRect.top);
        if (!o.attachedToDesktop) s += L" detached";
    }
    return s;
}

}  // namespace ecapture
