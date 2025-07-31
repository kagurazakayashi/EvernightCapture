#pragma once
// EvernightCapture - 能力与诊断查询（--capabilities / --diagnostics）
//
// 为什么要有这一层：调用方（尤其是 AI）在此之前只能"先试一次截图，再看那条错误码"才知道
// 这台机器行不行。而那一次尝试可能弹确认框、可能动到目标窗口、可能花掉一整份期限。
// 这一层把"这台机器现在能走哪几条路线"变成一次**只读**的问答，规则三条：
//
//   1. **不取帧、不弹框、不碰任何目标窗口。** 一条像素都不读，所以不会出现"为了看能不能截
//      而先截了一张"。编码器状态同样不去实测（见下面 EnvProbe::encoders 的注释）：
//      能问出来的只有版本下限、会话与屏幕拓扑这几件不依赖取帧的事实。
//   2. **问不出来就说问不出来。** 每一条判据都是三值的（yes / no / unknown），
//      unknown 既不写成 yes 也不写成 no，也不整个键悄悄省掉。这与身份复核那一条
//      "问不出来 ≠ 相同"、以及 note.os_unverifiable 那条规矩同源。
//   3. **"编译支持"、"这台机器现在可用"、"本项目在这台机器上实测过"是三件事，分开写。**
//      compiled 说的是这个 exe 里有没有实现那条路线；status 说的是本机的版本门槛与
//      屏幕拓扑有没有挡掉它；verifiedOnThisMachine 说的是**我们这个工具**有没有在
//      这一模一样的系统上跑过判据（见 verified_env，只有开发机那一台）。
//      三条都不保证"某个具体窗口这一次一定截得到"—— 驱动不喂帧、内容受保护、HDR 模式下
//      帧格式不对，这些不在版本号与拓扑里，本层一条都不预测（与 src/SystemCompat.h 同源）。
//
// 两条命令共用同一个 BuildEnvReport 与同一个渲染器，所以不存在"两份会互相打脸的环境信息"：
//   * --capabilities  面向"该选哪条路线"，backends / formats / consent / limits 全在。
//   * --diagnostics   面向"出问题后要提交什么"，同一份字段 + 可核对的构建标识（PE 头），
//                     默认不含任何路径、用户名或环境变量。
//
// 隐私（这一层是唯一的出口，所以规矩写在这里）：不联网、不上传、不采集画面、
// 不枚举用户文件、不读环境变量、不输出用户名 / 计算机名 / 任何绝对路径。
// 会话那一侧只报**编号**（终端会话 ID 是一个整数，不是身份）。
// 输出文件名固定为 ECAPTURE.EXE 这个名字本身，不含目录。
//
// 判据本体（BuildEnvReport）是纯函数：收一份 EnvProbe（一问一问的答案），不碰 Win32，
// 所以"没有图形环境""版本低于某条下限""某个编码器没登记""版本问不出来"这些现场
// 都由 tests\capabilities_state.cpp 注入逐条判 —— 这台开发机降级不了，也没有第二块显卡，
// 造不出现场的项在 tests\capabilities.ps1 里照实记未验证。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "CliOptions.h"
#include "SystemCompat.h"

