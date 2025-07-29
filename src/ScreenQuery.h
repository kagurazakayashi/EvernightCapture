#pragma once
// EvernightCapture —— 只读的屏幕枚举（--screens）
//
// 为什么要有这一层：调用方（尤其是 AI）要指定"截哪块屏"，此前只有一种写法 ——
// `--monitor <n>`，而那个 n 是本次枚举顺序里的位置，既不是「显示设置」里那个号，
// 也不保证插拔之后还指同一块屏。也就是说，它必须先跑一次什么东西、看一眼结果，
// 再**猜**一个数字。这一层把"本机现在有哪几块屏、每块屏各有哪几种身份、那几种身份
// 分别稳在哪一层"变成一次只读的结构化问答，交回的标识可以直接写回 --monitor，
// 于是"选哪块屏"从猜测变成点名。
//
// 五条规矩（与 src/ScreenIdentity.h 那四条同源，改代码前先对齐这里）：
//
//   1. **只读。** 一个像素都不取、不调任何取帧通道、不弹确认框、不写文件、不联网、
//      不改任何显示设置（不调 ChangeDisplaySettings / SetDisplayConfig —— 为了看清旋转
//      而去改旋转，等于把考卷改了再答题）。
//   2. **这份列表不是凭证。** 交回的每个字段都是问那一刻的值。真去截整屏仍然一定弹框，
//      `--yes` 对桌面像素那一级**不生效**（见 src/CaptureScope.cpp 的登记表），
//      而且取帧之前还要按 ScreenIdentity.h 那条跨会话标识再核一次身份。
//   3. **身份的稳定性逐条写出来。** 设备名 / devnode 路径 / 适配器 LUID / 编号说的是四件事，
//      所以各带一条 stableAcross（this_invocation / this_desktop_attach / this_session /
//      cross_session_expected），并且**只有**前两条有选择器写法。这里不作任何长期承诺：
//      本项目只在开发机的同一个会话里观察过它们，跨重启与跨会话没有实测（caveat 里钉着）。
//   4. **问不出来就是问不出来。** DPI、旋转、devnode 路径、适配器路径各自带下场
//      （readable / denied / failed + 那一条 API 自己的错误码）。读不到不写成 0、
//      不写成空串、也不写成 false，更不因此要求以管理员运行。
//   5. **DPI 与旋转是两条不同的问句。** 用户看到的朝向（DEVMODE 那一条）与"相对面板原生朝向
//      要转多少"（显示配置那一条）不是同一件事，两份各写各的，不互相换算也不互相冒充。
//
// 判据本体（BuildScreenQuery / RenderScreenQuery）是纯函数：候选表由调用方交进来，
// 所以"一块屏都没有""devnode 问不到""同一视图设备挂着两条路径""负坐标的副屏"这些现场
// 都由 tests\screens_state.cpp 注入逐条判。真机问答在 ScreenIdentity.h。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "ScreenIdentity.h"

namespace ecapture {

// 这份文档的机器可读契约名与版本（与 --capabilities / --list 那几份各自演进，互不影响）。
inline constexpr const wchar_t* kScreenQueryContractName = L"screens";
inline constexpr uint32_t kScreenQueryContractVersion = 1;

// 一种身份的"稳到哪一层"。取值只增不改名，且与 README / cli-contract.md 里那张表同源。
namespace screen_stability {
inline constexpr const wchar_t* kThisInvocation = L"this_invocation";       // 本次进程这一次枚举
inline constexpr const wchar_t* kThisAttach = L"this_desktop_attach";       // 本次桌面连接
inline constexpr const wchar_t* kThisSession = L"this_session";             // 本次登录会话 / 本次开机
inline constexpr const wchar_t* kCrossSession = L"cross_session_expected";  // 由设备节点决定，跨会话
}  // namespace screen_stability

// caveats 的稳定 token：这一组说的都是"这份文档**没有**断言什么"。
namespace screen_caveat {
inline constexpr const wchar_t* kNoCapture = L"no_capture_performed";
inline constexpr const wchar_t* kNoDialog = L"no_consent_dialog_shown";
inline constexpr const wchar_t* kNoFiles = L"no_files_written";
inline constexpr const wchar_t* kNoSettingsChange = L"no_display_settings_changed";
// 每个字段都是问那一刻的值，会过期。
inline constexpr const wchar_t* kSnapshotExpires = L"snapshot_expires";
// 交回的标识不能代替取帧之前的身份复核，也不是免确认的凭证。
inline constexpr const wchar_t* kNotAToken = L"identifiers_are_not_authorization";
// 设备名与编号会被系统重新发出去：它们不是可以存档以后长期复用的键。
inline constexpr const wchar_t* kDeviceNamesReassigned = L"device_names_are_not_persistent";
// 跨会话稳定性只在同一会话内观察过：本项目没有在真机上重启或换会话实测过这一条。
inline constexpr const wchar_t* kCrossSessionUntested = L"cross_session_stability_not_tested";
// 真去截整屏一定要人点头，--yes 对桌面像素那一级不生效。
inline constexpr const wchar_t* kScreenShotAlwaysAsks = L"screen_capture_always_asks";
// 显示配置那一路没能给出答案（这一次的身份字段全是 unknown）。
inline constexpr const wchar_t* kConfigUnverifiable = L"display_topology_unavailable";
// 读出来的是设备身份，不是人名：里面没有用户名与文件系统路径。
inline constexpr const wchar_t* kNamesHardware = L"identifiers_name_hardware_not_users";
}  // namespace screen_caveat

// 一次屏幕枚举问答的结果（也是渲染器的唯一输入）。
struct ScreenQueryResult {
    // 这份文档出完了就是 0，哪怕里面写着这台机器问不出来 —— 与 --capabilities 同一条规矩。
    int exitCode = 0;
    std::vector<ScreenCandidate> screens;
    // 整次 QueryDisplayConfig 的下场（同一份表里各屏共用，所以它在文档顶层写一次）。
    ScreenQuestion config;
    std::vector<Diagnostic> notes;
    // 虚拟屏幕的并集矩形（负坐标在这里看得见：副屏可以在主屏左边或上边）。
    int32_t virtualX = 0;
    int32_t virtualY = 0;
    uint32_t virtualWidth = 0;
    uint32_t virtualHeight = 0;
    bool virtualKnown = false;
    // 有几块屏在显示配置里对不上任何一条活动路径（对不上不等于它不存在）。
    uint32_t unmatchedPaths = 0;
    // 有几块屏的同一个视图设备挂着多条路径（复制模式下两块面板共享一个桌面）。
    uint32_t clonedScreens = 0;
};

// 纯判据：从一张候选表算出文档级事实与那条"快照会过期"的提示。不碰 Win32。
ScreenQueryResult BuildScreenQuery(const std::vector<ScreenCandidate>& all);

// 真机入口：问一遍（含显示配置那一路）再交给 BuildScreenQuery。
ScreenQueryResult RunScreenQuery();

// 渲染。机器可读的取值全 ASCII；监视器友好名是设备自报的，原样交付。
// verbose = 追加 input 段；quiet = 只省略 notes，caveats / identity / readability 都不省略。
std::wstring RenderScreenQuery(const ScreenQueryResult& r, bool verbose, bool quiet);

}  // namespace ecapture
