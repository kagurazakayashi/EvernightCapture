#include "CaptureWgc.h"

#include "WinrtApartment.h"
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
#include "CursorControl.h"   // cursor_effective:: 那两个机器名（读回不符时写进 message）
#include "Deadline.h"        // dl：透传给 CopyTextureRectToFrame，守 tone mapping 那趟扫描的预算
#include "HdrColor.h"        // --hdr：帧池该建 FP16 还是 B8G8R8A8、显示 HDR 状态怎么只读地问
#include "WgcGeometry.h"     // 每帧的几何判据与"要不要重建帧池 / 还来不来得及"这两条纯函数
#include "WgcGeometry.h"     // 每帧的实际内容尺寸与纹理尺寸对不对得上，由它判

namespace winrt_impl = winrt::impl;
namespace wg = winrt::Windows::Graphics;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;
namespace wdx11 = winrt::Windows::Graphics::DirectX::Direct3D11;
using Microsoft::WRL::ComPtr;

namespace ecapture {
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
    // 光标那两个读数一起清掉：上一条会话的答复不能冒充这一条（调用点里 out 可能被复用）。
    out->cursorStateKnown = false;
    out->cursorInFrame = false;
    // 色彩那两件也归位：out 可能被复用，上一条的 HDR 结论不能留在下一条的帧上（同光标的理由）。
    out->sourceColorSpace = FrameColorSpace::kSrgbBgra8;
    out->toneMapped = false;
}

// 把帧的 GPU 纹理复制到 CPU 可读的 staging 纹理，再按行搬进帧。这一段与桌面复制通道共用
//（CaptureCommon.h 的 CopyTextureToFrame）：形状与上限在分配之前判完，Map 之后抛异常也要 Unmap。
// rect 版只搬纹理左上角 contentWidth×contentHeight 那块有效矩形（缩小时纹理仍是帧池那份较大的
// 尺寸，多出来的边缘是没定义的内容）。
bool CopyToCpu(ID3D11Device* device, ID3D11Texture2D* src, uint32_t contentWidth,
               uint32_t contentHeight, const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
               Diagnostic* err) {
    return CopyTextureRectToFrame(device, src, 0, 0, contentWidth, contentHeight, L"wgc", hdr, dl,
                                  out, err);
}

// 会话与帧池的生命周期：WinRT 的 Close 是显式调用（析构只 Release COM 引用，不会停止采集 /
// 归还显卡资源），所以"正常返回""取帧失败""抛异常"三条路都必须经过同一个 Close。用一个小守卫
// 持有 session 与 pool 的引用，析构里按顺序关；重建帧池前手动关一次，再把成员换新。
struct SessionGuard {
    wgc::GraphicsCaptureSession* session = nullptr;
    wgc::Direct3D11CaptureFramePool* pool = nullptr;
    SessionGuard(wgc::GraphicsCaptureSession* s, wgc::Direct3D11CaptureFramePool* p)
        : session(s), pool(p) {}
    ~SessionGuard() { Close(); }
    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;

    void Close() {
        // 先关会话再关帧池：会话是从帧池创建的，反过来的顺序在池已释放后碰会话会二次故障。
        // 任何一步抛异常都吞掉：这是清理路径，不能让"关不干净"盖掉真正要交回去的那条诊断。
        try {
            if (session && *session) session->Close();
        } catch (...) {
        }
        if (session) *session = nullptr;
        try {
            if (pool && *pool) pool->Close();
        } catch (...) {
        }
        if (pool) *pool = nullptr;
    }
};

