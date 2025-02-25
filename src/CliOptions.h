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

// ---------------------------------------------------------------------------
// 结构化诊断：所有对外输出都是 JSON，因此每条信息都带稳定 code，
// message 只是给人看的补充。code 一旦发布只能追加，不要改名。
// ---------------------------------------------------------------------------
namespace codes {
// 参数层（退出码 1）
inline constexpr const wchar_t* kUnknownOption = L"cli.unknown_option";
inline constexpr const wchar_t* kMissingValue = L"cli.missing_value";
inline constexpr const wchar_t* kSwitchTakesNoValue = L"cli.switch_takes_no_value";
inline constexpr const wchar_t* kInvalidNumber = L"cli.invalid_number";
inline constexpr const wchar_t* kInvalidRegex = L"cli.invalid_regex";
inline constexpr const wchar_t* kInvalidValue = L"cli.invalid_value";
inline constexpr const wchar_t* kInvalidFormat = L"cli.invalid_format";
inline constexpr const wchar_t* kUnrecognizedExtension = L"cli.unrecognized_extension";
inline constexpr const wchar_t* kUnexpectedPositional = L"cli.unexpected_positional";
inline constexpr const wchar_t* kMissingOutput = L"cli.missing_output";
inline constexpr const wchar_t* kDuplicateOutput = L"cli.duplicate_output";
inline constexpr const wchar_t* kConflictingOptions = L"cli.conflicting_options";
inline constexpr const wchar_t* kInternalError = L"cli.internal_error";
// 参数层（退出码 2 / 3）
inline constexpr const wchar_t* kNoCondition = L"cli.no_condition";
// 提示（不影响退出码）
inline constexpr const wchar_t* kDuplicateValue = L"note.duplicate_value";
inline constexpr const wchar_t* kExtensionAppended = L"note.extension_appended";
inline constexpr const wchar_t* kExeLooksLikeName = L"note.exe_path_looks_like_name";
inline constexpr const wchar_t* kFormatExtensionMismatch = L"note.format_extension_mismatch";
inline constexpr const wchar_t* kQualityIgnored = L"note.quality_ignored";
inline constexpr const wchar_t* kAllWithoutPlaceholder = L"note.all_without_placeholder";
inline constexpr const wchar_t* kFlagOverridesQuiet = L"note.flag_overrides_quiet";
inline constexpr const wchar_t* kPipeDefaultFormat = L"note.pipe_default_format";
inline constexpr const wchar_t* kJsonFlagDeprecated = L"note.json_flag_deprecated";
inline constexpr const wchar_t* kHelpIgnoredArguments = L"note.help_ignored_arguments";
// 后续阶段
inline constexpr const wchar_t* kNoWindow = L"match.no_window";
inline constexpr const wchar_t* kAmbiguousWindow = L"match.ambiguous_window";
inline constexpr const wchar_t* kAccessDenied = L"capture.access_denied";
inline constexpr const wchar_t* kCaptureFailed = L"capture.failed";
inline constexpr const wchar_t* kWriteFailed = L"io.write_failed";
}  // namespace codes

// option / value 为空时序列化为 null；hint 用于"是不是想输入 --title"这类纠正建议。
struct Diagnostic {
    std::wstring code;
    std::wstring message;
    std::wstring option;
    std::wstring value;
    std::wstring hint;
};

struct ParseResult {
    bool ok = false;
    Options options;
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> warnings;
};

// 选项目录：--help 的 JSON 与人类可读文本都由它生成，是 CLI 契约的唯一来源。
struct OptionInfo {
    std::wstring name;         // 规范长名，不含前导 -
    std::wstring shortName;    // 单字母别名，可为空
    bool takesValue = false;
    std::wstring group;        // match / pick / output / behavior
    std::wstring valueHint;    // 例如 "<完整路径>"；开关为空
    std::vector<std::wstring> allowedValues;  // 取值枚举，可为空
    std::wstring description;
};

const std::vector<OptionInfo>& OptionCatalog();

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
    EX_INTERNAL = 9,       // 未预期的内部异常
};

// 版本与阶段：只在 --version 文本里出现；JSON 不携带任何程序元信息
inline constexpr const wchar_t* kVersion = L"0.3.0";
// 本阶段只到参数解析；截图实现后改成 "capture"
inline constexpr const wchar_t* kStage = L"cli-parsing-only";

const wchar_t* FormatName(ImageFormat format);  // "png" / "jpeg" / ...
const wchar_t* MultiKey(MultiMatch m);          // "ask" / "index" / "newest" / "oldest" / "all"
const wchar_t* MultiDescription(MultiMatch m);  // 中文说明，用于人读字段

}  // namespace ecapture
