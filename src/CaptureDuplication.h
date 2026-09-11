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
//
// 光标这一问在这条通道上**没有答案**：官方说明写明那一幅桌面图像里指针要么已经画在上面、要么由
// 显卡单独叠加，而本工具不取指针元数据也不修图像，所以交回的帧里有没有指针像素说不准。
// 判据与它带来的三种下场（include / exclude 做不到、default 报 unverified）只有一份，
// 写在 src/CursorControl.h 的那张按路径登记的表里，这里不另写一套。

#include <cstdint>

#include "CaptureCommon.h"
#include "ScreenMatch.h"

namespace ecapture {

// 窗口目标：定位那块输出、按旋转取出窗口矩形那一大块。
// hdr / dl：桌面纹理跟随那块输出的显示模式，HDR 时可能不是 B8G8R8A8。没写 --hdr（或写成 auto）
// 时这条路径照旧只认 B8G8R8A8（非它就报 cap.frame_format），并把 hdr 原样交给共用的拷回那一步
//（映射/拒绝的算术在那一侧，见 src/HdrColor.h 与 CopyTextureToFrame）。
// 但这条通道**不算兑现得了显式 tonemap / refuse**：它仍用 IDXGIOutput1::DuplicateOutput()，
// 采集之前不问那块屏此刻的色彩空间，也不选广色域格式，所以一张 8 位桌面帧说不出"原始内容是不是
// HDR"。显式要过那两种策略时，它在闸门与解析期都被判成不合格候选（登记表里那条
// kWideGamutUnverified），因此这里的 hdr 实际只会带着"没要求处理"那一份答案进来。
// 完整的广色域采集是独立后续任务，本轮不靠一句没核实的"支持 HDR"放行。
bool CaptureWindowDuplication(uint64_t hwnd, uint32_t timeoutMs, const DesktopPermit& permit,
                              const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
                              Diagnostic* err);

// 屏幕目标：按设备名（对不上时按完全相同的矩形）取该屏对应的那块 DXGI 输出。
// 交付尺寸必须等于该屏矩形；裁不全就是拓扑在确认之后变了，报 capture.monitor_changed
// 而不是悄悄换成另一块屏。hdr / dl 与窗口目标同义。
bool CaptureScreenDuplication(const ScreenInfo& screen, uint32_t timeoutMs,
                              const DesktopPermit& permit, const HdrRequest& hdr, const Deadline& dl,
                              CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
