#pragma once
// Windows.Graphics.Capture 取帧：一次调用抓一帧，转成 CPU 可读的 BGRA8。

#include <cstdint>

#include "CaptureCommon.h"
#include "ScreenMatch.h"

namespace ecapture {

// 抓取指定窗口。失败时填 *err（code 以 capture. 开头）并返回 false。
// 这条路径的采集项是由 HWND 建出来的窗口自己那份合成面，屏幕上别的东西不会进图，
// 所以它属于窗口内容路径，不需要桌面凭证。
bool CaptureWindowWgc(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err);

// 抓取整块屏幕（--monitor 的屏幕目标）：同一个会话机制，只是采集项由 HMONITOR 建出来。
// 整屏 WGC 取的就是那块屏上此刻的全部画面，和其它桌面路径同级 —— 必须带桌面凭证。
bool CaptureScreenWgc(const ScreenInfo& screen, uint32_t timeoutMs, const DesktopPermit& permit,
                      CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
