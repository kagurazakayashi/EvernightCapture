// 窗口内部裁剪（--roi / --client-area）的离线判据（由 tests\crop.ps1 运行
// build\ecapture-crop-tests.exe）。
//
// 测的是 src/CropGeometry.cpp 那三个纯函数本身，而不是在测试里另抄一份算式：真机上
// "一张比请求小的交付图像""客户区正好挂在屏幕边缘之外""窗口在选定之后改了尺寸"这几种现场
// 要么要求在取帧那一瞬命中某个尺寸，要么根本没有阴性对照（identity 与授权两道复核会先把
// 挪了位置的目标拦掉）。所以把注入面摆在几何这一层：交付图像的尺寸、它的屏幕矩形问没问到、
// 客户区那块矩形问没问到，全部由测试给。
//
// 判据关心的从来不是"裁得动裁不动"，而是这四件事：
//   1. 越界与空矩形只能拒绝，绝不裁到边上为止、也不退回整窗交出；
//   2. "问不出来"与"放不下"是两种结果，各有一种状态，不互相冒充；
//   3. 图像原点核实不出来时，宁可少写一个 cropScreenRect，也不交一个猜出来的屏幕坐标；
//   4. 相加与比较都在 64 位里判，坏输入不会绕回成一个"看起来合法"的矩形。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "../src/CaptureCommon.h"   // 只要 kFrameMaxSide 那一条资源上限（用来核对它没跟解析层漂移）
#include "../src/CropGeometry.h"

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

// 单边上限在这里**独立写死**：判据要能发现"实现偷偷改了上限而没人复核文档"，
// 而不是跟着实现走（与 tests\wgc_state.cpp 同一做法）。
constexpr uint32_t kMaxSide = 16384u;

CropRequest Roi(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    CropRequest r;
    r.mode = CropMode::kRoi;
    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    return r;
}

CropRequest ClientArea() {
    CropRequest r;
    r.mode = CropMode::kClientArea;
    return r;
}

RECT Rect(int64_t l, int64_t t, int64_t r, int64_t b) {
    RECT rc{};
    rc.left = static_cast<LONG>(l);
    rc.top = static_cast<LONG>(t);
    rc.right = static_cast<LONG>(r);
    rc.bottom = static_cast<LONG>(b);
    return rc;
}

DeliveredImage Image(uint32_t w, uint32_t h) {
    DeliveredImage img;
    img.width = w;
    img.height = h;
    return img;
}

// 一条"图像原点核实得出来"的图像：屏幕位置就是 screenRect，尺寸与它一致。
DeliveredImage ImageAt(uint32_t w, uint32_t h, int64_t left, int64_t top) {
    DeliveredImage img = Image(w, h);
    img.hasScreenRect = true;
    img.screenRect = Rect(left, top, left + w, top + h);
    return img;
}

ClientAreaProbe Client(int64_t l, int64_t t, int64_t r, int64_t b) {
    ClientAreaProbe p;
    p.readable = true;
    p.screenRect = Rect(l, t, r, b);
    return p;
}

ClientAreaProbe NoClient() { return ClientAreaProbe{}; }

