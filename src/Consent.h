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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "CaptureScope.h"
#include "CliOptions.h"
#include "ScreenMatch.h"
#include "core/Error.h"     // NativeErrorCode：这一格放 GetLastError 的原值
#include "core/Geometry.h"   // PhysRect：批准的那一块屏幕区域

namespace ecapture {

class Deadline;

// 弹框要问的那一轮。全部由授权判定器算出，UI 只是把它显示出来。
// 授权层与像素范围的语义在 core/Authorization.h（PixelScope、--yes 那条例外、
// 各平台要求哪几层授权）。本文件是**这次请求的授权状态机**与它问人的那份接口。
//
// 【GUI 归属】IConsentPrompt 是程序自有界面的接缝。生产实现 DialogConsentPrompt 在
// src/ConsentPrompt.cpp 里把这一问渲染成 wxWidgets 对话框（Windows 与 Linux 同一份代码，
// 见 src/gui/）；ConsentGate 与 CaptureScope 那两层判据不认识任何控件。Wayland 的系统授权
// 是**另一层**（操作系统自己的来源选择界面），本接缝既不实现它也不替代它。
struct ConsentQuestion {
    PixelScope scope = PixelScope::kWindowContent;
    std::wstring path;                  // 实际要走的那条内部路径（dwm.screen / screen.wgc …）
    std::wstring captureLabel;          // --capture 的取值（人话那份，随 --lang 变）
    std::vector<std::wstring> targets;  // 给人看的目标行
    std::vector<std::wstring> outputs;  // 展开后的绝对输出，或"标准输出"
    bool yesGiven = false;              // 调用方给了 --yes：桌面路径要明说它不生效
    // 这一次会不会在程序目录下另存一份历史副本（归档根解析得通时为真）。副本这件事要写在人
    // 批准的那段文字里：不能批了一张主输出、事后才发现别处还留着一份持久截图。
    bool historyCopy = false;
};

// 一次问人的结局。六档各说一件事，谁也不冒充谁：
//   * 只有 kAccepted 是同意；窗口消失、关框、超时、取消、初始化失败全部不是。
//   * kDeclined / kClosed / kTimedOut / kCancelled 在诊断上都归到"没有取得同意"，
//     但状态机与判据能分清人到底做了什么（下一步该换条件、再来一次还是换会话）。
enum class ConsentAnswer {
    kAccepted,    // 人点了"是"
    kDeclined,    // 人点了"否"
    kClosed,      // 人按 X 或 Esc 关掉了对话框
    kTimedOut,    // --consent-timeout-ms 到点，由程序收尾
    kCancelled,   // 程序主动取消（取消令牌），不是人的回答
    kInitFailed,  // GUI 初始化失败 / 对话框根本弹不出来
};

struct ConsentReply {
    ConsentAnswer answer = ConsentAnswer::kInitFailed;
    // answer == kInitFailed 时这才是"为什么弹不出来"的系统原值
    // （Windows 是 GetLastError，Linux 是 errno）；其余为 0。
    NativeErrorCode nativeError = kNativeErrorNone;
};

// 程序取消的接缝。后台任务（或将来接进来的信号）只置一个原子标记；真正的关框发生在
// GUI 主线程，由 RunMonitoredDialog 的看守线程转成一次 RequestClose。一次令牌只取消一次。
class ConsentCancelToken {
public:
    void Cancel() noexcept { cancelled_.store(true); }
    bool Cancelled() const noexcept { return cancelled_.load(); }

private:
    std::atomic<bool> cancelled_{false};
};

// 弹框这一件事的接口。测试里注入假的应答器，就能在没有桌面的情况下把整台状态机跑完。
class IConsentPrompt {
public:
    virtual ~IConsentPrompt() = default;
    virtual ConsentReply Ask(const ConsentQuestion& question) = 0;
};

// 真机上那一个：wxWidgets 模态对话框，默认焦点与默认按钮都在"否"上。
//
// 对话框在 GUI 主线程上创建、显示、销毁；--consent-timeout-ms 由 RunMonitoredDialog 的
// 看守线程计时，到点只把"关框"消息投递给 GUI 主线程。CLI 线程阻塞在模态循环里，不忙等，
// 也不允许后台线程直接碰控件。timeoutMs = 0 表示一直等人回答。
class DialogConsentPrompt final : public IConsentPrompt {
public:
    explicit DialogConsentPrompt(uint64_t timeoutMs = 0,
                                 ConsentCancelToken* cancelToken = nullptr)
        : timeoutMs_(timeoutMs), cancelToken_(cancelToken) {}

