// 授权确认框的"文案与结构"这一层：把 ConsentQuestion 渲染成跨平台 GUI 请求。
//
// 本文件不 include 任何 wx 头，也不创建窗口；真正的 wxWidgets 对话框在
// src/gui/WxConsentDriver.cpp / WxGuiDialog.cpp。这样文案与授权判据仍然可以在没有 GUI 的
// 目标里编译，而控件细节只有一处。
#include "Consent.h"

#include <string>
#include <utility>
#include <vector>

#include "Lang.h"
#include "gui/ConsentGuiDriver.h"
#include "gui/Gui.h"

namespace ecapture {
namespace {

constexpr size_t kMaxListedTargets = 6;

std::wstring JoinLines(const std::vector<std::wstring>& lines, size_t maxListed,
                       const wchar_t* moreKey) {
    std::wstring text;
    for (size_t i = 0; i < lines.size() && i < maxListed; ++i) {
        if (i) text += L"\n  ";
        text += lines[i];
    }
    if (lines.size() > maxListed) {
        text += L"\n  " + Msgf(moreKey, lines.size() - maxListed);
    }
    return text;
}

GuiDialogRequest BuildRequest(const ConsentQuestion& q, uint64_t timeoutMs) {
    const bool desktop = q.scope == PixelScope::kDesktop;
    GuiDialogRequest request;
    request.title = Msg(L"consent.title");
    request.heading = Msg(L"consent.head");

    request.paragraphs.push_back(Msg(desktop ? L"consent.scope_desktop"
                                             : L"consent.scope_window"));
    // 副本这件事要说在人点头之前：批准的是"截这几扇窗口、写到这几个名字"，而程序自己的目录里
    // 还会另存一份不自动清理的历史图。归档根解析不出来的那一次（那一路走不通）就不说这句，
    // 免得承诺一份根本不会存在的拷贝。
    if (q.historyCopy) request.paragraphs.push_back(Msg(L"consent.history"));
    // 桌面那一级必须把"--yes 在这儿没用"写在人眼前：不然调用方以为加了开关就等于授权。
    if (desktop && q.yesGiven) request.paragraphs.push_back(Msg(L"consent.yes_desktop"));
    if (!desktop && !q.yesGiven) request.paragraphs.push_back(Msg(L"consent.yes_window"));
    request.paragraphs.push_back(Msg(L"consent.dialog_closes"));

    // 目标与输出放在只读多行框里：长路径可以选中复制，也会在框内折行而不是把对话框
    // 撑出屏幕。文案本身沿用既有资源 key，屏幕上的说法与旧版一致。
    request.detailsLabel = Msg(L"consent.details_label");
    request.details =
        Msgf(L"consent.targets", q.targets.size(),
             JoinLines(q.targets, kMaxListedTargets, L"consent.more_targets")) +
        L"\n\n" +
        Msgf(L"consent.plan", q.captureLabel, q.path,
             JoinLines(q.outputs, kMaxListedTargets, L"consent.more_targets"));

    request.question = Msg(L"consent.ask");
    if (timeoutMs > 0) {
        // 等人回答的那段时间是有上限的，这件事要写在框上：不然人以为"放着不管以后还能截"，
        // 而实际上到点就按拒绝处理了。
        request.footer = Msgf(L"consent.deadline_line", timeoutMs);
    }
    request.acceptLabel = Msg(L"consent.yes");
    request.declineLabel = Msg(L"consent.no");
    request.severity = desktop ? GuiDialogSeverity::kWarning : GuiDialogSeverity::kInformation;
    return request;
}

}  // namespace

ConsentReply DialogConsentPrompt::Ask(const ConsentQuestion& q) {
    GuiDialogRequest request = BuildRequest(q, timeoutMs_);
    return RunMonitoredDialog(MakeWxConsentDialogDriver(std::move(request)), timeoutMs_,
                              cancelToken_);
}

}  // namespace ecapture
