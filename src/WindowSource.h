#pragma once
// 【平台接口】窗口来源：枚举候选、几何问答与"物理像素坐标"那条前置设置。
//
// 这一份只声明平台无关的形状（WindowInfo / PhysRect / BlockedStatus / Identity 查询层），
// 实现在 src/platform/windows/WinWindowSource.cpp（Linux 后端将来在自己的目录 provide 同一组
// 函数）。共用层（src/WindowMatch.cpp、src/WindowQueryRun.cpp、src/Capture.cpp）只 include
// 这一份，不直接 include windows.h，也不再自己拼 EnumWindows / GetWindowRect 那几条调用。
//
// 三条边界（与 WindowMatch.h、WindowIdentity.h、CaptureCommon.h 的分工同源）：
//   1. **只读。** 枚举与几何问答不取一个像素、不弹框、不激活 / 恢复 / 移动任何窗口。
//   2. **问不出来就说问不出来。** 几何问答的返回值与"量到一块空矩形"是两件事：
//      WindowFullRectOf / WindowScreenRectOf 量不出来时交回空矩形（那是既有行为：调用方按
//      "这一问没有答案"处理，而不是当成 0 尺寸的目标）；ClientScreenRectOf 用 bool 把
//      "问不出来"与"客户区此刻真的是空的"分开。
//   3. **不往目标线程发消息。** 枚举时取标题（GetWindowText）本来就是会发的（有期限时那一步
//      走辅助进程，见 Worker.h）；本文件的几何问答只用 user32 / kernel32 自己那份结构。
//
// 【过渡桥接】枚举候选里仍带着归属进程的映像路径与创建时间，那是 WindowInfo 的既有字段
// （结构化窗口查询要按字段级 readability 报出它们），本步不改形状；身份复核的 kCheap 四问
// 另在 WindowIdentity.h 的 WindowQueryLayer 上，不与这里混成一份。

#include <cstdint>
#include <string>
#include <vector>

#include "core/Geometry.h"   // PhysRect
#include "core/Status.h"     // BlockedStatus
#include "WindowMatch.h"     // WindowInfo / ReadState
#include "WindowIdentity.h"  // WindowQueryLayer（真机 kCheap 那四问的实现从这一层接出去）

namespace ecapture {

// 枚举全部顶层窗口，按 EnumWindows 的访问顺序（0 = 最前）交回候选。
//
// 交回的是**枚举那一刻**的原始候选：可见性、尺寸、最小化都是候选自己的事实（WindowInfo 里
// 已有 iconic 与矩形），条件求值的过滤与 AND/OR 判定仍留在共用层 src/WindowMatch.cpp ——
// 这样"列出来的窗口"与"截图会选中的窗口"用的是同一份匹配语义，平台层只负责把候选问出来。
//
// 失败时 status / detail 填原因（只放 ASCII 细节，文案由调用方按 --lang 现取）；成功后
// out 里按 zOrder 升序排列。这里不收集最小化窗口的额外信息：它们就是候选里的 iconic 那批。
bool EnumerateWindowCandidates(std::vector<WindowInfo>* out, BlockedStatus* status,
                               std::string* detail);

// 窗口完整矩形（含 DWM 那圈透明边框），虚拟屏幕坐标的物理像素。量不出来时交回空矩形。
PhysRect WindowFullRectOf(uint64_t hwnd);

// 用户实际看到的矩形：优先 DWMWA_EXTENDED_FRAME_BOUNDS，问不到时退回完整矩形。
// 量不出来时交回空矩形（与 WindowFullRectOf 同一条规矩）。
PhysRect WindowScreenRectOf(uint64_t hwnd);

// 客户区在**虚拟屏幕坐标**里的那一块（左上含、右下不含）。
// 返回 false = 这一问没有答案（句柄已失效 / 两条 API 有一条失败 / 加出来的坐标不在 int32 里）；
// 返回 true 而矩形的宽或高为 0 = 这扇窗口此刻确实还没有客户区（是一条事实，不是失败）。
bool ClientScreenRectOf(uint64_t hwnd, PhysRect* out);

// 让本进程按物理像素与虚拟屏幕坐标工作（Windows：per-monitor DPI v2）。
// 窗口矩形、屏幕矩形与 GDI 取图都靠这一条对齐；不调用时被 DPI 虚拟化的进程会截偏或截半个窗口。
// 这条设置是幂等的，重复调用无副作用。
void EnsureDpiAware();

}  // namespace ecapture
