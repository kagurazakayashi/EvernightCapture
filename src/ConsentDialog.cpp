#include "ConsentDialog.h"

#include <string>
#include <vector>

#include "Lang.h"
#include "core/DisplayText.h"

namespace ecapture {
namespace {

constexpr size_t kMaxListedTargets = 6;
// 调用方自述与清单行各自的上限。超长不是错误（不因此拒绝请求），只是展示时截断并标出省略号。
constexpr size_t kMaxSourceLines = 4;
constexpr size_t kMaxSourceLineUnits = 160;
constexpr size_t kMaxLineCodePoints = 200;

std::wstring SafeLine(const std::wstring& text) {
    return display_text::SanitizeDisplayLine(text, kMaxLineCodePoints);
}

std::wstring JoinSafeLines(const std::vector<std::wstring>& lines, size_t maxListed,
                           const wchar_t* moreKey) {
    std::wstring text;
    for (size_t i = 0; i < lines.size() && i < maxListed; ++i) {
        if (i) text += L"\n  ";
        text += SafeLine(lines[i]);
    }
    if (lines.size() > maxListed) {
        text += L"\n  " + Msgf(moreKey, lines.size() - maxListed);
    }
    return text;
}

}  // namespace

GuiDialogRequest BuildConsentDialogRequest(const ConsentQuestion& q, uint64_t timeoutMs) {
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

    // 谁在请求：两段分开写，标签也不同（可信 / 不可信），不把两种来源揉成一句话。
    request.paragraphs.push_back(
        Msgf(L"consent.program_facts", SafeLine(q.programPath), SafeLine(q.programVersion),
             static_cast<unsigned long long>(q.programPid)));
    if (q.requestSourceGiven && !q.requestSource.empty()) {
        request.paragraphs.push_back(Msgf(
            L"consent.request_source_untrusted",
            display_text::SanitizeDisplayBlock(q.requestSource, kMaxSourceLines,
                                               kMaxSourceLineUnits)));
    } else {
        request.paragraphs.push_back(Msg(L"consent.request_source_none"));
    }

    // 桌面那一级必须把"--yes 在这儿没用"写在人眼前：不然调用方以为加了开关就等于授权。
    // 窗口那一级也说清 --yes 的边界（能力与目标都核实过、且只限单窗口自身图像）。
    if (desktop && q.yesGiven) request.paragraphs.push_back(Msg(L"consent.yes_desktop"));
    if (!desktop && !q.yesGiven) request.paragraphs.push_back(Msg(L"consent.yes_window"));
    request.paragraphs.push_back(Msg(L"consent.dialog_closes"));

    // 目标与输出放在只读多行框里：长路径可以选中复制，也会在框内折行而不是把对话框
    // 撑出屏幕。每一行动态内容都先净化（标题可能带换行 / 双向控制符 / 超长）。
    request.detailsLabel = Msg(L"consent.details_label");
    request.details =
        Msgf(L"consent.targets", q.targets.size(),
             JoinSafeLines(q.targets, kMaxListedTargets, L"consent.more_targets")) +
        L"\n\n" +
        Msgf(L"consent.plan", SafeLine(q.captureLabel), SafeLine(q.path),
             JoinSafeLines(q.outputs, kMaxListedTargets, L"consent.more_targets"));

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

}  // namespace ecapture
