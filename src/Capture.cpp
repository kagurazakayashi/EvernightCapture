#include "Capture.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureWgc.h"
#include "CaptureBitBlt.h"
#include "CaptureCommon.h"
#include "CaptureDwm.h"
#include "CaptureDuplication.h"
#include "CapturePrintWindow.h"
#include "Consent.h"
#include "Encoder.h"
#include "FileSave.h"
#include "OutputPlan.h"
#include "Report.h"
#include "ScreenMatch.h"
#include "WindowMatch.h"

namespace ecapture {
namespace {

constexpr uint32_t kFrameTimeoutMs = 2000;

std::wstring HwndHexOf(uint64_t hwnd) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%08X", static_cast<unsigned>(hwnd));
    return buf;
}

// 单个通道的取帧入口
bool CaptureOneChannel(uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                       Diagnostic* err) {
    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureWindowWgc(hwnd, timeoutMs, out, err);
        case CaptureMethod::kDwmThumbnail:
            return CaptureWindowDwmThumbnail(hwnd, timeoutMs, out, err);
        case CaptureMethod::kPrintWindow:
            return CaptureWindowPrintWindow(hwnd, timeoutMs, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureWindowBitBlt(hwnd, timeoutMs, out, err);
        case CaptureMethod::kDuplication:
            return CaptureWindowDuplication(hwnd, timeoutMs, out, err);
        case CaptureMethod::kAuto:
            break;  // auto 由 CaptureWithMethod 展开成回退链
    }
    if (err) *err = Diagnostic{codes::kUnsupported, Msg(L"cap.unsupported"), L"--capture",
                               CaptureMethodName(method), std::wstring()};
    return false;
}

// 屏幕目标的单通道取帧。dwm / printwindow 取的是"某个窗口的画面"，屏幕上并没有
// 这么一个窗口可让它们画，所以这两种通道在解析期就已经被挡在屏幕目标之外。
bool CaptureScreenOneChannel(const ScreenInfo& screen, CaptureMethod method, uint32_t timeoutMs,
                             CapturedFrame* out, Diagnostic* err) {
    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureScreenWgc(screen, timeoutMs, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureScreenBitBlt(screen, timeoutMs, out, err);
        case CaptureMethod::kDuplication:
            return CaptureScreenDuplication(screen, timeoutMs, out, err);
        case CaptureMethod::kAuto:
            break;  // auto 由 CaptureScreenWithMethod 展开成回退链
        default:
            break;
    }
    if (err) {
        *err = Diagnostic{codes::kUnsupported, Msgf(L"cap.unsupported_for_screen", CaptureMethodName(method)),
                          L"--capture", CaptureMethodName(method),
                          Msg(L"cap.unsupported_for_screen_hint")};
    }
    return false;
}

// auto 的回退链：按顺序试到第一个成功的通道。实际用的不是链首时留一条 note，
// 让调用方知道画面来路不同。
template <typename Try>
bool FallbackChain(const std::vector<CaptureMethod>& chain, CapturedFrame* out, Diagnostic* err,
                   std::vector<Diagnostic>* notes, Try tryOne) {
    std::wstring tried;
    Diagnostic last{};
    for (const CaptureMethod m : chain) {
        CapturedFrame attempt;
        Diagnostic attemptErr{};
        if (tryOne(m, &attempt, &attemptErr)) {
            *out = std::move(attempt);
            if (m != chain.front() && notes) {
                notes->push_back(Diagnostic{
                    codes::kCaptureChannel,
                    Msgf(L"note.capture_channel", CaptureMethodName(chain.front()), CaptureMethodName(m)),
                    L"--capture", L"auto", std::wstring()});
            }
            return true;
        }
        if (!tried.empty()) tried += L", ";
        tried += CaptureMethodName(m);
        last = std::move(attemptErr);
    }
    if (err) {
        *err = Diagnostic{codes::kCaptureFailed, Msgf(L"cap.auto_failed", tried), L"--capture",
                          L"auto", last.message};
    }
    return false;
}

// --capture 分派。auto 对窗口按 wgc -> dwm -> printwindow -> bitblt，对屏幕按
// wgc -> duplication -> bitblt。显式指定的通道绝不回退：用户要哪个就要哪个。
bool CaptureWithMethod(uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                       Diagnostic* err, std::vector<Diagnostic>* notes) {
    if (method != CaptureMethod::kAuto) return CaptureOneChannel(hwnd, method, timeoutMs, out, err);
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                              CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt};
    return FallbackChain(chain, out, err, notes, [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
        return CaptureOneChannel(hwnd, m, timeoutMs, frame, e);
    });
}

