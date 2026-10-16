// 【GUI · wxWidgets】GUI 上下文的按需初始化、主线程约束与通知接口。
//
// 这里不做任何授权判断，也不取像素。初始化只在第一次真的要显示窗口时发生，且绑定到
// 调用它的那条线程（本程序的 CLI 正文是单线程的，所以那就是 GUI 主线程）。
// 后台线程只能通过 GuiPostToMainThread / ConsentCancelToken 把消息交过来。
#include "Gui.h"

#include <wx/app.h>
#include <wx/init.h>
#include <wx/notifmsg.h>
#include <wx/string.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "WxGuiDialog.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ecapture {
namespace {

class GuiApp final : public wxApp {
public:
    bool OnInit() override { return true; }
    int OnExit() override { return 0; }
};

struct RuntimeState {
    bool started = false;
    bool failed = false;
    std::thread::id thread;
    std::wstring detail;
    NativeErrorCode nativeError = kNativeErrorNone;
};

std::mutex g_mutex;
RuntimeState g_state;
bool g_atexitRegistered = false;
std::atomic<bool> g_shuttingDown{false};

NativeErrorCode LastNativeError() {
#ifdef _WIN32
    return static_cast<NativeErrorCode>(GetLastError());
#else
    return static_cast<NativeErrorCode>(errno);
#endif
}

void ShutdownAtExit() { GuiRuntimeShutdown(); }

void RememberFailure(const wchar_t* detail, NativeErrorCode nativeError) {
    g_state.failed = true;
    g_state.detail = detail ? detail : L"";
    g_state.nativeError = nativeError;
}

bool OnGuiThread() {
    return g_state.started && g_state.thread == std::this_thread::get_id();
}

}  // namespace

bool GuiRuntimeEnsureStarted(std::wstring* detail, NativeErrorCode* nativeError) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_state.started) {
        if (g_state.thread != std::this_thread::get_id()) {
            if (detail) *detail = L"GUI 只能在首次初始化它的主线程上使用";
            if (nativeError) *nativeError = kNativeErrorNone;
            return false;
        }
        if (detail) detail->clear();
        if (nativeError) *nativeError = kNativeErrorNone;
        return true;
    }
    if (g_state.failed) {
        if (detail) *detail = g_state.detail;
        if (nativeError) *nativeError = g_state.nativeError;
        return false;
    }
    if (g_shuttingDown.load()) {
        if (detail) *detail = L"GUI 上下文正在收尾";
        if (nativeError) *nativeError = kNativeErrorNone;
        return false;
    }

    if (!g_atexitRegistered) {
        std::atexit(&ShutdownAtExit);
        g_atexitRegistered = true;
    }

    // wxEntryStart 只在这一条线程上调用；之后所有窗口也都在这条线程上创建与销毁。
    // argv 只是一份进程名，wx 不拿它解析本程序的选项。
    int argc = 1;
    wxChar arg0[] = wxT("ecapture");
    wxChar* argv[] = {arg0, nullptr};
    wxApp::SetInstance(new GuiApp());

    if (!wxEntryStart(argc, argv)) {
        const NativeErrorCode code = LastNativeError();
        RememberFailure(L"wxWidgets 初始化失败（没有可用的图形会话）", code);
        if (detail) *detail = g_state.detail;
        if (nativeError) *nativeError = code;
        return false;
    }

    if (!wxTheApp->CallOnInit()) {
        const NativeErrorCode code = LastNativeError();
        // OnInit 失败时 wx 的套间可能只初始化了一半；按它自己的收尾顺序退出。
        wxTheApp->OnExit();
        wxEntryCleanup();
        RememberFailure(L"wxWidgets 的 OnInit 失败", code);
        if (detail) *detail = g_state.detail;
        if (nativeError) *nativeError = code;
        return false;
    }

    g_state.started = true;
    g_state.thread = std::this_thread::get_id();
    g_state.detail.clear();
    g_state.nativeError = kNativeErrorNone;
    if (detail) detail->clear();
    if (nativeError) *nativeError = kNativeErrorNone;
    return true;
}

bool GuiRuntimeStarted() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.started;
}

std::wstring GuiRuntimeFailureDetail() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.detail;
}

GuiDialogResult ShowGuiDialog(const GuiDialogRequest& request) {
    GuiDialogResult result;
    std::wstring detail;
    NativeErrorCode nativeError = kNativeErrorNone;
    if (!GuiRuntimeEnsureStarted(&detail, &nativeError)) {
        result.outcome = GuiDialogOutcome::kInitFailed;
        result.nativeError = nativeError;
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!OnGuiThread()) {
            result.outcome = GuiDialogOutcome::kInitFailed;
            return result;
        }
    }

    try {
        WxGuiDialog dialog(nullptr, request);
        dialog.ShowModal();
        return dialog.Result();
    } catch (...) {
        // 窗口创建失败或 wx 抛出异常：没有人能回答"是"，按初始化失败处理。
        result.outcome = GuiDialogOutcome::kInitFailed;
        result.nativeError = LastNativeError();
        return result;
    }
}

GuiNotifyResult ShowGuiNotification(const GuiNotification& notification) {
    GuiNotifyResult result;
    std::wstring detail;
    NativeErrorCode nativeError = kNativeErrorNone;
    if (!GuiRuntimeEnsureStarted(&detail, &nativeError)) {
        result.status = GuiNotifyStatus::kUnavailable;
        result.nativeError = nativeError;
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!OnGuiThread()) {
            result.status = GuiNotifyStatus::kUnavailable;
            return result;
        }
    }

#if wxUSE_NOTIFICATION_MESSAGE
    try {
        wxNotificationMessage message(wxString(notification.title), wxString(notification.body));
        if (message.Show(wxNotificationMessage::Timeout_Auto)) {
            result.status = GuiNotifyStatus::kSubmitted;
            return result;
        }
    } catch (...) {
        result.nativeError = LastNativeError();
    }
    result.status = GuiNotifyStatus::kFailed;
    return result;
#else
    // 这个构建没有编译 wxNotificationMessage：如实说"这条平台路径不可用"，
    // 而不是假装提交成功。
    result.status = GuiNotifyStatus::kUnavailable;
    return result;
#endif
}

bool GuiPostToMainThread(std::function<void()> task) {
    if (!task) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.started || g_state.failed) return false;
    // CallAfter 内部走 QueueEvent，可以从后台线程调用；任务只在 GUI 事件循环里执行。
    wxTheApp->CallAfter(std::move(task));
    return true;
}

void GuiRuntimeShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_state.started) return;
    g_shuttingDown.store(true);
    g_state.started = false;
    if (wxTheApp) {
        wxTheApp->OnExit();
        wxEntryCleanup();
    }
    g_state.thread = std::thread::id();
}

}  // namespace ecapture
