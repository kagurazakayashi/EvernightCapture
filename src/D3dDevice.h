#pragma once
// 共享的 D3D11 设备。Windows.Graphics.Capture 与 DXGI 桌面复制都要求一个
// 支持 BGRA、且能取到 IDXGIDevice 的设备，两边走同一套创建策略。

#include <d3d11.h>
#include <wrl/client.h>

namespace ecapture {

// 先试硬件设备，再退 WARP（远程桌面 / 没有可用 GPU 的环境）；都失败返回空指针。
// lastHr 回收真实错误码：调用方拿它写诊断。硬件那条路的码才是环境问题的原因，所以
// 硬件失败时优先报硬件的；只有硬件成功、WARP 也试过时才报 WARP 的。
// 别用 E_FAIL 顶替——"设备被移除"与"不支持 BGRA"在 E_FAIL 下长得一模一样。
Microsoft::WRL::ComPtr<ID3D11Device> CreateCaptureDevice(HRESULT* lastHr = nullptr);

}  // namespace ecapture
