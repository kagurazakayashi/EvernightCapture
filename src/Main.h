#pragma once
// 命令行正文的入口边界：平台入口 ↔ 共用的一次执行。
//
// 平台侧（Windows 的 wmain、Linux 上将来的 main）只做两件事：把本系统的 argv 换成 CliTokens
// （唯一的一次换码，见 src/CliArgs.h），以及分诊"辅助工作进程"那一种调用形态。剩下的都在这里：
// 解析 → 组装响应 → 按约定的那条流写出 → 交出退出码。
//
// 这一份正文不 include 任何平台头，也不做 GUI/桌面初始化：--help / --version / --show-config
// 这些与桌面无关的命令，走的也是这一条路。

#include "core/CliTokens.h"

namespace ecapture {

int CliMain(const CliTokens& tokens);

}  // namespace ecapture
