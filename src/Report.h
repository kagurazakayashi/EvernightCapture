#pragma once
// 输出策略
//   --help / --version / 未给任何条件  -> 纯文本（简短，人读）
//   其余：解析结果、截图结果、错误      -> JSON，只装"捕获到的窗口 + 保存的文件"
//
// JSON 固定形状（不写任何程序名/版本/schema 之类元信息）：
// {
//   "captured": 1,
//   "images": [ { "file","bytes","width","height","format","source",
//                 "hwnd","pid","title","class","elapsedMs" } ],
//   "errors": [ { "code", "message"?, "option"?, "value"?, "hint"?,
//                 "target"?, "backend"?, "stage"?, "hresult"?, "win32"? } ],   // 仅有错时出现
//   "notes":  [ 同上 ]                                                     // 仅有提示时出现
//   "input":  { ... }                                                      // 仅 --verbose：规范化后的输入
// }
//
// 规则：captured 与 images 恒在（空时是 []）；errors 只要非空就一定输出（--quiet 也不抑制），
// notes 仅非空且未 --quiet 时出现，input 仅 --verbose 出现。所以调用方先看 errors 再读 images。退出码始终有效：0 成功 / 1 参数错 / 2 未给条件 /
// 3 --help / 4 无匹配 / 5 多匹配 / 6 受保护 / 7 截图失败 / 8 写文件失败 / 9 内部异常。
// 通道：默认全部写 stdout；一旦 -o -（图片占用标准输出）JSON 改走 stderr。
// 连渲染结果本身都异常时的兜底诊断也一律走 stderr —— 那时无法确定图片是否已经占了 stdout
// （也不为此再解析一遍 argv），保守选择是不碰 stdout。结果送不到约定通道时返回 8，
// 即使另一条流写成功也不改回 0：调用方按约定流读，读不到就是失败。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // DWORD（写标准输出的错误码原值）

#include "CliOptions.h"

namespace ecapture {

struct Response {
    std::wstring body;      // 已带结尾换行
    int exitCode = 0;
    bool toStderr = false;  // true => body 写 stderr
};

// 结果 JSON 该去哪条流：图片占了 stdout 时只能是 stderr，其余情况走 stdout。
// 兜底诊断也用它，避免在 Main.cpp / Report.cpp 各写一遍判断。
bool ResultGoesToStderr(const Options& opt);

// 根据解析结果生成响应；返回退出码。
int BuildResponse(const ParseResult& parse, int argc, wchar_t* const* argv, Response* out);

// 纯文本帮助，由选项目录生成（不重复维护一份文字）
std::wstring HelpText();
std::wstring VersionText();

bool EmitStdout(const std::wstring& text);
bool EmitStderrRaw(const std::wstring& text);
// 图片字节写 stdout；ioError 回收 GetLastError 原值（断管与磁盘满是两种故障，别混成一句话）。
// 第一次写之前就把 stdout 声明为图片专用，此后 EmitStdout 一律返回 false 而不再写它。
bool EmitStdoutBytes(const std::vector<uint8_t>& bytes, DWORD* ioError = nullptr);

// stdout 是否已被图片字节占用（或明确将要用它发图）。为真时结果 JSON 只能走 stderr。
void ClaimStdout();
bool StdoutClaimed();

}  // namespace ecapture
