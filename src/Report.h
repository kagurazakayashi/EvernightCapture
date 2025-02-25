#pragma once
// 输出策略
//   --help / --version / 未给任何条件  -> 纯文本（简短，人读）
//   其余：解析结果、截图结果、错误      -> JSON，只装"捕获到的窗口 + 保存的文件"
//
// JSON 固定形状（不写任何程序名/版本/schema 之类元信息）：
// {
//   "captured": 1,
//   "images": [ { "file","bytes","width","height","format",
//                 "hwnd","pid","title","class","elapsedMs" } ],
//   "errors": [ { "code", "message"?, "option"?, "value"?, "hint"? } ],   // 仅有错时出现
//   "notes":  [ 同上 ]                                                     // 仅有提示时出现
//   "input":  { ... }                                                      // 仅 --verbose：规范化后的输入
// }
//
// 规则：captured 与 images 恒在（空时是 []）；errors 只要非空就一定输出（--quiet 也不抑制），
// notes 仅非空且未 --quiet 时出现，input 仅 --verbose 出现。所以调用方先看 errors 再读 images。退出码始终有效：0 成功 / 1 参数错 / 2 未给条件 /
// 3 --help / 4 无匹配 / 5 多匹配 / 6 受保护 / 7 截图失败 / 8 写文件失败 / 9 内部异常。
// 通道：默认全部写 stdout；一旦 -o -（图片占用标准输出）JSON 改走 stderr。

#include <string>

#include "CliOptions.h"

namespace ecapture {

struct Response {
    std::wstring body;      // 已带结尾换行
    int exitCode = 0;
    bool toStderr = false;  // true => body 写 stderr
};

// 根据解析结果生成响应；返回退出码。
int BuildResponse(const ParseResult& parse, int argc, wchar_t* const* argv, Response* out);

// 纯文本帮助，由选项目录生成（不重复维护一份文字）
std::wstring HelpText();
std::wstring VersionText();

bool EmitStdout(const std::wstring& text);
bool EmitStderrRaw(const std::wstring& text);

}  // namespace ecapture
