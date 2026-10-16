#pragma once
// 【GUI · wxWidgets】程序自有对话框的唯一一份控件布局（确认 / 权限说明 / 错误提示共用）。
//
// 这个头只给 src/gui/ 与 GUI 判据使用；共用模型、授权状态机与平台截图层都不 include 它。
// 生产构建没有测试钩子：下面的 ECAPTURE_GUI_TESTING 只由 ecapture-gui-tests 定义。

#include <wx/dialog.h>
#include <wx/event.h>

#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "Gui.h"

class wxButton;
class wxScrolledWindow;
class wxStaticText;
class wxTextCtrl;
class wxTimer;
class wxTimerEvent;

namespace ecapture {

class WxGuiDialog final : public wxDialog {
public:
    WxGuiDialog(wxWindow* parent, const GuiDialogRequest& request);
    ~WxGuiDialog() override;

    GuiDialogResult Result() const;

    // 后台看守线程通过驱动调用这里：把"到点"或"程序取消"排进 GUI 事件队列。
    // 只接受 kTimedOut / kCancelled；重复请求与已经答过的框都返回 true（幂等）。
    bool RequestClose(GuiDialogOutcome forcedOutcome);

    // 由状态机在 ShowModal 之前给出同一个截止时刻；点击处理据此判"到点了没有"。
    // 传空 = 没有期限（一直等人）。
    void SetDeadline(std::function<bool()> deadlineReached);

#ifdef ECAPTURE_GUI_TESTING
    // 仅供 GUI 判据：检查可访问名称、长文本是否完整、按钮焦点与默认项，然后投递
    // 按钮 / 关框事件。生产构建不编译这些成员。
    const GuiDialogRequest& RequestForTest() const { return request_; }
    wxButton* AcceptButtonForTest() const { return acceptButton_; }
    wxButton* DeclineButtonForTest() const { return declineButton_; }
    wxStaticText* HeadingForTest() const { return headingText_; }
    wxStaticText* QuestionForTest() const { return questionText_; }
    wxStaticText* FooterForTest() const { return footerText_; }
    wxTextCtrl* DetailsForTest() const { return detailsText_; }
    wxScrolledWindow* ContentForTest() const { return contentWindow_; }
#endif

private:
    void BuildUi();
    void EndWith(GuiDialogOutcome outcome);
    void ApplyPendingClose();
    void OnAccept(wxCommandEvent& event);
    void OnDecline(wxCommandEvent& event);
    void OnClose(wxCloseEvent& event);
    void OnCharHook(wxKeyEvent& event);
    void OnRequestCloseEvent(wxCommandEvent& event);
    void OnClosePoll(wxTimerEvent& event);

    GuiDialogRequest request_;
    std::function<bool()> deadlineReached_;

    mutable std::mutex stateMutex_;
    bool answered_ = false;
    GuiDialogResult result_;
    std::optional<GuiDialogOutcome> pendingClose_;

    wxStaticText* headingText_ = nullptr;
    wxStaticText* questionText_ = nullptr;
    wxStaticText* footerText_ = nullptr;
    wxTextCtrl* detailsText_ = nullptr;
    wxScrolledWindow* contentWindow_ = nullptr;
    wxButton* acceptButton_ = nullptr;
    wxButton* declineButton_ = nullptr;
    wxTimer* closePollTimer_ = nullptr;
};

#ifdef ECAPTURE_GUI_TESTING
// 判据在构造完成后、ShowModal 之前拿到对话框：检查布局并投递"是 / 否 / 关框"事件。
// 定义在测试目标里；生产构建不引用这个符号。
using WxGuiDialogTestHook = void (*)(WxGuiDialog&);
extern WxGuiDialogTestHook g_wxGuiDialogTestHook;
#endif

}  // namespace ecapture
