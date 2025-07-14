#include "CaptureWgc.h"

#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <inspectable.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <wrl/client.h>

#include "D3dDevice.h"
#include "CaptureCommon.h"   // CaptureError：backend / stage / hresult 由它统一填

namespace winrt_impl = winrt::impl;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wdx11 = winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace ecapture {

void EnsureWinrtInitialized() {
    static bool initialized = [] {
        const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        // S_FALSE / RPC_E_CHANGED_MODE 表示已经初始化过，不算错
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    (void)initialized;
}

namespace {

// stepKey 指向 resources 里"某一步失败"的文案（cap.wgc.step.*），套进统一的失败句式。
// hr 必须是那一步 API 自己的返回值：这里曾是全文件唯一"没有码可用"的地方，
// 拿 E_FAIL 顶上去会把"驱动拒了""设备被移除""接口没实现"三种完全不同的故障写成同一条。
bool Fail(Diagnostic* err, const wchar_t* stepKey, HRESULT hr,
          const wchar_t* code = codes::kCaptureFailed) {
    CaptureError(err, L"wgc", Msgf(L"cap.wgc.failed", Msg(stepKey), HResultText(hr)),
                 Msg(L"cap.wgc.hint"), code, 0, hr);
    return false;
}

ComPtr<ID3D11Device> CreateDevice(HRESULT* hr) {
    return CreateCaptureDevice(hr);  // 与桌面复制通道共用一套设备创建策略
}

bool WrapDevice(ID3D11Device* device, HRESULT* hr, wdx11::IDirect3DDevice* out) {
    ComPtr<IDXGIDevice> dxgiDevice;
    *hr = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(*hr)) return false;
    winrt::com_ptr<IInspectable> inspectable;
    *hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
    if (FAILED(*hr)) return false;
    *out = inspectable.as<wdx11::IDirect3DDevice>();
    if (!*out) {
        *hr = E_NOINTERFACE;   // 只有这一种情况确实没有更好的码可给
        return false;
    }
    return true;
}

// 从 HWND 或 HMONITOR 建采集项。失败时 *hr 是那条 API 自己的返回值 —— 拿不到采集项与
// "这台机器没有这个接口"是两件事，诊断必须能分开它们。
wgc::GraphicsCaptureItem CreateItem(uint64_t hwnd, HMONITOR monitor, HRESULT* hr) {
    *hr = S_OK;
    try {
        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        *hr = monitor ? factory->CreateForMonitor(
                            monitor,
                            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                            winrt::put_abi(item))
                      : factory->CreateForWindow(
                            reinterpret_cast<HWND>(hwnd),
                            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                            winrt::put_abi(item));
        if (FAILED(*hr) || !item) {
            if (SUCCEEDED(*hr)) *hr = E_UNEXPECTED;   // 说成功却没给东西
            return nullptr;
        }
        return item;
    } catch (const winrt::hresult_error& e) {
        *hr = e.code();
        return nullptr;
    }
}

void ResetFrame(CapturedFrame* out) {
    out->pixels.clear();
    out->width = out->height = out->stride = 0;
}

// 把帧的 GPU 纹理复制到 CPU 可读的 staging 纹理
bool CopyToCpu(ID3D11Device* device, ID3D11Texture2D* src, CapturedFrame* out, Diagnostic* err) {
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        CaptureError(err, L"wgc", Msg(L"cap.wgc.frame_zero"), Msg(L"cap.wgc.frame_zero_hint"));
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    const HRESULT stagingHr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(stagingHr)) return Fail(err, L"cap.wgc.step.staging", stagingHr);

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(staging.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapHr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapHr)) return Fail(err, L"cap.wgc.step.map", mapHr);

    out->width = desc.Width;
    out->height = desc.Height;
    out->stride = mapped.RowPitch;
    out->pixels.resize(static_cast<size_t>(mapped.RowPitch) * desc.Height);
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    auto* target = out->pixels.data();
    for (uint32_t row = 0; row < desc.Height; ++row) {
        memcpy(target + static_cast<size_t>(row) * mapped.RowPitch,
               source + static_cast<size_t>(row) * mapped.RowPitch,
               static_cast<size_t>(desc.Width) * 4u);
    }
    context->Unmap(staging.Get(), 0);
    out->source = L"wgc";
    return true;
}

