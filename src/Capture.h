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
#include "CropGeometry.h"   // ImageRect：images[].cropRect 用的那个图像像素坐标矩形
#include "CursorControl.h"  // CursorReport：images[] 里光标那三个键的本体（判据在那个头文件）

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
    // 窗口内部裁剪（--roi / --client-area）：只有这一次真的裁了才写这一组字段。
    // cropRect 是**这张交付图像自己的像素坐标**（左上角 = (0,0)，物理像素），不是桌面坐标；
    // cropScreenRect 是同一块矩形在虚拟屏幕坐标里的那一块 —— 它和 rect / capturedRect 同一套系，
    // 所以调用方能直接对上号。只有在能核实这块图像的屏幕原点时才写（核实不出来就整个键不出现，
    // 并留一条 note.crop_mapping_unavailable）。
    // 于是坐标映射这一层是闭合的：图像原点 = cropScreenRect 左上角 − cropRect 左上角，
    // 而裁之前的整窗图像尺寸在 fullWidth / fullHeight，裁之后的最终尺寸就是 width / height。
    bool cropped = false;
    std::wstring cropMode;   // "roi" / "client-area"
    ImageRect crop{};
    uint32_t fullWidth = 0;  // 裁之前那张整窗图像的尺寸
    uint32_t fullHeight = 0;
    bool hasCropScreen = false;
    RECT cropScreen{};
    // 光标（--cursor）：交出去的这一帧里"有没有光标"这件事的报告，三个键一次算完
    // （requested / effective / basis）。written=false（没写过 --cursor）时渲染层一个键都不写，
    // 于是那条流与这条选项存在之前逐字节相同。合成判据只有 MakeCursorReport 那一份。
    CursorReport cursor;
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
