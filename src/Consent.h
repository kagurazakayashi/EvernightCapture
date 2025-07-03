#pragma once
// 整屏截图前的人工确认：弹一个模态对话框，列出要截哪几块屏、走哪条通道、图去哪里，
// 只有用户点"是"才开始取帧。
//
// 刻意不提供命令行或环境变量旁路：任何"输出一整块屏幕"的取图都必须问人一次。
// 弹不出对话框（服务会话、没有交互桌面）也按拒绝处理，绝不悄悄截屏。

#include <vector>

#include "CliOptions.h"
#include "ScreenMatch.h"

namespace ecapture {

// true = 用户同意。false 时 *err 已填好（code 是 capture.access_denied，退出码 6）。
bool AskScreenCaptureConsent(const Options& opt, const std::vector<ScreenInfo>& screens,
                             Diagnostic* err);

}  // namespace ecapture
