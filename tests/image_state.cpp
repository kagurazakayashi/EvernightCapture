// 图像帧校验与质量判断的离线判据（由 tests\image.ps1 运行 build\ecapture-image-tests.exe）。
//
// 为什么在这一层测：这一批判据全是"一帧像素的内存形状说不说得通"与"整幅到底是不是一个颜色"，
// 两者都要**手工摆出特定的像素排布**才判得出来 —— 竖条纹、棋盘、只在一列或一处变化的图，
// 用真机截图造不出来（真机那一层的判据是"单色窗口要留一条质量提示、多色窗口不许留"，
// 写在 tests\image.ps1）。
// 而且这里测的是生产函数本身（src/ImageOps.cpp、src/CaptureCommon.cpp 那几份源文件），
// 不是在测试文件里另抄一份算法当判据 —— 抄一份的话，实现改坏了而判据还绿，正是这次要修的错。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../src/CaptureCommon.h"
#include "../src/CliOptions.h"
#include "../src/ImageOps.h"
#include "../src/Lang.h"

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

std::vector<uint8_t> Px(int b, int g, int r, int a = 255) {
    return std::vector<uint8_t>{static_cast<uint8_t>(b), static_cast<uint8_t>(g),
                                static_cast<uint8_t>(r), static_cast<uint8_t>(a)};
}

// 紧凑帧：行距 = 宽 * 4，像素全 0
CapturedFrame FrameOf(uint32_t width, uint32_t height) {
    CapturedFrame f;
    f.width = width;
    f.height = height;
    f.stride = width * 4u;
    f.pixels.assign(static_cast<size_t>(f.stride) * static_cast<size_t>(height), 0);
    return f;
}

// 带行末填充的帧：画面部分先清零，填充那几字节写成 0xEE —— 用来检查实现有没有
// 把对齐填充当成画面内容（旧实现的采样是按字节走的，正好会踩到）
CapturedFrame FrameWithPadding(uint32_t width, uint32_t height, uint32_t pad) {
    CapturedFrame f;
    f.width = width;
    f.height = height;
    f.stride = width * 4u + pad;
    f.pixels.assign(static_cast<size_t>(f.stride) * static_cast<size_t>(height), 0);
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t off = width * 4u; off < f.stride; ++off) {
            f.pixels[static_cast<size_t>(row) * f.stride + off] = 0xEE;
        }
    }
    return f;
}

void PutPixel(CapturedFrame* f, uint32_t x, uint32_t y, const std::vector<uint8_t>& bgra) {
    const size_t off = static_cast<size_t>(y) * f->stride + static_cast<size_t>(x) * 4u;
    for (int i = 0; i < 4; ++i) f->pixels[off + i] = bgra[i];
}

void FillAll(CapturedFrame* f, const std::vector<uint8_t>& bgra) {
    for (uint32_t y = 0; y < f->height; ++y) {
        for (uint32_t x = 0; x < f->width; ++x) PutPixel(f, x, y, bgra);
    }
}

