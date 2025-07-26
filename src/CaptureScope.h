#pragma once
// 取帧路径的像素来源分类 —— 截图授权的唯一判据。
//
// 这里刻意不按"通道名字"决定风险，而是按"这条路径实际会不会从屏幕上取像素"：
//   * 窗口内容路径：帧只绑定在所选窗口自己的合成面 / 自绘结果上，屏幕上别的东西进不了图。
//   * 桌面路径：取的是显示器上此刻那一块区域的像素，最终裁成窗口大小也一样是桌面像素。
// 判不出来的一律按桌面路径处理（默认强制确认），所以新增通道时忘了登记 = 更严格而不是更松。
//
// 本文件只做纯判断，不碰窗口、不弹框、不取帧：这样它和 ConsentGate 都能在没有交互桌面的
// 测试进程里被逐条断言（见 tests\consent_state.cpp）。

#include <string>
#include <vector>

namespace ecapture {

enum class CaptureMethod;

// 一条实际执行到的内部路径的像素来源。
enum class PixelScope {
    kWindowContent,  // 只有所选窗口自己的画面
    kDesktop,        // 可能含其它窗口 / 桌面像素
};

// 内部路径的机器名（进 JSON 的 images[].path，取值只增不改名）。
// 一个通道可以有多条路径：dwm 的缩略图主路径不走屏幕，它的"把宿主窗口盖到目标位置上再拷
// 屏幕"退路走屏幕 —— 这两条必须能分开，否则一条 --yes 就把桌面取样也一起批掉了。
namespace paths {
inline constexpr const wchar_t* kWgc = L"wgc";                          // 窗口自己的 WGC 面
inline constexpr const wchar_t* kDwmThumbnail = L"dwm.thumbnail";        // DWM 缩略图 + 屏幕外 PrintWindow
inline constexpr const wchar_t* kDwmScreen = L"dwm.screen";              // DWM 内部退路：拷屏幕上那块矩形
inline constexpr const wchar_t* kPrintWindow = L"printwindow";          // PrintWindow 让窗口自绘到 DC
inline constexpr const wchar_t* kBitBltScreen = L"bitblt.screen";        // 从屏幕 DC 拷该窗口矩形
inline constexpr const wchar_t* kDuplicationFrame = L"duplication.frame";  // 整幅桌面帧 + 按矩形裁
inline constexpr const wchar_t* kScreenWgc = L"screen.wgc";              // 整块屏幕的 WGC 项目
inline constexpr const wchar_t* kScreenBitBlt = L"screen.bitblt";
inline constexpr const wchar_t* kScreenDuplication = L"screen.duplication";
inline constexpr const wchar_t* kUnknown = L"unknown";
}  // namespace paths

// scope 的机器名（进 JSON 的 images[].scope）。
inline constexpr const wchar_t* kScopeWindow = L"window";
inline constexpr const wchar_t* kScopeDesktop = L"desktop";

// 这条路径的像素从哪来。未登记的名字（含空串）返回 kDesktop。
PixelScope ScopeOf(const wchar_t* path);

// 同上，给 std::wstring 用。
PixelScope ScopeOf(const std::wstring& path);

// 这一条要不要弹框问人。
//   桌面路径：永远要 —— --yes、--quiet、环境变量、调用身份都不能跳过。
//   窗口内容路径：给了 --yes 就不问，没给就要问（"没有 --yes 时真实截图需要确认"）。
bool NeedsHumanConsent(const wchar_t* path, bool yesGiven);

// 窗口目标 / 屏幕目标上，某个通道实际走的那条路径名。
// 屏幕目标只有 wgc / duplication / bitblt 可用（dwm 与 printwindow 在解析期就被挡掉）。
const wchar_t* WindowPathOf(CaptureMethod method);
const wchar_t* ScreenPathOf(CaptureMethod method);

// scope -> JSON 里的那个取值。
const wchar_t* ScopeName(PixelScope scope);

// 登记表本体的一行（只读视图）。
struct PathScopeEntry {
    const wchar_t* path;
    PixelScope scope;
};

// 登记表整份交出去。能力查询（--capabilities / --diagnostics）要把"--yes 到底管哪几条"
// 说清楚，而那份事实只应该有一个出处，所以这里给的是下面那个数组本身而不是又抄一份表。
const std::vector<PathScopeEntry>& RegisteredCapturePaths();

}  // namespace ecapture
