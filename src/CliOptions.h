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
    kAsk,        // 默认：匹配到多个窗口时报错，要求用户消歧
    kIndex,      // --index N，取第 N 个（1 起）
    // 命中列表里的**当前 Z 序**首尾那一个。名字刻意说"叠放次序"而不是"新旧"：
    // EnumWindows 给的就是从最顶到底的 z 序，工具既没有窗口创建时间戳、也不去看进程创建时间
    // （旧名字 --newest/--oldest 是兼容别名，它们选的同样是 Z 序首尾）。
    kTopmost,    // --topmost-match（别名 --newest）
    kBottommost, // --bottommost-match（别名 --oldest）
    kAll,        // --all，每个窗口各存一张
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

    // --timeout-ms：自动处理阶段的**总**预算（匹配、后端重试、取帧等待、编码、提交共用这一份，
    // 每一步只拿"还剩多少"）。0 = 不设总预算，此时各隔离调用仍受内置上限约束（Worker.h）。
    uint64_t timeoutMs = 0;
    // --consent-timeout-ms：人工确认框最多等多久。0 = 一直等。到点按"拒绝"处理，
    // 绝不按"默认同意"处理；这段等待不占上面那份自动处理预算。
    uint64_t consentTimeoutMs = 0;

    bool dryRun = false;              // --dry-run：只解析并打印候选信息（本阶段的默认行为）
    bool yes = false;                 // --yes：免掉"只取所选窗口画面"那条路径的确认，桌面像素路径无效
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
// 多个目标却要写到标准输出：标准输出一次只能交付一张图，属参数用法错误（退出码 1）。
// 判据是"实际命中的目标数"，所以 --all / --monitor all 只命中一个时仍然放行。
inline constexpr const wchar_t* kStdoutMultipleTargets = L"cli.stdout_multiple_targets";
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
// 用了已被语义更准确的名字取代的旧选项（--newest / --oldest 见 --topmost-match）。
// 行为没变，只是名字说的不是一回事：按 Z 序首尾挑窗口，跟创建时间无关。
inline constexpr const wchar_t* kDeprecatedOption = L"note.deprecated_option";
inline constexpr const wchar_t* kDryRun = L"note.dry_run";
inline constexpr const wchar_t* kHelpIgnoredArguments = L"note.help_ignored_arguments";
inline constexpr const wchar_t* kCaptureChannel = L"note.capture_channel";
// 质量提示（不是错误，图片照常交付）：整帧逐像素比过之后确实只有一个颜色。
// 单色本身不说明采集失败 —— 目标窗口可以本来就是一块纯色；它也可能是没合成出画面。
// 所以这条只说事实、把两种可能都列在 hint 里，由调用方自己判断要不要再看一眼图。
inline constexpr const wchar_t* kFrameUniform = L"note.frame_uniform";
// 质量提示（不是错误，图片照常交付）：目标矩形没有被完整截到 —— 从整幅桌面帧里只能裁出与
// 那块输出重叠的部分（窗口跨屏、一部分在屏幕外）。图里是可见的那一块，尺寸比目标小。
// 只报事实与两边矩形，不断言"为什么没截全"，也不拿它升级授权。
inline constexpr const wchar_t* kCaptureClipped = L"note.capture_clipped";
// 后续阶段
inline constexpr const wchar_t* kNoWindow = L"match.no_window";
inline constexpr const wchar_t* kAmbiguousWindow = L"match.ambiguous_window";
inline constexpr const wchar_t* kIndexOutOfRange = L"match.index_out_of_range";
inline constexpr const wchar_t* kMonitorOutOfRange = L"match.monitor_out_of_range";
inline constexpr const wchar_t* kAccessDenied = L"capture.access_denied";
// 确认框弹不出来（服务会话、计划任务、锁屏：那里没有人能答"是"）。它与"人答了否"分开给码，
// 调用方才知道该换会话再来，而不是把这次失败当成"用户不让"再问一遍。退出码同样算 6。
inline constexpr const wchar_t* kConsentUnavailable = L"capture.consent_unavailable";
// 确认之后目标又挪了位置或变了大小：要取的矩形已经不在人批准的那一片里，于是不取。
// 这不是拒绝（那是 capture.access_denied），而是一次可以重来的截图失败：重新枚举、再问一次。
inline constexpr const wchar_t* kConsentStale = L"capture.consent_stale";
// 显示器这一侧变了，而不是窗口挪了位置：目标屏在确认之后被拔掉 / 禁用，或者分辨率、旋转
// 变了（桌面复制只能取当前拓扑里那块输出的画面）。与 capture.failed 分开给码：调用方的下一步
// 是重新枚举屏幕并重新确认，而不是换一条通道再来 —— 换通道截到的会是另一块屏。退出码仍是 7。
inline constexpr const wchar_t* kMonitorChanged = L"capture.monitor_changed";
inline constexpr const wchar_t* kUnsupported = L"capture.unsupported";
inline constexpr const wchar_t* kEncoderUnavailable = L"capture.encoder_unavailable";
inline constexpr const wchar_t* kCaptureFailed = L"capture.failed";
// 帧超时与窗口消失：旧版这两种都写成 capture.failed，只能靠 message 分辨。
// 退出码仍是 7（截图失败），但这两类调用方的下一步动作不同（前者可重试、后者要重新枚举），
// 所以各给一个稳定的新码；capture.failed 保留给其它取帧失败。
inline constexpr const wchar_t* kFrameTimeout = L"capture.frame_timeout";
// 身份复核（WindowIdentity.h）那三条：选定目标之后、真正取帧之前，那个 HWND 已经不是当初
// 挑中的那一扇窗口了。与 capture.window_gone 分开的理由是"发现得有多早"：window_gone 是
// 通道在取帧途中发现目标尺寸为 0 / 矩形量不出来，这三条是**一次像素都没读**之前就判掉的。
// 三条的下一步也不同：gone 要重新枚举窗口；changed 说明句柄已被另一个对象占用，
// 用户当初批准的是旧对象、许可不转移，同样要重新枚举并重新确认；unverifiable 说明有一道
// 判据问不出来（进程信息读不到、条件求值没能重新跑完），该查的是执行环境，不是再试一次。
// 退出码都是 7（截图失败）。
inline constexpr const wchar_t* kTargetGone = L"capture.target_gone";
inline constexpr const wchar_t* kTargetChanged = L"capture.target_changed";
inline constexpr const wchar_t* kTargetUnverifiable = L"capture.target_unverifiable";
// 本工具自己的辅助进程出了问题（起不来、管道断了、消息不合本协议、任务不合法），
// 而不是目标窗口拒绝对话。它和 capture.failed 分开给码：调用方看到这条就知道
// 该查的是这台机器的执行环境（权限、杀软、策略），而不是"目标是不是受保护"。退出码仍是 7。
inline constexpr const wchar_t* kWorkerFailed = L"capture.worker_failed";
inline constexpr const wchar_t* kWindowGone = L"capture.window_gone";
// 交回来的那帧像素自己说不通（宽高为 0 / 超过单边与整帧上限 / 行距装不下一行 / 缓冲区比
// 行距×高还短）。它与 capture.failed 分开给码：调用方的下一步是换通道或报告实现缺陷，
// 而不是"再试一次这个窗口"。退出码仍是 7。
inline constexpr const wchar_t* kFrameInvalid = L"capture.frame_invalid";
// 期限。三条各归一个阶段，因为调用方的下一步不同：
//   match.timeout    —— 条件求值（含 --title-regex 的正则）没在预算内跑完，重来或加大预算
//   capture.timeout  —— 取帧 / 编码没在预算内完成（含"辅助进程被中止"这种情况）
//   io.timeout       —— 写文件或写标准输出的预算已尽，还没开工
// 前两条退出码仍是 7，io.timeout 是 8（与它们各自的失败同类）。
inline constexpr const wchar_t* kMatchTimeout = L"match.timeout";
inline constexpr const wchar_t* kCaptureTimeout = L"capture.timeout";
inline constexpr const wchar_t* kIoTimeout = L"io.timeout";
// 确认框在 --consent-timeout-ms 之内没人应答。它是"按拒绝处理"，与"人答了否"同为 6，
// 但分开给码：调用方据此知道"再问一次可能就有人在"，而不是"这个人不同意"。
inline constexpr const wchar_t* kConsentTimeout = L"capture.consent_timeout";
inline constexpr const wchar_t* kWriteFailed = L"io.write_failed";
inline constexpr const wchar_t* kFileExists = L"io.file_exists";
// 多个目标算出同一个输出名：整批一张都不截，也不静默改名
inline constexpr const wchar_t* kOutputCollision = L"io.output_collision";
}  // namespace codes

