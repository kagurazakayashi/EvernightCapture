#include "DesktopGeometry.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include "core/Frame.h"

namespace ecapture {
namespace {

constexpr int64_t kCoordMin = std::numeric_limits<int32_t>::min();
constexpr int64_t kCoordMax = std::numeric_limits<int32_t>::max();

bool FitsInt32(int64_t v) { return v >= kCoordMin && v <= kCoordMax; }

bool SameRect(const PhysRect& a, const PhysRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool RootContains(const DesktopLayout& layout, const PhysRect& area) {
    if (!layout.known || RectIsEmpty(area) || RectIsEmpty(layout.bounds)) return false;
    return RectContains(layout.bounds, area);
}

}  // namespace

const char* DesktopTargetKindName(DesktopTargetKind kind) {
    switch (kind) {
        case DesktopTargetKind::kMonitor: return "monitor";
        case DesktopTargetKind::kDesktop: return "desktop";
        case DesktopTargetKind::kRegion: return "region";
    }
    return "unknown";
}

DesktopLayout BuildDesktopLayout(const std::vector<ScreenInfo>& screens) {
    DesktopLayout layout;
    layout.screens = screens;
    if (screens.empty()) return layout;

    // 平台若明确交回了根窗口范围，就以它为准：桌面不是"各屏 union"，因为屏与屏之间可能有
    // 未被任何输出覆盖的空洞。没有这一格时只为离线兼容合成 union，不声称这来自 X server。
    bool haveDesktop = false;
    for (const ScreenInfo& s : screens) {
        if (!s.desktopKnown || RectIsEmpty(s.desktopBounds)) continue;
        layout.bounds = s.desktopBounds;
        layout.known = true;
        haveDesktop = true;
        break;
    }
    if (!haveDesktop) {
        bool first = true;
        int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;
        for (const ScreenInfo& s : screens) {
            if (RectIsEmpty(s.bounds)) continue;
            if (first) {
                minX = s.bounds.left;
                minY = s.bounds.top;
                maxX = s.bounds.right;
                maxY = s.bounds.bottom;
                first = false;
            } else {
                minX = std::min<int64_t>(minX, s.bounds.left);
                minY = std::min<int64_t>(minY, s.bounds.top);
                maxX = std::max<int64_t>(maxX, s.bounds.right);
                maxY = std::max<int64_t>(maxY, s.bounds.bottom);
            }
        }
        if (!first && FitsInt32(minX) && FitsInt32(minY) && FitsInt32(maxX) && FitsInt32(maxY)) {
            layout.bounds = PhysRect{static_cast<int32_t>(minX), static_cast<int32_t>(minY),
                                     static_cast<int32_t>(maxX), static_cast<int32_t>(maxY)};
            layout.known = true;
        }
    }
    for (const ScreenInfo& s : screens) {
        if (layout.rootWindow == 0) layout.rootWindow = s.rootWindow;
        if (layout.displayScope.empty()) layout.displayScope = s.displayScope;
    }
    return layout;
}

DesktopLayout QueryDesktopLayout() { return BuildDesktopLayout(EnumScreens()); }

bool SameDesktopLayout(const DesktopLayout& a, const DesktopLayout& b) {
    if (a.known != b.known) return false;
    if (!a.known) return true;
    return SameRect(a.bounds, b.bounds) && a.rootWindow == b.rootWindow &&
           a.displayScope == b.displayScope &&
           ScreenTopologyFingerprint(a.screens) == ScreenTopologyFingerprint(b.screens);
}

DesktopRegionResult ResolveDesktopRegion(const RegionRequest& request,
                                         const DesktopLayout& layout) {
    DesktopRegionResult r;
    r.width = request.width;
    r.height = request.height;
    if (!request.given || request.width == 0 || request.height == 0) {
        r.status = DesktopRegionStatus::kEmpty;
        return r;
    }
    if (!layout.known || RectIsEmpty(layout.bounds)) {
        r.status = DesktopRegionStatus::kLayoutUnavailable;
        return r;
    }
    // 先用 64 位算真实右下边：越界诊断里要报用户请求的边，而不是回绕后的数。
    const int64_t right = static_cast<int64_t>(request.x) + static_cast<int64_t>(request.width);
    const int64_t bottom = static_cast<int64_t>(request.y) + static_cast<int64_t>(request.height);
    r.requestedRight = right > 0 ? static_cast<uint64_t>(right) : 0;
    r.requestedBottom = bottom > 0 ? static_cast<uint64_t>(bottom) : 0;
    if (!FitsInt32(right) || !FitsInt32(bottom)) {
        r.status = DesktopRegionStatus::kOverflow;
        return r;
    }
    if (request.width > kFrameMaxSide || request.height > kFrameMaxSide) {
        r.status = DesktopRegionStatus::kTooLarge;
        return r;
    }
    const uint64_t pixels = static_cast<uint64_t>(request.width) * request.height;
    if (pixels > kFrameMaxBytes / 4u) {
        r.status = DesktopRegionStatus::kTooLarge;
        return r;
    }
    r.area = PhysRect{request.x, request.y, static_cast<int32_t>(right),
                      static_cast<int32_t>(bottom)};
    if (!RectContains(layout.bounds, r.area)) {
        r.status = DesktopRegionStatus::kOutsideDesktop;
        return r;
    }
    r.status = DesktopRegionStatus::kOk;
    return r;
}

DesktopPixelMapping MapDesktopAreaToRootPixels(const PhysRect& area,
                                               const DesktopLayout& layout) {
    DesktopPixelMapping m;
    if (!RootContains(layout, area)) return m;
    const int64_t x = static_cast<int64_t>(area.left) - layout.bounds.left;
    const int64_t y = static_cast<int64_t>(area.top) - layout.bounds.top;
    if (!FitsInt32(x) || !FitsInt32(y)) return m;
    m.x = static_cast<int32_t>(x);
    m.y = static_cast<int32_t>(y);
    m.width = static_cast<uint32_t>(static_cast<int64_t>(area.right) - area.left);
    m.height = static_cast<uint32_t>(static_cast<int64_t>(area.bottom) - area.top);
    m.ok = m.width != 0 && m.height != 0;
    return m;
}

DesktopAreaCheck RecheckDesktopAreaSampling(const DesktopLayout& approvedLayout,
                                            const PhysRect& approvedArea,
                                            const DesktopLayout& current) {
    // "这一问没答案"与"桌面已经没了"分开：前者不能按没变放行，后者明确拒绝。
    if (!current.known) return DesktopAreaCheck::kUnverifiable;
    if (RectIsEmpty(current.bounds)) return DesktopAreaCheck::kGone;
    if (!approvedLayout.known || RectIsEmpty(approvedLayout.bounds) ||
        RectIsEmpty(approvedArea)) {
        return DesktopAreaCheck::kUnverifiable;
    }
    if (!SameDesktopLayout(approvedLayout, current)) return DesktopAreaCheck::kStale;
    if (!RootContains(current, approvedArea)) return DesktopAreaCheck::kStale;
    const DesktopPixelMapping m = MapDesktopAreaToRootPixels(approvedArea, current);
    return m.ok ? DesktopAreaCheck::kOk : DesktopAreaCheck::kStale;
}

std::wstring DescribeDesktopArea(const PhysRect& area) {
    return L"desktop " + std::to_wstring(area.right - area.left) + L"x" +
           std::to_wstring(area.bottom - area.top) + L"+" + std::to_wstring(area.left) + L"+" +
           std::to_wstring(area.top);
}

std::wstring DescribeDesktopLayout(const DesktopLayout& layout) {
    std::wstring text = L"desktop " +
                        std::to_wstring(layout.bounds.right - layout.bounds.left) + L"x" +
                        std::to_wstring(layout.bounds.bottom - layout.bounds.top) + L"+" +
                        std::to_wstring(layout.bounds.left) + L"+" +
                        std::to_wstring(layout.bounds.top);
    text += L" (" + std::to_wstring(layout.screens.size()) + L" logical monitor";
    if (layout.screens.size() != 1) text += L"s";
    text += L")";
    return text;
}

}  // namespace ecapture
