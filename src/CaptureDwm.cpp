#include "CaptureDwm.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dwmapi.h>

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

#include "CaptureCommon.h"
#include "Worker.h"

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

    // 失败只给原因码 + 系统错误码（这条函数在本进程与辅助进程里都要能跑，
    // 而辅助进程不产出任何本地化文字）。
    bool Start(HWND src, const RECT& at, BlockedStatus* fail, DWORD* gle, HRESULT* hr);
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

bool ThumbHost::Start(HWND src, const RECT& at, BlockedStatus* fail, DWORD* gle, HRESULT* hr) {
    if (!RegisterHostClassOnce()) {
        if (fail) *fail = BlockedStatus::kHostClass;
        if (gle) *gle = LastError();
        return false;
    }
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass, L"", WS_POPUP, x_, y_,
                            at.right - at.left, at.bottom - at.top, nullptr, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        if (fail) *fail = BlockedStatus::kHostCreate;
        if (gle) *gle = LastError();
        return false;
    }
    // 必须真的可见，DWM 才会往它的表面合成；SW_SHOWNA 显示但不抢焦点
    ShowWindow(hwnd_, SW_SHOWNA);

    const HRESULT regHr = DwmRegisterThumbnail(hwnd_, src, &thumb_);
    if (FAILED(regHr) || !thumb_) {
        if (fail) *fail = BlockedStatus::kRegisterThumb;
        if (hr) *hr = SUCCEEDED(regHr) ? E_POINTER : regHr;
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
    const HRESULT propHr = DwmUpdateThumbnailProperties(thumb_, &props);
    if (FAILED(propHr)) {
        if (fail) *fail = BlockedStatus::kUpdateProps;
        if (hr) *hr = propHr;
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

// 主路径：对屏幕外的宿主窗口调 PrintWindow，走 DWM 的重定向位图，屏幕上毫无动静。
// 只给原因码，因为这条函数在辅助进程里跑 —— 那里没有本地化文案，也不该有。
bool GrabViaPrintWindow(const ThumbHost& host, CapturedFrame* out, BlockedStatus* fail,
                        DWORD* gle) {
    const RECT rc = host.Rect();
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) {
        if (fail) *fail = BlockedStatus::kHostRectEmpty;
        return false;
    }
    Diagnostic dibErr;
    Dib dib;
    if (!dib.Create(static_cast<uint32_t>(width), static_cast<uint32_t>(height), &dibErr, kChannel)) {
        if (fail) *fail = BlockedStatus::kDibCreate;
        if (gle) *gle = dibErr.win32;
        return false;
    }
    if (!PrintWindow(host.hwnd(), dib.dc(), kPwRenderFullContent) &&
        !PrintWindow(host.hwnd(), dib.dc(), 0)) {
        if (fail) *fail = BlockedStatus::kHostPrintWindowFailed;
        if (gle) *gle = LastError();
        return false;
    }
    dib.ToFrame(kChannel, out);
    out->path = paths::kDwmThumbnail;
    if (fail) *fail = BlockedStatus::kOk;
    return true;
}

// 宿主窗口摆在屏幕上之后要拷的那块矩形：以整窗外框（含 DWM 那圈不可见边框）为左上角，
// 尺寸用宿主窗口自己的大小 —— 也就是实际会从屏幕上读走的那一片。
RECT OverlayRect(const ThumbHost& host, const RECT& full) {
    const RECT rc = host.Rect();
    return RECT{full.left, full.top, full.left + (rc.right - rc.left),
                full.top + (rc.bottom - rc.top)};
}

// 退路里"没盖住目标位置 / 没抢到 z 序"那条诊断：优先沿用主路径失败的那一条（它更具体，
// 讲的是那次 PrintWindow 或那次建位图到底为什么没成）。单色画面不走这条路，所以这里
// 说的"没合成出来"只可能来自真正的 API 失败。
Diagnostic NotComposed(const Diagnostic& earlier) {
    if (!earlier.message.empty()) return earlier;
    Diagnostic d;
    d.code = codes::kCaptureFailed;
    d.message = Msg(L"cap.dwm.not_composed");
    d.option = L"--capture";
    d.value = kChannel;
    d.hint = Msg(L"cap.dwm.not_composed_hint");
    d.backend = kChannel;
    d.stage = stages::kCapture;
    return d;
}

}  // namespace

RenderOutcome RenderDwmThumbnailContent(uint64_t hwndValue, uint32_t waitMs) {
    RenderOutcome o;
    try {
        const HWND src = reinterpret_cast<HWND>(hwndValue);
        const RECT at = WindowScreenRect(src);
        if (at.right <= at.left || at.bottom <= at.top) {
            o.status = BlockedStatus::kRectEmpty;
            return o;
        }
        ThumbHost host;
        if (!host.Start(src, at, &o.status, &o.win32, &o.hresult)) {
            o.frame = CapturedFrame{};
            return o;
        }
        // DWM 异步合成，先泵一会儿消息再取。宿主窗口在 -32000，屏幕上不会有任何动静。
        PumpMessagesFor(waitMs);
        BlockedStatus grabFail = BlockedStatus::kOk;
        DWORD gle = 0;
        if (!GrabViaPrintWindow(host, &o.frame, &grabFail, &gle)) {
            o = RenderOutcome{};
            o.status = grabFail;
            o.win32 = gle;
            return o;
        }
        o.status = BlockedStatus::kOk;
        return o;
    } catch (...) {
        o = RenderOutcome{};
        o.status = BlockedStatus::kInternal;
        DetailFromCurrentException(&o.detail);
        return o;
    }
}

