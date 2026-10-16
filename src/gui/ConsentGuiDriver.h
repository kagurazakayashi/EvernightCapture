#pragma once
// 授权状态机与 wxWidgets 确认框之间的接缝。
//
// 这个头不 include 任何 wx 头：Consent.cpp / ConsentPrompt.cpp 只认识 DialogDriver 与
// 这一条工厂函数；真正创建 wxDialog 的代码在 src/gui/WxConsentDriver.cpp。

#include <memory>

#include "Consent.h"
#include "gui/Gui.h"

namespace ecapture {

std::shared_ptr<DialogDriver> MakeWxConsentDialogDriver(GuiDialogRequest request);

}  // namespace ecapture