    ConsentReply Ask(const ConsentQuestion& question) override;

private:
    uint64_t timeoutMs_ = 0;
    ConsentCancelToken* cancelToken_ = nullptr;
};

// ---------------------------------------------------------------------------
// 弹框生命周期机制
//
// "框怎么弹、怎么关"（驱动）与"等多久、到点关不关得掉"（状态机）分开。状态机本身只认识
// 一个与 wxWidgets 无关的 DialogDriver；生产驱动在 src/gui/WxConsentDriver.cpp，测试驱动
// 只编译进独立测试目标。这不是发布版后门：ECAPTURE.EXE 里没有能选驱动的运行时入口。
//
// 线程模型：驱动 Show() 在 GUI 主线程上创建并显示模态框，阻塞到窗口销毁；状态机在看守
// 线程上计时，到点或收到取消令牌时调用 RequestClose()。RequestClose 必须线程安全，并且
// 只能把消息投递给 GUI 主线程 —— 它**不得**在后台线程直接操作控件。
// ---------------------------------------------------------------------------

// 看守线程要求驱动关闭窗口的理由。
enum class DialogCloseReason {
    kTimedOut,
    kCancelled,
};

struct DialogResult {
    ConsentAnswer answer = ConsentAnswer::kInitFailed;
    NativeErrorCode nativeError = kNativeErrorNone;  // 仅 kInitFailed：那一次失败的系统原值
};

class DialogDriver {
public:
    DialogDriver() = default;
    virtual ~DialogDriver() = default;
    DialogDriver(const DialogDriver&) = delete;
    DialogDriver& operator=(const DialogDriver&) = delete;

    // 在 GUI 主线程上被调用：把框弹出来，阻塞到它关闭并销毁。
    virtual DialogResult Show() = 0;

    // 在看守线程上被调用，宽限期内每个轮询切片重试一次。
    // 返回值只表示"关的指令送出去了没有"；生效与否由 Show() 返回、窗口销毁之后裁决。
    // 实现必须把关闭请求投递到 GUI 主线程（wxQueueEvent / 等价机制），不能直接碰控件。
    virtual bool RequestClose(DialogCloseReason reason) = 0;

    // Show() 之前由状态机给出同一个截止时刻。驱动可以据此让"到点之后才落地的点击"
    // 仍按超时收尾 —— 与看守线程用的是同一个时刻，不是另算一份期限。
    virtual void SetDeadline(std::optional<std::chrono::steady_clock::time_point> deadline) {
        (void)deadline;
    }
};

// 看守一次弹框直到拿到一个可信的结束。规则：
//   * 只有窗口真的关闭、且驱动报回 kAccepted、且期限还没到，才是 kAccepted；
//     同意之后固定留一段关闭动画缓冲（kDialogSettleMs，见 Consent.cpp），不为赶期限省略。
//   * 人答"否" => kDeclined；人按 X / Esc 关框 => kClosed。
//   * 期限到点 => 先 latch kTimedOut 再裁决：宽限期内反复 RequestClose；哪怕"是"在同一
//     瞬间被点下，也一律按 kTimedOut 收尾，绝不改成同意。
//   * 取消令牌 => 同样的关闭通道，结局是 kCancelled（与人的回答分开）。
//   * 框根本没弹出来、驱动抛异常 => kInitFailed（带系统原值）；它比"没人回答"更具体。
ConsentReply RunMonitoredDialog(std::shared_ptr<DialogDriver> driver, uint64_t timeoutMs,
                                ConsentCancelToken* cancelToken = nullptr);

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
    bool Covers(const PhysRect& area) const;
    const std::wstring& TargetKey() const { return targetKey_; }
    PhysRect Approved() const { return approved_; }

private:
    friend class ConsentGate;
    DesktopPermit(std::wstring targetKey, PhysRect approved)
        : targetKey_(std::move(targetKey)), approved_(approved) {}

    static DesktopPermit Issue(std::wstring targetKey, PhysRect approved) {
        return DesktopPermit(std::move(targetKey), approved);
    }

    std::wstring targetKey_;
    PhysRect approved_{};
};

