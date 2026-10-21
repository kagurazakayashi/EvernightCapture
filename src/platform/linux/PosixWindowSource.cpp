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

PhysRect FramePhysRectOf(const x11::ClientWindowFacts& facts) {
    if (facts.frameRectRead != ReadState::kReadable) {
        // 外框量不出来时不编一块屏幕矩形：调用方会把它当成空矩形（这一问没有答案）。
        // 需要客户区的路径必须显式走 ClientScreenRectOf。
        return PhysRect{};
    }
    const int64_t right = static_cast<int64_t>(facts.frameX) + facts.frameWidth;
    const int64_t bottom = static_cast<int64_t>(facts.frameY) + facts.frameHeight;
    PhysRect rect;
    rect.left = facts.frameX;
    rect.top = facts.frameY;
    rect.right = static_cast<int32_t>(std::max<int64_t>(facts.frameX, right));
    rect.bottom = static_cast<int32_t>(std::max<int64_t>(facts.frameY, bottom));
    return rect;
}

void ToWindowInfo(const x11::ClientWindowFacts& facts, WindowInfo* out) {
    out->hwnd = static_cast<uint64_t>(facts.window);   // 中性承载：这一格在 X11 上是 XID
    out->pid = facts.pid;
    out->pidRead = facts.pidRead;
    out->processStartTicks = facts.processStartTicks;
    out->pathRead = facts.pathRead;
    out->startRead = facts.startRead;
    out->pathWin32 = facts.pathErrno;
    out->startWin32 = facts.startErrno;
    out->processAttribution = facts.processAttribution;
    out->title = facts.title;
    out->titleRead = facts.titleRead;
    out->className = facts.className;
    out->classRead = facts.classRead;
    out->imageName = facts.imageName;
    out->imagePath = facts.imagePath;
    out->x = facts.x;
    out->y = facts.y;
    out->width = static_cast<int32_t>(std::min<uint32_t>(facts.width, 0x7fffffffu));
    out->height = static_cast<int32_t>(std::min<uint32_t>(facts.height, 0x7fffffffu));
    out->borderWidth = facts.borderWidth;
    out->rectRead = facts.rectRead;
    out->rectWin32 = facts.rectErrno;
    out->frameRectRead = facts.frameRectRead;
    out->frameRectWin32 = facts.frameRectErrno;
    out->frameX = facts.frameX;
    out->frameY = facts.frameY;
    out->frameWidth = static_cast<int32_t>(std::min<uint32_t>(facts.frameWidth, 0x7fffffffu));
    out->frameHeight = static_cast<int32_t>(std::min<uint32_t>(facts.frameHeight, 0x7fffffffu));
    out->frameRectSource = facts.frameRectSource;
    out->zOrder = facts.zOrder;
    out->iconic = facts.iconic;
    out->visible = facts.viewable;

    // XID 与显示连接作用域是一个整体：只把整数带下去，换一台 X 服务器就可能指到
    // 另一个对象。X11 的 scopeRequired 为真，复核时缺少会话基线就 fail-closed。
    out->native.backend = facts.session.backend.empty() ? L"x11" : facts.session.backend;
    out->native.idForm = L"xid";
    out->native.sessionRead = facts.session.read;
    out->native.sessionKnown = facts.session.known;
    out->native.sessionScope = facts.session.scope;
    out->native.displayName = facts.session.displayName;
    out->native.serverVendor = facts.session.serverVendor;
    out->native.serverRelease = facts.session.serverRelease;
    out->native.protocolMajor = facts.session.protocolMajor;
    out->native.protocolMinor = facts.session.protocolMinor;
    out->native.rootWindow = facts.session.rootWindow;
    out->native.xwayland = facts.session.xwayland;
    out->native.scopeRequired = true;
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
    // 复核时重新问一次当前显示连接作用域。每次新开连接并读同一组服务器事实，
    // 不依赖进程启动时的全局 Display*；跨 DISPLAY / XWayland 会话的 XID 不可比。
    layer.nativeIdentity = [](uint64_t hwnd, WindowNativeIdentity* out) {
        if (!out) return false;
        x11::DisplayConnection connection;
        if (!connection.ok()) return false;
        x11::ClientWindowFacts facts;
        if (!x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts)) {
            return false;
        }
        out->backend = facts.session.backend.empty() ? L"x11" : facts.session.backend;
        out->idForm = L"xid";
        out->sessionRead = facts.session.read;
        out->sessionKnown = facts.session.known;
        out->sessionScope = facts.session.scope;
        out->displayName = facts.session.displayName;
        out->serverVendor = facts.session.serverVendor;
        out->serverRelease = facts.session.serverRelease;
        out->protocolMajor = facts.session.protocolMajor;
        out->protocolMinor = facts.session.protocolMinor;
        out->rootWindow = facts.session.rootWindow;
        out->xwayland = facts.session.xwayland;
        out->scopeRequired = true;
        return facts.session.known;
    };
    // selectionStillMatches 由 Capture.cpp 补上（它要条件与期限）。
    return layer;
}

PhysRect WindowFullRectOf(uint64_t hwnd) {
    // 授权 / 取帧前置的范围用**外框**：WM 装饰也是用户以为被批准的那一块。
    // 外框读不到时交回空矩形，绝不拿客户区冒充外框。
    x11::DisplayConnection connection;
    if (!connection.ok()) return PhysRect{};
    x11::ClientWindowFacts facts;
    if (!x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts)) {
        return PhysRect{};
    }
    return FramePhysRectOf(facts);
}

PhysRect WindowScreenRectOf(uint64_t hwnd) {
    // 这一问要的是"交付图像对应屏幕上的哪一块"。XComposite 交的是客户区自身存储，
    // 所以这里必须是客户区矩形，不能换成 WM 装饰外框；外框只用于授权范围与诊断。
    // 量不到时交空矩形，不制造一个虚假的屏幕矩形。
    x11::DisplayConnection connection;
    if (!connection.ok()) return PhysRect{};
    x11::ClientWindowFacts facts;
    if (!x11::QueryClientWindow(connection.get(), static_cast<Window>(hwnd), &facts)) {
        return PhysRect{};
    }
    return ToPhysRect(facts);
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
