// WGC 动态尺寸与帧池生命周期的离线判据（由 tests\wgc.ps1 运行 build\ecapture-wgc-tests.exe）。
//
// 测的是 src/WgcGeometry.cpp 那两个纯函数本身，而不是在测试里另抄一份算法：
// "这一帧的有效内容尺寸落在纹理之内 / 之外""采集项长过了帧池""重建还剩不剩预算"这几件事
// 在真机上要么很难稳定造（要窗口恰好在取帧那一瞬被缩放），要么根本拿不到阴性对照，
// 所以把判据搬进纯算术这一层逐条注入。真机那层（tests\wgc.ps1 的第二层）用可控测试窗口
// 连续缩放，核对交付出来的 PNG 尺寸确实跟着窗口走、边缘不是没定义的杂色。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <cstdio>
#include <cstdint>

#include "../src/WgcGeometry.h"

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

// 单边资源上限：与生产代码里传给 DecideWgcFrame 的 kFrameMaxSide 同一条线（16384）。这里独立
// 写死这份数字是刻意的——判据要能发现"实现偷偷改了上限而没人复核"，而不是跟着实现走。
constexpr uint32_t kMax = 16384u;

// 造一份几何输入：默认给一个"内容 = 纹理 = 帧池 = 采集项"的正常局面，调用方按需要改单个字段。
WgcGeometry Geo(uint32_t poolW, uint32_t poolH, uint32_t texW, uint32_t texH, uint32_t cW,
                uint32_t cH, uint32_t itemW, uint32_t itemH) {
    WgcGeometry g;
    g.poolWidth = poolW;
    g.poolHeight = poolH;
    g.textureWidth = texW;
    g.textureHeight = texH;
    g.contentWidth = cW;
    g.contentHeight = cH;
    g.itemWidth = itemW;
    g.itemHeight = itemH;
    return g;
}

