// 【平台层 · Windows】WinRT toast 那份发送器实现（Notification.h 的接缝在这里落地；wxWidgets 版是后续步骤）。
#include "Notification.h"

#include "platform/windows/WinrtApartment.h"

#include <winrt/base.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

namespace ecapture {

// 生产那一条发送路径：把渲染好的 XML 交给系统的通知平台。
//
// 这里用的是**显式传 AUMID** 的那一个入口（CreateToastNotifier(applicationId)），而不是
// "看当前进程身份"的那一条：本工具的进程 AUMID 取决于它从哪儿被启动（开始菜单快捷方式、
// 某个 shell、AI 的执行环境），拿它去对号会在开发目录下必然对不上。显式传那一份与安装器
// 逐字相同的字符串，两种现场走的是同一条判据。
//
// 发不出去一律是"这一次没弹"，绝不升级为截图失败：调用方拿到 false 与这一次自己的 HRESULT，
// 剩下的事在 SendCaptureNotifications 里汇总成一条 note。
bool ShowToastViaWinrt(const NotifyPayload& payload, NativeErrorCode* hr) {
    if (hr) *hr = kNativeErrorNone;

    // 套间按线程建（见 WinrtApartment.h：进程级"只初始化一次"是错的）。通知平台这一步跑在
    // 父进程的主线程上 —— 辅助子进程一个像素都不产，也就没有任何一条通知该从那里发。
    HRESULT initHr = 0;
    if (!EnsureWinrtOnThisThread(&initHr)) {
        if (hr) *hr = initHr;
        return false;
    }

    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        doc.LoadXml(winrt::hstring(RenderToastXml(payload)));

        auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager::
                            CreateToastNotifier(winrt::hstring(kNotifyAumid));
        winrt::Windows::UI::Notifications::ToastNotification toast(doc);
        notifier.Show(toast);
        return true;
    } catch (const winrt::hresult_error& e) {
        // 留住那一次调用自己的码：AUMID 没注册（REGDB_E_CLASSNOTREG 一类）、通知平台被策略关掉、
        // XmlDocument 收了不合法的文本、代理被组策略拦下，是四种不同的下一步。
        if (hr) *hr = e.code();
        return false;
    } catch (...) {
        // 异常对象本身不带 HRESULT。这里**不**编一个 E_FAIL 顶上去冒充"知道原因"：留下 0，
        // 让上层按"这条路说不通"（notify.unavailable）而不是"系统拒了"（notify.send_failed）归类。
        return false;
    }
}

}  // namespace ecapture
