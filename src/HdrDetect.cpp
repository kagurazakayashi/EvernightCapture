#include "HdrColor.h"

// 这一份单独成文件，是为了让只读能力判据（EnvReport）与各条离线判据目标不必为了"问一次显示
// HDR 状态"而被拖进 DXGI / 显卡库：真正会去问这条的只有 CaptureWgc.cpp（它本来就链 d3d/dxgi），
// 于是它编进主 ecapture 目标，离线目标只链纯算术那半（HdrColor.cpp）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ecapture {
namespace {

// 拿一个设备名（\\.\DISPLAY1 那种）问它此刻输出到桌面的 color space。任何一步问不成
// （没有 DXGI factory / 这块设备不在枚举里 / 那条 IDXGIOutput6 问不到）都返回 kUnknown，
// 不弹框、不写文件、也不改任何显示设置（只 GetDesc，绝不调 ChangeDisplaySettings / SetDisplayConfig）。
DisplayHdrState ProbeColorSpaceForDeviceName(const wchar_t* deviceName) {
    if (!deviceName || !deviceName[0]) return DisplayHdrState::kUnknown;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return DisplayHdrState::kUnknown;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            // 逐字符不区分大小写：Windows 自己就是这么看设备名的（与 --screens / ScreenIdentity 同源）。
            if (_wcsicmp(desc.DeviceName, deviceName) != 0) continue;
            ComPtr<IDXGIOutput6> out6;
            if (FAILED(output.As(&out6))) return DisplayHdrState::kUnknown;  // 问不到 v6 = 没答案
            DXGI_OUTPUT_DESC1 desc1{};
            if (FAILED(out6->GetDesc1(&desc1))) return DisplayHdrState::kUnknown;
            return DisplayHdrStateOfDxgiColorSpace(static_cast<uint32_t>(desc1.ColorSpace));
        }
    }
    return DisplayHdrState::kUnknown;
}

}  // namespace

DisplayHdrState ProbeDisplayHdrForMonitor(void* hmonitor) {
    if (!hmonitor) return DisplayHdrState::kUnknown;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(static_cast<HMONITOR>(hmonitor), &mi)) return DisplayHdrState::kUnknown;
    return ProbeColorSpaceForDeviceName(mi.szDevice);
}

DisplayHdrState ProbeDisplayHdrForHwnd(uint64_t hwndValue) {
    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    if (!hwnd) return DisplayHdrState::kUnknown;
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    return ProbeDisplayHdrForMonitor(mon);
}

}  // namespace ecapture
