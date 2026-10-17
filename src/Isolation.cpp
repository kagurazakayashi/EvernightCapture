#include "Isolation.h"

#include <string>

#include "ErrorText.h"
#include "Lang.h"
#include "core/Error.h"

namespace ecapture {
namespace {

// 把 ASCII 细节（异常 what()、正则库的机器说明）按字节宽化：这些字节本来就不是本地化
// 文字，逐字节映射到 wchar_t 不会破坏任何东西，改造前 Worker.cpp 里那一份也是这么做的。
std::wstring WideFromAscii(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (char ch : s) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 判据：一步"会被卡住的调用"失败 -> 结构化诊断。
//
// 这一份原先住在 Worker.cpp（Windows 平台层），但它判的是**稳定码与文案**：同一组
// BlockedStatus 在父进程本进程跑的退路与辅助进程报回来的失败上必须给出同一句话。
// 平台层只回答"哪一问失败"（BlockedStatus）与"系统码是多少"（NativeErrorCode），
// 文字与码的映射因此回到共用层；0x%08X 那一格由平台的 HResultText 给。
// ---------------------------------------------------------------------------
Diagnostic BlockedToDiagnostic(BlockedStatus status, NativeErrorCode win32,
                               NativeErrorCode hresult, const std::string& detail,
                               const wchar_t* backend, const wchar_t* stage) {
    Diagnostic d;
    d.code = codes::kCaptureFailed;
    d.option = L"--capture";
    d.value = backend ? backend : std::wstring();
    d.backend = d.value;
    d.stage = stage;
    d.win32 = win32;
    // 0 = 该来源没有失败过（core/Error.h 的既有约定），此时整个键不写。
    if (hresult != kNativeErrorNone) d.hresult = HResultCodeText(hresult);

    switch (status) {
        case BlockedStatus::kRectEmpty:
            d.code = codes::kWindowGone;
            d.message = Msg(L"cap.rect_empty_draw");
            d.hint = Msg(L"cap.window_gone");
            break;
        case BlockedStatus::kDibCreate:
            d.message = Msg(L"cap.create_dib_section");
            d.hint = Msgf(L"err.win32_code", win32);
            break;
        case BlockedStatus::kPrintWindowFailed:
            d.message = Msg(L"cap.pw.failed");
            d.hint = Msgf(L"cap.pw.failed_hint", NativeErrorText(win32));
            break;
        case BlockedStatus::kHostPrintWindowFailed:
            d.message = Msg(L"cap.dwm.pw_failed");
            d.hint = Msgf(L"err.win32_code", win32);
            break;
        case BlockedStatus::kHostClass:
            d.message = Msg(L"cap.dwm.register_class");
            d.hint = Msgf(L"err.win32_code", win32);
            break;
        case BlockedStatus::kHostCreate:
            d.message = Msg(L"cap.dwm.create_host");
            d.hint = Msgf(L"err.win32_code", win32);
            break;
        case BlockedStatus::kRegisterThumb:
            d.message = Msg(L"cap.dwm.register_thumb");
            d.hint = Msgf(L"cap.dwm.register_thumb_hint",
                          Msgf(L"cap.hresult", HResultCodeText(hresult)));
            break;
        case BlockedStatus::kUpdateProps:
            d.message = Msg(L"cap.dwm.update_props");
            d.hint = Msgf(L"cap.hresult", HResultCodeText(hresult));
            break;
        case BlockedStatus::kHostRectEmpty:
            d.message = Msg(L"cap.dwm.host_zero");
            break;
        case BlockedStatus::kRegexInvalid:
            // 本机正则库拒绝这条模式 —— 语法就是在这里（受约束的匹配执行层）第一次判的，
            // 解析层不构造正则。码沿用 cli.invalid_regex、退出码仍是 1，stage/backend 说 match。
            d.code = codes::kInvalidRegex;
            d.option = L"--title-regex";
            d.value = std::wstring();
            d.message = Msg(L"cli.regex_late");
            d.hint = Msg(L"cli.regex_hint") + L" (" + WideFromAscii(detail) + L")";
            break;
        case BlockedStatus::kRegexTooComplex:
            // 语法没错，是这台机器的正则库不肯把它跑完。加大 --timeout-ms 不会有帮助
            //（它不是慢，是被上限挡下），所以文案要指着"改模式"说，而不是"再等久一点"。
            d.code = codes::kInvalidRegex;
            d.option = L"--title-regex";
            d.value = std::wstring();
            d.message = Msg(L"cli.regex_too_complex");
            d.hint = Msgf(L"cli.regex_too_complex_hint", WideFromAscii(detail));
            break;
        case BlockedStatus::kBadTask:
            d.code = codes::kWorkerFailed;
            d.message = Msg(L"cap.worker.bad_task");
            d.hint = Msg(L"cap.worker.bad_task_hint");
            break;
        case BlockedStatus::kInternal:
            d.message = Msgf(L"err.exception", WideFromAscii(detail));
            break;
        case BlockedStatus::kOk:
        default:
            d.message = Msg(L"cap.no_detail");
            break;
    }
    return d;
}

}  // namespace ecapture
