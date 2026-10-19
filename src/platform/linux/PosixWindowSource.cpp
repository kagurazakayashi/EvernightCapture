// 【平台层 · Linux】窗口来源：X11 顶层客户端枚举、进程事实问答与几条几何包装。
//
// 与 Windows 那一份（WinWindowSource.cpp）提供同一组入口，共用层（WindowMatch /
// WindowIdentity / Capture）不需要知道下面是 Xlib。四条边界照旧：
//   * 只读：不取像素、不弹框、不激活 / 恢复 / 移动任何窗口。
//   * 问不出来就说问不出来：矩形那一问失败时 ReadState 是 kFailed，不是"零尺寸窗口"。
//   * 不往目标线程发消息：X11 的属性读取由服务器回答，目标进程不参与。
//   * 枚举的是**客户端**窗口，不是窗口管理器的装饰框；z 序 0 = 最前。
//
// 这一份没有"辅助进程"实现：条件求值的期限在 PosixIsolation.cpp 里按步检查（见那里的边界）。

#include "WindowSource.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "platform/linux/X11Capture.h"

namespace ecapture {
namespace {

PhysRect ToPhysRect(const x11::ClientWindowFacts& facts) {
    if (facts.rectRead != ReadState::kReadable) return PhysRect{};
    const int64_t right = static_cast<int64_t>(facts.x) + facts.width;
    const int64_t bottom = static_cast<int64_t>(facts.y) + facts.height;
    PhysRect rect;
    rect.left = facts.x;
    rect.top = facts.y;
    rect.right = static_cast<int32_t>(std::max<int64_t>(facts.x, right));
    rect.bottom = static_cast<int32_t>(std::max<int64_t>(facts.y, bottom));
    return rect;
}

void ToWindowInfo(const x11::ClientWindowFacts& facts, WindowInfo* out) {
    out->hwnd = static_cast<uint64_t>(facts.window);
    out->pid = facts.pid;
    out->processStartTicks = facts.processStartTicks;
    out->pathRead = facts.pathRead;
    out->startRead = facts.startRead;
    out->pathWin32 = facts.pathErrno;
    out->startWin32 = facts.startErrno;
    out->title = facts.title;
    out->className = facts.className;
    out->imageName = facts.imageName;
    out->imagePath = facts.imagePath;
    out->x = facts.x;
    out->y = facts.y;
    out->width = static_cast<int32_t>(std::min<uint32_t>(facts.width, 0x7fffffffu));
    out->height = static_cast<int32_t>(std::min<uint32_t>(facts.height, 0x7fffffffu));
    out->rectRead = facts.rectRead;
    out->rectWin32 = facts.rectErrno;
    out->zOrder = facts.zOrder;
    out->iconic = facts.iconic;
    out->visible = facts.viewable;
}

}  // namespace

bool EnumerateWindowCandidates(std::vector<WindowInfo>* out, BlockedStatus* status,
                               std::string* detail) {
    if (!out) return false;
    out->clear();
    x11::DisplayConnection connection;
    if (!connection.ok()) {
        if (status) *status = BlockedStatus::kInternal;
        if (detail) *detail = "XOpenDisplay: errno=" + std::to_string(connection.error());
        return false;
    }
    try {
        const std::vector<x11::ClientWindowFacts> facts =
            x11::EnumerateClientWindows(connection.get());
        out->reserve(facts.size());
        for (const auto& item : facts) {
            WindowInfo info;
            ToWindowInfo(item, &info);
            out->push_back(std::move(info));
        }
    } catch (...) {
        // 半套候选看起来像"就这些窗口"，所以整份作废。
        if (status) *status = BlockedStatus::kInternal;
        if (detail) *detail = "unknown";
        out->clear();
        return false;
    }
    std::sort(out->begin(), out->end(),
              [](const WindowInfo& a, const WindowInfo& b) { return a.zOrder < b.zOrder; });
    if (status) *status = BlockedStatus::kOk;
    return true;
}

WindowQueryLayer SystemWindowQueryLayer() {
    WindowQueryLayer layer;
    layer.alive = [](uint64_t hwnd) {
        x11::DisplayConnection connection;
        if (!connection.ok()) return false;
        x11::ClientWindowFacts facts;
        return x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts);
    };
    layer.windowPid = [](uint64_t hwnd, uint32_t* pid) {
        if (!pid) return false;
        x11::DisplayConnection connection;
        if (!connection.ok()) return false;
        x11::ClientWindowFacts facts;
        if (!x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts)) return false;
        if (facts.pidRead != ReadState::kReadable) return false;
        *pid = facts.pid;
        return true;
    };
    layer.windowClass = [](uint64_t hwnd, std::wstring* out) {
        if (!out) return false;
        x11::DisplayConnection connection;
        if (!connection.ok()) return false;
        x11::ClientWindowFacts facts;
        if (!x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts)) return false;
        if (facts.classRead != ReadState::kReadable) return false;
        *out = facts.className;
        return true;
    };
    layer.processStart = [](uint32_t pid, uint64_t* ticks) {
        if (!ticks) return false;
        uint32_t error = 0;
        // 与枚举时 _NET_WM_PID 那条问答共用同一份 /proc 解析与单位换算。
        return x11::ReadProcessStartTicksForPid(pid, ticks, &error);
    };
    // selectionStillMatches 由 Capture.cpp 补上（它要条件与期限）。
    return layer;
}

PhysRect WindowFullRectOf(uint64_t hwnd) {
    x11::DisplayConnection connection;
    if (!connection.ok()) return PhysRect{};
    x11::ClientWindowFacts facts;
    if (!x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts)) return PhysRect{};
    return ToPhysRect(facts);
}

PhysRect WindowScreenRectOf(uint64_t hwnd) {
    // X11 没有 DWM 那圈透明边框：完整矩形就是用户实际看到的那一块。
    return WindowFullRectOf(hwnd);
}

bool ClientScreenRectOf(uint64_t hwnd, PhysRect* out) {
    if (!out) return false;
    x11::DisplayConnection connection;
    if (!connection.ok()) return false;
    x11::ClientWindowFacts facts;
    if (!x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts)) return false;
    if (facts.rectRead != ReadState::kReadable) return false;
    *out = ToPhysRect(facts);
    return true;
}

void EnsureDpiAware() {
    // X11 的坐标本来就是设备像素：没有 Windows 那套 DPI 虚拟化，也不需要声明感知级别。
    // 原生 Wayland 的分数缩放在 XWayland 里由 X 服务器换算成设备像素后交回。
}

}  // namespace ecapture