// ---------------------------------------------------------------------------
// 整帧单色判定：定义 = 每个像素的 B/G/R/A 四个字节都等于左上角那个参考像素；
// 行末的对齐填充不参与。旧实现按 y+=3、x+=13 抽样并与第一行同偏移比，下面前四条
// 都是它当时会判错（或根本没看过）的形状。
// ---------------------------------------------------------------------------
void TestUniformity() {
    Section("整帧单色判定（严格逐像素，不采样）");

    FrameColor c{};

    // 1) 4×4 竖条纹：每行第 2 个像素是蓝的，其余黑。每行都与第一行一样 => 旧判据说"单色"
    {
        CapturedFrame f = FrameOf(4, 4);
        for (uint32_t y = 0; y < 4; ++y) PutPixel(&f, 1, y, Px(255, 0, 0));
        Check(!FrameIsUniform(f, &c), "4x4 竖条纹（每行第 2 像素蓝）不是单色");
    }

    // 2) 单行多色：只有一行时它是与自己的第一行比 => 旧判据说"单色"
    {
        CapturedFrame f = FrameOf(4, 1);
        PutPixel(&f, 1, 0, Px(0, 255, 0));
        PutPixel(&f, 3, 0, Px(0, 0, 255));
        Check(!FrameIsUniform(f, &c), "单行多色不是单色");
    }

    // 3) 横条纹：每行一个颜色（这一条旧实现也判得对，留着当回归）
    {
        CapturedFrame f = FrameOf(4, 3);
        for (uint32_t y = 0; y < 3; ++y) {
            for (uint32_t x = 0; x < 4; ++x) PutPixel(&f, x, y, Px(y == 0 ? 255 : 0, y == 1 ? 255 : 0, 0));
        }
        Check(!FrameIsUniform(f, &c), "横条纹不是单色");
    }

    // 4) 棋盘
    {
        CapturedFrame f = FrameOf(4, 4);
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                PutPixel(&f, x, y, ((x + y) % 2) ? Px(0, 0, 0) : Px(255, 255, 255));
            }
        }
        Check(!FrameIsUniform(f, &c), "棋盘不是单色");
    }

    // 5) 只在旧实现没看过的位置变化：4 像素宽的行走完 x+=13 只会取到字节 0..3，
    //    而 y 只走 0 与 3 两行 —— 第 1 行、以及第 0 行第 4 个像素它都没碰过
    {
        CapturedFrame a = FrameOf(4, 4);
        FillAll(&a, Px(9, 9, 9));
        PutPixel(&a, 3, 0, Px(9, 9, 8));   // 同一行、采样点之外的字节
        Check(!FrameIsUniform(a, &c), "只改第一行未被采样的那个像素也判得出不是单色");

        CapturedFrame b = FrameOf(4, 4);
        FillAll(&b, Px(9, 9, 9));
        PutPixel(&b, 1, 1, Px(9, 9, 8));   // 采样跳过的第 1 行
        Check(!FrameIsUniform(b, &c), "只改采样跳过的那一行也判得出不是单色");
    }

    // 6) RGB 相同、只有 alpha 不同：alpha 参与比较，所以不是单色
    {
        CapturedFrame f = FrameOf(2, 2);
        FillAll(&f, Px(10, 20, 30, 255));
        PutPixel(&f, 1, 0, Px(10, 20, 30, 0));
        Check(!FrameIsUniform(f, &c), "只有 alpha 不同也不算单色");
    }

    // 7) 真单色：给出那个颜色本身，且行末 0xEE 填充不算内容
    {
        CapturedFrame f = FrameWithPadding(5, 5, 8);
        FillAll(&f, Px(12, 200, 77));
        Check(FrameIsUniform(f, &c), "整帧同色（带行末填充）判为单色");
        Check(c.b == 12 && c.g == 200 && c.r == 77 && c.a == 255,
              "单色判据给出的是那个颜色本身（含 alpha）");
    }

    // 8) 全透明的单色与不透明的单色是两件事（alpha 就是画面的一部分）
    {
        CapturedFrame f = FrameOf(3, 3);
        FillAll(&f, Px(0, 0, 0, 0));
        FrameColor tc{};
        Check(FrameIsUniform(f, &tc), "整帧全透明也算单色（它就是同一个颜色）");
        Check(tc.a == 0, "alpha 为 0 的那个单色照实报出来");
    }

    // 9) 形状说不通的帧不作任何断言
    {
        CapturedFrame f = FrameOf(2, 2);
        f.height = 9;   // 缓冲区只够两行，形状已经坏了
        Check(!FrameIsUniform(f, &c), "形状不合法的帧不作单色断言");
        Check(!FrameIsUniform(FrameOf(0, 0), &c), "空图不作单色断言");
    }

    // 10) out 允许为空（调用方只想知道是不是单色）
    {
        CapturedFrame f = FrameOf(2, 2);
        FillAll(&f, Px(1, 2, 3));
        Check(FrameIsUniform(f, nullptr), "只问是不是单色时可以不给颜色放的地方");
    }
}

