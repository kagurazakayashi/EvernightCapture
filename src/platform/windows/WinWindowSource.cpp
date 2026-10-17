// 【平台层 · Windows】窗口来源：EnumWindows 枚举、进程事实问答与几条几何包装。
//
// 从 src/WindowMatch.cpp（枚举）与 src/WindowIdentity.cpp（kCheap 四问）搬出来：
// 这两段都直接碰 user32 / kernel32 的原生句柄，共用层的匹配语义与身份判据不依赖它们。
// 共用声明在 src/WindowSource.h 与 src/WindowIdentity.h（WindowQueryLayer）。
//
// 本文件不取一个像素、不弹框、不移动任何窗口；枚举时给每扇窗口取标题（GetWindowText）
// 这一点与改造前逐字相同 —— 那一步会往目标线程发消息，需要期限的场景由调用方整步交给
// 辅助进程（Worker.h），本文件只提供"在本进程里跑"的那一份实现。

#include "WindowSource.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <psapi.h>

#include "CaptureCommon.h"   // WindowFullRect / WindowScreenRect / ClientScreenRect（原生 RECT 那一侧）
#include "WindowMatch.h"
#include "platform/windows/WinBridge.h"

namespace ecapture {
namespace {

std::wstring FileNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

// 「被挡下」与「问不出来」是两件事：前者意味着换一个更高权限的调用方就能读到，
// 后者可能是那个进程刚刚已经没了。结构化窗口查询要把这两种下场分开写，所以这里只认
// 系统明确说不许你读的那两个码，其余一律按「问过而失败」处理。
bool IsDeniedError(DWORD gle) {
    return gle == ERROR_ACCESS_DENIED || gle == ERROR_PRIVILEGE_NOT_HELD;
}

// 一条候选窗口的归属进程问答结果（由 ProcessFactsOf 填，落到 WindowInfo 的同名诸项）。
struct WindowFacts {
    std::wstring path;
    uint64_t startTicks = 0;
    ReadState pathRead = ReadState::kFailed;
    ReadState startRead = ReadState::kFailed;
    uint32_t pathWin32 = 0;
    uint32_t startWin32 = 0;
};

// 一次 OpenProcess 问两件事：映像路径与进程创建时间。分成两次开句柄没有意义，
// 而"当场再问一次创建时间"更不行 —— 身份复核要比的是**枚举那一刻**的值
//（见 WindowIdentity.h：中间那次销毁重建会被记成基线，复核就成了自己跟自己对答案）。
// 读不到的一律留空 / 0：那是"这一条判据没做出来"，不是"它相同"。
WindowFacts ProcessFactsOf(HWND hwnd) {
    WindowFacts f;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return f;   // 连归属进程都问不出来：两条问句都没答案
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        const DWORD gle = GetLastError();
        f.pathWin32 = gle;
        f.startWin32 = gle;
        const ReadState state = IsDeniedError(gle) ? ReadState::kDenied : ReadState::kFailed;
        f.pathRead = state;
        f.startRead = state;
        return f;
    }
    wchar_t buffer[8192];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (QueryFullProcessImageNameW(proc, 0, buffer, &size)) {
        f.path.assign(buffer, size);
        f.pathRead = ReadState::kReadable;
    } else {
        f.pathWin32 = GetLastError();
        f.pathRead = IsDeniedError(f.pathWin32) ? ReadState::kDenied : ReadState::kFailed;
    }
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    if (GetProcessTimes(proc, &creation, &exitTime, &kernel, &user)) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = creation.dwLowDateTime;
        ticks.HighPart = creation.dwHighDateTime;
        // QuadPart 为 0 是"没拿到"的写法，不能当成一个真值传给复核去比。
        if (ticks.QuadPart != 0) {
            f.startTicks = ticks.QuadPart;
            f.startRead = ReadState::kReadable;
        }
    } else {
        f.startWin32 = GetLastError();
        f.startRead = IsDeniedError(f.startWin32) ? ReadState::kDenied : ReadState::kFailed;
    }
    CloseHandle(proc);
    return f;
}

// EnumWindows 的回调只负责把窗口的**事实**问出来；可见性 / 尺寸 / 最小化 / 条件匹配
// 全部留给共用层 src/WindowMatch.cpp，于是"--list 列出来的"与"截图会选中的"用同一套语义。
struct CollectState {
    std::vector<WindowInfo>* out;
    int32_t order = 0;
};

