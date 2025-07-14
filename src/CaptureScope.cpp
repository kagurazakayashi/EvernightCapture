#include "CaptureScope.h"

#include <cwchar>

#include "CliOptions.h"

namespace ecapture {
namespace {

// 登记表：路径名 -> 像素来源。这里就是"哪种截图要真人确认"的唯一一份事实，
// 加新通道时必须在这里登记一条；漏登记不会变松，因为 ScopeOf 对未知名字给 kDesktop。
struct PathEntry {
    const wchar_t* path;
    PixelScope scope;
};

constexpr PathEntry kTable[] = {
    {paths::kWgc, PixelScope::kWindowContent},
    {paths::kPrintWindow, PixelScope::kWindowContent},
    {paths::kDwmThumbnail, PixelScope::kWindowContent},
    // 下面这几条都会读到屏幕上那一块区域的像素：bitblt 与 duplication 即使最终裁成窗口
    // 大小，取的仍然是"显示器上此刻的样子"；dwm.screen 是 DWM 通道内部的屏幕退路。
    {paths::kBitBltScreen, PixelScope::kDesktop},
    {paths::kDuplicationFrame, PixelScope::kDesktop},
    {paths::kDwmScreen, PixelScope::kDesktop},
    {paths::kScreenWgc, PixelScope::kDesktop},
    {paths::kScreenBitBlt, PixelScope::kDesktop},
    {paths::kScreenDuplication, PixelScope::kDesktop},
    {paths::kUnknown, PixelScope::kDesktop},
};

}  // namespace

PixelScope ScopeOf(const wchar_t* path) {
    if (!path || !*path) return PixelScope::kDesktop;  // 说不清来路 = 按最危险的处理
    for (const auto& e : kTable) {
        if (std::wcscmp(e.path, path) == 0) return e.scope;
    }
    return PixelScope::kDesktop;
}

PixelScope ScopeOf(const std::wstring& path) { return ScopeOf(path.c_str()); }

bool NeedsHumanConsent(const wchar_t* path, bool yesGiven) {
    if (ScopeOf(path) == PixelScope::kDesktop) return true;
    return !yesGiven;
}

const wchar_t* WindowPathOf(CaptureMethod method) {
    switch (method) {
        case CaptureMethod::kWgc: return paths::kWgc;
        case CaptureMethod::kPrintWindow: return paths::kPrintWindow;
        // 这里给的是 dwm 的缩略图主路径。它内部那条"盖到目标位置上拷屏幕"的退路自己会回到
        // 授权判定器重新要许可（那条路径名是 dwm.screen），所以不能在这里预先替它要。
        case CaptureMethod::kDwmThumbnail: return paths::kDwmThumbnail;
        case CaptureMethod::kBitBlt: return paths::kBitBltScreen;
        case CaptureMethod::kDuplication: return paths::kDuplicationFrame;
        case CaptureMethod::kAuto: break;  // auto 由回退链逐条展开，每条各有自己的路径名
    }
    return paths::kUnknown;
}

const wchar_t* ScreenPathOf(CaptureMethod method) {
    switch (method) {
        case CaptureMethod::kWgc: return paths::kScreenWgc;
        case CaptureMethod::kBitBlt: return paths::kScreenBitBlt;
        case CaptureMethod::kDuplication: return paths::kScreenDuplication;
        case CaptureMethod::kAuto: break;
        default: break;  // dwm / printwindow 在屏幕目标上没有可用路径，解析期已挡掉
    }
    return paths::kUnknown;
}

const wchar_t* ScopeName(PixelScope scope) {
    return scope == PixelScope::kWindowContent ? kScopeWindow : kScopeDesktop;
}

}  // namespace ecapture
