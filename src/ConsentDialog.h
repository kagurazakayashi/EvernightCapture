#pragma once
// 授权确认框的**内容构造**接口（纯函数）：把 ConsentQuestion 变成 GuiDialogRequest。
//
// 为什么从 src/ConsentPrompt.cpp 里提出来：对话框文案与"可信 / 不可信怎么分开展示"是判据，
// 可以也必须在没有 wx 的目标里逐条判（tests/consent_state.cpp 就是这么做的）；真正弹框的
// 驱动仍然只在 src/gui/。
//
// 三条规矩：
//   1. 调用方自述（--purpose 之类）永远带"本程序没有核实"的标签，并且先过
//      core/DisplayText.h 的净化（控制字符 / 双向控制符 / 零宽字符 / 超长截断）。
//   2. 本程序核实的事实（映象路径、版本、进程号）单独一段展示，不能与自述混成一句。
//   3. 窗口标题、输出路径这类外部数据同样先净化；长清单仍按既有上限折叠。

#include <cstdint>

#include "Consent.h"
#include "gui/Gui.h"

namespace ecapture {

GuiDialogRequest BuildConsentDialogRequest(const ConsentQuestion& question, uint64_t timeoutMs);

}  // namespace ecapture
