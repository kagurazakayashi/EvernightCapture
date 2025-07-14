#pragma once
// 截图授权：谁能截什么，以及"截之前要不要问人"。
//
// 分成两层，边界写清楚：
//   * ConsentScope（CaptureScope.h）判"这条路径的像素从哪来"，是纯函数。
//   * ConsentGate 是这次请求的授权状态机：它决定弹不弹框、弹几次、什么时候作废，
//     并且是桌面取样凭证（DesktopPermit）唯一的发出者。
//
// 产品规则（改代码前先对齐这里）：
//   1. 没有 --yes 时，任何真实截图都要先问人，可靠窗口内容路径也一样。
//   2. --yes 只能免掉"绑定所选窗口、不从桌面采样别的像素"那条路径的确认；它不代表目标一定有
//      画面，也不忽略权限、DRM、错误或覆盖保护。
//   3. 一切可能含其它窗口 / 桌面像素的路径必须真人确认，--yes、--quiet、环境变量、stdin、
//      调用身份都不能跳过 —— 整块屏幕的 WGC 也算这类。
//   4. 先确认了窗口内容不等于确认桌面内容：范围升级要重新确认。
//   5. 被拒绝、框关掉、或根本没有交互桌面 => 停止本次请求剩下的采集，不换后端也不重试；
//      之前已经完成的图保留。
//   6. 一次确认可以覆盖本次明确列出的那一批目标，但绝不跨请求缓存，也不扩大目标；
//      目标区域或屏幕拓扑一变，相应授权立即作废。
//
// 这不是安全边界：确认框是合作式自动化（人或 AI）的误操作防护，它分不清点击的是不是人，
// 也挡不住同一个权限级别里存心绕过的进程。能保证的是"照规矩跑的调用方一定会先问一次"。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureScope.h"
#include "CliOptions.h"
#include "ScreenMatch.h"

namespace ecapture {

// 弹框要问的那一轮。全部由授权判定器算出，UI 只是把它显示出来。
struct ConsentQuestion {
    PixelScope scope = PixelScope::kWindowContent;
    std::wstring path;                  // 实际要走的那条内部路径（dwm.screen / screen.wgc …）
    std::wstring captureLabel;          // --capture 的取值（人话那份，随 --lang 变）
    std::vector<std::wstring> targets;  // 给人看的目标行
    std::vector<std::wstring> outputs;  // 展开后的绝对输出，或"标准输出"
    bool yesGiven = false;              // 调用方给了 --yes：桌面路径要明说它不生效
};

enum class ConsentAnswer {
    kAccepted,     // 人点了"是"
    kDeclined,     // 人点了"否"或关掉对话框
    kUnavailable,  // 根本没有可交互的桌面，没人能回答"是"
};

struct ConsentReply {
    ConsentAnswer answer = ConsentAnswer::kUnavailable;
    // answer == kUnavailable 时这才是"为什么弹不出来"的 GetLastError 原值；其余为 0。
    DWORD win32 = 0;
};

// 弹框这一件事的接口。测试里注入假的应答器，就能在没有桌面的情况下把整台状态机跑完。
class IConsentPrompt {
public:
    virtual ~IConsentPrompt() = default;
    virtual ConsentReply Ask(const ConsentQuestion& question) = 0;
};

// 真机上那一个：模态 MessageBox，默认焦点在"否"上。
class DialogConsentPrompt final : public IConsentPrompt {
public:
    ConsentReply Ask(const ConsentQuestion& question) override;
};

// 一次桌面取样的凭证。构造函数私有，唯一的发出者是 ConsentGate，而 GrabScreenRect、
// duplication、整屏 wgc 都把它做成必需参数 —— 内部退路想绕开授权就拿不到这张凭证，
// 编译期就过不去，而不是"记得调用一下检查"。
class DesktopPermit {
public:
    DesktopPermit(const DesktopPermit&) = delete;
    DesktopPermit& operator=(const DesktopPermit&) = delete;
    DesktopPermit(DesktopPermit&&) = default;
    DesktopPermit& operator=(DesktopPermit&&) = default;

    // 当时批准的是不是这一块区域（要求实际取样矩形落在批准的矩形之内，长大或挪走都不算）
    bool Covers(const RECT& area) const;
    const std::wstring& TargetKey() const { return targetKey_; }
    RECT Approved() const { return approved_; }

private:
    friend class ConsentGate;
    DesktopPermit(std::wstring targetKey, RECT approved)
        : targetKey_(std::move(targetKey)), approved_(approved) {}

