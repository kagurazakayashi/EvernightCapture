// 【GUI · wxWidgets】确认 / 权限说明 / 错误提示共用的对话框本体。
//
// 布局只在这里写一次：标题区（可选图标 + 可折行的标题）、可滚动的说明与细节区、
// 加粗的问题、按钮行。高 DPI 全部走 FromDIP；长路径放在只读多行文本框里（可选中复制、
// 在框内折行），不会把窗口撑出屏幕。默认焦点与默认按钮都在"否"上，Esc / X 记为关框，
// 绝不把窗口消失读成同意。
#include "WxGuiDialog.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/display.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>

namespace ecapture {
namespace {

wxDEFINE_EVENT(EVT_GUI_REQUEST_CLOSE, wxCommandEvent);

constexpr int kClosePollMs = 50;

}  // namespace

WxGuiDialog::WxGuiDialog(wxWindow* parent, const GuiDialogRequest& request)
    : wxDialog(parent, wxID_ANY, wxString(request.title), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      request_(request) {
    SetName(wxS("ecapture-gui-dialog"));
    SetEscapeId(wxID_NONE);   // Esc 由 OnCharHook 收成"关框"，不交给默认的 Cancel

    closePollTimer_ = new wxTimer(this);
    Bind(wxEVT_TIMER, &WxGuiDialog::OnClosePoll, this, closePollTimer_->GetId());
    Bind(EVT_GUI_REQUEST_CLOSE, &WxGuiDialog::OnRequestCloseEvent, this);
    Bind(wxEVT_CHAR_HOOK, &WxGuiDialog::OnCharHook, this);
    Bind(wxEVT_CLOSE_WINDOW, &WxGuiDialog::OnClose, this);
    Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
        event.Skip();
        if (!event.IsShown()) return;
        if (declineButton_) {
            declineButton_->SetFocus();
        } else if (acceptButton_) {
            acceptButton_->SetFocus();
        }
    });

    BuildUi();
    closePollTimer_->Start(kClosePollMs);

#if defined(ECAPTURE_GUI_TESTING)
    if (g_wxGuiDialogTestHook) g_wxGuiDialogTestHook(*this);
#endif
}

WxGuiDialog::~WxGuiDialog() {
    if (closePollTimer_) {
        closePollTimer_->Stop();
        delete closePollTimer_;
        closePollTimer_ = nullptr;
    }
}

