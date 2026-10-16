// 跨平台 GUI 层与确认框的离线/隔离图形判据（ecapture-gui-tests）。
//
// 有图形会话时：真的创建 wxWidgets 对话框，检查长中文标题、长路径、默认焦点、可访问名称、
// 高 DPI 下的尺寸上限，以及同意 / 拒绝 / 关框 / 超时 / 程序取消五种结局。后台取消与
// GuiPostToMainThread 也在这里真跑 —— 后台线程只投递消息，不直接碰控件。
//
// 没有图形会话时（由 tests/gui-runtime.sh 用 env -u DISPLAY 跑 --init-failure）：
// 初始化必须失败并带出系统原值，绝不能假装成功或把窗口消失读成同意。
//
// 测试替身只在编译期钩子里：g_wxGuiDialogTestHook 只在 ECAPTURE_GUI_TESTING 下存在，
// 发布版 ECAPTURE.EXE 不编译这些成员。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>

#include "../src/Consent.h"
#include "../src/Lang.h"
#include "../src/gui/Gui.h"
#include "../src/gui/WxGuiDialog.h"

using namespace ecapture;

namespace ecapture {
// 测试替身：由本目标定义，生产构建不引用这个符号。
WxGuiDialogTestHook g_wxGuiDialogTestHook = nullptr;
}  // namespace ecapture

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        ++g_passed;
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

std::wstring ToW(const wxString& s) { return std::wstring(s.wc_str()); }

