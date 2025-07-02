#pragma once
// 截图流水线：匹配窗口 -> WGC 取帧 -> 编码 -> 落盘（或写标准输出）。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"

namespace ecapture {

struct CapturedImage {
    std::wstring file;         // 已展开占位符的绝对路径；"-" 表示写到标准输出
    uint64_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::wstring format;       // "png" 等
    std::wstring hwndHex;
    uint32_t pid = 0;
    std::wstring title;
    std::wstring windowClass;
    std::wstring imageName;
    uint32_t elapsedMs = 0;
    // 屏幕目标（--monitor 且无窗口条件）：没有窗口可归属，改带屏幕信息
    bool screen = false;
    uint32_t monitorOrdinal = 0;
    std::wstring deviceName;
    bool primary = false;
};

struct CaptureOutcome {
    std::vector<CapturedImage> images;
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> notes;
    int exitCode = 0;          // 与退出码一致；没有图片且出错时非 0
};

// 执行整条链路。假定 opt 已通过参数校验且至少有一个条件。
CaptureOutcome RunCapture(const Options& opt);

// 展开 --out 里的占位符：%d 日期 %t 时间 %h 句柄 %p 进程 %i 序号 %n 标题
std::wstring ExpandOutputPath(const std::wstring& pattern, const std::wstring& hwndHex, uint32_t pid,
                              size_t ordinal, const std::wstring& title);

}  // namespace ecapture