// ---------------------------------------------------------------------------
// 1) --roi 的算术：贴边、越界、空矩形、绕回
// ---------------------------------------------------------------------------
void TestRoiArithmetic() {
    Section("--roi 的矩形算术（取帧前后的同一条算式）");

    Check(RoiFitsTarget(Roi(0, 0, 10, 10), 100, 80), "落在窗口之内：放行");
    Check(RoiFitsTarget(Roi(90, 70, 10, 10), 100, 80), "右下边正好贴到边界：合法（含左上、不含右下）");
    Check(!RoiFitsTarget(Roi(91, 0, 10, 10), 100, 80), "右边越过 1 像素：拒绝");
    Check(!RoiFitsTarget(Roi(0, 71, 10, 10), 100, 80), "下边越过 1 像素：拒绝");
    Check(!RoiFitsTarget(Roi(0, 0, 101, 10), 100, 80), "宽度本身比窗口还大：拒绝");
    Check(!RoiFitsTarget(Roi(0, 0, 10, 81), 100, 80), "高度本身比窗口还大：拒绝");
    Check(!RoiFitsTarget(Roi(0, 0, 10, 10), 0, 0), "目标矩形量不出来（0×0）时什么都不放得下");
    Check(!RoiFitsTarget(Roi(0, 0, 10, 10), 100, 0), "只有高度是 0 也一样放不下");
    Check(RoiFitsTarget(Roi(0, 0, 100, 80), 100, 80), "整幅都要（等于图像尺寸）：合法");

    // 判据不在解析层写死"不超过 16384"就万事大吉：相加仍在 64 位里判，
    // 大偏移 + 大宽度不会绕回成一个小数（旧实现借 wcstoull 时这类值会被静默强转）。
    Check(!RoiFitsTarget(Roi(kMaxSide, 0, kMaxSide, 1), 2 * kMaxSide - 1, kMaxSide),
          "上限 + 上限的相加在 64 位里判，不绕回");
    Check(RoiFitsTarget(Roi(0, 0, kMaxSide, kMaxSide), kMaxSide, kMaxSide),
          "单边上限本身是可达的尺寸");

    // --client-area 不是用户写的矩形，取帧之前那条判据对它不作任何结论
    Check(RoiFitsTarget(ClientArea(), 0, 0), "取帧之前不预判 --client-area（它照目标自己的几何算）");

    const CropResolution full = ResolveWindowCrop(Roi(0, 0, 10, 10), Image(10, 10), NoClient());
    Check(full.status == CropStatus::kCropped && full.crop.width == 10u && full.crop.height == 10u,
          "交付图像与请求一样大时整幅裁下来");
    const CropResolution none = ResolveWindowCrop(CropRequest{}, Image(10, 10), NoClient());
    Check(none.status == CropStatus::kNoCrop, "没给裁剪请求时状态就是没裁（不是裁成 0×0）");
}

// ---------------------------------------------------------------------------
// 2) 取到帧之后：图像比当初小（resize / 挂在屏幕外）必须拒绝，而不是往里挪
// ---------------------------------------------------------------------------
void TestResolveRoi() {
    Section("交付图像与请求的对照：放不下就整张不落地");

    const DeliveredImage img = Image(200, 120);
    CropResolution ok = ResolveWindowCrop(Roi(10, 20, 50, 40), img, NoClient());
    Check(ok.status == CropStatus::kCropped && ok.crop.x == 10u && ok.crop.y == 20u &&
              ok.crop.width == 50u && ok.crop.height == 40u,
          "落在图像之内：按请求原样裁下来");
    Check(!ok.hasCropScreen, "图像原点没核实过就不写 cropScreenRect（不猜一个）");

    CropResolution right = ResolveWindowCrop(Roi(160, 0, 50, 10), img, NoClient());
    Check(right.status == CropStatus::kOutOfRange && right.fail == CropFail::kRoiBeyondImage,
          "右边越界：拒绝，不往里挪到 150 宽");
    Check(right.requestRight == 210ull && right.requestBottom == 10ull,
          "诊断里给的是请求自己的右下边（越界也报真实数字）");
    Check(right.imageWidth == 200u && right.imageHeight == 120u,
          "诊断里同时报图像实际尺寸，调用方能对上号");

    Check(ResolveWindowCrop(Roi(0, 80, 10, 50), img, NoClient()).fail ==
              CropFail::kRoiBeyondImage,
          "下边越界：同一条判据");
    Check(ResolveWindowCrop(Roi(200, 0, 10, 10), img, NoClient()).status ==
              CropStatus::kOutOfRange,
          "起点就已经在图像外：拒绝");
    Check(ResolveWindowCrop(Roi(0, 0, 0, 10), img, NoClient()).status == CropStatus::kOutOfRange,
          "零宽（判据层自己也不放行，不依赖解析层挡过）");
    Check(ResolveWindowCrop(Roi(0, 0, 10, 0), img, NoClient()).status == CropStatus::kOutOfRange,
          "零高同上");

    // 目标在选定之后改了尺寸：同一份请求从"放得下"变成"放不下"，而且不会退回整窗交出。
    const DeliveredImage shrunk = Image(100, 50);
    Check(ResolveWindowCrop(Roi(10, 20, 50, 40), shrunk, NoClient()).status ==
              CropStatus::kOutOfRange,
          "窗口改小之后：这条请求落到图像之外，拒绝");
    Check(ResolveWindowCrop(Roi(10, 20, 50, 40), Image(120, 60), NoClient()).status ==
              CropStatus::kCropped,
          "只改小到仍然装得下时照旧裁得下来（判据认尺寸，不认窗口有没有被动过）");
    Check(ResolveWindowCrop(Roi(0, 0, 10, 10), Image(0, 0), NoClient()).fail ==
              CropFail::kEmptyImage,
          "交付的图像本身零尺寸：不作裁剪，也不裁出一个 0×0 的图");

    // 与取帧之前那一条必须自洽：同一条请求、同一个尺寸，两边结论不能打架。
    Check(RoiFitsTarget(Roi(160, 0, 50, 10), 200, 120) ==
              (ResolveWindowCrop(Roi(160, 0, 50, 10), img, NoClient()).status ==
               CropStatus::kCropped),
          "取帧前后的判据同一条算式（预检放行 <=> 取到帧后裁得下来）");
}

