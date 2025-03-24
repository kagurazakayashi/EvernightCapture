#pragma once
// DWM 缩略图取帧通道（--capture dwm）。

#include <cstdint>

#include "CaptureWgc.h"

namespace ecapture {

// 用 DwmRegisterThumbnail 把源窗口的 DWM 缓存面画到一个临时目标窗口上，再把那块
// 屏幕矩形读回来。DWM 缓存里是被遮挡窗口自己的内容，所以源窗口在后台也能截。
bool CaptureWindowDwmThumbnail(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
