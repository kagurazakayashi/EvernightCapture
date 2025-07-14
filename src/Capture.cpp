#include "Capture.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>   // DXGI_ERROR_DEVICE_* 要参与"这条错误是不是致命"的判断

#include <winrt/base.h>   // 后端可能直接抛出 hresult_error，而不是返回错误码

#include "CaptureWgc.h"
#include "CaptureBitBlt.h"
#include "CaptureCommon.h"
#include "CaptureDwm.h"
#include "CaptureDuplication.h"
#include "CapturePrintWindow.h"
#include "Consent.h"
#include "Encoder.h"
#include "FileSave.h"
#include "Lang.h"
#include "OutputPlan.h"
#include "Report.h"
#include "ScreenMatch.h"
#include "WindowMatch.h"

namespace ecapture {
namespace {

constexpr uint32_t kFrameTimeoutMs = 2000;

// 流水线阶段名统一用 CliOptions.h 的 stages::，这里不再另写一份字面量。

std::wstring HwndHexOf(uint64_t hwnd) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%08X", static_cast<unsigned>(hwnd));
    return buf;
}

// 一次截图的目标：一个窗口，或一整块屏幕。
struct Target {
    bool isScreen = false;
    WindowInfo window;
    ScreenInfo screen;
    RECT area{};  // 授权与 JSON 都用它：窗口 = 整窗外框矩形，屏幕 = 该屏矩形

    // %n 用的名字
    std::wstring Name() const { return isScreen ? ScreenDisplayName(screen) : window.title; }
    // 诊断里标识"是哪个目标"：与 images[].hwnd / images[].device 同源，调用方能对上号
    std::wstring Tag() const {
        return isScreen ? ScreenDisplayName(screen) : HwndHexOf(window.hwnd);
    }
    // 给人看的那一行（弹框与 hint 用）
    std::wstring Describe() const {
        return isScreen ? DescribeScreen(screen) : DescribeWindow(window);
    }
};

// 一次通道尝试的授权结果。桌面路径必须带着判定器签发的凭证才准去读屏幕像素，
// 所以这里把"判过了"和"凭证在手上"绑成同一件事：凭证没拿到就是没判过。
struct AttemptAuth {
    bool ok = false;
    std::optional<DesktopPermit> permit;
};

AttemptAuth AuthorizeAttempt(ConsentGate& gate, const wchar_t* path, const std::wstring& targetKey,
                             const RECT& area, Diagnostic* err) {
    AttemptAuth auth;
    if (ScopeOf(path) != PixelScope::kDesktop) {
        auth.ok = gate.AuthorizeWindow(path, targetKey, err);
        return auth;
    }
    auth.ok = gate.AuthorizeDesktop(path, targetKey, area, &auth.permit, err) &&
              auth.permit.has_value();
    return auth;
}

// 抛出来的异常换成结构化诊断。判据只有"换一条通道会不会有区别"：
// 内存耗尽、显卡设备没了 —— 换了也不会有区别，判致命、终止整批，不许继续换后端。
void FillFromCurrentException(Diagnostic* diag, const wchar_t* stage, const wchar_t* backend,
                              bool* fatal) {
    if (fatal) *fatal = false;
    if (!diag) return;

    std::wstring detail;
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const winrt::hresult_error& e) {
        const HRESULT hr = e.code();
        diag->hresult = HResultText(hr);
        detail = diag->hresult;
        const bool deviceLost = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                                hr == DXGI_ERROR_DEVICE_HUNG;
        if (fatal) *fatal = hr == E_OUTOFMEMORY || deviceLost;
    } catch (const std::bad_alloc&) {
        detail = L"std::bad_alloc";
        if (fatal) *fatal = true;
    } catch (const std::length_error&) {
        detail = L"std::length_error";
        if (fatal) *fatal = true;
    } catch (const std::exception& e) {
        // what() 是 ASCII 说明，逐字节宽化（本项目所有对外文字都走宽字符）
        const char* p = e.what();
        if (p) {
            for (; *p; ++p) detail.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        }
        if (detail.empty()) detail = L"std::exception";
    } catch (...) {
        detail = L"unknown";
    }

