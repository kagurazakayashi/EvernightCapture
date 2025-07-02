#pragma once
// BitBlt 屏幕 DC 取帧通道（--capture bitblt）：拷屏幕上该窗口那块矩形。

#include <cstdint>

#include "CaptureCommon.h"
#include "ScreenMatch.h"

namespace ecapture {

// 只拿得到屏幕上当前可见的部分：被别的窗口挡住就截到挡住它的窗口，
// 完全在屏幕外时直接报错。需要被遮挡的内容请用 wgc / dwm / printwindow。
bool CaptureWindowBitBlt(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err);

// 整块屏幕本来就是"屏幕上当前的样子"，不需要 z 序判定，也不受窗口位置影响。
bool CaptureScreenBitBlt(const ScreenInfo& screen, uint32_t timeoutMs, CapturedFrame* out,
                         Diagnostic* err);

}  // namespace ecapture
