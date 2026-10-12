#pragma once
// 平台服务：标准输出的两条通道与"stdout 归谁"这件事。
//
// 声明共用（src/Report.cpp 的响应组装、src/Capture.cpp 的发图、src/Main.cpp 的兜底都读这一份），
// 实现分平台。通道规矩本身是共用的，平台只回答"这几个字节怎么送出去"：
//
//   * 文本走 UTF-8：内部文字（src/core/TextCoding.h 的约定）在出口处编码一次。
//     Windows 用 WideCharToMultiByte(CP_UTF8)，Linux 用 EncodeUtf8 —— 两边都不走 CRT 的
//     文本模式与 locale 转换（那会把中文按代码页截断，也会让 \n 变成别的东西）。
//   * 行尾由平台决定：内部文本只写 \n。Windows 那一路把整份输出归一成 CRLF（老式控制台
//     光按 \n 走会出现阶梯错位）；Linux 按 \n 原样写。这一条差别只影响人看的文本与 JSON 的
//     空白，两者对解析都不敏感。
//   * 图片字节走 stdout 之后，stdout 就归图片：此后 EmitStdout 一律返回 false 而不再写它，
//     结果 JSON 整份改走 stderr（见 Report.h 与 ClaimStdout）。这条边界是硬的 ——
//     一句本地化提示混进 PNG 的字节流里，那张图就再也读不出来。

#include <cstdint>
#include <string>
#include <vector>

#include "core/Error.h"   // NativeErrorCode（写标准输出的错误码原值）

namespace ecapture {

// 文本写标准输出 / 标准错误。返回 false = 没送到那条流（写失败，或 stdout 已被图片占用）。
bool EmitStdout(const std::wstring& text);
bool EmitStderrRaw(const std::wstring& text);

// 图片字节写 stdout；ioError 回收原生错误码原值（断管与磁盘满是两种故障，别混成一句话）。
// emittedBytes 回收**实际发出去**的字节数：返回 false 时它可能是 0（一个字节都没出去），
// 也可能停在中间（管道里留下的是半张图）。这两种现场调用方要分得开，不能都当成"什么都没写"。
// 第一次写之前就把 stdout 声明为图片专用，此后 EmitStdout 一律返回 false 而不再写它。
bool EmitStdoutBytes(const std::vector<uint8_t>& bytes, NativeErrorCode* ioError = nullptr,
                     uint64_t* emittedBytes = nullptr);

// stdout 是否已被图片字节占用（或明确将要用它发图）。为真时结果 JSON 只能走 stderr。
void ClaimStdout();
bool StdoutClaimed();

}  // namespace ecapture
