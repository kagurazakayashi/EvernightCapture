#pragma once
// DXGI 桌面复制取帧通道（--capture duplication）：拿整张显示器的合成分，再按窗口矩形裁剪。

#include <cstdint>

#include "CaptureCommon.h"

namespace ecapture {

// 显示器合成后的画面，硬件加速内容正常；但只能拿到"当前可见"的部分——
// 被别的窗口挡住就截到挡住它的窗口，且窗口必须落在某个显示器内。
bool CaptureWindowDuplication(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
