#pragma once
// 【平台接口】原生错误码 -> 给人看的短文本。
//
// 为什么单独一份：诊断 hint 里那两句（"系统错误码 N 是什么意思"与"一次 COM / WinRT 调用
// 返回了哪个 HRESULT"）原先分别长在 CaptureCommon.cpp（平台层）与 Worker.cpp 里，两处都
// 直接调 FormatMessage / 拼 0x%08X。共用层（src/Isolation.cpp 的 BlockedToDiagnostic）
// 要按稳定码拼同一份文案，所以"码怎么写成文字"必须从平台层接出来，而不是让共用层去
// include 一个 Windows 头。
//
// 两条边界：
//   1. 这里只把**码**变成文字，不做分支、不改稳定码、不决定下一步。
//   2. 数值空间按来源分开：NativeErrorText 认的是 GetLastError / errno 那一族，
//      HResultText 认的是一次 COM / WinRT 调用的返回值。跨平台时同一串数字在两边
//      含义不同，所以调用方必须用字段名说清来源（core/Error.h 的那条规矩）。

#include <cstdint>
#include <string>

#include "core/Error.h"   // NativeErrorCode

namespace ecapture {

// 系统错误码 -> 一句人话（Windows 走 Lang 的 err.win32_code 那一条；0 = 没有失败过，
// 调用方负责判断这一格有没有来源，这一层不把 0 换成空串）。
std::wstring NativeErrorText(NativeErrorCode code);

// 一次 COM / WinRT 调用的返回值 -> "0x........" 形状的文本（长度恒 10，与既有诊断逐字相同）。
// 名字带 Code 是有意的：平台层另有一个收 HRESULT 的同名薄包装（CaptureCommon.h 的
// HResultText），两个重载收的宽度不同、混在一起会被隐式转换选错。
std::wstring HResultCodeText(NativeErrorCode hr);

}  // namespace ecapture
