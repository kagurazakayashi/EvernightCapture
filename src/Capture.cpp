#include "Capture.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <exception>
#include <optional>
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
#include "CropGeometry.h"
#include "CursorControl.h"   // 光标能力的闸门与 requested/effective/basis 那三个键的合成（判据在那个头文件）
#include "Deadline.h"
#include "Delivery.h"   // 交付那一步的接缝与记账（开工前判预算、完工后核预算，两件事分开）
#include "Encoder.h"
#include "FallbackChain.h"   // auto 那条回退链本体与"换后端有没有意义"的判据（离线判据注入假后端）
#include "FileSave.h"
#include "HistoryArchive.h"   // 主交付之外那一份历史副本（命名、独占提交与失败归类都在那一层）
#include "ImageOps.h"
#include "Lang.h"
#include "OutputPlan.h"
#include "Report.h"
#include "ScreenIdentity.h"   // 屏幕目标的选定与复核（身份只在这里判一次，见那个头文件）
#include "ScreenMatch.h"
#include "SystemCompat.h"
#include "WindowIdentity.h"
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
    // 选定那一刻的身份快照 + 复核要用的查询层。窗口目标在每一次真正读像素之前都要照它复核
    //（WindowIdentity.h）；屏幕目标没有窗口身份，那边靠 ScreenIdentity.h 的
    // CompareScreenIdentity（弹框前）与 RecheckScreenSampling（拿到桌面凭证之后、采样之前）
    // 重新核对那块屏，win 在这里留空、也永远不会被用到。
    WindowTarget win;
    ScreenInfo screen;
    // 选定那一刻问到的屏幕身份（按标识选屏那一路才有答案）。取帧之前的复核靠它分辨
    // "还是那块屏"与"这个设备名已经发给另一块面板了"—— 只看名字的话后者看不出来。
    ScreenFacts screenFacts;
    RECT area{};  // 授权与 JSON 都用它：窗口 = 整窗外框矩形，屏幕 = 该屏矩形

    // %n 用的名字
    std::wstring Name() const { return isScreen ? ScreenDisplayName(screen) : window.title; }
    // 诊断里标识"是哪个目标"：与 images[].hwnd / images[].device 同源，调用方能对上号
    std::wstring Tag() const {
        return isScreen ? ScreenDisplayName(screen) : HwndHexOf(window.hwnd);
    }
    // 给人看的那一行（弹框与 hint 用）：只有身份那一段，区域由授权判定器在弹框那一刻
    // 与它冻结的区域一起渲染 —— 文案与实际批准的范围不会有先后之差。
    std::wstring Describe() const {
        return isScreen ? DescribeScreen(screen) : DescribeWindow(window);
    }
    // 屏幕目标的"身份 + 当下值"一份快照：整屏链路的采样入口复核拿它比对（见 ScreenIdentity.h）。
    ScreenCandidate Candidate() const { return ScreenCandidate{screen, screenFacts}; }
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

