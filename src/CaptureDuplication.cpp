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
#include <utility>
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

bool EnumerateOutputs(IDXGIDevice* device, std::vector<std::pair<ComPtr<IDXGIOutput1>, DXGI_OUTPUT_DESC>>* out,
                      Diagnostic* err, const wchar_t* failKey) {
    ComPtr<IDXGIAdapter> adapter;
    HRESULT hr = device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, Msg(L"cap.dup.adapter"), Msgf(L"cap.hresult", HResultText(hr)),
                     codes::kCaptureFailed, 0, hr);
        return false;
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(i, &output))) break;
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) continue;
        ComPtr<IDXGIOutput1> as1;
        if (FAILED(output.As(&as1))) continue;
        out->emplace_back(std::move(as1), desc);
    }
    if (out->empty()) {
        CaptureError(err, kChannel, Msg(failKey), std::wstring());
        return false;
    }
    return true;
}

bool PickOutputOverlapping(IDXGIDevice* device, const RECT& window, PickedOutput* out,
                           Diagnostic* err) {
    std::vector<std::pair<ComPtr<IDXGIOutput1>, DXGI_OUTPUT_DESC>> outputs;
    if (!EnumerateOutputs(device, &outputs, err, L"cap.dup.no_adapter_outputs")) return false;
    long long best = 0;
    for (const auto& [output1, desc] : outputs) {
        const long long overlap = AreaOf(IntersectRect2(window, desc.DesktopCoordinates));
        if (overlap <= best) continue;
        best = overlap;
        out->output1 = output1;
        out->desc = desc;
    }
    if (!out->output1 || best <= 0) {
        CaptureError(err, kChannel, Msg(L"cap.dup.no_output"), Msg(L"cap.dup.no_output_hint"));
        return false;
    }
    return true;
}

// 屏幕目标：EnumDisplayMonitors 与 DXGI 是两套枚举，能把它们对上的只有设备名
// （两边都是 "\\.\DISPLAY1" 这个形状）。名字对不上时退回比矩形——
// 虚拟显卡偶有设备名不一致的情况。
bool PickOutputForScreen(IDXGIDevice* device, const ScreenInfo& screen, PickedOutput* out,
                         Diagnostic* err) {
    std::vector<std::pair<ComPtr<IDXGIOutput1>, DXGI_OUTPUT_DESC>> outputs;
    if (!EnumerateOutputs(device, &outputs, err, L"cap.dup.no_adapter_outputs")) return false;
    for (const auto& [output1, desc] : outputs) {
        if (screen.deviceName == desc.DeviceName) {
            out->output1 = output1;
            out->desc = desc;
            return true;
        }
    }
    for (const auto& [output1, desc] : outputs) {
        if (desc.DesktopCoordinates.left == screen.bounds.left &&
            desc.DesktopCoordinates.top == screen.bounds.top &&
            desc.DesktopCoordinates.right == screen.bounds.right &&
            desc.DesktopCoordinates.bottom == screen.bounds.bottom) {
            out->output1 = output1;
            out->desc = desc;
            return true;
        }
    }
    CaptureError(err, kChannel, Msg(L"cap.dup.no_output"), Msg(L"cap.dup.no_output_for_screen_hint"));
    return false;
}

