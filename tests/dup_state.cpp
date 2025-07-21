// Desktop Duplication 的旋转、裁剪与适配器定位的离线判据（由 tests\dup.ps1 运行
// build\ecapture-dup-tests.exe）。
//
// 测的是生产判据本体：src/DupGeometry.cpp（桌面坐标 -> 纹理坐标 -> 交付图像坐标）、
// src/ImageOps.cpp 的 RotateCropFrame（像素搬移）、以及 src/ScreenMatch.cpp 的 CompareScreen
// （确认之后那块屏变了怎么办）。真机上这几件事要么造不出稳定现场（要把用户的显示方向改掉、
// 要拔显示器、必须两块显卡），要么根本没有阴性对照（旋转屏上"尺寸对、画面横躺"的图看起来
// 就是一次成功），所以在这里逐条注入。
//
// 关键的一条：像素判据**独立于实现**。测试自己先按定义把整幅纹理顺时针旋转成用户看到的朝向
// （前向映射：源点 (ox,oy) 落在目标哪一格），再按桌面坐标裁出目标那一大块 —— "先旋转再裁剪"
// 这个参照物是测试自己写的朴素版本，生产的"先换算矩形再只搬需要的像素"必须逐点给出同一张图。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <cstdio>
#include <cstdint>
#include <vector>

#include "../src/CaptureCommon.h"
#include "../src/DupGeometry.h"
#include "../src/ImageOps.h"
#include "../src/ScreenMatch.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

RECT Rect(long l, long t, long r, long b) { return RECT{l, t, r, b}; }

// ---------------------------------------------------------------------------
// 合成帧：每个像素的 RGBA 都由它的 (x,y) 唯一决定，所以"这一格是从哪一格来的"能被逐点核对。
// 行距刻意可以比 width*4 大（GPU 与 GDI 都会填充），判据要证明填充字节不参与画面。
// ---------------------------------------------------------------------------
CapturedFrame MakeFrame(uint32_t width, uint32_t height, uint32_t stridePadding = 0) {
    CapturedFrame f;
    f.width = width;
    f.height = height;
    f.stride = width * 4u + stridePadding;
    f.pixels.assign(static_cast<size_t>(f.stride) * height, 0);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = f.pixels.data() + static_cast<size_t>(y) * f.stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = row + static_cast<size_t>(x) * 4u;
            p[0] = static_cast<uint8_t>((x * 7u + y * 13u) % 251u + 1u);
            p[1] = static_cast<uint8_t>(x % 256u);
            p[2] = static_cast<uint8_t>(y % 256u);
            p[3] = static_cast<uint8_t>((x / 256u) * 16u + (y / 256u) + 200u);
        }
    }
    return f;
}

std::vector<uint8_t> At(const CapturedFrame& f, uint32_t x, uint32_t y) {
    const uint8_t* p = f.pixels.data() + static_cast<size_t>(y) * f.stride +
                       static_cast<size_t>(x) * 4u;
    return {p[0], p[1], p[2], p[3]};
}

bool SamePixels(const CapturedFrame& a, const CapturedFrame& b) {
    if (a.width != b.width || a.height != b.height) return false;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            if (At(a, x, y) != At(b, x, y)) return false;
        }
    }
    return true;
}

// 独立判据第一步：把整幅纹理**按定义**顺时针转 angle 度（前向映射：源点落在目标哪一格），
// 得到用户看到的那个朝向。这里不复用生产实现里的任何一条公式。
CapturedFrame NaiveRotateWhole(const CapturedFrame& tex, uint32_t angle) {
    CapturedFrame out;
    const bool swapSides = angle == 90u || angle == 270u;
    out.width = swapSides ? tex.height : tex.width;
    out.height = swapSides ? tex.width : tex.height;
    out.stride = out.width * 4u;
    out.pixels.assign(static_cast<size_t>(out.stride) * out.height, 0);
    for (uint32_t oy = 0; oy < tex.height; ++oy) {
        for (uint32_t ox = 0; ox < tex.width; ++ox) {
            uint32_t nx = ox;
            uint32_t ny = oy;
            switch (angle) {
                case 90u:  nx = tex.height - 1u - oy; ny = ox; break;
                case 180u: nx = tex.width - 1u - ox; ny = tex.height - 1u - oy; break;
                case 270u: nx = oy; ny = tex.width - 1u - ox; break;
                default: break;
            }
            const uint8_t* src = tex.pixels.data() + static_cast<size_t>(oy) * tex.stride +
                                 static_cast<size_t>(ox) * 4u;
            uint8_t* dst = out.pixels.data() + static_cast<size_t>(ny) * out.stride +
                           static_cast<size_t>(nx) * 4u;
            for (int i = 0; i < 4; ++i) dst[i] = src[i];
        }
    }
    return out;
}

