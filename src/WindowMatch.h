#pragma once
// 窗口枚举与条件求值：把 --hwnd/--pid/--process/--exe/--title/--title-contains/--title-regex/--class
// 编译成对每个顶层候选窗口的 AND/OR 判定，再按选择策略消歧。
// 同时给了 --monitor 时，只在所选那块屏的矩形范围内找窗口。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"

namespace ecapture {

struct WindowInfo {
    uint64_t hwnd = 0;
    uint32_t pid = 0;
    std::wstring title;
    std::wstring className;
    std::wstring imageName;   // 映像文件名，取不到时为空
    std::wstring imagePath;   // 完整路径，取不到时为空
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t zOrder = 0;       // EnumWindows 的访问顺序，0 = 最前
    bool iconic = false;      // 最小化
};

// 命中条件的所有窗口（已按策略消歧）。
// 出错时返回空并把诊断写进 errors（无匹配 / 多匹配未消歧 / 索引越界）。
std::vector<WindowInfo> SelectWindows(const Options& opt, std::vector<Diagnostic>* errors,
                                     std::vector<Diagnostic>* notes);

// 供错误信息与调试用：把窗口列表渲染成一行文字
std::wstring DescribeWindow(const WindowInfo& w);

}  // namespace ecapture