// ---------------------------------------------------------------------------
// 形状检查
// ---------------------------------------------------------------------------
void TestShape() {
    Section("帧内存形状与资源上限");

    const CapturedFrame good = FrameOf(3, 2);
    Check(InspectFrameShape(good) == FrameShape::kOk, "紧凑的 3x2 帧形状合格");
    Check(InspectFrameShape(FrameOf(0, 0)) == FrameShape::kEmpty, "空图（0x0）判 empty");

    CapturedFrame shortBuf = FrameOf(4, 4);
    shortBuf.pixels.resize(4u * 4u * 4u - 1u);
    Check(InspectFrameShape(shortBuf) == FrameShape::kBufferShort, "缓冲区少一个字节判 buffer_short");

    CapturedFrame tiny = FrameOf(4, 4);
    tiny.stride = 4u * 4u - 4u;   // 行距装不下一行像素
    Check(InspectFrameShape(tiny) == FrameShape::kStrideTooSmall, "过小 stride 判 stride_too_small");

    CapturedFrame tooWide = FrameOf(4, 4);
    tooWide.stride = 4u * 4u * 2u + 4u;   // 比两倍行长还宽，不可能是对齐填充
    Check(InspectFrameShape(tooWide) == FrameShape::kStrideTooLarge, "过大 stride 判 stride_too_large");

    CapturedFrame padded = FrameWithPadding(4, 4, 4);
    Check(InspectFrameShape(padded) == FrameShape::kOk, "带行末填充（stride = 行长 + 4）形状合格");

    CapturedFrame tail = FrameOf(4, 4);
    tail.pixels.resize(4u * 4u * 4u + 64u, 0xEE);   // 尾巴上多一截别人留的字节
    Check(InspectFrameShape(tail) == FrameShape::kOk, "缓冲区比需要的大也合格（多出来的不是画面）");

    CapturedFrame edge = FrameOf(kFrameMaxSide, 1);
    Check(InspectFrameShape(edge) == FrameShape::kOk, "单边正好等于上限（16384）合格");

    CapturedFrame over = FrameOf(kFrameMaxSide + 1u, 1);
    Check(InspectFrameShape(over) == FrameShape::kSideTooLarge, "单边超过上限判 side_too_large");

    // 判序很重要：宽 0xFFFFFFFF 若先按 32 位乘 4 会绕回 4 个字节，看起来就"合"格了。
    // 所以形状检查先比边长，乘法一律在 64 位里做。
    CapturedFrame wrap = FrameOf(1, 1);
    wrap.width = 0xFFFFFFFFu;
    wrap.stride = static_cast<uint32_t>(wrap.width) * 4u;   // = 4，绕回来了
    Check(InspectFrameShape(wrap) == FrameShape::kSideTooLarge, "边长乘法绕回的帧仍按超限拒收");

    // 边长与行距都各自合法，但 stride*height 顶到整帧上限之外。
    // 这里故意不分配缓冲区：判据必须发生在分配之前，测试也不能先分配 2 GiB。
    CapturedFrame huge;
    huge.width = kFrameMaxSide;
    huge.height = kFrameMaxSide;
    huge.stride = kFrameMaxSide * 4u * 2u;
    Check(InspectFrameShape(huge) == FrameShape::kTooManyBytes,
          "16384x16384 且行距两倍行长 -> 2 GiB，判 too_many_bytes");

    // 诊断：code / stage / backend 是机器看的，必须齐；文字不许漏成 "?key"
    Diagnostic err{};
    Check(!FrameShapeOk(shortBuf, L"dwm", stages::kCapture, &err), "FrameShapeOk 对坏帧返回 false");
    Check(err.code == codes::kFrameInvalid, "形状诊断的 code 是 capture.frame_invalid");
    Check(err.backend == L"dwm" && err.stage == stages::kCapture, "形状诊断带着通道与阶段");
    Check(err.value == L"dwm", "形状诊断的 value 是被判的那条通道");
    Check(!err.message.empty() && err.message.find(L'?') == std::wstring::npos,
          "形状诊断有文案且没有未定义的 key");
    Check(err.hint.find(L"16384") != std::wstring::npos, "上限的数字写在 hint 里");
    Check(err.win32 == 0 && err.hresult.empty(), "形状不合格不是系统调用失败，不编造错误码");

    Diagnostic ok{};
    Check(FrameShapeOk(good, L"wgc", stages::kCapture, &ok), "好帧过形状检查");
    Check(ok.code.empty(), "好帧不写诊断");

    // 编码那一步的通道名未知：backend 为空就是整个键不出现，不是空字符串
    Diagnostic enc{};
    Check(!FrameShapeOk(shortBuf, nullptr, stages::kEncode, &enc), "编码阶段的形状检查也判坏帧");
    Check(enc.stage == stages::kEncode && enc.backend.empty(), "编码阶段不带通道名");
}

