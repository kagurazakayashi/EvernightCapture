#pragma once
// DXGI 桌面复制取帧通道（--capture duplication）：拿整张显示器的合成分，再按目标矩形裁剪。
//
// 这一条通道读的是**某一块输出**的合成分，所以有两件事必须说清楚：
//   * 显示器可以处于 0/90/180/270 度旋转。驱动交回的纹理未必是用户看到的那个朝向，本通道
//     先按 DupGeometry 的判据把朝向核清楚，再把目标矩形换算到纹理坐标上取那一块，
//     交付的图像坐标空间恒等于虚拟屏幕坐标（与人工确认矩形同一系）。
//   * 一台机器上有多块显卡时，屏可能挂在第二块适配器上，所以先枚举所有 adapter/output
//     定位目标，再到目标所属的适配器上建 D3D11 设备（DuplicateOutput 的参数要求）。
//
// 只能拿到"当前可见"的部分：被别的窗口挡住就截到挡住它的窗口，且窗口必须落在某块输出内。
// 跨屏的窗口只截与它重叠最多的那一块输出，丢掉的部分照实报（capturedRect / clipped +
// note.capture_clipped），不静默当成完整目标。整幅桌面帧按矩形裁仍然是桌面像素，所以要桌面凭证。

#include <cstdint>

#include "CaptureCommon.h"
#include "ScreenMatch.h"

namespace ecapture {

// 窗口目标：定位那块输出、按旋转取出窗口矩形那一大块
bool CaptureWindowDuplication(uint64_t hwnd, uint32_t timeoutMs, const DesktopPermit& permit,
                              CapturedFrame* out, Diagnostic* err);

// 屏幕目标：按设备名（对不上时按完全相同的矩形）取该屏对应的那块 DXGI 输出。
// 交付尺寸必须等于该屏矩形；裁不全就是拓扑在确认之后变了，报 capture.monitor_changed
// 而不是悄悄换成另一块屏。
bool CaptureScreenDuplication(const ScreenInfo& screen, uint32_t timeoutMs,
                              const DesktopPermit& permit, CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
