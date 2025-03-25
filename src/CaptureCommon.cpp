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

std::wstring Win32ErrorText() { return L"错误码 " + std::to_wstring(GetLastError()); }

std::wstring HResultText(HRESULT hr) {
    wchar_t buf[40];
    swprintf(buf, 40, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

void CaptureError(Diagnostic* err, const wchar_t* channel, const std::wstring& message,
                  const std::wstring& hint) {
    if (err) *err = Diagnostic{codes::kCaptureFailed, message, L"--capture", channel, hint};
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
        CaptureError(err, channel, L"窗口尺寸无法分配位图",
                     L"尺寸 " + std::to_wstring(width) + L"x" + std::to_wstring(height) +
                         L"，窗口可能已最小化或正在退出");
        return false;
    }
    const HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) {
        CaptureError(err, channel, L"CreateCompatibleDC 失败", Win32ErrorText());
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
        DeleteDC(dc);
        CaptureError(err, channel, L"CreateDIBSection 失败", Win32ErrorText());
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

bool GrabScreenRect(const RECT& rect, const wchar_t* channel, CapturedFrame* out, Diagnostic* err) {
    const int left = rect.left;
    const int top = rect.top;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) {
        CaptureError(err, channel, L"窗口在屏幕上的矩形为空", L"窗口可能被最小化或已关闭");
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
        CaptureError(err, channel, L"窗口完全在屏幕之外，取不到像素",
                     L"该通道只能拷屏幕上可见的部分");
        return false;
    }

    Dib dib;
    if (!dib.Create(static_cast<uint32_t>(cw), static_cast<uint32_t>(ch), err, channel)) return false;
    const HDC screen = GetDC(nullptr);
    // CAPTUREBLT：把分层窗口（截图工具自己的提示层等）也算进画面
    const BOOL blt = BitBlt(dib.dc(), 0, 0, cw, ch, screen, clip.left, clip.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!blt) {
        CaptureError(err, channel, L"BitBlt 屏幕失败", Win32ErrorText());
        return false;
    }
    dib.ToFrame(channel, out);
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
