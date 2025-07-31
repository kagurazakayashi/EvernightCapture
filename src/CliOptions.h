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
    // --hwnd：文档承诺过的三种写法都有效（纯数字按十进制、0x 前缀按十六进制、含 a-f 的裸写法
    //        按十六进制），但不接受正负号与溢出；下划线只在十六进制写法里夹在两位数字之间时合法
    std::vector<uint64_t> hwnds;
    std::vector<uint32_t> pids;             // --pid             只认十进制，1..0xFFFFFFFF
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
//   无窗口条件  -> 整块屏幕截图（kPrimary 用主屏，kAll 则每块屏各一张）
//   有窗口条件  -> 只算落在那块屏上的窗口
// 之所以要显式给 --monitor 才截屏，而不是"没条件就当全屏"：漏写条件的调用方
// 不该在无意间拍到整个桌面。
//
// 定位一块屏有三种来路，说的是三件事（数字只在本机本次枚举里有意义）：
//   kOrdinal  本次 EnumDisplayMonitors 顺序里的位置。它既不是「显示设置」里那个标识号，
//             也不保证插拔/改分辨率之后还指同一块屏 —— 所以要认屏请用下面两条。
//   kDevice   当下的 GDI 视图设备名（`\\.\DISPLAY1`）。它比编号稳：同一块屏在本次桌面连接里
//             一直是这个名字。但这个名字是系统按连接顺序**发**的，拔掉重插、换接口之后可能
//             发给另一块屏，所以它仍然只是"本次连接的身份"。
//   kPath     监视器 devnode 的设备接口路径（`\\?\DISPLAY#...`）。这是唯一一个由设备自己
//             决定、跨会话与重启都成立的标识（--screens 里那条 id:），也是本工具推荐的
//             "我要那一块屏"的写法。
// 取值语法与"下一个参数要不要被吃掉"写在同一处（CliOptions.cpp 的 LooksLikeMonitorValue）。
struct MonitorSelector {
    enum class Kind { kPrimary, kOrdinal, kAll, kDevice, kPath };

    bool given = false;
    Kind kind = Kind::kPrimary;
    uint32_t ordinal = 0;      // Kind::kOrdinal
    std::wstring id;           // Kind::kDevice：不含前缀的设备名（"DISPLAY1"）；Kind::kPath：devnode 路径原样
    std::wstring written;      // 用户实际敲下去的那一条取值（诊断的 value 与 -v 回显都用它）
};

// 只读的结构化窗口发现与检查（--list / --inspect）。两条都不取一个像素、不弹框、不写文件，
// 走的是与截图**同一套**条件求值语义，差别只在「命中多个」这件事怎么处理：
//   kList    把命中的窗口列成机器可读的列表（多匹配不是截图歧义，分页交出去）
//   kInspect 把一个明确选择器对应的那扇窗口逐项查清楚（多匹配照实报歧义，不替人选一个）
// 取值只增不改名。kNone = 这一次不是窗口查询（截图或环境查询）。
enum class WindowAction { kNone, kList, kInspect };

// ---------------------------------------------------------------------------
// 窗口内部裁剪（--roi / --client-area）
// ---------------------------------------------------------------------------
//
// 交付的整窗图像拿到之后，再按**这张图像自己的像素坐标**裁一次。坐标系、DPI 与边框那三条
// 规矩的判据本体在 src/CropGeometry.h，这里只落"用户要求的是哪一种裁剪"。
// 两条互斥（同时给出是 cli.conflicting_options，不是"挑一条执行"），而它与 --monitor 那种
// "整块屏幕"的目标说不通：屏幕上没有这么一个窗口可以让坐标相对它的左上角去算，
// 所以屏幕模式在解析期就报 capture.unsupported，而不是悄悄按桌面绝对坐标去截。
enum class CropMode {
    kNone,        // 没给裁剪：整窗图像原样交付
    kRoi,         // --roi x,y,w,h
    kClientArea,  // --client-area：只留客户区
};

