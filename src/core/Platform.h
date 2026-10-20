#pragma once
// 跨平台核心：平台族的身份词表。
//
// 这一层是"哪个系统、哪条后端"的唯一说法处。它只做一件事 —— 把平台写成一个可以比较、
// 可以登记状态、可以进能力矩阵的**枚举值**，而不是让 HWND、Display*、portal object path
// 这些原生句柄渗进共用的数据模型里（AGENTS.md《结构与依赖边界》：共用核心不暴露原生资源）。
//
// 三条规矩：
//   1. 平台族是**编译期**能确定的事（BuildPlatform），而"这次跑在 X11 还是 Wayland 上"是
//      **运行时**探测出来的事（由 EnvReport 那一路的三值判据回答）。两者不混成一个字段。
//   2. 认不出来的平台一律 kUnknown。不拿 kLinuxX11 当"非 Windows 的默认值"，也不因为
//      DISPLAY 变量存在就断定是 X11 —— 那是 XWayland 也满足的条件。
//   3. 枚举的数值是给能力矩阵与将来的序列化用的，只增不改名；新增取值要同时补那一格的判定，
//      没有判定就写 kUnknown，不填一个看起来合理的默认值。

#include <cstdint>

namespace ecapture {

enum class PlatformKind : uint32_t {
    kWindows = 0,      // Win32 + WinRT 那一路（WGC / DWM / PrintWindow / BitBlt / Desktop Duplication）
    kLinuxX11 = 1,     // X11 协议那一路（含 XWayland 里可见的那部分窗口）
    kLinuxWayland = 2, // 原生 Wayland：截屏只能走 XDG Desktop Portal + PipeWire
    kUnknown = 3,      // 认不出来 = 不作任何断言
};

// 平台族的 ASCII 机器名（能力矩阵与将来的 JSON 都读这一份，不在三处各拼一遍）。
// 恒为 ASCII：判据不随 --lang 变。
inline const char* PlatformName(PlatformKind platform) {
    switch (platform) {
        case PlatformKind::kWindows: return "windows";
        case PlatformKind::kLinuxX11: return "linux-x11";
        case PlatformKind::kLinuxWayland: return "linux-wayland";
        case PlatformKind::kUnknown: return "unknown";
    }
    return "unknown";  // 加了枚举值却没登记：按"认不出来"处理，不冒充任何一种平台
}

// 这个构建是**为哪个平台族**编译的。
//
// 这是全仓第一处按平台分支的代码，也是它唯一该出现的地方：往下的共用核心只读这个函数的结果，
// 不再各自 #ifdef。平台族只回答"为哪个系统编译"，不回答"这次跑在哪个会话上"——后者是运行时
// 探测（EnvReport）的事，也不能因为某个后端还没接进来就把平台身份写成 kUnknown。
inline PlatformKind BuildPlatform() {
#if defined(_WIN32)
    return PlatformKind::kWindows;
#elif defined(__linux__)
    // Linux 构建只带 X11 单窗口后端（XWayland 里可见的那部分 X11 窗口也算）。原生 Wayland
    // 的 Portal / PipeWire 后端还没有接进来，所以这里不写 kLinuxWayland —— 那是运行时
    // 会话探测的事（EnvReport），不是编译期平台族。缺 DISPLAY 时取帧会如实报不可用。
    return PlatformKind::kLinuxX11;
#else
    return PlatformKind::kUnknown;
#endif
}

// 这个平台族的名字能不能写进产物（例如 --capabilities 里那一条）。
// kUnknown 返回 false：没有平台身份时连"这是哪台机器"都说不准，更不该写成一个具体名字。
inline bool PlatformIsKnown(PlatformKind platform) {
    return platform != PlatformKind::kUnknown;
}

// 本平台的可执行文件名，给"文案里要提到自己是哪个程序"的地方用（帮助首屏、用法行、示例）。
//
// 这是**事实**而不是措辞：Windows 上的文件名是 ECAPTURE.EXE，Linux 上是 ecapture，
// 写错就是让用户照着敲一条不存在的命令。所以它跟平台族一样放在这里，由文案以 %exe% 取用，
// 各语言只翻译句子、不各自拼程序名。
inline const wchar_t* PlatformExecutableName() {
#if defined(_WIN32)
    return L"ECAPTURE.EXE";
#elif defined(__linux__)
    return L"ecapture";
#else
    // 认不出平台就不冒充任何文件名：文案里会留下 %exe% 的字面痕迹，而不是让人敲错命令。
    return L"%exe%";
#endif
}

// --process 的取值在比对前要不要补 .exe。
//
// 这是本平台对"进程映像名"的命名约定，不是措辞：Windows 的映像名带扩展名（notepad.exe），
// 所以用户写 --process notepad 时要按 notepad.exe 去比；Linux 的映像名取自 /proc/<pid>/exe
// 的文件名（xterm、firefox），本来就不带扩展名，替它补一个 .exe 只会谁都对不上。
// 解析期那条 note.extension_appended 与匹配期真的补后缀读的是**同一个**判据：
// 两处各写一份，迟早出现"说是按 app.exe 比的，实际按 app 比"这种对不上的结果。
inline bool ProcessNameAssumesExeSuffix() {
#if defined(_WIN32)
    return true;
#else
    // 不带扩展名的平台，以及认不出来的平台：不替用户编一个后缀出来。
    return false;
#endif
}

// --monitor 的两条标识写法（--screens 交回的选择器形态，也是 --monitor= 的取值形状）
// 在本平台成不成立。
//
//   device:<名字>  —— 这块屏在**本次桌面连接**里叫什么。Windows 是 GDI 的视图设备名
//                    （`\\.\DISPLAY1`），X11 是 XRandR 输出名（`HDMI-1`；拿不到输出名时
//                    退回 `X11-<编号>`）。两边都有这一层名字，所以两边都收：解析层只判形状，
//                    选屏那一步按枚举交回的 deviceName 逐字符比（ScreenIdentity.cpp）。
//   id:<路径>      —— 监视器设备节点（devnode）的设备接口路径。只有 Windows 的显示配置
//                    （QueryDisplayConfig）答得上来；Linux 构建里那一路整个不存在
//                    （ScreenCandidate::facts.hasFacts 恒为 false），所以不收，也绝不拿
//                    "看起来像路径"的输出名去冒充跨会话标识。
//
// 为什么写成函数而不是各调用点自己判平台：解析期那条 cli.platform_unsupported、帮助里的取值
// 提示、--screens 里的 usableAsSelector，三处说的是同一件事。各写一遍就有三处各说一套的余地
// （帮助里印着 device: 而解析层一给就拒），那正是这一条要消掉的不一致。
inline bool MonitorDeviceSelectorSupported() {
    // 两个平台族都答得上来"本次连接里这块屏叫什么"，所以恒成立；写成函数是为了让读它的三处
    // 与下面那一条同形，不必在调用点上重新判断平台。
    return true;
}

inline bool MonitorPathSelectorSupported() {
#if defined(_WIN32)
    return true;
#else
    // 没有 devnode 那一路（X11 不提供监视器的设备接口路径）：不编一条看起来像的东西出来。
    return false;
#endif
}

// "这次枚举里的第几块"这个编号在本平台是谁给的（--screens 的 identity.ordinal.kind）。
//
// 编号本身两个平台都有、`--monitor <n>` 两边都收，但**给出这个顺序的枚举**不是同一个东西：
// Windows 是 `EnumDisplayMonitors` 的驱动枚举顺序，X11 是 `XRRGetMonitors` 交回的监视器列表
// 顺序（拿不到 RandR 时退回"整个根窗口算唯一一块"）。两者都只是"这次枚举里的位置"，
// 都可能在插拔或改分辨率之后指到另一块屏 —— 这一点不变；变的只是这个位置的出处，
// 所以出处照实写，不拿 Windows 的枚举名去描述 X11 交回的编号。
inline const wchar_t* MonitorOrdinalKind() {
#if defined(_WIN32)
    return L"enum_display_monitors_position";
#elif defined(__linux__)
    return L"xrandr_monitor_position";
#else
    // 认不出来的平台不冒充任何一种枚举的出处。
    return L"unspecified_ordinal_position";
#endif
}

// "本次桌面连接里这块屏叫什么"这一层名字在本平台是哪一套（--screens 的 identity.deviceName.kind）。
// 同一个槽位，两种系统的答案不是同一个东西，所以照实写，不拿 Windows 的叫法去描述 X11 的输出名。
inline const wchar_t* MonitorDeviceNameKind() {
#if defined(_WIN32)
    return L"gdi_view_device_name";
#elif defined(__linux__)
    return L"xrandr_output_name";
#else
    // 认不出来的平台不冒充任何一种名字的出处。
    return L"unspecified_device_name";
#endif
}

}  // namespace ecapture
