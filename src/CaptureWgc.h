#pragma once
// Windows.Graphics.Capture 取帧：一次调用抓一帧，转成 CPU 可读的 BGRA8。

#include <cstdint>

#include "CaptureCommon.h"
#include "ScreenMatch.h"

namespace ecapture {

// 抓取指定窗口。失败时填 *err（code 以 capture. 开头）并返回 false。
// 这条路径的采集项是由 HWND 建出来的窗口自己那份合成面，屏幕上别的东西不会进图，
// 所以它属于窗口内容路径，不需要桌面凭证。
//
// cursor 是这一次的光标要求（--cursor）。wgc 是**唯一**有一条真能设进去、也能读回来核实的
// 光标开关的通道（IGraphicsCaptureSession2::IsCursorCaptureEnabled），所以这条参数是必需的：
// 明确要求过而开关问不到 / 设不下去 / 读回来不是那一件事时，这里在 StartCapture **之前**就交回
// capture.cursor_unverifiable，一个像素都不读 —— 通道手里没有"那就照默认交一张"的余地。
// 没写 --cursor 时（given=false）这里一个字节都不改，也不去读那个属性。
//
// hdr 是这一次的 HDR 处理要求（--hdr），dl 是这一次运行的剩余预算（守 tone mapping 那趟扫描）。
// 只有显式要过 HDR 处理时这条路径才会去只读地问一次那块屏是不是 HDR 模式，并按需要把帧池
// 建成 FP16 scRGB（而不是默认的 B8G8R8A8），再在拷回 CPU 之后映射成 8 位 sRGB。
// 没写 --hdr（given=false）或 --hdr auto 时这里连那次显示状态都不问，与这条选项存在之前逐字节相同。
bool CaptureWindowWgc(uint64_t hwnd, uint32_t timeoutMs, const CursorRequest& cursor,
                      const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
                      Diagnostic* err);

// 抓取整块屏幕（--monitor 的屏幕目标）：同一个会话机制，只是采集项由 HMONITOR 建出来。
// 整屏 WGC 取的就是那块屏上此刻的全部画面，和其它桌面路径同级 —— 必须带桌面凭证。
// 光标那一条开关与窗口路径共用同一条会话接口，所以整屏也照样设得进去（要不要设照上面同一条规矩）。
// hdr / dl 那两条与窗口路径同义（同一条会话接口，屏就是那块 HMONITOR）。
bool CaptureScreenWgc(const ScreenInfo& screen, uint32_t timeoutMs, const CursorRequest& cursor,
                      const HdrRequest& hdr, const Deadline& dl, const DesktopPermit& permit,
                      CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
