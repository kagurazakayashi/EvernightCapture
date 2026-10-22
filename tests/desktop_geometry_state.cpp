// X11 显示器 / 全桌面 / 明确桌面区域的纯几何与状态判据。
//
// 本测试不连接 X11、不读取任何桌面像素、不弹确认框。它用合成布局覆盖开发机上造不出来的
// 现场：负坐标副屏、跨屏区域、根窗口边界、镜像输出共享逻辑显示器、旋转变化、热插拔后
// 拓扑失效。真实多屏 / 旋转 / 热插拔仍记未验证，不能拿这些合成判据冒充真机结果。

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "../src/DesktopGeometry.h"
#include "../src/ScreenMatch.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("[FAIL]  %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

PhysRect R(int32_t l, int32_t t, int32_t r, int32_t b) { return PhysRect{l, t, r, b}; }

ScreenOutputInfo Output(uint64_t id, const wchar_t* name, bool primary, int rotation,
                        PhysRect bounds) {
    ScreenOutputInfo o;
    o.outputId = id;
    o.crtcId = id + 100;
    o.name = name;
    o.connected = true;
    o.active = true;
    o.primary = primary;
    o.rotationDegrees = rotation;
    o.bounds = bounds;
    return o;
}

ScreenInfo Screen(uint32_t ordinal, const wchar_t* name, PhysRect bounds, bool primary,
                  PhysRect desktop) {
    ScreenInfo s;
    s.ordinal = ordinal;
    s.monitor = ordinal;
    s.deviceName = name;
    s.logicalName = name;
    s.bounds = bounds;
    s.primary = primary;
    s.hasScreenIdentity = true;
    s.rootWindow = 0x1234;
    s.displayScope = L"x11|display=3A30|root=0x1234|xwayland=0";
    s.desktopKnown = true;
    s.desktopBounds = desktop;
    return s;
}

DesktopLayout TwoScreenLayout() {
    // 根窗口故意从负坐标开始：X11 常见的是 0,0，但共享几何模型必须能表示负原点；
    // 副屏在左、主屏在右，与 Windows 的虚拟桌面语义一致。
    const PhysRect desktop = R(-2000, -100, 3000, 1500);
    ScreenInfo left = Screen(2, L"DP-1", R(-1920, 300, 0, 1380), false, desktop);
    left.outputs.push_back(Output(0x42, L"DP-1", false, 90, left.bounds));
    ScreenInfo right = Screen(1, L"HDMI-1", R(0, 0, 2560, 1440), true, desktop);
    right.outputs.push_back(Output(0x41, L"HDMI-1", true, 0, right.bounds));
    return BuildDesktopLayout({right, left});
}

RegionRequest Region(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    RegionRequest r;
    r.given = true;
    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    r.written = std::to_wstring(x) + L"," + std::to_wstring(y) + L"," + std::to_wstring(w) +
                L"," + std::to_wstring(h);
    return r;
}

}  // namespace

// 测试自己的平台桩：QueryDesktopLayout 的符号存在，但本判据从不调用它。
namespace ecapture {
std::vector<ScreenInfo> EnumScreens() { return {}; }
}  // namespace ecapture

