#include "CapturePrintWindow.h"

#include <exception>
#include <string>

#include "ImageOps.h"
#include "Worker.h"

namespace ecapture {
namespace {

constexpr const wchar_t* kChannel = L"printwindow";

}  // namespace

RenderOutcome RenderPrintWindowContent(uint64_t hwndValue) {
    RenderOutcome o;
    try {
        const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
        const RECT full = WindowFullRect(hwnd);
        const int width = full.right - full.left;
        const int height = full.bottom - full.top;
        if (width <= 0 || height <= 0) {
            o.status = BlockedStatus::kRectEmpty;
            return o;
        }

        Dib dib;
        Diagnostic dibErr;
        if (!dib.Create(static_cast<uint32_t>(width), static_cast<uint32_t>(height), &dibErr,
                        kChannel)) {
            o.status = BlockedStatus::kDibCreate;
            o.win32 = dibErr.win32;   // 码在失败点当场就取走了，这里只剩翻译的活
            return o;
        }

        // PrintWindow 是"让这个窗口自己画到给它的 DC"，那块 DC 是新建的 DIB 而不是屏幕；
        // 上面 dib.Create 里取屏幕 DC 只为了拿一个兼容的像素格式，没有读回任何像素。
        // 所以这条路径属于窗口内容路径，不需要桌面凭证 —— 也正因为它只读这一个窗口，
        // 才可以在辅助进程里跑。
        if (!PrintWindow(hwnd, dib.dc(), kPwRenderFullContent) && !PrintWindow(hwnd, dib.dc(), 0)) {
            o.status = BlockedStatus::kPrintWindowFailed;
            o.win32 = LastError();
            return o;
        }
        dib.ToFrame(kChannel, &o.frame);

        // 裁掉 DWM 那圈透明边框，让尺寸与其它通道一致。
        // 裁不下来（边框矩形量不到、算出的范围越界）就把没裁的这一张交出去：它仍然是这个
        // 窗口自己画出来的画面，只是多带一圈边框，比报错少一张图更合算。
        const RECT vis = WindowScreenRect(hwnd);
        const int dx = vis.left - full.left;
        const int dy = vis.top - full.top;
        if (dx >= 0 && dy >= 0) {
            (void)CropFrame(&o.frame, static_cast<uint32_t>(dx), static_cast<uint32_t>(dy),
                            static_cast<uint32_t>(vis.right - vis.left),
                            static_cast<uint32_t>(vis.bottom - vis.top));
        }
        o.frame.path = paths::kPrintWindow;
        return o;
    } catch (...) {
        o = RenderOutcome{};
        o.status = BlockedStatus::kInternal;
        DetailFromCurrentException(&o.detail);
        return o;
    }
}

bool CaptureWindowPrintWindow(uint64_t hwnd, const Deadline& dl, CapturedFrame* out,
                              Diagnostic* err) {
    return IsolatedPrintWindow(hwnd, dl, out, err);
}

}  // namespace ecapture
