#pragma once
// EvernightCapture - 结构化窗口查询的真机问答那一步（--list / --inspect）
//
// 判据与渲染都在 WindowQuery.h（那一份全是纯函数，可以离线逐条注入）。这里只补上必须碰
// Win32 的那半件事：把条件求值跑一遍，拿到「当时那一屏窗口」的快照，再交给判据。
//
// 三条与截图链路同源的规矩，改之前先对齐：
//   1. **求值走同一条路线。** 用了 --title-regex 或设了 --timeout-ms 就整步交给本工具的辅助进程
//      （Worker.h），到点能结束；两者都没有时照旧在本进程枚举。列窗口不该比截图更容易被一扇
//      挂住的窗口拖住 —— GetWindowText 是往那个线程发消息并等它回。
//   2. **一个像素都不取。** 这一步不弹确认框、不调任何取帧通道、不写文件、不激活/恢复/移动
//      任何窗口。所以它不需要、也拿不到 DesktopPermit（Consent.h）。
//   3. **只在父进程里跑。** 报告、期限、退出码都归父进程；辅助进程里既没有文案也没有输出路径。

#include "CliOptions.h"
#include "WindowQuery.h"

namespace ecapture {

// 跑一次窗口查询：按屏限缩 -> 条件求值 -> 判据 -> 补上「快照会过期」那一条提示。
// errors 非空时 windows 为空，exitCode 已按那批稳定码定好（1 / 4 / 5 / 7）。
WindowQueryResult RunWindowQuery(const Options& opt);

}  // namespace ecapture
