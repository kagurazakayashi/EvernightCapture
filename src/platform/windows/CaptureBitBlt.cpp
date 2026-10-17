#include "CaptureBitBlt.h"

#include <string>

#include "platform/windows/WinBridge.h"   // WindowScreenRect 交回的是原生 RECT，进共用结构前转一次

namespace ecapture {
namespace {

constexpr const wchar_t* kChannel = L"bitblt";

}  // namespace

bool CaptureWindowBitBlt(uint64_t hwndValue, uint32_t /*timeoutMs*/, const DesktopPermit& permit,
                         CapturedFrame* out, Diagnostic* err) {
    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const PhysRect rect = FromNativeRect(WindowScreenRect(hwnd));
    if (RectIsEmpty(rect)) {
        CaptureError(err, kChannel, Msg(L"cap.rect_empty"), Msg(L"cap.window_gone"),
                     codes::kWindowGone);
        return false;
    }
    // 凭证在这里是硬要求：这一条截的是屏幕上那块矩形此刻的样子，不是窗口自己的画面。
    return GrabScreenRect(rect, kChannel, paths::kBitBltScreen, permit, out, err);
}

bool CaptureScreenBitBlt(const ScreenInfo& screen, uint32_t /*timeoutMs*/,
                         const DesktopPermit& permit, CapturedFrame* out, Diagnostic* err) {
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const PhysRect& rect = screen.bounds;
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        CaptureError(err, kChannel, Msg(L"cap.rect_empty_screen"), Msg(L"cap.screen_rect_broken"));
        return false;
    }
    return GrabScreenRect(rect, kChannel, paths::kScreenBitBlt, permit, out, err);
}

}  // namespace ecapture