// 光标要不要画进帧里（--cursor）。这条会话接口是本工具唯一**设得进去也读得回来**的一处：
// IGraphicsCaptureSession2::IsCursorCaptureEnabled（本机 SDK 的 windows.graphics.capture.idl 里
// 这条接口写在 contract UniversalApiContract 10.0，文档标 10.0.19041.0）。
// 必须在 StartCapture **之前**设：开始采集之后再改，这一帧要不要画已经定了。
// 三种下场：
//   * 没写 --cursor（given=false）—— 一个字节都不改，也不去读那个属性；帧里两个值留 false/false，
//     于是结果里那三个键一个都不出现（与这条选项存在之前逐字节相同）。
//   * 写成 default —— 只 get 不 put：读到的值进 effective，读不到就留"不知道"，不作任何断言。
//   * 写成 include / exclude —— put 之后必须 get 回来核对；问不到接口、设不下去、或读回来不是
//     那一件事，都交回 capture.cursor_unverifiable 而**不开始采集**：一个像素都不读，
//     也绝不"那就照默认交一张"（那等于把"我要求过"当成"已经办到了"）。
// effective 断言到的边界写在 src/CursorControl.h：它说的是这条会话被设置成画 / 不画，
// 而不是"此刻光标正停在目标上所以图里一定看得见"—— 本 SDK 的会话接口没有 IsCursorVisible
// 那个只读属性，所以像素级的事这里一条都不声称。
bool ApplyCursorControl(const wgc::GraphicsCaptureSession& session, const CursorRequest& cursor,
                        CapturedFrame* out, Diagnostic* err) {
    if (!cursor.given) return true;

    const bool wantIncluded = cursor.mode == CursorMode::kInclude;
    const std::wstring wanted = CursorModeName(cursor.mode);

    auto Reject = [&](const wchar_t* reason, const std::wstring& detail, HRESULT hr) {
        CaptureError(err, L"wgc",
                     Msgf(L"cap.cursor_unverifiable", wanted, reason, detail),
                     Msg(L"cap.cursor_unverifiable_hint"), codes::kCursorUnverifiable, 0, hr);
        return false;
    };

    auto s2 = session.try_as<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession2>();
    if (!s2) {
        // 接口问不到：这一条会话对光标说不出任何东西。
        if (cursor.mode == CursorMode::kDefault) return true;   // 没要求就不打扰这一次截图
        // E_NOINTERFACE 在这里不是顶掉真码的占位符：这一问的下场**就是**"这个接口没实现"。
        return Reject(L"interface_unavailable", HResultText(E_NOINTERFACE), E_NOINTERFACE);
    }

    if (cursor.mode != CursorMode::kDefault) {
        const HRESULT putHr = s2->put_IsCursorCaptureEnabled(wantIncluded ? TRUE : FALSE);
        if (FAILED(putHr)) return Reject(L"set_failed", HResultText(putHr), putHr);
    }

    BOOLEAN actual = FALSE;
    const HRESULT getHr = s2->get_IsCursorCaptureEnabled(&actual);
    if (FAILED(getHr)) {
        if (cursor.mode != CursorMode::kDefault)
            return Reject(L"read_failed", HResultText(getHr), getHr);
        return true;   // default 那一路读不到 = effective 留 unverified，这一次截图照旧
    }

    out->cursorStateKnown = true;
    out->cursorInFrame = actual != FALSE;
    if (cursor.mode != CursorMode::kDefault && out->cursorInFrame != wantIncluded) {
        // 设下去之后读回来不是那一件事：这条会话对光标的答复与要求相反，
        // 交出去的图就在光标这件事上不是用户要的那一种。宁可这一张不截。
        return Reject(L"read_back_mismatch",
                      out->cursorInFrame ? cursor_effective::kInclude : cursor_effective::kExclude,
                      S_OK);
    }
    return true;
}