// ---------------------------------------------------------------------------
// 3) 图像原点的核实：桌面裁切那条报 capturedRect、窗口内容那条比尺寸
// ---------------------------------------------------------------------------
void TestDescribeImage() {
    Section("这块图像对应屏幕上哪一块：判得出来才承认");

    // 桌面裁切那几条（bitblt / duplication）自己报了实际截到的那一块
    const DeliveredImage viaSampled =
        DescribeDeliveredImage(200, 120, Rect(0, 0, 1, 1), true, Rect(640, 29, 840, 149), true);
    Check(viaSampled.hasScreenRect && viaSampled.screenRect.left == 640 &&
              viaSampled.screenRect.top == 29,
          "通道报的实际取样矩形尺寸相符：那块就是图像的屏幕位置");

    // 窗口内容那几条：此刻量到的可见矩形尺寸与交付尺寸完全相同才承认
    const DeliveredImage viaMeasured =
        DescribeDeliveredImage(200, 120, Rect(100, 50, 300, 170), true, RECT{}, false);
    Check(viaMeasured.hasScreenRect && viaMeasured.screenRect.left == 100,
          "可见矩形与交付尺寸相同：承认它");

    // 尺寸不同 = 窗口在这中间改了大小，或者这条通道把透明边框一起交了（printwindow 那条退路）。
    // 这时不承认，宁可少写一个字段。
    const DeliveredImage mismatched =
        DescribeDeliveredImage(200, 120, Rect(100, 50, 301, 170), true, RECT{}, false);
    Check(!mismatched.hasScreenRect, "可见矩形宽 1 像素之差：不承认，也不写猜出来的屏幕坐标");
    const DeliveredImage biggerFrame =
        DescribeDeliveredImage(214, 134, Rect(100, 50, 300, 170), true, RECT{}, false);
    Check(!biggerFrame.hasScreenRect && biggerFrame.width == 214u,
          "交付的图像比可见矩形大（带上了透明边框）：同样不承认");
    Check(!DescribeDeliveredImage(200, 120, RECT{}, false, RECT{}, false).hasScreenRect,
          "两问都没答案：hasScreenRect 为假，尺寸本身照旧交回");
    Check(!DescribeDeliveredImage(200, 120, Rect(100, 50, 100, 170), true, RECT{}, false)
               .hasScreenRect,
          "量到的矩形本身是零宽：不作为原点证据");
    const DeliveredImage preferSampled =
        DescribeDeliveredImage(200, 120, Rect(0, 0, 200, 120), true, Rect(5, 5, 205, 125), true);
    Check(preferSampled.hasScreenRect && preferSampled.screenRect.left == 5,
          "通道自己报的取样矩形相符时优先用它（事实排在推测之前）");
}