// ---------------------------------------------------------------------------
// 裁剪
// ---------------------------------------------------------------------------
void TestCrop() {
    Section("裁剪：越界、坏形状与溢出都不改动原帧");

    CapturedFrame f = FrameOf(4, 4);
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) PutPixel(&f, x, y, Px(static_cast<int>(x), static_cast<int>(y), 0));
    }
    const std::vector<uint8_t> before = f.pixels;

    Check(CropFrame(&f, 1, 1, 2, 2), "裁一块 2x2 成功");
    Check(f.width == 2 && f.height == 2 && f.stride == 8, "裁完的宽高与行距是 2x2 / 8");
    bool cropOk = f.pixels.size() == 16;
    for (uint32_t y = 0; y < 2 && cropOk; ++y) {
        for (uint32_t x = 0; x < 2 && cropOk; ++x) {
            const size_t off = static_cast<size_t>(y) * f.stride + static_cast<size_t>(x) * 4u;
            if (f.pixels[off] != static_cast<uint8_t>(x + 1) ||
                f.pixels[off + 1] != static_cast<uint8_t>(y + 1)) {
                cropOk = false;
            }
        }
    }
    Check(cropOk, "裁下来的内容就是原来那一块（起点 1,1）");

    CapturedFrame corner = FrameOf(4, 4);
    corner.pixels = before;
    Check(CropFrame(&corner, 2, 2, 2, 2), "裁到右下角贴着边界是合法的");

    CapturedFrame h = FrameOf(4, 4);
    h.pixels = before;
    const std::vector<uint8_t> keep = h.pixels;
    Check(!CropFrame(&h, 3, 0, 2, 1), "右边越界的裁剪被拒");
    Check(!CropFrame(&h, 0, 3, 1, 2), "下边越界的裁剪被拒");
    Check(!CropFrame(&h, 0xFFFFFFFEu, 0, 4, 1), "x + width 在 32 位里会绕回的裁剪被拒");
    Check(!CropFrame(&h, 0, 0xFFFFFFFEu, 1, 4), "y + height 在 32 位里会绕回的裁剪被拒");
    Check(!CropFrame(&h, 0, 0, 0, 1), "宽 0 的裁剪被拒");
    Check(!CropFrame(&h, 0, 0, 1, 0), "高 0 的裁剪被拒");
    Check(!CropFrame(&h, 0, 0, kFrameMaxSide + 1u, 1), "裁出比单边上限还宽的图被拒");
    Check(h.pixels == keep && h.width == 4 && h.height == 4 && h.stride == 16,
          "被拒的裁剪没有改动原帧的任何一个字节");

    CapturedFrame broken = FrameOf(4, 4);
    broken.pixels = before;
    broken.height = 40;   // 缓冲区只够 4 行
    Check(!CropFrame(&broken, 0, 0, 2, 2), "形状坏了的帧拒绝裁剪");
    Check(broken.pixels == before, "形状坏了的帧裁剪失败后仍是原样");

    // 带行末填充：裁出来的是紧凑行，填充不参与也不算越界
    CapturedFrame p = FrameWithPadding(4, 4, 8);
    FillAll(&p, Px(1, 2, 3));
    Check(CropFrame(&p, 1, 0, 2, 4), "带填充的帧能裁");
    Check(p.stride == 8 && p.pixels.size() == 32, "裁完行距变成紧凑行长，尾部填充没跟过来");
    bool tight = true;
    for (size_t i = 0; i < p.pixels.size(); ++i) {
        if (p.pixels[i] != Px(1, 2, 3)[i % 4]) tight = false;
    }
    Check(tight, "裁出来的每一行都是那两个像素，没有填充字节");

    Check(!CropFrame(nullptr, 0, 0, 1, 1), "没给帧指针就不动任何东西");
    CapturedFrame small = FrameOf(2, 2);
    Check(CropFrame(&small, 0, 0, 1, 1), "正常的小裁剪照样能成");
}