    const bool ioStage = std::wcscmp(stage, stages::kWrite) == 0 ||
                         std::wcscmp(stage, stages::kStdout) == 0;
    diag->code = ioStage ? codes::kWriteFailed
               : std::wcscmp(stage, stages::kEncode) == 0 ? codes::kEncoderUnavailable
                                                       : codes::kCaptureFailed;
    diag->message = Msgf(L"err.exception", detail);
    diag->option = ioStage ? L"--out" : L"--capture";
    if (backend) {
        diag->value = backend;
        diag->backend = backend;
    }
    diag->stage = stage;
}

// 一次后端调用的异常边界：后端"失败"与后端"崩了"必须走同一条出口，否则一个抛异常的后端
// 会把整条 auto 回退链、把 --all 的其余目标一起带走。
template <typename Fn>
bool CallBackend(const wchar_t* stage, const wchar_t* backend, Fn fn, Diagnostic* err, bool* fatal) {
    try {
        return fn();
    } catch (...) {
        FillFromCurrentException(err, stage, backend, fatal);
        return false;
    }
}

// 单个通道的取帧入口。进任何一条通道之前先过授权判定：
//   窗口内容路径 —— --yes 免问，否则整批问一次；
//   桌面路径 —— 永远问人，并换来那张凭证，没有它就调不动那条通道的取像素函数。
bool CaptureOneChannel(ConsentGate& gate, const std::wstring& targetKey, const RECT& area,
                       uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                       Diagnostic* err) {
    const wchar_t* path = WindowPathOf(method);
    AttemptAuth auth = AuthorizeAttempt(gate, path, targetKey, area, err);
    if (!auth.ok) {
        // 一帧都不去取。诊断里补上"是哪条通道要去的"：路径名在 value，通道名在 backend。
        if (err && err->backend.empty()) err->backend = CaptureMethodName(method);
        return false;
    }

    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureWindowWgc(hwnd, timeoutMs, out, err);
        case CaptureMethod::kDwmThumbnail:
            // 它自己会在内部升级到桌面路径时回来重新要一次许可，所以把判定器传进去
            return CaptureWindowDwmThumbnail(hwnd, timeoutMs, gate, targetKey, out, err);
        case CaptureMethod::kPrintWindow:
            return CaptureWindowPrintWindow(hwnd, timeoutMs, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureWindowBitBlt(hwnd, timeoutMs, *auth.permit, out, err);
        case CaptureMethod::kDuplication:
            return CaptureWindowDuplication(hwnd, timeoutMs, *auth.permit, out, err);
        case CaptureMethod::kAuto:
            break;  // auto 由 CaptureWithMethod 展开成回退链
    }
    if (err) *err = Diagnostic{codes::kUnsupported, Msg(L"cap.unsupported"), L"--capture",
                               CaptureMethodName(method), std::wstring(), std::wstring(),
                               CaptureMethodName(method), stages::kCapture};
    return false;
}

// 屏幕目标的单通道取帧。dwm / printwindow 取的是"某个窗口的画面"，屏幕上并没有
// 这么一个窗口可让它们画，所以这两种通道在解析期就已经被挡在屏幕目标之外。
// 剩下的三条（wgc / duplication / bitblt）拍的都是那块屏上此刻的一切，全部是桌面路径。
bool CaptureScreenOneChannel(ConsentGate& gate, const std::wstring& targetKey, const ScreenInfo& screen,
                             CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                             Diagnostic* err) {
    const wchar_t* path = ScreenPathOf(method);
    AttemptAuth auth = AuthorizeAttempt(gate, path, targetKey, screen.bounds, err);
    if (!auth.ok) {
        if (err && err->backend.empty()) err->backend = CaptureMethodName(method);
        return false;   // 没通过授权：整块屏幕一个像素都不读
    }

    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureScreenWgc(screen, timeoutMs, *auth.permit, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureScreenBitBlt(screen, timeoutMs, *auth.permit, out, err);
        case CaptureMethod::kDuplication:
            return CaptureScreenDuplication(screen, timeoutMs, *auth.permit, out, err);
        case CaptureMethod::kAuto:
            break;  // auto 由 CaptureScreenWithMethod 展开成回退链
        default:
            break;
    }
    if (err) {
        *err = Diagnostic{codes::kUnsupported, Msgf(L"cap.unsupported_for_screen", CaptureMethodName(method)),
                          L"--capture", CaptureMethodName(method),
                          Msg(L"cap.unsupported_for_screen_hint"), std::wstring(),
                          CaptureMethodName(method), stages::kCapture};
    }
    return false;
}