// ---------------------------------------------------------------------------
// 每帧的几何判据：ContentSize 与纹理、帧池、采集项尺寸的关系决定下一步。
// ---------------------------------------------------------------------------
void TestDecide() {
    Section("每帧内容尺寸与纹理尺寸的对照");

    // 1) 内容 == 纹理 == 帧池 == 采集项：正常一帧，按内容尺寸整块复制。
    {
        const WgcFrameDecision d = DecideWgcFrame(Geo(640, 480, 640, 480, 640, 480, 640, 480), kMax);
        Check(d.action == WgcFrameAction::kCopy, "内容等于纹理：判为复制");
        Check(d.copyWidth == 640 && d.copyHeight == 480, "复制尺寸就是内容尺寸");
        Check(d.copyWidth <= 640 && d.copyHeight <= 480, "复制矩形落在纹理之内（读不到未定义边缘）");
    }

    // 2) 内容 < 纹理（窗口缩小、纹理停在帧池那份较大尺寸）：只复制左上角那块有效矩形。
    {
        const WgcFrameDecision d = DecideWgcFrame(Geo(640, 480, 640, 480, 300, 200, 300, 200), kMax);
        Check(d.action == WgcFrameAction::kCopy, "内容小于纹理：判为复制");
        Check(d.copyWidth == 300 && d.copyHeight == 200, "只复制内容那么大，不含纹理多出来的边缘");
        Check(d.copyWidth < 640 && d.copyHeight < 480, "复制尺寸严格小于纹理（旧实现会整张复制）");
    }

    // 3) 内容 > 纹理（采集项没超出帧池，但这一帧自相矛盾）：判不合法，绝不按内容尺寸去读。
    {
        const WgcFrameDecision d = DecideWgcFrame(Geo(640, 480, 640, 480, 700, 480, 640, 480), kMax);
        Check(d.action == WgcFrameAction::kInvalid, "内容比纹理宽：判不合法（照它复制会读越界）");
    }
    {
        const WgcFrameDecision d = DecideWgcFrame(Geo(640, 480, 640, 480, 640, 520, 640, 480), kMax);
        Check(d.action == WgcFrameAction::kInvalid, "内容比纹理高：判不合法");
    }

    // 4) 采集项长到帧池之外（窗口被放大）：这一帧不完整，判重建帧池，新尺寸取 max(采集项,内容)。
    {
        const WgcFrameDecision d = DecideWgcFrame(Geo(640, 480, 640, 480, 640, 480, 800, 600), kMax);
        Check(d.action == WgcFrameAction::kResize, "采集项超出帧池：判重建（不交这张不完整的帧）");
        Check(d.newWidth == 800 && d.newHeight == 600, "重建尺寸按更大的采集项来");
        Check(d.newWidth > 640 && d.newHeight > 480, "重建确实把帧池变大（对旧池有进展）");
    }
    // 内容也比池大时取两者较大者当新池，别只按内容或只按采集项。
    {
        const WgcFrameDecision d = DecideWgcFrame(Geo(640, 480, 640, 480, 900, 700, 800, 600), kMax);
        Check(d.action == WgcFrameAction::kResize && d.newWidth == 900 && d.newHeight == 700,
              "重建尺寸取采集项与内容里的较大者");
    }

    // 5) 采集项为 0 或内容为 0：目标已经没了，既不复制也不重建。
    {
        Check(DecideWgcFrame(Geo(640, 480, 640, 480, 640, 480, 0, 0), kMax).action ==
                  WgcFrameAction::kGone,
              "采集项尺寸为 0：判目标消失");
        Check(DecideWgcFrame(Geo(640, 480, 640, 480, 0, 480, 640, 480), kMax).action ==
                  WgcFrameAction::kGone,
              "内容尺寸为 0：判目标消失");
    }

    // 6) 要重建、但重建后的尺寸超出单边上限：不是"改大点再取"能救的，判不合法。
    {
        const WgcGeometry g = Geo(640, 480, 640, 480, 640, 480, kMax + 1, 480);
        Check(DecideWgcFrame(g, kMax).action == WgcFrameAction::kInvalid,
              "重建尺寸超单边上限：判不合法，不拿装不下的数字去 Recreate");
    }

    // 7) 任何判为复制的决定，复制矩形都必须在纹理之内 —— 这条是"不读未定义边缘"的硬约束，
    //    拿一批形状扫一遍，确认没有一条会给出越界的矩形。
    {
        const uint32_t texW = 512, texH = 256;
        const uint32_t cand[] = {0u, 1u, 255u, 512u, 513u, 1000u};
        bool safe = true;
        for (uint32_t cw : cand) {
            for (uint32_t ch : cand) {
                const WgcFrameDecision d = DecideWgcFrame(Geo(512, 256, texW, texH, cw, ch, 512, 256), kMax);
                if (d.action != WgcFrameAction::kCopy) continue;
                if (d.copyWidth == 0 || d.copyHeight == 0) safe = false;
                if (d.copyWidth > texW || d.copyHeight > texH) safe = false;
            }
        }
        Check(safe, "所有判为复制的决定，复制矩形都在纹理之内且非零");
    }
}

// ---------------------------------------------------------------------------
// 判出要重建之后，这一步做不做得成：预算优先，其次次数上限。
// ---------------------------------------------------------------------------
void TestRecreate() {
    Section("重建帧池的预算与次数闸门");

    Check(DecideWgcRecreate(/*budgetSpent=*/false, /*attempts=*/0, kMaxWgcRecreates) ==
              WgcRecreateStep::kRecreate,
          "有预算、没到上限：允许重建");
    Check(DecideWgcRecreate(true, 0, kMaxWgcRecreates) == WgcRecreateStep::kNoBudget,
          "预算用尽：不重建（宁可按超时报告，也不交一张不完整的帧）");
    Check(DecideWgcRecreate(true, kMaxWgcRecreates - 1, kMaxWgcRecreates) ==
              WgcRecreateStep::kNoBudget,
          "预算与次数同时踩线时先认预算（不默认还能再来一次）");
    Check(DecideWgcRecreate(false, kMaxWgcRecreates, kMaxWgcRecreates) ==
              WgcRecreateStep::kTooManyTries,
          "重建次数到上限仍追不上：判放弃，不无限重建");
    Check(DecideWgcRecreate(false, kMaxWgcRecreates - 1, kMaxWgcRecreates) ==
              WgcRecreateStep::kRecreate,
          "最后一次重建名额仍然放行（上限之前都还能追）");
}

}  // namespace

int main() {
    TestDecide();
    TestRecreate();

    std::printf("\nwgc-geometry: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