std::wstring NormalizeNewlines(std::wstring text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            out.push_back(L'\n');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::function<void(WxGuiDialog&)> g_hook;

void HookTrampoline(WxGuiDialog& dialog) {
    if (g_hook) g_hook(dialog);
}

void SetHook(std::function<void(WxGuiDialog&)> hook) {
    g_hook = std::move(hook);
    g_wxGuiDialogTestHook = HookTrampoline;
}

void ClearHook() {
    g_hook = nullptr;
    g_wxGuiDialogTestHook = nullptr;
}

void PostDecline(WxGuiDialog& d) {
    wxPostEvent(d.DeclineButtonForTest(), wxCommandEvent(wxEVT_BUTTON, wxID_NO));
}

void PostAccept(WxGuiDialog& d) {
    wxPostEvent(d.AcceptButtonForTest(), wxCommandEvent(wxEVT_BUTTON, wxID_YES));
}

struct Snapshot {
    std::wstring heading;
    std::wstring details;
    std::wstring acceptLabel;
    std::wstring declineLabel;
    std::wstring dialogName;
    std::wstring detailsName;
    bool defaultIsDecline = false;
    bool hasDeclineButton = false;
    wxSize size;
    wxRect screen;
};

Snapshot g_snapshot;

GuiDialogRequest MakeLongRequest() {
    GuiDialogRequest request;
    request.title = L"EvernightCapture 确认一次很长的截图请求（标题故意写得长一些，"
                    L"用来检查窗口标题和高 DPI 下的折行）";
    request.heading = L"本工具准备截取画面，需要你同意。这里有一段很长的中文说明，"
                      L"用来验证标题在窄窗口里会折行而不是被裁掉。";
    request.paragraphs = {
        L"这条路径取的是屏幕上那块区域此刻的样子：其它窗口、打开的文档、通知、桌面上的文字"
        L"都可能进图。请确认你允许的范围。",
        L"每张成功交付的图还会在程序自己那目录下的 history 子目录里另存一份独立副本。",
    };
    request.detailsLabel = L"目标与输出";
    request.details = L"目标（共 2 个）：窗口标题很长很长很长很长很长很长很长很长很长很长\n\n"
                      L"取图方式 auto，实际路径 wgc，图片写往 /home/用户/图片/";
    request.details += std::wstring(240, L'长');
    request.details += L"/截图.png";
    request.question = L"允许现在截图吗？";
    request.footer = L"请在 60000 毫秒内回答；到点仍没人回答就按“否”处理，不会默认同意。";
    request.acceptLabel = L"允许";
    request.declineLabel = L"不允许";
    request.severity = GuiDialogSeverity::kWarning;
    return request;
}

ConsentQuestion MakeLongQuestion() {
    ConsentQuestion q;
    q.scope = PixelScope::kWindowContent;
    q.path = L"wgc";
    q.captureLabel = L"auto";
    q.targets = {L"标题很长的测试窗口（区域 100,100 尺寸 800x600）",
                 L"另一个标题同样很长的测试窗口（区域 940,100 尺寸 800x600）"};
    q.outputs = {L"/home/用户/图片/" + std::wstring(180, L'输') + L"/截图.png"};
    q.yesGiven = false;
    q.historyCopy = true;
    return q;
}

int InitFailureMain() {
    std::wstring detail;
    NativeErrorCode nativeError = 0;
    const bool started = GuiRuntimeEnsureStarted(&detail, &nativeError);
    std::printf("init-failure: started=%d detail=%ls native=%u\n", started ? 1 : 0,
                detail.c_str(), static_cast<unsigned>(nativeError));
    const bool ok = !started && !detail.empty();
    std::printf("%d 条通过，%d 条失败\n", ok ? 1 : 0, ok ? 0 : 1);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--init-failure") return InitFailureMain();
    }

    SetLanguage(Language::kZhCn);

    Section("1) GUI 上下文按需初始化");
    std::wstring detail;
    NativeErrorCode nativeError = 0;
    if (!GuiRuntimeEnsureStarted(&detail, &nativeError)) {
        std::printf("GUI 初始化失败：%ls（native=%u）\n", detail.c_str(),
                    static_cast<unsigned>(nativeError));
        std::printf("0 条通过，1 条失败\n");
        return 1;
    }
    Check(GuiRuntimeStarted(), "GUI 主线程初始化成功");
    Check(wxTopLevelWindows.GetCount() == 0, "初始化不建任何常驻窗口");

    Section("2) 公共对话框：长中文标题、长路径、默认焦点与可访问名称");
    {
        g_snapshot = {};
        const GuiDialogRequest request = MakeLongRequest();
        SetHook([](WxGuiDialog& d) {
            g_snapshot.heading = ToW(d.HeadingForTest()->GetLabelText());
            g_snapshot.details = NormalizeNewlines(ToW(d.DetailsForTest()->GetValue()));
            g_snapshot.acceptLabel = ToW(d.AcceptButtonForTest()->GetLabelText());
            g_snapshot.declineLabel = ToW(d.DeclineButtonForTest()->GetLabelText());
            g_snapshot.dialogName = ToW(d.GetName());
            g_snapshot.detailsName = ToW(d.DetailsForTest()->GetName());
            g_snapshot.defaultIsDecline =
                d.GetDefaultItem() == static_cast<wxWindow*>(d.DeclineButtonForTest());
            g_snapshot.size = d.GetSize();
            g_snapshot.screen = wxDisplay().GetClientArea();
            PostDecline(d);
        });
        const GuiDialogResult result = ShowGuiDialog(request);
        ClearHook();

        Check(result.outcome == GuiDialogOutcome::kDeclined, "点“不允许” => kDeclined");
        Check(g_snapshot.details == NormalizeNewlines(request.details),
              "长路径在只读多行框里一字不差");
        Check(g_snapshot.heading.find(L"本工具准备截取画面") != std::wstring::npos,
              "长中文标题完整进入窗口（没有被裁掉）");
        // wxStaticText 在 wxGTK 上不把折行写回 GetLabelText，GetBestSize 也不随 Wrap 变化，
        // 所以这里不拿它当"是否折行"的跨平台判据；标题完整性 + 对话框不超出屏幕 + 折行宽度
        // 由 WxGuiDialog::Wrap 固定给出，三项一起守住"长标题不被裁掉/不撑破屏幕"。
        Check(g_snapshot.size.GetWidth() <= g_snapshot.screen.GetWidth() &&
                  g_snapshot.size.GetHeight() <= g_snapshot.screen.GetHeight(),
              "长标题与长路径不会把对话框撑出屏幕（高 DPI 下也按屏幕客户区封顶）");
        Check(g_snapshot.acceptLabel == request.acceptLabel &&
                  g_snapshot.declineLabel == request.declineLabel,
              "按钮文字来自多语言资源");
        Check(g_snapshot.defaultIsDecline, "默认按钮是“不允许”，回车不会误同意");
        Check(g_snapshot.dialogName == L"ecapture-gui-dialog", "对话框有稳定的可访问名称");
        Check(g_snapshot.detailsName == request.detailsLabel, "细节区有可访问名称");
        Check(wxTopLevelWindows.GetCount() == 0, "关闭后窗口确实销毁（没有残留）");
    }

    Section("3) 错误提示模式：共用对话框只留一个确认按钮");
    {
        g_snapshot = {};
        GuiDialogRequest request;
        request.title = L"EvernightCapture 错误";
        request.heading = L"这次操作没有完成";
        request.paragraphs = {L"这里是一个需要人看到的错误说明；它不取像素、也不涉及授权。"};
        request.acceptLabel = L"知道了";
        request.showDecline = false;
        request.severity = GuiDialogSeverity::kError;
        SetHook([](WxGuiDialog& d) {
            g_snapshot.hasDeclineButton = d.DeclineButtonForTest() != nullptr;
            PostAccept(d);
        });
        const GuiDialogResult result = ShowGuiDialog(request);
        ClearHook();
        Check(result.outcome == GuiDialogOutcome::kAccepted, "错误提示点确认 => kAccepted");
        Check(!g_snapshot.hasDeclineButton, "错误提示不显示拒绝按钮");
        Check(wxTopLevelWindows.GetCount() == 0, "错误提示关闭后没有残留窗口");
    }

    Section("4) 授权确认框：文案沿用四语资源，拒绝落地");
    {
        g_snapshot = {};
        SetHook([](WxGuiDialog& d) {
            g_snapshot.heading = ToW(d.HeadingForTest()->GetLabelText());
            g_snapshot.details = NormalizeNewlines(ToW(d.DetailsForTest()->GetValue()));
            g_snapshot.acceptLabel = ToW(d.AcceptButtonForTest()->GetLabelText());
            g_snapshot.declineLabel = ToW(d.DeclineButtonForTest()->GetLabelText());
            PostDecline(d);
        });
        DialogConsentPrompt prompt(0);
        const ConsentReply reply = prompt.Ask(MakeLongQuestion());
        ClearHook();
        Check(reply.answer == ConsentAnswer::kDeclined, "授权框答“不允许” => kDeclined");
        Check(g_snapshot.heading.find(L"本工具准备截取画面") != std::wstring::npos,
              "标题文案取自 consent.head");
        Check(g_snapshot.acceptLabel == Msg(L"consent.yes") &&
                  g_snapshot.declineLabel == Msg(L"consent.no"),
              "按钮文案取自 consent.yes / consent.no");
        Check(g_snapshot.details.find(L"目标（共 2 个）") != std::wstring::npos &&
                  g_snapshot.details.find(L"截图.png") != std::wstring::npos,
              "目标与输出都在细节区（含中文长路径）");
        Check(wxTopLevelWindows.GetCount() == 0, "授权框关闭后没有残留窗口");
    }

    Section("5) 同意、关框、超时与取消四档分得开");
    {
        SetHook([](WxGuiDialog& d) { PostAccept(d); });
        DialogConsentPrompt prompt(0);
        const auto started = std::chrono::steady_clock::now();
        const ConsentReply reply = prompt.Ask(MakeLongQuestion());
        ClearHook();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        Check(reply.answer == ConsentAnswer::kAccepted, "点“允许” => kAccepted");
        Check(elapsed >= 950, "同意路径保留约 1 秒关闭动画缓冲");
    }
    {
        SetHook([](WxGuiDialog& d) {
            wxQueueEvent(&d, new wxCloseEvent(wxEVT_CLOSE_WINDOW));
        });
        DialogConsentPrompt prompt(0);
        const ConsentReply reply = prompt.Ask(MakeLongQuestion());
        ClearHook();
        Check(reply.answer == ConsentAnswer::kClosed, "按 X 关框 => kClosed（不是同意）");
    }
    {
        SetHook([](WxGuiDialog&) {});
        DialogConsentPrompt prompt(250);
        const auto started = std::chrono::steady_clock::now();
        const ConsentReply reply = prompt.Ask(MakeLongQuestion());
        ClearHook();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        Check(reply.answer == ConsentAnswer::kTimedOut, "没人回答 => kTimedOut");
        Check(elapsed >= 250 && elapsed < 4000, "超时收尾有界（期限 + 宽限之内）");
        Check(wxTopLevelWindows.GetCount() == 0, "超时后窗口也真的销毁了");
    }

    Section("6) 程序取消：后台令牌只投递消息，不碰控件");
    {
        SetHook([](WxGuiDialog&) {});
        ConsentCancelToken token;
        std::thread canceller([&token] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            token.Cancel();
        });
        DialogConsentPrompt prompt(0, &token);
        const ConsentReply reply = prompt.Ask(MakeLongQuestion());
        canceller.join();
        ClearHook();
        Check(reply.answer == ConsentAnswer::kCancelled, "取消令牌 => kCancelled");
        Check(wxTopLevelWindows.GetCount() == 0, "取消后窗口最终关闭并确认销毁");
    }

    Section("7) 后台任务通过 GuiPostToMainThread 投递，不直接操作控件");
    {
        std::atomic<bool> posted{false};
        std::thread bg;
        SetHook([&bg, &posted](WxGuiDialog& d) {
            wxButton* noButton = d.DeclineButtonForTest();
            bg = std::thread([noButton, &posted] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const bool ok = GuiPostToMainThread([noButton] {
                    wxPostEvent(noButton, wxCommandEvent(wxEVT_BUTTON, wxID_NO));
                });
                posted.store(ok);
            });
        });
        const GuiDialogResult result = ShowGuiDialog(MakeLongRequest());
        if (bg.joinable()) bg.join();
        ClearHook();
        Check(posted.load(), "后台线程投递成功");
        Check(result.outcome == GuiDialogOutcome::kDeclined,
              "任务在 GUI 主线程上执行并关掉了框");
    }

    Section("8) 通知接口已接线（不在自动判据里真弹系统通知）");
    std::printf("  note 通知接口已编译并链接；实际弹出交给结果通知那一步与人工检查，"
                "判据不打扰当前桌面。\n");

    GuiRuntimeShutdown();
    Check(!GuiRuntimeStarted(), "GUI 上下文可以按序收尾");

    std::printf("\n%d 条通过，%d 条失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
