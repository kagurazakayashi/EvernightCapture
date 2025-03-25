#include "D3dDevice.h"

#include <dxgi.h>

namespace ecapture {

Microsoft::WRL::ComPtr<ID3D11Device> CreateCaptureDevice() {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    // 硬件设备必须真能拿到 IDXGIDevice，否则对采集通道没有意义（WARP 也满足）
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                    D3D11_SDK_VERSION, &device, &level, &context))) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(device.As(&dxgi))) return device;
        device.Reset();
    }
    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                    D3D11_SDK_VERSION, &device, &level, &context))) {
        return device;
    }
    return nullptr;
}

}  // namespace ecapture
