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
//   7. "展示什么，就批准什么"：桌面那一级在弹框**之前**把目标清单、区域与屏幕拓扑冻结成
//      一份不可变快照，框上的文字与答"是"后冻结的授权都出自这同一份事实。人点头之后先重新
//      查询一遍拓扑再与这份快照比较 —— 确认期间布局变了（热插拔、改分辨率、挪位置）时，
//      那次"是"批的是人最后看到的旧布局，新布局不会被当成旧请求的已批准基线：这一问以
//      capture.consent_stale 收尾、一张都不截，之后的调用会拿新的事实重新问一次。
//
// 这不是安全边界：确认框是合作式自动化（人或 AI）的误操作防护，它分不清点击的是不是人，
// 也挡不住同一个权限级别里存心绕过的进程。能保证的是"照规矩跑的调用方一定会先问一次"。

#include <cstdint>
#include <functional>
#include <memory>
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

class Deadline;

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
    // 这一条是"期限到点、由本工具把框按'否'关掉"的标记：与"人自己点了否"同为拒绝，
    // 但调用方要能分清（--consent-timeout-ms 到点 vs 人不同意）。
    bool timedOut = false;
};

// 弹框这一件事的接口。测试里注入假的应答器，就能在没有桌面的情况下把整台状态机跑完。
class IConsentPrompt {
public:
    virtual ~IConsentPrompt() = default;
    virtual ConsentReply Ask(const ConsentQuestion& question) = 0;
};

// 真机上那一个：模态 MessageBox（MB_YESNO），默认焦点在"否"上。
//
// 确认框跑在一条专门的线程上，本线程只负责等它 —— 因为 MessageBoxW 自己是阻塞的，
// 而 --consent-timeout-ms 要求"到点没人答就按拒绝处理"，那就必须有一个能主动把框关掉、
// 并且**任何超时路径都能收尾**的观察者（见下面的 RunMonitoredDialog）。这条线程从不被
// TerminateThread 抛弃：宽限用尽仍关不掉时，等待方按"拒绝"返回，弹框线程与它引用的
// 一切由共享生命周期接管（进程退出时一起消失，其间绝不构成同意）。
// timeoutMs = 0 表示一直等人回答。
class DialogConsentPrompt final : public IConsentPrompt {
public:
    explicit DialogConsentPrompt(uint64_t timeoutMs = 0) : timeoutMs_(timeoutMs) {}

    ConsentReply Ask(const ConsentQuestion& question) override;

private:
    uint64_t timeoutMs_ = 0;
};

// ---------------------------------------------------------------------------
// 弹框生命周期机制
//
// "框怎么弹、怎么关"（驱动）与"等多久、到点关不关得掉、线程怎么收尾"（状态机）分开：
// 状态机才是 F05 的要害 —— 到点必须有一个有界、可信的结束路径，不管框是没弹出来、
// 关它的话没送到、还是送到了没生效。真机的驱动是 MessageBoxW（ShowConsentDialog）；
// 测试目标链的是**同一份实现**，只是把驱动换成自己的傀儡，判的是生产那台状态机本身。
// 这不是发布版后门：ECAPTURE.EXE 里注入点不接线、也不存在任何能选驱动的运行时入口，
// 傀儡驱动只编译进测试可执行文件。
// ---------------------------------------------------------------------------

// 驱动在弹框线程上跑完一次的结果。沿用 MessageBoxW 的约定：code 0 = 没弹成。
struct DialogResult {
    int code = 0;
    DWORD win32 = 0;      // code == 0 时这才是失败原值（在弹框那条线程上取的）
};

class DialogDriver {
public:
    DialogDriver() = default;
    virtual ~DialogDriver() = default;
    DialogDriver(const DialogDriver&) = delete;
    DialogDriver& operator=(const DialogDriver&) = delete;

    // 在专门的弹框线程上被调用：把框弹出来，阻塞到它关闭。
    // 状态机保证这条线程绝不被终止 —— 它可能在等待方放弃之后才返回，
    // 那时它引用的状态由它自己持有的那份共享所有权接管，不落空。
    virtual DialogResult Show() = 0;

