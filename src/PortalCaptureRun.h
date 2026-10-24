#pragma once
// --portal-capture=window|monitor 的编排：程序确认 -> Portal 系统授权/单一来源选择 ->
// 当前会话的 OpenPipeWireRemote() -> PipeWire 取一帧 -> 统一裁剪/缩放/编码/主交付/历史副本。
//
// 本入口只在 Linux 构建里链接；Windows 构建在解析期就拒绝这两个 Portal 动作。

#include "Capture.h"
#include "CliOptions.h"

namespace ecapture {

CaptureOutcome RunPortalCapture(const Options& opt);

}  // namespace ecapture