void WxGuiDialog::BuildUi() {
    const int margin = FromDIP(18);
    const int gap = FromDIP(10);
    const int smallGap = FromDIP(6);

    wxDisplay primary;
    const wxRect screen = primary.GetClientArea();
    // 先按屏幕客户区封顶，再决定内容宽度；高 DPI 下 FromDIP(420/700) 可能比逻辑屏幕还宽，
    // 不能拿"期望下限"去反向撑破屏幕。
    const int screenWidth = screen.width > FromDIP(48) ? screen.width - FromDIP(24) : screen.width;
    const int screenHeight = screen.height > FromDIP(48) ? screen.height - FromDIP(24) : screen.height;
    const int maxWidth = std::min(FromDIP(736), std::max(FromDIP(240), screenWidth));
    const int maxHeight = std::min(FromDIP(900), std::max(FromDIP(240), screenHeight));
    const int contentWidth = std::max(FromDIP(240), maxWidth - 2 * margin);
    const int wrapWidth = std::max(FromDIP(200), contentWidth - 2 * smallGap);

    auto* outer = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    const wxArtID art = request_.severity == GuiDialogSeverity::kWarning ? wxART_WARNING
                       : request_.severity == GuiDialogSeverity::kError   ? wxART_ERROR
                                                                         : wxART_INFORMATION;
    const wxBitmap icon = wxArtProvider::GetBitmap(art, wxART_MESSAGE_BOX,
                                                   wxSize(FromDIP(32), FromDIP(32)));
    int headingWrapWidth = contentWidth;
    if (icon.IsOk()) {
        auto* iconWindow = new wxStaticBitmap(this, wxID_ANY, icon);
        iconWindow->SetName(wxS("dialog-icon"));
        header->Add(iconWindow, 0, wxALIGN_TOP | wxRIGHT, gap);
        // 标题与图标同一行：折行宽度要扣掉图标与间距，否则高 DPI 下会横向溢出。
        headingWrapWidth = std::max(FromDIP(200), contentWidth - FromDIP(32) - gap);
    }

    headingText_ = new wxStaticText(this, wxID_ANY, wxString(request_.heading));
    wxFont headingFont = headingText_->GetFont();
    headingFont.SetWeight(wxFONTWEIGHT_BOLD);
    headingFont.SetPointSize(headingFont.GetPointSize() + 2);
    headingText_->SetFont(headingFont);
    headingText_->SetName(wxString(request_.heading));
    headingText_->Wrap(headingWrapWidth);
    header->Add(headingText_, 1, wxEXPAND | wxALIGN_TOP);
    outer->Add(header, 0, wxEXPAND | wxALL, margin);

    contentWindow_ = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                          wxVSCROLL | wxBORDER_NONE);
    contentWindow_->SetName(wxS("dialog-content"));
    contentWindow_->SetScrollRate(0, FromDIP(10));
    auto* content = new wxBoxSizer(wxVERTICAL);

    for (const std::wstring& paragraph : request_.paragraphs) {
        if (paragraph.empty()) continue;
        auto* text = new wxStaticText(contentWindow_, wxID_ANY, wxString(paragraph));
        text->SetName(wxString(paragraph));
        text->Wrap(wrapWidth);
        content->Add(text, 0, wxEXPAND | wxBOTTOM, gap);
    }

    if (!request_.details.empty()) {
        if (!request_.detailsLabel.empty()) {
            auto* label = new wxStaticText(contentWindow_, wxID_ANY,
                                           wxString(request_.detailsLabel));
            wxFont labelFont = label->GetFont();
            labelFont.SetWeight(wxFONTWEIGHT_BOLD);
            label->SetFont(labelFont);
            label->SetName(wxString(request_.detailsLabel));
            label->Wrap(wrapWidth);
            content->Add(label, 0, wxEXPAND | wxBOTTOM, smallGap);
        }
        const int detailsHeight = std::min(FromDIP(220), std::max(FromDIP(90), maxHeight / 3));
        detailsText_ = new wxTextCtrl(contentWindow_, wxID_ANY, wxString(request_.details),
                                      wxDefaultPosition, wxSize(contentWidth, detailsHeight),
                                      wxTE_MULTILINE | wxTE_READONLY | wxBORDER_SIMPLE);
        detailsText_->SetName(request_.detailsLabel.empty() ? wxS("dialog-details")
                                                            : wxString(request_.detailsLabel));
        detailsText_->SetMinSize(wxSize(contentWidth, detailsHeight));
        content->Add(detailsText_, 0, wxEXPAND | wxBOTTOM, gap);
    }

    if (!request_.question.empty()) {
        questionText_ = new wxStaticText(contentWindow_, wxID_ANY, wxString(request_.question));
        wxFont questionFont = questionText_->GetFont();
        questionFont.SetWeight(wxFONTWEIGHT_BOLD);
        questionText_->SetFont(questionFont);
        questionText_->SetName(wxString(request_.question));
        questionText_->Wrap(wrapWidth);
        content->Add(questionText_, 0, wxEXPAND | wxBOTTOM, gap);
    }

    if (!request_.footer.empty()) {
        footerText_ = new wxStaticText(contentWindow_, wxID_ANY, wxString(request_.footer));
        footerText_->SetName(wxString(request_.footer));
        footerText_->Wrap(wrapWidth);
        content->Add(footerText_, 0, wxEXPAND | wxBOTTOM, gap);
    }

    content->SetMinSize(wxSize(contentWidth, -1));
    contentWindow_->SetSizer(content);
    contentWindow_->SetMinSize(wxSize(contentWidth, std::min(FromDIP(320), maxHeight / 2)));
    content->Layout();
    contentWindow_->FitInside();
    outer->Add(contentWindow_, 1, wxEXPAND | wxLEFT | wxRIGHT, margin);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();

    if (request_.showDecline) {
        declineButton_ = new wxButton(this, wxID_NO, wxString(request_.declineLabel));
        declineButton_->SetName(wxString(request_.declineLabel));
        declineButton_->Bind(wxEVT_BUTTON, &WxGuiDialog::OnDecline, this);
        buttons->Add(declineButton_, 0, wxRIGHT, smallGap);
    }

    acceptButton_ = new wxButton(this, wxID_YES, wxString(request_.acceptLabel));
    acceptButton_->SetName(wxString(request_.acceptLabel));
    acceptButton_->Bind(wxEVT_BUTTON, &WxGuiDialog::OnAccept, this);
    buttons->Add(acceptButton_, 0);

    outer->Add(buttons, 0, wxEXPAND | wxALL, margin);

    SetSizer(outer);
    Layout();
    Fit();

    wxSize size = GetSize();
    size.SetWidth(std::min(size.GetWidth(), maxWidth));
    size.SetHeight(std::min(size.GetHeight(), maxHeight));
    SetSize(size);
    SetMinSize(wxSize(std::min(FromDIP(420), maxWidth), std::min(FromDIP(300), maxHeight)));
    CentreOnScreen();

    // 默认焦点与默认按钮都在"否"上：回车不能变成一次意外的同意。
    if (declineButton_) {
        SetDefaultItem(declineButton_);
        // 窗口还没显示时 SetFocus 未必生效；wxEVT_SHOW 里再落一次焦点。
        declineButton_->SetFocus();
    } else {
        SetDefaultItem(acceptButton_);
        acceptButton_->SetFocus();
    }
}