    // 期限到点后在**等待线程**上被调用，宽限期内每个轮询切片重试一次：
    // 试着把框关掉。dialogThreadId 是弹框线程的 id（0 = 那条线程还没跑起来）。
    // 返回值只表示"关的指令送出去了没有"；生效与否一律由 done 事件 + 宽限期裁决。
    virtual bool RequestClose(DWORD dialogThreadId) = 0;
};

// 看守一次弹框直到拿到一个可信的结束（或放弃这条框）。规则：
//   * 只有弹框真的关闭、且驱动报回 IDYES、且期限还没到，才是 kAccepted；
//     同意之后固定留一段关闭动画缓冲（kDialogSettleMs，见 Consent.cpp），不为赶期限省略。
//   * 人答"否"、人自己关框 => kDeclined。
//   * 期限到点 => 先 latch timedOut 再裁决：宽限期内反复 RequestClose；到点仍没人答过，
//     不管是关不掉、还是"是"在同一瞬间被点下，一律 kDeclined + timedOut，绝不改成同意。
//     timeoutMs > 0 时本函数在 timeoutMs + 关框宽限 + 一个切片内必定返回。
//   * 框根本没弹出来、驱动抛异常、或事件/线程创建失败 => kUnavailable（带 win32 原值）。
//     弹框线程已经交出结果（code 0）之后，即使期限也到了，仍报 kUnavailable ——
//     "弹都没弹出去"比"没人回答"更具体，两者同为拒绝，隐私上等价。
ConsentReply RunMonitoredDialog(std::shared_ptr<DialogDriver> driver, uint64_t timeoutMs);

// 生产入口：用 MessageBoxW 弹一帧（flags 须含 MB_YESNO —— 到点关框的升级链要点那块
// "否"按钮，见 Consent.cpp 的 MessageBoxDriver），其余规则同上。
ConsentReply ShowConsentDialog(const std::wstring& body, const std::wstring& title, UINT flags,
                               uint64_t timeoutMs);

// 一次桌面取样的凭证。构造函数私有，唯一的发出者是 ConsentGate，而 GrabScreenRect、
// duplication、整屏 wgc 都把它做成必需参数 —— 内部退路想绕开授权就拿不到这张凭证，
// 编译期就过不去，而不是"记得调用一下检查"。
class DesktopPermit {
public:
    DesktopPermit(const DesktopPermit&) = delete;
    DesktopPermit& operator=(const DesktopPermit&) = delete;
    DesktopPermit(DesktopPermit&&) = default;
    DesktopPermit& operator=(DesktopPermit&&) = default;

    // 当时批准的是不是这一块区域：**严格包含，零容差**。向外扩一个像素去采人没批准过的
    // 画面就是扩大批准的隐私范围，所以没有"测量误差"这回事可以豁免它 ——
    // 而正常取样的矩形本来就量在批准矩形之内（见 Consent.cpp 里 Covers 的注释）。
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
// 这里**不存**拼好的整行文案 —— 文案由判定器在弹框那一刻从 (subject, area) 现渲染，
// 所以 SetTargetArea 刷新区域之后，框上写的坐标与实际要采样的矩形必然是同一份事实，
// 不存在"内部矩形更新了、给人看的那行还是旧坐标"这种平行文本过期的形状。
struct GateTarget {
    bool screen = false;
    std::wstring key;      // 窗口给 0x…，屏幕给设备名 —— 与诊断里的 target 同形
    std::wstring subject;  // 目标身份那一段文字（DescribeWindow / DescribeScreen），不含区域
    RECT area{};           // 该目标当前的屏幕矩形（窗口矩形 / 该屏矩形）
};

struct GateConfig {
    bool yes = false;                       // --yes
    std::vector<GateTarget> targets;        // 本次全部目标
    std::vector<std::wstring> outputs;      // 整批展开后的输出（"-" 用"标准输出"那行代替）
    std::wstring captureLabel;              // 弹框里那句"取图方式"（人话，随 --lang 变）
    // 人工确认最多等多久（--consent-timeout-ms）。0 = 一直等。
    // 这一段计时与 --timeout-ms 那份自动处理预算**分开**：等一个人不是在处理任务，
    // 把等待的时间算进自动预算会让"人离开了键盘"变成"截图失败"。
    uint64_t consentTimeoutMs = 0;
    // 本次运行的自动处理预算（可空）。非空时，判定器**只在真的要等人回答的弹框区间**
    // 暂停它（含同意后 DialogConsentPrompt 里的关闭动画缓冲）：--timeout-ms 因此只计算
    // 自动处理时间。--yes 直通、复用已给出的许可这些根本不弹框的分支不暂停、不补时间。
    // 指向的 Deadline 由建立它的流水线保活（判定器不持有所有权）。
    const Deadline* autoBudget = nullptr;
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
    // 弹框前把清单（目标、区域、拓扑、输出）冻结成快照：框上的文字与答"是"后签发的授权
    // 都出自这同一份；应答返回之后先重新查询拓扑与快照比较，确认期间布局变了就按
    // capture.consent_stale 收尾（不授权，也不把新布局当成批准过的基线）。
    bool AuthorizeDesktop(const wchar_t* path, const std::wstring& targetKey, const RECT& area,
                          std::optional<DesktopPermit>* out, Diagnostic* err);