// 独立判据第二步：从"已经转正的整幅桌面图"里按桌面坐标裁出目标那一块。
CapturedFrame NaiveCrop(const CapturedFrame& rotated, const RECT& desktop, const RECT& visible) {
    CapturedFrame out;
    out.width = static_cast<uint32_t>(visible.right - visible.left);
    out.height = static_cast<uint32_t>(visible.bottom - visible.top);
    out.stride = out.width * 4u;
    out.pixels.assign(static_cast<size_t>(out.stride) * out.height, 0);
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            const uint8_t* src = rotated.pixels.data() +
                                 static_cast<size_t>(visible.top - desktop.top + y) *
                                     rotated.stride +
                                 static_cast<size_t>(visible.left - desktop.left + x) * 4u;
            uint8_t* dst = out.pixels.data() + static_cast<size_t>(y) * out.stride +
                           static_cast<size_t>(x) * 4u;
            for (int i = 0; i < 4; ++i) dst[i] = src[i];
        }
    }
    return out;
}

// 一条完整的"旋转 + 裁剪"判据：生产的换算与搬移必须与上面那两步逐点相同。
// 纹理尺寸按旋转推：90/270 时驱动交的是面板朝向，宽高与桌面相反。
void CheckRotateThenCrop(uint32_t angle, const RECT& desktop, const RECT& target,
                        const char* label) {
    const uint32_t dw = static_cast<uint32_t>(desktop.right - desktop.left);
    const uint32_t dh = static_cast<uint32_t>(desktop.bottom - desktop.top);
    const bool swap = angle == 90u || angle == 270u;
    const CapturedFrame tex =
        MakeFrame(swap ? dh : dw, swap ? dw : dh, /*stridePadding=*/64);

    const DupCropPlan plan = PlanDupCrop(desktop, target, tex.width, tex.height, angle);
    CapturedFrame got;
    if (!plan.ok || !RotateCropFrame(tex, plan.src, angle, &got)) {
        Check(false, label);
        return;
    }
    const CapturedFrame want = NaiveCrop(NaiveRotateWhole(tex, angle), desktop, plan.captured);
    Check(SamePixels(got, want) && got.width == want.width && got.height == want.height, label);
}

