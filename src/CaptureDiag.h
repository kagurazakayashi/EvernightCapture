#pragma once
// 取帧那一段诊断的**共用**骨架：稳定码、--capture 那一对取值、通道名与 stage 的组合只写这一处。
//
// 为什么搬出来（跨平台图像处理与编码这一步）：判据本体在共用层的三处（HdrColor 的 HDR 闸门、
// CaptureScope 的像素范围问答、将来的 X11 / Portal 后端）都要产出一条同形状的诊断，而它原来
// 长在 CaptureCommon.h —— 那一份是【平台层 · Windows】的 GDI / D3D 件。共用层为了拼一句
// 文案去 include 平台头，等于把刚划开的边界从中间断开（AGENTS.md《结构与依赖边界》）。
//
// Windows 那一条 CaptureError(...) 仍然存在、签名与行为一字不改：它在这份骨架之后**追加**
// GetLastError 的原值与一次 COM 调用的返回值。那两格是平台事实，只有平台层拿得到，
// 所以它们留在那一层，不伪装成跨平台字段（core/Error.h 那条"数值空间只作核对不作分支"同源）。

#include <string>

#include "CliOptions.h"   // Diagnostic / codes:: / stages::

namespace ecapture {

// 统一的取帧诊断：option = --capture，value 与 backend 都是这条通道的名字，stage = capture。
// code 默认 capture.failed；帧超时、窗口消失与 HDR 那几条各有自己的稳定码
//（调用方的处理方式不同：前者等一会儿还能重试，后者要重新枚举窗口）。
// target 由流水线补（只有它知道当前处理的是哪个目标）。
void CaptureDiag(Diagnostic* err, const wchar_t* channel, const std::wstring& message,
                 const std::wstring& hint, const wchar_t* code = codes::kCaptureFailed);

}  // namespace ecapture
