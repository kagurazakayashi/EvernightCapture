#pragma once
// WGC 每一帧的几何判据：这一帧交回来的"实际内容尺寸"（frame.ContentSize）与"纹理尺寸"
//（frame.Surface 那张纹理的宽高）对不对得上，对不上就有两条完全不同的下一步：
//   * 内容落在纹理之内（窗口缩小、纹理还停在帧池那个较大的尺寸）—— 只能复制左上角那块有效
//     矩形，纹理剩下的边缘是没定义的内容，整张复制等于把它当画面交出去；
//   * 采集项已经长到帧池容量之外（窗口变大）—— 这一帧顶多是"帧池那么大"，本身就是不完整的，
//     必须把帧池重建到新的尺寸再取一张，绝不交一张不完整却按完整尺寸宣称的图。
// 这里全是纯算术：不碰 GPU、不碰窗口、不弹框，所以能被离线判据逐条注入（判据在
// tests\wgc_state.cpp），包括"内容比纹理大""窗口超出帧池"这些真机上很难稳定复现的形状。

#include <cstdint>

namespace ecapture {

// 一次 WGC 取帧的几何输入，全部取自已经手的数据：帧池当初的尺寸（= 池能给出的最大尺寸）、
// 这一帧交回来的纹理尺寸、frame.ContentSize 报的内容尺寸，以及取到帧之后重新量一次的采集项
// 尺寸（目标可能在"建池"与"取帧"这两步之间被拖动改变大小，判据要按当下的尺寸，不是按建池那份）。
struct WgcGeometry {
    uint32_t poolWidth = 0;
    uint32_t poolHeight = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    uint32_t contentWidth = 0;
    uint32_t contentHeight = 0;
    uint32_t itemWidth = 0;
    uint32_t itemHeight = 0;
};

// 一次取帧最多愿意把帧池重建几次：窗口在被截图的这一刻正好连续改变大小是有的，但一直追着
// 一个不停变大的窗口重建帧池就是无限循环了。这个上限与"重建超时"那条判据共用。
inline constexpr uint32_t kMaxWgcRecreates = 4;

enum class WgcFrameAction {
    kCopy,     // 纹理左上角 contentWidth×contentHeight 是有效内容：只复制这一块
    kResize,   // 采集项已经长到帧池之外：这一帧不完整，重建帧池到 newSize 后再取一张
    kGone,     // 内容尺寸或采集项尺寸为 0：目标已经关了 / 正在销毁
    kInvalid,  // 内容比承载它的纹理还大，或超出单边资源上限：不作任何交付
};

struct WgcFrameDecision {
    WgcFrameAction action = WgcFrameAction::kInvalid;
    uint32_t copyWidth = 0;    // kCopy：从纹理左上角复制这块有效矩形
    uint32_t copyHeight = 0;
    uint32_t newWidth = 0;     // kResize：建议的新帧池尺寸（其余情形为 0）
    uint32_t newHeight = 0;
};

// 每帧的几何判据。maxSide 与 ImageOps / CaptureCommon 的 kFrameMaxSide 同一条线：重建帧池
// 之前先按这条线拦下来，不拿一个本来就装不下的尺寸去 Recreate。
WgcFrameDecision DecideWgcFrame(const WgcGeometry& g, uint32_t maxSide);

// 判出"需要重建帧池"之后，这一步到底做不做得成：把"还剩多少预算"与"已经试了几次"折成一条
// 决定，好让重建超时这条路（真机上要靠窗口恰好在取帧中途被放大才造得出）能被离线逐条判。
enum class WgcRecreateStep {
    kRecreate,     // 还有预算、还没到次数上限：把帧池重建到 newSize 后重取
    kNoBudget,     // 预算已经用尽：不再重建，按帧超时报告（不交这一帧的不完整图）
    kTooManyTries, // 重建次数已到上限仍追不上目标尺寸：按不合法报告，不无限重建
};

WgcRecreateStep DecideWgcRecreate(bool budgetSpent, uint32_t attemptsDone, uint32_t maxAttempts);

}  // namespace ecapture
