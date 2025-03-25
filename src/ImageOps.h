#pragma once
// 图像处理：对已取回的帧做像素级操作。裁剪、行重排、纯色判定集中在这里，
// 各通道只负责把像素搞进 CapturedFrame，形状统一由本文件保证。

#include <cstdint>
#include <vector>

#include "CaptureCommon.h"

namespace ecapture {

// 就地裁剪；范围越界时不改动并返回 false
bool CropFrame(CapturedFrame* frame, uint32_t x, uint32_t y, uint32_t width, uint32_t height);

// 整幅画面只有一个颜色：说明内容根本没被合成出来，不能算取到了画面
bool FrameIsFlat(const CapturedFrame& frame);

// 行距补齐（stride > width*4）重排成紧凑行，编码接口只接受紧凑行
std::vector<uint8_t> PackTight(const CapturedFrame& frame);

}  // namespace ecapture
