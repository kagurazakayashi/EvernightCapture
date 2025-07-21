#pragma once
// 共享的 D3D11 设备。Windows.Graphics.Capture 与 DXGI 桌面复制都要求一个
// 支持 BGRA、且能取到 IDXGIDevice 的设备，两边走同一套创建策略。

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace ecapture {

// 默认适配器上的设备（WGC 用）：硬件优先、退 WARP。
// 这条 WARP 兜底只对 WGC 成立 —— 它取的是 DWM 已经合成好的窗口面，与设备挂在哪块卡上无关，
// 所以软件光栅器也能交出真像素。
Microsoft::WRL::ComPtr<ID3D11Device> CreateCaptureDevice(HRESULT* lastHr = nullptr);

// 在**指定适配器**上建设备，供桌面复制使用：IDXGIOutput1::DuplicateOutput 要求设备与该
// 输出来自同一块适配器，否则直接 DXGI_ERROR_INVALID_CALL。所以"先建设备、再找输出"这个顺序
// 在多显卡机器上是错的（默认适配器上可能根本没有那块屏），必须先定位输出、再在它所属适配器上
// 建设备。这里**不做 WARP 兜底**：软件光栅器不拥有任何物理输出，拿它去复制只会得到一张永远
// 没有 present 记录的空帧 —— 那比当场失败更坏，因为调用方看到的是一张尺寸正确的图。
// 失败时把真实的 HRESULT 写进 *lastHr 交回，由调用方说清楚是哪块适配器、哪块输出。
Microsoft::WRL::ComPtr<ID3D11Device> CreateDeviceOnAdapter(IDXGIAdapter* adapter, HRESULT* lastHr);

}  // namespace ecapture