// 诊断的 stage 取值（上面 Diagnostic 的 stage 字段）：出在哪一步。与 code 一样只增不改名。
namespace stages {
inline constexpr const wchar_t* kParse = L"parse";      // 命令行解析自身（异常兜底）
inline constexpr const wchar_t* kMatch = L"match";      // 条件求值（枚举窗口 + 正则）
inline constexpr const wchar_t* kPlan = L"plan";        // 整批输出名规划
inline constexpr const wchar_t* kConsent = L"consent";  // 整屏截图的人工确认框
inline constexpr const wchar_t* kCapture = L"capture";  // 取帧后端
inline constexpr const wchar_t* kEncode = L"encode";    // 编码成 png / jpg / ...
inline constexpr const wchar_t* kWrite = L"write";      // 原子写文件
inline constexpr const wchar_t* kStdout = L"stdout";    // 图片字节写标准输出
inline constexpr const wchar_t* kReport = L"report";    // 结果渲染与送出
}  // namespace stages

// 诊断项：code 恒在，其余为空时整个键省略（不输出 null 占位）。hint 用于"是不是想输入 --title"
// 这类纠正建议；下面几个定位字段只在对应那一步真拿到值时才出现，调用方据此分支而不必从
// message 里抠 —— message / hint 是人看的文字（随 --lang 变），这几个是机器看的坐标（不变）。
struct Diagnostic {
    std::wstring code;
    std::wstring message;
    std::wstring option;
    std::wstring value;
    std::wstring hint;
    // 出错的那个目标：窗口给 "0x001A0B4C"（与 images[].hwnd 同形），屏幕给 "DISPLAY1"
    std::wstring target;
    // 真实产出（或真实尝试过）的通道名，来自实际执行路径；auto 回退链失败时是链上试过的
    // 那些通道，不是请求值 "auto"
    std::wstring backend;
    // 流水线阶段：capture / encode / write / stdout / consent / report
    std::wstring stage;
    std::wstring hresult;  // "0x80070005" 形式
    uint32_t win32 = 0;    // GetLastError 的原值，0 = 不适用
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
const wchar_t* MultiKey(MultiMatch m);  // "ask" / "index" / "topmost" / "bottommost" / "all"
                                        // -v 的 input.policy 回显的是**策略**，不是用户写的那个名字：
                                        // 新旧别名指向同一条策略，追溯时看这里。

// 取图方式的机器名（--capture 的取值）。能力差别写在 --capture 的选项说明里。
const wchar_t* CaptureMethodName(CaptureMethod m);

}  // namespace ecapture
