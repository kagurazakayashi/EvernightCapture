#pragma once
// 程序自有 GUI 的跨平台接口（Windows 与 Linux 共用同一份 wxWidgets 实现）。
//
// 本层只负责"把一段已经决定好的文字与几个按钮显示出来"。它不判授权、不取像素、不决定
// 要不要弹框 —— 那些仍在 ConsentGate / CaptureScope 那一侧。这样确认框、权限说明与必要的
// 错误提示共用同一套控件与同一套返回值，不会各写一份 Win32 / GTK / Qt 界面。
//
// 线程模型（改代码前先对齐这里）：
//   * GUI 上下文**按需**在第一次真正要显示窗口时创建，且只能在调用它的那条线程上创建；
//     本程序的 CLI 正文本身是单线程的，所以那条线程就是 GUI 主线程。帮助、版本、配置查询
//     与只读查询都不经过这里，也就不会初始化 wxWidgets。
//   * 对话框是模态的，ShowDialog 阻塞到窗口关闭并销毁之后才返回；wx 的事件循环就是这条
//     模态循环。没有常驻主窗口、托盘或后台服务。
//   * 后台线程（例如取帧任务或取消信号）**不得**直接操作控件。要关框请用
//     ConsentCancelToken（见 Consent.h）；要投递普通任务请走 GuiPostToMainThread()。
//   * 返回值把"同意 / 拒绝 / 关框 / 超时 / 初始化失败 / 程序取消"分成六档；窗口消失本身
//     永远不等于同意。
//
// 这里不 include 任何 wx 头：共用模型与授权状态机只认这份声明，wxWidgets 的头只允许出现在
// src/gui/ 的实现文件里。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Error.h"   // NativeErrorCode：初始化失败时原样带出系统那一侧的错误

namespace ecapture {

enum class GuiDialogSeverity {
    kInformation,
    kWarning,
    kError,
};

// 一次显示请求。文字已在调用方按 --lang 取好，本层不再查文案表：
//   * paragraphs —— 普通说明段（可换行、按宽度折行）
//   * details    —— 目标清单、输出路径这类需要逐行看清并允许复制的文本（只读多行框，
//                   长路径会在框内折行/滚动，不会把对话框撑出屏幕）
//   * footer     —— 期限、补充说明
//   * question   —— 加粗的那一句问题
//   * accept/declineLabel —— 按钮文字；showDecline=false 时只显示一个确认按钮
//     （必要的错误提示 / 通知型说明走这一种：它没有"拒绝"语义，只是让人看见并确认）
struct GuiDialogRequest {
    std::wstring title;
    std::wstring heading;
    std::vector<std::wstring> paragraphs;
    std::wstring detailsLabel;
    std::wstring details;
    std::wstring footer;
    std::wstring question;
    std::wstring acceptLabel;
    std::wstring declineLabel;
    GuiDialogSeverity severity = GuiDialogSeverity::kInformation;
    bool showDecline = true;
};

enum class GuiDialogOutcome {
    kAccepted,    // 人点了确认 / "是"
    kDeclined,    // 人点了拒绝 / "否"
    kClosed,      // 人按 X 或 Esc 关掉了窗口
    kTimedOut,    // 期限到点，由程序收尾（绝不改成同意）
    kInitFailed,  // GUI 初始化失败或窗口弹不出来
    kCancelled,   // 程序主动取消（后台任务 / 取消令牌），不是人的回答
};

struct GuiDialogResult {
    GuiDialogOutcome outcome = GuiDialogOutcome::kInitFailed;
    // outcome == kInitFailed 时才是"为什么起不来"的系统原值（Linux 上是 errno，
    // Windows 上是 GetLastError）；其余为 0。
    NativeErrorCode nativeError = kNativeErrorNone;
};

// 按需初始化 GUI 上下文。只能在主线程调用；失败时 *detail 写一句人能读懂的原因，
// *nativeError 写系统原值。重复调用是幂等的：成功过就一直成功，失败过就一直失败。
bool GuiRuntimeEnsureStarted(std::wstring* detail = nullptr,
                             NativeErrorCode* nativeError = nullptr);

// 只读查询，不触发初始化。测试与诊断用。
bool GuiRuntimeStarted();
std::wstring GuiRuntimeFailureDetail();

// 显示一个模态对话框，返回时窗口已经关闭并销毁。
// 必须在 GUI 主线程（也就是第一次成功初始化的那条线程）调用。
GuiDialogResult ShowGuiDialog(const GuiDialogRequest& request);

// 统一的跨平台通知接口（wxWidgets）。本步只把接口与实现接起来；截图完成后到底发什么、
// 什么时候发，留给结果通知那一步，生产路径当前没有调用这里。
//
// 与 Windows toast 那一路一样：提交成功只代表"交给系统的通知平台"，不证明真的显示过。
enum class GuiNotifyStatus { kSubmitted, kUnavailable, kFailed };

struct GuiNotification {
    std::wstring title;
    std::wstring body;
};

struct GuiNotifyResult {
    GuiNotifyStatus status = GuiNotifyStatus::kUnavailable;
    NativeErrorCode nativeError = kNativeErrorNone;
};

GuiNotifyResult ShowGuiNotification(const GuiNotification& notification);

// 把一个任务投递到 GUI 主线程。后台线程可以用它安排界面更新，而**不要**直接碰控件。
// 返回 false = GUI 上下文还没起来或任务为空（调用方据此决定是丢弃还是稍后重试）；
// 返回 true 只代表"已经排进事件队列"，任务会在下一次 GUI 事件循环里执行。
bool GuiPostToMainThread(std::function<void()> task);

// 进程退出前由 atexit 自动收尾；测试也可以显式调用，确认 wx 上下文被正确释放。
// 有窗口还开着时不要调用。
void GuiRuntimeShutdown();

}  // namespace ecapture