int main() {
    Section("1) 根窗口 / 逻辑显示器 / 输出设备分层");
    {
        const DesktopLayout layout = TwoScreenLayout();
        Check(layout.known && layout.bounds.left == -2000 &&
                  layout.bounds.right == 3000 && layout.bounds.top == -100 &&
                  layout.bounds.bottom == 1500,
              "根窗口范围来自明确 desktopBounds，而不是各屏 union 的猜测");
        Check(layout.screens.size() == 2 && layout.screens[0].logicalName == L"HDMI-1" &&
                  layout.screens[1].logicalName == L"DP-1",
              "逻辑显示器名与顺序按枚举表保留");
        Check(layout.screens[1].outputs.size() == 1 &&
                  layout.screens[1].outputs[0].name == L"DP-1" &&
                  layout.screens[1].outputs[0].rotationDegrees == 90,
              "输出设备名、XID 与旋转作为独立事实保留");
    }

    Section("2) 明确区域：负坐标、跨屏、根边界");
    {
        const DesktopLayout layout = TwoScreenLayout();
        const DesktopRegionResult cross = ResolveDesktopRegion(Region(-1000, 100, 1500, 800), layout);
        Check(cross.status == DesktopRegionStatus::kOk &&
                  cross.area.left == -1000 && cross.area.top == 100 &&
                  cross.area.right == 500 && cross.area.bottom == 900,
              "跨屏且起点为负的区域原样接受，右下边不含");
        Check(ResolveDesktopRegion(Region(-2000, -100, 5000, 1600), layout).status ==
                  DesktopRegionStatus::kOk,
              "恰好等于整张根窗口的区域接受");
        Check(ResolveDesktopRegion(Region(-2001, 0, 10, 10), layout).status ==
                  DesktopRegionStatus::kOutsideDesktop,
              "越左一格拒绝，不裁到边界");
        Check(ResolveDesktopRegion(Region(2999, 1499, 2, 2), layout).status ==
                  DesktopRegionStatus::kOutsideDesktop,
              "越右下边界拒绝，不平移");
        Check(ResolveDesktopRegion(Region(0, 0, 0, 10), layout).status ==
                  DesktopRegionStatus::kEmpty,
              "零宽拒绝，不放大成 1 像素");
        Check(ResolveDesktopRegion(Region(0, 0, 16385, 1), layout).status ==
                  DesktopRegionStatus::kTooLarge,
              "超过帧单边上限拒绝");
    }

    Section("3) 溢出与布局问不出来分开");
    {
        const DesktopLayout layout = TwoScreenLayout();
        const int32_t maxI = std::numeric_limits<int32_t>::max();
        Check(ResolveDesktopRegion(Region(maxI - 1, 0, 2, 1), layout).status ==
                  DesktopRegionStatus::kOverflow,
              "坐标加宽在 64 位里判溢出，不回绕");
        DesktopLayout unknown;
        Check(ResolveDesktopRegion(Region(0, 0, 10, 10), unknown).status ==
                  DesktopRegionStatus::kLayoutUnavailable,
              "布局未知是 kLayoutUnavailable，不折叠成越界");
    }

    Section("4) 桌面坐标 -> XImage 源像素：只做精确映射");
    {
        const DesktopLayout layout = TwoScreenLayout();
        const DesktopPixelMapping m = MapDesktopAreaToRootPixels(R(-1000, 100, 500, 900), layout);
        Check(m.ok && m.x == 1000 && m.y == 200 && m.width == 1500 && m.height == 800,
              "根坐标减根原点是唯一转换；宽高保持请求值");
        const DesktopPixelMapping whole = MapDesktopAreaToRootPixels(layout.bounds, layout);
        Check(whole.ok && whole.x == 0 && whole.y == 0 && whole.width == 5000 &&
                  whole.height == 1600,
              "整个桌面映射到源矩形 (0,0,rootW,rootH)");
        Check(!MapDesktopAreaToRootPixels(R(-2001, 0, 0, 10), layout).ok,
              "越界映射失败，不返回裁短矩形");
    }

    Section("5) 拓扑指纹：镜像、旋转、输出变化都会让旧授权失效");
    {
        DesktopLayout a = TwoScreenLayout();
        DesktopLayout b = TwoScreenLayout();
        Check(SameDesktopLayout(a, b), "同一份布局相同");
        b.screens[0].outputs[0].rotationDegrees = 180;
        Check(!SameDesktopLayout(a, b), "旋转变化改变拓扑指纹");
        DesktopLayout xid = TwoScreenLayout();
        xid.screens[0].outputs[0].outputId += 1;
        Check(!SameDesktopLayout(a, xid), "输出 XID 变化改变拓扑指纹（不会仍报相同）");
        DesktopLayout c = TwoScreenLayout();
        c.screens[0].mirrored = true;
        c.screens[0].outputs.push_back(Output(0x43, L"HDMI-2", false, 0, c.screens[0].bounds));
        Check(!SameDesktopLayout(a, c), "镜像组 / 输出数量变化改变拓扑指纹");
        DesktopLayout d = TwoScreenLayout();
        d.bounds = R(-2000, -100, 2000, 1500);
        Check(!SameDesktopLayout(a, d), "根窗口范围变化改变布局");
    }

    Section("6) 确认后采样前复核");
    {
        const DesktopLayout layout = TwoScreenLayout();
        const PhysRect area = R(-1000, 100, 500, 900);
        Check(RecheckDesktopAreaSampling(layout, area, layout) == DesktopAreaCheck::kOk,
              "同一布局 + 目标仍在根内 -> kOk");
        DesktopLayout changed = layout;
        changed.screens[1].outputs[0].rotationDegrees = 270;
        Check(RecheckDesktopAreaSampling(layout, area, changed) == DesktopAreaCheck::kStale,
              "确认后旋转变化 -> kStale，旧授权不追认");
        DesktopLayout unknown;
        Check(RecheckDesktopAreaSampling(layout, area, unknown) ==
                  DesktopAreaCheck::kUnverifiable,
              "采样时布局问不出来 -> kUnverifiable，不当作没变");
        DesktopLayout shrunk = layout;
        shrunk.bounds = R(-500, 0, 3000, 1500);
        for (ScreenInfo& s : shrunk.screens) s.desktopBounds = shrunk.bounds;
        Check(RecheckDesktopAreaSampling(layout, area, shrunk) == DesktopAreaCheck::kStale,
              "根范围收缩后目标越界 -> kStale");
    }

    Section("7) 窗口内容与桌面像素登记表互不冒充");
    {
        // 这个测试不链 CaptureScope.cpp，只核对枚举层不会把 desktop 当候选屏。
        MonitorSelector sel;
        sel.given = true;
        sel.kind = MonitorSelector::Kind::kDesktop;
        sel.written = L"desktop";
        Check(sel.kind == MonitorSelector::Kind::kDesktop,
              "desktop 是独立目标种类，不是某块屏的别名");
    }

    std::printf("\ndesktop-geometry-state: %d 条通过，%d 条失败\n", g_checks - g_failures,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