    // 流水线在取帧前量到的目标矩形要能刷新到这里，好让弹框上写的是"现在就在那儿"的矩形。
    // 只影响还没弹出去的那一轮：一旦弹框，本轮问的就是那一刻的快照，之后刷新改不了它。
    // 已经批准的桌面授权用的是快照里冻结的区域，所以挪动过的窗口会被判成"范围已变"而重新问。
    void SetTargetArea(const std::wstring& targetKey, const RECT& area);

    // 已经有人在某一级确认上答过"否"（或根本弹不出框）：本次请求剩下的采集一律停止。
    bool Refused() const { return windowLevel_ == Level::kRefused || desktopLevel_ == Level::kRefused; }

    // 以下给测试看：弹了几次框、当前是哪一级。真实调用方不需要。
    int Asks() const { return windowAsks_ + desktopAsks_; }

    // 屏幕拓扑指纹（变化 = 已给出的桌面授权作废）
    uint64_t TopologyFingerprint() const;

private:
    enum class Level { kNotAsked, kGranted, kRefused };

    // 弹框前冻结的授权快照（"展示什么，就批准什么"的那一份事实）：
    // 框上的每一行、答"是"之后 desktopAreas_ 里冻结的每个矩形、以及拓扑基线，
    // 全部出自这同一次取值。之后的 SetTargetArea / 布局变化都改不了在世的那一轮。
    struct ConsentSnapshot {
        std::vector<GateTarget> listed;   // 清单与各自区域（弹框那一刻）
        std::vector<std::wstring> outputs;
        uint64_t topology = 0;            // 弹框那一刻的拓扑指纹
    };

    ConsentSnapshot TakeSnapshot() const;
    ConsentQuestion MakeQuestion(const wchar_t* path, PixelScope scope,
                                 const ConsentSnapshot& snap) const;
    Diagnostic Denied(const wchar_t* path, const std::wstring& targetKey, PixelScope scope,
                      bool unavailable, DWORD gle, bool timedOut) const;
    // 确认基线在弹框期间失效（拓扑变了）：这一问不授权，也不换基线重试
    Diagnostic Stale(const wchar_t* path, const std::wstring& targetKey) const;
    // 人同意、且确认后拓扑复核与快照一致之后：把快照里列出的目标区域冻结成本次桌面授权
    void GrantDesktop(const ConsentSnapshot& snap);
    bool DesktopGrantedFor(const std::wstring& targetKey, const RECT& area, RECT* approved) const;

    GateConfig config_;
    IConsentPrompt& prompt_;
    TopologyProvider topology_;
    uint64_t topologyAtAsk_ = 0;   // 弹框**之前**的拓扑基线（来自快照，不是应答之后首次所见）

    Level windowLevel_ = Level::kNotAsked;
    Level desktopLevel_ = Level::kNotAsked;
    // 拒绝的原因（人答否 / 弹不出框 / 期限到点）要粘住：后面那些目标不再问第二遍，
    // 但它们各自的诊断得说清当初是哪一种，而不是统统写成"被拒绝"。
    bool lastDenyUnavailable_ = false;
    bool lastDenyTimedOut_ = false;
    std::vector<GateTarget> desktopAreas_;  // 答"是"那一轮弹框前冻结的授权快照清单
    int windowAsks_ = 0;
    int desktopAsks_ = 0;
};

}  // namespace ecapture