BOOL CALLBACK CollectCandidate(HWND hwnd, LPARAM lParam) {
    auto* state = reinterpret_cast<CollectState*>(lParam);
    const int32_t thisOrder = state->order++;
    if (!IsWindow(hwnd)) return TRUE;

    WindowInfo w;
    w.hwnd = reinterpret_cast<uint64_t>(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    w.pid = static_cast<uint32_t>(pid);

    const int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen > 0) {
        w.title.resize(static_cast<size_t>(titleLen) + 1);
        const int got = GetWindowTextW(hwnd, w.title.data(), titleLen + 1);
        w.title.resize(got > 0 ? static_cast<size_t>(got) : 0);
    }
    wchar_t cls[256];
    const int clsLen = GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    if (clsLen > 0) w.className.assign(cls, static_cast<size_t>(clsLen));

    const WindowFacts facts = ProcessFactsOf(hwnd);
    w.imagePath = facts.path;
    w.processStartTicks = facts.startTicks;
    w.pathRead = facts.pathRead;
    w.startRead = facts.startRead;
    w.pathWin32 = facts.pathWin32;
    w.startWin32 = facts.startWin32;
    if (!w.imagePath.empty()) w.imageName = FileNameOf(w.imagePath);

    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        w.x = rect.left;
        w.y = rect.top;
        w.width = rect.right - rect.left;
        w.height = rect.bottom - rect.top;
        w.rectRead = ReadState::kReadable;
    } else {
        // 量不出来与「量到了一个零尺寸的窗口」是两件事，两者都不能写成同一个值：
        // 前者是这一次问答没有答案，后者是窗口自己的形状。查询层据此写字段级 unknown。
        w.rectWin32 = GetLastError();
        w.rectRead = ReadState::kFailed;
    }
    w.zOrder = thisOrder;
    w.iconic = IsIconic(hwnd) != FALSE;

    // 旧行为：不可见与零尺寸的顶层窗口根本不进候选；最小化的仍进（供 --list=all 与 hint）。
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (w.width <= 0 || w.height <= 0) return TRUE;

    state->out->push_back(std::move(w));
    return TRUE;
}

// ----- 身份复核的 kCheap 四问（原 SystemWindowQueryLayer 的 Win32 实现） -----

bool WindowIsAlive(uint64_t hwnd) {
    return IsWindow(reinterpret_cast<HWND>(hwnd)) != FALSE;
}

bool ReadWindowPid(uint64_t hwnd, uint32_t* pid) {
    if (!pid) return false;
    DWORD own = 0;
    if (GetWindowThreadProcessId(reinterpret_cast<HWND>(hwnd), &own) == 0) return false;
    if (own == 0) return false;
    *pid = static_cast<uint32_t>(own);
    return true;
}

bool ReadWindowClass(uint64_t hwnd, std::wstring* out) {
    if (!out) return false;
    wchar_t buffer[256];
    const int len = GetClassNameW(reinterpret_cast<HWND>(hwnd), buffer,
                                  static_cast<int>(std::size(buffer)));
    if (len <= 0) return false;
    out->assign(buffer, static_cast<size_t>(len));
    return true;
}

// 进程创建时间。PROCESS_QUERY_LIMITED_INFORMATION 就够（这条句柄枚举那一步本来也要开一次），
// 拿不到就是拿不到：调用方按"这一条判据没做出来"处理，而不是当成失败。
bool ReadProcessStartTicks(uint32_t pid, uint64_t* out) {
    if (!out) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    FILETIME creation{}, quitting{}, kernel{}, user{};
    const bool ok = GetProcessTimes(proc, &creation, &quitting, &kernel, &user) != FALSE;
    CloseHandle(proc);
    if (!ok) return false;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = creation.dwLowDateTime;
    ticks.HighPart = creation.dwHighDateTime;
    if (ticks.QuadPart == 0) return false;   // 0 是"没读到"的写法，不能当成一个真值
    *out = ticks.QuadPart;
    return true;
}

}  // namespace

bool EnumerateWindowCandidates(std::vector<WindowInfo>* out, BlockedStatus* status,
                               std::string* detail) {
    if (!out) return false;
    out->clear();
    CollectState state{out, 0};
    try {
        if (!EnumWindows(CollectCandidate, reinterpret_cast<LPARAM>(&state))) {
            if (status) *status = BlockedStatus::kInternal;
            if (detail) *detail = "EnumWindows failed";
            out->clear();
            return false;
        }
    } catch (...) {
        // 回调里不抛（取标题那一路的失败都折成字段级 read 状态），这里只是兜底：
        // 半套候选看起来像"就这些窗口"，所以整份作废。
        if (status) *status = BlockedStatus::kInternal;
        if (detail) *detail = "unknown";
        out->clear();
        return false;
    }
    // EnumWindows 的访问顺序就是当下的 Z 序；保留该顺序，不再另排。
    if (status) *status = BlockedStatus::kOk;
    return true;
}

WindowQueryLayer SystemWindowQueryLayer() {
    WindowQueryLayer q;
    q.alive = &WindowIsAlive;
    q.windowPid = &ReadWindowPid;
    q.windowClass = &ReadWindowClass;
    q.processStart = &ReadProcessStartTicks;
    return q;
}

PhysRect WindowFullRectOf(uint64_t hwnd) {
    return FromNativeRect(WindowFullRect(reinterpret_cast<HWND>(hwnd)));
}

PhysRect WindowScreenRectOf(uint64_t hwnd) {
    return FromNativeRect(WindowScreenRect(reinterpret_cast<HWND>(hwnd)));
}

bool ClientScreenRectOf(uint64_t hwnd, PhysRect* out) {
    if (!out) return false;
    RECT native{};
    if (!ClientScreenRect(reinterpret_cast<HWND>(hwnd), &native)) return false;
    *out = FromNativeRect(native);
    return true;
}

}  // namespace ecapture
