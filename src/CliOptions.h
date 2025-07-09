#pragma once
// EvernightCapture - 命令行参数定义与解析
//
// 本文件（以及本阶段的全部代码）只负责"条件输入"：解析、校验、回显。
// 窗口查找与 Windows.Graphics.Capture 截图尚未实现。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Lang.h"
#include "Version.h"

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

// 编码格式。没有"auto"这个取值：不给 --format 就由输出文件扩展名决定，
// 扩展名也判不出时用 png（见 note.format_defaulted_png）。
// webp / ico 曾列在取值里，但当前 SDK 的 BitmapEncoder 没有对应编码器，已删除。
enum class ImageFormat {
    kPng, kJpeg, kBmp, kTiff, kGif,
};

enum class MultiMatch {
    kAsk,     // 默认：匹配到多个窗口时报错，要求用户消歧
    kIndex,   // --index N，取第 N 个（1 起）
    kNewest,  // --newest
    kOldest,  // --oldest
    kAll,     // --all，每个窗口各存一张
};

// 取图方式。--capture 选择，默认 wgc；互不替代，能力差别见帮助文本。
// magnification（Magnification API）实现过又被删了：Win11 的 magnification.dll 不再导出
// MagGetImage，没有离屏读图路线，只能把放大镜控件窗口摆到屏幕上抢 z 序，而拿到的画面与
// bitblt 等价，代价换不到收益。
enum class CaptureMethod {
    kWgc,           // Windows.Graphics.Capture：DWM 合成后的窗口面，被遮挡也能截
    kPrintWindow,   // PrintWindow + PW_RENDERFULLCONTENT：让窗口自绘到 DC
    kBitBlt,        // BitBlt 屏幕 DC：拷屏幕上该窗口矩形，只截得到可见部分
    kDwmThumbnail,  // DwmRegisterThumbnail：DWM 缓存表面，能截被遮挡窗口（Win7+）
    kDuplication,   // DXGI Desktop Duplication：整张显示器合成分，再按窗口矩形裁剪（Win8+）
    kAuto,          // 按 wgc -> dwm -> printwindow -> bitblt 依次回退
};

// --monitor 的取值域：给了这个选项就有屏幕目标。
//   无窗口条件  -> 整块屏幕截图（ordinal=0 用主屏，all 则每块屏各一张）
//   有窗口条件  -> 只算落在那块屏上的窗口
// 之所以要显式给 --monitor 才截屏，而不是"没条件就当全屏"：漏写条件的调用方
// 不该在无意间拍到整个桌面。
struct MonitorSelector {
    bool given = false;
    bool all = false;   // --monitor all
    int ordinal = 0;    // 1 起；0 = 未指定，用主屏
};

struct Options {
    MatchOptions match;
    MonitorSelector monitor;    // --monitor / -m

    std::wstring output;              // 位置参数或 --out；"-" 表示写标准输出
    bool outputImplicitStdout = false;  // 未给输出路径 => 按 "--out -" 处理，PNG 写标准输出
    ImageFormat format = ImageFormat::kPng;
    bool formatExplicit = false;      // 是否显式指定过 --format
    int jpegQuality = 100;            // --quality
    bool overwrite = true;            // --no-overwrite 置 false（=true 与裸开关同义，=false 取消）

    MultiMatch multi = MultiMatch::kAsk;
    int index = 1;                    // multi == kIndex 时有效

    CaptureMethod capture = CaptureMethod::kWgc;   // --capture，默认 Windows.Graphics.Capture
    bool captureExplicit = false;                  // 是否显式指定过 --capture

    bool dryRun = false;              // --dry-run：只解析并打印候选信息（本阶段的默认行为）
    bool json = false;                // --json
    bool verbose = false;             // -v / --verbose（本阶段默认开启详细回显，便于验证）
    bool quiet = false;               // -q / --quiet

    bool showHelp = false;            // --help，或"一个条件都没给"
    std::wstring helpReason;          // 非空 => 因为缺少条件而显示帮助
    bool showVersion = false;