namespace ecapture {

// 一条事实的三值结果。JSON 里固定写成 "yes" / "no" / "unknown" 三个 ASCII 小写词。
enum class Tri { kYes, kNo, kUnknown };

// 三值结果的机器名（进 JSON）。
const wchar_t* TriName(Tri value);

// 一条路线 / 一个格式在这次查询里的状态。取值只增不改名：
//   kAvailable    这个构建实现了它，且这次问出来的环境判据（版本下限 + 屏幕拓扑）没有挡掉它。
//                 **不**含"目标一定截得到"这层意思，见 caveats。
//   kUnavailable  这次问出来的判据确定挡掉了它（有原因码）。换目标、重试都没用。
//   kUnverified   该问的判据没能给出答案（版本问不出来、拓扑枚举失败……），不作任何断言。
enum class CapStatus { kAvailable, kUnavailable, kUnverified };

const wchar_t* CapStatusName(CapStatus status);

// 状态的原因（稳定 ASCII token，与诊断码那一组一样不随 --lang 变）。
namespace cap_reason {
inline constexpr const wchar_t* kNone = L"none";                                // 没被挡掉
inline constexpr const wchar_t* kNotCompiled = L"not_compiled";                 // 这个构建里没有这条路线
inline constexpr const wchar_t* kOsBelowMin = L"os_below_min_build";            // 本机内部版本低于这条的下限
inline constexpr const wchar_t* kOsUnverifiable = L"os_version_unavailable";    // 本机版本没能问出来
inline constexpr const wchar_t* kNoDisplayTopology = L"no_display_topology";    // 这个会话里没有可用的屏幕输出
inline constexpr const wchar_t* kDisplayTopologyUnknown = L"display_topology_unavailable";
inline constexpr const wchar_t* kEncoderNotRegistered = L"encoder_not_registered";
}  // namespace cap_reason

// caveats 的固定 token（写在 README 与 cli-contract.md 的契约表里，只增不改名）。
// 这一组说的都是"这份报告**没有**断言什么"，免得调用方把一条 available 读成保证。
namespace caveat {
inline constexpr const wchar_t* kNotAGuarantee = L"available_is_not_a_guarantee";
inline constexpr const wchar_t* kNoCapture = L"no_capture_performed";
inline constexpr const wchar_t* kNoDialog = L"no_consent_dialog_shown";
inline constexpr const wchar_t* kEncoderUnprobed = L"encoder_state_not_probed";
inline constexpr const wchar_t* kDeviceNotPredicted = L"device_capability_not_predicted";
inline constexpr const wchar_t* kSessionInferred = L"consent_dialog_state_inferred_not_probed";
inline constexpr const wchar_t* kSubsystemIsLinkerDefault = L"subsystem_version_is_linker_default";
inline constexpr const wchar_t* kRemoteSession = L"remote_session_observed";
inline constexpr const wchar_t* kOsUnverifiable = L"os_version_unavailable";
inline constexpr const wchar_t* kTopologyUnverifiable = L"display_topology_unavailable";
inline constexpr const wchar_t* kTopologyAbsent = L"display_topology_absent";
inline constexpr const wchar_t* kBuildIdUnavailable = L"build_identity_unavailable";
inline constexpr const wchar_t* kNotTestedHere = L"this_environment_not_tested";
inline constexpr const wchar_t* kTestedEnvUnknown = L"tested_environment_unknown";
// 这一会话里弹不出确认框，而桌面像素那一级非要人点头不可：那几条路线这一次走不通，
// 而且不是"换个目标再试"的事，要换一个有人在的会话。
inline constexpr const wchar_t* kDesktopNeedsDialog = L"desktop_paths_need_answerable_dialog";
// 本进程没被提升过，而更高完整性级别的目标窗口本来就不一定会响应跨进程的绘制请求。
// 只报这个事实，不断言"截不到"（那一条要看具体目标）。
inline constexpr const wchar_t* kNotElevated = L"unelevated_process_may_miss_elevated_targets";
// 光标（--cursor）那一段的边界：结果里 cursorEffective 断言到的是"这条会话被设置成画 / 不画
// 光标"与"这条路径的来源像素里本来就没有光标"这两层，**不是**"这一张图里看得见或看不见指针"。
// 本 SDK 的会话接口没有 IsCursorVisible 那个只读属性，像素级的事这一层一条都不声称，
// 也不去用像素反推（那需要真的截一次，而这一份查询的规矩是一个像素都不取）。
inline constexpr const wchar_t* kCursorSettingNotPixels =
    L"cursor_effective_is_a_setting_not_a_pixel_check";
// 桌面复制那两条的指针形状是**独立元数据**，本工具从不取它、也从不动手把它画进帧里，
// 反过来也不抹。所以"排除"这件事在那些路径上说的是来源本来就没有，而不是事后修过图。
inline constexpr const wchar_t* kPointerNeverComposited =
    L"pointer_shape_never_composited_nor_erased";
}  // namespace caveat

// 本项目**唯一实测过**这套工具的环境。README《系统支持》与 AGENTS.md 里"已实测"记的就是它，
// 数字只在这里写一次，查询结果里那个"这台机器实测过没有"由它算。
// 它不参与任何"能不能截"的判定，只用来防止把开发机的结果当成对整个系统的承诺。
namespace verified_env {
inline constexpr uint32_t kOsBuild = 19045;             // Windows 10 22H2
inline constexpr const wchar_t* kArch = L"x64";
}  // namespace verified_env

// 两条查询命令（同一份判据与同一个渲染器，只有段落取舍不同）。
enum class EnvQueryKind {
    kCapabilities,
    kDiagnostics,
};

// 这次查询的机器可读契约名与版本号。版本号只属于这两份文档，
// **不**加到普通截图 JSON 上（那份按「输出契约」保持精简）。
const wchar_t* EnvContractName(EnvQueryKind kind);
constexpr uint32_t kEnvContractVersion = 1;

// ---------------------------------------------------------------------------
// 探针：一问一答的答案。真机上由 ProbeEnvFacts 填；测试里手工构造，所以每一条
// "这台机器给不出答案"的形状都能离线判。这里没有一条来自取帧或弹框。
// ---------------------------------------------------------------------------
struct EnvProbe {
    // 本机 Windows 内部版本（ProbeOsVersion；known=false = 这一问没成功）
    OsVersion os;

