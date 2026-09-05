#include "CaptureCommon.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <string>

#include <d3d11.h>
#include <dxgiformat.h>
#include <wrl/client.h>

#include <dwmapi.h>

#include "ImageOps.h"

using Microsoft::WRL::ComPtr;

namespace ecapture {
namespace {

typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);

// Map 之后可能抛的那一步是 pixels 的扩容。抛出去时 staging 纹理还挂在 Map 状态，
// 之后的取帧会连锁失败，所以 Unmap 交给析构函数，正常路径与异常路径走同一件事。
class MappedStaging {
public:
    MappedStaging(ID3D11DeviceContext* context, ID3D11Texture2D* texture)
        : context_(context), texture_(texture) {}
    MappedStaging(const MappedStaging&) = delete;
    MappedStaging& operator=(const MappedStaging&) = delete;
    ~MappedStaging() {
        if (mapped_) context_->Unmap(texture_, 0);
    }

    HRESULT Map() {
        const HRESULT hr = context_->Map(texture_, 0, D3D11_MAP_READ, 0, &map_);
        if (SUCCEEDED(hr)) mapped_ = true;
        return hr;
    }
    const void* Data() const { return map_.pData; }
    UINT RowPitch() const { return map_.RowPitch; }

private:
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* texture_ = nullptr;
    D3D11_MAPPED_SUBRESOURCE map_{};
    bool mapped_ = false;
};

// 形状检查的一条诊断：把 FrameShape 换成 capture.frame_invalid + 数字。
// 这里在分配之前判，所以判的是"打算按这个形状分配"，不是已经存在的帧。
bool RejectShape(const FrameShapeInfo& intent, const wchar_t* channel, Diagnostic* err) {
    const FrameShape shape = CheckFrameShape(intent);
    if (shape == FrameShape::kOk) return false;
    FrameShapeError(shape, intent, channel, stages::kCapture, err);
    return true;
}

// GPU 纹理上 (x,y) 起 width×height 那块矩形 -> CPU 帧。整幅复制是它的特例（fullBox=true 时
// 走 CopyResource，形状与旧实现完全一致）。分成两条拷贝 API 是有意的：
//   * CopyResource 要求源与目标同尺寸，只能整幅拷 —— 有效内容比纹理小的时候，它会把纹理
//     里那块没定义的边缘一起搬进 staging，正是要避免的读法；
//   * CopySubresourceRegion 带一个源矩形，只搬那块，且 staging 纹理按矩形本身那么大建，
//     于是连"分配整幅再丢掉"这一步都省了。
// fullBox 之外一律按源矩形判形状与上限，分配之前先算清楚。
bool CopyTextureBoxToFrame(ID3D11Device* device, ID3D11Texture2D* src, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height, bool fullBox, const wchar_t* channel,
                           const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
                           Diagnostic* err) {
    if (!device || !src || !out) {
        CaptureError(err, channel, Msg(L"cap.no_detail"), std::wstring());
        return false;
    }
    out->pixels.clear();
    out->width = out->height = out->stride = 0;
    out->sourceColorSpace = FrameColorSpace::kSrgbBgra8;
    out->toneMapped = false;

    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    // 默认（没写 --hdr 或 --hdr auto）只按 BGRA8 解释像素：非 BGRA8 就照旧交回 cap.frame_format，
    // 与这条选项存在之前逐字节相同。显式要过 HDR 处理（tonemap / refuse）时才认得广色域来源，
    // 而且无论哪一条，出了这道门的帧永远是 8 位 BGRA —— 下游的旋转 / 裁剪 / 单色判定 / 编码一行都不改。
    const bool isBgra8 = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
    uint32_t bpp = 4u;
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (!isBgra8) {
        if (!hdr.given || hdr.policy == HdrPolicy::kAuto) {
            CaptureError(err, channel, Msgf(L"cap.frame_format", static_cast<uint64_t>(desc.Format)),
                         Msg(L"cap.frame_format_hint"));
            return false;
        }
        const FrameColorSpace srcCs = FrameColorSpaceFromDxgiFormat(static_cast<uint32_t>(desc.Format));
        if (hdr.policy == HdrPolicy::kRefuse) {
            // refuse 要的就是"别给我一张被硬压成 BGRA8 的发白图"：确凿是 HDR 就停在这里，
            // 认不出的广色域格式也停在这里（猜一个映射与硬按 BGRA8 解释是同一类错误）。
            if (FrameColorSpaceIsHdr(srcCs)) {
                CaptureError(err, channel, Msgf(L"cap.hdr_refused", FrameColorSpaceName(srcCs)),
                             Msg(L"cap.hdr_refused_hint"), codes::kHdrRefused);
            } else {
                CaptureError(err, channel, Msgf(L"cap.hdr_unverifiable", static_cast<uint64_t>(desc.Format)),
                             Msg(L"cap.hdr_unverifiable_hint"), codes::kHdrUnverifiable);
            }
            return false;
        }
        // tonemap：只有**叫得出色彩空间**的那三种广色域格式才继续映射。连布局都认不出
        // （kUnknown）与布局认得出而输出色彩空间没有来源（kRgb10A2Unverified）都一律
        // hdr_unverifiable：认不出不等于"那就硬解释"，而按一套没核实过的传递函数去解一幅图
        // 与按 BGRA8 硬解释是同一类错误。
        if (!FrameColorSpaceIsHdr(srcCs)) {
            CaptureError(err, channel, Msgf(L"cap.hdr_unverifiable", static_cast<uint64_t>(desc.Format)),
                         Msg(L"cap.hdr_unverifiable_hint"), codes::kHdrUnverifiable);
            return false;
        }
        out->sourceColorSpace = srcCs;
        bpp = FrameColorSpaceBytesPerPixel(srcCs);
        stagingFormat = desc.Format;   // CopyResource / CopySubresourceRegion 要求源与目标同格式
    }

    // fullBox 那条沿用"整幅纹理就是画面"的旧语义；带源矩形那条由调用方给出它确认有效的区域。
    uint32_t copyW = width;
    uint32_t copyH = height;
    if (fullBox) {
        copyW = desc.Width;
        copyH = desc.Height;
        x = y = 0;
    }

    // 相加与越界都在 64 位里判（x + width 在 32 位里绕回会伪装成"没越界"）：源矩形必须整个
    // 落在纹理之内，否则就是拿没分配的内存当画面，读之前先拦下。
    if (copyW == 0 || copyH == 0 ||
        static_cast<uint64_t>(x) + copyW > desc.Width ||
        static_cast<uint64_t>(y) + copyH > desc.Height) {
        return RejectShape(FrameShapeInfo{copyW, copyH, copyW * bpp, 0ull, bpp}, channel, err);
    }
    if (copyW > kFrameMaxSide || copyH > kFrameMaxSide) {
        return RejectShape(FrameShapeInfo{copyW, copyH, copyW * bpp, 0ull, bpp}, channel, err);
    }

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = copyW;
    stagingDesc.Height = copyH;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = stagingFormat;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    const HRESULT stagingHr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(stagingHr)) {
        CaptureError(err, channel, Msg(L"cap.gpu.staging"), Msgf(L"cap.hresult", HResultText(stagingHr)),
                     codes::kCaptureFailed, 0, stagingHr);
        return false;
    }

    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) {
        CaptureError(err, channel, Msg(L"cap.gpu.context"), std::wstring());
        return false;
    }
    // CopyResource / CopySubresourceRegion 都返回 void，它们自己失败只能由 GetDeviceRemovedReason
    // 这条 **API 层面** 的问法发现（设备被移除 / 重置 / 挂住）。
    if (fullBox) {
        context->CopyResource(staging.Get(), src);
    } else {
        const D3D11_BOX box{static_cast<UINT>(x), static_cast<UINT>(y), 0u,
                            static_cast<UINT>(x + copyW), static_cast<UINT>(y + copyH), 1u};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, src, 0, &box);
    }
    const HRESULT removed = device->GetDeviceRemovedReason();
    if (FAILED(removed)) {
        CaptureError(err, channel, Msg(L"cap.gpu.copy_failed"), Msgf(L"cap.hresult", HResultText(removed)),
                     codes::kCaptureFailed, 0, removed);
        return false;
    }

    MappedStaging mapped(context.Get(), staging.Get());
    const HRESULT mapHr = mapped.Map();
    if (FAILED(mapHr)) {
        CaptureError(err, channel, Msg(L"cap.gpu.map"), Msgf(L"cap.hresult", HResultText(mapHr)),
                     codes::kCaptureFailed, 0, mapHr);
        return false;
    }
    const uint64_t rowBytes = static_cast<uint64_t>(copyW) * bpp;
    // 行距由驱动给，先核对它装不装得下一行像素，再决定搬多少字节 —— 旧的写法是直接把
    // RowPitch 当 stride 用，RowPitch 比行长小时就是读越界。
    if (mapped.RowPitch() < rowBytes) {
        return RejectShape(FrameShapeInfo{copyW, copyH, mapped.RowPitch(), 0ull, bpp}, channel, err);
    }
    // 分配之前判完字节数：这大小是要 resize 的，判据不能用"分配失败"来发现
    const uint64_t bytes = static_cast<uint64_t>(mapped.RowPitch()) * copyH;
    if (bytes > kFrameMaxBytes) {
        return RejectShape(FrameShapeInfo{copyW, copyH, mapped.RowPitch(), bytes, bpp}, channel, err);
    }

    out->width = copyW;
    out->height = copyH;
    out->stride = mapped.RowPitch();
    out->pixels.assign(static_cast<size_t>(bytes), 0);
    const auto* from = static_cast<const uint8_t*>(mapped.Data());
    auto* to = out->pixels.data();
    for (uint32_t row = 0; row < copyH; ++row) {
        std::memcpy(to + static_cast<size_t>(row) * mapped.RowPitch(),
                    from + static_cast<size_t>(row) * mapped.RowPitch(), static_cast<size_t>(rowBytes));
    }
    out->source = channel;

    // 广色域来源：搬进 CPU 的这块像素此刻还是 FP16 / 10 位，就地映射成 8 位 BGRA sRGB 再交出，
    // 于是下游永远只看见 BGRA8。转换失败（预算用尽 / 形状不合法）时这一张不落地。
    if (out->sourceColorSpace != FrameColorSpace::kSrgbBgra8) {
        if (!ConvertWideFrameToSdrBgra8(out, &dl, err)) {
            out->pixels.clear();
            out->width = out->height = out->stride = 0;
            return false;
        }
        return true;   // 转换里已经重核过 BGRA8 的形状
    }
    // BGRA8 那条：搬完再核一次形状（这次带真实缓冲区大小），不合格就是这里自己写坏了
    return FrameShapeOk(*out, channel, stages::kCapture, err);
}

}  // namespace

