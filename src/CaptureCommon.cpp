#include "CaptureCommon.h"

#include <algorithm>
#include <string>

#include <dwmapi.h>

namespace ecapture {
namespace {

typedef BOOL(WINAPI* SetProcessDpiAwarenessContextFn)(DPI_AWARENESS_CONTEXT);

}  // namespace

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
    if (width == 0 || height == 0 || width > 16384u || height > 16384u) {
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
        CaptureError(err, channel, Msg(L"cap.create_dib_section"), Win32ErrorText(gle),
                     codes::kCaptureFailed, gle);
        return false;
    }
    dc_ = dc;
    bitmap_ = bitmap;
    bits_ = bits;
    saved_ = SelectObject(dc_, bitmap_);
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