    // CPU 架构（GetNativeSystemInfo）。arch 为空 = 认不出来。
    std::wstring arch;

    // 会话：本进程的会话号，与"接在控制台上的那个会话"是不是同一个。
    // 不是同一个（服务会话、计划任务、无人登录的会话）= 确认框弹出去也没人看得见。
    Tri consoleAttached = Tri::kUnknown;
    bool processSessionKnown = false;
    uint32_t processSessionId = 0;
    bool consoleSessionKnown = false;
    uint32_t consoleSessionId = 0;

    // 这个会话是不是远程桌面会话（GetSystemMetrics(SM_REMOTESESSION)）。
    Tri remoteSession = Tri::kUnknown;

    // 本进程有没有被提升过（GetTokenInformation(TokenElevation)）。完整性级别决定了
    // 跨进程的绘制请求能不能送进别人的窗口，所以这条与"某些目标没把握"有关。
    // 只回答是/否，不读用户名、不读 SID 归属。
    Tri elevated = Tri::kUnknown;

    // 桌面上有没有可用的屏幕输出，以及有几块（EnumDisplayMonitors）。
    // kUnknown = 那次枚举本身没跑成；kNo = 枚举成功但一块屏都没有（无图形会话）。
    Tri displayTopology = Tri::kUnknown;
    bool monitorCountKnown = false;
    uint32_t monitorCount = 0;

    // 可核对的构建标识：读本进程自己那份已经映射进内存的 PE 头，不开文件、不枚举目录。
    // 时间戳 + 机器类型 + 映像大小足以和发布产物逐字节核对（dumpbin /headers 读到同一份数）。
    bool buildIdKnown = false;
    std::wstring buildId;            // 例："0.4.0-x64-665f1a2b"，认不出来时为空
    bool linkTimestampKnown = false;
    uint64_t linkTimestamp = 0;
    bool peMachineKnown = false;
    uint32_t peMachine = 0;          // IMAGE_FILE_MACHINE_* 原值
    bool imageSizeKnown = false;
    uint64_t imageSize = 0;
    bool subsystemKnown = false;
    uint32_t subsystem = 0;          // IMAGE_SUBSYSTEM_*（本项目是 3 = 控制台程序）
    uint32_t subsystemMajor = 0;
    uint32_t subsystemMinor = 0;
    bool subsystemVersionKnown = false;
    std::wstring linkerVersion;      // 链接器版本，例："14.51"，认不出来时为空

