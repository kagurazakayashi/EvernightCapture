#pragma once
// DWM 缩略图取帧通道（--capture dwm）。

#include <cstdint>
#include <string>

#include "Consent.h"
#include "CaptureWgc.h"

namespace ecapture {

// 用 DwmRegisterThumbnail 把源窗口的 DWM 缓存面画到一个临时目标窗口上，再读回那块画面。
// DWM 缓存里是被遮挡窗口自己的内容，所以源窗口在后台也能截。
//
// 这个通道有两条内部路径，风险级别不同，因此要把授权判定器传进来：
//   * 主路径：临时宿主窗口摆在屏幕外，用 PrintWindow 读 DWM 的重定向位图 —— 屏幕上毫无动静，
//     属于窗口内容路径（dwm.thumbnail）。
//   * 退路：宿主窗口盖到目标位置上、从屏幕上把那块矩形拷回来（dwm.screen）—— 读的是桌面像素，
//     走之前必须回 gate 重新要一次桌面凭证，--yes 免不掉它。
bool CaptureWindowDwmThumbnail(uint64_t hwnd, uint32_t timeoutMs, ConsentGate& gate,
                               const std::wstring& targetKey, CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
