#pragma once
// 窗口枚举与条件求值：把 --hwnd/--pid/--process/--exe/--title/--title-contains/--title-regex/--class
// 编译成对每个顶层候选窗口的 AND/OR 判定，再按选择策略消歧。
// 同时给了 --monitor 时，只在所选那块屏的矩形范围内找窗口。
//
// 这一步拆成两半是有意为之：
//   * 求值（EnumerateMatches）里有一次跨进程取标题，还有 --title-regex 的回溯匹配 ——
//     两者都没有中断点，需要期限时把它整半交给辅助进程（见 Worker.h）。
//   * 消歧（SelectFromHits）只对着已经拿到手的列表做决定，纯计算，在哪跑都一样。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"
#include "CliOptions.h"

namespace ecapture {

// 一条跨进程问答的三种下场。「问不出来」不等于「读到了空值」，也不等于「没有这个问题」：
// 结构化窗口查询要把这一区分原样交给调用方（字段级 unknown），而不是把读不到写成空值。
// 数值是辅助进程管道上的线上格式（WorkerProtocol.h），加取值要同步协议版本。
enum class ReadState : uint32_t {
    kReadable = 0,   // 这一问给出了答案（答案本身可以是空串，那是真值）
    kDenied = 1,     // 被系统挡下（ERROR_ACCESS_DENIED / ERROR_PRIVILEGE_NOT_HELD）
    kFailed = 2,     // 问过而失败，原因码记在同一条的 win32 字段里
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
    ReadState pathRead = ReadState::kFailed;    // QueryFullProcessImageNameW 的下场
    ReadState startRead = ReadState::kFailed;   // GetProcessTimes 的下场
    uint32_t pathWin32 = 0;                     // 问句失败时的系统原因码，0 = 没失败过
    uint32_t startWin32 = 0;
    std::wstring title;
    std::wstring className;
    std::wstring imageName;   // 映像文件名，取不到时为空
    std::wstring imagePath;   // 完整路径，取不到时为空
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    // 窗口矩形那一问的下场：GetWindowRect 失败时上面四个数是没问出来的初值，
    // 不是「一个零尺寸的窗口」。查询层靠这一条把 unknown 与 0 分开写。
    ReadState rectRead = ReadState::kFailed;
    uint32_t rectWin32 = 0;
    int32_t zOrder = 0;       // EnumWindows 的访问顺序，0 = 最前
    bool iconic = false;      // 最小化
};

// 条件求值的输入：解析过的匹配条件，加上 --monitor 限缩到哪几块屏的矩形。
struct MatchRequest {
    MatchOptions match;
    std::vector<RECT> onScreens;   // 空 = 不限屏
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