bool CaptureScreenWithMethod(const ScreenInfo& screen, CaptureMethod method, uint32_t timeoutMs,
                             CapturedFrame* out, Diagnostic* err, std::vector<Diagnostic>* notes) {
    if (method != CaptureMethod::kAuto)
        return CaptureScreenOneChannel(screen, method, timeoutMs, out, err);
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDuplication,
                                             CaptureMethod::kBitBlt};
    return FallbackChain(chain, out, err, notes, [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
        return CaptureScreenOneChannel(screen, m, timeoutMs, frame, e);
    });
}

// 一次截图的目标：一个窗口，或一整块屏幕。
struct Target {
    bool isScreen = false;
    WindowInfo window;
    ScreenInfo screen;
};

std::wstring TargetName(const Target& t) {
    return t.isScreen ? ScreenDisplayName(t.screen) : t.window.title;
}

}  // namespace

CaptureOutcome RunCapture(const Options& opt) {
    CaptureOutcome outcome;

    // 屏幕矩形必须与物理像素一致，否则 GDI 通道会截偏
    EnsureDpiAware();

    std::vector<Target> targets;
    if (opt.ScreenMode()) {
        for (const auto& s : SelectScreens(opt, &outcome.errors)) {
            Target t;
            t.isScreen = true;
            t.screen = s;
            targets.push_back(std::move(t));
        }
    } else {
        for (const auto& w : SelectWindows(opt, &outcome.errors, &outcome.notes)) {
            Target t;
            t.window = w;
            targets.push_back(std::move(t));
        }
    }
    if (!outcome.errors.empty()) {
        const std::wstring& code = outcome.errors.front().code;
        outcome.exitCode = code == codes::kAmbiguousWindow ? EX_AMBIGUOUS
                         : code == codes::kIndexOutOfRange || code == codes::kMonitorOutOfRange
                             ? EX_USAGE
                             : EX_NO_MATCH;
        return outcome;
    }

    if (opt.dryRun) {
        std::wstring list;
        for (const Target& t : targets) {
            if (!list.empty()) list += L" | ";
            list += t.isScreen ? DescribeScreen(t.screen) : DescribeWindow(t.window);
        }
        const wchar_t* key = targets.front().isScreen ? L"note.dry_run_monitor" : L"note.dry_run";
        outcome.notes.push_back(Diagnostic{codes::kDryRun, Msgf(key, targets.size()), L"--dry-run",
                                           list, std::wstring()});
        return outcome;
    }

    // 输出路径整批先规划好，再问人、再取帧：撞名要在第一张落地之前就报出来，
    // 而不是截完第一张才发现第二张会把它盖掉（旧实现就是这么静默盖的）。
    // 放在确认框之前也是为了让调用方别为一注定存不下来的批次去打扰人。
    std::vector<OutputTarget> planned;
    planned.reserve(targets.size());
    for (const Target& t : targets) {
        OutputTarget item;
        // %h / %p 只对窗口有意义，屏幕目标给 0；%n 是窗口标题或设备名
        item.hwnd = t.isScreen ? 0 : t.window.hwnd;
        item.pid = t.isScreen ? 0 : t.window.pid;
        item.name = TargetName(t);
        planned.push_back(std::move(item));
    }
    std::vector<std::wstring> paths;
    std::vector<Diagnostic> planNotes;
    Diagnostic planErr;
    // 规划失败时 notes 整个丢掉：那一次什么都没写，"已补扩展名"之类的提示反而误导
    if (!PlanOutputPaths(opt, planned, &paths, &planNotes, &planErr)) {
        outcome.errors.push_back(std::move(planErr));
        outcome.exitCode = EX_IO_FAILED;
        return outcome;
    }
    for (auto& n : planNotes) outcome.notes.push_back(std::move(n));

    // 整屏截图先问人：没有命令行旁路，答"否"或弹不出框都不取帧。
    // --dry-run 在上面就已经返回，所以"只看会截到什么"不会被打扰。
    if (opt.ScreenMode()) {
        std::vector<ScreenInfo> screens;
        for (const auto& t : targets) screens.push_back(t.screen);
        Diagnostic consent;
        if (!AskScreenCaptureConsent(opt, screens, &consent)) {
            outcome.errors.push_back(std::move(consent));
            outcome.exitCode = EX_DENIED;
            return outcome;
        }
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        const Target& t = targets[i];
        const ULONGLONG started = GetTickCount64();

        CapturedImage img;
        img.format = FormatName(opt.format);
        img.file = paths[i];      // 与实际写入的那个名字是同一个字符串
        if (t.isScreen) {
            img.screen = true;
            img.monitorOrdinal = t.screen.ordinal;
            img.deviceName = t.screen.deviceName;
            img.primary = t.screen.primary;
        } else {
            img.hwndHex = HwndHexOf(t.window.hwnd);
            img.pid = t.window.pid;
            img.title = t.window.title;
            img.windowClass = t.window.className;
            img.imageName = t.window.imageName;
        }

        CapturedFrame frame;
        Diagnostic capErr;
        const bool got = t.isScreen
                             ? CaptureScreenWithMethod(t.screen, opt.capture, kFrameTimeoutMs, &frame,
                                                       &capErr, &outcome.notes)
                             : CaptureWithMethod(t.window.hwnd, opt.capture, kFrameTimeoutMs, &frame,
                                                 &capErr, &outcome.notes);
        if (!got) {
            outcome.errors.push_back(std::move(capErr));
            continue;
        }
        img.width = frame.width;
        img.height = frame.height;

        std::vector<uint8_t> bytes;
        Diagnostic encErr;
        if (!EncodeFrame(frame, opt.format, opt.jpegQuality, &bytes, &encErr)) {
            outcome.errors.push_back(std::move(encErr));
            continue;
        }

        if (img.file == L"-") {
            if (!EmitStdoutBytes(bytes)) {
                outcome.errors.push_back(Diagnostic{codes::kWriteFailed, Msg(L"io.stdout_failed"),
                                                    L"--out", L"-", std::wstring()});
                continue;
            }
        } else {
            Diagnostic writeErr;
            if (!SaveFileAtomic(img.file, bytes, opt.overwrite, &writeErr)) {
                outcome.errors.push_back(std::move(writeErr));
                continue;
            }
        }
        img.bytes = bytes.size();
        img.elapsedMs = static_cast<uint32_t>(GetTickCount64() - started);
        outcome.images.push_back(std::move(img));
    }

    if (outcome.images.empty()) {
        const std::wstring& code =
            outcome.errors.empty() ? std::wstring() : outcome.errors.front().code;
        outcome.exitCode = code == codes::kWriteFailed || code == codes::kFileExists
                               ? EX_IO_FAILED
                               : EX_CAPTURE_FAILED;
    } else {
        // 部分成功：图片已写出，但有目标失败 -> 用截图失败码提示调用方看 errors
        outcome.exitCode = outcome.errors.empty() ? EX_OK : EX_CAPTURE_FAILED;
    }
    return outcome;
}

}  // namespace ecapture
