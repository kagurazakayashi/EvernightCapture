#pragma once
// 共享的 D3D11 设备。Windows.Graphics.Capture 与 DXGI 桌面复制都要求一个
// 支持 BGRA、且能取到 IDXGIDevice 的设备，两边走同一套创建策略。

#include <d3d11.h>
#include <wrl/client.h>

namespace ecapture {

// 先试硬件设备，再退 WARP（远程桌面 / 没有可用 GPU 的环境）；都失败返回空指针。
Microsoft::WRL::ComPtr<ID3D11Device> CreateCaptureDevice();

}  // namespace ecapture
