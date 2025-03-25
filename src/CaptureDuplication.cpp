#include "CaptureDuplication.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "CaptureCommon.h"
#include "D3dDevice.h"
#include "ImageOps.h"

using Microsoft::WRL::ComPtr;

namespace ecapture {
namespace {

constexpr const wchar_t* kChannel = L"duplication";
constexpr UINT kAcquireSliceMs = 100;

// 取到的桌面帧必须 ReleaseFrame，否则显示器的这份资源一直被占着
class AcquiredFrame {
public:
    AcquiredFrame(IDXGIOutputDuplication* dup, const DXGI_OUTDUPL_FRAME_INFO& info,
                  IDXGIResource* resource)
        : dup_(dup), info_(info), resource_(resource) {}
    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;
    ~AcquiredFrame() {
        if (dup_) dup_->ReleaseFrame();
    }
    const DXGI_OUTDUPL_FRAME_INFO& info() const { return info_; }
    IDXGIResource* resource() const { return resource_.Get(); }

private:
    ComPtr<IDXGIOutputDuplication> dup_;
    DXGI_OUTDUPL_FRAME_INFO info_{};
    ComPtr<IDXGIResource> resource_;
};

RECT IntersectRect2(const RECT& a, const RECT& b) {
    RECT r{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
           std::min(a.bottom, b.bottom)};
    if (r.right <= r.left || r.bottom <= r.top) return RECT{};
    return r;
}

long long AreaOf(const RECT& r) {
    return static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
}

// 选出与窗口重叠面积最大的那个输出：多显示器时桌面复制是按输出取的
struct PickedOutput {
    ComPtr<IDXGIOutput1> output1;
    DXGI_OUTPUT_DESC desc{};
};

bool PickOutput(IDXGIDevice* device, const RECT& window, PickedOutput* out, Diagnostic* err) {
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, L"取显卡适配器失败", L"HRESULT " + HResultText(hr));
        return false;
    }
    long long best = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(i, &output))) break;
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) continue;
        const long long overlap = AreaOf(IntersectRect2(window, desc.DesktopCoordinates));
        if (overlap <= best) continue;
        ComPtr<IDXGIOutput1> as1;
        if (FAILED(output.As(&as1))) continue;
        best = overlap;
        out->output1 = as1;
        out->desc = desc;
    }
    if (!out->output1 || best <= 0) {
        CaptureError(err, kChannel, L"窗口不在任何显示器范围内",
                     L"该通道取的是显示器合成分，窗口必须至少有一部分在屏幕上");
        return false;
    }
    return true;
}

std::wstring DuplicationHint(HRESULT hr) {
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        return L"DXGI_ERROR_UNSUPPORTED：远程桌面 / 部分虚拟机与基本显示驱动不支持桌面复制";
    }
    if (hr == E_ACCESSDENIED) {
        return L"E_ACCESSDENIED：内容受保护，或当前处于安全桌面（UAC）";
    }
    if (hr == DXGI_ERROR_INVALID_CALL) {
        return L"DXGI_ERROR_INVALID_CALL：设备与输出不匹配，或该输出已被别的进程复制";
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return L"超时都没有新帧：桌面完全静止时可能不发帧，让目标窗口动一下再试";
    }
    return std::wstring();
}

// 把 GPU 上的桌面帧拷进 CPU 可读的 staging 纹理，再按行搬进帧
bool CopyDesktopToCpu(ID3D11Device* device, IDXGIResource* resource, CapturedFrame* out,
                      Diagnostic* err) {
    // 桌面复制给的资源本身就是 D3D11 纹理，直接 QueryInterface 到 ID3D11Texture2D 即可
    ComPtr<ID3D11Texture2D> desktop;
    HRESULT hr = resource->QueryInterface(IID_PPV_ARGS(&desktop));
    if (FAILED(hr)) {
        CaptureError(err, kChannel, L"桌面帧转纹理失败", L"HRESULT " + HResultText(hr));
        return false;
    }
    D3D11_TEXTURE2D_DESC src{};
    desktop->GetDesc(&src);
    if (src.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        CaptureError(err, kChannel, L"桌面帧不是 BGRA8 格式",
                     L"HDR / 10 位显示模式下取不到普通 BGRA 帧，请在系统设置里关掉 HDR");
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = src;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
        CaptureError(err, kChannel, L"创建 staging 纹理失败", std::wstring());
        return false;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    // CopyResource 返回 void，失败只能靠设备状态与画面内容反证（见调用方的单色判定）
    context->CopyResource(staging.Get(), desktop.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, L"锁定桌面帧失败", L"HRESULT " + HResultText(hr));
        return false;
    }
    out->width = src.Width;
    out->height = src.Height;
    out->stride = mapped.RowPitch;
    out->pixels.resize(static_cast<size_t>(mapped.RowPitch) * src.Height);
    const auto* from = static_cast<const uint8_t*>(mapped.pData);
    auto* to = out->pixels.data();
    for (uint32_t row = 0; row < src.Height; ++row) {
        std::memcpy(to + static_cast<size_t>(row) * mapped.RowPitch,
                    from + static_cast<size_t>(row) * mapped.RowPitch,
                    static_cast<size_t>(src.Width) * 4u);
    }
    context->Unmap(staging.Get(), 0);
    return true;
}

}  // namespace

