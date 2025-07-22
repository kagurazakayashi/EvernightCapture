#pragma once
// DWM 缩略图取帧通道（--capture dwm）。

#include <cstdint>
#include <string>

#include "CaptureCommon.h"
#include "Consent.h"
#include "Deadline.h"
#include "WindowIdentity.h"

namespace ecapture {

// 用 DwmRegisterThumbnail 把源窗口的 DWM 缓存面画到一个临时目标窗口上，再读回那块画面。
// DWM 缓存里是被遮挡窗口自己的内容，所以源窗口在后台也能截。
//
// 这个通道有两条内部路径，风险级别不同，因此要把授权判定器传进来：
//   * 主路径：临时宿主窗口摆在屏幕外，用 PrintWindow 读 DWM 的重定向位图 —— 屏幕上毫无动静，
//     属于窗口内容路径（dwm.thumbnail）。这条**在辅助进程里跑**：PrintWindow 会同步等目标
//     窗口的线程，而那次等待没有中断点，只有把调用放进可结束的进程，期限才管用。
//   * 退路：宿主窗口盖到目标位置上、从屏幕上把那块矩形拷回来（dwm.screen）—— 读的是桌面像素，
//     所以留在父进程里，走之前必须回 gate 重新要一次桌面凭证，--yes 免不掉它。
//
// win 是选定目标那一刻的快照 + 复核要用的查询层。升级到屏幕退路之前必须照它做一次 kFull 复核：
// 这一次升级中间隔着"回 gate 重新问一次人"，而人点头批准的是当初那一扇窗口 —— 问完到把宿主
// 窗口摆上屏幕之间目标如果被销毁重建，读回来的就是没人批准过的画面。
bool CaptureWindowDwmThumbnail(uint64_t hwnd, uint32_t timeoutMs, ConsentGate& gate,
                               const std::wstring& targetKey, const WindowTarget& win,
                               const Deadline& dl, CapturedFrame* out, Diagnostic* err);

// 主路径那一段：建屏幕外宿主窗口 + 注册缩略图 + 泵 waitMs 毫秒 + PrintWindow 读回。
// 一个桌面像素都不读（宿主窗口在 -32000，屏幕上看不见），所以能被辅助进程直接调用。
// waitMs 是"DWM 要把缓存面合成进来需要几帧"的等待，调用方已经把剩余预算压进去了。
RenderOutcome RenderDwmThumbnailContent(uint64_t hwnd, uint32_t waitMs);

}  // namespace ecapture