// 建帧池 -> 开会话 -> 取一帧 -> 拷进 CPU。窗口与屏幕只有"采集项从哪来"这一步不同。
bool GrabFrame(const wgc::GraphicsCaptureItem& item, uint32_t timeoutMs, CapturedFrame* out,
               Diagnostic* err) {
    HRESULT deviceHr = S_OK;
    ComPtr<ID3D11Device> device = CreateDevice(&deviceHr);
    if (!device) return Fail(err, L"cap.wgc.step.device", deviceHr);

    HRESULT wrapHr = S_OK;
    wdx11::IDirect3DDevice winrtDevice{nullptr};
    if (!WrapDevice(device.Get(), &wrapHr, &winrtDevice))
        return Fail(err, L"cap.wgc.step.wrap", wrapHr);

    const auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) {
        // 采集项说自己没有面积：窗口刚关掉的典型表现（句柄还有效，画面已经没了）
        CaptureError(err, L"wgc", Msg(L"cap.wgc.size_zero"), Msg(L"cap.window_gone"),
                     codes::kWindowGone);
        return false;
    }

    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    try {
        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        session = pool.CreateCaptureSession(item);
        // 隐藏窗口边框需要 Win11 的会话接口；本 SDK 没有暴露光标可见性属性，
        // 因此截图中可能出现鼠标指针。
        if (auto s3 = session.try_as<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3>())
            s3->put_IsBorderRequired(FALSE);
        session.StartCapture();
    } catch (const winrt::hresult_error& e) {
        return Fail(err, L"cap.wgc.step.session", e.code());
    }

    // 轮询等帧：一次性截图用事件反而麻烦，FreeThreaded 池允许任意线程取帧
    wgc::Direct3D11CaptureFrame frame{nullptr};
    const DWORD deadline = GetTickCount() + timeoutMs;
    while (true) {
        try {
            frame = pool.TryGetNextFrame();
        } catch (const winrt::hresult_error&) {
            frame = nullptr;
        }
        if (frame) break;
        if (static_cast<int32_t>(GetTickCount() - deadline) >= 0) break;
        Sleep(8);
    }

    struct Closer {
        wgc::GraphicsCaptureSession* session;
        wgc::Direct3D11CaptureFramePool* pool;
        ~Closer() {
            try {
                if (*session) session->Close();
            } catch (...) {}
            try {
                if (*pool) pool->Close();
            } catch (...) {}
        }
    } closer{&session, &pool};

    if (!frame) {
        CaptureError(err, L"wgc", Msgf(L"cap.wgc.timeout", timeoutMs),
                     Msg(L"cap.wgc.timeout_hint"), codes::kFrameTimeout);
        return false;
    }

    // WinRT 的 IDirect3DSurface 不直接暴露 IDXGISurface，必须经 IDirect3DDxgiInterfaceAccess 取
    ComPtr<IDXGISurface> dxgiSurface;
    {
        ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
        auto* surfaceAbi = static_cast<IInspectable*>(winrt::get_abi(frame.Surface()));
        HRESULT hr = surfaceAbi->QueryInterface(IID_PPV_ARGS(&access));
        if (FAILED(hr)) return Fail(err, L"cap.wgc.step.access", hr);
        hr = access->GetInterface(IID_PPV_ARGS(&dxgiSurface));
        if (FAILED(hr)) return Fail(err, L"cap.wgc.step.surface", hr);
    }
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT qhr = dxgiSurface->QueryInterface(IID_PPV_ARGS(&texture));
    if (FAILED(qhr)) return Fail(err, L"cap.wgc.step.texture", qhr);

    return CopyToCpu(device.Get(), texture.Get(), out, err);
}

}  // namespace

bool CaptureWindowWgc(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err) {
    EnsureWinrtInitialized();
    ResetFrame(out);

    HRESULT itemHr = S_OK;
    const auto item = CreateItem(hwnd, nullptr, &itemHr);
    if (!item) return Fail(err, L"cap.wgc.step.item", itemHr);
    if (!GrabFrame(item, timeoutMs, out, err)) return false;
    out->path = paths::kWgc;  // 采集项就是这个窗口自己，帧里没有桌面像素
    return true;
}

bool CaptureScreenWgc(const ScreenInfo& screen, uint32_t timeoutMs, const DesktopPermit& permit,
                      CapturedFrame* out, Diagnostic* err) {
    EnsureWinrtInitialized();
    ResetFrame(out);

    // 整块屏幕的 WGC 拍到的是那块屏上此刻的一切，跟拷屏幕 DC 是同一级风险：
    // 没有人的确认凭证就不建采集项。
    if (!PermitCovers(permit, screen.bounds, L"wgc", err)) return false;

    HRESULT itemHr = S_OK;
    const auto item = CreateItem(0, reinterpret_cast<HMONITOR>(screen.monitor), &itemHr);
    if (!item) return Fail(err, L"cap.wgc.step.item_monitor", itemHr);
    if (!GrabFrame(item, timeoutMs, out, err)) return false;
    out->path = paths::kScreenWgc;
    return true;
}

}  // namespace ecapture