// ---------------------------------------------------------------------------
// GPU 纹理 -> CPU 帧
// ---------------------------------------------------------------------------

bool CopyTextureToFrame(ID3D11Device* device, ID3D11Texture2D* src, const wchar_t* channel,
                        const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out, Diagnostic* err) {
    return CopyTextureBoxToFrame(device, src, 0, 0, 0, 0, /*fullBox=*/true, channel, hdr, dl, out,
                                 err);
}

bool CopyTextureRectToFrame(ID3D11Device* device, ID3D11Texture2D* src, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height, const wchar_t* channel,
                            const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
                            Diagnostic* err) {
    return CopyTextureBoxToFrame(device, src, x, y, width, height, /*fullBox=*/false, channel, hdr,
                                 dl, out, err);
}




void EnsureDpiAware() {
    // 只做一次。用 GetProcAddress 而不是直接调用，是为了让二进制在 Win10 1703
    // 以前的系统上仍能跑（那时只有 SetProcessDPIAware）。
    static const bool done = [] {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (fn && fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return true;
            auto legacy = reinterpret_cast<FARPROC>(GetProcAddress(user32, "SetProcessDPIAware"));
            if (legacy) reinterpret_cast<BOOL(WINAPI*)()>(legacy)();
        }
        return true;
    }();
    (void)done;
}

