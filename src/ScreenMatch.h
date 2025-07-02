#pragma once
// 屏幕（显示器）目标：--monitor 的取值在这里落到具体的某块屏上。
//
// 编号按 EnumDisplayMonitors 的顺序，从 1 起；这顺序与"显示设置"里列出的顺序一致，
// 也和主屏的判定（MONITORINFOF_PRIMARY）同源，所以不引入第二套编号。
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

// 一行描述，用于 --dry-run 与 hint
std::wstring DescribeScreen(const ScreenInfo& s);
std::wstring BriefScreenList(const std::vector<ScreenInfo>& screens);

// 去掉 "\\.\\" 前缀的设备名（"DISPLAY1"），给 %n 占位符当文件名用
std::wstring ScreenDisplayName(const ScreenInfo& s);

}  // namespace ecapture