// 建帧池 -> 开会话 -> 去边框 -> 把光标这件事办妥 -> 开始采集。pool/session 必须是空的
//（重建那条路先经 SessionGuard::Close 把它们关干净再进来）。返回 false 有两条出口，都在
// 这里合成一条诊断：WinRT 那几步抛出的 hresult_error 换成"这一步失败"（原来由调用方 catch，
// 现在两处调用点不再各写一份 try），光标那一条是 capture.cursor_unverifiable。
// 重建帧池时这里会被再走一遍，所以光标设置在**每一条**新会话上都重新应用并重新读回一次：
// 旧会话上设过的值不会跟着新会话过来，而"窗口在取帧之间被放大"恰恰是新池那一条出图。
// poolFormat 是帧池的像素格式：默认 B8G8R8A8（与 --hdr 这条选项存在之前逐字节一致）；只有显式
// 要过 --hdr tonemap 且那块屏此刻核实是 HDR 模式时，调用方才传 A16R16G16B16Float（scRGB 线性），
// 于是交回的帧带得回 HDR 那份亮度，再由 CopyTextureToFrame 映射成 8 位 sRGB。
bool OpenSession(const wdx11::IDirect3DDevice& winrtDevice,
                 const wgc::GraphicsCaptureItem& item, wg::SizeInt32 newSize,
                 wdx::DirectXPixelFormat poolFormat, const CursorRequest& cursor,
                 const wchar_t* stepKey, wgc::Direct3D11CaptureFramePool& pool,
                 wgc::GraphicsCaptureSession& session, CapturedFrame* out, Diagnostic* err) {
    try {
        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice, poolFormat, 2,
                                                                   newSize);
        session = pool.CreateCaptureSession(item);
        // 隐藏窗口边框需要 Win11 的会话接口；拿不到这个接口就不去边框，与本工具无关。
        if (auto s3 =
                session.try_as<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession3>())
            s3->put_IsBorderRequired(FALSE);
        if (!ApplyCursorControl(session, cursor, out, err)) return false;
        session.StartCapture();
    } catch (const winrt::hresult_error& e) {
        return Fail(err, stepKey, e.code());
    }
    return true;
}

// 在 deadline 之前轮询等一帧；拿到返回该帧，超时返回空对象。一次性截图用事件反而麻烦，
// FreeThreaded 池允许任意线程取帧。轮询里 TryGetNextFrame 抛异常当作"这一拍没有帧"。
wgc::Direct3D11CaptureFrame WaitForFrame(const wgc::Direct3D11CaptureFramePool& pool,
                                         const DWORD deadline) {
    while (true) {
        wgc::Direct3D11CaptureFrame frame{nullptr};
        try {
            frame = pool.TryGetNextFrame();
        } catch (const winrt::hresult_error&) {
            frame = nullptr;
        }
        if (frame) return frame;
        if (static_cast<int32_t>(GetTickCount() - deadline) >= 0) return nullptr;
        Sleep(8);
    }
}