GuiDialogResult WxGuiDialog::Result() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return result_;
}

void WxGuiDialog::SetDeadline(std::function<bool()> deadlineReached) {
    deadlineReached_ = std::move(deadlineReached);
}

bool WxGuiDialog::RequestClose(GuiDialogOutcome forcedOutcome) {
    if (forcedOutcome != GuiDialogOutcome::kTimedOut &&
        forcedOutcome != GuiDialogOutcome::kCancelled) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (answered_) return true;
        if (pendingClose_) return true;   // 先到的理由生效；重复请求不是新决定
        pendingClose_ = forcedOutcome;
    }
    // 从后台线程排一个事件进 GUI 事件队列；真正的 EndModal 只在 GUI 主线程发生。
    wxQueueEvent(this, new wxCommandEvent(EVT_GUI_REQUEST_CLOSE));
    return true;
}

void WxGuiDialog::EndWith(GuiDialogOutcome outcome) {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (answered_) return;
        answered_ = true;
        result_.outcome = outcome;
    }
    if (closePollTimer_) closePollTimer_->Stop();
    EndModal(wxID_OK);
}

void WxGuiDialog::ApplyPendingClose() {
    GuiDialogOutcome forced = GuiDialogOutcome::kTimedOut;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (answered_ || !pendingClose_) return;
        forced = *pendingClose_;
    }
    EndWith(forced);
}

void WxGuiDialog::OnAccept(wxCommandEvent&) {
    // 截止时刻已经过了就按超时收尾 —— 哪怕这一下点击与到点在同一瞬间。
    if (deadlineReached_ && deadlineReached_()) {
        EndWith(GuiDialogOutcome::kTimedOut);
        return;
    }
    EndWith(GuiDialogOutcome::kAccepted);
}

void WxGuiDialog::OnDecline(wxCommandEvent&) {
    if (deadlineReached_ && deadlineReached_()) {
        EndWith(GuiDialogOutcome::kTimedOut);
        return;
    }
    EndWith(GuiDialogOutcome::kDeclined);
}

void WxGuiDialog::OnClose(wxCloseEvent&) {
    if (deadlineReached_ && deadlineReached_()) {
        EndWith(GuiDialogOutcome::kTimedOut);
        return;
    }
    EndWith(GuiDialogOutcome::kClosed);
}

void WxGuiDialog::OnCharHook(wxKeyEvent& event) {
    if (event.GetKeyCode() == WXK_ESCAPE) {
        if (deadlineReached_ && deadlineReached_()) {
            EndWith(GuiDialogOutcome::kTimedOut);
        } else {
            EndWith(GuiDialogOutcome::kClosed);
        }
        return;
    }
    event.Skip();
}

void WxGuiDialog::OnRequestCloseEvent(wxCommandEvent&) { ApplyPendingClose(); }

void WxGuiDialog::OnClosePoll(wxTimerEvent&) {
    // 后备通道：即使某个后台线程投递的事件在窗口刚创建时丢了，轮询仍会把
    // 已经记下的"到点 / 取消"在 GUI 线程上兑现。这是事件驱动，不是忙等。
    ApplyPendingClose();
}

}  // namespace ecapture
