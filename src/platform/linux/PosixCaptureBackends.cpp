// 【平台层 · Linux】X11 取帧通道的登记与调用口。
//
// 两条独立通道：
//   * xcomposite：只读目标窗口自己的重定向存储，窗口内容路径，--yes 可免程序确认；
//   * x11desktop：只读 X11 根窗口的明确矩形，屏幕 / 整个桌面 / --region 区域都走它。
//     这条路径全是桌面像素，--yes 一律无效；越界 / 空矩形不会在这里被裁切或平移。
//
// 两条通道不互相回退。尤其窗口内容路径失败时绝不会改读桌面像素。

#include "CaptureBackend.h"

#include <string>
#include <utility>
#include <vector>

#include "CaptureScope.h"
#include "DesktopGeometry.h"
#include "Lang.h"
#include "platform/linux/X11Capture.h"

namespace ecapture {
namespace {

bool RequestGuard(const wchar_t* backend, bool havePermit, bool needPermit, bool haveDeadline,
                  Diagnostic* err) {
    if (needPermit && !havePermit) {
        if (err) {
            err->code = codes::kAccessDenied;
            err->message = Msg(L"cap.access_denied");
            err->hint = Msg(L"cap.access_denied_hint");
            err->backend = backend;
            err->stage = stages::kCapture;
        }
        return false;
    }
    if (!haveDeadline) {
        if (err) {
            err->code = codes::kCaptureFailed;
            err->message = Msg(L"cap.no_detail");
            err->backend = backend;
            err->stage = stages::kCapture;
        }
        return false;
    }
    return true;
}

}  // namespace

const std::vector<CaptureBackend>& RegisteredCaptureBackends() {
    static const std::vector<CaptureBackend> kBackends = {
        {CaptureMethod::kXComposite, L"xcomposite", WindowPathOf(CaptureMethod::kXComposite),
         ScreenPathOf(CaptureMethod::kXComposite), true, false},
        {CaptureMethod::kX11Desktop, L"x11desktop", WindowPathOf(CaptureMethod::kX11Desktop),
         ScreenPathOf(CaptureMethod::kX11Desktop), false, true},
    };
    return kBackends;
}

const CaptureBackend* FindCaptureBackend(CaptureMethod method) {
    for (const CaptureBackend& backend : RegisteredCaptureBackends()) {
        if (backend.method == method) return &backend;
    }
    return nullptr;
}

bool AcquireWindowFrame(const CaptureBackend& backend, const CaptureWindowRequest& r,
                        CapturedFrame* out, Diagnostic* err) {
    if (!out) return false;
    if (!RequestGuard(backend.name, r.permit != nullptr,
                      ScopeOf(backend.windowPath) == PixelScope::kDesktop, r.dl != nullptr, err)) {
        return false;
    }
    if (backend.method != CaptureMethod::kXComposite || !r.window) {
        if (err) {
            err->code = codes::kUnsupported;
            err->message = Msg(L"cap.unsupported");
            err->value = backend.name;
            err->backend = backend.name;
            err->stage = stages::kCapture;
        }
        return false;
    }

    x11::DisplayConnection connection;
    if (!connection.ok()) {
        if (err) {
            err->code = codes::kCaptureFailed;
            err->message = Msg(L"cap.x11.display_unavailable");
            err->hint = Msg(L"cap.x11.display_unavailable_hint");
            err->value = backend.name;
            err->backend = backend.name;
            err->stage = stages::kCapture;
            err->detail = L"XOpenDisplay: errno=" + std::to_wstring(connection.error());
        }
        return false;
    }

    CapturedFrame frame;
    bool childrenIncomplete = false;
    if (!x11::CaptureWindowStorage(connection.get(), static_cast<Window>(r.hwnd), r.timeoutMs,
                                   *r.dl, &frame, err, &childrenIncomplete)) {
        return false;
    }
    if (childrenIncomplete) {
        // 有映射中的子窗口没取到：这一帧不完整。绝不把"少了一块"当成完整窗口交出去。
        if (err) {
            err->code = codes::kCaptureFailed;
            err->message = Msg(L"note.x11_children_incomplete");
            err->hint = Msg(L"note.x11_children_incomplete_hint");
            err->value = backend.name;
            err->backend = backend.name;
            err->stage = stages::kCapture;
            err->detail = L"mapped child window unavailable during capture";
        }
        return false;
    }

    frame.source = backend.name;
    frame.path = backend.windowPath;
    *out = std::move(frame);
    return true;
}

bool AcquireScreenFrame(const CaptureBackend& backend, const CaptureScreenRequest& r,
                        CapturedFrame* out, Diagnostic* err) {
    if (!out) return false;
    if (!RequestGuard(backend.name, r.permit != nullptr,
                      ScopeOf(backend.screenPath) == PixelScope::kDesktop, r.dl != nullptr, err)) {
        return false;
    }
    if (backend.method != CaptureMethod::kX11Desktop || !r.permit) {
        if (err) {
            err->code = codes::kUnsupported;
            err->message = Msg(L"cap.unsupported_for_screen");
            err->value = backend.name;
            err->backend = backend.name;
            err->stage = stages::kCapture;
        }
        return false;
    }
    // 凭证是最后一道编译期/运行期闸门：它批准的是弹框里那一块，采样的 area 只能落在里面。
    // 这里再判一次，不把错误目标交给 XGetImage。
    if (RectIsEmpty(r.area) || !r.permit->Covers(r.area)) {
        if (err) {
            err->code = codes::kConsentStale;
            err->message = Msg(L"cap.consent.stale");
            err->hint = Msg(L"cap.consent.stale_hint");
            err->option = L"--capture";
            err->value = backend.screenPath;
            err->backend = backend.name;
            err->stage = stages::kCapture;
        }
        return false;
    }

    x11::DisplayConnection connection;
    if (!connection.ok()) {
        if (err) {
            err->code = codes::kCaptureFailed;
            err->message = Msg(L"cap.x11.display_unavailable");
            err->hint = Msg(L"cap.x11.display_unavailable_hint");
            err->value = backend.name;
            err->backend = backend.name;
            err->stage = stages::kCapture;
            err->detail = L"XOpenDisplay: errno=" + std::to_wstring(connection.error());
        }
        return false;
    }

    CapturedFrame frame;
    if (!x11::CaptureRootArea(connection.get(), *r.permit, r.area, r.timeoutMs, *r.dl, &frame,
                              err)) {
        if (err && err->backend.empty()) err->backend = backend.name;
        return false;
    }
    frame.source = backend.name;
    frame.path = backend.screenPath;
    *out = std::move(frame);
    return true;
}

}  // namespace ecapture