// 建帧池 -> 开会话 -> 取一帧 -> 按这一帧的实际内容尺寸拷进 CPU。窗口与屏幕只有"采集项从哪来"
// 这一步不同。旧的实现只认 item.Size() 那一份初始尺寸、并整张纹理复制：窗口在取帧这一刻被缩小，
// 交回的纹理仍是帧池当初那份较大的尺寸，多出来的边缘是没定义的像素，却被当成整幅画面交了出去；
// 窗口被放大超过帧池，那一帧本身就是残缺的，也被当成完整尺寸宣称。现在每帧都按 frame.ContentSize
// 与纹理尺寸重新判（WgcGeometry），并且只在预算之内重建帧池追放大的目标。
bool GrabFrame(const wgc::GraphicsCaptureItem& item, uint32_t timeoutMs,
               const CursorRequest& cursor, wdx::DirectXPixelFormat poolFormat,
               const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out, Diagnostic* err) {
    HRESULT deviceHr = S_OK;
    ComPtr<ID3D11Device> device = CreateDevice(&deviceHr);
    if (!device) return Fail(err, L"cap.wgc.step.device", deviceHr);

    HRESULT wrapHr = S_OK;
    wdx11::IDirect3DDevice winrtDevice{nullptr};
    if (!WrapDevice(device.Get(), &wrapHr, &winrtDevice))
        return Fail(err, L"cap.wgc.step.wrap", wrapHr);

    const auto initial = item.Size();
    if (initial.Width <= 0 || initial.Height <= 0) {
        // 采集项说自己没有面积：窗口刚关掉的典型表现（句柄还有效，画面已经没了）
        CaptureError(err, L"wgc", Msg(L"cap.wgc.size_zero"), Msg(L"cap.window_gone"),
                     codes::kWindowGone);
        return false;
    }

    // 一个绝对期限贯穿整个取帧：等第一帧、重建帧池后再等的每一帧都只花到这里为止，
    // 绝不为"重建一次"重新领一份完整预算（timeoutMs 已是调用方按剩余预算压过的值）。
    const DWORD deadline = GetTickCount() + timeoutMs;

    wg::SizeInt32 poolSize{initial.Width, initial.Height};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    SessionGuard guard{&session, &pool};
    // 开始采集之前这一关：光标那一条要求兑现不了就是 false，一个像素都不读。
    if (!OpenSession(winrtDevice, item, poolSize, poolFormat, cursor, L"cap.wgc.step.session", pool,
                     session, out, err))
        return false;

    uint32_t recreates = 0;
    for (;;) {
        bool reopen = false;
        {
            // 这一帧与它的纹理都关在内层作用域里：需要重建帧池时，先把它们放掉（COM 引用
            // 一 Release，旧池就没有在用的帧了），再关旧会话旧池、开新的，不会带着在用的帧去 Close。
            const wgc::Direct3D11CaptureFrame frame = WaitForFrame(pool, deadline);
            if (!frame) {
                CaptureError(err, L"wgc", Msgf(L"cap.wgc.timeout", timeoutMs),
                             Msg(L"cap.wgc.timeout_hint"), codes::kFrameTimeout);
                return false;
            }

            // 从帧表面取回纹理：WinRT 的 IDirect3DSurface 不直接暴露 IDXGISurface，必须经
            // IDirect3DDxgiInterfaceAccess 转。取到的这张纹理的宽高就是"表面尺寸"。
            ComPtr<ID3D11Texture2D> texture;
            {
                ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
                auto* surfaceAbi = static_cast<IInspectable*>(winrt::get_abi(frame.Surface()));
                HRESULT hr = surfaceAbi->QueryInterface(IID_PPV_ARGS(&access));
                if (FAILED(hr)) return Fail(err, L"cap.wgc.step.access", hr);
                ComPtr<IDXGISurface> dxgiSurface;
                hr = access->GetInterface(IID_PPV_ARGS(&dxgiSurface));
                if (FAILED(hr)) return Fail(err, L"cap.wgc.step.surface", hr);
                hr = dxgiSurface->QueryInterface(IID_PPV_ARGS(&texture));
                if (FAILED(hr)) return Fail(err, L"cap.wgc.step.texture", hr);
            }
            D3D11_TEXTURE2D_DESC texDesc{};
            texture->GetDesc(&texDesc);

            // 每帧重新量一次内容尺寸与采集项尺寸：这两样都可能在"建池"与"取帧"之间被用户拖动改变。
            const auto content = frame.ContentSize();
            const auto now = item.Size();
            WgcGeometry geo;
            geo.poolWidth = static_cast<uint32_t>(poolSize.Width);
            geo.poolHeight = static_cast<uint32_t>(poolSize.Height);
            geo.textureWidth = texDesc.Width;
            geo.textureHeight = texDesc.Height;
            geo.contentWidth = content.Width > 0 ? static_cast<uint32_t>(content.Width) : 0u;
            geo.contentHeight = content.Height > 0 ? static_cast<uint32_t>(content.Height) : 0u;
            geo.itemWidth = now.Width > 0 ? static_cast<uint32_t>(now.Width) : 0u;
            geo.itemHeight = now.Height > 0 ? static_cast<uint32_t>(now.Height) : 0u;

            const WgcFrameDecision decide = DecideWgcFrame(geo, kFrameMaxSide);
            switch (decide.action) {
                case WgcFrameAction::kGone:
                    // 内容或采集项已经没了面积：与建池前那条同一判断，只是这次发生在取到帧之后。
                    CaptureError(err, L"wgc", Msg(L"cap.wgc.size_zero"), Msg(L"cap.window_gone"),
                                 codes::kWindowGone);
                    return false;

                case WgcFrameAction::kInvalid:
                    // 内容比纹理还宽（驱动自相矛盾）或超出单边资源上限：照这种数字复制一定读到
                    // 没分配的内存。交回一条不合法，而不是硬凑一张"看起来完整"的图。
                    CaptureError(err, L"wgc", Msg(L"cap.wgc.frame_invalid"),
                                 Msgf(L"cap.wgc.frame_invalid_hint", geo.contentWidth,
                                      geo.contentHeight, geo.textureWidth, geo.textureHeight),
                                 codes::kFrameInvalid);
                    return false;

                case WgcFrameAction::kResize: {
                    // 目标在取帧这一刻被放大到帧池之外：这一帧不完整。能不能重建由"还剩预算 +
                    // 已试次数"决定，判据与真机取帧解耦在 DecideWgcRecreate 里（离线逐条判见
                    // tests\wgc_state.cpp）。重建不重新领预算：期限还是同一个 deadline。
                    const bool spent = static_cast<int32_t>(GetTickCount() - deadline) >= 0;
                    const WgcRecreateStep step =
                        DecideWgcRecreate(spent, recreates, kMaxWgcRecreates);
                    if (step == WgcRecreateStep::kNoBudget) {
                        CaptureError(err, L"wgc", Msgf(L"cap.wgc.timeout", timeoutMs),
                                     Msg(L"cap.wgc.timeout_hint"), codes::kFrameTimeout);
                        return false;
                    }
                    if (step == WgcRecreateStep::kTooManyTries) {
                        CaptureError(err, L"wgc", Msg(L"cap.wgc.resize_exhausted"),
                                     Msg(L"cap.wgc.resize_exhausted_hint"), codes::kFrameTimeout);
                        return false;
                    }
                    ++recreates;
                    poolSize.Width = static_cast<int32_t>(decide.newWidth);
                    poolSize.Height = static_cast<int32_t>(decide.newHeight);
                    reopen = true;
                    break;
                }

                case WgcFrameAction::kCopy:
                    // 只复制纹理左上角那块有效矩形（contentWidth×contentHeight）：纹理比它大的那
                    // 一圈是没定义的边缘，一个字节都不读。
                    return CopyToCpu(device.Get(), texture.Get(), decide.copyWidth,
                                     decide.copyHeight, hdr, dl, out, err);
            }
            // 走到这里说明枚举里加了新动作却没处理：宁可当不合法交回，也不要默默掉出循环。
            if (!reopen) {
                CaptureError(err, L"wgc", Msg(L"cap.wgc.frame_invalid"),
                             Msg(L"cap.wgc.frame_invalid_hint_generic"), codes::kFrameInvalid);
                return false;
            }
        }  // frame 与 texture 在这里释放，旧池再没有"在用的帧"

        // 重建帧池：关掉这一轮的会话与池（守卫把指针置空），再用更大的尺寸开一条新的。
        // 新会话上光标要**重新设一次并重新读回**（旧会话的设置在 Close 之后不跟着过来），
        // 所以这里走的是同一个 OpenSession，兑现不了同样在开始采集之前就停下。
        guard.Close();
        if (!OpenSession(winrtDevice, item, poolSize, poolFormat, cursor, L"cap.wgc.step.recreate",
                         pool, session, out, err))
            return false;
    }
}

