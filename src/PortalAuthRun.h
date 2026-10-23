#pragma once
// --portal-authorize 的编排与 JSON 渲染（授权 / 来源选择验证，不交付图像）。
//
// 这一路会弹一次 wxWidgets 程序确认，再打开系统 Portal 授权与来源选择窗口；两层都成功
// 之后也只报告“授权与选择链可用”。它不打开 PipeWire，不读像素，captured 恒为 0。

#include <string>

#include "CliOptions.h"

namespace ecapture {

struct PortalAuthRunResult {
    std::wstring body;
    int exitCode = EX_CAPTURE_FAILED;
};

PortalAuthRunResult RunPortalAuthorization(const Options& opt);

}  // namespace ecapture
