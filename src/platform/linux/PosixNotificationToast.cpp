// 【平台层 · Linux】桌面通知的占位实现。
//
// X11 本身没有跨桌面的通知协议：libnotify / org.freedesktop.Notifications 是另一层
// 依赖，本步没有接入。这里如实交回"没有交出去"，不伪造 submitted，也不把通知失败
// 记成截图失败（见 Notification.h 的五条规矩）。
//
// 调用方（Report.cpp）在 status 为 kFailed 时只记一条 note，不改交付事实与退出码。

#include "Notification.h"

namespace ecapture {

bool ShowToastViaWinrt(const NotifyPayload& payload, NativeErrorCode* hr) {
    (void)payload;
    if (hr) *hr = kNativeErrorNone;
    return false;
}

}  // namespace ecapture