// 只有显式要过 HDR 处理（--hdr tonemap / refuse）时才去只读地问一次那块屏此刻是不是 HDR 模式，
// 再据此决定帧池格式与是否拒绝。没写或 --hdr auto 时这里连那次问答都不发（与这条选项存在之前
// 逐字节相同）。问不出来（kUnknown）既不 tonemap 也不 refuse：HDR 这件事说不出结论时照 SDR 那条
// 走，而不是猜一个（规矩 5；tonemap 因此是恒等透传，refuse 因此不拒绝它没确证过的东西）。
bool DecideWgcPool(uint64_t hwnd, HMONITOR monitor, const HdrRequest& hdr,
                   wdx::DirectXPixelFormat* poolFormat, Diagnostic* err) {
    *poolFormat = wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    if (!hdr.given || hdr.policy == HdrPolicy::kAuto) return true;
    const DisplayHdrState st =
        monitor ? ProbeDisplayHdrForMonitor(monitor) : ProbeDisplayHdrForHwnd(hwnd);
    if (hdr.policy == HdrPolicy::kRefuse && st == DisplayHdrState::kHdr) {
        // 用户要的就是"别给我一张被硬压成 BGRA8 的发白图"：这块屏确实在 HDR 模式，
        // 在 StartCapture 之前就停下，一个像素都不读。
        CaptureError(err, L"wgc",
                     Msgf(L"cap.hdr_refused", FrameColorSpaceName(FrameColorSpace::kScRgbFloat16)),
                     Msg(L"cap.hdr_refused_hint"), codes::kHdrRefused);
        return false;
    }
    if (hdr.policy == HdrPolicy::kToneMap && st == DisplayHdrState::kHdr)
        *poolFormat = wdx::DirectXPixelFormat::R16G16B16A16Float;
    return true;
}

}  // namespace