// auto 的回退链：按顺序试到第一个成功的通道。实际用的不是链首时留一条 note，
// 让调用方知道画面来路不同。
// 两条规矩：
//   * 被拒绝（访问被拒 / 用户不让）不是继续换后端的理由 —— 换一条照样不该给，
//     多问一次只是多扰一次，直接把这条错误交出去。
//   * 后端抛出异常时按异常性质决定：致命（资源或设备没了）立刻终止整条链，
//     可恢复的才继续往下试。
template <typename Try>
bool FallbackChain(const std::vector<CaptureMethod>& chain, CapturedFrame* out, Diagnostic* err,
                   std::vector<Diagnostic>* notes, bool* fatal, Try tryOne) {
    std::wstring tried;
    Diagnostic last{};
    for (const CaptureMethod m : chain) {
        CapturedFrame attempt;
        Diagnostic attemptErr{};
        const wchar_t* backend = CaptureMethodName(m);
        const bool ok = CallBackend(stages::kCapture, backend,
                                    [&] { return tryOne(m, &attempt, &attemptErr); }, &attemptErr,
                                    fatal);
        if (ok) {
            *out = std::move(attempt);
            if (m != chain.front() && notes) {
                notes->push_back(Diagnostic{
                    codes::kCaptureChannel,
                    Msgf(L"note.capture_channel", CaptureMethodName(chain.front()), CaptureMethodName(m)),
                    L"--capture", L"auto", std::wstring(), std::wstring(), backend, stages::kCapture});
            }
            return true;
        }
        if (attemptErr.code == codes::kAccessDenied ||
            attemptErr.code == codes::kConsentUnavailable || attemptErr.code == codes::kConsentStale) {
            if (err) *err = std::move(attemptErr);
            return false;   // 授权这一关的结果不换后端重跑：拒绝就是拒绝，位置变了就重新确认
        }
        if (fatal && *fatal) {
            if (err) *err = std::move(attemptErr);
            return false;   // 致命错误：换后端不会有区别
        }
        if (!tried.empty()) tried += L", ";
        tried += backend;
        last = std::move(attemptErr);
    }
    if (err) {
        // backend 记的是"真实试过的那几条"，不是请求值 auto —— 调用方要据此判断该重试还是换通道
        Diagnostic d{codes::kCaptureFailed, Msgf(L"cap.auto_failed", tried), L"--capture", L"auto",
                     last.message, last.target, tried, stages::kCapture};
        d.hresult = last.hresult;
        d.win32 = last.win32;
        *err = std::move(d);
    }
    return false;
}

// --capture 分派。auto 对窗口按 wgc -> dwm -> printwindow -> bitblt，对屏幕按
// wgc -> duplication -> bitblt。显式指定的通道绝不回退：用户要哪个就要哪个。
bool CaptureWithMethod(ConsentGate& gate, const std::wstring& targetKey, const RECT& area,
                       uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                       Diagnostic* err, std::vector<Diagnostic>* notes, bool* fatal) {
    if (method != CaptureMethod::kAuto) {
        return CallBackend(stages::kCapture, CaptureMethodName(method),
                           [&] {
                               return CaptureOneChannel(gate, targetKey, area, hwnd, method,
                                                        timeoutMs, out, err);
                           },
                           err, fatal);
    }
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                              CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt};
    return FallbackChain(chain, out, err, notes, fatal,
                         [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
                             return CaptureOneChannel(gate, targetKey, area, hwnd, m, timeoutMs,
                                                      frame, e);
                         });
}

