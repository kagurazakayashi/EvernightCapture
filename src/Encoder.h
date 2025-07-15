#pragma once
// BGRA 像素 -> 图片文件字节。用 Windows.Graphics.Imaging.BitmapEncoder（与 WGC 同一套依赖，
// 且 JPEG 质量接口明确；WIC 的编码器属性袋 CLSID 在当前 SDK 里没有公开声明）。

#include <cstdint>
#include <string>
#include <vector>

#include "CaptureCommon.h"
#include "CliOptions.h"
#include "Deadline.h"

namespace ecapture {

// 把 frame 编码成 fmt 指定的格式，结果写进 bytes。
// dl 是这一次运行的总预算：编码的每一步异步等待都只按"还剩多少"来等，并且到点会向 WinRT
// 编码器**请求取消**（IAsyncInfo::Cancel）而不是干等 —— 但取消是合作式的，组件自己不回应时
// 我们只能留下那条等待自行结束（期限已经报出去了）。
bool EncodeFrame(const CapturedFrame& frame, ImageFormat fmt, int jpegQuality, const Deadline& dl,
                 std::vector<uint8_t>* bytes, Diagnostic* err);

// 本机可编码的格式名（用于错误提示）
std::wstring AvailableFormats();

}  // namespace ecapture
