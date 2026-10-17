#pragma once
// ---------------------------------------------------------------------------
// 【平台层 · Windows】原生类型与跨平台核心之间的唯一转换处
//
// 归属：src/platform/windows —— 这一头文件只在 Windows 构建里被 include，
// 共用核心（src/core/）与截图通道之外的那几层都不许把它拉进去。
//
// 为什么要有它：跨平台核心只认 core/Geometry.h 的 PhysRect 与 core/Error.h 的
// NativeErrorCode，而 Win32 API 只认 RECT 与 DWORD / HRESULT。两边之间必须有一次**看得见**的
// 转换，否则要么核心里渗出 HWND 那一族的类型，要么每个通道自己写一遍逐字段搬运。
// 这里刻意逐字段赋值，不做 memcpy、也不把 PhysRect 定义成 RECT 的别名：
// 别名等于宣称"三家平台都有一个 RECT"，而转换函数在代码里出现的那一次，就是审查时
// 能看见"这里跨了一次平台边界"的那一次（AGENTS.md：不通过简单类型别名把 Windows 语义
// 伪装为跨平台语义）。
//
// 【过渡桥接】本文件与下面那个 WaitTimeout 都是本步特意留下的桥：
//   * 矩形转换在 Linux 后端接进来之后仍然需要（那时是 X11/XRect ↔ PhysRect），
//     但不会再有"整个核心都按 RECT 说话"这种反向依赖。
//   * WaitTimeout 原先写在 Deadline.h 里，把 INFINITE 与 DWORD 带进了共用的截止时间头；
//     本步把它挪到平台侧，Deadline 本体（uint64 毫秒 + 单调时钟）保持平台无关。
//     调用方只有 Encoder.cpp 与 Worker.cpp 两处，两处本来就都在 Windows 执行段上。
//   * 后续要拆的是 WorkerProtocol.h / WorkerIo.h（命名管道与 OVERLAPPED 的线上格式）、
//     Notification 的 WinRT 投递、CaptureCommon.h 的 Dib 与 D3D 拷贝，本步**没有**动它们。
// ---------------------------------------------------------------------------

#include <windows.h>

#include <vector>

#include "Deadline.h"        // Deadline（本头文件按裸名 include，与各通道头一样走 src 包含路径）
#include "core/Geometry.h"   // PhysRect

namespace ecapture {

inline RECT ToNativeRect(const PhysRect& r) {
    RECT out{};
    out.left = r.left;
    out.top = r.top;
    out.right = r.right;
    out.bottom = r.bottom;
    return out;
}

// 整批转换（--monitor 的那几块屏矩形要穿过辅助进程管道的线上格式，一次转换一整份）。
inline std::vector<RECT> ToNativeRects(const std::vector<PhysRect>& rects) {
    std::vector<RECT> out;
    out.reserve(rects.size());
    for (const PhysRect& r : rects) out.push_back(ToNativeRect(r));
    return out;
}

inline PhysRect FromNativeRect(const RECT& r) {
    PhysRect out;
    out.left = static_cast<int32_t>(r.left);
    out.top = static_cast<int32_t>(r.top);
    out.right = static_cast<int32_t>(r.right);
    out.bottom = static_cast<int32_t>(r.bottom);
    return out;
}

inline std::vector<PhysRect> FromNativeRects(const std::vector<RECT>& rects) {
    std::vector<PhysRect> out;
    out.reserve(rects.size());
    for (const RECT& r : rects) out.push_back(FromNativeRect(r));
    return out;
}

// 把 Deadline 换算成等待毫秒数（未启用 = INFINITE）。
// 换算规则与它从 Deadline.h 搬过来之前逐字相同：超过 DWORD 范围的剩余时间仍然按"不限"交出去，
// 不截成一个小的数（那会让一次本来还剩两小时的等待变成立刻超时）。
inline DWORD WaitTimeout(const Deadline& dl) {
    if (!dl.Enabled()) return INFINITE;
    const uint64_t left = dl.RemainingMs();
    return left > 0xFFFFFFFFull ? INFINITE : static_cast<DWORD>(left);
}

}  // namespace ecapture