std::wstring DuplicationHint(HRESULT hr) {
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        return Msg(L"cap.dup.hint_unsupported");
    }
    if (hr == E_ACCESSDENIED) {
        return Msg(L"cap.dup.hint_denied");
    }
    if (hr == DXGI_ERROR_INVALID_CALL) {
        return Msg(L"cap.dup.hint_invalid_call");
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return Msg(L"cap.dup.hint_timeout");
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
        CaptureError(err, kChannel, Msg(L"cap.dup.to_texture"), Msgf(L"cap.hresult", HResultText(hr)),
                     codes::kCaptureFailed, 0, hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC src{};
    desktop->GetDesc(&src);
    if (src.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        CaptureError(err, kChannel, Msg(L"cap.dup.format"), Msg(L"cap.dup.format_hint"));
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = src;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    const HRESULT stagingHr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(stagingHr)) {
        // 这里以前把错误码整个丢掉：只剩一句"建不出来"，无从判断是显存不够还是格式不支持
        CaptureError(err, kChannel, Msg(L"cap.dup.staging"), Msgf(L"cap.hresult", HResultText(stagingHr)),
                     codes::kCaptureFailed, 0, stagingHr);
        return false;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    // CopyResource 返回 void，失败只能靠设备状态与画面内容反证（见调用方的单色判定）
    context->CopyResource(staging.Get(), desktop.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, Msg(L"cap.dup.map"), Msgf(L"cap.hresult", HResultText(hr)),
                     codes::kCaptureFailed, 0, hr);
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

// 取该输出的整幅桌面帧：等一次真实 present、拷进 CPU、把单色帧判掉。
bool GrabOutputFrame(ID3D11Device* device, const PickedOutput& picked, uint32_t timeoutMs,
                     CapturedFrame* desktop, Diagnostic* err) {
    ComPtr<IDXGIOutputDuplication> dup;
    const HRESULT hr = picked.output1->DuplicateOutput(device, &dup);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, Msgf(L"cap.dup.duplicate", HResultText(hr)),
                     DuplicationHint(hr), codes::kCaptureFailed, 0, hr);
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
            CaptureError(err, kChannel, Msgf(L"cap.dup.timeout", timeoutMs),
                         DuplicationHint(DXGI_ERROR_WAIT_TIMEOUT), codes::kFrameTimeout);
            return false;
        }
        const HRESULT acquire = dup->AcquireNextFrame(
            std::min(kAcquireSliceMs, static_cast<UINT>(remaining)), &info, &resource);
        if (acquire == DXGI_ERROR_WAIT_TIMEOUT || acquire == S_FALSE) continue;
        if (FAILED(acquire)) {
            CaptureError(err, kChannel, Msgf(L"cap.dup.acquire", HResultText(acquire)),
                         DuplicationHint(acquire), codes::kCaptureFailed, 0, acquire);
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

    bool copied = false;
    {
        AcquiredFrame guard(dup.Get(), info, resource.Get());
        copied = CopyDesktopToCpu(device, guard.resource(), desktop, err);
    }  // 这里 ReleaseFrame，之后才能安全地只用 CPU 副本
    if (!copied) return false;

    // 整幅桌面帧单色 = 根本没拿到内容（虚拟显卡 / 远程桌面 / 内容被驱动屏蔽的典型表现）。
    // 判整幅而不是判裁剪后的目标区域：目标本身可能就是一块纯黑内容。
    if (FrameIsFlat(*desktop)) {
        CaptureError(err, kChannel,
                     Msgf(L"cap.dup.flat", info.LastPresentTime.QuadPart, info.AccumulatedFrames,
                          info.ProtectedContentMaskedOut ? 1 : 0),
                     Msg(L"cap.dup.flat_hint"));
        return false;
    }
    desktop->source = kChannel;
    return true;
}

// 桌面帧覆盖整个输出，目标矩形要换算到该输出的坐标系再裁。
// 屏幕目标传的就是这块输出本身，裁下来等价于整幅帧。
bool CropDesktopToRect(CapturedFrame* desktop, const RECT& desktopCoordinates, const RECT& rect,
                       Diagnostic* err) {
    const int x = rect.left - desktopCoordinates.left;
    const int y = rect.top - desktopCoordinates.top;
    const int cropX = std::max(0, x);
    const int cropY = std::max(0, y);
    const int visibleW = std::min(static_cast<int>(desktop->width), x + static_cast<int>(rect.right - rect.left));
    const int visibleH = std::min(static_cast<int>(desktop->height), y + static_cast<int>(rect.bottom - rect.top));
    if (visibleW <= cropX || visibleH <= cropY) {
        CaptureError(err, kChannel, Msg(L"cap.dup.not_in_frame"),
                     Msg(L"cap.dup.not_in_frame_hint"));
        return false;
    }
    CropFrame(desktop, static_cast<uint32_t>(cropX), static_cast<uint32_t>(cropY),
              static_cast<uint32_t>(visibleW - cropX), static_cast<uint32_t>(visibleH - cropY));
    return true;
}

// 公共路线：建设备 -> 挑输出 -> 取整幅桌面帧 -> 按目标矩形裁
bool CaptureRectDuplication(const RECT& rect, const ScreenInfo* screen, const wchar_t* path,
                            uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err) {
    HRESULT deviceHr = S_OK;
    ComPtr<ID3D11Device> device = CreateCaptureDevice(&deviceHr);
    if (!device) {
        CaptureError(err, kChannel, Msg(L"cap.dup.device"), Msgf(L"cap.hresult", HResultText(deviceHr)),
                     codes::kCaptureFailed, 0, deviceHr);
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    const HRESULT qx = device.As(&dxgiDevice);
    if (FAILED(qx)) {
        CaptureError(err, kChannel, Msg(L"cap.dup.dxgi"), Msgf(L"cap.hresult", HResultText(qx)),
                     codes::kCaptureFailed, 0, qx);
        return false;
    }

    PickedOutput picked;
    if (screen) {
        if (!PickOutputForScreen(dxgiDevice.Get(), *screen, &picked, err)) return false;
    } else {
        if (!PickOutputOverlapping(dxgiDevice.Get(), rect, &picked, err)) return false;
    }

    CapturedFrame desktop;
    if (!GrabOutputFrame(device.Get(), picked, timeoutMs, &desktop, err)) return false;
    if (!CropDesktopToRect(&desktop, picked.desc.DesktopCoordinates, rect, err)) return false;
    // 裁成窗口大小不改变来路：这一帧取自显示器合成分，仍然是桌面像素。
    desktop.path = path;
    *out = std::move(desktop);
    return true;
}

}  // namespace

bool CaptureWindowDuplication(uint64_t hwndValue, uint32_t timeoutMs, const DesktopPermit& permit,
                              CapturedFrame* out, Diagnostic* err) {
    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const RECT ext = WindowScreenRect(hwnd);
    if (ext.right <= ext.left || ext.bottom <= ext.top) {
        CaptureError(err, kChannel, Msg(L"cap.rect_empty"), Msg(L"cap.window_gone"));
        return false;
    }
    if (!PermitCovers(permit, ext, kChannel, err)) return false;
    return CaptureRectDuplication(ext, nullptr, paths::kDuplicationFrame, timeoutMs, out, err);
}

bool CaptureScreenDuplication(const ScreenInfo& screen, uint32_t timeoutMs,
                              const DesktopPermit& permit, CapturedFrame* out, Diagnostic* err) {
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    if (screen.bounds.right <= screen.bounds.left || screen.bounds.bottom <= screen.bounds.top) {
        CaptureError(err, kChannel, Msg(L"cap.rect_empty_screen"), Msg(L"cap.screen_rect_broken"));
        return false;
    }
    if (!PermitCovers(permit, screen.bounds, kChannel, err)) return false;
    return CaptureRectDuplication(screen.bounds, &screen, paths::kScreenDuplication, timeoutMs, out,
                                  err);
}

}  // namespace ecapture
