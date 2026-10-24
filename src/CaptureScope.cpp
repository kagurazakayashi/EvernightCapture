#include "CaptureScope.h"

#include <cwchar>

#include "CliOptions.h"

namespace ecapture {
namespace {

// 登记表：路径名 -> 像素来源。这里就是"哪种截图要真人确认"的唯一一份事实，
// 加新通道时必须在这里登记一条；漏登记不会变松，因为 ScopeOf 对未知名字给 kDesktop。
// 表体本身通过 RegisteredCapturePaths() 整份对外（能力查询要照着它列 --yes 的适用范围）。
constexpr PathScopeEntry kTable[] = {
    {paths::kWgc, PixelScope::kWindowContent},
    {paths::kPrintWindow, PixelScope::kWindowContent},
    {paths::kDwmThumbnail, PixelScope::kWindowContent},
    // XComposite 读的是目标窗口（及其子窗口）各自的存储，屏幕上别的东西进不了图。
    {paths::kXComposite, PixelScope::kWindowContent},
    // X11 根窗口读取桌面/屏幕/明确区域像素：即使最终只交付一小块，也是桌面像素，
    // --yes 对它一律不生效。
    {paths::kX11Desktop, PixelScope::kDesktop},
    // 下面这几条都会读到屏幕上那一块区域的像素：bitblt 与 duplication 即使最终裁成窗口
    // 大小，取的仍然是"显示器上此刻的样子"；dwm.screen 是 DWM 通道内部的屏幕退路。
    {paths::kBitBltScreen, PixelScope::kDesktop},
    {paths::kDuplicationFrame, PixelScope::kDesktop},
    {paths::kDwmScreen, PixelScope::kDesktop},
    {paths::kScreenWgc, PixelScope::kDesktop},
    {paths::kScreenBitBlt, PixelScope::kDesktop},
    {paths::kScreenDuplication, PixelScope::kDesktop},
    // XDG Desktop Portal + PipeWire：窗口请求只接受单一的 window 来源，屏幕请求只接受单一的
    // monitor 来源。前者不保证是“目标窗口自身”以外的桌面像素；两者都由 Portal 系统授权
    // 先行批准。窗口路径仍是窗口内容，monitor 路径仍是桌面像素。
    {paths::kPortalWindow, PixelScope::kWindowContent},
    {paths::kPortalMonitor, PixelScope::kDesktop},
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

const std::vector<PathScopeEntry>& RegisteredCapturePaths() {
    static const std::vector<PathScopeEntry> view(kTable, kTable + (sizeof(kTable) / sizeof(kTable[0])));
    return view;
}

PixelScope ScopeOf(const std::wstring& path) { return ScopeOf(path.c_str()); }

bool NeedsHumanConsent(const wchar_t* path, bool yesGiven) {
    // "给了 --yes 就免掉程序确认"这一条判据只在 core/Authorization.h 写一次
    //（YesGivenExempts：只有绑定窗口自己的画面那一档能免）。这里负责的是另一半事实：
    // 这条路径的像素到底从哪来。
    return !YesGivenExempts(ScopeOf(path), yesGiven);
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
        case CaptureMethod::kXComposite: return paths::kXComposite;
        case CaptureMethod::kX11Desktop: return paths::kUnknown;  // 没有窗口目标路径
        case CaptureMethod::kAuto: break;  // auto 由回退链逐条展开，每条各有自己的路径名
    }
    return paths::kUnknown;
}

const wchar_t* ScreenPathOf(CaptureMethod method) {
    switch (method) {
        case CaptureMethod::kWgc: return paths::kScreenWgc;
        case CaptureMethod::kBitBlt: return paths::kScreenBitBlt;
        case CaptureMethod::kDuplication: return paths::kScreenDuplication;
        // X11 根窗口后端只认屏幕/桌面/区域目标；不会拿窗口内容冒充桌面。
        case CaptureMethod::kX11Desktop: return paths::kX11Desktop;
        case CaptureMethod::kXComposite: break;
        case CaptureMethod::kAuto: break;
        default: break;  // dwm / printwindow 在屏幕目标上没有可用路径，解析期已挡掉
    }
    return paths::kUnknown;
}

const wchar_t* ScopeName(PixelScope scope) {
    return scope == PixelScope::kWindowContent ? kScopeWindow : kScopeDesktop;
}

}  // namespace ecapture
