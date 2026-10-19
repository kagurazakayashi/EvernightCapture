// 【平台层 · Linux】X11 单窗口取帧通道的登记与调用口。
//
// 本构建只有一条通道：xcomposite。它读的是目标窗口（及其子窗口）自己的重定向存储，
// 不是桌面像素，所以落在 CaptureScope 的窗口内容路径上 —— --yes 对它有效，但 --yes
// 不等于"一定有画面"：最小化 / 未映射 / 窗口销毁都如实失败，并明确交回原因。
//
// 没有屏幕目标通道：Linux 本构建不做整屏 / 桌面像素，ScreenPathOf(kXComposite) 在
// 解析期就会被 capability 判成 unsupported（见 CaptureScope.cpp）。

#include "CaptureBackend.h"

#include <string>
#include <utility>
#include <vector>

#include "CaptureScope.h"
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
    (void)r;
    if (!out) return false;
    if (err) {
        err->code = codes::kUnsupported;
        err->message = Msg(L"cap.unsupported_for_screen");
        err->value = backend.name;
        err->backend = backend.name;
        err->stage = stages::kCapture;
    }
    return false;
}

}  // namespace ecapture
