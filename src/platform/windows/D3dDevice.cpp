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

Microsoft::WRL::ComPtr<ID3D11Device> CreateDeviceOnAdapter(IDXGIAdapter* adapter, HRESULT* lastHr) {
    if (!adapter) {
        if (lastHr) *lastHr = E_POINTER;
        return nullptr;
    }
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    // 传了 adapter 就必须是 D3D_DRIVER_TYPE_UNKNOWN（其它取值会被参数检查拒绝）
    const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
                                         D3D11_SDK_VERSION, &device, &level, &context);
    if (lastHr) *lastHr = hr;
    if (FAILED(hr)) return nullptr;

    // 建成了却要不到 DXGI 接口，对复制这条路等于没用（DuplicateOutput 要的正是同一块适配器）
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    const HRESULT qhr = device.As(&dxgi);
    if (FAILED(qhr)) {
        if (lastHr) *lastHr = qhr;
        return nullptr;
    }
    return device;
}

}  // namespace ecapture
