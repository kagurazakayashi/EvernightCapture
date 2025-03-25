#include "CaptureDwm.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dwmapi.h>

#include <string>
#include <utility>

#include "CaptureCommon.h"
#include "ImageOps.h"

namespace ecapture {
namespace {

constexpr wchar_t kHostClass[] = L"EvernightCaptureThumbHost";
constexpr const wchar_t* kChannel = L"dwm";
constexpr uint32_t kComposeWaitMs = 700;
constexpr int kOffscreen = -32000;  // 完全在显示器之外：屏幕上看不见，也不参与取图

ATOM RegisterHostClassOnce() {
    static const ATOM atom = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // 纯黑背景：宿主窗口自己什么都不画，看到的像素只可能来自 DWM 合成进来的缩略图
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kHostClass;
        return RegisterClassExW(&wc);
    }();
    return atom;
}

// DWM 缩略图的目标窗口。注册后 DWM 会把源窗口的缓存面持续合成进这个窗口，
// 于是"读这个窗口"就等于"读源窗口当时的画面"，源窗口被遮挡也照样有。
class ThumbHost {
public:
    ThumbHost() = default;
    ~ThumbHost() {
        if (thumb_) DwmUnregisterThumbnail(thumb_);
        if (hwnd_) DestroyWindow(hwnd_);
    }
    ThumbHost(const ThumbHost&) = delete;
    ThumbHost& operator=(const ThumbHost&) = delete;

    bool Start(HWND src, const RECT& at, Diagnostic* err);
    // 从屏幕外挪到源窗口位置上，供退路（从屏幕拷）使用
    void MoveOver(const RECT& at);
    bool OnTopOfItsRect() const;
    RECT Rect() const { return WindowFullRect(hwnd_); }
    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_ = nullptr;
    HTHUMBNAIL thumb_ = nullptr;
    int x_ = kOffscreen;
    int y_ = kOffscreen;
};

bool ThumbHost::Start(HWND src, const RECT& at, Diagnostic* err) {
    if (!RegisterHostClassOnce()) {
        CaptureError(err, kChannel, L"注册缩略图目标窗口类失败", Win32ErrorText());
        return false;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass, L"", WS_POPUP, x_, y_,
                            at.right - at.left, at.bottom - at.top, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        CaptureError(err, kChannel, L"创建缩略图目标窗口失败", Win32ErrorText());
        return false;
    }
    // 必须真的可见，DWM 才会往它的表面合成；SW_SHOWNA 显示但不抢焦点
    ShowWindow(hwnd_, SW_SHOWNA);

    HRESULT hr = DwmRegisterThumbnail(hwnd_, src, &thumb_);
    if (FAILED(hr) || !thumb_) {
        CaptureError(err, kChannel, L"DwmRegisterThumbnail 失败",
                     L"HRESULT " + HResultText(hr) + L"；窗口可能已关闭，或系统未开启桌面合成");
        return false;
    }

    // 用 DWM 缓存面的尺寸当目标窗口大小，保证 1:1 不被缩放
    SIZE source{};
    if (SUCCEEDED(DwmQueryThumbnailSourceSize(thumb_, &source)) && source.cx > 0 && source.cy > 0) {
        SetWindowPos(hwnd_, nullptr, x_, y_, source.cx, source.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    const RECT rc = WindowFullRect(hwnd_);
    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_RECTDESTINATION;
    props.fVisible = TRUE;
    props.fSourceClientAreaOnly = FALSE;  // 连标题栏和边框，与其它通道的整窗画面一致
    props.rcDestination = RECT{0, 0, rc.right - rc.left, rc.bottom - rc.top};
    hr = DwmUpdateThumbnailProperties(thumb_, &props);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, L"DwmUpdateThumbnailProperties 失败", L"HRESULT " + HResultText(hr));
        return false;
    }
    return true;
}

void ThumbHost::MoveOver(const RECT& at) {
    const RECT rc = WindowFullRect(hwnd_);
    x_ = at.left;
    y_ = at.top;
    SetWindowPos(hwnd_, HWND_TOPMOST, x_, y_, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 该矩形是否真的归我们：后台进程抢 z 序可能被别的置顶窗口压住，
// 那样从屏幕拷回来的就是别人的画面，必须报错而不是交一张错图。
bool ThumbHost::OnTopOfItsRect() const {
    const RECT rc = Rect();
    const POINT probes[4] = {
        POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2},
        POINT{rc.left + 2, rc.top + 2},
        POINT{rc.right - 3, rc.top + 2},
        POINT{rc.left + 2, rc.bottom - 3},
    };
    for (const POINT& p : probes) {
        if (WindowFromPoint(p) != hwnd_) return false;
    }
    return true;
}

// 主路径：对屏幕外的宿主窗口调 PrintWindow，走 DWM 的重定向位图，屏幕上毫无动静
bool GrabViaPrintWindow(const ThumbHost& host, CapturedFrame* out, Diagnostic* err) {
    const RECT rc = host.Rect();
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        CaptureError(err, kChannel, L"缩略图宿主窗口尺寸为空", std::wstring());
        return false;
    }
    Dib dib;
    if (!dib.Create(static_cast<uint32_t>(width), static_cast<uint32_t>(height), err, kChannel))
        return false;
    if (!PrintWindow(host.hwnd(), dib.dc(), kPwRenderFullContent) &&
        !PrintWindow(host.hwnd(), dib.dc(), 0)) {
        CaptureError(err, kChannel, L"对缩略图宿主窗口调用 PrintWindow 失败", Win32ErrorText());
        return false;
    }
    dib.ToFrame(kChannel, out);
    return true;
}

}  // namespace

bool CaptureWindowDwmThumbnail(uint64_t hwndValue, uint32_t timeoutMs, CapturedFrame* out,
                               Diagnostic* err) {
    const HWND src = reinterpret_cast<HWND>(hwndValue);
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const RECT at = WindowScreenRect(src);
    if (at.right <= at.left || at.bottom <= at.top) {
        CaptureError(err, kChannel, L"窗口矩形为空", L"窗口可能被最小化或已关闭");
        return false;
    }

    ThumbHost host;
    if (!host.Start(src, at, err)) return false;
    const uint32_t wait = timeoutMs < kComposeWaitMs ? timeoutMs : kComposeWaitMs;

    // DWM 异步合成，先泵一会儿消息再取
    PumpMessagesFor(wait);
    CapturedFrame frame;
    Diagnostic firstErr;
    if (GrabViaPrintWindow(host, &frame, &firstErr) && !FrameIsFlat(frame)) {
        *out = std::move(frame);
        return true;
    }

    // 退路：PW_RENDERFULLCONTENT 要 Win8.1+，更早的系统只能把宿主窗口盖到目标位置上，
    // 再从屏幕拷那块矩形。
    host.MoveOver(at);
    PumpMessagesFor(wait);
    if (!host.OnTopOfItsRect()) {
        if (err) {
            *err = firstErr.message.empty()
                       ? Diagnostic{codes::kCaptureFailed, L"缩略图未被合成，且无法把宿主窗口置顶",
                                    L"--capture", kChannel,
                                    L"屏幕上另有更高层的置顶窗口；改用 wgc 或 printwindow 通道"}
                       : firstErr;
        }
        return false;
    }
    return GrabScreenRect(host.Rect(), kChannel, out, err);
}

}  // namespace ecapture
