#include "CaptureBitBlt.h"

#include <string>

namespace ecapture {
namespace {

constexpr const wchar_t* kChannel = L"bitblt";

}  // namespace

bool CaptureWindowBitBlt(uint64_t hwndValue, uint32_t /*timeoutMs*/, CapturedFrame* out,
                         Diagnostic* err) {
    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    const RECT rect = WindowScreenRect(hwnd);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        CaptureError(err, kChannel, L"窗口矩形为空", L"窗口可能被最小化或已关闭");
        return false;
    }
    return GrabScreenRect(rect, kChannel, out, err);
}

}  // namespace ecapture