    // 每个格式各自的编码器登记状态。**这次查询不去实测编码器**（没列出的按 unknown 处理）：
    // 实测要么得起 WinRT 异步调用并等它（那就不再是"只读、必然有界"的查询），要么去猜
    // WIC 那份注册表而它与 BitmapEncoder 并不等价。所以真机上这一律是 unknown，
    // 而"某个编码器确实不可用"的现场由离线层注入判（那条路上 status 会真的变成 unavailable）。
    std::vector<std::pair<ImageFormat, Tri>> encoders;
    Tri EncoderRegistered(ImageFormat format) const;
};

// 问一遍本机。全部是 kernel32 / user32 的廉价问答：不弹框、不取帧、不读文件、不读环境变量。
EnvProbe ProbeEnvFacts();

// ---------------------------------------------------------------------------
// 报告本体：由 BuildEnvReport(probe) 算出来，渲染器只是把它写成 JSON。
// 判据与呈现分开，所以字段稳定性可以在不解析 JSON 的情况下逐条断言。
// ---------------------------------------------------------------------------
// 一条路线在某类目标上实际走的那条内部路径，以及它的像素来源与确认要求。
struct ConsentPathReport {
    std::wstring target;             // "window" / "screen"
    std::wstring path;               // images[].path 里那个机器名（wgc / dwm.screen / ...）
    std::wstring scope;              // images[].scope 里那个（window / desktop）
    bool consentWithoutYes = true;   // 不给 --yes 时要不要问人
    bool consentWithYes = true;      // 给了 --yes 还要不要问人（桌面路径恒为 true）
};

struct BackendReport {
    CaptureMethod method = CaptureMethod::kWgc;
    std::wstring name;               // --capture 的那个取值
    bool compiled = true;            // 这个构建里有没有实现这条路线
    CapStatus status = CapStatus::kUnverified;
    std::wstring reason = cap_reason::kNone;
    uint32_t minBuild = 0;           // 这条要求的内部版本，0 = 没有版本门槛
    Tri verifiedOnThisMachine = Tri::kUnknown;
    std::vector<ConsentPathReport> paths;
};

// 一条取帧路径对"光标在不在画面里"能做到什么（判据与 capability / reason 两套 token 的唯一
// 出处在 src/CursorControl.h，这一份只是把它写成机器可读的一行，不再判第二次）。
// 那两个状态是三值的（yes / no / unknown），因为"这台机器上做不到"与"这一问没答案"是两件事：
// 开关那条的版本门槛问不出来时只能写 unknown，绝不折成 yes 或 no（与三值判据那一条同源）。
struct CursorPathReport {
    std::wstring path;           // images[].path 里那个机器名
    std::wstring capability;     // settable / excludes_cursor / unregistered
    std::wstring reason;         // cursor_reason:: 那一个 token（这条路径的根据）
    std::wstring includeState;   // 这条路径兑现得了 --cursor include 吗：yes / no / unknown
    std::wstring excludeState;   // 这条路径**敢不敢声称**兑现了 --cursor exclude：同上三值
};

struct CursorControlReport {
    std::wstring api;                    // 那条会话接口的名字（要能对着微软文档查到同一条）
    bool compiled = true;                // 这个构建里有没有用它
    CapStatus status = CapStatus::kUnverified;
    std::wstring reason = cap_reason::kNone;
    uint32_t minBuild = 0;               // 这个开关自己的版本门槛（os_floor::kWgcCursor）
    Tri verifiedOnThisMachine = Tri::kUnknown;
};

// --cursor 这一段：默认值、三种取值、那条唯一的开关在本机的状态、以及每条路径各能做到什么。
// 三件事照旧分开写：compiled（这个构建里有没有）/ status（本机现在让不让走）/
// verifiedOnThisMachine（本项目有没有在这种系统上实测过）。
struct EnvCursorReport {
    std::wstring option = L"--cursor";
    std::wstring defaultValue = L"default";   // 明确记录默认值：不给 = default = 一个字都不改
    std::vector<std::wstring> values;         // default / include / exclude
    CursorControlReport control;              // JSON 里那一段叫 "switch"（switch 是 C++ 关键字）
    std::vector<CursorPathReport> paths;
    // 本工具对指针形状与像素动手的那两问，答案恒是"没有"：既不把独立元数据合成进帧，
    // 也不事后抹掉已经画进去的光标。这两条写出来，是让调用方不必读源码就知道 effective
    // 那个结论**不是**靠图像修补得来的。
    std::wstring pointerCompositing = L"never";
    std::wstring pixelRetouching = L"never";
};

struct FormatReport {
    std::wstring name;               // png / jpeg / ...
    bool compiled = true;
    CapStatus status = CapStatus::kUnverified;
    std::wstring reason = cap_reason::kNone;
    uint32_t minBuild = 0;
    Tri registered = Tri::kUnknown;  // 编码器登记状态（这次没去实测 = unknown）
};

struct EnvReport {
    std::wstring contract;
    uint32_t contractVersion = kEnvContractVersion;
    EnvQueryKind kind = EnvQueryKind::kCapabilities;

    // 程序：版本号只在 src/Version.h 写一次，这里取的就是 --version 那个数字。
    std::wstring programName;
    std::wstring binaryName;         // 只有文件名 ECAPTURE.EXE，不含目录
    std::wstring version;
    std::wstring arch;               // 认不出来时 "unknown"
    std::wstring buildId;            // 认不出来时 "unknown"