std::wstring Win32ErrorText(DWORD gle) { return Msgf(L"err.win32_code", gle); }

void DetailFromCurrentException(std::string* detail) {
    if (!detail) return;
    detail->clear();
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
        // 只留可打印的 ASCII：这段文字要穿过管道，非 ASCII 字节按码点宽化会得到一堆怪字符，
        // 反而把"崩在哪"这条信息弄丢。
        for (const char* p = e.what(); p && *p; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c >= 32 && c < 127) detail->push_back(*p);
        }
        if (detail->empty()) *detail = "std::exception";
    } catch (...) {
        *detail = "unknown";
    }
}

std::wstring HResultText(HRESULT hr) {
    wchar_t buf[40];
    swprintf(buf, 40, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

void CaptureError(Diagnostic* err, const wchar_t* channel, const std::wstring& message,
                  const std::wstring& hint, const wchar_t* code, DWORD gle, HRESULT hr) {
    if (!err) return;
    *err = Diagnostic{code, message, L"--capture", channel, hint, std::wstring(), channel,
                      stages::kCapture};
    // 错误码只在"当时真拿到了"的时候才写：0 与 S_OK 都意味着"这一步不是靠 Win32 错误码
    // 失败的"，填个 0 进去反而让调用方以为有个叫 0 的故障。
    err->win32 = gle;
    if (FAILED(hr)) err->hresult = HResultText(hr);
}

// ---------------------------------------------------------------------------
// Dib
// ---------------------------------------------------------------------------

Dib::~Dib() { Destroy(); }

void Dib::Destroy() {
    if (saved_ && dc_) SelectObject(dc_, saved_);
    if (bitmap_) DeleteObject(bitmap_);
    if (dc_) DeleteDC(dc_);
    dc_ = nullptr;
    bitmap_ = nullptr;
    saved_ = nullptr;
    bits_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool Dib::Create(uint32_t width, uint32_t height, Diagnostic* err, const wchar_t* channel) {
    Destroy();
    if (width == 0 || height == 0 || width > kFrameMaxSide || height > kFrameMaxSide) {
        CaptureError(err, channel, Msg(L"cap.dib_size"),
                     Msgf(L"cap.dib_size_hint",
                          std::to_wstring(width) + L"x" + std::to_wstring(height)));
        return false;
    }
    const HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) {
        const DWORD gle = LastError();
        CaptureError(err, channel, Msg(L"cap.create_compat_dc"), Win32ErrorText(gle),
                     codes::kCaptureFailed, gle);
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);  // 负值 = 自上而下，与帧布局一致
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        // 先取码再清理：DeleteDC 也会写最后一次错误码，晚一步就拿不到失败原因了
        const DWORD gle = LastError();
        DeleteDC(dc);
        if (bitmap) DeleteObject(bitmap);   // 有极小可能给了位图却没给 bits，位图也得跟着清
        CaptureError(err, channel, Msg(L"cap.create_dib_section"), Win32ErrorText(gle),
                     codes::kCaptureFailed, gle);
        return false;
    }
    const HGDIOBJ saved = SelectObject(dc, bitmap);
    if (!saved) {
        const DWORD gle = LastError();
        DeleteObject(bitmap);
        DeleteDC(dc);
        CaptureError(err, channel, Msg(L"cap.select_bitmap"), Win32ErrorText(gle),
                     codes::kCaptureFailed, gle);
        return false;
    }
    dc_ = dc;
    bitmap_ = bitmap;
    bits_ = bits;
    saved_ = saved;
    width_ = width;
    height_ = height;
    return true;
}

void Dib::ToFrame(const wchar_t* channel, CapturedFrame* out) const {
    const size_t rowBytes = static_cast<size_t>(width_) * 4u;
    out->width = width_;
    out->height = height_;
    out->stride = static_cast<uint32_t>(rowBytes);
    out->pixels.assign(static_cast<const uint8_t*>(bits_),
                       static_cast<const uint8_t*>(bits_) + rowBytes * height_);
    out->source = channel;
}

// ---------------------------------------------------------------------------
// 窗口矩形
// ---------------------------------------------------------------------------

RECT WindowFullRect(HWND hwnd) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return RECT{};
    return rc;
}

