#pragma once
// 截图流水线：匹配窗口 -> WGC 取帧 -> 编码 -> 落盘（或写标准输出）。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // RECT（images[].rect 用的是屏幕矩形）

#include "CliOptions.h"

namespace ecapture {

struct CapturedImage {
    std::wstring file;         // 已展开占位符的绝对路径；"-" 表示写到标准输出
    uint64_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::wstring format;       // "png" 等
    std::wstring source;       // 真正产出这帧的通道名（auto 回退后可能是链尾那一条）
    // 实际走的那条内部路径名（"dwm.thumbnail" / "dwm.screen" / "bitblt.screen" / ...）。
    // 一条通道可以有多条路径，光看 source 分不出"读的是窗口自己的画面"还是"读的是屏幕"。
    std::wstring path;
    // 这一帧的像素来源："window" = 只有所选窗口自己的画面，"desktop" = 屏幕上那块区域，
    // 可能含其它窗口。由 path 算出来，所以它与实际授权的那次判断同源，不会各说一套。
    std::wstring scope;
    RECT rect{};               // 授权与实际取样的那块屏幕矩形（量不出来时整个键不输出）
    // 会读桌面像素的那几条通道（duplication、拷屏幕的 bitblt）另外报告"实际截到的那一块"：
    // rect 是请求的目标矩形，capturedRect 是真正截到的那块，两者不同就是 clipped —— 调用方
    // 由此能判"这张图不是完整目标"，而不是拿一张比窗口小的图当成整个窗口。
    // reportsCrop 为假表示这条通道截的就是整个目标（窗口内容路径），没有丢区域这回事。
    bool reportsCrop = false;
    RECT requestedRect{};
    RECT capturedRect{};
    bool clipped = false;
    uint32_t rotation = 0;     // reportsCrop 为真时有效：交付前对桌面帧顺时针转的度数
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

}  // namespace ecapture
