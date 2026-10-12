#pragma once
// 输出策略
//   --help / --version / 未给任何条件  -> 纯文本（简短，人读）
//   其余：解析结果、截图结果、错误      -> JSON，只装"捕获到的窗口 + 保存的文件"
//
// JSON 固定形状（不写任何程序名/版本/schema 之类元信息）：
// {
//   "captured": 1,
//   "images": [ { "file","bytes","width","height","format","source",
//                 "hwnd","pid","title","class","elapsedMs",
//                 "history": { "status","file"?,"code"? } } ],
//   "errors": [ { "code", "message"?, "option"?, "value"?, "hint"?,
//                 "target"?, "backend"?, "stage"?, "hresult"?, "win32"? } ],   // 仅有错时出现
//   "notes":  [ 同上 ]                                                     // 仅有提示时出现
//   "input":  { ... }                                                      // 仅 --verbose：规范化后的输入
// }
//
// 规则：captured 与 images 恒在（空时是 []）；errors 只要非空就一定输出（--quiet 也不抑制），
// notes 仅非空且未 --quiet 时出现，input 仅 --verbose 出现。所以调用方先看 errors 再读 images。
// images[].history 是**主交付之外那一次独立拷贝**的下落（默认开启，位置跟着实际运行的程序目录）：
// 主图已经落地时这一格只说副本，failed / skipped 都改不到 file / captured / bytes 那三件，
// 而"图交出去了、副本没交出去"这一种部分成功由退出码 7 与 errors 里那条 history.* 一起说。
// 这里读到的 opt.quiet 已经是解析层定过的最终值：--verbose 与 --quiet 同时给出时按 --verbose
// 处理（quiet 归 false 并留一条 note.flag_overrides_quiet），所以"notes 为什么还在"永远有据可查。
// 退出码始终有效：0 成功 / 1 参数错 / 2 未给条件 /
// 3 --help / 4 无匹配 / 5 多匹配 / 6 受保护 / 7 截图失败 / 8 写文件失败 / 9 内部异常。
// 通道：默认全部写 stdout；一旦 -o -（图片占用标准输出）JSON 改走 stderr。
// 连渲染结果本身都异常时的兜底诊断也一律走 stderr —— 那时无法确定图片是否已经占了 stdout
// （也不为此再解析一遍 argv），保守选择是不碰 stdout。结果送不到约定通道时返回 8，
// 即使另一条流写成功也不改回 0：调用方按约定流读，读不到就是失败。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "OutputStream.h"  // 标准流的两条通道与"stdout 归谁"：判据在这里，动作在各平台服务里
#include "core/Error.h"   // NativeErrorCode（写标准输出的错误码原值）

namespace ecapture {

struct Response {
    std::wstring body;      // 已带结尾换行
    int exitCode = 0;
    bool toStderr = false;  // true => body 写 stderr
};

// 结果 JSON 该去哪条流：图片占了 stdout 时只能是 stderr，其余情况走 stdout。
// 兜底诊断也用它，避免在入口与这里各写一遍判断。
bool ResultGoesToStderr(const Options& opt);

// 根据解析结果生成响应；返回退出码。参数不必再传一遍：解析层交回来的 ParseResult 已经装了
// 全部规范化后的输入（-v 的 input 回显读的就是它），再拿着 argv 解析一次只会得出第二种答案。
int BuildResponse(const ParseResult& parse, Response* out);

// 纯文本帮助，由选项目录生成（不重复维护一份文字）。这三份文本与桌面无关，
// 实现在 src/CliText.cpp：没有截图后端、也没有图形会话的构建里照样能跑。
std::wstring HelpText();
std::wstring VersionText();
// --show-config 的只读视图：来源与文件位置全部读解析层已经算好的那一份，这里不重读文件、
// 不重判优先级，也一个像素都不取。
std::wstring RenderConfigView(const Options& opt);

}  // namespace ecapture