// ---------------------------------------------------------------------------
// 等比缩小（--scale）：默认不放大、比例只减不增、向下取整、只有最近邻一种内插
// ---------------------------------------------------------------------------
// 为什么在这一层测：比例怎么取、取整取到哪、映射落在哪一个源像素上，全都能用**手工摆出的
// 特定像素排布**逐点判（大到 16384x16384 的输入连分配都不用，ResolveScale 只做算术）。
// 这里测的是生产函数本体（src/ImageOps.cpp），不是另抄一份算法当判据。
void TestScale() {
    Section("等比缩小：天花板怎么取、放大怎么被拒、内插与映射怎么闭合");

    // 没写过 --scale：这一层一个字节都不动（与这条选项存在之前逐字节相同）
    ScaleRequest none{};
    ScaleResolution r = ResolveScale(none, 100, 50);
    Check(r.status == ScaleStatus::kNoScale, "没给请求时状态是 kNoScale（连尺寸都不算）");
    Check(r.width == 0 && r.height == 0, "没给请求时不报尺寸（不是缩到 0）");

    // 写了却一条天花板都没给：解析期拦得住，这一层也不把它当成"缩到 0"
    ScaleRequest empty{};
    empty.given = true;
    r = ResolveScale(empty, 100, 50);
    Check(r.status == ScaleStatus::kUnchanged && r.width == 100 && r.height == 50,
          "空请求按原样交回（不是缩到 0）");

    // 源形状说不通：与帧形状那一关同一组边界，不猜
    Check(ResolveScale(empty, 0, 50).status == ScaleStatus::kRejected, "源宽 0 被拒");
    Check(ResolveScale(empty, 100, 0).status == ScaleStatus::kRejected, "源高 0 被拒");
    Check(ResolveScale(empty, kFrameMaxSide + 1u, 10).status == ScaleStatus::kRejected,
          "源宽超过单边上限被拒");
    Check(ResolveScale(empty, 10, kFrameMaxSide + 1u).status == ScaleStatus::kRejected,
          "源高超过单边上限被拒");

    // 默认不放大：天花板比图大（或正好相等）时原样交回
    ScaleRequest wide{};
    wide.given = true;
    wide.hasMaxWidth = true;
    wide.maxWidth = 1920;
    r = ResolveScale(wide, 100, 50);
    Check(r.status == ScaleStatus::kUnchanged && r.width == 100 && r.height == 50,
          "图本来就在天花板之内：原样交回（不放大）");
    Check(ResolveScale(wide, 1920, 1080).status == ScaleStatus::kUnchanged,
          "正好等于天花板也算放得下");

    // 横图：限宽，高跟着同一个比例走
    ScaleRequest mw{};
    mw.given = true;
    mw.hasMaxWidth = true;
    mw.maxWidth = 200;
    r = ResolveScale(mw, 400, 200);
    Check(r.status == ScaleStatus::kScaled && r.width == 200 && r.height == 100,
          "横图 400x200 限宽 200 -> 200x100");

    // 竖图：限高，宽跟着同一个比例走（不是两边各自独立缩）
    ScaleRequest mh{};
    mh.given = true;
    mh.hasMaxHeight = true;
    mh.maxHeight = 200;
    r = ResolveScale(mh, 200, 400);
    Check(r.status == ScaleStatus::kScaled && r.width == 100 && r.height == 200,
          "竖图 200x400 限高 200 -> 100x200");
    mh.maxHeight = 100;
    r = ResolveScale(mh, 400, 200);
    Check(r.status == ScaleStatus::kScaled && r.width == 200 && r.height == 100,
          "单边限制只限高时，宽按同一个比例走");

    // 三条一起给：取最紧的那一条（这里限高 100 最紧）
    ScaleRequest all{};
    all.given = true;
    all.hasMaxWidth = true;
    all.maxWidth = 1000;
    all.hasMaxHeight = true;
    all.maxHeight = 100;
    all.hasMaxPixels = true;
    all.maxPixels = 10000000u;
    r = ResolveScale(all, 400, 200);
    Check(r.status == ScaleStatus::kScaled && r.width == 200 && r.height == 100,
          "三条天花板一起给：取最紧的那一条（限高 100）");

    // 比例舍入：向下取整
    ScaleRequest round1{};
    round1.given = true;
    round1.hasMaxWidth = true;
    round1.maxWidth = 333;
    r = ResolveScale(round1, 1000, 333);
    Check(r.status == ScaleStatus::kScaled && r.width == 333 && r.height == 110,
          "比例舍入向下取整：1000x333 限宽 333 -> 333x110");

    // 极小图：再缩也要留 1 像素（空图不是一张图）
    ScaleRequest tiny{};
    tiny.given = true;
    tiny.hasMaxWidth = true;
    tiny.maxWidth = 1;
    r = ResolveScale(tiny, 3, 1);
    Check(r.status == ScaleStatus::kScaled && r.width == 1 && r.height == 1,
          "极小图 3x1 限宽 1 -> 1x1（另一边被夹到至少 1 像素）");

    // 超大输入：面积天花板按开方取比例，16384x16384 只做算术、不分配
    ScaleRequest mp{};
    mp.given = true;
    mp.hasMaxPixels = true;
    mp.maxPixels = 2073600u;
    r = ResolveScale(mp, 16384, 16384);
    Check(r.status == ScaleStatus::kScaled, "超大输入 16384x16384 能被面积天花板缩小");
    Check(r.width == 1440 && r.height == 1440, "16384x16384 限 2073600 像素 -> 1440x1440");
    Check(static_cast<uint64_t>(r.width) * r.height <= mp.maxPixels,
          "缩完之后的总像素数真的落在天花板之内（不靠浮点运气）");
    mp.maxPixels = 2;
    r = ResolveScale(mp, 5, 5);
    Check(r.status == ScaleStatus::kScaled && r.width == 1 && r.height == 1,
          "5x5 限 2 像素 -> 1x1（夹到至少 1 像素，且 1 不越过面积天花板）");

    // 三条天花板同时给时，整数核对之后仍同时落在三条之内
    ScaleRequest tight{};
    tight.given = true;
    tight.hasMaxWidth = true;
    tight.maxWidth = 7;
    tight.hasMaxHeight = true;
    tight.maxHeight = 7;
    tight.hasMaxPixels = true;
    tight.maxPixels = 41;
    r = ResolveScale(tight, 100, 100);
    Check(r.status == ScaleStatus::kScaled && r.width <= 7 && r.height <= 7 &&
              static_cast<uint64_t>(r.width) * r.height <= 41u,
          "三条天花板同时给：结果同时落在三条之内");

    // ScaleFrame：最近邻，交付像素 (x,y) 取源 (x*srcW/outW, y*srcH/outH)（整数除法）
    CapturedFrame f = FrameOf(4, 2);
    for (uint32_t y = 0; y < 2; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            PutPixel(&f, x, y, Px(static_cast<int>(x + 1), static_cast<int>(y + 1), 0));
        }
    }
    Check(ScaleFrame(&f, 2, 1), "4x2 缩到 2x1 成功");
    Check(f.width == 2 && f.height == 1 && f.stride == 8 && f.pixels.size() == 8,
          "缩完的宽高与行距是 2x1 / 8（紧凑行）");
    Check(f.pixels[0] == 1 && f.pixels[1] == 1 && f.pixels[4] == 3 && f.pixels[5] == 1,
          "最近邻取的是源 x = floor(0*4/2) = 0 与 floor(1*4/2) = 2 那两个像素");

    // 恒等尺寸：内容逐字节不变（"给了 --scale 但天花板不生效"不会被搬成另一张图）
    CapturedFrame id = FrameOf(3, 2);
    FillAll(&id, Px(9, 8, 7));
    const std::vector<uint8_t> idBefore = id.pixels;
    Check(ScaleFrame(&id, 3, 2), "同尺寸的 ScaleFrame 成功");
    Check(id.pixels == idBefore && id.width == 3 && id.height == 2, "同尺寸时内容逐字节不变");

    // 只缩不放：任何一边要放大都被拒，且不改动原帧
    CapturedFrame grow = FrameOf(4, 4);
    const std::vector<uint8_t> growBefore = grow.pixels;
    Check(!ScaleFrame(&grow, 5, 4), "要放大的请求被拒");
    Check(!ScaleFrame(&grow, 4, 5), "另一边要放大也被拒");
    Check(!ScaleFrame(&grow, 0, 4), "输出宽 0 被拒");
    Check(!ScaleFrame(&grow, 4, 0), "输出高 0 被拒");
    Check(!ScaleFrame(nullptr, 2, 2), "没给帧指针就不动任何东西");
    Check(grow.pixels == growBefore && grow.width == 4 && grow.height == 4,
          "被拒的调用没有改动原帧的任何一个字节");

    // 形状坏了的帧：拒绝缩放，且不改动
    CapturedFrame broken = FrameOf(4, 4);
    broken.pixels = growBefore;
    broken.height = 40;   // 缓冲区只够 4 行
    Check(!ScaleFrame(&broken, 2, 2), "形状坏了的帧拒绝缩放");
    Check(broken.pixels == growBefore, "形状坏了的帧失败后仍是原样");

    // 带行末填充：缩出来的是紧凑行，填充不参与
    CapturedFrame pad = FrameWithPadding(8, 8, 12);
    FillAll(&pad, Px(1, 2, 3));
    Check(ScaleFrame(&pad, 4, 4), "带填充的帧能缩");
    Check(pad.stride == 16 && pad.pixels.size() == 64, "缩完行距变成紧凑行长，尾部填充没跟过来");
    bool tightRows = true;
    for (size_t i = 0; i < pad.pixels.size(); ++i) {
        if (pad.pixels[i] != Px(1, 2, 3)[i % 4]) tightRows = false;
    }
    Check(tightRows, "缩出来的每一行都是那两个像素，没有填充字节");

    // 与 ROI 组合（顺序：有效帧 -> 裁剪 -> 缩放 -> 编码）：
    // 8x6 的源裁出 (2,1,4,2)，再限宽 2 缩成 2x1。
    // 交付 (0,0) 出自裁后 (0,0) = 源 (2,1)；(1,0) 出自裁后 (2,0) = 源 (4,1)。
    CapturedFrame combo = FrameOf(8, 6);
    for (uint32_t y = 0; y < 6; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            PutPixel(&combo, x, y, Px(static_cast<int>(x), static_cast<int>(y), 0x11));
        }
    }
    Check(CropFrame(&combo, 2, 1, 4, 2), "组合现场：先在 8x6 上裁 (2,1,4,2)");
    Check(combo.width == 4 && combo.height == 2, "裁完是 4x2（缩的是裁完的那一块）");
    ScaleRequest comboReq{};
    comboReq.given = true;
    comboReq.hasMaxWidth = true;
    comboReq.maxWidth = 2;
    const ScaleResolution cr = ResolveScale(comboReq, combo.width, combo.height);
    Check(cr.status == ScaleStatus::kScaled && cr.width == 2 && cr.height == 1,
          "裁完那一块 4x2 限宽 2 -> 2x1");
    Check(ScaleFrame(&combo, cr.width, cr.height), "裁完再缩成功");
    Check(combo.pixels[0] == 2 && combo.pixels[4] == 4,
          "缩完的 (0,0) 出自源 (2,1)、(1,0) 出自源 (4,1)：裁与缩的映射按顺序闭合");

    // 缩完仍是一帧紧凑、形状说得通的帧：后面那几路编码器（png / jpeg / bmp）不需要再补齐，
    // 也不会因为"缩过一道"而看到另一种形状。格式之间真正的差别（字节流）在 tests\scale.ps1 判。
    Check(InspectFrameShape(combo) == FrameShape::kOk, "缩完之后仍是一帧形状说得通的帧");
    Check(combo.stride == combo.width * 4u &&
              combo.pixels.size() == static_cast<size_t>(combo.stride) * combo.height,
          "缩完是紧凑行距，交给任何编码器都不需要再补齐");
}

