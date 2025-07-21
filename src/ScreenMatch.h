#pragma once
// 屏幕（显示器）目标：--monitor 的取值在这里落到具体的某块屏上。
//
// 编号是 **本次进程这一次 EnumDisplayMonitors 的顺序**，1 起。它是"这次枚举里的位置"，
// 不是 Windows「显示设置」里那个标识号：系统那边给每块屏编的号存在注册表里、可以按设备重新
// 排，而 EnumDisplayMonitors 的顺序由驱动枚举决定，两者常常一致但不保证一致，热插拔或改分辨率
// 之后也会变。所以这里不声称"--monitor 2 就是设置里写着 2 的那块屏"，要跨调用追踪同一块屏
// 请认 images[].device（"\.\DISPLAY1" 这个形状的设备名）。
// 矩形用的是虚拟屏幕坐标系的物理像素（进程已声明 per-monitor DPI v2）。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CliOptions.h"

namespace ecapture {

struct ScreenInfo {
    uint64_t monitor = 0;     // HMONITOR，wgc 通道拿它建 GraphicsCaptureItem
    uint32_t ordinal = 0;     // 1 起的屏幕编号
    std::wstring deviceName;  // "\\.\DISPLAY1"，duplication 通道按它挑 DXGI 输出
    RECT bounds{};            // 该屏在虚拟屏幕上的矩形
    bool primary = false;
};

// 本机全部屏幕，按编号升序。
std::vector<ScreenInfo> EnumScreens();

// 按 opt.monitor 取出目标屏（all = 全部；未给编号 = 主屏；否则按编号取一块）。
// 编号越界时写 match.monitor_out_of_range 诊断并返回空。
std::vector<ScreenInfo> SelectScreens(const Options& opt, std::vector<Diagnostic>* errors);

// "这个窗口算不算落在所选屏上"：矩形与该屏有重叠即算，跨屏窗口在两块屏上都能被找到。
std::vector<RECT> SelectedScreenRects(const Options& opt, std::vector<Diagnostic>* errors);

// 屏幕目标的"身份"在本进程内靠设备名追。编号（ordinal）只是 EnumDisplayMonitors 在本次枚举里
// 给的位置，热插拔或改分辨率之后同一个编号可能指到另一块屏上，所以它不能用来核对"还是那块屏"。
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

// 去掉 "\\.\\" 前缀的设备名（"DISPLAY1"），给 %n 占位符当文件名用
std::wstring ScreenDisplayName(const ScreenInfo& s);

}  // namespace ecapture