bool CaptureScreenWithMethod(ConsentGate& gate, const std::wstring& targetKey,
                             const ScreenInfo& screen, CaptureMethod method, uint32_t timeoutMs,
                             CapturedFrame* out, Diagnostic* err, std::vector<Diagnostic>* notes,
                             bool* fatal) {
    if (method != CaptureMethod::kAuto) {
        return CallBackend(stages::kCapture, CaptureMethodName(method),
                           [&] {
                               return CaptureScreenOneChannel(gate, targetKey, screen, method,
                                                              timeoutMs, out, err);
                           },
                           err, fatal);
    }
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDuplication,
                                              CaptureMethod::kBitBlt};
    return FallbackChain(chain, out, err, notes, fatal,
                         [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
                             return CaptureScreenOneChannel(gate, targetKey, screen, m, timeoutMs,
                                                            frame, e);
                         });
}

// 把已经拿到的错误补上"哪个目标、哪一步"。通道填过的 backend 不改：
// 它写的是自己那一条真实路径，比这里能推断出的更准。
void TagTarget(Diagnostic* d, const Target& t, const wchar_t* stage) {
    if (!d) return;
    // code 恒在是输出契约的一部分：哪一环节漏写了一条诊断，都在这里补成能分支的形状，
    // 而不是让调用方读到一条没有 code 的条目（那等于整份 JSON 都不能按 code 分支了）。
    if (d->code.empty()) {
        const bool ioStage = std::wcscmp(stage, stages::kWrite) == 0 ||
                             std::wcscmp(stage, stages::kStdout) == 0;
        d->code = ioStage ? codes::kWriteFailed : codes::kCaptureFailed;
        d->message = Msg(L"cap.no_detail");
        d->option = ioStage ? L"--out" : L"--capture";
    }
    if (d->target.empty()) d->target = t.Tag();
    if (d->stage.empty()) d->stage = stage;
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
            t.area = s.bounds;
            targets.push_back(std::move(t));
        }
    } else {
        for (const auto& w : SelectWindows(opt, &outcome.errors, &outcome.notes)) {
            Target t;
            t.window = w;
            // 窗口目标用整窗外框当授权范围：桌面路径实际会从屏幕上读走的就是这一块
            // （DWM 那条退路摆的覆盖窗口也按它对齐），比可见边框矩形更宽一点而不是更窄。
            t.area = WindowFullRect(reinterpret_cast<HWND>(w.hwnd));
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

    // 标准输出与文件路径是两条不同的路：stdout 一次只能交付一张图，多张 PNG 首尾拼在
    // 同一条流上不是一幅可解码的图像，而旧实现把 "-" 当文件名前缀算出 "-_1.png" 这种
    // 本地文件更是凭空造路径。所以这里在规划名字、问人、取帧之前就用实际目标数判掉，
    // 一张都不截、一个文件都不写（判据是目标数而不是 --all：--all 也可能只命中一个）。
    if (opt.output == L"-" && targets.size() > 1) {
        outcome.errors.push_back(Diagnostic{codes::kStdoutMultipleTargets,
                                            Msgf(L"cli.stdout_multiple_targets", targets.size()),
                                            L"--out", L"-",
                                            Msgf(L"cli.stdout_multiple_targets_hint", targets.size()),
                                            std::wstring(), std::wstring(), stages::kPlan});
        outcome.exitCode = EX_USAGE;
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
        item.name = t.Name();
        planned.push_back(std::move(item));
    }
    std::vector<std::wstring> plannedPaths;
    std::vector<Diagnostic> planNotes;
    Diagnostic planErr;
    // 规划失败时 notes 整个丢掉：那一次什么都没写，"已补扩展名"之类的提示反而误导
    if (!PlanOutputPaths(opt, planned, &plannedPaths, &planNotes, &planErr)) {
        planErr.stage = stages::kPlan;
        outcome.errors.push_back(std::move(planErr));
        outcome.exitCode = EX_IO_FAILED;
        return outcome;
    }
    for (auto& n : planNotes) outcome.notes.push_back(std::move(n));

    // 授权判定器到这里才建立：目标已选定、输出名已展开，弹框上写的就是它将要截的那些东西。
    // 前面那几关（无匹配 / 歧义、--dry-run、stdout 一次一张、整批名字规划）都在它之前，
    // 所以注定什么都没截的调用不会先打扰人一次。
    //
    // 这里只建立判定器，不预先弹框：要不要问、问几次，取决于每个目标实际走的那条路径
    //（见 CaptureOneChannel）。带 --yes 的窗口内容路径可以一次都不弹；会拍到桌面像素的
    // 那几条一定会弹，且 --yes 在其中不起作用。
    DialogConsentPrompt prompt;
    GateConfig gateCfg;
    gateCfg.yes = opt.yes;
    gateCfg.captureLabel = CaptureMethodName(opt.capture);
    for (const Target& t : targets) {
        GateTarget gt;
        gt.screen = t.isScreen;
        gt.key = t.Tag();
        gt.area = t.area;
        gt.description = Msgf(L"consent.target_line", t.Describe(),
                              static_cast<long long>(gt.area.left),
                              static_cast<long long>(gt.area.top),
                              static_cast<long long>(gt.area.right - gt.area.left),
                              static_cast<long long>(gt.area.bottom - gt.area.top));
        gateCfg.targets.push_back(std::move(gt));
    }
    for (const std::wstring& p : plannedPaths) {
        gateCfg.outputs.push_back(p == L"-" ? Msg(L"consent.output_stdout") : p);
    }
    ConsentGate gate(std::move(gateCfg), prompt);

    // 到这里结果 JSON 去哪条流就已经定死了（图片占 stdout => JSON 走 stderr）。
    // 提前声明归属，后面的应急路径与正常路径才不会各说一套：哪怕中途抛异常、
    // 哪怕一张都没写成，stdout 也不会冒出文字。
    if (opt.output == L"-") ClaimStdout();

    for (size_t i = 0; i < targets.size(); ++i) {
        Target& t = targets[i];
        const ULONGLONG started = GetTickCount64();

        // 量一遍当下的矩形再交给判定器：确认框上写的区域与实际要取样的区域必须是同一块。
        // 目标在确认之后挪走或变大，凭证的 Covers 就会拒绝，这一次不截。
        if (!t.isScreen) t.area = WindowFullRect(reinterpret_cast<HWND>(t.window.hwnd));
        gate.SetTargetArea(t.Tag(), t.area);

        CapturedImage img;
        img.format = FormatName(opt.format);
        img.file = plannedPaths[i];   // 与实际写入的那个名字是同一个字符串
        img.rect = t.area;            // 授权与实际取样的那块屏幕矩形
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

        // 一个目标一个事务：这一步之内任何异常都只作废这一个目标，前面成功的图留着。
        const wchar_t* stage = stages::kCapture;
        Diagnostic targetErr;
        bool fatal = false;
        bool ok = false;
        bool recorded = false;   // stdout 那条纹路里结果条目已提前入列，末尾不再重复入列
        std::vector<uint8_t> encoded;
        try {
            CapturedFrame frame;
            ok = CallBackend(stages::kCapture, CaptureMethodName(opt.capture),
                             [&] {
                                 return t.isScreen
                                            ? CaptureScreenWithMethod(gate, t.Tag(), t.screen,
                                                                      opt.capture, kFrameTimeoutMs,
                                                                      &frame, &targetErr,
                                                                      &outcome.notes, &fatal)
                                            : CaptureWithMethod(gate, t.Tag(), t.area,
                                                                t.window.hwnd, opt.capture,
                                                                kFrameTimeoutMs, &frame,
                                                                &targetErr, &outcome.notes, &fatal);
                             },
                             &targetErr, &fatal);
            if (ok) {
                img.width = frame.width;
                img.height = frame.height;
                img.source = frame.source;   // 真正出图的那条通道，auto 时与请求值不同
                // 实际路径与像素来源：这一帧到底是"窗口自己的画面"还是"屏幕上那块区域"，
                // 调用方要靠它判断自己拿到了什么，--quiet 也不许把它藏起来。
                img.path = frame.path.empty() ? std::wstring(paths::kUnknown) : frame.path;
                img.scope = ScopeName(ScopeOf(img.path));
            }

            const wchar_t* backend = img.source.empty() ? CaptureMethodName(opt.capture)
                                                        : img.source.c_str();
            if (ok) {
                stage = stages::kEncode;
                ok = CallBackend(stage, backend,
                                 [&] {
                                     return EncodeFrame(frame, opt.format, opt.jpegQuality, &encoded,
                                                        &targetErr);
                                 },
                                 &targetErr, &fatal);
            }

            if (ok && img.file == L"-") {
                stage = stages::kStdout;
                DWORD ioError = 0;
                // 结果条目先构造好，再发图片字节：发出去了这一张就算成，
                // 发不出去就撤回来，captured 与实际到达 stdout 的字节不会互相打脸。
                img.bytes = encoded.size();
                img.elapsedMs = static_cast<uint32_t>(GetTickCount64() - started);
                outcome.images.push_back(img);
                recorded = true;

                ok = EmitStdoutBytes(encoded, &ioError);
                if (!ok) {
                    outcome.images.pop_back();
                    recorded = false;
                    targetErr = Diagnostic{codes::kWriteFailed, Msg(L"io.stdout_failed"),
                                           L"--out", L"-", std::wstring(), t.Tag(), backend,
                                           stages::kStdout};
                    targetErr.win32 = ioError;
                }
            } else if (ok) {
                stage = stages::kWrite;
                Diagnostic writeErr;
                ok = CallBackend(stage, backend,
                                 [&] {
                                     return SaveFileAtomic(img.file, encoded, opt.overwrite,
                                                           &writeErr);
                                 },
                                 &writeErr, &fatal);
                if (!ok) targetErr = std::move(writeErr);
            }
        } catch (...) {
            // 走到这里说明上面那几层边界之外还有东西抛（例如 std::vector 扩容失败）：
            // 同样只作废这一个目标，除非它确实是整机级别的资源问题。
            FillFromCurrentException(&targetErr, stage, CaptureMethodName(opt.capture), &fatal);
            ok = false;
        }

        if (!ok) {
            if (recorded) outcome.images.pop_back();   // 字节没到 stdout，那张不算
            TagTarget(&targetErr, t, stage);
            outcome.errors.push_back(std::move(targetErr));
            if (fatal || gate.Refused()) {
                // 致命错误，或者已经有人在确认框上答过"否"（包括根本弹不出框）：
                // 剩下那些目标不再换后端、不再重试，也不再问第二次 —— 直接停在这里。
                // 前面已经写出的图与这条诊断都留着，调用方看得到"停在哪、为什么停"。
                break;
            }
            continue;
        }

        if (recorded) continue;   // stdout 那条路已经入过列
        img.bytes = encoded.size();
        img.elapsedMs = static_cast<uint32_t>(GetTickCount64() - started);
        outcome.images.push_back(std::move(img));
    }

    if (outcome.images.empty()) {
        const std::wstring& code =
            outcome.errors.empty() ? std::wstring() : outcome.errors.front().code;
        outcome.exitCode = code == codes::kAccessDenied || code == codes::kConsentUnavailable
                             ? EX_DENIED
                         : code == codes::kWriteFailed || code == codes::kFileExists
                             ? EX_IO_FAILED
                             : EX_CAPTURE_FAILED;
    } else {
        // 部分成功：图片已写出，但有目标失败 -> 用截图失败码提示调用方看 errors
        outcome.exitCode = outcome.errors.empty() ? EX_OK : EX_CAPTURE_FAILED;
    }
    return outcome;
}

}  // namespace ecapture
