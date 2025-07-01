#include "CapturePrintWindow.h"

#include <string>

#include "ImageOps.h"

namespace ecapture {
namespace {

constexpr const wchar_t* kChannel = L"printwindow";

}  // namespace

bool CaptureWindowPrintWindow(uint64_t hwndValue, uint32_t /*timeoutMs*/, CapturedFrame* out,
                              Diagnostic* err) {
    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const RECT full = WindowFullRect(hwnd);
    const int width = full.right - full.left;
    const int height = full.bottom - full.top;
    if (width <= 0 || height <= 0) {
        CaptureError(err, kChannel, Msg(L"cap.rect_empty_draw"), Msg(L"cap.window_gone"));
        return false;
    }

    Dib dib;
    if (!dib.Create(static_cast<uint32_t>(width), static_cast<uint32_t>(height), err, kChannel))
        return false;

    if (!PrintWindow(hwnd, dib.dc(), kPwRenderFullContent) && !PrintWindow(hwnd, dib.dc(), 0)) {
        CaptureError(err, kChannel, Msg(L"cap.pw.failed"),
                     Msgf(L"cap.pw.failed_hint", Win32ErrorText()));
        return false;
    }
    dib.ToFrame(kChannel, out);

    // 裁掉 DWM 那圈透明边框，让尺寸与其它通道一致
    const RECT vis = WindowScreenRect(hwnd);
    const int dx = vis.left - full.left;
    const int dy = vis.top - full.top;
    if (dx >= 0 && dy >= 0) {
        CropFrame(out, static_cast<uint32_t>(dx), static_cast<uint32_t>(dy),
                  static_cast<uint32_t>(vis.right - vis.left),
                  static_cast<uint32_t>(vis.bottom - vis.top));
    }
    return true;
}

}  // namespace ecapture