// ---------------------------------------------------------------------------
// 行距补齐（编码接口只接受紧凑行）
// ---------------------------------------------------------------------------
void TestPackTight() {
    Section("行距补齐：只搬画面，不搬填充");

    CapturedFrame f = FrameWithPadding(3, 2, 12);
    PutPixel(&f, 0, 0, Px(1, 1, 1));
    PutPixel(&f, 2, 1, Px(2, 2, 2));
    std::vector<uint8_t> tight;
    Check(PackTight(f, &tight), "带填充的帧能重排");
    Check(tight.size() == 3u * 4u * 2u, "重排后的大小是紧凑行 x 高");
    Check(tight[0] == 1 && tight[1] == 1 && tight[2] == 1 && tight[3] == 255, "第一行第一个像素搬对了");
    const size_t lastOff = static_cast<size_t>(1) * 3u * 4u + static_cast<size_t>(2) * 4u;
    Check(tight[lastOff] == 2 && tight[lastOff + 1] == 2 && tight[lastOff + 2] == 2,
          "最后一行最后一个像素搬对了");
    bool noPad = true;
    for (const uint8_t v : tight) {
        if (v == 0xEE) noPad = false;
    }
    Check(noPad, "行末的填充字节一个都没进重排结果");

    std::vector<uint8_t> sink;
    CapturedFrame shortBuf = FrameOf(4, 4);
    shortBuf.pixels.resize(4u * 4u * 4u - 4u);
    Check(!PackTight(shortBuf, &sink), "缓冲区不够的帧拒绝重排（旧实现会读越界）");
    Check(sink.empty(), "拒绝重排时不交出半张图");
    Check(!PackTight(f, nullptr), "重排结果没给地方放就不做");
}