    // 本机系统
    OsVersion os;
    uint32_t declaredMinBuild = 0;   // 本工具对外声明的下限（kSupportedMinBuild）
    uint32_t encoderMinBuild = 0;    // 任何一张图都要求的那一道（kEncoder）
    uint32_t verifiedOsBuild = verified_env::kOsBuild;
    std::wstring verifiedArch = verified_env::kArch;
    Tri matchesVerifiedEnv = Tri::kUnknown;

    // 会话与桌面
    Tri consoleAttached = Tri::kUnknown;
    Tri remoteSession = Tri::kUnknown;
    Tri elevated = Tri::kUnknown;
    Tri displayTopology = Tri::kUnknown;
    Tri consentDialogExpected = Tri::kUnknown;   // 推出来的，没有真去弹框（见 caveat.kSessionInferred）
    bool monitorCountKnown = false;
    uint32_t monitorCount = 0;
    bool processSessionKnown = false;
    uint32_t processSessionId = 0;
    bool consoleSessionKnown = false;
    uint32_t consoleSessionId = 0;

    std::vector<BackendReport> backends;
    // 光标这件事（--cursor）：默认值 + 那条开关在本机的状态 + 每条路径各能做到什么。
    EnvCursorReport cursor;
    std::vector<FormatReport> formats;
    // 本机现在能试的 auto 链（两种目标各一份，被版本挡掉的那几条不在里面）。
    // 与 GateChannels 同源：查询里给的那一份和真去截图时用的那一份必须是同一个判据算的。
    std::vector<std::wstring> autoChainWindow;
    std::vector<std::wstring> autoChainScreen;

    std::vector<ConsentPathReport> consentPaths;   // --yes 的适用范围（CaptureScope 登记表那一整份）

    struct Limits {
        // 不变量：下面每一条都必须**直接**取实现自己那一个常量（EnvReport.cpp 里就是这么赋值的），
        // 任何一条都不许在这份结构里或渲染里另写一个数 —— 这份查询存在的意义就是让调用方
        // 不必读源码也能核对"帮助说的、解析判的、这里报的是同一件事"。
        // 判据在 tests\capabilities_state.cpp 的「limits 与实现同源」那一段逐条现场核对。
        uint64_t maxFrameSide = 0;
        uint64_t maxFrameBytes = 0;
        uint64_t maxTimeoutMs = 0;
        uint64_t isolatedCallMs = 0;
        uint64_t maxWgcRecreates = 0;
        uint64_t maxOrdinal = 0;
        uint64_t maxPid = 0;
        uint64_t stdoutTargetsMax = 0;   // 标准输出一次只交付一张图
        uint64_t jpegQualityMin = 0;
        uint64_t jpegQualityMax = 0;
        uint64_t roiMaxValue = 0;        // --roi 四条数各自的上限（= 帧的单边上限那条线）
    } limits;

    // 隐私自述：这几条是真的这么实现的，写出来让调用方不必读源码就能判。
    bool privacyCapturesScreen = false;
    bool privacyShowsDialog = false;
    bool privacyUploads = false;
    bool privacyEnumeratesUserFiles = false;
    bool privacyReadsEnvVars = false;
    bool privacyIncludesUsernames = false;
    bool privacyIncludesPaths = false;

    std::vector<std::wstring> caveats;

    // --verbose 才展开的"每一问的原始答案"，方便用户核对后再提交（不展开时上面那一段已经够了）。
    // 存的是探针的副本：报告要能脱离探针的生命周期被断言与渲染（离线判据就是这么用的）。
    EnvProbe probe;

    // 报告里某个后端的状态；没有这条路线时返回 nullptr。
    const BackendReport* BackendOf(CaptureMethod method) const;
};

// 纯判据：从一份探针算出整份报告。不碰 Win32、不弹框、不取帧，所以能离线逐条注入。
EnvReport BuildEnvReport(const EnvProbe& probe, EnvQueryKind kind);

// 渲染成 JSON 文档（带结尾换行由调用方决定）。machine 可读的取值一律 ASCII，
// 不读文案资源，所以同一台机器上 --lang 换成任何一种，这份文档逐字节相同。
// verbose = 追加 probes 段与 --capabilities 的构建标识明细；quiet = 只省略 caveats 段。
std::wstring RenderEnvJson(const EnvReport& report, bool verbose, bool quiet);

}  // namespace ecapture