bool CaptureWindowWgc(uint64_t hwnd, uint32_t timeoutMs, const CursorRequest& cursor,
                      const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out, Diagnostic* err) {
    HRESULT aptHr = S_OK;
    // 套间是按线程的：不能拿"进程里初始化过一次"当凭证（见 WinrtApartment.h）
    if (!EnsureWinrtOnThisThread(&aptHr)) return Fail(err, L"cap.wgc.step.apartment", aptHr);
    ResetFrame(out);

    wdx::DirectXPixelFormat poolFormat = wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    if (!DecideWgcPool(hwnd, nullptr, hdr, &poolFormat, err)) return false;

    HRESULT itemHr = S_OK;
    const auto item = CreateItem(hwnd, nullptr, &itemHr);
    if (!item) return Fail(err, L"cap.wgc.step.item", itemHr);
    if (!GrabFrame(item, timeoutMs, cursor, poolFormat, hdr, dl, out, err)) return false;
    out->path = paths::kWgc;  // 采集项就是这个窗口自己，帧里没有桌面像素
    return true;
}

bool CaptureScreenWgc(const ScreenInfo& screen, uint32_t timeoutMs, const CursorRequest& cursor,
                      const HdrRequest& hdr, const Deadline& dl, const DesktopPermit& permit,
                      CapturedFrame* out, Diagnostic* err) {
    HRESULT aptHr = S_OK;
    // 套间是按线程的：不能拿"进程里初始化过一次"当凭证（见 WinrtApartment.h）
    if (!EnsureWinrtOnThisThread(&aptHr)) return Fail(err, L"cap.wgc.step.apartment", aptHr);
    ResetFrame(out);

    // 整块屏幕的 WGC 拍到的是那块屏上此刻的一切，跟拷屏幕 DC 是同一级风险：
    // 没有人的确认凭证就不建采集项。
    if (!PermitCovers(permit, screen.bounds, L"wgc", err)) return false;

    const HMONITOR monitor = reinterpret_cast<HMONITOR>(screen.monitor);
    wdx::DirectXPixelFormat poolFormat = wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    if (!DecideWgcPool(0, monitor, hdr, &poolFormat, err)) return false;

    HRESULT itemHr = S_OK;
    const auto item = CreateItem(0, monitor, &itemHr);
    if (!item) return Fail(err, L"cap.wgc.step.item_monitor", itemHr);
    if (!GrabFrame(item, timeoutMs, cursor, poolFormat, hdr, dl, out, err)) return false;
    out->path = paths::kScreenWgc;
    return true;
}

}  // namespace ecapture