// ---------------------------------------------------------------------------
// 上限常量本身：这三处（GDI 建位图、辅助进程管道协议、帧形状判据）必须是同一条线，
// 否则"过得了检查的一帧"会在下一次分配时崩掉。
// ---------------------------------------------------------------------------
void TestLimits() {
    Section("资源上限：数值与别处那道线一致");

    Check(kFrameMaxSide == 16384u, "帧的单边上限就是 16384");
    Check(kFrameMaxBytes == 1024ull * 1024ull * 1024ull, "帧的整幅上限就是 1 GiB");
    // 上限之内最大的一帧（行距 = 行长）正好落在整幅上限上，不多不少：判据不会把自己卡死
    Check(static_cast<uint64_t>(kFrameMaxSide) * 4ull * kFrameMaxSide == kFrameMaxBytes,
          "16384x16384 的紧凑帧正好等于整幅上限");
    // GDI 那条 Dib::Create 用的就是同一个常数（源码里没有第二份 16384 的字面量），
    // 这里只能判"形状检查接受正好等于上限的边长"，真机分配留给 smoke / channels。
    Check(InspectFrameShape(FrameOf(kFrameMaxSide, 1)) == FrameShape::kOk,
          "边长取到上限时形状检查放行");
}

// ---------------------------------------------------------------------------
// 这一批新诊断与质量提示的文案 key：四种语言都要真取得到，占位符都要代得进去。
// check-lang.ps1 判的是四份表的 key 对齐，判不到**代码里写的那个名字对不对** ——
// 写错一个字母时 Lang 返回 "?cap.frame_invalidx"，只有在这里才拦得住。
// ---------------------------------------------------------------------------
void TestStrings() {
    Section("新增文案：四种语言都取得到，占位符都代得进去");

    const Language langs[] = {Language::kZhCn, Language::kZhTw, Language::kEn, Language::kJa};
    const wchar_t* keys[] = {
        L"cap.frame_invalid",     L"cap.frame_invalid_hint", L"cap.frame_format",
        L"cap.frame_format_hint", L"cap.gpu.staging",        L"cap.gpu.context",
        L"cap.gpu.copy_failed",   L"cap.gpu.map",            L"cap.select_bitmap",
        L"cap.crop_failed",       L"cap.crop_failed_hint",   L"cap.scale_apply_failed",
        L"cap.scale_apply_failed_hint",
        L"note.frame_uniform",    L"note.frame_uniform_hint",
    };
    auto narrow = [](const wchar_t* w) {
        std::string out;
        for (; w && *w; ++w) out.push_back(static_cast<char>(*w & 0xFFu));
        return out;
    };
    for (const Language lang : langs) {
        SetLanguage(lang);
        std::string tag;
        for (const wchar_t c : std::wstring(LanguageTag(lang))) {
            tag.push_back(static_cast<char>(c & 0xFFu));
        }
        for (const wchar_t* key : keys) {
            const std::wstring text = Msg(key);
            Check(!text.empty() && text.front() != L'?',
                  (tag + " 取得到 " + narrow(key)).c_str());
        }
        // 占位符全部代进去：形状那条 hint 有六个位置，格式与单色那条各一个
        Diagnostic err{};
        FrameShapeError(FrameShape::kBufferShort, FrameShapeInfo{4u, 4u, 16u, 60u}, L"wgc",
                        stages::kCapture, &err);
        Check(!err.message.empty() && err.message.front() != L'?' &&
                  err.message.find(L"%1") == std::wstring::npos,
              (tag + " 的形状诊断把状态名代进去了").c_str());
        Check(!err.hint.empty() && err.hint.find(L'%') == std::wstring::npos &&
                  err.hint.find(L"16384") != std::wstring::npos,
              (tag + " 的形状 hint 六个位置都代进去了，且写着那条上限").c_str());
        const std::wstring fmt = Msgf(L"cap.frame_format", static_cast<uint64_t>(90));
        Check(fmt.find(L'%') == std::wstring::npos && fmt.find(L"90") != std::wstring::npos,
              (tag + " 的像素格式诊断带得上格式编号").c_str());
        const std::wstring note = Msgf(L"note.frame_uniform", std::wstring(L"0xFF2E7D32"));
        Check(note.find(L'%') == std::wstring::npos &&
                  note.find(L"0xFF2E7D32") != std::wstring::npos,
              (tag + " 的单色提示带得上那个颜色").c_str());
    }
    SetLanguage(Language::kEn);
}

}  // namespace

int main() {
    // 判据只看 code / stage / 数字，文案跟着英语那份走（与 consent_state.cpp 同一约定）
    SetLanguage(Language::kEn);

    TestUniformity();
    TestShape();
    TestCrop();
    TestScale();
    TestPackTight();
    TestLimits();
    TestStrings();

    std::printf("\nimage-state: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
