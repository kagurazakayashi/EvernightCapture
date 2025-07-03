#include "Consent.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"

namespace ecapture {
namespace {

constexpr size_t kMaxListedScreens = 4;

// 点"是"到真正取帧之间要等一下：对话框刚销毁时关闭动画还在 DWM 的画面上，
// 立刻截就会把半透明的残影拍进图里。1 秒足够动画放完，也不会让人觉得卡住。
constexpr DWORD kDialogSettleMs = 1000;

// 屏幕上没有窗口可归属时，"图去哪里"是调用方最关心的一条信息，必须写进弹框。
std::wstring OutputLabel(const Options& opt) {
    return opt.output == L"-" ? Msg(L"consent.output_stdout") : opt.output;
}

std::wstring TargetList(const std::vector<ScreenInfo>& screens) {
    std::wstring text;
    for (size_t i = 0; i < screens.size() && i < kMaxListedScreens; ++i) {
        if (i) text += L"\r\n  ";
        text += DescribeScreen(screens[i]);
    }
    if (screens.size() > kMaxListedScreens)
        text += L"\r\n  " + Msgf(L"consent.more_screens", screens.size() - kMaxListedScreens);
    return text;
}

Diagnostic Refused(const std::vector<ScreenInfo>& screens, const wchar_t* key,
                   const std::wstring& hint) {
    return Diagnostic{codes::kAccessDenied, Msg(key), L"--monitor",
                      std::to_wstring(screens.size()), hint};
}

}  // namespace

bool AskScreenCaptureConsent(const Options& opt, const std::vector<ScreenInfo>& screens,
                             Diagnostic* err) {
    std::wstring body;
    body += Msg(L"consent.head") + L"\r\n\r\n";
    body += Msgf(L"consent.targets", screens.size(), TargetList(screens)) + L"\r\n";
    body += Msgf(L"consent.plan", CaptureMethodName(opt.capture), OutputLabel(opt)) + L"\r\n\r\n";
    body += Msg(L"consent.warning") + L"\r\n";
    body += Msg(L"consent.dialog_closes") + L"\r\n\r\n";
    body += Msg(L"consent.ask");

    // 默认焦点在"否"上，回车不会误批；置顶是因为用户可能正全屏开着别的东西。
    const int answer = MessageBoxW(nullptr, body.c_str(), Msg(L"consent.title").c_str(),
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND |
                                       MB_TOPMOST);
    if (answer == IDYES) {
        Sleep(kDialogSettleMs);
        return true;
    }
    if (answer == IDNO || answer == IDCANCEL) {
        if (err) *err = Refused(screens, L"consent.denied", Msg(L"consent.denied_hint"));
        return false;
    }
    // 返回 0：MessageBoxW 调用失败（不在交互桌面、窗口站没有桌面、安全桌面里）。
    // 这时没有人能回答"是"，按拒绝处理。
    if (err) {
        *err = Refused(screens, L"consent.unavailable",
                       Msgf(L"consent.unavailable_hint", Win32ErrorText()));
    }
    return false;
}

}  // namespace ecapture