// ---------------------------------------------------------------------------
// 4) --client-area：需要两问都有答案；三种失败各有各的状态
// ---------------------------------------------------------------------------
void TestClientArea() {
    Section("--client-area：客户区在图像里的位置");

    const DeliveredImage img = ImageAt(400, 300, 100, 50);
    // 客户区从 (100,90) 起到 (500,350)：在这张 400×300、原点在 (100,50) 的图像里就是 (0,40) 起 400×260
    CropResolution ok = ResolveWindowCrop(ClientArea(), img, Client(100, 90, 500, 350));
    Check(ok.status == CropStatus::kCropped && ok.crop.x == 0u && ok.crop.y == 40u &&
              ok.crop.width == 400u && ok.crop.height == 260u,
          "客户区落在图像之内：按偏移算出图像坐标里的那一块");
    Check(ok.hasCropScreen && ok.cropScreen.left == 100 && ok.cropScreen.bottom == 350,
          "cropScreenRect 与客户区自己的屏幕矩形逐字相同（映射不另算一套）");

    // 边框：标题栏在图像里被裁掉之后，客户区起点必然不早于图像原点
    CropResolution above = ResolveWindowCrop(ClientArea(), img, Client(100, 20, 500, 300));
    Check(above.status == CropStatus::kOutOfRange && above.fail == CropFail::kClientBeyondImage,
          "客户区起点在图像原点之上（那条边根本没被交付）：拒绝，不夹到 0");

    CropResolution left = ResolveWindowCrop(ClientArea(), img, Client(90, 50, 400, 200));
    Check(left.fail == CropFail::kClientBeyondImage, "客户区左边在图像之外：同一条拒绝");

    CropResolution wide = ResolveWindowCrop(ClientArea(), img, Client(100, 60, 520, 200));
    Check(wide.fail == CropFail::kClientBeyondImage,
          "客户区右边越过图像宽度（窗口挂在屏幕外那一段没交付）：拒绝");

    CropResolution empty = ResolveWindowCrop(ClientArea(), img, Client(100, 90, 100, 350));
    Check(empty.status == CropStatus::kOutOfRange && empty.fail == CropFail::kEmptyClientArea,
          "问到了、但客户区是零宽：这是放不下，不是没答案");

    Check(ResolveWindowCrop(ClientArea(), img, NoClient()).fail == CropFail::kClientUnmeasurable,
          "客户区问不出来：capture.roi_unmeasurable 那一类，不混进放不下");
    const DeliveredImage noOrigin = Image(400, 300);
    Check(ResolveWindowCrop(ClientArea(), noOrigin, Client(100, 90, 500, 350)).fail ==
              CropFail::kImageUnmeasurable,
          "图像原点核实不出来：客户区在图里落在哪儿无从确定，同样归没答案");
    Check(!ResolveWindowCrop(ClientArea(), noOrigin, Client(100, 90, 500, 350)).hasCropScreen,
          "这时连 cropRect 都不该出现（压根没裁）");

    // 图像本身零尺寸时先判图像，不拿客户区那块矩形去减一个不存在的原点
    Check(ResolveWindowCrop(ClientArea(), Image(0, 0), Client(0, 0, 10, 10)).fail ==
              CropFail::kEmptyImage,
          "交付图像零尺寸：先按图像判，不猜客户区位置");

    // 负坐标那一侧：副屏可以在主屏左边/上边，屏幕坐标本身带负数不是错误
    const DeliveredImage negative = ImageAt(400, 300, -800, -200);
    CropResolution neg = ResolveWindowCrop(ClientArea(), negative, Client(-800, -160, -400, 100));
    Check(neg.status == CropStatus::kCropped && neg.crop.y == 40u && neg.hasCropScreen &&
              neg.cropScreen.left == -800,
          "负坐标的屏上照样算得出来（虚拟屏幕原点可以为负）");
}

