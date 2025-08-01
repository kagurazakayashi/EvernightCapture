#pragma once
// 图像处理：对已取回的帧做像素级操作。裁剪、行重排、单色判定与"这帧的内存形状还不还说得通"
// 这件事集中在这里，各通道只负责把像素搞进 CapturedFrame，形状统一由本文件保证。
// 这里的函数都是纯计算：不分配超出上限的内存、不读屏幕、不弹框，所以能被离线判据直接调用
//（判据在 tests\image_state.cpp，测的就是下面这几个生产函数本身）。

#include <cstdint>
#include <vector>

#include "CaptureCommon.h"

namespace ecapture {

// 帧的内存形状检查：每个入口（裁剪、行重排、单色判定、编码、GPU/管道交回来的帧）在动手之前
// 都要先过这一道。检查全在 64 位里算，不依赖"分配失败"来发现坏形状。
enum class FrameShape : uint32_t {
    kOk = 0,
    kEmpty,           // 宽或高为 0
    kSideTooLarge,    // 某一边超过 kFrameMaxSide
    kStrideTooSmall,  // 行距装不下一行像素（< width*4）
    kStrideTooLarge,  // 行距超过行长两倍（不可能是对齐填充）
    kTooManyBytes,    // 行距 × 高超过 kFrameMaxBytes
    kBufferShort,     // pixels 装不下 行距 × 高（最后一行会被读越界）
};

// 一帧像素的内存形状。它与"字节实际存在哪"解耦：GPU 拷回 CPU 那一步要先按**打算分配**的
// 形状判一遍（那时帧还不存在），判过才动手分配。
// bytesPerPixel 默认 4（BGRA8 那一套，也是既有全部调用点的语义：行距落在 width*4 与 2*width*4 之间）。
// 只有会带回广色域帧的那一步（FP16 scRGB 一条像素 8 字节）会传别的值，于是同一道"分配之前判形状"
// 的判据对 8 字节的行也成立，而不必另写第二套。默认值让既有调用点逐字节不变。
struct FrameShapeInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint64_t size = 0;  // 可用的字节数（缓冲区大小，或准备分配的大小）
    uint32_t bytesPerPixel = 4;
};

// 只做判断，不产生文字。宽高与缓冲区自不自洽、乘法溢不溢出，都在这一个函数里判
//（旧实现没有这一道：CropFrame / PackTight 直接按调用方给的形状算偏移，帧本身坏了就读越界）。
FrameShape CheckFrameShape(const FrameShapeInfo& info);

// 已经存在的帧：把 pixels.size() 当作可用字节数走同一条判据
inline FrameShape InspectFrameShape(const CapturedFrame& frame) {
    return CheckFrameShape(FrameShapeInfo{frame.width, frame.height, frame.stride,
                                          static_cast<uint64_t>(frame.pixels.size())});
}

// 把检查结果换成一条诊断（capture.frame_invalid + 上限与实际形状的数字）。
void FrameShapeError(FrameShape shape, const FrameShapeInfo& info, const wchar_t* channel,
                     const wchar_t* stage, Diagnostic* err);

// 形状合格返回 true 且不碰 *err；不合格时填好诊断再返回 false。
bool FrameShapeOk(const CapturedFrame& frame, const wchar_t* channel, const wchar_t* stage,
                  Diagnostic* err);

// 检查结果里那个 ASCII 状态名，写进诊断的 message 与测试判据用（不随 --lang 变）
const char* FrameShapeName(FrameShape shape);

// 一个 BGRA 像素。alpha 一起比较（见 FrameIsUniform 的规矩）。
struct FrameColor {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 0;
};

// 整帧是不是真的只有一个颜色：**完整遍历每个像素**，与左上角那个参考像素逐字节比，
// B/G/R/A 四个通道都算，行末的对齐填充不参与。
//   * 返回 true  => 严格结论：这一帧的每个像素都等于 *out 那个颜色。
//   * 返回 false => 帧里有不止一个颜色，**或**这帧的形状根本说不通（不作任何断言）。
// 旧实现按 y+=3、x+=13 抽样，还把每行同偏移的字节与第一行比：竖条纹、单行多色的图会被判成
// "单色"，而 x+=13 落在 BGRA 的像素中间（一像素 4 字节）比的是错位字节。抽样只能得到"疑似"，
// 所以这里不留采样版本 —— 需要严格结论就完整比，代价是一趟线性扫描。
bool FrameIsUniform(const CapturedFrame& frame, FrameColor* out);

// 就地裁剪；帧形状不合格、范围越界或算出的偏移会溢出时不改动并返回 false
bool CropFrame(CapturedFrame* frame, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

// 读出 srcRect（左上含、右下不含，坐标是这张帧自己的像素坐标）那一块，顺时针转 angle 度之后
// 作为新的一帧交给 *out（*out 原来持有的内存会被换掉；失败时 *out 保持不动）。
// angle 只接受 0 / 90 / 180 / 270，其余值不写 *out 并返回 false —— 不猜该按哪种处理。
// 这一条与"先把整幅帧旋转到用户看到的朝向、再按桌面坐标裁剪"逐像素等价，但只搬目标那么大的一份：
// 90/270 时 srcRect 本来就是交换过宽高的那一块，转回来正好等于交付尺寸，不会二次交换。
// 输出行距恒等于 width*4（源帧的行距填充不参与，也不复制到结果里）。
// 源帧形状不合法、srcRect 越界、相加绕回、结果超过单边与整帧上限都返回 false。
bool RotateCropFrame(const CapturedFrame& frame, const RECT& srcRect, uint32_t angle,
                     CapturedFrame* out);

// 行距补齐（stride > width*4）重排成紧凑行，编码接口只接受紧凑行。
// 形状不合格时不写 *out 并返回 false。
bool PackTight(const CapturedFrame& frame, std::vector<uint8_t>* out);

}  // namespace ecapture