// ---------------------------------------------------------------------------
// 1) 尺寸对照：驱动交回的纹理到底是"面板朝向"还是"已经转好的"
// ---------------------------------------------------------------------------
void TestTransform() {
    Section("交回的纹理尺寸与该输出宣称的桌面尺寸");

    const DupTransformDecision id =
        DecideDupTransform(DupRotation::kIdentity, 1920, 1080, 1920, 1080);
    Check(id.ok && id.angle == 0u, "不旋转 + 尺寸一致：转 0 度");
    const DupTransformDecision idBad =
        DecideDupTransform(DupRotation::kIdentity, 1920, 1080, 1080, 1920);
    Check(!idBad.ok && idBad.reason == DupTransformReason::kFrameMismatch,
          "不旋转却给了交换过宽高的纹理：判这张帧说不通，不去裁");

    const DupTransformDecision r90 =
        DecideDupTransform(DupRotation::kRotate90, 1080, 1920, 1920, 1080);
    Check(r90.ok && r90.angle == 90u, "竖屏（桌面 1080x1920）+ 纹理 1920x1080：顺时针转 90 度");
    const DupTransformDecision r270 =
        DecideDupTransform(DupRotation::kRotate270, 1080, 1920, 1920, 1080);
    Check(r270.ok && r270.angle == 270u, "反向竖屏：顺时针转 270 度");

    // 这一条是"宽高重复交换"的判据：有的驱动已经把画面转好才交回来，
    // 此时纹理尺寸与桌面一致，再转一次就把画面转回去了。
    const DupTransformDecision pre90 =
        DecideDupTransform(DupRotation::kRotate90, 1080, 1920, 1080, 1920);
    Check(pre90.ok && pre90.angle == 0u, "报 90 度但纹理已经是桌面那么大：不再转（避免二次交换）");
    const DupTransformDecision pre270 =
        DecideDupTransform(DupRotation::kRotate270, 1080, 1920, 1080, 1920);
    Check(pre270.ok && pre270.angle == 0u, "报 270 度但纹理已经是桌面那么大：同样不再转");

    const DupTransformDecision r180 =
        DecideDupTransform(DupRotation::kRotate180, 1920, 1080, 1920, 1080);
    Check(r180.ok && r180.angle == 180u, "反向横屏：180 度不交换宽高，按报出的旋转转");
    const DupTransformDecision r180Bad =
        DecideDupTransform(DupRotation::kRotate180, 1920, 1080, 1080, 1920);
    Check(!r180Bad.ok, "180 度却给了交换过的纹理：判不合法（180 度不会交换）");

    // 正方形屏幕：交换与不交换是同一个尺寸，只能照报出的旋转处理（尺寸给不出反证）
    const DupTransformDecision square =
        DecideDupTransform(DupRotation::kRotate90, 1000, 1000, 1000, 1000);
    Check(square.ok && square.angle == 90u, "正方形桌面 + 报 90 度：按报出的旋转转");

    const DupTransformDecision unspec =
        DecideDupTransform(DupRotation::kUnspecified, 1920, 1080, 1920, 1080);
    Check(unspec.ok && unspec.angle == 0u, "旋转值没报：尺寸与桌面一致时当作不用转");
    const DupTransformDecision unspecSwap =
        DecideDupTransform(DupRotation::kUnspecified, 1920, 1080, 1080, 1920);
    Check(!unspecSwap.ok, "旋转值没报而纹理是交换过的：不猜方向，判不合法");

    const DupTransformDecision zero =
        DecideDupTransform(DupRotation::kIdentity, 0, 0, 1920, 1080);
    Check(!zero.ok && zero.reason == DupTransformReason::kDesktopRectEmpty, "桌面矩形为空：不处理");
}

// ---------------------------------------------------------------------------
// 2) 桌面坐标 -> 纹理坐标 -> 交付图像：与"先旋转再裁剪"逐点对拍
// ---------------------------------------------------------------------------
void TestRotateCropGeometry() {
    Section("四种旋转下的矩形换算（与朴素的先旋转再裁剪对拍）");

    const RECT land = Rect(0, 0, 200, 120);
    CheckRotateThenCrop(0u, land, Rect(20, 30, 90, 80), "0 度：屏内一块窗口逐点相同");
    CheckRotateThenCrop(180u, land, Rect(20, 30, 90, 80), "180 度：换算与搬移逐点相同");

    const RECT port = Rect(0, 0, 120, 200);
    CheckRotateThenCrop(90u, port, Rect(10, 40, 70, 150), "90 度：换算与搬移逐点相同");
    CheckRotateThenCrop(270u, port, Rect(10, 40, 70, 150), "270 度：换算与搬移逐点相同");
    CheckRotateThenCrop(90u, port, Rect(0, 0, 120, 200), "90 度：整屏大小也要逐点相同");

    // 负坐标：副屏在主屏左边/上边，桌面矩形整个在负值区
    CheckRotateThenCrop(0u, Rect(-200, -120, 0, 0), Rect(-180, -90, -110, -40),
                        "负坐标桌面 + 0 度：仍然逐点相同");
    CheckRotateThenCrop(90u, Rect(-120, -200, 0, 0), Rect(-110, -160, -50, -50),
                        "负坐标桌面 + 90 度：仍然逐点相同");
    CheckRotateThenCrop(270u, Rect(-120, -200, 0, 0), Rect(-110, -160, -50, -50),
                        "负坐标桌面 + 270 度：仍然逐点相同");

    // 丢区域的那种（目标比这块屏大）也要对拍：交付的只能是重叠那一片
    CheckRotateThenCrop(90u, port, Rect(-50, 100, 200, 300), "90 度 + 跨出该屏：逐点仍然相同");
    CheckRotateThenCrop(180u, land, Rect(150, 60, 400, 400), "180 度 + 跨出该屏：逐点仍然相同");
}

