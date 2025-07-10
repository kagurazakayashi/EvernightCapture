#include "D3dDevice.h"

#include <dxgi.h>

namespace ecapture {

Microsoft::WRL::ComPtr<ID3D11Device> CreateCaptureDevice(HRESULT* lastHr) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    // 硬件设备必须真能拿到 IDXGIDevice，否则对采集通道没有意义（WARP 也满足）
    const HRESULT hwHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                           nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (SUCCEEDED(hwHr)) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        const HRESULT qhr = device.As(&dxgi);
        if (SUCCEEDED(qhr)) return device;
        if (lastHr) *lastHr = qhr;   // 硬件设备有了却不是 DXGI 设备，这条信息比"再试 WARP"更有用
        device.Reset();
    } else if (lastHr) {
        *lastHr = hwHr;
    }
    const HRESULT warpHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr,
                                            0, D3D11_SDK_VERSION, &device, &level, &context);
    if (SUCCEEDED(warpHr)) {
        if (lastHr) *lastHr = warpHr;   // 成功也要写清：调用方据此知道"硬件那条路失败过、现在是 WARP"
        return device;
    }
    if (lastHr) *lastHr = warpHr;
    return nullptr;
}

}  // namespace ecapture
