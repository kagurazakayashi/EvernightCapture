#pragma once
// Windows.Graphics.Capture 取帧：一次调用抓一帧，转成 CPU 可读的 BGRA8。

#include <cstdint>

#include "CaptureCommon.h"

namespace ecapture {

// 抓取指定窗口。失败时填 *err（code 以 capture. 开头）并返回 false。
bool CaptureWindowWgc(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err);

// WinRT 套间只需初始化一次；内部自带幂等保护。
void EnsureWinrtInitialized();

}  // namespace ecapture