void TestCropPlan() {
    Section("裁剪计划：交付尺寸、capturedRect 与丢了哪几边");

    const RECT d = Rect(0, 0, 1920, 1080);
    const DupCropPlan full = PlanDupCrop(d, Rect(100, 100, 500, 300), 1920, 1080, 0u);
    Check(full.ok, "屏内窗口：计划成立");
    Check(!full.clipped && full.outWidth == 400 && full.outHeight == 200, "屏内窗口不算丢区域");
    Check(full.captured.left == 100 && full.captured.right == 500 && full.requested.left == 100,
          "capturedRect 与 requestedRect 分开报告");

    // 跨屏：窗口横跨两块屏，这一条通道只取一块输出 —— 必须报成丢区域而不是默认"这就是整个窗口"
    const DupCropPlan cross = PlanDupCrop(d, Rect(1800, 100, 2600, 300), 1920, 1080, 0u);
    Check(cross.ok && cross.clipped, "窗口跨到第二块屏：判为丢区域（只截到重叠那块）");
    Check(cross.outWidth == 120 && cross.captured.right == 1920, "交付尺寸只到该屏右边界");
    Check(cross.lostRight == 680 && cross.lostLeft == 0, "丢掉的部分写在右边 680 像素");

    const DupCropPlan left = PlanDupCrop(d, Rect(-200, 50, 300, 250), 1920, 1080, 0u);
    Check(left.ok && left.clipped && left.lostLeft == 200 && left.outWidth == 300,
          "一部分在屏幕左边之外：只从 0 开始取，左边丢 200");

    const DupCropPlan bottom = PlanDupCrop(d, Rect(10, 1000, 210, 2000), 1920, 1080, 0u);
    Check(bottom.ok && bottom.clipped && bottom.lostBottom == 920, "超出下边界：下面丢 920");

    const DupCropPlan outside = PlanDupCrop(d, Rect(3000, 0, 3100, 100), 1920, 1080, 0u);
    Check(!outside.ok && outside.reason == DupCropReason::kNotOnOutput,
          "完全在这块屏之外：不裁（换一块屏去截就是截了没人批准过的画面）");

    const DupCropPlan empty = PlanDupCrop(d, Rect(50, 50, 50, 90), 1920, 1080, 0u);
    Check(!empty.ok && empty.reason == DupCropReason::kTargetEmpty, "目标矩形宽为 0：判目标空");

    const DupCropPlan corner = PlanDupCrop(d, Rect(1900, 1060, 1930, 1090), 1920, 1080, 0u);
    Check(corner.ok && corner.outWidth == 20 && corner.outHeight == 20,
          "只有一角在屏内：交出那 20x20，不谎称完整窗口");

    // 帧比宣称的桌面小（驱动自相矛盾）：换算出来的读取矩形会出界，必须拦下来
    const DupCropPlan shortFrame = PlanDupCrop(d, Rect(100, 100, 500, 300), 400, 200, 0u);
    Check(!shortFrame.ok && shortFrame.reason == DupCropReason::kOutsideTexture,
          "桌面宣称 1920 宽而帧只有 400：读取矩形出界，判不合法");

    const DupCropPlan badAngle = PlanDupCrop(d, Rect(0, 0, 100, 100), 1920, 1080, 45u);
    Check(!badAngle.ok && badAngle.reason == DupCropReason::kBadAngle, "角度不是 0/90/180/270：不猜");

    // 90 度时读取矩形的宽高与交付尺寸相反 —— 这条正是"宽高交换只做一次"的判据
    const RECT v = Rect(0, 0, 1080, 1920);
    const DupCropPlan r90 = PlanDupCrop(v, Rect(20, 100, 620, 400), 1920, 1080, 90u);
    Check(r90.ok && r90.outWidth == 600 && r90.outHeight == 300,
          "90 度：交付尺寸是桌面朝向的 600x300，不是纹理的 300x600");
    Check(r90.src.right - r90.src.left == 300 && r90.src.bottom - r90.src.top == 600,
          "90 度：读取矩形是交换过的那一份（转回来才等于交付尺寸）");
}

