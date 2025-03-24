#pragma once
// PrintWindow 取帧通道（--capture printwindow）：让窗口自己画到 DC。

#include <cstdint>

#include "CaptureCommon.h"

namespace ecapture {

// 先带 PW_RENDERFULLCONTENT 试（Win8.1+，能逼出部分硬件加速内容），
// 失败再退回普通 WM_PRINT。不接受自绘的窗口（游戏 / DirectComposition）会拿到空帧。
bool CaptureWindowPrintWindow(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
