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

std::wstring HResultText(HRESULT hr) {
    wchar_t buf[64];
    swprintf(buf, 64, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

bool Fail(Diagnostic* err, const wchar_t* what, HRESULT hr) {
    if (err) {
        *err = Diagnostic{codes::kCaptureFailed,
                          std::wstring(what) + L" 失败 (HRESULT " + HResultText(hr) + L")",
                          L"--capture", L"wgc",
                          L"该窗口可能受保护、已关闭，或系统不支持 Windows.Graphics.Capture"};
    }
    return false;
}

ComPtr<ID3D11Device> CreateDevice() {
    return CreateCaptureDevice();  // 与桌面复制通道共用一套设备创建策略
}

wdx11::IDirect3DDevice WrapDevice(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return nullptr;
    winrt::com_ptr<IInspectable> inspectable;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put())))
        return nullptr;
    return inspectable.as<wdx11::IDirect3DDevice>();
}

wgc::GraphicsCaptureItem CreateItem(uint64_t hwnd) {
    try {
        auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        const HRESULT hr = factory->CreateForWindow(
            reinterpret_cast<HWND>(hwnd),
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item));
        if (FAILED(hr) || !item) return nullptr;
        return item;
    } catch (const winrt::hresult_error&) {
        return nullptr;
    }
}

// 把帧的 GPU 纹理复制到 CPU 可读的 staging 纹理
bool CopyToCpu(ID3D11Device* device, ID3D11Texture2D* src, CapturedFrame* out, Diagnostic* err) {
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        if (err) *err = Diagnostic{codes::kCaptureFailed, L"取到的帧尺寸为 0", L"--capture", L"wgc",
                                   L"窗口可能已最小化或刚被关闭"};
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
        return Fail(err, L"创建 staging 纹理", E_FAIL);

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    context->CopyResource(staging.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapHr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapHr)) return Fail(err, L"锁定 staging 纹理", mapHr);

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

}  // namespace

bool CaptureWindowWgc(uint64_t hwnd, uint32_t timeoutMs, CapturedFrame* out, Diagnostic* err) {
    EnsureWinrtInitialized();
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    ComPtr<ID3D11Device> device = CreateDevice();
    if (!device) return Fail(err, L"创建 D3D11 设备", E_FAIL);

    const auto winrtDevice = WrapDevice(device.Get());
    if (!winrtDevice) return Fail(err, L"包装 Direct3D 设备", E_FAIL);

    auto item = CreateItem(hwnd);
    if (!item) return Fail(err, L"GraphicsCaptureItem.CreateForWindow", E_NOINTERFACE);

    const auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0) {
        if (err) *err = Diagnostic{codes::kCaptureFailed, L"窗口当前尺寸为 0，无法采集", L"--capture",
                                   L"wgc", L"窗口可能被最小化或正在退出"};
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
        return Fail(err, L"创建采集会话", e.code());
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
        if (err) *err = Diagnostic{codes::kCaptureFailed,
                                   L"超时未收到帧（" + std::to_wstring(timeoutMs) + L" ms）",
                                   L"--capture", L"wgc",
                                   L"窗口可能没有内容更新，或受 DRM/安全策略保护而拒绝被采集"};
        return false;
    }

    // WinRT 的 IDirect3DSurface 不直接暴露 IDXGISurface，必须经 IDirect3DDxgiInterfaceAccess 取
    ComPtr<IDXGISurface> dxgiSurface;
    {
        ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
        auto* surfaceAbi = static_cast<IInspectable*>(winrt::get_abi(frame.Surface()));
        HRESULT hr = surfaceAbi->QueryInterface(IID_PPV_ARGS(&access));
        if (FAILED(hr)) return Fail(err, L"取帧表面访问器", hr);
        hr = access->GetInterface(IID_PPV_ARGS(&dxgiSurface));
        if (FAILED(hr)) return Fail(err, L"取帧表面", hr);
    }
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT qhr = dxgiSurface->QueryInterface(IID_PPV_ARGS(&texture));
    if (FAILED(qhr)) return Fail(err, L"帧表面转纹理", qhr);

    return CopyToCpu(device.Get(), texture.Get(), out, err);
}

}  // namespace ecapture
