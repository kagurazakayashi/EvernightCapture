// 【平台层 · Windows】五条真实取帧通道的统一登记与调用口。
//
// 共用编排（src/Capture.cpp）只认识 CaptureBackend（src/CaptureBackend.h），不认识任何
// 一个通道头；新增 / 移除一条 Windows 通道只改这一张表与它自己的文件。
//
// 这张表说的都是**静态事实**：这条通道叫什么、窗口/屏幕两种目标支不支持、落到哪条内部
// 路径名。像素来源（窗口内容 / 桌面）仍由 src/CaptureScope.cpp 按**路径**判 —— 不按通道
// 名字猜，也不在这里抄第二份 scope 表。
//
// 授权与期限是请求的一部分：窗口内容路径的目标身份快照、桌面路径的 DesktopPermit、
// 以及这一次还剩的 Deadline 都随请求进来。流水线在调用前已经做完身份复核与授权判定
// （见 src/Capture.cpp 的 CaptureOneChannel / CaptureScreenOneChannel），这里不重做 ——
// 重做会让"两次复核之间"的竞态看起来像已解决，实际没有。缺必需凭证的调用一律拒绝
// （见 RequestGuard），所以不存在"忘了带凭证还能读桌面像素"的入口。

#include "CaptureBackend.h"

#include <vector>

#include "CaptureBitBlt.h"
#include "CaptureCommon.h"
#include "CaptureDuplication.h"
#include "CaptureDwm.h"
#include "CapturePrintWindow.h"
#include "CaptureWgc.h"
#include "CaptureScope.h"
#include "Lang.h"

namespace ecapture {
namespace {

// 一条通道必备的原料不齐时，这里按"没有授权 / 没有期限"拒绝，而不是把空指针交给通道。
// 生产路径上流水线一定给齐；这一道是给将来新增调用点的编译期外兜底（失败原因用既有稳定码，
// 不发明新码：没有凭证就是没取得同意，没有期限就是这一次请求不成立）。
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
    // 顺序与 CaptureMethod 的枚举顺序一致（kAuto 不在表里）；调用方不该依赖这个顺序做选择，
    // auto 链的顺序仍只有 SystemCompat.h 的 AutoChain 一份。
    static const std::vector<CaptureBackend> kBackends = {
        {CaptureMethod::kWgc, L"wgc", WindowPathOf(CaptureMethod::kWgc),
         ScreenPathOf(CaptureMethod::kWgc), true, true},
        {CaptureMethod::kDwmThumbnail, L"dwm", WindowPathOf(CaptureMethod::kDwmThumbnail),
         nullptr, true, false},
        {CaptureMethod::kPrintWindow, L"printwindow", WindowPathOf(CaptureMethod::kPrintWindow),
         nullptr, true, false},
        {CaptureMethod::kBitBlt, L"bitblt", WindowPathOf(CaptureMethod::kBitBlt),
         ScreenPathOf(CaptureMethod::kBitBlt), true, true},
        {CaptureMethod::kDuplication, L"duplication",
         WindowPathOf(CaptureMethod::kDuplication), ScreenPathOf(CaptureMethod::kDuplication), true,
         true},
    };
    return kBackends;
}

const CaptureBackend* FindCaptureBackend(CaptureMethod method) {
    for (const CaptureBackend& b : RegisteredCaptureBackends()) {
        if (b.method == method) return &b;
    }
    return nullptr;
}

bool AcquireWindowFrame(const CaptureBackend& backend, const CaptureWindowRequest& r,
                        CapturedFrame* out, Diagnostic* err) {
    if (!out) return false;
    if (!RequestGuard(backend.name, r.permit != nullptr, ScopeOf(backend.windowPath) == PixelScope::kDesktop,
                      r.dl != nullptr, err)) {
        return false;
    }
    switch (backend.method) {
        case CaptureMethod::kWgc:
            // 光标这件事只有这一条通道有一个真设得进去、也读得回来的开关，所以要求原样交给它；
            // HDR 那一条同通道也只有它有广色域帧池可建，与桌面凭证无关。
            if (!r.cursor || !r.hdr) break;
            return CaptureWindowWgc(r.hwnd, r.timeoutMs, *r.cursor, *r.hdr, *r.dl, out, err);
        case CaptureMethod::kDwmThumbnail:
            // 它自己会在内部升级到桌面路径时回来重新要一次许可，所以把判定器与身份一并交给它。
            if (!r.gate || !r.targetKey || !r.window) break;
            return CaptureWindowDwmThumbnail(r.hwnd, r.timeoutMs, *r.gate, *r.targetKey, *r.window,
                                             *r.dl, out, err);
        case CaptureMethod::kPrintWindow:
            // 这条一律走辅助进程：PrintWindow 同步等目标窗口的线程，本进程里没有中断点。
            return CaptureWindowPrintWindow(r.hwnd, *r.dl, out, err);
        case CaptureMethod::kBitBlt:
            if (!r.permit) break;
            return CaptureWindowBitBlt(r.hwnd, r.timeoutMs, *r.permit, out, err);
        case CaptureMethod::kDuplication:
            if (!r.permit || !r.hdr) break;
            return CaptureWindowDuplication(r.hwnd, r.timeoutMs, *r.permit, *r.hdr, *r.dl, out, err);
        default:
            break;
    }
    // 请求不齐（不该发生的调用形状）或未登记的通道：一条明确的 unsupported，绝不静默交给通道。
    if (err) {
        err->code = codes::kUnsupported;
        err->message = Msg(L"cap.unsupported");
        err->value = backend.name;
        err->backend = backend.name;
        err->stage = stages::kCapture;
    }
    return false;
}

bool AcquireScreenFrame(const CaptureBackend& backend, const CaptureScreenRequest& r,
                        CapturedFrame* out, Diagnostic* err) {
    if (!out) return false;
    if (!RequestGuard(backend.name, r.permit != nullptr, true, r.dl != nullptr, err)) return false;
    if (!backend.supportsScreen || !r.screen) {
        if (err) {
            err->code = codes::kUnsupported;
            err->message = Msg(L"cap.unsupported_for_screen");
            err->value = backend.name;
            err->backend = backend.name;
            err->stage = stages::kCapture;
        }
        return false;
    }
    switch (backend.method) {
        case CaptureMethod::kWgc:
            // 整屏那条与窗口那条共用同一条会话接口，所以光标开关同样设得进去；广色域帧池也在同一处。
            if (!r.cursor || !r.hdr) break;
            return CaptureScreenWgc(*r.screen, r.timeoutMs, *r.cursor, *r.hdr, *r.dl, *r.permit, out,
                                    err);
        case CaptureMethod::kBitBlt:
            return CaptureScreenBitBlt(*r.screen, r.timeoutMs, *r.permit, out, err);
        case CaptureMethod::kDuplication:
            if (!r.hdr) break;
            return CaptureScreenDuplication(*r.screen, r.timeoutMs, *r.permit, *r.hdr, *r.dl, out,
                                            err);
        default:
            break;
    }
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