struct CropRequest {
    CropMode mode = CropMode::kNone;
    // --roi 那四个数：只认十进制 [0-9]+，逗号分隔，不认空白、正负号与任何前缀。
    // x / y 可以是 0，width / height 至少 1，四条都不超过 cli_limits::kRoiMaxValue。
    // 这里是**已经规范化过的值**，用户原样写的那一条在 written（诊断的 value 用它）。
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::wstring written;
    // 这两条各自记"有没有被写出过"：mode 只留最后一个写法（与其它取值选项的顺序语义一致），
    // 而 --roi 与 --client-area 同时给出的那条冲突要看得见用户到底写了哪两个名字。
    bool roiGiven = false;
    bool clientAreaGiven = false;
};

// ---------------------------------------------------------------------------
// 光标包含与排除（--cursor）
// ---------------------------------------------------------------------------
//
// "画面里有没有鼠标指针"这件事，各条取图通道的**来源像素**本来就不一样：有的通道有一个
// 可设的开关，有的通道交回的那块像素里根本没有光标可去（详见 src/CursorControl.h 那张登记表）。
// 所以这一条不允许写成"我要求过就等于我拿到了"：结果里 requested 是用户要的那一种，
// effective 是这条路径**实际**交回的那一种，basis 说这个结论是从哪一条事实来的。
//
// 默认值是 kDefault，而且它的含义是"本工具对这件事一个字都不改"：不给 --cursor 时既不调
// 任何通道的光标开关，结果里也不出现那三个键 —— 与这条选项存在之前的行为逐字节相同。
// 显式写 --cursor default 才是"照通道默认交回，但把读到的状态报给我"。
enum class CursorMode {
    kDefault,  // 不改动任何通道的默认行为（--cursor 不给就是这一种）
    kInclude,  // 要求画面里有光标：只有那个开关设得进去的通道能做到
    kExclude,  // 要求画面里没有光标：设得进去的通道去设，来源本来就没光标的通道照实报
};

struct CursorRequest {
    CursorMode mode = CursorMode::kDefault;
    // 与 --roi 那条同一做法：mode 只留最后一个写法，而"到底写没写过这条选项"要单独记 ——
    // 没写过 = 结果里三个键都不出现（兼容），写过 = 报 requested/effective/basis。
    // 报错与回显都用**规范化后**的那个取值（与 `--capture` 一条规矩），所以这里不另存原样写法。
    bool given = false;
};


struct Options {
    MatchOptions match;
    MonitorSelector monitor;    // --monitor / -m

    std::wstring output;              // 位置参数或 --out；"-" 表示写标准输出
    bool outputImplicitStdout = false;  // 未给输出路径 => 按 "--out -" 处理，PNG 写标准输出
    ImageFormat format = ImageFormat::kPng;
    bool formatExplicit = false;      // 是否显式指定过 --format
    int jpegQuality = 100;            // --quality，只认十进制 1..100
    bool overwrite = true;            // --no-overwrite 置 false（=true 与裸开关同义，=false 取消）

    MultiMatch multi = MultiMatch::kAsk;
    int index = 1;                    // multi == kIndex 时有效；只认十进制 1..0xFFFF

    CaptureMethod capture = CaptureMethod::kWgc;   // --capture，默认 Windows.Graphics.Capture
    bool captureExplicit = false;                  // 是否显式指定过 --capture

    // --cursor：画面里要不要鼠标指针。判据在 src/CursorControl.h（按"这条路径的来源像素里
    // 本来有没有光标"登记，不按通道名字猜），实现与核实只在 wgc 那一条走真正的开关。
    CursorRequest cursor;

    // --roi / --client-area：取到整窗图像之后再按图像像素坐标裁一次。
    // 这件事**不改变**授权判断：确认框上列出的是整个目标，会读桌面像素的那几条照样一定问人，
    // --yes 也不因为"最后只留一小块"而开始生效（判据见 src/CaptureScope.cpp 那张登记表）。
    CropRequest crop;

    // --timeout-ms：自动处理阶段的**总**预算（匹配、后端重试、取帧等待、编码、提交共用这一份，
    // 每一步只拿"还剩多少"）。0 = 不设总预算，此时各隔离调用仍受内置上限约束（Worker.h）。
    uint64_t timeoutMs = 0;
    // --consent-timeout-ms：人工确认框最多等多久。0 = 一直等。到点按"拒绝"处理，
    // 绝不按"默认同意"处理；这段等待不占上面那份自动处理预算。
    uint64_t consentTimeoutMs = 0;