// 单个通道的取帧入口。做两件事才放行，顺序不能反：
//   1. 身份复核（kCheap 那一档）—— 这个句柄现在还是不是当初选中的那一扇窗口。
//   2. 授权判定 —— 窗口内容路径：--yes 免问，否则整批问一次；
//      桌面路径：永远问人，并换来那张凭证，没有它就调不动那条通道的取像素函数。
//
// 身份复核在这里做**两次**，理由各不相同：
//   * 授权之前那一次：目标已经没了或已经换人，就不该再拿"要不要截它"去打扰人 ——
//     人看到的确认清单是选目标那一刻算出来的，那份清单已经不成立了。
//   * 授权之后、取帧之前那一次：确认框可能在屏幕上停了几秒，而点"是"之后还有约 1 秒
//     关闭动画（那一段睡在 DialogConsentPrompt 里）。这段时间足够目标被销毁、
//     而它的 HWND 被另一扇窗口拿走。用户批准的是旧对象，许可不转移给新对象。
// 两次都是 kCheap：这一档那四问全都不往目标线程发消息（答案在 user32 / kernel32 自己那份
// 结构里），所以 auto 回退链把它乘四遍也不花钱、更不会在这里卡住。
// "当初那条选择条件现在还成立吗"（标题、按屏过滤那类易变属性）是 kFull，由 RunCapture
// 在每个目标开工之前问一次 —— 那一问要重跑条件求值，按回退链的次数乘上去就是平白多几倍枚举。
//
// timeoutMs 是"这一步自己愿意等多久"（等帧、泵消息），dl 是"这一次运行还剩多少预算"。
// 每条通道真正等下去的时长都是两者里小的那个 —— 预算不被任何一条通道重新领一份。
bool CaptureOneChannel(ConsentGate& gate, const std::wstring& targetKey, const RECT& area,
                       const WindowTarget& win, CaptureMethod method, uint32_t timeoutMs,
                       const Deadline& dl, const CursorRequest& cursor, const HdrRequest& hdr,
                       CapturedFrame* out, Diagnostic* err) {
    if (!win.Recheck(IdentityScope::kCheap, err)) return false;

    const wchar_t* path = WindowPathOf(method);
    AttemptAuth auth = AuthorizeAttempt(gate, path, targetKey, area, err);
    if (!auth.ok) {
        // 一帧都不去取。诊断里补上"是哪条通道要去的"：路径名在 value，通道名在 backend。
        if (err && err->backend.empty()) err->backend = CaptureMethodName(method);
        return false;
    }
    // 授权之后再过一次身份：上面那次判定与人点头之间隔着的这段时间不能当它不存在。
    if (!win.Recheck(IdentityScope::kCheap, err)) return false;

    const uint32_t wait = dl.ClampWait(timeoutMs);
    const uint64_t hwnd = win.id.hwnd;

    switch (method) {
        case CaptureMethod::kWgc:
            // 光标这件事只有这一条通道有一个真设得进去、也读得回来的开关，所以要求原样交给它；
            // 兑现不了时它在开始采集**之前**就停下（capture.cursor_unverifiable）。
            // HDR 那一条同通道也只有它有广色域帧池可建，与桌面凭证无关，照实交给它。
            return CaptureWindowWgc(hwnd, wait, cursor, hdr, dl, out, err);
        case CaptureMethod::kDwmThumbnail:
            // 它自己会在内部升级到桌面路径时回来重新要一次许可，所以把判定器传进去；
            // 身份也一并交给它 —— 那条退路读的是桌面像素，升级之前要按 kFull 再复核一次。
            // dwm.thumbnail 结构上只带得回 8 位重定向位图，没有 HDR 可映射，所以 hdr 不传进它。
            return CaptureWindowDwmThumbnail(hwnd, wait, gate, targetKey, win, dl, out, err);
        case CaptureMethod::kPrintWindow:
            // 这条一律走辅助进程：PrintWindow 同步等目标窗口的线程，本进程里没有中断点
            return CaptureWindowPrintWindow(hwnd, dl, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureWindowBitBlt(hwnd, wait, *auth.permit, out, err);
        case CaptureMethod::kDuplication:
            // hdr 照旧交给它，但**只有没要求过处理的那一路真会用到**：显式 tonemap / refuse 时
            // 这条通道在闸门（以及解析期那一关）就被判成"本构建兑现不了"，不会成为候选。
            // 它自己也不问那块屏此刻的色彩空间，所以交回的 8 位帧不声称来源是 SDR。
            return CaptureWindowDuplication(hwnd, wait, *auth.permit, hdr, dl, out, err);
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
// wanted 是授权快照那一刻的身份 + 当下值：授权之后、读像素之前按它再核一次那块屏还在不在、
// 还是不是人批准时的样子（RecheckScreenSampling），采样对象换成复核答复里的**当下**候选 ——
// 热插拔之后旧 HMONITOR 句柄可能已经不作废地指向别的面板，拿选定那一刻的句柄直接采是错的。
bool CaptureScreenOneChannel(ConsentGate& gate, const std::wstring& targetKey,
                             const ScreenCandidate& wanted, CaptureMethod method,
                             uint32_t timeoutMs, const Deadline& dl, const CursorRequest& cursor,
                             const HdrRequest& hdr, CapturedFrame* out, Diagnostic* err) {
    const wchar_t* path = ScreenPathOf(method);
    AttemptAuth auth = AuthorizeAttempt(gate, path, targetKey, wanted.screen.bounds, err);
    if (!auth.ok) {
        if (err && err->backend.empty()) err->backend = CaptureMethodName(method);
        return false;   // 没通过授权：整块屏幕一个像素都不读
    }

    // 凭证到手 != 那块屏还是人点头时看到的那块屏。采样入口这一问用当下的枚举结果回答：
    // 没了 / 问不出身份 / 换了样子 / 当下矩形越出批准区域 —— 都停在这里，一个像素不读，
    // 也不替它挑另一块屏、不把旧授权追认到新布局上（诊断沿用既有稳定码）。
    ScreenCandidate live{};
    const SamplingRecheck rc =
        RecheckScreenSampling(wanted, auth.permit->Approved(),
                              EnumScreenCandidates(wanted.facts.hasFacts), &live);
    if (rc != SamplingRecheck::kOk) {
        const wchar_t* backend = CaptureMethodName(method);
        if (rc == SamplingRecheck::kStale) {
            CaptureError(err, backend, Msg(L"cap.consent.stale"), Msg(L"cap.consent.stale_hint"),
                         codes::kConsentStale);
        } else if (rc == SamplingRecheck::kGone) {
            Diagnostic d{codes::kMonitorChanged, Msg(L"cap.monitor_changed"), L"--monitor",
                         wanted.screen.deviceName, Msg(L"cap.monitor_changed_hint"), targetKey,
                         backend, stages::kCapture};
            if (err) *err = std::move(d);
        } else {
            Diagnostic d{codes::kMonitorUnverifiable, Msg(L"cap.monitor_unverifiable"), L"--monitor",
                         wanted.screen.deviceName, Msg(L"cap.monitor_unverifiable_hint"), targetKey,
                         backend, stages::kCapture};
            if (err) *err = std::move(d);
        }
        return false;
    }

    const uint32_t wait = dl.ClampWait(timeoutMs);
    const ScreenInfo& screen = live.screen;   // 采样对象 = 当下问到的那块屏（句柄与矩形都是）

    switch (method) {
        case CaptureMethod::kWgc:
            // 整屏那条与窗口那条共用同一条会话接口，所以光标开关同样设得进去；
            // 广色域帧池也同一条接口，hdr 一并交给它（屏就是那块 HMONITOR）。
            return CaptureScreenWgc(screen, wait, cursor, hdr, dl, *auth.permit, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureScreenBitBlt(screen, wait, *auth.permit, out, err);
        case CaptureMethod::kDuplication:
            return CaptureScreenDuplication(screen, wait, *auth.permit, hdr, dl, out, err);
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

// auto 的回退链本体在 src/FallbackChain.h（连同"这一关换后端有没有意义"那一条判据）：
// 提到那里是为了让离线判据能注入假后端逐条判"策略结论之后其余后端一次都不调用"，
// 而不是只在注释里声称。这里留下的两件事是它消费的链（已经过三道闸门筛好）与异常边界。

// --capture 分派。chain 是"这一次真正可以试的通道"（已经过通道闸门按本机 Windows 版本、
// 这一次的光标要求与显式要过的 HDR 处理要求筛过；见 SystemCompat.h 与 src/CursorControl.h、
// src/HdrColor.h）：显式指定一条时它就只有那一条，auto 时是回退链减去被挡掉的那几条。
// 显式指定的那条绝不回退：用户要哪个就要哪个。
// 交给这里的不是裸句柄，而是选定那一刻的快照与查询层（WindowTarget）—— 每一次尝试之前
// 都要照它复核一遍，所以通道手里没有"跳过复核直接取像素"的那条路可走。
bool CaptureWithMethod(ConsentGate& gate, const std::wstring& targetKey, const RECT& area,
                       const WindowTarget& win, CaptureMethod method,
                       const std::vector<CaptureMethod>& chain, uint32_t timeoutMs,
                       const Deadline& dl, const CursorRequest& cursor, const HdrRequest& hdr,
                       CapturedFrame* out, Diagnostic* err, std::vector<Diagnostic>* notes,
                       bool* fatal) {
    if (method != CaptureMethod::kAuto) {
        return CallBackend(stages::kCapture, CaptureMethodName(method),
                           [&] {
                               return CaptureOneChannel(gate, targetKey, area, win, method,
                                                        timeoutMs, dl, cursor, hdr, out, err);
                           },
                           err, fatal);
    }
    return FallbackChain(
        chain, dl, out, err, notes, fatal,
        [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e, bool* fat) {
            return CallBackend(
                stages::kCapture, CaptureMethodName(m),
                [&] {
                    return CaptureOneChannel(gate, targetKey, area, win, m, timeoutMs, dl, cursor,
                                             hdr, frame, e);
                },
                e, fat);
        });
}

bool CaptureScreenWithMethod(ConsentGate& gate, const std::wstring& targetKey,
                             const ScreenCandidate& wanted, CaptureMethod method,
                             const std::vector<CaptureMethod>& chain, uint32_t timeoutMs,
                             const Deadline& dl, const CursorRequest& cursor, const HdrRequest& hdr,
                             CapturedFrame* out, Diagnostic* err, std::vector<Diagnostic>* notes,
                             bool* fatal) {
    if (method != CaptureMethod::kAuto) {
        return CallBackend(stages::kCapture, CaptureMethodName(method),
                           [&] {
                               return CaptureScreenOneChannel(gate, targetKey, wanted, method,
                                                              timeoutMs, dl, cursor, hdr, out,
                                                              err);
                           },
                           err, fatal);
    }
    return FallbackChain(
        chain, dl, out, err, notes, fatal,
        [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e, bool* fat) {
            return CallBackend(
                stages::kCapture, CaptureMethodName(m),
                [&] {
                    return CaptureScreenOneChannel(gate, targetKey, wanted, m, timeoutMs, dl,
                                                   cursor, hdr, frame, e);
                },
                e, fat);
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

// 一条矩形边的长度，非正数一律当 0（量不出来的矩形就是"什么都没有"，不是"负数个像素"）。
uint32_t SpanOf(LONG from, LONG to) {
    return to > from ? static_cast<uint32_t>(static_cast<int64_t>(to) - from) : 0u;
}

// 这次裁剪没成时的诊断。两条码分开给：「放不下」与「那一问没答案」的下一步不同
//（前者照当下的尺寸改请求，后者换一条窗口内容通道或整窗重取），与 capture.target_unverifiable
// 和 capture.target_changed 分家同一道理。ASCII 的原因名（CropFailName）写进 message 末尾，
// 因为 message 会随 --lang 变，而调用方排障要能拿到一条不变的标识。
Diagnostic CropDiagnostic(const CropRequest& request, const CropResolution& res,
                          const wchar_t* backend) {
    const bool unmeasurable = res.fail == CropFail::kClientUnmeasurable ||
                              res.fail == CropFail::kImageUnmeasurable;
    const std::wstring failName = [&]() {
        std::wstring s;
        for (const char* p = CropFailName(res.fail); p && *p; ++p)
            s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        return s;
    }();
    Diagnostic d;
    d.code = unmeasurable ? codes::kRoiUnmeasurable : codes::kRoiInvalid;
    d.message = unmeasurable
                    ? Msgf(L"cap.roi_unmeasurable", static_cast<uint64_t>(res.imageWidth),
                           static_cast<uint64_t>(res.imageHeight), failName)
                    : Msgf(L"cap.roi_invalid", static_cast<uint64_t>(res.imageWidth),
                           static_cast<uint64_t>(res.imageHeight), res.requestRight,
                           res.requestBottom, failName);
    d.option = request.mode == CropMode::kClientArea ? L"--client-area" : L"--roi";
    d.value = request.mode == CropMode::kClientArea ? std::wstring() : request.written;
    d.hint = Msg(unmeasurable ? L"cap.roi_unmeasurable_hint" : L"cap.roi_invalid_hint");
    d.backend = backend ? std::wstring(backend) : std::wstring();
    d.stage = stages::kCapture;
    return d;
}

// 把一次裁剪请求落到已经拿到手的那张整窗图像上。判据全在 src/CropGeometry.cpp（纯算术，
// 离线逐条注入），这里只负责**量出那三件事**再交给它：
//   1. 交付图像的尺寸（就是这张帧的 width / height，物理像素）；
//   2. 这块图像对应虚拟屏幕坐标里的哪一块 —— 能不能核实。桌面裁切那几条自己就把实际截到的
//      那块矩形报在 capturedRect 里，那是事实；窗口内容那几条只能拿"此刻量到的可见矩形尺寸
//      与交付尺寸完全相同"这一条证据承认，尺寸不同就不承认（宁可少写一个字段）。
//   3. --client-area 要的：客户区在屏幕坐标里的那一块，问得到问不到。
// 判不下来就是**这一张不落地**：既不"往里挪一挪"，也不"那就整窗交出"。
// 裁剪发生在取帧之后，所以它一丝一毫都没有改变授权判断 —— 确认框上列出的是整个目标区域，
// 会读桌面像素的那几条照样一定问人，--yes 也不会因为"最后只留一小块"而开始生效。
bool ApplyWindowCrop(const Options& opt, const Target& t, CapturedFrame* frame,
                     CapturedImage* img, std::optional<Diagnostic>* mappingNote, Diagnostic* err) {
    if (opt.crop.mode == CropMode::kNone) return true;
    const HWND hwnd = reinterpret_cast<HWND>(t.window.hwnd);
    const RECT visible = WindowScreenRect(hwnd);
    const DeliveredImage image = DescribeDeliveredImage(
        frame->width, frame->height, visible,
        visible.right > visible.left && visible.bottom > visible.top, frame->capturedRect,
        frame->reportsCrop);

    ClientAreaProbe client;
    if (opt.crop.mode == CropMode::kClientArea) {
        RECT area{};
        client.readable = ClientScreenRect(hwnd, &area);
        client.screenRect = area;
    }

    const CropResolution res = ResolveWindowCrop(opt.crop, image, client);
    // backend 写的是**真实出了这一帧的那条通道**（--capture auto 回退之后与请求值不同），
    // 所以读帧里那份来源，而不是 opt.capture 那个请求值。
    const wchar_t* backend =
        frame->source.empty() ? CaptureMethodName(opt.capture) : frame->source.c_str();
    if (res.status != CropStatus::kCropped) {
        *err = CropDiagnostic(opt.crop, res, backend);
        return false;
    }

    const uint32_t widthBefore = frame->width;
    const uint32_t heightBefore = frame->height;
    if (!CropFrame(frame, res.crop.x, res.crop.y, res.crop.width, res.crop.height)) {
        // 判据说过放得下、裁不下来：唯一剩下的可能是这一帧的内存形状自己说不通（那是
        // capture.frame_invalid 那条线的职责，退出码同为 7），所以这里照实报那条码而不硬交半张图。
        CaptureError(err, backend, Msg(L"cap.crop_apply_failed"),
                     Msg(L"cap.crop_apply_failed_hint"), codes::kFrameInvalid);
        return false;
    }

    img->cropped = true;
    img->cropMode = CropModeName(opt.crop.mode);
    img->crop = res.crop;
    img->fullWidth = widthBefore;
    img->fullHeight = heightBefore;
    img->hasCropScreen = res.hasCropScreen;
    img->cropScreen = res.cropScreen;
    // 图像原点核实不出来：裁剪本身是确定的（它只需要图像尺寸），但 cropScreenRect 那一行交不出
    // 来 —— 这是一个"少了一个定位字段"的事实，要让人看见，所以留一条提示（--quiet 可抑制，
    // 但缺失的那个键本身就是同一个判据）。
    if (!res.hasCropScreen && mappingNote) {
        *mappingNote = Diagnostic{codes::kCropMappingUnavailable,
                                  Msgf(L"note.crop_mapping_unavailable",
                                       static_cast<uint64_t>(widthBefore),
                                       static_cast<uint64_t>(heightBefore)),
                                  opt.crop.mode == CropMode::kClientArea ? L"--client-area"
                                                                         : L"--roi",
                                  opt.crop.written, Msg(L"note.crop_mapping_unavailable_hint"),
                                  t.Tag(), std::wstring(backend), stages::kCapture};
    }
    return true;
}

// 生产用的那一个出口：真文件（同目录临时文件 + 原子改名，见 FileSave.h）与真标准输出。
// 这里不加任何判断与重试 —— 交付这件事的判据只有一份，写在 src/Delivery.h；这一层只是
// 把那两个调用接上，异常照旧往外飞，由每个目标那一层边界接住。
class RealOutputSink final : public OutputSink {
public:
    bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                  Diagnostic* err) override {
        return SaveFileAtomic(path, bytes, overwrite, err);
    }
    bool EmitBytes(const std::vector<uint8_t>& bytes, uint64_t* emitted,
                   DWORD* ioError) override {
        return EmitStdoutBytes(bytes, ioError, emitted);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 目标选择：屏幕目标直接按 --monitor 取；窗口目标先做条件求值，再按选择策略消歧。
//
// 条件求值这一步有两条执行路线，判据不是用户有没有要求，而是"这一步有没有中断点"：
//   * 用了 --title-regex —— std::regex 的编译与回溯匹配都没有可查的中断点，
//     而"限制模式串长度"根本不是执行期限（短串一样能爆炸性回溯）。所以编译与匹配整步
//     放进辅助进程，到点就结束那个进程（Worker.h）；解析层不碰正则库，语法不合照样在这
//     一步之前判出 cli.invalid_regex（stage=match），不取帧、不弹框。
//   * 设了 --timeout-ms —— 连"给每个顶层窗口取标题"都可能被一扇挂住的窗口拖住
//     （GetWindowText 是往那个线程发消息并等它回），所以整步也放进辅助进程。
//   * 两者都没有 —— 照旧在本进程枚举，与没有期限机制时的行为完全一致。
// 消歧（SelectFromHits）只对着已经拿到手的列表做决定，永远在本进程跑。
// ---------------------------------------------------------------------------
namespace {

// 一次窗口目标选择的结果，连同"当初是怎么选的"。
// 那一份来路必须一起交出去：身份复核里"kFull 那一问"的答案是**拿同一份条件重新求值一次**，
// 看这个句柄还在不在命中列表里（WindowIdentity.h）。只把选中的窗口交出去就没法重问。
struct WindowSelection {
    std::vector<WindowInfo> picked;
    MatchRequest request;      // 当初那份条件 + 按屏过滤的矩形
    bool isolate = false;      // 当初那一步是不是整半交给辅助进程
};

WindowSelection PickWindows(const Options& opt, const Deadline& dl,
                            std::vector<Diagnostic>* errors) {
    WindowSelection sel;
    std::vector<RECT> onScreens;
    if (opt.monitor.given) {
        onScreens = SelectedScreenRects(opt, errors);
        if (!errors->empty()) return sel;
    }
    sel.request.match = opt.match;
    sel.request.onScreens = onScreens;
    sel.isolate = !opt.match.titleRegexes.empty() || dl.Enabled();

    std::vector<WindowInfo> hits;
    std::vector<WindowInfo> iconic;
    if (sel.isolate) {
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
            return sel;
        }
    } else {
        const MatchOutcome m = EnumerateMatches(sel.request);
        if (m.status != BlockedStatus::kOk) {
            errors->push_back(
                BlockedToDiagnostic(m.status, 0, S_OK, m.detail, L"match", stages::kMatch));
            return sel;
        }
        hits = std::move(m.hits);
        iconic = std::move(m.iconic);
    }
    sel.picked = SelectFromHits(opt, hits, iconic, MonitorLabelOf(opt), errors);
    return sel;
}

// kFull 那一问的查询层：拿当初那份条件**重新求值一次**，看这个句柄还在不在命中列表里。
//
// 为什么用"重跑条件"而不是"逐字比标题"：应用刷新标题是正常现象（播放进度、文档修改标记、
// 标签页标题），逐字比较会把每一次正常刷新都判成"换了目标"；而条件不再成立，才说明当初把它
// 挑出来的那条理由已经不属于它了。同理，--monitor 那种"按屏过滤"也在这一问里 —— 窗口挪到
// 别的屏上，就是不再满足当初那个条件。
//
// 为什么这一问不进 kCheap：它要枚举一遍全部顶层窗口，还可能给每个窗口取一次标题 ——
// 那是整条链里最贵、也最可能被挂住的窗口拖住的一步。所以它只在每个目标开工之前跑一次
//（外加 dwm 要升级到桌面像素之前那一次），并且**沿用第一次求值那同一条 isolate 判据**：
// 用了 --title-regex 或设了预算就照旧进辅助进程、到点能结束，绝不因为要复核就在父进程里
// 新造一个没有中断点的等待（Worker.h）。
WindowQueryLayer MakeTargetQuery(const MatchRequest& request, bool isolate, const Deadline& dl) {
    WindowQueryLayer q = SystemWindowQueryLayer();
    q.selectionStillMatches = [request, isolate, dl](uint64_t hwnd, bool* matched) -> bool {
        std::vector<WindowInfo> hits;
        std::vector<WindowInfo> iconic;
        if (isolate) {
            Diagnostic err;
            if (!IsolatedMatch(request.match, request.onScreens, dl, &hits, &iconic, &err)) {
                return false;   // 问不出来：期限到了 / 辅助进程坏了，调用方按无法验证处理
            }
        } else {
            const MatchOutcome m = EnumerateMatches(request);
            if (m.status != BlockedStatus::kOk) return false;
            hits = m.hits;
        }
        *matched = std::any_of(hits.begin(), hits.end(),
                               [&](const WindowInfo& w) { return w.hwnd == hwnd; });
        return true;
    };
    return q;
}

}  // namespace

CaptureOutcome RunCapture(const Options& opt) {
    CaptureOutcome outcome;

    // 屏幕矩形必须与物理像素一致，否则 GDI 通道会截偏
    EnsureDpiAware();

    // 运行环境这一关开在枚举窗口、规划输出名、弹确认框、读像素之前：
    // 这一台机器上的 Windows 版本提供不了所要求的东西时，后面每一步都只是白做功，而最要紧的是
    // **别去打扰人** —— 人在确认框上点"是"之后才知道根本截不出来，是这条链最坏的失败形状。
    // 判据与那三条下限各是什么见 src/SystemCompat.h。
    // --dry-run 不取帧，所以这一关不替它下结论（它那条"这次会挑到哪几条通道"的答案在 -v 的
    // input.captureChain 与 input.osBuild 里，问能力不必等到要截图的时候）。
    const OsVersion os = ProbeOsVersion();
    // 三条闸门串成一份（GateChannels 按本机版本筛，FilterChainForCursor 按这次的光标要求筛，
    // FilterChainForHdr 按这次显式要过的 HDR 处理要求筛）：--capture auto 时兑现不了的那几条
    // 在这里就摘掉并各留一条 note，显式指定的那条做不到时一条错误、链为空 ——
    // **绝不**替用户换成另一条通道（尤其不会换成会读桌面像素的那几条）。
    const ChannelGate caps =
        GateCaptureChain(opt.capture, opt.ScreenMode(), os, opt.cursor, opt.hdr);
    if (!opt.dryRun) {
        if (!caps.error.code.empty()) {
            outcome.errors.push_back(caps.error);
            outcome.exitCode = EX_CAPTURE_FAILED;
            return outcome;
        }
        for (const Diagnostic& n : caps.notes) outcome.notes.push_back(n);
    }

    // 整条自动处理链路共用这一份预算：目标选择、后端重试、等帧、编码、提交。
    // 人工确认那一段不计在这里（见 GateConfig.consentTimeoutMs 与 --consent-timeout-ms）。
    const Deadline dl = Deadline::FromTotalMs(opt.timeoutMs);

    std::vector<Target> targets;
    if (opt.ScreenMode()) {
        // 屏幕目标连身份一起选定：按标识选屏那一路问到的那条跨会话标识，下面每一次取帧之前
        // 都要拿它再核一次（只按设备名核的话，"名字被系统重新发给另一块面板"这种现场看不出来）。
        for (const ScreenCandidate& c : SelectScreenCandidatesOf(opt.monitor, &outcome.errors)) {
            Target t;
            t.isScreen = true;
            t.screen = c.screen;
            t.screenFacts = c.facts;
            t.area = c.screen.bounds;
            targets.push_back(std::move(t));
        }
    } else {
        const WindowSelection sel = PickWindows(opt, dl, &outcome.errors);
        // 查询层对本次全部目标共用一份：它带着"当初那份条件"与同一份预算，
        // 复核时拿它重新求值一次（MakeTargetQuery 上面写了为什么这么做）。
        WindowQueryLayer query;
        if (outcome.errors.empty()) {
            query = MakeTargetQuery(sel.request, sel.isolate, dl);
        }
        for (const auto& w : sel.picked) {
            Target t;
            t.window = w;
            // 选定那一刻就把身份记下来。之后每一次真正取帧之前都要照这份快照复核一遍，
            // 因为"这还是那一扇窗口吗"必须拿**当时**的值来比 —— 事后补问等于自己跟自己对答案。
            t.win.id = MakeWindowIdentity(w, opt.match, opt.monitor.given);
            t.win.query = query;
            // 窗口目标用整窗外框当授权范围：桌面路径实际会从屏幕上读走的就是这一块
            // （DWM 那条退路摆的覆盖窗口也按它对齐），比可见边框矩形更宽一点而不是更窄。
            t.area = WindowFullRect(reinterpret_cast<HWND>(w.hwnd));
            targets.push_back(std::move(t));
        }
    }
    if (!outcome.errors.empty()) {
        // 与窗口查询那一份同一张映射表（WindowQueryExitCodeFor 里也是这几条码这几个数）：
        // "这个屏幕标识现在不在桌面上"与"没有窗口命中这批条件"同为 4，
        // "同一个标识命中多块屏"与"多扇窗口没消歧"同为 5，"这一问没答案"归 7。
        // 三条都不许被折叠成"那就用主屏吧"—— 那等于替用户挑一块他没点名的屏。
        const std::wstring& code = outcome.errors.front().code;
        outcome.exitCode = code == codes::kAmbiguousWindow || code == codes::kMonitorAmbiguousId
                             ? EX_AMBIGUOUS
                         : code == codes::kIndexOutOfRange || code == codes::kMonitorOutOfRange ||
                                 code == codes::kInvalidRegex
                             ? EX_USAGE
                         : code == codes::kMatchTimeout || code == codes::kCaptureTimeout ||
                                 code == codes::kMonitorIdUnverifiable
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

    // 取帧之前先把 --roi 核一遍（--client-area 是照目标自己的几何算的，没有"用户写越界"
    // 这一种形状，所以它只在取到帧之后由 ApplyWindowCrop 判）。这条线在输出名规划与确认框
    // **之前**，两个理由：一条注定裁不出来的请求不该先去打扰人一次，也不该留下半批已经
    // 写好的文件。判据用的是当初准备交付的那块可见矩形 —— 裁剪坐标就锚在它的左上角，
    // 所以落在它之内的 ROI 必然也落在人批准过的那块目标之内；越界的照实拒绝，
    // 既不"那就裁到边上为止"，也**绝不**当成桌面绝对坐标去截别的位置。
    // 一批里有一扇窗口放不下就整批不开工（与 match.index_out_of_range 同一条规矩：
    // 不放宽条件去凑，也不交付"那一扇我没法裁所以干脆不裁"的混和结果）。
    if (opt.crop.mode == CropMode::kRoi) {
        for (const Target& t : targets) {
            if (t.isScreen) continue;   // 屏幕模式在解析期就被 capture.unsupported 挡掉了
            const RECT visible = WindowScreenRect(reinterpret_cast<HWND>(t.window.hwnd));
            if (RoiFitsTarget(opt.crop, SpanOf(visible.left, visible.right),
                              SpanOf(visible.top, visible.bottom))) {
                continue;
            }
            Diagnostic d{codes::kRoiOutOfRange,
                         Msgf(L"match.roi_out_of_range", opt.crop.written,
                              static_cast<uint64_t>(opt.crop.x) + opt.crop.width,
                              static_cast<uint64_t>(opt.crop.y) + opt.crop.height,
                              static_cast<uint64_t>(SpanOf(visible.left, visible.right)),
                              static_cast<uint64_t>(SpanOf(visible.top, visible.bottom))),
                         L"--roi", opt.crop.written, Msg(L"match.roi_out_of_range_hint"), t.Tag(),
                         std::wstring(), stages::kMatch};
            outcome.errors.push_back(std::move(d));
            outcome.exitCode = EX_USAGE;
            return outcome;
        }
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
    // 历史归档的位置在弹框**之前**就解析出来：确认框上要写"程序自己那份目录里还会另存一份持久
    // 副本"，而那一句只有在这一次真能走到归档根时才说得出（解析不出来的那一路不承诺副本）。
    // 这一步只问模块位置与那个位置此刻是什么属性，一个目录都不建、一个字节都不写。
    HistoryArchive historyArchive;

    DialogConsentPrompt prompt(opt.consentTimeoutMs);
    GateConfig gateCfg;
    gateCfg.yes = opt.yes;
    gateCfg.captureLabel = CaptureMethodName(opt.capture);
    gateCfg.consentTimeoutMs = opt.consentTimeoutMs;
    gateCfg.historyCopy = historyArchive.root().usable;
    // 真人等待从上面那份整批自动预算里暂停出去（判定器只在真要弹框时套上暂停作用域；
    // --consent-timeout-ms 有自己的秒表，不跟着冻结）。dl 比判定器活得久，这里只借指针。
    gateCfg.autoBudget = &dl;
    for (const Target& t : targets) {
        GateTarget gt;
        gt.screen = t.isScreen;
        gt.key = t.Tag();
        gt.area = t.area;
        // 只交身份那一段：整行（含区域坐标）由判定器在弹框那一刻从 (subject, area) 现渲染，
        // SetTargetArea 刷新区域后文案自动同步，不会出现"框上还写着旧坐标"的平行文本。
        gt.subject = t.Describe();
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

    // 交付那一步的几件事：整批共用的那一份预算（不是一个目标一份）、真的出口（文件 / 标准输出）、
    // 量 elapsedMs 的那把时钟，以及主交付之外那一份历史副本（命名、独占提交与失败归类都在
    // src/HistoryArchive.h 那一层，六条后端一条都不必知道历史的存在）。时钟必须与下面每个目标
    // 开工时记的 started 同一把，否则 images[].elapsedMs 会跟着预算那把 QPC 一起漂；预算本身仍是
    // Deadline 里那份，不在这里另领 —— 归档也不另领（预算已尽时它记成 skipped 而不是重来一次）。
    RealOutputSink sink;
    const DeliveryRun delivery{&dl, &sink, [] { return GetTickCount64(); }, &historyArchive,
                               opt.format};

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
        // 每个目标开工之前一次 kFull 复核：连"当初那条选择条件现在还成立吗"一起问
        //（标题、按屏过滤那类易变属性）。这一档要重跑条件求值，所以一个目标一次，
        // 不在每条通道的每一次尝试之前重复；那几处用的是 kCheap（CaptureOneChannel）。
        // 屏幕目标没有窗口身份，它那一侧的核对是下面的 CompareScreen。
        if (!t.isScreen) {
            Diagnostic idErr;
            if (!t.win.Recheck(IdentityScope::kFull, &idErr)) {
                outcome.errors.push_back(std::move(idErr));
                continue;   // 这一张一个像素都不读，也不替它另找一个"看起来一样"的目标
            }
        }
        // 屏幕目标另外要按**身份**重新核对一次：编号只是枚举位置，而设备名是系统按连接顺序发
        // 出去的 —— 当初问得到那条跨会话标识（devnode 路径）就按它核，名字被重新发给另一块面板
        // 时只有这条发现得了；当初问不到就照旧按名字核，不新增失败。
        // 那块屏拔掉了就一个像素都不读；改了分辨率或位置就换成新矩形交给判定器 —— 这一问定下
        // 弹框快照的基线。弹框之后到采样之前再变的，由判定器的确认后拓扑复核与采样入口的
        // RecheckScreenSampling 拦下（旧的不再追认，新的必须重新给人看过）。
        if (t.isScreen) {
            ScreenCandidate fresh{};
            ScreenCandidate wanted{};
            wanted.screen = t.screen;
            wanted.facts = t.screenFacts;
            const ScreenIdentityCheck check =
                CompareScreenIdentity(wanted, EnumScreenCandidates(wanted.facts.hasFacts), &fresh);
            if (check == ScreenIdentityCheck::kUnverifiable) {
                // 这一次那一路一个标识都没问出来。这时**不**退回按名字截：那个名字可能已经
                // 发给别的面板了，照它截就是一张没人批准过的画面。
                Diagnostic d{codes::kMonitorUnverifiable, Msg(L"cap.monitor_unverifiable"),
                             L"--monitor", t.screen.deviceName,
                             Msg(L"cap.monitor_unverifiable_hint"), t.Tag()};
                d.stage = stages::kCapture;
                outcome.errors.push_back(std::move(d));
                continue;   // 一个像素都不读，也不替它挑另一块屏
            }
            if (check == ScreenIdentityCheck::kGone) {
                Diagnostic d{codes::kMonitorChanged, Msg(L"cap.monitor_changed"),
                             L"--monitor", t.screen.deviceName,
                             Msg(L"cap.monitor_changed_hint"), t.Tag()};
                d.stage = stages::kCapture;
                outcome.errors.push_back(std::move(d));
                continue;   // 这一张不取帧，也不替它挑另一块屏
            }
            if (check == ScreenIdentityCheck::kMoved) {
                t.screen = fresh.screen;
                t.screenFacts = fresh.facts;
                t.area = fresh.screen.bounds;
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
        // 交付那一段的结论与"这一张的账是不是已经在那里记完"（见 src/Delivery.h）。
        DeliveryStep step;
        bool settled = false;
        std::optional<Diagnostic> uniformNote;   // 单色质量提示：等这张图真交出去了再送
        std::optional<Diagnostic> clippedNote;   // 区域丢失提示：同上，没交出去就不提示
        std::optional<Diagnostic> cropMappingNote;   // 屏幕原点核实不出来：同上
        std::optional<Diagnostic> hdrNote;       // 要求过 HDR 处理而这一张按 8 位交付：同上
        std::vector<uint8_t> encoded;
        try {
            CapturedFrame frame;
            ok = CallBackend(stages::kCapture, CaptureMethodName(opt.capture),
                             [&] {
                                 return t.isScreen
                                            ? CaptureScreenWithMethod(gate, t.Tag(), t.Candidate(),
                                                                      opt.capture, caps.chain,
                                                                      kFrameTimeoutMs, dl,
                                                                      opt.cursor, opt.hdr, &frame,
                                                                      &targetErr, &outcome.notes,
                                                                      &fatal)
                                            : CaptureWithMethod(gate, t.Tag(), t.area, t.win,
                                                                opt.capture, caps.chain,
                                                                kFrameTimeoutMs, dl, opt.cursor,
                                                                opt.hdr, &frame,
                                                                &targetErr, &outcome.notes, &fatal);
                             },
                             &targetErr, &fatal);
            // 窗口内部裁剪在质量提示与编码**之前**：交出去的就是裁过的那一块，所以
            // "整幅是不是只有一个颜色"判的也必须是它，而不是裁之前那一整张。
            // 它在授权之后：这一层不改变"这条路径的像素从哪来"，桌面路径照样一定问人。
            if (ok) {
                Diagnostic cropErr;
                if (!ApplyWindowCrop(opt, t, &frame, &img, &cropMappingNote, &cropErr)) {
                    ok = false;
                    targetErr = std::move(cropErr);
                }
            }
            // 等比缩小（--scale）排在裁剪之后、编码与质量提示之前：交出去的就是缩过的那一张，
            // 所以下面"整幅是不是只有一个颜色"判的也是它。它同样在授权之后、在帧形状检查之后：
            // 一帧大到过不了 capture.frame_invalid 的，缩不回来（这一层根本没有机会碰它），
            // --roi 的越界判据也已经按**原图**判过了（缩放在它之后），所以缩放绕不过这两道。
            if (ok && opt.scale.given) {
                const wchar_t* scaleChannel =
                    frame.source.empty() ? CaptureMethodName(opt.capture) : frame.source.c_str();
                const ScaleResolution scaleRes = ResolveScale(opt.scale, frame.width, frame.height);
                if (scaleRes.status == ScaleStatus::kRejected) {
                    ok = false;
                    CaptureError(&targetErr, scaleChannel, Msg(L"cap.scale_apply_failed"),
                                 Msg(L"cap.scale_apply_failed_hint"), codes::kFrameInvalid);
                } else {
                    img.scaled = true;
                    img.scaleFromWidth = frame.width;
                    img.scaleFromHeight = frame.height;
                    img.scaleApplied = scaleRes.status == ScaleStatus::kScaled;
                    if (img.scaleApplied && !ScaleFrame(&frame, scaleRes.width, scaleRes.height)) {
                        ok = false;
                        CaptureError(&targetErr, scaleChannel, Msg(L"cap.scale_apply_failed"),
                                     Msg(L"cap.scale_apply_failed_hint"), codes::kFrameInvalid);
                    }
                }
            }
            if (ok) {
                img.width = frame.width;
                img.height = frame.height;
                img.source = frame.source;   // 真正出图的那条通道，auto 时与请求值不同
                // 实际路径与像素来源：这一帧到底是"窗口自己的画面"还是"屏幕上那块区域"，
                // 调用方要靠它判断自己拿到了什么，--quiet 也不许把它藏起来。
                img.path = frame.path.empty() ? std::wstring(paths::kUnknown) : frame.path;
                img.scope = ScopeName(ScopeOf(img.path));
                // 光标那三个键在这里合成，一次算完（判据与取值都在 src/CursorControl.h）：
                // requested 是用户要的那一种，effective 是**这条路径实际**交回的那一种，
                // basis 说这个结论凭什么。frame 里那两个值是 wgc 设完再读回来的答复；
                // 其余通道留 false/false，它们的"这一帧里没有光标"来自登记表而不是那次问答，
                // 所以不会在这里冒充"我读过开关"。没写 --cursor 时 written=false，三个键都不出现。
                img.cursor = MakeCursorReport(opt.cursor, img.path, frame.cursorStateKnown,
                                              frame.cursorInFrame);
                // HDR 那组键也在这里一次算完（判据与取值都在 src/HdrColor.h）：requested 是要求的策略，
                // effective 是这一帧**实际**经历的处理，basis 说这个结论凭什么，再加来源色彩空间与位深。
                // 三份事实各自交上来，谁也不替谁作保：frame.sourceColorSpace 是取帧那一步读回的内存
                // 布局（编码之前那一份；wide 帧映射后仍保留映射前那一份），frame.toneMapped 是映射
                // 函数自己记下的"这一帧真过了浮点那条链路"，frame.displayHdrState 是采集之前那次
                // 只读问答的答复（没问过与问不出来都是 kUnknown）。"这张帧是 8 位"不证明来源是 SDR，
                // 所以那一格说不说 sdr_passthrough 只看后面两份，不看这一帧自己的形状。
                // 没写 --hdr 时 written=false，那组键一个都不出现，这条流与之前逐字节相同。
                img.hdr = MakeHdrReport(opt.hdr, img.path, frame.sourceColorSpace, frame.toneMapped,
                                       frame.displayHdrState);
                // 明确要过 HDR 处理（tonemap / refuse）而这一张是按 8 位交付的：这不是错误（图照常交），
                // 但"我要过 HDR 处理"与"这一张其实没有 HDR 可处理"是两件事，要放在调用方眼前，
                // 而不是拿一个静默的通过冒充"HDR 已经被正确映射"。auto 不提示（它本就只是被动上报）。
                // 留哪一条由 JudgeHdrPassiveNote 判：那句"来源是 SDR"只能由**采集之前真的问到
                // 这块屏此刻是 SDR**来支撑（frame.displayHdrState），一张 8 位帧自己不算证据 ——
                // 问不出来时改发 note.hdr_source_unverified，而不是把没核实说成没有 HDR。
                // 这一条与上面那一组键说的是同一件事，但它是说给人听的：notes 整段会被 --quiet 去掉，
                // 所以"没核实"那一句必须由 MakeHdrReport 那一份自己写进 hdrEffective / hdrBasis，
                // 不能靠这里补（提示藏得掉，机器字段藏不掉）。
                switch (JudgeHdrPassiveNote(opt.hdr, frame.sourceColorSpace,
                                            frame.displayHdrState)) {
                    case HdrPassiveNote::kSourceSdr:
                        hdrNote = Diagnostic{codes::kHdrSourceSdr, Msg(L"note.hdr_source_sdr"),
                                             L"--hdr", HdrPolicyName(opt.hdr.policy),
                                             Msg(L"note.hdr_source_sdr_hint"), t.Tag(), img.source,
                                             stages::kCapture};
                        break;
                    case HdrPassiveNote::kSourceUnverified:
                        hdrNote = Diagnostic{
                            codes::kHdrSourceUnverified, Msg(L"note.hdr_source_unverified"),
                            L"--hdr", HdrPolicyName(opt.hdr.policy),
                            Msg(L"note.hdr_source_unverified_hint"), t.Tag(), img.source,
                            stages::kCapture};
                        break;
                    case HdrPassiveNote::kNone:
                        break;
                }
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

            // 这里借的是 img.source 自己那块缓冲，所以它只活到 img 移交为止：下面 pending.image
            // 按值搬走整张图，交付那一步再按值收一份 pending，异常在交付函数里抛出来时，那一份
            // 在展开到本层 catch 之前就已经销毁。于是指针只能用在移交之前的这两处（编码那一段与
            // dtarget.backend 的赋值），交付那一段的诊断改拿 dtarget.backend —— 它是独立拥有的
            // 同一份来源名，且活到本层调用之外。
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

            // 交付这一段（写文件 / 写标准输出）整个交给 DeliverImage：开工之前判一次预算并备好
            // 落地之后要用的记账资源，完工之后再核一次，交付事实与期限合规分开记账（那四段各做
            // 什么、为什么必须分开，判据本体写在 src/Delivery.h）。step 由本层持有而不是取返回值：
            // 那一段抛出东西时没有返回值可读，而"这一张到底有没有交出去、账记到哪儿"正是那一刻
            // 最需要分清的，所以 DeliverImage 在事实成立的那一刻直接把它写在这里。
            if (ok) {
                stage = img.file == L"-" ? stages::kStdout : stages::kWrite;
                DeliveryTarget dtarget;
                dtarget.file = img.file;
                dtarget.tag = t.Tag();
                dtarget.backend = backend;
                dtarget.overwrite = opt.overwrite;
                dtarget.implicitStdout = opt.outputImplicitStdout;

                PendingImage pending;
                pending.startedClockMs = started;
                pending.image = std::move(img);
                // 质量提示的先后顺序与这一段存在之前一致；送不送由交付那一步判（图没落地就不提示）。
                if (clippedNote) pending.notes.push_back(std::move(*clippedNote));
                if (cropMappingNote) pending.notes.push_back(std::move(*cropMappingNote));
                if (hdrNote) pending.notes.push_back(std::move(*hdrNote));
                if (uniformNote) pending.notes.push_back(std::move(*uniformNote));

                // 这里的来源名拿 dtarget.backend 那一份，不拿 backend：那缓冲已经跟着 img 搬进
                // pending，异常展开时先于本层 catch 销毁；dtarget 是本层局部对象，catch 里读它还活着。
                CallBackend(
                    stage, dtarget.backend.c_str(),
                    [&] {
                        DeliverImage(delivery, dtarget, std::move(pending), encoded, &outcome,
                                     &step);
                        return step.delivered;
                    },
                    &targetErr, &fatal);
                settled = step.recorded;
            }
        } catch (...) {
            // 走到这里说明上面那几层边界之外还有东西抛（例如 std::vector 扩容失败）：
            // 同样只作废这一个目标，除非它确实是整机级别的资源问题。
            FillFromCurrentException(&targetErr, stage, CaptureMethodName(opt.capture), &fatal);
            // 交付那一段已经把这一张记完账（图与提示都已入列）之后才抛出来的，只剩那条期限记录的
            // 两句文字组不起来：这一张的账记过就是记过，这里不再补第二条，否则等于把一次成功的
            // 交付又说成一次写失败。抛在入账之前时 step.recorded 仍是 false，照旧记这一条真实原因。
            settled = step.recorded;
        }

        if (!settled) {
            // 这一张没走完成功的交付：要么在进交付段之前就停了（取帧 / 裁剪 / 缩放 / 编码那几段
            // 的错误），要么交付那一段抛了异常被上面接住。两种都在这里记一条，backend 已被
            // 那两层填过的不改（它写的是自己那条真实路径，比这里能推断出的更准）。
            // 交付那一段自己记过账的（含"图已落地但预算才跨"那一种）不在这里重复入账。
            TagTarget(&targetErr, t, stage);
            outcome.errors.push_back(std::move(targetErr));
        }
        // 致命错误，或者已经有人在确认框上答过"否"（包括根本弹不出框）：
        // 剩下那些目标不再换后端、不再重试，也不再问第二次 —— 直接停在这里。
        // 前面已经写出的图与这条诊断都留着，调用方看得到"停在哪、为什么停"。
        // 交付完成之后预算才跨的那一张不会停在这里：它已经落地，下一个目标会在开工之前
        // 被上面那份共用预算挡下来，不会再领一笔预算继续干活。
        if (fatal || gate.Refused()) break;
    }

    outcome.exitCode = OutcomeExitCode(outcome);
    return outcome;
}

}  // namespace ecapture