bool CaptureWindowDuplication(uint64_t hwndValue, uint32_t timeoutMs, CapturedFrame* out,
                              Diagnostic* err) {
    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const RECT ext = WindowScreenRect(hwnd);
    if (ext.right <= ext.left || ext.bottom <= ext.top) {
        CaptureError(err, kChannel, L"窗口矩形为空", L"窗口可能被最小化或已关闭");
        return false;
    }

    ComPtr<ID3D11Device> device = CreateCaptureDevice();
    if (!device) {
        CaptureError(err, kChannel, L"创建 D3D11 设备失败", std::wstring());
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device.As(&dxgiDevice))) {
        CaptureError(err, kChannel, L"设备不支持 DXGI", std::wstring());
        return false;
    }

    PickedOutput picked;
    if (!PickOutput(dxgiDevice.Get(), ext, &picked, err)) return false;

    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = picked.output1->DuplicateOutput(device.Get(), &dup);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, L"DuplicateOutput 失败 (HRESULT " + HResultText(hr) + L")",
                     DuplicationHint(hr));
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const DWORD deadline = GetTickCount() + (timeoutMs ? timeoutMs : 2000u);
    // 刚建好复制时拿到的往往是"只有时间信息、没有新画面"的空帧，纹理里可能还是空内容。
    // 先给真实 present 一点宽限期，超期就用当前帧（静态桌面上它仍是有效的当前画面）。
    constexpr UINT kRealFrameGraceMs = 300;
    const DWORD graceEnd = GetTickCount() + kRealFrameGraceMs;
    bool have = false;
    while (!have) {
        const int32_t remaining = static_cast<int32_t>(deadline - GetTickCount());
        if (remaining <= 0) {
            CaptureError(err, kChannel, L"超时未取到桌面帧（" + std::to_wstring(timeoutMs) + L" ms）",
                         DuplicationHint(DXGI_ERROR_WAIT_TIMEOUT));
            return false;
        }
        hr = dup->AcquireNextFrame(std::min(kAcquireSliceMs, static_cast<UINT>(remaining)), &info,
                                   &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT || hr == S_FALSE) continue;
        if (FAILED(hr)) {
            CaptureError(err, kChannel, L"AcquireNextFrame 失败 (HRESULT " + HResultText(hr) + L")",
                         DuplicationHint(hr));
            return false;
        }
        const bool presented = info.LastPresentTime.QuadPart != 0 || info.AccumulatedFrames > 0;
        if (presented || static_cast<int32_t>(GetTickCount() - graceEnd) >= 0) {
            have = true;
        } else {
            dup->ReleaseFrame();
            resource.Reset();
        }
    }

    CapturedFrame desktop;
    bool copied = false;
    {
        AcquiredFrame guard(dup.Get(), info, resource.Get());
        copied = CopyDesktopToCpu(device.Get(), guard.resource(), &desktop, err);
    }  // 这里 ReleaseFrame，之后才能安全地只用 CPU 副本
    if (!copied) return false;

    // 整幅桌面帧单色 = 根本没拿到内容（虚拟显卡 / 远程桌面 / 内容被驱动屏蔽的典型表现）。
    // 判整幅而不是判裁剪后的窗口区域：窗口本身可能就是一块纯黑内容。
    if (FrameIsFlat(desktop)) {
        CaptureError(err, kChannel,
                     L"桌面复制取到的整幅画面是单色：LastPresentTime=" +
                         std::to_wstring(info.LastPresentTime.QuadPart) +
                         L" AccumulatedFrames=" + std::to_wstring(info.AccumulatedFrames) +
                         L" ProtectedContentMaskedOut=" +
                         std::to_wstring(info.ProtectedContentMaskedOut ? 1 : 0),
                     L"该会话的显卡驱动可能不向桌面复制输出内容（远程桌面 / 基本显示驱动 / "
                     L"受保护内容被屏蔽），改用 wgc 或 bitblt");
        return false;
    }

    // 桌面帧覆盖整个输出，窗口矩形要换算到该输出的坐标系再裁
    const int x = ext.left - picked.desc.DesktopCoordinates.left;
    const int y = ext.top - picked.desc.DesktopCoordinates.top;
    const int cropX = std::max(0, x);
    const int cropY = std::max(0, y);
    const int visibleW = std::min(static_cast<int>(desktop.width), x + static_cast<int>(ext.right - ext.left));
    const int visibleH = std::min(static_cast<int>(desktop.height),
                                 y + static_cast<int>(ext.bottom - ext.top));
    if (visibleW <= cropX || visibleH <= cropY) {
        CaptureError(err, kChannel, L"窗口不在这块显示器的桌面帧内",
                     L"窗口可能刚被移到别的显示器或已关闭");
        return false;
    }
    desktop.source = kChannel;
    CropFrame(&desktop, static_cast<uint32_t>(cropX), static_cast<uint32_t>(cropY),
              static_cast<uint32_t>(visibleW - cropX), static_cast<uint32_t>(visibleH - cropY));
    *out = std::move(desktop);
    return true;
}

}  // namespace ecapture
