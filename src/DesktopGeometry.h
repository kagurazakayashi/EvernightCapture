#pragma once
// X11 / 跨平台桌面目标的纯几何模型：逻辑显示器、整个桌面与明确区域。
//
// 这一层不打开 X 连接、不读像素、不弹框，只回答四件事：
//   1. 当前屏幕事实里，根窗口（整个桌面）的范围是什么；
//   2. 用户给的 --region 是否**完整**落在根窗口里，空矩形 / 越界 / 溢出 / 超帧上限分别是什么；
//   3. 一块根坐标矩形如何映射成 XImage 的源像素矩形（源坐标 + 宽高），不裁切、不平移；
//   4. 人确认前后 / 采样前后，桌面拓扑与目标范围是否还是同一份。
//
// 坐标系：PhysRect 在这里就是 X11 根窗口像素坐标（Windows 虚拟屏幕同形）。
// 原点可以在负坐标，右下边不含。XFixes / RandR 的旋转已经在根窗口像素里落地，
// 下游绝不能把 rotationDegrees 再拿来旋转一次；本工具在 X11 桌面后端只填 rotation=0。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "ScreenMatch.h"
#include "core/Geometry.h"

namespace ecapture {

enum class DesktopTargetKind : uint32_t {
    kMonitor = 0,
    kDesktop = 1,
    kRegion = 2,
};

const char* DesktopTargetKindName(DesktopTargetKind kind);

// 一次只读拓扑快照。known=false 表示"根窗口范围没问出来"，不是"桌面是空的"。
struct DesktopLayout {
    bool known = false;
    PhysRect bounds{};
    uint64_t rootWindow = 0;
    std::wstring displayScope;
    std::vector<ScreenInfo> screens;
};

// 从一份已枚举的屏幕表构造布局。优先使用 ScreenInfo 里平台交回的根窗口范围；
// 没有该字段时只用各屏 union 作为兼容形状（X11 生产枚举总会带根窗口范围）。
DesktopLayout BuildDesktopLayout(const std::vector<ScreenInfo>& screens);

// 真机只读入口：调用平台 EnumScreens（不取像素、不改显示设置）。
DesktopLayout QueryDesktopLayout();

bool SameDesktopLayout(const DesktopLayout& a, const DesktopLayout& b);

// 区域解析的下场。kOutsideDesktop 的下一步是改坐标；kLayoutUnavailable 的下一步是查显示服务；
// kTooLarge 是请求本身超过帧交付上限。
enum class DesktopRegionStatus : uint32_t {
    kOk = 0,
    kEmpty,
    kLayoutUnavailable,
    kOutsideDesktop,
    kOverflow,
    kTooLarge,
};

struct DesktopRegionResult {
    DesktopRegionStatus status = DesktopRegionStatus::kEmpty;
    PhysRect area{};
    uint64_t requestedRight = 0;
    uint64_t requestedBottom = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// 严格判据：不把越界矩形裁到边界，不把负坐标平移，不把空矩形放大成 1x1。
DesktopRegionResult ResolveDesktopRegion(const RegionRequest& request,
                                         const DesktopLayout& layout);

// 根坐标 -> XImage 源像素坐标。source.x/y 是根窗口里的左上角，width/height 是请求的原尺寸。
// 只有完整落在根窗口里的非空矩形才成立；所有减法与加法在 64 位里判溢出。
struct DesktopPixelMapping {
    bool ok = false;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

DesktopPixelMapping MapDesktopAreaToRootPixels(const PhysRect& area,
                                               const DesktopLayout& layout);

// 采样前复核：拓扑指纹变了、根范围变了、或目标不再完整落在根范围里，都不能沿用旧授权。
enum class DesktopAreaCheck : uint32_t {
    kOk = 0,
    kStale,
    kGone,
    kUnverifiable,
};

DesktopAreaCheck RecheckDesktopAreaSampling(const DesktopLayout& approvedLayout,
                                            const PhysRect& approvedArea,
                                            const DesktopLayout& current);

// 人读的一行，不含用户名 / 文件路径；坐标可以是负数。
std::wstring DescribeDesktopArea(const PhysRect& area);
std::wstring DescribeDesktopLayout(const DesktopLayout& layout);

}  // namespace ecapture