RECT WindowScreenRect(HWND hwnd) {
    const RECT full = WindowFullRect(hwnd);
    RECT bounds{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) &&
        bounds.right > bounds.left && bounds.bottom > bounds.top) {
        return bounds;
    }
    return full;
}

bool ClientScreenRect(HWND hwnd, RECT* out) {
    if (!hwnd || !out) return false;
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    // 客户区是空的（还没客户区、或者正被销毁）是一条事实：照原样交回，由调用方判"放不下"
    if (client.right <= client.left || client.bottom <= client.top) {
        *out = client;
        return true;
    }
    POINT origin{0, 0};
    if (!ClientToScreen(hwnd, &origin)) return false;
    // 相加在 64 位里判：落在 LONG 之外就是这一问给不出可信答案，不写一个绕回来的矩形
    const int64_t left = static_cast<int64_t>(origin.x) + client.left;
    const int64_t top = static_cast<int64_t>(origin.y) + client.top;
    const int64_t right = static_cast<int64_t>(origin.x) + client.right;
    const int64_t bottom = static_cast<int64_t>(origin.y) + client.bottom;
    if (left < static_cast<int64_t>(LONG_MIN) || right > static_cast<int64_t>(LONG_MAX) ||
        top < static_cast<int64_t>(LONG_MIN) || bottom > static_cast<int64_t>(LONG_MAX)) {
        return false;
    }
    *out = RECT{static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right),
                static_cast<LONG>(bottom)};
    return true;
}

