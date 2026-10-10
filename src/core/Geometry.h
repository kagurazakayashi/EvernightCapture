#pragma once
// 跨平台核心：坐标模型。
//
// PhysRect 取代原先直接写在共用结构里的 RECT。它**不是**给 RECT 换个名字：字段宽度写死成
// int32_t（RECT 的 LONG 在别的平台上没有对应物），并且这一层自带"空矩形""包含""量不出来"
// 三条判据，Windows 与将来的 X11/Wayland 后端共用同一份算术，而不是各抄一遍再对答案。
//
// 坐标系约定（整条链路的根，改之前先对齐 README《窗口内部裁剪》那一节）：
//   * 单位是**物理像素**，没有任何按 DPI 缩放的步骤。进程侧负责让自己不被虚拟化
//     （Windows 走 per-monitor DPI v2，见 CaptureCommon.h 的 EnsureDpiAware）。
//   * 左上角含、右下角不含：宽 = right - left，高 = bottom - top。
//   * 每个矩形必须说清它落在哪个坐标空间里，不同空间的数不许直接相减：
//       - **虚拟屏幕坐标**：CapturedImage.rect / requestedRect / capturedRect、
//         ScreenInfo.bounds、DesktopPermit 批准的那一块、GateTarget.area。
//         Windows 上是虚拟屏幕（多屏拼接后的那一整张，原点不必是 0,0，可以出现负数）；
//         X11 上是根窗口的全局坐标；Wayland 的合成器坐标只在屏幕上成立，窗口没有全局坐标，
//         所以那一路要按"该屏本地坐标"登记，不许假装它和 Windows 同一套系。
//       - **图像像素坐标**：CropGeometry.h 的 ImageRect（原点 = 这张交付图像的左上角）。
//       - **纹理坐标**：只出现在 duplication 那条通道内部，不进出共用结构。
//   * 负数合法（虚拟屏幕坐标里左上方那块屏就是负的），所以"left 是负数"不是错误，
//     而 right <= left 才是空矩形。
//   * 相加一律在 64 位里判，不靠回绕：边界值本身落在 int32 之内，但 right - left 与
//     坐标相加都可能出界（见 RectWidth 那条）。

#include <cstdint>

namespace ecapture {

struct PhysRect {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

// 一个"量没量得出来"的矩形。
//
// 这一条是整个模型里最要紧的区分：零尺寸矩形是一**事实**（那扇窗口此刻确实没有客户区、
// 那块区域确实还没被映射），而"问不出来"是一次**没有答案**，两者对调用方是完全不同的下一步。
// 既有代码已经用同样的形状表达过这件事（WindowInfo.rectRead、DeliveredImage.hasScreenRect、
// CropGeometry 的 kUnmeasurable），这个结构只是把它写成一个类型，让新增接口不必各自再造一对
// bool + 值，也不许在问不出来的时候填一个"大概是零"的矩形蒙过去。
struct MaybePhysRect {
    bool known = false;
    PhysRect rect{};

    static MaybePhysRect Of(const PhysRect& r) { return MaybePhysRect{true, r}; }
    static MaybePhysRect Unknown() { return MaybePhysRect{}; }

    // known 为假时这一问返回 false —— 那不是"矩形是空的"，那是"没有答案"，
    // 所以判据不许把它当成空矩形放行，也不许当成非空矩形拦截。
    bool IsEmpty() const { return known && rect.right <= rect.left; }
};

// 空矩形：右下边不越过左上边（宽或高为 0 都算空）。
// 与原先写在 Consent.cpp 里的那一条逐字相同，只是搬到这一层，四个平台共用一份。
inline bool RectIsEmpty(const PhysRect& r) { return r.right <= r.left || r.bottom <= r.top; }

// outer 是否**完整包住** inner：左上、右下两条边都不许差一个像素。
// 零容差是有意的（DesktopPermit 批准的是屏幕上那一块，向外扩一格就是采了没人批准过的画面）。
// 这一条与原先写在 Consent.cpp 里的判据**逐字相同**：空矩形在这里不当成特例，
// 覆盖与否完全由四条边决定 —— 要拒绝空矩形请另外用 RectIsEmpty 明说（DesktopPermit::Covers
// 就是这么两条一起用的），否则搬一次家就把既有授权判据改松或改紧了。
inline bool RectContains(const PhysRect& outer, const PhysRect& inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

// 宽与高。空矩形与"右下边在左上边之前"一律回 0，不返回负数：
// 尺寸要进 uint32 的帧形状判据，负数在那里会被读成一个巨大的数。
// 差值在 64 位里算再收回来，所以坐标本身取极端值时也不会回绕成一个小正数。
inline int32_t RectWidth(const PhysRect& r) {
    const int64_t w = static_cast<int64_t>(r.right) - static_cast<int64_t>(r.left);
    if (w <= 0) return 0;
    return w > 0x7FFFFFFFll ? 0x7FFFFFFF : static_cast<int32_t>(w);
}

inline int32_t RectHeight(const PhysRect& r) {
    const int64_t h = static_cast<int64_t>(r.bottom) - static_cast<int64_t>(r.top);
    if (h <= 0) return 0;
    return h > 0x7FFFFFFFll ? 0x7FFFFFFF : static_cast<int32_t>(h);
}

// 两条矩形有没有公共部分（touching 边缘不算重叠，与"右下不含"那一条自洽）。
// 屏幕过滤那一步用的是这一条：跨屏窗口在两块屏上都能被找到。
// 判据与原先写在 WindowMatch.cpp 的 OnAnyScreen 里那一句**逐字相同**，刻意不加"空矩形"特例：
// 一条零宽度的窗口矩形落在某块屏之内时，旧行为算命中（那是既有的下注，改成不命中
// 会让同一条命令行忽然找不到窗口）。要拒绝空矩形请调用方自己先用 RectIsEmpty 明说。
inline bool RectOverlaps(const PhysRect& a, const PhysRect& b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}

}  // namespace ecapture
