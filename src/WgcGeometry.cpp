#include "WgcGeometry.h"

namespace ecapture {

WgcFrameDecision DecideWgcFrame(const WgcGeometry& g, uint32_t maxSide) {
    WgcFrameDecision d;

    // 内容或采集项尺寸为 0：窗口刚关掉的典型表现（句柄还有效，画面已经没了）。
    // 这时既不复制也不重建 —— 交回来的那一帧根本没有面积可谈。
    if (g.contentWidth == 0 || g.contentHeight == 0 || g.itemWidth == 0 || g.itemHeight == 0) {
        d.action = WgcFrameAction::kGone;
        return d;
    }

    // 采集项长到帧池容量之外：帧池按当初那份尺寸给不出完整的帧，这一帧是"被帧池截到那么大"的
    // 结果。宁可重建帧池重取，也不能把这张不完整的帧按它自己的（偏小的）尺寸当成整窗画面交出去。
    if (g.itemWidth > g.poolWidth || g.itemHeight > g.poolHeight) {
        const uint32_t wantW = g.itemWidth > g.contentWidth ? g.itemWidth : g.contentWidth;
        const uint32_t wantH = g.itemHeight > g.contentHeight ? g.itemHeight : g.contentHeight;
        // 重建之前先照这条线拦一下：连这个尺寸都装不下，就不是"把帧池改大点再取"能解决的。
        if (wantW > maxSide || wantH > maxSide) {
            d.action = WgcFrameAction::kInvalid;
            return d;
        }
        d.action = WgcFrameAction::kResize;
        d.newWidth = wantW;
        d.newHeight = wantH;
        return d;
    }

    // 内容比承载它的纹理还大：驱动自相矛盾（有效区域比缓冲区还宽）。照这种数字去复制一定会
    // 读到纹理之外的内存，所以不作任何交付，直接判这一帧不合法。
    if (g.contentWidth > g.textureWidth || g.contentHeight > g.textureHeight) {
        d.action = WgcFrameAction::kInvalid;
        return d;
    }

    // 剩下的就是正常形状：内容落在纹理之内（相等，或窗口变小后纹理停在帧池那份较大的尺寸）。
    // 只复制左上角那块有效矩形，纹理多出来的边缘一个字节都不碰。
    d.action = WgcFrameAction::kCopy;
    d.copyWidth = g.contentWidth;
    d.copyHeight = g.contentHeight;
    return d;
}

WgcRecreateStep DecideWgcRecreate(bool budgetSpent, uint32_t attemptsDone, uint32_t maxAttempts) {
    // 预算优先：预算用尽时就算还有重建次数也不该再开工 —— 交回这一帧是"不完整却按完整宣称"，
    // 唯一正确的做法是停下来按超时报告，让调用方加大 --timeout-ms 或换有人在的时候再来。
    if (budgetSpent) return WgcRecreateStep::kNoBudget;
    if (attemptsDone >= maxAttempts) return WgcRecreateStep::kTooManyTries;
    return WgcRecreateStep::kRecreate;
}

}  // namespace ecapture