    bool dryRun = false;              // --dry-run：只解析并打印候选信息（本阶段的默认行为）
    bool yes = false;                 // --yes：免掉"只取所选窗口画面"那条路径的确认，桌面像素路径无效
    bool json = false;                // --json
    bool verbose = false;             // -v / --verbose：追加 input 段并保留 notes
    // -q / --quiet：省略 notes。它与 --verbose 同时给出时按 --verbose 处理，所以这里
    // 落下来的已经是"生效后的 quiet"（冲突时解析层会把它归 false 并留一条提示）；
    // errors 与 images[].source / path / scope 从来不受这条影响。
    bool quiet = false;

    bool showHelp = false;            // --help，或"一个条件都没给"
    std::wstring helpReason;          // 非空 => 因为缺少条件而显示帮助
    bool showVersion = false;

    // 只读的机器可读查询（判据与字段本体在 src/EnvReport.h）。这两条不截图、不弹框、不写文件，
    // 所以与"截图意图"那一整套选项互斥：一起给出是 cli.query_conflict，而不是挑一个来执行。
    // 查询命令也不参加"零条件 => 帮助"那一条 —— 它们本身就是明确的意图。
    bool capabilities = false;        // --capabilities
    bool diagnostics = false;         // --diagnostics
    // 只读的屏幕枚举（--screens，判据与渲染在 src/ScreenQuery.h）。它与环境查询同一类：
    // 不取像素、不弹框、不写文件，也不需要任何条件，所以与截图那一系列选项互斥。
    // 差别只在它交回的是"这个会话里有哪几块屏、每块屏的几种身份各稳在哪一层"，
    // 让调用方（含 AI）不必拿本次枚举顺序里的那个编号去猜哪一块是它要的那块。
    bool screens = false;             // --screens

    // 只读的结构化窗口发现与检查（判据与渲染在 src/WindowQuery.h）。这两条复用与截图
    // **同一套**条件求值，所以窗口条件、--monitor、--timeout-ms 都在允许之列；
    // 它们不参加「零条件 => 帮助」那一条（一次截图都不做正是它们的正常用法）。
    WindowAction windowAction = WindowAction::kNone;   // --list / --inspect
    // 这两条查询的取舍写在**自己的取值**里（--list=all / --inspect=path），不再各立一条开关：
    // 帮助文本的体积是真实约束（见 tests\cli.ps1 那条上限），少两条选项就少两行表。
    bool listIconic = false;          // --list=all：把最小化窗口也列进结果
    bool inspectPath = false;         // --inspect=path：把归属映像的完整路径也写进结果
    uint64_t offset = 0;              // --offset：跳过命中列表开头多少个（0 起）
    uint64_t limit = 0;               // --limit：这一次最多交多少个（0 = 用默认值）