bool CaptureWindowDwmThumbnail(uint64_t hwndValue, uint32_t timeoutMs, ConsentGate& gate,
                               const std::wstring& targetKey, const WindowTarget& win,
                               const Deadline& dl, CapturedFrame* out, Diagnostic* err) {
    const HWND src = reinterpret_cast<HWND>(hwndValue);
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const RECT at = WindowScreenRect(src);
    if (at.right <= at.left || at.bottom <= at.top) {
        // 矩形已经量不出来 = 这个窗口现在没了（或正被销毁），调用方该重新枚举而不是重试这条通道
        CaptureError(err, kChannel, Msg(L"cap.rect_empty"), Msg(L"cap.window_gone"),
                     codes::kWindowGone);
        return false;
    }
    if (dl.Spent()) {
        *err = BudgetSpent(dl, codes::kCaptureTimeout, stages::kCapture, kChannel);
        return false;
    }

    // 主路径在辅助进程里跑：读 DWM 重定向位图的那次 PrintWindow 会同步等目标窗口的线程，
    // 而那次等待没有中断点。等待时长已经被剩余预算压过，所以"预算只剩 100 ms"时不会
    // 在子进程里空耗 700 ms。
    const uint32_t want = timeoutMs < kComposeWaitMs ? timeoutMs : kComposeWaitMs;
    const uint32_t wait = dl.ClampWait(want);

    CapturedFrame frame;
    Diagnostic earlier;
    const bool mainOk = IsolatedDwmThumbnail(hwndValue, wait, dl, &frame, &earlier);
    if (mainOk) {
        // 缩略图路径取的就是这个窗口自己的画面，成不成立看的是那次 PrintWindow 的返回值。
        // **画面是不是单色不参与这个判断**：一扇本来就纯黑/纯色的窗口会被误当成"没合成出来"，
        // 于是白白升级到读桌面像素那一条 —— 那是要另外问一次的隐私升级。
        // 单色只作为质量提示交给调用方（note.frame_uniform，见 Capture.cpp）。
        *out = std::move(frame);
        return true;   // 窗口内容路径：屏幕上没有任何动静，也不需要桌面凭证
    }
    if (earlier.code == codes::kCaptureTimeout || earlier.code == codes::kConsentTimeout ||
        earlier.code == codes::kWindowGone || earlier.code == codes::kAccessDenied ||
        earlier.code == codes::kConsentUnavailable || earlier.code == codes::kWorkerFailed ||
        earlier.code == codes::kFrameInvalid) {
        // 期限已经用尽、本工具的辅助进程自己坏了、目标已经没了、帧的形状说不通、
        // 或者授权那一关已经过了/被拒：都不该再用"把宿主窗口盖到目标位置上拷一块屏幕"去掩盖。
        *err = std::move(earlier);
        return false;
    }

    // 退路：PW_RENDERFULLCONTENT 要 Win8.1+，更早的系统只能把宿主窗口盖到目标位置上，
    // 再从屏幕拷那块矩形。这一条读的是桌面像素 —— 先回授权判定器重新确认，
    // 人点头之后才把宿主窗口摆上屏幕（不然确认框开着的时候屏幕上就多了个东西）。
    if (dl.Spent()) {
        *err = BudgetSpent(dl, codes::kCaptureTimeout, stages::kCapture, kChannel);
        return false;
    }
    ThumbHost host;
    BlockedStatus startFail = BlockedStatus::kOk;
    DWORD startGle = 0;
    HRESULT startHr = S_OK;
    if (!host.Start(src, at, &startFail, &startGle, &startHr)) {
        *err = BlockedToDiagnostic(startFail, startGle, startHr, std::string(), kChannel,
                                  stages::kCapture);
        return false;
    }
    const RECT full = WindowFullRect(src);
    const RECT over = OverlayRect(host, full);
    std::optional<DesktopPermit> permit;
    if (!gate.AuthorizeDesktop(paths::kDwmScreen, targetKey, over, &permit, err)) return false;
    // 人点头之后、把宿主窗口摆上屏幕之前，再核一次身份，而且用的是 kFull 那一档：
    // 上面那次 PrintWindow 失败到这里的确认框之间可能停了几秒，而屏幕上那块区域此刻属于谁
    // 完全取决于目标还在不在 —— 用户批准的是当初那一扇窗口，许可不会转移给一个后来的新对象。
    // 这一条路读的是桌面像素，所以"当初那条选择条件还算不算成立"也要一起重问一次。
    if (!win.Recheck(IdentityScope::kFull, err)) {
        if (err) err->backend = kChannel;
        return false;
    }

    host.MoveOver(full);
    PumpMessagesFor(dl.ClampWait(want));
    if (!host.OnTopOfItsRect()) {
        if (err) *err = NotComposed(earlier);
        return false;
    }
    if (!permit) {   // 判定器说可以却没给凭证：宁可什么都不截
        CaptureError(err, kChannel, Msg(L"cap.consent.stale"), Msg(L"cap.consent.stale_hint"),
                     codes::kConsentStale);
        return false;
    }
    return GrabScreenRect(host.Rect(), kChannel, paths::kDwmScreen, *permit, out, err);
}

}  // namespace ecapture