    bool HasAnyCondition() const { return !match.IsEmpty(); }
    // 屏幕目标模式：截整块屏幕，而不是某个窗口的画面
    bool ScreenMode() const { return monitor.given && match.IsEmpty(); }
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
inline constexpr const wchar_t* kUnknownCaptureMethod = L"cli.unknown_capture_method";
inline constexpr const wchar_t* kUnknownLanguage = L"cli.unknown_language";
inline constexpr const wchar_t* kMonitorConflict = L"cli.monitor_conflict";
inline constexpr const wchar_t* kInternalError = L"cli.internal_error";
// 参数层（退出码 2 / 3）
inline constexpr const wchar_t* kNoCondition = L"cli.no_condition";
// 提示（不影响退出码）
inline constexpr const wchar_t* kDuplicateValue = L"note.duplicate_value";
inline constexpr const wchar_t* kExtensionAppended = L"note.extension_appended";
inline constexpr const wchar_t* kExeLooksLikeName = L"note.exe_path_looks_like_name";
inline constexpr const wchar_t* kFormatExtensionMismatch = L"note.format_extension_mismatch";
inline constexpr const wchar_t* kFormatDefaultedPng = L"note.format_defaulted_png";
inline constexpr const wchar_t* kOutputDefaultedStdout = L"note.output_defaulted_stdout";
inline constexpr const wchar_t* kOutputExtensionAppended = L"note.output_extension_appended";
inline constexpr const wchar_t* kQualityIgnored = L"note.quality_ignored";
inline constexpr const wchar_t* kAllWithoutPlaceholder = L"note.all_without_placeholder";
inline constexpr const wchar_t* kFlagOverridesQuiet = L"note.flag_overrides_quiet";
inline constexpr const wchar_t* kPipeDefaultFormat = L"note.pipe_default_format";
inline constexpr const wchar_t* kJsonFlagDeprecated = L"note.json_flag_deprecated";
inline constexpr const wchar_t* kDryRun = L"note.dry_run";
inline constexpr const wchar_t* kHelpIgnoredArguments = L"note.help_ignored_arguments";
inline constexpr const wchar_t* kCaptureChannel = L"note.capture_channel";
// 后续阶段
inline constexpr const wchar_t* kNoWindow = L"match.no_window";
inline constexpr const wchar_t* kAmbiguousWindow = L"match.ambiguous_window";
inline constexpr const wchar_t* kIndexOutOfRange = L"match.index_out_of_range";
inline constexpr const wchar_t* kMonitorOutOfRange = L"match.monitor_out_of_range";
inline constexpr const wchar_t* kAccessDenied = L"capture.access_denied";
inline constexpr const wchar_t* kUnsupported = L"capture.unsupported";
inline constexpr const wchar_t* kEncoderUnavailable = L"capture.encoder_unavailable";
inline constexpr const wchar_t* kCaptureFailed = L"capture.failed";
inline constexpr const wchar_t* kWriteFailed = L"io.write_failed";
inline constexpr const wchar_t* kFileExists = L"io.file_exists";
// 多个目标算出同一个输出名：整批一张都不截，也不静默改名
inline constexpr const wchar_t* kOutputCollision = L"io.output_collision";
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

// 选项目录：--help 的文本由它生成，是 CLI 契约的唯一来源。
// 说明文案不写字面量，只写资源 key（"opt.<名字>"），文案本体在 resources/strings-*.txt。
struct OptionInfo {
    std::wstring name;         // 规范长名，不含前导 -
    std::wstring shortName;    // 单字母别名，可为空
    bool takesValue = false;
    std::wstring group;        // match / pick / output / behavior
    std::wstring valueHint;    // 例如 "<full-path>"；开关为空。各语言共用，故保持 ASCII
    std::vector<std::wstring> allowedValues;  // 取值枚举，可为空
    std::wstring messageKey;   // 说明文案的资源 key
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

// 版本与阶段：只在 --version 文本里出现；JSON 不携带任何程序元信息。
// 版本数字本体在 src/Version.h（文件属性里的 VERSIONINFO 与 CMake 都从那里取）
inline constexpr const wchar_t* kVersion = ECAPTURE_TEXT(ECAPTURE_VERSION_STRING);
// 已实现的取图通道
inline constexpr const wchar_t* kStage = L"capture-channels";

const wchar_t* FormatName(ImageFormat format);  // "png" / "jpeg" / ...
const wchar_t* MultiKey(MultiMatch m);          // "ask" / "index" / "newest" / "oldest" / "all"

// 取图方式的机器名（--capture 的取值）。能力差别写在 --capture 的选项说明里。
const wchar_t* CaptureMethodName(CaptureMethod m);

}  // namespace ecapture
