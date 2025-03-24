#pragma once
// BGRA 像素 -> 图片文件字节。用 Windows.Graphics.Imaging.BitmapEncoder（与 WGC 同一套依赖，
// 且 JPEG 质量接口明确；WIC 的编码器属性袋 CLSID 在当前 SDK 里没有公开声明）。

#include <cstdint>
#include <string>
#include <vector>

#include "CaptureCommon.h"
#include "CliOptions.h"

namespace ecapture {

// 把 frame 编码成 fmt 指定的格式，结果写进 bytes。
bool EncodeFrame(const CapturedFrame& frame, ImageFormat fmt, int jpegQuality,
                 std::vector<uint8_t>* bytes, Diagnostic* err);

// 本机可编码的格式名（用于错误提示）
std::wstring AvailableFormats();

}  // namespace ecapture