// ---------------------------------------------------------------------------
// 5) --roi 的屏幕映射：能核实就写、核实不出来就整个键不出现
// ---------------------------------------------------------------------------
void TestRoiScreenMapping() {
    Section("--roi 的坐标映射");

    const DeliveredImage known = ImageAt(400, 300, 100, 50);
    CropResolution k = ResolveWindowCrop(Roi(10, 20, 50, 40), known, NoClient());
    Check(k.status == CropStatus::kCropped && k.hasCropScreen && k.cropScreen.left == 110 &&
              k.cropScreen.top == 70 && k.cropScreen.right == 160 && k.cropScreen.bottom == 110,
          "图像原点核实得出来：cropScreenRect = 原点 + 图像坐标偏移");
    // 图像原点 = cropScreenRect 左上角 − cropRect 左上角（文档里那条闭合关系）
    Check(k.cropScreen.left - static_cast<LONG>(k.crop.x) == known.screenRect.left,
          "映射是自反的：由 cropScreenRect 与 cropRect 能倒推出图像原点");

    CropResolution unknown = ResolveWindowCrop(Roi(10, 20, 50, 40), Image(400, 300), NoClient());
    Check(unknown.status == CropStatus::kCropped && !unknown.hasCropScreen,
          "图像原点核实不出来：裁剪照做（它只需要图像尺寸），但那一行整个不出现");

    // 屏幕坐标本身贴到 LONG 边缘时不许绕回：宁可少写一行
    const DeliveredImage edge =
        ImageAt(400, 300, std::numeric_limits<LONG>::max() - 5, 0);
    CropResolution edgeRes = ResolveWindowCrop(Roi(10, 20, 50, 40), edge, NoClient());
    Check(edgeRes.status == CropStatus::kCropped && !edgeRes.hasCropScreen,
          "加上偏移会超出 LONG 范围：不写 cropScreenRect，也不交一个绕回来的坐标");
}

// ---------------------------------------------------------------------------
// 6) 上限与 ASCII 标识：两处数字不许漂移，原因名不许改名
// ---------------------------------------------------------------------------
void TestLimitsAndNames() {
    Section("上限同源与稳定标识");

    // 解析层的 --roi 上限与帧的单边上限必须是同一个数（帮助、解析、--capabilities 三处读同一份）。
    Check(cli_limits::kRoiMaxValue == kMaxSide, "解析层的 --roi 上限就是文档承诺的那一个数");
    Check(cli_limits::kRoiMaxValue == kFrameMaxSide,
          "解析层的 --roi 上限与帧的单边上限同值（漂移时这条先红）");

    Check(std::strcmp(CropFailName(CropFail::kRoiBeyondImage), "roi_beyond_image") == 0,
          "原因名稳定：roi_beyond_image");
    Check(std::strcmp(CropFailName(CropFail::kClientUnmeasurable), "client_unmeasurable") == 0,
          "原因名稳定：client_unmeasurable");
    Check(std::strcmp(CropFailName(CropFail::kImageUnmeasurable), "image_unmeasurable") == 0,
          "原因名稳定：image_unmeasurable");
    Check(std::strcmp(CropFailName(CropFail::kEmptyClientArea), "empty_client_area") == 0,
          "原因名稳定：empty_client_area");
    Check(std::strcmp(CropFailName(CropFail::kEmptyImage), "empty_image") == 0,
          "原因名稳定：empty_image");
    Check(std::strcmp(CropFailName(CropFail::kClientBeyondImage), "client_beyond_image") == 0,
          "原因名稳定：client_beyond_image");

    // "没答案"的两条与"放不下"的几条要能一眼分开（Capture.cpp 按这两组各选一条 code）
    const CropFail unmeasurable[] = {CropFail::kClientUnmeasurable, CropFail::kImageUnmeasurable};
    const CropFail unwritable[] = {CropFail::kRoiBeyondImage,  CropFail::kEmptyImage,
                                   CropFail::kClientBeyondImage, CropFail::kEmptyClientArea};
    for (CropFail f : unmeasurable) {
        Check(std::strstr(CropFailName(f), "unmeasurable") != nullptr,
              "问不出来那一类的原因名都带 unmeasurable");
    }
    for (CropFail f : unwritable) {
        Check(std::strstr(CropFailName(f), "unmeasurable") == nullptr,
              "放不下那一类的原因名都不带 unmeasurable（两条码不会互相冒充）");
    }
}

}  // namespace

int main() {
    TestRoiArithmetic();
    TestResolveRoi();
    TestDescribeImage();
    TestClientArea();
    TestRoiScreenMapping();
    TestLimitsAndNames();

    std::printf("\ncrop-geometry: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