    static DesktopPermit Issue(std::wstring targetKey, RECT approved) {
        return DesktopPermit(std::move(targetKey), approved);
    }

    std::wstring targetKey_;
    RECT approved_{};
};

// 本次请求的目标清单里的一项：给判定器用来核对"要截的就是人看到的那些"。
struct GateTarget {
    bool screen = false;
    std::wstring key;          // 窗口给 0x…，屏幕给设备名 —— 与诊断里的 target 同形
    std::wstring description;  // 弹框与 hint 里的那一行
    RECT area{};               // 该目标当前的屏幕矩形（窗口矩形 / 该屏矩形）
};

struct GateConfig {
    bool yes = false;                       // --yes
    std::vector<GateTarget> targets;        // 本次全部目标
    std::vector<std::wstring> outputs;      // 整批展开后的输出（"-" 用"标准输出"那行代替）
    std::wstring captureLabel;              // 弹框里那句"取图方式"（人话，随 --lang 变）
};

// 屏幕拓扑的取值函数。测试注入假布局，真机用 EnumScreens。
using TopologyProvider = std::function<std::vector<ScreenInfo>()>;

// 一次进程内运行的授权状态机。不做线程共享（本工具单线程跑流水线）。
class ConsentGate {
public:
    explicit ConsentGate(GateConfig config, IConsentPrompt& prompt,
                         TopologyProvider topology = nullptr);

    // 窗口内容路径（wgc / printwindow / dwm 的缩略图主路径）。
    // 带 --yes 时直接放行；否则整批问一次，之后同批次不再重复打扰。
    bool AuthorizeWindow(const wchar_t* path, const std::wstring& targetKey, Diagnostic* err);

    // 桌面路径（bitblt、duplication、整屏任何通道、以及 DWM 内部的屏幕退路）。
    // 永远要人确认；批准了才把 *out 填好，调用方没有它就取不到屏幕像素。
    // area 是"这次实际要取样的那块屏幕矩形"，必须落在弹框上给人看过的那一片区域内。
    bool AuthorizeDesktop(const wchar_t* path, const std::wstring& targetKey, const RECT& area,
                          std::optional<DesktopPermit>* out, Diagnostic* err);

    // 流水线在取帧前量到的目标矩形要能刷新到这里，好让弹框上写的是"现在就在那儿"的矩形。
    // 这只影响给人看的那份清单；已经批准的桌面授权用的是答"是"那一刻的快照，
    // 所以挪动过的窗口会被判成"范围已变"而重新问一次。
    void SetTargetArea(const std::wstring& targetKey, const RECT& area);

    // 已经有人在某一级确认上答过"否"（或根本弹不出框）：本次请求剩下的采集一律停止。
    bool Refused() const { return windowLevel_ == Level::kRefused || desktopLevel_ == Level::kRefused; }

    // 以下给测试看：弹了几次框、当前是哪一级。真实调用方不需要。
    int Asks() const { return windowAsks_ + desktopAsks_; }

    // 屏幕拓扑指纹（变化 = 已给出的桌面授权作废）
    uint64_t TopologyFingerprint() const;

private:
    enum class Level { kNotAsked, kGranted, kRefused };

    ConsentQuestion MakeQuestion(const wchar_t* path, PixelScope scope,
                                 const std::vector<GateTarget>& listed) const;
    Diagnostic Denied(const wchar_t* path, const std::wstring& targetKey, PixelScope scope,
                      bool unavailable, DWORD gle) const;
    // 人同意之后：把当前列出的目标区域冻结成本次桌面授权
    void GrantDesktop(const std::vector<GateTarget>& listed);
    bool DesktopGrantedFor(const std::wstring& targetKey, const RECT& area, RECT* approved) const;

    GateConfig config_;
    IConsentPrompt& prompt_;
    TopologyProvider topology_;
    uint64_t topologyAtAsk_ = 0;

    Level windowLevel_ = Level::kNotAsked;
    Level desktopLevel_ = Level::kNotAsked;
    std::vector<GateTarget> desktopAreas_;  // 答"是"那一刻的目标快照（不随窗口移动更新）
    int windowAsks_ = 0;
    int desktopAsks_ = 0;
};

}  // namespace ecapture
