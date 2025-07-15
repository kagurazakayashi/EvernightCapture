#pragma once
// PrintWindow 取帧通道（--capture printwindow）：让窗口自己画到 DC。

#include <cstdint>

#include "CaptureCommon.h"
#include "Deadline.h"

namespace ecapture {

// 先带 PW_RENDERFULLCONTENT 试（Win8.1+，能逼出部分硬件加速内容），
// 失败再退回普通 WM_PRINT。不接受自绘的窗口（游戏 / DirectComposition）会拿到空帧。
//
// 这条通道**一律在辅助进程里执行**：PrintWindow 把绘制请求发给目标窗口的线程并同步等它画完，
// 那个线程要是卡在自己的消息处理里，这个调用就没有返回的时候 —— 传进来的 timeoutMs 在过去
// 根本不起作用，因为没有中断点可查。放进可结束的进程里，期限才是期限（见 Worker.h）。
// 期限由调用方给：给了 --timeout-ms 就是剩余预算，没给就是本工具的内置上限。
bool CaptureWindowPrintWindow(uint64_t hwnd, const Deadline& dl, CapturedFrame* out, Diagnostic* err);

// 只把窗口自己画进新建的 DIB：读的是这个窗口自己的画面，一个桌面像素都不碰，
// 所以这条函数既能在本进程跑，也能被辅助进程直接调用（父进程才是授权的地方）。
// 失败只给原因码与系统错误码，文字由调用方按 --lang 现取。
RenderOutcome RenderPrintWindowContent(uint64_t hwnd);

}  // namespace ecapture