void TestRotateCropPixels() {
    Section("RotateCropFrame：四个角、行距填充、边界与不合法输入");

    const CapturedFrame tex = MakeFrame(64, 32, /*stridePadding=*/128);
    CapturedFrame out = MakeFrame(3, 3);

    Check(RotateCropFrame(tex, Rect(4, 5, 12, 9), 0u, &out), "0 度：裁剪成功");
    Check(out.width == 8 && out.height == 4 && out.stride == 32u,
          "0 度：行距恒等于宽*4（源帧的填充不带进结果）");
    Check(At(out, 0, 0) == At(tex, 4, 5) && At(out, 7, 3) == At(tex, 11, 8),
          "0 度：左上与右下都取自填过行距的那张源帧");

    Check(RotateCropFrame(tex, Rect(0, 0, 64, 32), 90u, &out), "90 度：整幅纹理转成竖的");
    Check(out.width == 32 && out.height == 64, "90 度：交付尺寸是交换过的那一份");
    Check(At(out, 0, 0) == At(tex, 0, 31), "90 度：交付左上角 = 源矩形左下角");
    Check(At(out, 31, 63) == At(tex, 63, 0), "90 度：交付右下角 = 源矩形右上角");

    Check(RotateCropFrame(tex, Rect(0, 0, 64, 32), 180u, &out), "180 度：不交换尺寸");
    Check(out.width == 64 && out.height == 32, "180 度：交付尺寸不变");
    Check(At(out, 0, 0) == At(tex, 63, 31), "180 度：交付左上角 = 源矩形右下角");

    Check(RotateCropFrame(tex, Rect(0, 0, 64, 32), 270u, &out), "270 度：整幅纹理转成竖的");
    Check(out.width == 32 && out.height == 64, "270 度：交付尺寸同样是交换过的那一份");
    Check(At(out, 0, 0) == At(tex, 63, 0), "270 度：交付左上角 = 源矩形右上角");
    Check(At(out, 31, 63) == At(tex, 0, 31), "270 度：交付右下角 = 源矩形左下角");

    CapturedFrame untouched = MakeFrame(5, 5);
    const std::vector<uint8_t> before = untouched.pixels;
    Check(!RotateCropFrame(tex, Rect(0, 0, 10, 10), 45u, &untouched), "角度 45 度：拒绝");
    Check(!RotateCropFrame(tex, Rect(60, 0, 80, 10), 0u, &untouched), "越出帧的右边界：拒绝");
    Check(!RotateCropFrame(tex, Rect(0, 30, 10, 40), 0u, &untouched), "越出下边界：拒绝");
    Check(!RotateCropFrame(tex, Rect(-1, 0, 10, 10), 0u, &untouched), "负起点：拒绝");
    Check(!RotateCropFrame(tex, Rect(0, 0, 0, 10), 0u, &untouched), "零宽：拒绝");
    Check(untouched.pixels == before && untouched.width == 5u, "失败时 *out 一个字节都没被改");

    CapturedFrame broken = MakeFrame(8, 8);
    broken.pixels.resize(broken.pixels.size() - 4u);   // 缓冲区比 行距×高 短一截
    Check(!RotateCropFrame(broken, Rect(0, 0, 4, 4), 90u, &untouched),
          "源帧缓冲区与行距不自洽：拒绝，不读越界");

    const CapturedFrame padded = MakeFrame(8, 8, /*stridePadding=*/16);
    Check(RotateCropFrame(padded, Rect(0, 0, 8, 4), 90u, &out) && out.width == 4u &&
              out.height == 8u,
          "带行距填充的源帧 + 90 度：按行距取字节并交换尺寸");
    Check(At(out, 0, 0) == At(padded, 0, 3) && At(out, 3, 7) == At(padded, 7, 0),
          "带行距填充 + 90 度：四个角仍然各就各位");
}

