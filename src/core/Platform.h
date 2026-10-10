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
// 不再各自 #ifdef。Linux 侧目前还没有后端，所以非 Windows 构建如实返回 kUnknown，
// 而不是猜一个"大概是 X11 吧"—— 猜出来的平台身份会被能力矩阵当成事实读。
inline PlatformKind BuildPlatform() {
#if defined(_WIN32)
    return PlatformKind::kWindows;
#else
    return PlatformKind::kUnknown;
#endif
}

// 这个平台族的名字能不能写进产物（例如 --capabilities 里那一条）。
// kUnknown 返回 false：没有平台身份时连"这是哪台机器"都说不准，更不该写成一个具体名字。
inline bool PlatformIsKnown(PlatformKind platform) {
    return platform != PlatformKind::kUnknown;
}

}  // namespace ecapture
