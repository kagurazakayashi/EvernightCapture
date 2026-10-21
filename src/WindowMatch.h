#pragma once
// 窗口枚举与条件求值：把 --hwnd/--pid/--process/--exe/--title/--title-contains/--title-regex/--class
// 编译成对每个顶层候选窗口的 AND/OR 判定，再按选择策略消歧。
// 同时给了 --monitor 时，只在所选那块屏的矩形范围内找窗口。
//
// 这一步拆成两半是有意为之：
//   * 求值（EnumerateMatches）里有正则库的编译、一次跨进程取标题，还有 --title-regex 的
//     回溯匹配 —— 三者都没有中断点，需要期限时把它整半交给辅助进程（见 Isolation.h）。
//   * 求值（EnumerateMatches）本身住在 src/WindowMatchRun.cpp：它是唯一调用平台窗口枚举的地方，
//     纯匹配规则（CompileConditions / MatchesWindow / SelectFromHits）留在这份文件与它的 .cpp 里。
//     解析层不预先编译"验语法"（CliOptions.cpp 的 --title-regex）：这里就是唯一的编译点，
//     预算与隔离管不管得到，取决于调用方走 EnumerateMatches 还是 IsolatedMatch。
//   * 消歧（SelectFromHits）只对着已经拿到手的列表做决定，纯计算，在哪跑都一样。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "core/Geometry.h"   // PhysRect：--monitor 限缩到哪几块屏的矩形
#include "core/Status.h"     // BlockedStatus：那一步停在哪儿（与辅助进程同一份线上词表）

