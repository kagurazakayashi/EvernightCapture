// 【GUI · wxWidgets】授权确认框的驱动：在 GUI 主线程上创建模态框，在看守线程上只收
// "到点 / 取消"消息并转投给 GUI 事件队列。
#include "ConsentGuiDriver.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

#include "WxGuiDialog.h"

namespace ecapture {
namespace {

class WxConsentDialogDriver final : public DialogDriver {
public:
    explicit WxConsentDialogDriver(GuiDialogRequest request) : request_(std::move(request)) {}

    DialogResult Show() override {
        DialogResult result;

        std::wstring detail;
        NativeErrorCode nativeError = kNativeErrorNone;
        if (!GuiRuntimeEnsureStarted(&detail, &nativeError)) {
            result.answer = ConsentAnswer::kInitFailed;
            result.nativeError = nativeError;
            return result;
        }

        try {
            WxGuiDialog dialog(nullptr, request_);
            if (deadline_) {
                const auto deadline = *deadline_;
                dialog.SetDeadline([deadline] {
                    return std::chrono::steady_clock::now() >= deadline;
                });
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                dialog_ = &dialog;
            }
            dialog.ShowModal();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                dialog_ = nullptr;
            }

            const GuiDialogResult shown = dialog.Result();
            result.answer = MapOutcome(shown.outcome);
            result.nativeError = shown.nativeError;
            return result;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            dialog_ = nullptr;
            // 窗口没创建出来或 wx 抛异常：没有人能回答"是"。异常绝不跑出 GUI 主线程。
            result.answer = ConsentAnswer::kInitFailed;
            result.nativeError = kNativeErrorNone;
            return result;
        }
    }

    bool RequestClose(DialogCloseReason reason) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!dialog_) return false;
        const GuiDialogOutcome outcome = reason == DialogCloseReason::kTimedOut
                                             ? GuiDialogOutcome::kTimedOut
                                             : GuiDialogOutcome::kCancelled;
        return dialog_->RequestClose(outcome);
    }

    void SetDeadline(std::optional<std::chrono::steady_clock::time_point> deadline) override {
        deadline_ = deadline;
    }

private:
    static ConsentAnswer MapOutcome(GuiDialogOutcome outcome) {
        switch (outcome) {
            case GuiDialogOutcome::kAccepted: return ConsentAnswer::kAccepted;
            case GuiDialogOutcome::kDeclined: return ConsentAnswer::kDeclined;
            case GuiDialogOutcome::kClosed: return ConsentAnswer::kClosed;
            case GuiDialogOutcome::kTimedOut: return ConsentAnswer::kTimedOut;
            case GuiDialogOutcome::kCancelled: return ConsentAnswer::kCancelled;
            case GuiDialogOutcome::kInitFailed: return ConsentAnswer::kInitFailed;
        }
        return ConsentAnswer::kInitFailed;
    }

    GuiDialogRequest request_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
    std::mutex mutex_;
    WxGuiDialog* dialog_ = nullptr;
};

}  // namespace

std::shared_ptr<DialogDriver> MakeWxConsentDialogDriver(GuiDialogRequest request) {
    return std::make_shared<WxConsentDialogDriver>(std::move(request));
}

}  // namespace ecapture
