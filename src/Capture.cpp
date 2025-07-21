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
#include "Deadline.h"
#include "Encoder.h"
#include "FileSave.h"
#include "ImageOps.h"
#include "Lang.h"
#include "OutputPlan.h"
#include "Report.h"
#include "ScreenMatch.h"
#include "WindowMatch.h"
#include "Worker.h"

namespace ecapture {
namespace {

// 等帧的内置上限：wgc / duplication 等一帧最多等多久，dwm 泵消息最多泵多久。
// 给了 --timeout-ms 时它还要被剩余预算压一道（Deadline::ClampWait），所以"总预算只剩
// 300 ms"不会在这里被花成 2000 ms。没给 --timeout-ms 时它照旧生效。
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
//
// timeoutMs 是"这一步自己愿意等多久"（等帧、泵消息），dl 是"这一次运行还剩多少预算"。
// 每条通道真正等下去的时长都是两者里小的那个 —— 预算不被任何一条通道重新领一份。
bool CaptureOneChannel(ConsentGate& gate, const std::wstring& targetKey, const RECT& area,
                       uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, const Deadline& dl,
                       CapturedFrame* out, Diagnostic* err) {
    const wchar_t* path = WindowPathOf(method);
    AttemptAuth auth = AuthorizeAttempt(gate, path, targetKey, area, err);
    if (!auth.ok) {
        // 一帧都不去取。诊断里补上"是哪条通道要去的"：路径名在 value，通道名在 backend。
        if (err && err->backend.empty()) err->backend = CaptureMethodName(method);
        return false;
    }
    const uint32_t wait = dl.ClampWait(timeoutMs);

    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureWindowWgc(hwnd, wait, out, err);
        case CaptureMethod::kDwmThumbnail:
            // 它自己会在内部升级到桌面路径时回来重新要一次许可，所以把判定器传进去
            return CaptureWindowDwmThumbnail(hwnd, wait, gate, targetKey, dl, out, err);
        case CaptureMethod::kPrintWindow:
            // 这条一律走辅助进程：PrintWindow 同步等目标窗口的线程，本进程里没有中断点
            return CaptureWindowPrintWindow(hwnd, dl, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureWindowBitBlt(hwnd, wait, *auth.permit, out, err);
        case CaptureMethod::kDuplication:
            return CaptureWindowDuplication(hwnd, wait, *auth.permit, out, err);
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
bool CaptureScreenOneChannel(ConsentGate& gate, const std::wstring& targetKey,
                             const ScreenInfo& screen, CaptureMethod method, uint32_t timeoutMs,
                             const Deadline& dl, CapturedFrame* out, Diagnostic* err) {
    const wchar_t* path = ScreenPathOf(method);
    AttemptAuth auth = AuthorizeAttempt(gate, path, targetKey, screen.bounds, err);
    if (!auth.ok) {
        if (err && err->backend.empty()) err->backend = CaptureMethodName(method);
        return false;   // 没通过授权：整块屏幕一个像素都不读
    }
    const uint32_t wait = dl.ClampWait(timeoutMs);

    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureScreenWgc(screen, wait, *auth.permit, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureScreenBitBlt(screen, wait, *auth.permit, out, err);
        case CaptureMethod::kDuplication:
            return CaptureScreenDuplication(screen, wait, *auth.permit, out, err);
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
// 三条规矩：
//   * 被拒绝（访问被拒 / 用户不让）不是继续换后端的理由 —— 换一条照样不该给，
//     多问一次只是多扰一次，直接把这条错误交出去。
//   * 后端抛出异常时按异常性质决定：致命（资源或设备没了）立刻终止整条链，
//     可恢复的才继续往下试。
//   * 预算已经用尽就不再试下一条：回退链最容易把"一次截图"变成"四次各拿一份完整超时"，
//     而 --timeout-ms 要管的就是这种重复领取。
template <typename Try>
bool FallbackChain(const std::vector<CaptureMethod>& chain, const Deadline& dl, CapturedFrame* out,
                   Diagnostic* err, std::vector<Diagnostic>* notes, bool* fatal, Try tryOne) {
    std::wstring tried;
    Diagnostic last{};
    for (const CaptureMethod m : chain) {
        if (dl.Spent()) {
            if (err) *err = BudgetSpent(dl, codes::kCaptureTimeout, stages::kCapture,
                                         CaptureMethodName(m));
            return false;
        }
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
                       uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, const Deadline& dl,
                       CapturedFrame* out, Diagnostic* err, std::vector<Diagnostic>* notes,
                       bool* fatal) {
    if (method != CaptureMethod::kAuto) {
        return CallBackend(stages::kCapture, CaptureMethodName(method),
                           [&] {
                               return CaptureOneChannel(gate, targetKey, area, hwnd, method,
                                                        timeoutMs, dl, out, err);
                           },
                           err, fatal);
    }
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                              CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt};
    return FallbackChain(chain, dl, out, err, notes, fatal,
                         [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
                             return CaptureOneChannel(gate, targetKey, area, hwnd, m, timeoutMs, dl,
                                                      frame, e);
                         });
}

bool CaptureScreenWithMethod(ConsentGate& gate, const std::wstring& targetKey,
                             const ScreenInfo& screen, CaptureMethod method, uint32_t timeoutMs,
                             const Deadline& dl, CapturedFrame* out, Diagnostic* err,
                             std::vector<Diagnostic>* notes, bool* fatal) {
    if (method != CaptureMethod::kAuto) {
        return CallBackend(stages::kCapture, CaptureMethodName(method),
                           [&] {
                               return CaptureScreenOneChannel(gate, targetKey, screen, method,
                                                              timeoutMs, dl, out, err);
                           },
                           err, fatal);
    }
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDuplication,
                                              CaptureMethod::kBitBlt};
    return FallbackChain(chain, dl, out, err, notes, fatal,
                         [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
                             return CaptureScreenOneChannel(gate, targetKey, screen, m, timeoutMs,
                                                            dl, frame, e);
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

// ---------------------------------------------------------------------------
// 目标选择：屏幕目标直接按 --monitor 取；窗口目标先做条件求值，再按选择策略消歧。
//
// 条件求值这一步有两条执行路线，判据不是用户有没有要求，而是"这一步有没有中断点"：
//   * 用了 --title-regex —— std::regex 的编译与回溯匹配都没有可查的中断点，
//     而"限制模式串长度"根本不是执行期限（短串一样能爆炸性回溯）。所以放进辅助进程，
//     到点就结束那个进程（Worker.h）。语法在解析期已经校验过一遍，这里跑的是匹配。
//   * 设了 --timeout-ms —— 连"给每个顶层窗口取标题"都可能被一扇挂住的窗口拖住
//     （GetWindowText 是往那个线程发消息并等它回），所以整步也放进辅助进程。
//   * 两者都没有 —— 照旧在本进程枚举，与没有期限机制时的行为完全一致。
// 消歧（SelectFromHits）只对着已经拿到手的列表做决定，永远在本进程跑。
// ---------------------------------------------------------------------------
namespace {

std::vector<WindowInfo> PickWindows(const Options& opt, const Deadline& dl,
                                    std::vector<Diagnostic>* errors) {
    std::vector<RECT> onScreens;
    if (opt.monitor.given) {
        onScreens = SelectedScreenRects(opt, errors);
        if (!errors->empty()) return {};
    }
    const bool isolate = !opt.match.titleRegexes.empty() || dl.Enabled();

    std::vector<WindowInfo> hits;
    std::vector<WindowInfo> iconic;
    if (isolate) {
        Diagnostic err;
        if (!IsolatedMatch(opt.match, onScreens, dl, &hits, &iconic, &err)) {
            // 期限到了 / 辅助进程没起来 / 消息不合：都照实报，不悄悄退回本进程再跑一遍 ——
            // 那样等于把期限当成建议，而慢的那一步下一次还会再慢一遍。
            if (err.code == codes::kMatchTimeout || err.code == codes::kInvalidRegex) {
                errors->push_back(std::move(err));
            } else {
                err.code = codes::kCaptureFailed;
                errors->push_back(std::move(err));
            }
            return {};
        }
    } else {
        MatchRequest req;
        req.match = opt.match;
        req.onScreens = onScreens;
        const MatchOutcome m = EnumerateMatches(req);
        if (m.status != BlockedStatus::kOk) {
            errors->push_back(
                BlockedToDiagnostic(m.status, 0, S_OK, m.detail, L"match", stages::kMatch));
            return {};
        }
        hits = std::move(m.hits);
        iconic = std::move(m.iconic);
    }
    return SelectFromHits(opt, hits, iconic, MonitorLabelOf(opt), errors);
}

}  // namespace

CaptureOutcome RunCapture(const Options& opt) {
    CaptureOutcome outcome;

    // 屏幕矩形必须与物理像素一致，否则 GDI 通道会截偏
    EnsureDpiAware();

    // 整条自动处理链路共用这一份预算：目标选择、后端重试、等帧、编码、提交。
    // 人工确认那一段不计在这里（见 GateConfig.consentTimeoutMs 与 --consent-timeout-ms）。
    const Deadline dl = Deadline::FromTotalMs(opt.timeoutMs);

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
        for (const auto& w : PickWindows(opt, dl, &outcome.errors)) {
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
                         : code == codes::kIndexOutOfRange || code == codes::kMonitorOutOfRange ||
                                 code == codes::kInvalidRegex
                             ? EX_USAGE
                         : code == codes::kMatchTimeout || code == codes::kCaptureTimeout
                             ? EX_CAPTURE_FAILED
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
    // 确认框的等待时长走 --consent-timeout-ms（与上面那份自动预算分开计时）：0 = 一直等人。
    DialogConsentPrompt prompt(opt.consentTimeoutMs);
    GateConfig gateCfg;
    gateCfg.yes = opt.yes;
    gateCfg.captureLabel = CaptureMethodName(opt.capture);
    gateCfg.consentTimeoutMs = opt.consentTimeoutMs;
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

        // 批次语义：预算是**整批一份**，不是一个目标一份。剩下的预算已经用尽时，
        // 后面的目标一个都不开工（不取帧、不弹框、不写文件），各自留下一条 capture.timeout，
        // 调用方因此看得见"这一批停在哪、前面那几张还在不在"。
        if (dl.Spent()) {
            Diagnostic d = BudgetSpent(dl, codes::kCaptureTimeout, stages::kCapture,
                                       CaptureMethodName(opt.capture));
            d.target = t.Tag();
            outcome.errors.push_back(std::move(d));
            break;
        }

        // 量一遍当下的矩形再交给判定器：确认框上写的区域与实际要取样的区域必须是同一块。
        // 目标在确认之后挪走或变大，凭证的 Covers 就会拒绝，这一次不截。
        if (!t.isScreen) t.area = WindowFullRect(reinterpret_cast<HWND>(t.window.hwnd));
        // 屏幕目标另外要按**设备名**重新核对一次（编号只是枚举位置，热插拔之后同一个编号可能
        // 指到另一块屏上）：那块屏拔掉了就一个像素都不读；改了分辨率或位置就换成新矩形交给
        // 判定器 —— 屏幕拓扑一变，已给出的桌面授权自动作废，人会看到重新列出的具体范围，
        // 旧授权不会被用在新显示器上。
        if (t.isScreen) {
            ScreenInfo fresh{};
            const ScreenCheck check = CompareScreen(t.screen, EnumScreens(), &fresh);
            if (check == ScreenCheck::kGone) {
                Diagnostic d{codes::kMonitorChanged, Msg(L"cap.monitor_changed"),
                             L"--monitor", t.screen.deviceName,
                             Msg(L"cap.monitor_changed_hint"), t.Tag()};
                d.stage = stages::kCapture;
                outcome.errors.push_back(std::move(d));
                continue;   // 这一张不取帧，也不替它挑另一块屏
            }
            if (check == ScreenCheck::kMoved) {
                t.screen = fresh;
                t.area = fresh.bounds;
            }
        }
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
        std::optional<Diagnostic> uniformNote;   // 单色质量提示：等这张图真交出去了再送
        std::optional<Diagnostic> clippedNote;   // 区域丢失提示：同上，没交出去就不提示
        std::vector<uint8_t> encoded;
        try {
            CapturedFrame frame;
            ok = CallBackend(stages::kCapture, CaptureMethodName(opt.capture),
                             [&] {
                                 return t.isScreen
                                            ? CaptureScreenWithMethod(gate, t.Tag(), t.screen,
                                                                      opt.capture, kFrameTimeoutMs,
                                                                      dl, &frame, &targetErr,
                                                                      &outcome.notes, &fatal)
                                            : CaptureWithMethod(gate, t.Tag(), t.area,
                                                                t.window.hwnd, opt.capture,
                                                                kFrameTimeoutMs, dl, &frame,
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
                // 从整幅桌面帧里裁出目标的通道（duplication / 拷屏幕的 bitblt）会报告实际截到的
                // 那块矩形：请求的矩形没被完整截到时，图照常交付但要说清楚，绝不能默认"这就是
                // 整个窗口"。窗口内容路径不报，等于"没有丢区域"。
                img.reportsCrop = frame.reportsCrop;
                img.requestedRect = frame.requestedRect;
                img.capturedRect = frame.capturedRect;
                img.clipped = frame.clipped;
                img.rotation = frame.rotation;
                if (frame.reportsCrop && frame.clipped) {
                    const RECT& want = frame.requestedRect;
                    const RECT& got = frame.capturedRect;
                    clippedNote = Diagnostic{
                        codes::kCaptureClipped,
                        Msgf(L"note.capture_clipped",
                             static_cast<uint64_t>(want.right - want.left),
                             static_cast<uint64_t>(want.bottom - want.top),
                             static_cast<uint64_t>(got.right - got.left),
                             static_cast<uint64_t>(got.bottom - got.top)),
                        L"--capture", img.source,
                        Msgf(L"note.capture_clipped_hint", want.left - got.left,
                             want.top - got.top, got.right - want.right, got.bottom - want.bottom),
                        t.Tag(), img.source, stages::kCapture};
                }

                // 质量提示，与"这次采集失败"是两件事：整帧逐像素比过之后确实只有一个颜色，
                // 但**单色不等于没截到东西** —— 一扇纯色窗口、一块刚铺好的单色壁纸本来就是这样。
                // 这里只把事实记下来（不改退出码、不丢图、也不升级授权），到这张图真的交出去时才送出。
                // 判据放在这一层而不是各通道内部：哪条通道交回的单色帧都值得让人看见。
                FrameColor uniform{};
                if (FrameIsUniform(frame, &uniform)) {
                    wchar_t argb[16];
                    swprintf(argb, 16, L"0x%02X%02X%02X%02X", uniform.a, uniform.r, uniform.g,
                             uniform.b);
                    uniformNote = Diagnostic{codes::kFrameUniform,
                                             Msgf(L"note.frame_uniform", std::wstring(argb)),
                                             L"--capture", img.source,
                                             Msg(L"note.frame_uniform_hint"), t.Tag(), img.source,
                                             stages::kCapture};
                }
            }

            const wchar_t* backend = img.source.empty() ? CaptureMethodName(opt.capture)
                                                        : img.source.c_str();
            if (ok) {
                stage = stages::kEncode;
                ok = CallBackend(stage, backend,
                                 [&] {
                                     return EncodeFrame(frame, opt.format, opt.jpegQuality, dl,
                                                        &encoded, &targetErr);
                                 },
                                 &targetErr, &fatal);
            }

            // 提交这一步（写文件 / 写标准输出）同样不许重新领一份预算：预算已经用尽就在这里
            // 停下，帧被丢掉而不落地。已经取到的像素在磁盘写坏之前丢弃，比"先写了再说"干净。
            // 这一关是**开工之前**的判定：真开始写之后，磁盘写与被人堵住的管道都没有中断点，
            // 期限对它们只能在完工之后核对 —— 这条边界写在 README 与 AGENTS.md 里。
            if (ok && dl.Spent()) {
                const bool toStdout = img.file == L"-";
                targetErr = BudgetSpent(dl, codes::kIoTimeout,
                                        toStdout ? stages::kStdout : stages::kWrite, backend);
                targetErr.option = L"--out";
                targetErr.value = img.file;
                ok = false;
                stage = toStdout ? stages::kStdout : stages::kWrite;
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

        if (recorded) {   // stdout 那条路已经入过列
            if (clippedNote) outcome.notes.push_back(std::move(*clippedNote));
            if (uniformNote) outcome.notes.push_back(std::move(*uniformNote));
            continue;
        }
        // 到这里这一张是真交出去了（文件已提交，或字节已达标准输出），质量提示这时才有意义
        if (clippedNote) outcome.notes.push_back(std::move(*clippedNote));
        if (uniformNote) outcome.notes.push_back(std::move(*uniformNote));
        img.bytes = encoded.size();
        img.elapsedMs = static_cast<uint32_t>(GetTickCount64() - started);
        outcome.images.push_back(std::move(img));
    }

    if (outcome.images.empty()) {
        const std::wstring& code =
            outcome.errors.empty() ? std::wstring() : outcome.errors.front().code;
        outcome.exitCode = code == codes::kAccessDenied || code == codes::kConsentUnavailable ||
                             code == codes::kConsentTimeout
                         ? EX_DENIED
                         : code == codes::kWriteFailed || code == codes::kFileExists ||
                                 code == codes::kIoTimeout
                             ? EX_IO_FAILED
                             : EX_CAPTURE_FAILED;
    } else {
        // 部分成功：图片已写出，但有目标失败 -> 用截图失败码提示调用方看 errors
        outcome.exitCode = outcome.errors.empty() ? EX_OK : EX_CAPTURE_FAILED;
    }
    return outcome;
}

}  // namespace ecapture