namespace ecapture {

// 一条跨进程问答的三种下场。「问不出来」不等于「读到了空值」，也不等于「没有这个问题」：
// 结构化窗口查询要把这一区分原样交给调用方（字段级 unknown），而不是把读不到写成空值。
// 数值是辅助进程管道上的线上格式（WorkerProtocol.h），加取值要同步协议版本。
enum class ReadState : uint32_t {
    kReadable = 0,   // 这一问给出了答案（答案本身可以是空串，那是真值）
    kDenied = 1,     // 被系统挡下（ERROR_ACCESS_DENIED / EACCES / X11 BadAccess）
    kFailed = 2,     // 问过而失败，原因码记在同一条的 win32 / errno / X 错误字段里
    // 这一问成立，但对象本身没有声明该属性（例如 X11 窗口没有 `_NET_WM_PID`）。
    // 它与 kFailed 必须分开：缺失是窗口的事实，读取失败是这一次问答的故障，
    // 调用方不能把前者当成"这个值是 0 / 空串"。数值是辅助进程管道上的线上格式，
    // 加取值要同步 core/WorkerFraming.h 的 kProtocolVersion 与 WorkerProtocol.cpp 的判界。
    kAbsent = 3,
};

// ReadState 的机器名（readable / denied / failed）。不进文案、不随 --lang 变。
// 屏幕那一路（--screens 的每一问，ScreenIdentity.h）交回的是同一个三值枚举，
// 所以这个词表也只写一处：两条路的答案形状必须能拿同一份解析代码读。
const wchar_t* ReadStateName(ReadState state);

// 归属进程这一格是怎么得到的。X11 的 `_NET_WM_PID` 是客户端自己声明的，X11 核心协议
// 没有"这个 XID 由哪个进程创建"的公开证据；读到 `/proc/<pid>` 只能证明那个进程存在，
// 不能把声明的 PID 升级成已核实的归属。Windows 的 GetWindowThreadProcessId 由系统回答，
// 这一格才是 verified。查询与复核都必须把两者分开写，不能用一个 pid 整数抹平。
enum class ProcessAttribution : uint32_t {
    kVerified = 0,          // 平台把窗口归属进程作为可信事实交回（Windows）
    kDeclaredUnverified = 1,  // 窗口自己声明的 PID（X11 EWMH），进程存在不等于归属成立
    kUnknown = 2,           // 没有 PID 可问，或问不出来
};

const wchar_t* ProcessAttributionName(ProcessAttribution attribution);

// 一扇窗口的"后端原生身份"。这一格只在本平台/本会话内有意义，不是可跨会话保存的键。
//   * backend：x11 / win32；
//   * idForm：xid / hwnd —— XID 不是 HWND，二者不可互抄格式或生命周期；
//   * sessionScope：后端会话的不可变比较串。X11 由显示名、服务端厂商标识、协议版本与
//     root XID 组成；它是**本次显示连接作用域**的记号，不承诺跨重启 / 跨 XWayland 会话；
//   * sessionRead：这一组会话事实有没有问全；
//   * scopeRequired：该后端复核时是否必须有会话基线（X11 为 true；Windows 暂为 false，
//     以保持既有 Windows 身份复核的兼容行为）。
struct WindowNativeIdentity {
    std::wstring backend;        // 恒为机器可读 ASCII 词
    std::wstring idForm;         // 恒为机器可读 ASCII 词
    ReadState sessionRead = ReadState::kFailed;
    bool sessionKnown = false;
    std::wstring sessionScope;   // opaque ASCII（不含本地化文案）
    std::wstring displayName;    // X11：DISPLAY 字符串；Windows：空
    std::wstring serverVendor;   // X11：XServerVendor；Windows：空
    int32_t serverRelease = 0;
    int32_t protocolMajor = 0;
    int32_t protocolMinor = 0;
    uint64_t rootWindow = 0;     // X11 root XID；Windows：0（不是 HWND）
    bool xwayland = false;       // X11 扩展 XWAYLAND 是否存在（仅 X11 有意义）
    bool scopeRequired = false;  // 复核时是否必须证明仍在同一会话作用域
};

struct WindowInfo {
    uint64_t hwnd = 0;
    uint32_t pid = 0;
    // 归属进程的创建时间（100 纳秒 ticks，FILETIME 原值）。0 = 枚举当时读不到。
    // 它是"同一个 PID 还是不是同一个进程"的唯一可靠判据：PID 会被系统复用，
    // 而身份复核（WindowIdentity.h）不能靠"当场再问一次"来补 —— 那等于自己跟自己对答案，
    // 所以这个值必须在**枚举那一刻**就记进这一条候选里（辅助进程枚举时也一样）。
    uint64_t processStartTicks = 0;
    // 上面那一条与下面三条问句各自的下场。默认是 kFailed 而不是 kReadable：
    // ProcessFactsOf 一条问句都没问成时（量不到 PID、开不到句柄），「没答案」是失败而不是空值。
    // 它们是**判据**字段而不是画面：结构化窗口查询按它们写「这一项能不能读」，
    // 截图链路不读它们（身份复核本来就是「基线没有的那一条整个跳过」）。
    ReadState pidRead = ReadState::kFailed;     // 归属 PID 那一问的下场
    ReadState pathRead = ReadState::kFailed;    // QueryFullProcessImageNameW / readlink 的下场
    ReadState startRead = ReadState::kFailed;   // GetProcessTimes / /proc/<pid>/stat 的下场
    uint32_t pathWin32 = 0;                     // 问句失败时的系统原因码，0 = 没失败过
    uint32_t startWin32 = 0;
    ProcessAttribution processAttribution = ProcessAttribution::kVerified;
    std::wstring title;
    ReadState titleRead = ReadState::kFailed;
    std::wstring className;
    ReadState classRead = ReadState::kFailed;
    std::wstring imageName;   // 映像文件名，取不到时为空
    std::wstring imagePath;   // 完整路径，取不到时为空
    // 客户区 / 整窗矩形在平台屏幕坐标里的那一块。Windows 这一格仍是 GetWindowRect 的
    // 整窗矩形；X11 这一格明确是客户区根坐标（外框另见 frameRect*）。坐标系单位均为
    // 该平台实际像素；没有逻辑坐标那一问。
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t borderWidth = 0;   // X11：客户窗口自己的边框宽度；不属于客户区内容
    // 窗口矩形那一问的下场：API 失败时上面四个数是没问出来的初值，
    // 不是「一个零尺寸的窗口」。查询层靠这一条把 unknown 与 0 分开写。
    ReadState rectRead = ReadState::kFailed;
    uint32_t rectWin32 = 0;
    // 客户区与装饰外框是两块不同的几何：x/y/width/height（上面那组）是**客户区**在
    // 根坐标里的位置与尺寸；这里再单独记录窗口管理器装饰外框，量不出来时保持 kFailed。
    // 不拿客户区冒充外框，也不在量不出来时编一个"屏幕矩形"填满模型。
    ReadState frameRectRead = ReadState::kFailed;
    uint32_t frameRectWin32 = 0;
    int32_t frameX = 0;
    int32_t frameY = 0;
    int32_t frameWidth = 0;
    int32_t frameHeight = 0;
    // frameRect 的来源：ewmh_net_frame_extents / x11_parent_window / client_is_top_level /
    // 空 = 没量到。后两种是真实的结构事实，不是按比例猜出来的。
    std::wstring frameRectSource;
    int32_t zOrder = 0;       // 顶层枚举顺序，0 = 最前
    bool iconic = false;      // 最小化（EWMH _NET_WM_STATE_HIDDEN 或 ICCCM IconicState）
    // 后端原生身份与会话作用域。由枚举那一刻的平台层填入；共用层只比较它，不解析原生类型。
    WindowNativeIdentity native;
    // 这一条候选此刻可不可见。平台层只枚举、不替共用层判可见性策略：不可见与零尺寸的
    // 候选照样交回来（字段是事实），要不要排除由 src/WindowMatch.cpp 的 EnumerateMatches
    // 按 --list 的既有规则判 —— 于是"列出来的"与"截图会选中的"永远走同一份策略。
    bool visible = true;
};

// 条件求值的输入：解析过的匹配条件，加上 --monitor 限缩到哪几块屏的矩形。
struct MatchRequest {
    MatchOptions match;
    std::vector<PhysRect> onScreens;   // 空 = 不限屏
};

// 条件求值的结果。失败只给原因码（文字由调用方按 --lang 现取），
// 因为这一半可能在辅助进程里跑 —— 那个进程里没有人话文案，也不该有。
struct MatchOutcome {
    BlockedStatus status = BlockedStatus::kOk;
    std::string detail;             // 只放 ASCII 细节
    std::vector<WindowInfo> hits;   // 命中且可见，按 z 序
    std::vector<WindowInfo> iconic; // 命中但最小化（只为给 hint）
};

// 枚举全部顶层窗口并按条件求值（含 --title-regex 的正则编译与回溯匹配）。
// 纯在本进程执行：不弹框、不取帧、不写文件，所以辅助进程可以直接调它。
// 反过来也正巧成立：需要期限保护的就是这一步里的正则与跨进程取标题，
// 所以"要限时"的调用方走 IsolatedMatch()，而不是在这里等。
MatchOutcome EnumerateMatches(const MatchRequest& req);

// --monitor 的人话标签（"all" / "primary" / 编号），给无匹配那条 hint 用。
std::wstring MonitorLabelOf(const Options& opt);

// 按选择策略从命中列表里定下目标（一个窗口，或 --all 的全部）。
// 出错时返回空并把诊断写进 errors（无匹配 / 多匹配未消歧 / 索引越界）。
std::vector<WindowInfo> SelectFromHits(const Options& opt, const std::vector<WindowInfo>& hits,
                                       const std::vector<WindowInfo>& iconic,
                                       const std::wstring& monitorLabel,
                                       std::vector<Diagnostic>* errors);

// 供错误信息与调试用：把窗口列表渲染成一行文字
std::wstring DescribeWindow(const WindowInfo& w);

}  // namespace ecapture