// ---------------------------------------------------------------------------
// 屏幕取图、消息泵与 z 序验证
// ---------------------------------------------------------------------------

bool PermitCovers(const DesktopPermit& permit, const RECT& area, const wchar_t* channel,
                  Diagnostic* err) {
    if (permit.Covers(area)) return true;
    // 凭证是授权判定器在人看过的那块区域上签发的。现在要取的矩形不在它里面，说明目标在
    // 确认之后挪了位置或变了大小 —— 换一块屏幕位置去截就等于截了别人批准之外的画面。
    CaptureError(err, channel, Msg(L"cap.consent.stale"), Msg(L"cap.consent.stale_hint"),
                 codes::kConsentStale);
    return false;
}

bool GrabScreenRect(const RECT& rect, const wchar_t* channel, const wchar_t* path,
                    const DesktopPermit& permit, CapturedFrame* out, Diagnostic* err) {
    if (!PermitCovers(permit, rect, channel, err)) return false;

    const int left = rect.left;
    const int top = rect.top;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        CaptureError(err, channel, Msg(L"cap.rect_empty_screen"), Msg(L"cap.window_gone"),
                     codes::kWindowGone);
        return false;
    }
    // 与虚拟屏幕求交：多显示器时虚拟屏幕原点可能在负坐标
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT clip{std::max(left, vx), std::max(top, vy), std::min(left + width, vx + vw),
              std::min(top + height, vy + vh)};
    const int cw = clip.right - clip.left;
    const int ch = clip.bottom - clip.top;
    if (cw <= 0 || ch <= 0) {
        CaptureError(err, channel, Msg(L"cap.offscreen"), Msg(L"cap.visible_only"));
        return false;
    }

    Dib dib;
    if (!dib.Create(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), err, channel)) return false;
    const HDC screen = GetDC(nullptr);
    // CAPTUREBLT：把分层窗口（截图工具自己的提示层等）也算进画面
    const BOOL blt = BitBlt(dib.dc(), 0, 0, cw, ch, screen, clip.left, clip.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!blt) {
        const DWORD gle = LastError();
        CaptureError(err, channel, Msg(L"cap.bitblt_failed"), Win32ErrorText(gle),
                     codes::kCaptureFailed, gle);
        return false;
    }
    dib.ToFrame(channel, out);
    out->path = path ? path : paths::kUnknown;
    // 这一条取的是屏幕上那块矩形，超出虚拟屏幕的部分是被丢掉而不是画出来的：把"本来要截哪一块、
    // 实际截到哪一块"一起交回，调用方才不会把一张比目标小的图当成完整目标（窗口内容路径不填这几项）。
    out->reportsCrop = true;
    out->requestedRect = rect;
    out->capturedRect = clip;
    out->clipped = clip.left != left || clip.top != top || clip.right != left + width ||
                   clip.bottom != top + height;
    out->rotation = 0;   // GDI 拷的就是屏幕上此刻的朝向，没有再转一次
    return true;
}

void PumpMessagesFor(uint32_t ms) {
    const DWORD deadline = GetTickCount() + ms;
    MSG msg{};
    while (static_cast<int32_t>(GetTickCount() - deadline) < 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(8);
    }
}

bool WindowIsOnTopAt(HWND hwnd, const RECT& rect) {
    const POINT probes[4] = {
        POINT{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2},
        POINT{rect.left + 2, rect.top + 2},
        POINT{rect.right - 3, rect.top + 2},
        POINT{rect.left + 2, rect.bottom - 3},
    };
    for (const POINT& p : probes) {
        if (WindowFromPoint(p) != hwnd) return false;
    }
    return true;
}

}  // namespace ecapture