    bool HasAnyCondition() const { return !match.IsEmpty(); }
    // 屏幕目标模式：截整块屏幕，而不是某个窗口的画面
    bool ScreenMode() const { return monitor.given && match.IsEmpty(); }
    // 这一次是查询而不是截图（任意一条查询命令给出即为真）
    bool QueryMode() const { return capabilities || diagnostics || screens ||
                                    windowAction != WindowAction::kNone; }
    // 只读的环境查询（--capabilities / --diagnostics / --screens）：只接受 --lang / -v / -q
    bool EnvQueryMode() const { return capabilities || diagnostics || screens; }
    // 只读的屏幕枚举（--screens）：与环境查询同一套允许清单，但交回的是另一份契约文档
    bool ScreenQueryMode() const { return screens; }
    // 只读的窗口查询（--list / --inspect）：额外接受窗口条件、--monitor、--timeout-ms 等
    bool WindowQueryMode() const { return windowAction != WindowAction::kNone; }
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
// 保留而不再发出：0.4.0 之前"没给 --out 而这次又没出图"会把真实原因整段换成这一条，
// 于是调用方读到的是"缺少输出路径"而不是"人拒绝了 / 没命中 / 写坏了"。现在省略 --out 与
// 显式 --out - 语义一致，这条 code 一律不出现；留着是为了它不被挪作别的含义。
// 同源的文案 cli.missing_output_hint 还在用，只在"图片必须挤过 stdout 而 stdout 写坏了"
// 那一条上作为 hint 出现（见 Capture.cpp）。
inline constexpr const wchar_t* kMissingOutput = L"cli.missing_output";
inline constexpr const wchar_t* kDuplicateOutput = L"cli.duplicate_output";
inline constexpr const wchar_t* kConflictingOptions = L"cli.conflicting_options";
// --roi 与 --client-area 同时给出。与上面那条分开给码，是因为那句文案说的是"选择策略"
//（--index / --topmost-match 那一组），拿它去解释一次裁剪请求会把人引向"那我删掉 --index"
// 这种根本无关的下一步。退出码同为 1，两个名字一次列全。
inline constexpr const wchar_t* kCropConflict = L"cli.crop_conflict";
inline constexpr const wchar_t* kUnknownCaptureMethod = L"cli.unknown_capture_method";
inline constexpr const wchar_t* kUnknownLanguage = L"cli.unknown_language";
inline constexpr const wchar_t* kMonitorConflict = L"cli.monitor_conflict";
// --monitor 的标识写法在解析期就说不通：前缀认得而取值为空（`--monitor=device:`），
// 或者前缀根本不是这两种标识之一（`--monitor=foo:1`）。两条都是退出码 1，
// 都不去猜"是不是想写主屏"——猜成主屏等于把一次写错的选屏变成一次没人批准的截图。
inline constexpr const wchar_t* kMonitorSelectorEmpty = L"cli.monitor_selector_empty";
inline constexpr const wchar_t* kMonitorSelectorKind = L"cli.monitor_selector_kind";
inline constexpr const wchar_t* kInternalError = L"cli.internal_error";
// 只读的查询命令（--capabilities / --diagnostics）与"截图意图"的那一整套选项互斥：
// 两条命令同时给出，或查询与任何窗口条件 / 输出路径 / 取图方式 / 授权 / 期限选项一起给出，
// 都是这一条（退出码 1，value 里列出用户实际写的那些名字）。
// 理由是这两个意图对流的约定不同：截图可能把图片字节压进 stdout 而让 JSON 整份改走 stderr，
// 查询则一定把这一份文档写在 stdout。与其替用户猜一个执行，不如把这条用法说清楚。
inline constexpr const wchar_t* kQueryConflict = L"cli.query_conflict";
// 只读的窗口查询（--list / --inspect）有自己的一套允许项：它需要窗口条件与 --monitor 才有意义，
// 而截图那一级的选项（输出路径、格式、覆盖、--capture、--dry-run、确认框期限）对它一条都不成立，
// 选择策略那几条还要按实际入口分开判（--inspect 要的正是「多匹配里定哪一扇」，--list 不需要）。
// --yes 是唯一一条「不成立但不算错」的：它属于截图授权那一级，写了结果一模一样。
// 与上面那条分开给码是因为文案与允许清单本来就不同：拿环境查询那句
// 「只接受 --lang / -v / -q」去解释窗口查询会把人引向错误的下一步。退出码同为 1，
// 一次列全所有冲突项，一个像素都不取。
inline constexpr const wchar_t* kWindowQueryConflict = L"cli.window_query_conflict";
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
// 环境这一侧的提示（不是错误，图照常由别的那几条截出来）：
//   note.channel_unavailable  auto 回退链里那一条被本机 Windows 版本挡掉，已经不在链里了
//   note.os_unverifiable      本机版本没能问出来，所以这一次没有按版本筛通道（不等于支持）
inline constexpr const wchar_t* kNoteChannelUnavailable = L"note.channel_unavailable";
inline constexpr const wchar_t* kNoteOsUnverifiable = L"note.os_unverifiable";
// auto 回退链里那一条**做不到这次要求的光标状态**，已经从链里摘掉了（原因 token 在 message 末尾，
// ASCII、不随 --lang 变：`os_below_min_build:19041` = 这台机器的版本给不了那个开关，
// `window_self_drawn` / `dwm_redirection_surface` / `screen_dc_has_no_pointer` = 这条路径的来源
// 里根本没有光标，`pointer_shape_is_separate_metadata` = 桌面复制那条的指针是独立元数据而本工具
// 从不合成它，`not_registered` = 新增通道忘了在光标登记表里加一行）。
// 与 note.channel_unavailable 分开：那一条说的是"本机版本用不了这条通道"，这一条说的是
// "这条通道能用，但它兑现不了这次的光标要求"，而剩下的那几条仍会照顺序试。
inline constexpr const wchar_t* kNoteCursorChannelSkipped = L"note.cursor_channel_skipped";
// 窗口查询（--list / --inspect）交回的是一份**当时的快照**：句柄会复用、标题会变、进程会退出，
// 所以列表里的 hwnd / pid / 类名不是一种可以长期持有的凭证。真去截图时仍要按
// 《窗口选择与身份一致性》那一节复核，这一条提示随每一次成功的窗口查询发出（--quiet 可抑制，
// 但文档 caveats 里同源的 token 恒在，抑制不掉的才是判据）。
inline constexpr const wchar_t* kWindowQueryStale = L"note.window_query_stale";
// 屏幕枚举（--screens）交回的同样是一份**当时的快照**：编号是本次枚举顺序里的位置，
// 设备名是本次桌面连接发的，屏幕本身可能在下一次调用之前被拔掉或改分辨率。
// 所以这份列表不是可以长期持有的凭证，真去截图时那一路仍要在取帧之前重新核对一次
// （见 src/ScreenIdentity.h 与 ScreenMatch.h 的 CompareScreen）。
inline constexpr const wchar_t* kScreenQueryStale = L"note.screen_query_stale";
// 质量提示（不是错误，图片照常交付）：整帧逐像素比过之后确实只有一个颜色。
// 单色本身不说明采集失败 —— 目标窗口可以本来就是一块纯色；它也可能是没合成出画面。
// 所以这条只说事实、把两种可能都列在 hint 里，由调用方自己判断要不要再看一眼图。
inline constexpr const wchar_t* kFrameUniform = L"note.frame_uniform";
// 质量提示（不是错误，图片照常交付）：目标矩形没有被完整截到 —— 从整幅桌面帧里只能裁出与
// 那块输出重叠的部分（窗口跨屏、一部分在屏幕外）。图里是可见的那一块，尺寸比目标小。
// 只报事实与两边矩形，不断言"为什么没截全"，也不拿它升级授权。
inline constexpr const wchar_t* kCaptureClipped = L"note.capture_clipped";
// 提示（不是错误，图片照常交付）：这一次按 --roi 裁了，但**没能核实**这块交付图像对应屏幕上
// 的哪一块，所以结果里少了 cropScreenRect 那一行（图像坐标里的 cropRect 照写，裁剪本身没有
// 任何不确定）。什么时候会这样：窗口内容那条通道交回的尺寸与此刻量到的可见窗口矩形对不上
// （窗口在这中间改了大小、或这条通道把 DWM 那圈透明边框一起交了）。下一步是重取一次并核对
// images[].rect / fullWidth / fullHeight，而不是把 cropRect 当成屏幕坐标去用。
inline constexpr const wchar_t* kCropMappingUnavailable = L"note.crop_mapping_unavailable";
// 后续阶段
inline constexpr const wchar_t* kNoWindow = L"match.no_window";
inline constexpr const wchar_t* kAmbiguousWindow = L"match.ambiguous_window";
inline constexpr const wchar_t* kIndexOutOfRange = L"match.index_out_of_range";
inline constexpr const wchar_t* kMonitorOutOfRange = L"match.monitor_out_of_range";
// --roi 的矩形放不进选定那一刻那块窗口矩形之内（越界，或者那块矩形本身量不出来 = 零尺寸）。
// 与 match.index_out_of_range 同一条规矩：不放宽条件去凑，也不"那就裁到边上为止"，
// 而且在弹确认框与取帧**之前**给出 —— 一条注定裁不出来的请求不该先打扰人一次。
// 下一步是照 --dry-run / -v 回显的窗口尺寸重新给一条落在窗口内的 --roi。退出码 1。
inline constexpr const wchar_t* kRoiOutOfRange = L"match.roi_out_of_range";
// 按标识选屏（--monitor=device:… / --monitor=id:…）的三种下场，各给一条码：
// 它们的下一步动作不一样，而"没对上"绝不允许被折叠成"那就用主屏吧"——
// 用户批准的是当初列给他看的那一块屏，换一块屏截到的是谁都没批准过的画面。
//   match.monitor_unknown_id       这个标识现在不在桌面上（拔掉了、禁用了、或者本来就是
//                                  上一次枚举留下的旧值）。下一步：重新跑一次 --screens。退出码 4。
//   match.monitor_ambiguous_id     这个标识同时命中多块屏（同一台机器上接了两台型号与
//                                  连接方式完全相同的监视器时，devnode 路径以外的标识可能撞车）。
//                                  下一步：改用 --screens 里那条更具体的一条，或直接用编号。
//                                  本工具绝不替你挑一块。退出码 5。
//   match.monitor_id_unverifiable  这一问没给出答案（屏幕拓扑没能读出来），因此**无法判断**
//                                  哪个标识对应哪块屏。与"没匹配上"是两件事：一次是没找到，
//                                  一次是没问出来。退出码 7，与 match.timeout 同源。
inline constexpr const wchar_t* kMonitorUnknownId = L"match.monitor_unknown_id";
inline constexpr const wchar_t* kMonitorAmbiguousId = L"match.monitor_ambiguous_id";
inline constexpr const wchar_t* kMonitorIdUnverifiable = L"match.monitor_id_unverifiable";
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
// 与 capture.monitor_changed 分别是"发现了变化"和"没发现得起变化"：重新核对屏幕身份的那一问
// 自己没能给出答案（这次枚举里一个屏幕标识都没读出来），而当初选定那块屏靠的就是标识。
// 这时**不**按设备名退回去截 —— 名字可能已经发给了另一块屏，照名字截就是截一块没人批准过的屏。
// 下一步是查这台机器的显示拓扑（重新 --screens 一次），不是换通道碰运气。退出码仍是 7。
inline constexpr const wchar_t* kMonitorUnverifiable = L"capture.monitor_unverifiable";
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
// 交付的整窗图像拿不到了当初要求的那一块裁剪：窗口在选定之后改了尺寸所以 --roi 落在了图像
// 之外、目标被屏幕边缘裁短、客户区那一问没能给出答案、或者交付的那块图像核实不出它对应屏幕上
// 的哪一块（于是客户区在图里落在哪儿无从确定）。与 match.roi_out_of_range 分别是
// "取帧之前就看得出放不下"与"取到帧之后才发现放不下"，下一步也不同：后者要先重新量一次窗口
// 现在的尺寸。两种都**一个像素都不落地**，也不退回"那就整窗交出"。退出码 7。
inline constexpr const wchar_t* kRoiInvalid = L"capture.roi_invalid";
// 与 capture.roi_invalid 分开给码，因为"放不下"与"没能问出来"是这个仓库里始终分开的两件事
// （同 capture.target_unverifiable 与 capture.target_changed、capture.monitor_unverifiable 与
// capture.monitor_changed）：前者是请求的矩形本身超出这块图像，下一步是照当下的尺寸改请求；
// 后者是定位这块矩形所需要的那一问没有答案（客户区量不出来、或这块交付图像核实不出它对应
// 屏幕上哪一块），下一步是换一条窗口内容通道或整窗重取一次，而**不是**把请求往里挪一挪。
// 两条都在一个像素都没落地之前给出，都不许被当成"那就整窗交出"的理由。退出码 7。
inline constexpr const wchar_t* kRoiUnmeasurable = L"capture.roi_unmeasurable";
// 光标（--cursor）。三条码各自的下一步不同，而共同点是**绝不"那就先交出再说"**：
//   capture.cursor_unsupported   要的那种光标状态由所要求的这条通道结构上做不到（例如
//                                --cursor include 配 printwindow / bitblt / duplication：
//                                那几条的来源像素里根本没有光标可画）。解析期给出，退出码 1，
//                                **不换后端**（换一条高风险的桌面通道既没把光标加回来，
//                                还多拍了没人批准过的画面）。
//   env.cursor_unsupported       结构上能做到，但这台机器的 Windows 版本给不了那个开关
//                                （wgc 的 IsCursorCaptureEnabled 要内部版本 19041 起）。
//                                取帧、弹框之前就给出，退出码 7，与 env.channel_unsupported
//                                同一族：说的是这一台机器，不是这个目标。
//   capture.cursor_unverifiable  开关问不到、设不下去，或者设完读回来跟所要求的不是同一件事。
//                                问不出来不等于"照我要的办了"，这一张一个像素都不落地，退出码 7。
//                                与 capture.roi_unmeasurable 同源（放不下 vs 问不出来）。
inline constexpr const wchar_t* kCursorUnsupported = L"capture.cursor_unsupported";
inline constexpr const wchar_t* kEnvCursorUnsupported = L"env.cursor_unsupported";
inline constexpr const wchar_t* kCursorUnverifiable = L"capture.cursor_unverifiable";
// 运行环境（这一台机器上的 Windows 版本）提供不了所要求的东西，与"这个目标截不到"是两回事。
// 判据与三条下限各写在哪儿见 src/SystemCompat.h；两条都在枚举目标、弹确认框、读像素**之前**
// 给出，一个像素都不读，退出码 7。分开给码的理由就是调用方的下一步不同：
//   env.os_too_old            这台机器整工具都不行（唯一那套编码器不在）—— 换 --capture 没用
//   env.channel_unsupported   只有这一条通道不行 —— 换通道或 auto 回退是有意义的
inline constexpr const wchar_t* kEnvOsTooOld = L"env.os_too_old";
inline constexpr const wchar_t* kEnvChannelUnsupported = L"env.channel_unsupported";
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

// 命令行取值的上限。数字只在这里写一次：解析判据与 --capabilities 报告的 limits 段读的都是
// 同一份，免得"帮助里说 24 小时"而查询里报另一个数。
namespace cli_limits {
// --timeout-ms / --consent-timeout-ms 的上限：24 小时。再大的数字基本上是把期限当成装饰，
// 而那正是这条参数要解决的问题，所以宁可不接受。
inline constexpr uint64_t kMaxTimeoutMs = 86400000ull;
// --index 与 --monitor 的编号上限。一个条件命中六万多个窗口、或者机器上有六万多块屏，
// 都是不可能的；超过这个数的编号一定是敲错了，照实在解析期拒掉，不留到匹配阶段去凑越界。
inline constexpr uint64_t kMaxOrdinal = 0xFFFFull;
// 进程 ID 的上限就是 Windows 给 PID 留的那 32 位（0 不是合法 PID）。
inline constexpr uint64_t kMaxPid = 0xFFFFFFFFull;
// --quality 只对 jpeg 生效，取值区间写在帮助里。
inline constexpr int kJpegQualityMin = 1;
inline constexpr int kJpegQualityMax = 100;
// ---- 只读窗口查询（--list / --inspect）的条数上限 ----
// 上限取辅助进程回传窗口条数的那一道线（WorkerProtocol.h 的 kMaxWindowEntries），也就是
// 「一次求值本来就能拿到多少条」这个事实，不是另挑的数：超过它的 --limit / --offset
// 不可能对一次真实命中有意义，照实在解析期拒掉。
inline constexpr uint64_t kMaxWindowListItems = 8192ull;
// 默认条数是另一件事：调用方（含 AI）第一次列窗口时不该一口气拿到整机所有标题，所以要分页。
// 判「到底命中多少个」看结果里的 pagination.matched，不是看本批交回几条。
inline constexpr uint64_t kDefaultWindowListLimit = 50ull;
// ---- --roi 的四条数上限 ----
// 与帧的单边上限 kFrameMaxSide（src/CaptureCommon.h）**同一个数**：一条比交付图像还能宽的
// 裁剪矩形本来就不成立，而 kFrameMaxSide 那条"16384 是 D3D11 纹理边长上限"的理由在这里同样
// 成立。两份数字不许各写一套 —— 相等那条判据写在 tests\crop_state.cpp 里现场核对
//（CliOptions.h 不去 include 取帧公共件，那会把 Consent / ScreenMatch 全拖进解析层）。
inline constexpr uint64_t kRoiMaxValue = 16384ull;
}  // namespace cli_limits

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

// 裁剪方式的机器名（-v 的 input.crop.mode 与 images[].cropMode 都写它）：
// "none" / "roi" / "client-area"。与 codes 一样只增不改名，调用方按它分支。
const wchar_t* CropModeName(CropMode m);

// 光标要求的机器名（-v 的 input.cursor 与 images[].cursorRequested 都写它）：
// "default" / "include" / "exclude"。只增不改名；实际交回的那一种在 cursorEffective，
// 这个结论的根据在 cursorBasis（两套取值在 src/CursorControl.h，各一个出处）。
const wchar_t* CursorModeName(CursorMode m);

}  // namespace ecapture