// 本次请求的目标清单里的一项：给判定器用来核对"要截的就是人看到的那些"。
// 这里**不存**拼好的整行文案 —— 文案由判定器在弹框那一刻从 (subject, area) 现渲染，
// 所以 SetTargetArea 刷新区域之后，框上写的坐标与实际要采样的矩形必然是同一份事实，
// 不存在"内部矩形更新了、给人看的那行还是旧坐标"这种平行文本过期的形状。
struct GateTarget {
    bool screen = false;
    std::wstring key;      // 窗口给 0x…，屏幕给设备名 —— 与诊断里的 target 同形
    std::wstring subject;  // 目标身份那一段文字（DescribeWindow / DescribeScreen），不含区域
    PhysRect area{};           // 该目标当前的屏幕矩形（窗口矩形 / 该屏矩形）
};

struct GateConfig {
    bool yes = false;                       // --yes
    std::vector<GateTarget> targets;        // 本次全部目标
    std::vector<std::wstring> outputs;      // 整批展开后的输出（"-" 用"标准输出"那行代替）
    // 这一次会不会另外留一份历史副本（归档根解析得出来时才为真）。它属于"人要批准的那件事"的
    // 一部分：副本落在程序自己的目录里、而且不自动清理，所以不能只写在 README 与 JSON 里就
    // 当成已经告知过 —— 确认框上那一句与 images[].history 说的是同一件事，谁也不冒充谁。
    bool historyCopy = false;
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
    // topology 必须由调用方显式给出：真机传 EnumScreens，测试传假布局。
    // 不再在这里默认去问系统 —— 授权层不认识任何平台的屏幕枚举。
    ConsentGate(GateConfig config, IConsentPrompt& prompt, TopologyProvider topology);

    // 窗口内容路径（wgc / printwindow / dwm 的缩略图主路径）。
    // 带 --yes 时直接放行；否则整批问一次，之后同批次不再重复打扰。
    bool AuthorizeWindow(const wchar_t* path, const std::wstring& targetKey, Diagnostic* err);

    // 桌面路径（bitblt、duplication、整屏任何通道、以及 DWM 内部的屏幕退路）。
    // 永远要人确认；批准了才把 *out 填好，调用方没有它就取不到屏幕像素。
    // area 是"这次实际要取样的那块屏幕矩形"，必须落在弹框上给人看过的那一片区域内。
    // 弹框前把清单（目标、区域、拓扑、输出）冻结成快照：框上的文字与答"是"后签发的授权
    // 都出自这同一份；应答返回之后先重新查询拓扑与快照比较，确认期间布局变了就按
    // capture.consent_stale 收尾（不授权，也不把新布局当成批准过的基线）。
    bool AuthorizeDesktop(const wchar_t* path, const std::wstring& targetKey,
                          const PhysRect& area, std::optional<DesktopPermit>* out,
                          Diagnostic* err);

    // 流水线在取帧前量到的目标矩形要能刷新到这里，好让弹框上写的是"现在就在那儿"的矩形。
    // 只影响还没弹出去的那一轮：一旦弹框，本轮问的就是那一刻的快照，之后刷新改不了它。
    // 已经批准的桌面授权用的是快照里冻结的区域，所以挪动过的窗口会被判成"范围已变"而重新问。
    void SetTargetArea(const std::wstring& targetKey, const PhysRect& area);

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
                      ConsentAnswer answer, NativeErrorCode nativeError) const;
    // 确认基线在弹框期间失效（拓扑变了）：这一问不授权，也不换基线重试
    Diagnostic Stale(const wchar_t* path, const std::wstring& targetKey) const;
    // 人同意、且确认后拓扑复核与快照一致之后：把快照里列出的目标区域冻结成本次桌面授权
    void GrantDesktop(const ConsentSnapshot& snap);
    bool DesktopGrantedFor(const std::wstring& targetKey, const PhysRect& area,
                           PhysRect* approved) const;

    GateConfig config_;
    IConsentPrompt& prompt_;
    TopologyProvider topology_;
    uint64_t topologyAtAsk_ = 0;   // 弹框**之前**的拓扑基线（来自快照，不是应答之后首次所见）

    Level windowLevel_ = Level::kNotAsked;
    Level desktopLevel_ = Level::kNotAsked;
    // 拒绝的原因（人答否 / 关框 / 弹不出框 / 期限到点 / 程序取消）要粘住：后面那些目标
    // 不再问第二遍，但它们各自的诊断得说清当初是哪一种，而不是统统写成"被拒绝"。
    ConsentAnswer lastDenyAnswer_ = ConsentAnswer::kDeclined;
    NativeErrorCode lastDenyNativeError_ = kNativeErrorNone;
    std::vector<GateTarget> desktopAreas_;  // 答"是"那一轮弹框前冻结的授权快照清单
    int windowAsks_ = 0;
    int desktopAsks_ = 0;
};

}  // namespace ecapture
