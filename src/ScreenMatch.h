#pragma once
// 屏幕（显示器）目标：--monitor 的取值在这里落到具体的某块屏上。
//
// 编号是 **本次进程这一次屏幕枚举里的位置**，1 起。它是"这次枚举里的位置"，
// 不是系统「显示设置」里那个标识号：系统那边给每块屏编的号存在配置里、可以按设备重新
// 排，而枚举顺序由驱动 / 显示服务决定，两者常常一致但不保证一致，热插拔或改分辨率
// 之后也会变。所以这里不声称"--monitor 2 就是设置里写着 2 的那块屏"，要跨调用追踪同一块屏
// 请认 --screens 交回的设备名（Windows 是 `\\.\DISPLAY1`；X11 是逻辑显示器名，
// 物理输出名再单独用 `output:` 点名）。
// 给出这个顺序的枚举本身按平台分（core/Platform.h 的 MonitorOrdinalKind）：Windows 是
// EnumDisplayMonitors，X11 是 XRRGetMonitors。
// 矩形用的是虚拟屏幕坐标系的物理像素（进程已声明 per-monitor DPI v2）。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "core/Geometry.h"   // PhysRect：该屏在虚拟屏幕上的矩形（跨平台共用坐标模型）

namespace ecapture {

// XRandR 的一个输出设备。它与逻辑显示器不是同一个概念：镜像模式下多个输出
// 可以共享同一块桌面矩形。XID 只在同一个 X server 会话内可比较，不能存档跨会话使用。
struct ScreenOutputInfo {
    uint64_t outputId = 0;       // RROutput XID（本次 X server 会话）
    uint64_t crtcId = 0;         // RRCrtc XID（0 = 没有活动 CRTC）
    std::wstring name;           // XRandR 输出名（本次桌面连接）
    bool connected = false;      // RandR ConnectionStatus；与"当前是否有画面"分开
    bool active = false;         // 当前有活动 CRTC / 在 XRRGetMonitors 的组里
    bool primary = false;        // XRandR 输出主屏标记
    int rotationDegrees = -1;    // 输出在根窗口中的旋转变换；-1 = 没问出来
    PhysRect bounds{};           // 该输出当前 CRTC 的根坐标矩形（未旋转前的范围以 RandR 为准）
};

struct ScreenInfo {
    // 拿它去采样的那块屏对象：Windows 是 HMONITOR（wgc 通道拿它建 GraphicsCaptureItem），
    // X11 上只是本次枚举的序号（X11 那一路按 deviceName 找逻辑显示器）。
    uint64_t monitor = 0;
    uint32_t ordinal = 0;     // 1 起的屏幕编号（本次枚举里的位置）
    // 本次桌面连接里这块**逻辑显示器**叫什么：Windows 是 `\\.\DISPLAY1`，X11 是
    // XRandR monitor 名；拿不到 monitor atom 时退回第一个输出名，再退回 `X11-<编号>`。
    std::wstring deviceName;
    PhysRect bounds{};        // 该逻辑显示器在根/虚拟屏幕上的矩形（已含旋转后的范围）
    bool primary = false;
    // X11 的附加事实。Windows 上保持"没问过"的默认值，不拿占位值冒充。
    std::wstring logicalName;  // XRandR monitor atom 名（与 deviceName 同值或为空）
    std::vector<ScreenOutputInfo> outputs;
    bool automatic = false;    // XRRMonitorInfo::automatic
    bool mirrored = false;     // 这个逻辑显示器由多个输出共享（复制模式）
    bool hasScreenIdentity = false;
    uint64_t rootWindow = 0;
    std::wstring displayScope; // 本次 X11 连接作用域（ASCII，仅同后端内比较）
    bool desktopKnown = false;
    PhysRect desktopBounds{};  // 根窗口范围；X11 根坐标，可能从 0,0 开始
};

// 拓扑指纹：数量、根窗口、逻辑显示器、输出设备、镜像、旋转与主屏归属全部纳入。
// 只用于同一次进程内比较"人确认前后还是不是同一份布局"，不是可持久化的身份。
uint64_t ScreenTopologyFingerprint(const std::vector<ScreenInfo>& screens);

// 本机全部屏幕，按编号升序。
// 【平台入口】实现见 src/platform/windows/WinScreenSource.cpp（EnumDisplayMonitors）与
// src/platform/linux/PosixScreenSource.cpp（XRRGetMonitors）；没有 X 连接时交回空表。
std::vector<ScreenInfo> EnumScreens();

// 按 opt.monitor 取出目标屏（all = 全部；未给编号 = 主屏；否则按编号取一块）。
// 编号越界时写 match.monitor_out_of_range 诊断并返回空。
std::vector<ScreenInfo> SelectScreens(const Options& opt, std::vector<Diagnostic>* errors);

// "这个窗口算不算落在所选屏上"：矩形与该屏有重叠即算，跨屏窗口在两块屏上都能被找到。
std::vector<PhysRect> SelectedScreenRects(const Options& opt, std::vector<Diagnostic>* errors);

// 屏幕目标的"身份"在本进程内靠设备名追。编号（ordinal）只是本次屏幕枚举给的位置，
// 热插拔或改分辨率之后同一个编号可能指到另一块屏上，所以它不能用来核对"还是那块屏"。
// 复核结果决定这一步还要不要取帧：
//   kSame  —— 设备名与矩形都没变
//   kMoved —— 还是那块屏，但它的矩形或编号变了（改了分辨率、挪了位置、插拔之后顺序变了）：
//             用 *fresh 换掉手里的旧值，让人重新确认新的范围，绝不把旧授权用在新尺寸上
//   kGone  —— 那块屏已经不在桌面里（拔掉或禁用）：一个像素都不读
enum class ScreenCheck { kSame, kMoved, kGone };

// 用当下重新枚举的列表核对一块屏。current 由调用方给（真机传 EnumScreens()，
// 测试注入假布局），本函数不碰系统。
ScreenCheck CompareScreen(const ScreenInfo& wanted, const std::vector<ScreenInfo>& current,
                          ScreenInfo* fresh);

// 一行描述，用于 --dry-run 与 hint
std::wstring DescribeScreen(const ScreenInfo& s);
std::wstring BriefScreenList(const std::vector<ScreenInfo>& screens);

// GDI 视图设备名的前缀（形如 `\\.\`，一共四个字符）。这一条有三处在用：
// %n 占位符要的裸设备名、--monitor=device: 写的标识本体、以及把设备名与显示配置那条
// 活动路径对号时的比较键。三份各写一遍迟早打脸，所以形状只在这里写一次。
// 只去前缀，本体一字不改 —— 设备名是系统用来认对象的键，不是要归一化的文本。
// 只有 Windows 的设备名带这个前缀；X11 的输出名不带，原样返回。
inline std::wstring StripScreenDevicePrefix(const std::wstring& v) {
    const std::wstring prefix = L"\\\\.\\";
    if (v.size() > prefix.size() && v.compare(0, prefix.size(), prefix) == 0) {
        return v.substr(prefix.size());
    }
    return v;
}

// 去掉前缀的设备名（"DISPLAY1"），给 %n 占位符当文件名用
std::wstring ScreenDisplayName(const ScreenInfo& s);

}  // namespace ecapture
