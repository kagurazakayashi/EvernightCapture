#pragma once
// BitBlt 屏幕 DC 取帧通道（--capture bitblt）：拷屏幕上该窗口那块矩形。

#include <cstdint>

#include "CaptureCommon.h"
#include "ScreenMatch.h"

namespace ecapture {

// 两个入口都必须带桌面凭证：这条通道只会 GrabScreenRect，取的是屏幕上那块矩形此刻的样子，
// 即使最后正好是窗口大小，也仍然可能含其它窗口的像素（被挡住就截到挡住它的那个）。
// 需要被遮挡的内容请用 wgc / dwm 的缩略图主路径 / printwindow。
bool CaptureWindowBitBlt(uint64_t hwnd, uint32_t timeoutMs, const DesktopPermit& permit,
                         CapturedFrame* out, Diagnostic* err);

// 整块屏幕本来就是"屏幕上当前的样子"，不需要 z 序判定，也不受窗口位置影响。
bool CaptureScreenBitBlt(const ScreenInfo& screen, uint32_t timeoutMs, const DesktopPermit& permit,
                         CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