// ---------------------------------------------------------------------------
// 3) 多显卡：先按拓扑定位目标，再到目标所属适配器上建设备
// ---------------------------------------------------------------------------
DupOutputInfo Out(uint32_t adapter, uint32_t index, const wchar_t* name, long l, long t, long r,
                  long b, bool attached = true) {
    DupOutputInfo o;
    o.adapterIndex = adapter;
    o.outputIndex = index;
    o.deviceName = name;
    o.desktopRect = Rect(l, t, r, b);
    o.attachedToDesktop = attached;
    return o;
}

void TestPickOutput() {
    Section("adapter/output 定位（假枚举器）");

    // 两块显卡：核显（适配器 0）带 1 号屏，独显（适配器 1）带 2 号屏。
    // 旧实现先建默认适配器（0）的设备、只枚举它的输出，于是 2 号屏根本不在表里。
    const std::vector<DupOutputInfo> two = {
        Out(0, 0, L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080),
        Out(1, 0, L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080),
    };
    const DupPickResult onSecond =
        PickDupOutputForScreen(two, L"\\\\.\\DISPLAY2", Rect(1920, 0, 3840, 1080));
    Check(onSecond.status == DupPickStatus::kFound && onSecond.adapterIndex == 1u,
          "目标屏在第二块适配器上：定位到它（旧实现在这里会漏掉整块屏）");
    Check(onSecond.matchedByDeviceName, "设备名对上时优先用它，不用比矩形");

    const DupPickResult byRect =
        PickDupOutputForScreen(two, L"\\\\.\\WARP9", Rect(1920, 0, 3840, 1080));
    Check(byRect.status == DupPickStatus::kFound && byRect.adapterIndex == 1u &&
              !byRect.matchedByDeviceName,
          "设备名两边写法不一致时退而比矩形，并说明不是同名命中的");

    const DupPickResult gone =
        PickDupOutputForScreen(two, L"\\\\.\\DISPLAY3", Rect(4000, 0, 5000, 900));
    Check(gone.status == DupPickStatus::kNoMatch, "没有那块屏：报找不到，不随便挑一块还在的");

    const std::vector<DupOutputInfo> one = {Out(0, 0, L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080)};
    const DupPickResult first =
        PickDupOutputForScreen(one, L"\\\\.\\DISPLAY1", Rect(0, 0, 1920, 1080));
    Check(first.status == DupPickStatus::kFound && first.adapterIndex == 0u &&
              first.outputIndex == 0u,
          "单屏机器：仍然定位到 0 号适配器的 0 号输出");

    const std::vector<DupOutputInfo> detached = {
        Out(0, 0, L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, /*attached=*/false),
        Out(1, 0, L"\\\\.\\DISPLAY2", 1920, 0, 2560, 640, false),
    };
    Check(PickDupOutputForScreen(detached, L"\\\\.\\DISPLAY1", Rect(0, 0, 1920, 1080))
              .status == DupPickStatus::kNoOutputs,
          "输出全都没接进桌面（拔除中）：报没有可用输出");

    const std::vector<DupOutputInfo> none;
    Check(PickDupOutputForScreen(none, L"\\\\.\\DISPLAY1", Rect(0, 0, 100, 100))
              .status == DupPickStatus::kNoOutputs,
          "一个输出都没枚举到（远程会话 / 基本显示驱动）：报没有可用输出");

    // 窗口目标：取重叠最多那块；并列时取先枚举到的（同一台机器同一窗口每次同一块屏）
    const DupPickResult overlap = PickDupOutputForRect(two, Rect(1900, 0, 2400, 400));
    Check(overlap.status == DupPickStatus::kFound && overlap.adapterIndex == 1u,
          "窗口大部分在第二块屏：按重叠面积选第二块适配器上的输出");
    const DupPickResult tie = PickDupOutputForRect(two, Rect(880, 0, 2920, 100));
    Check(tie.status == DupPickStatus::kFound && tie.adapterIndex == 0u,
          "两块屏重叠一样多：取先枚举到的那块（确定性，不随驱动顺序抖）");
    Check(PickDupOutputForRect(two, Rect(5000, 5000, 5100, 5100)).status == DupPickStatus::kNoMatch,
          "窗口在桌面之外：报找不到而不是交错图");
    Check(PickDupOutputForRect(none, Rect(0, 0, 100, 100)).status == DupPickStatus::kNoOutputs,
          "没有输出可枚举：窗口目标也报同一件事");

    const std::wstring brief = BriefDupOutputs(two);
    Check(brief.find(L"a1/o0") != std::wstring::npos &&
              brief.find(L"DISPLAY2") != std::wstring::npos &&
              brief.find(L"1920x1080+1920+0") != std::wstring::npos,
          "输出一览带适配器/输出序号与矩形（找不到时 hint 要能定位）");
    Check(BriefDupOutputs(detached).find(L"detached") != std::wstring::npos,
          "没接进桌面的输出在一览里标出来");
}

