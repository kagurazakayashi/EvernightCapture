#pragma once
// EvernightCapture - 命令行参数定义与解析
//
// 本文件（以及本阶段的全部代码）只负责"条件输入"：解析、校验、回显。
// 窗口查找与 Windows.Graphics.Capture 截图尚未实现。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ecapture {

// ---------------------------------------------------------------------------
// 匹配语义：同一个选项写多次 = OR；不同选项同时出现 = AND（全部满足才开始截图）。
// 例：--process notepad.exe --title-contains 报告
//     => 进程名是 notepad.exe 且 标题含"报告" 的窗口。
// ---------------------------------------------------------------------------
struct MatchOptions {
    std::vector<uint64_t> hwnds;            // --hwnd            10 进制或 0x 前缀 16 进制
    std::vector<uint32_t> pids;             // --pid             10 进制
    std::vector<std::wstring> processes;    // --process         仅映像名，忽略大小写
    std::vector<std::wstring> exePaths;     // --exe             完整路径，忽略大小写
    std::vector<std::wstring> titles;       // --title           标题精确匹配
    std::vector<std::wstring> titleContains;// --title-contains  标题子串匹配
    std::vector<std::wstring> titleRegexes; // --title-regex     标题正则（ECMAScript）
    std::vector<std::wstring> classes;      // --class           窗口类名，忽略大小写

    bool IsEmpty() const;
    std::size_t CountGroups() const;  // 出现过的条件种类数
    std::size_t CountValues() const;  // 条件值总数
};

enum class ImageFormat {
    kAuto, kPng, kJpeg, kBmp, kTiff, kGif, kWebp, kIco,
};

enum class MultiMatch {
    kAsk,     // 默认：匹配到多个窗口时报错，要求用户消歧
    kIndex,   // --index N，取第 N 个（1 起）
    kNewest,  // --newest
    kOldest,  // --oldest
    kAll,     // --all，每个窗口各存一张
};

struct Options {
    MatchOptions match;

    std::wstring output;              // 位置参数或 --out；"-" 表示写标准输出
    ImageFormat format = ImageFormat::kAuto;
    int jpegQuality = 90;             // --quality
    bool overwrite = true;            // --no-overwrite 置 false

    MultiMatch multi = MultiMatch::kAsk;
    int index = 1;                    // multi == kIndex 时有效

    bool dryRun = false;              // --dry-run：只解析并打印候选信息（本阶段的默认行为）
    bool json = false;                // --json
    bool verbose = false;             // -v / --verbose（本阶段默认开启详细回显，便于验证）
    bool quiet = false;               // -q / --quiet

    bool showHelp = false;            // --help，或"一个条件都没给"
    std::wstring helpReason;          // 非空 => 因为缺少条件而显示帮助
    bool showVersion = false;

    bool HasAnyCondition() const { return !match.IsEmpty(); }
};

struct ParseResult {
    bool ok = false;
    Options options;
    std::vector<std::wstring> errors;    // 非空 => ok == false
    std::vector<std::wstring> warnings;
    std::vector<std::wstring> suggestions;  // 例如拼写纠正
};

// 解析 argc/argv（宽字符，来自主动 --help 之外的 wmain）。
ParseResult ParseCommandLine(int argc, wchar_t* const* argv);

// 退出码
enum ExitCode : int {
    EX_OK = 0,             // 成功
    EX_USAGE = 1,          // 参数错误 / 缺少输出路径
    EX_NO_CONDITION = 2,   // 未指定任何条件（同时打印帮助）
    EX_HELP = 3,           // --help（打印帮助后返回）
    EX_NO_MATCH = 4,       // 没有匹配的窗口          （未实现）
    EX_AMBIGUOUS = 5,      // 匹配到多个窗口且未消歧  （未实现）
    EX_DENIED = 6,         // 被 DRM/权限拒绝         （未实现）
    EX_CAPTURE_FAILED = 7, // 截图失败                （未实现）
    EX_IO_FAILED = 8,      // 写文件失败              （未实现）
};

const std::wstring& HelpText();
const std::wstring& VersionText();

const wchar_t* FormatName(ImageFormat format);  // "png" / "jpeg" / ...
const wchar_t* MultiKey(MultiMatch m);          // "ask" / "index" / "newest" / "oldest" / "all"

// 供 --help 与错误提示复用：把内部字符串安全写到控制台。
void PrintLine(const std::wstring& text, bool toStdErr);

}  // namespace ecapture
