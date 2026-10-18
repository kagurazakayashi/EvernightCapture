// 授权确认框的"文案与结构"这一层：把 ConsentQuestion 渲染成跨平台 GUI 请求。
//
// 本文件不 include 任何 wx 头，也不创建窗口；真正的 wxWidgets 对话框在
// src/gui/WxConsentDriver.cpp / WxGuiDialog.cpp。内容构造（含可信 / 不可信来源的分开展示与
// 外部文本净化）在 src/ConsentDialog.cpp，那样没有 GUI 的判据目标也能逐条判它。
#include "Consent.h"

#include "ConsentDialog.h"
#include "gui/ConsentGuiDriver.h"
#include "gui/Gui.h"

namespace ecapture {

ConsentReply DialogConsentPrompt::Ask(const ConsentQuestion& q) {
    GuiDialogRequest request = BuildConsentDialogRequest(q, timeoutMs_);
    return RunMonitoredDialog(MakeWxConsentDialogDriver(std::move(request)), timeoutMs_,
                              cancelToken_);
}

}  // namespace ecapture