// ---------------------------------------------------------------------------
// 4) 确认之后那块屏变了怎么办（编号只是本次枚举的位置，设备名才是身份）
// ---------------------------------------------------------------------------
ScreenInfo Scr(uint32_t ordinal, const wchar_t* name, long l, long t, long r, long b,
               bool primary = false) {
    ScreenInfo s;
    s.ordinal = ordinal;
    s.deviceName = name;
    s.bounds = Rect(l, t, r, b);
    s.primary = primary;
    return s;
}

void TestCompareScreen() {
    Section("取帧前重新核对那块屏");

    const ScreenInfo wanted = Scr(1, L"\\\\.\\DISPLAY1", 0, 0, 1920, 1080, true);
    ScreenInfo fresh{};

    const std::vector<ScreenInfo> same = {wanted, Scr(2, L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080)};
    Check(CompareScreen(wanted, same, &fresh) == ScreenCheck::kSame, "什么都没变：照旧取帧");

    const std::vector<ScreenInfo> resized = {
        Scr(1, L"\\\\.\\DISPLAY1", 0, 0, 1280, 720, true),
        Scr(2, L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080)};
    Check(CompareScreen(wanted, resized, &fresh) == ScreenCheck::kMoved &&
              fresh.bounds.right == 1280,
          "那块屏改了分辨率：换新矩形，人会重新确认这个范围（旧授权不用于新尺寸）");

    const std::vector<ScreenInfo> reordered = {
        Scr(2, L"\\\\.\\DISPLAY1", 1920, 0, 3840, 1080, false),
        Scr(1, L"\\\\.\\DISPLAY2", 0, 0, 1920, 1080, true)};
    Check(CompareScreen(wanted, reordered, &fresh) == ScreenCheck::kMoved && fresh.ordinal == 2u,
          "插拔之后编号变了：认设备名而不是编号，并报告新的编号");

    const std::vector<ScreenInfo> moved = {
        Scr(1, L"\\\\.\\DISPLAY1", 0, 100, 1920, 1180, true),
        Scr(2, L"\\\\.\\DISPLAY2", 1920, 0, 3840, 1080)};
    Check(CompareScreen(wanted, moved, &fresh) == ScreenCheck::kMoved,
          "那块屏被挪了位置（负坐标之外的另一种变化）：同样换新矩形");

    const std::vector<ScreenInfo> unplugged = {Scr(1, L"\\\\.\\DISPLAY2", 0, 0, 1920, 1080, true)};
    Check(CompareScreen(wanted, unplugged, &fresh) == ScreenCheck::kGone,
          "那块屏被拔掉：一个像素都不读");

    Check(CompareScreen(wanted, {}, &fresh) == ScreenCheck::kGone,
          "整个桌面枚举不到屏：同样判没了");
}

}  // namespace

int main() {
    TestTransform();
    TestRotateCropGeometry();
    TestCropPlan();
    TestRotateCropPixels();
    TestPickOutput();
    TestCompareScreen();

    std::printf("\ndup-geometry: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
