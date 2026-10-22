// 截图授权状态机的离线判据（由 tests\consent.ps1 编译并运行）。
//
// 为什么在这一层测：授权这件事的分支（升级要重新确认、拒绝之后不再换后端也不再问第二遍、
// 目标挪位置或换屏幕拓扑就作废凭证、--yes 只对窗口内容路径生效……）在真机上每一条都要人
// 去点一次确认框，而发布版里不许留任何"测试旁路"。所以这里注入假的应答器与假的屏幕布局，
// 把 ConsentGate 与路径分类整台跑完；真机那部分判据留在 tests\consent.ps1 与 window_shot.bat。
//
// 这个可执行文件不含任何取帧代码：它只链接纯逻辑那几份源文件（见 CMakeLists.txt）。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "../src/CaptureScope.h"
#include "../src/CliOptions.h"
#include "../src/Consent.h"
#include "../src/ConsentDialog.h"        // 弹框内容构造（纯函数，不链 wx）
#include "../src/Deadline.h"
#include "../src/Lang.h"
#include "../src/ScreenMatch.h"
#include "../src/core/ConsentState.h"    // 状态机与两层授权判据
#include "../src/core/DisplayText.h"     // 安全展示

using namespace ecapture;

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        ++g_passed;
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

PhysRect R(int l, int t, int r, int b) { return PhysRect{l, t, r, b}; }

// ---------------------------------------------------------------------------
// 假应答器：记下每一轮问的是什么，按脚本回答
// ---------------------------------------------------------------------------

struct Asked {
    PixelScope scope;
    std::wstring path;
    bool yesGiven;
    size_t targets;
    size_t outputs;
    std::vector<std::wstring> targetLines;   // 框上渲染出的每一行（"展示什么批准什么"要比文字）
};

class FakePrompt final : public IConsentPrompt {
public:
    ConsentReply Ask(const ConsentQuestion& q) override {
        ++asks;
        asked.push_back(Asked{q.scope, q.path, q.yesGiven, q.targets.size(), q.outputs.size(),
                               q.targets});
        // 真人等待那一段由回调推进假时钟（F04 判据），顺带记录"框挂着时"看到的剩余预算
        if (onAsk) onAsk();
        if (throwInAsk) throw std::runtime_error("fake prompt blew up");
        ConsentReply reply;
        reply.answer = ConsentAnswer::kAccepted;   // 脚本没写到的那一次一律答"是"
        const size_t n = asks;   // 第 n 次弹框（1 起）
        if (n <= answers.size()) reply.answer = answers[n - 1];
        // 初始化失败要带出系统原值；人答否 / 关框 / 超时 / 取消各自是一个独立答案。
        reply.nativeError = initFailedNativeError;
        return reply;
    }

    int asks = 0;
    std::vector<Asked> asked;
    std::vector<ConsentAnswer> answers;   // 空 = 一律答"是"
    NativeErrorCode initFailedNativeError = 0;
    std::function<void()> onAsk;          // 弹框在世时执行：推进假时钟 / 探测预算冻结
    bool throwInAsk = false;              // 应答器抛异常：判暂停作用域的异常退栈恢复
};

// ---------------------------------------------------------------------------
// 假屏幕拓扑：判据要能自己改布局，看看已给出的授权还作不作数
// ---------------------------------------------------------------------------

std::vector<ScreenInfo> g_screens;

std::vector<ScreenInfo> FakeTopology() { return g_screens; }

ScreenInfo MakeScreen(const wchar_t* device, PhysRect bounds, bool primary) {
    ScreenInfo s;
    s.deviceName = device;
    s.bounds = bounds;
    s.primary = primary;
    s.ordinal = 1;
    return s;
}

GateTarget Target(const wchar_t* key, PhysRect area, bool screen = false) {
    GateTarget t;
    t.key = key;
    t.area = area;
    t.screen = screen;
    // 只给身份那一段；整行由判定器在弹框那一刻从 (subject, area) 现渲染（F06：
    // 框上坐标与将要冻结的授权区域同源，不存在"刷新了矩形、文案还是旧的"）。
    t.subject = key;
    return t;
}

GateConfig Config(bool yes, std::vector<GateTarget> targets) {
    GateConfig c;
    c.yes = yes;
    c.targets = std::move(targets);
    c.outputs = {L"D:\\shots\\a.png", L"D:\\shots\\b.png"};
    c.captureLabel = L"auto";
    return c;
}

// 建一个判定器：目标窗口在 (100,100)-(500,400)，另有一个在 (600,0)-(900,300)
struct Fixture {
    FakePrompt prompt;
    ConsentGate gate;

    Fixture(bool yes, std::vector<GateTarget> targets,
            uint32_t requiredLayers = DefaultRequiredGrantLayers())
        : gate(MakeConfig(yes, std::move(targets), requiredLayers), prompt, FakeTopology) {}

    static GateConfig MakeConfig(bool yes, std::vector<GateTarget> targets,
                                 uint32_t requiredLayers) {
        GateConfig c = Config(yes, std::move(targets));
        c.requiredGrantLayers = requiredLayers;
        return c;
    }
};

// ---------------------------------------------------------------------------
// F04 判据的夹具：判定器带着**假时钟**的自动预算跑整台状态机。
// 真人等待由 FakePrompt::onAsk 推进假时钟（弹框阻塞 + 同意后关闭动画都发生在那一段），
// 于是"人工确认到底烧不烧 --timeout-ms"判的是精确毫秒数，不是墙上的秒表。
// ---------------------------------------------------------------------------

struct FakeClock {
    int64_t ms = 0;
};

struct BudgetFixture {
    FakeClock clock;
    Deadline dl;
    FakePrompt prompt;
    ConsentGate gate;

    BudgetFixture(bool yes, std::vector<GateTarget> targets, uint64_t totalMs)
        : dl(Deadline::FromTotalMs(totalMs, [this] { return clock.ms; })),
          gate(MakeConfig(yes, std::move(targets), &dl), prompt, FakeTopology) {}

    static GateConfig MakeConfig(bool yes, std::vector<GateTarget> targets, const Deadline* dl) {
        GateConfig c = Config(yes, std::move(targets));
        c.autoBudget = dl;
        return c;
    }
};

const PhysRect kAreaA = R(100, 100, 500, 400);
const PhysRect kAreaB = R(600, 0, 900, 300);
const std::wstring kA = L"0xAAAA";
const std::wstring kB = L"0xBBBB";

// --yes 想跳过程序确认时必须交齐的三件事实（写成一个夹具，测试里一眼看得出给了什么）。
YesEligibility EligibleYes() { return YesEligibility{true, true, true}; }
// 授权后绑定核实：正常通过 / 目标已经变了。
bool VerifyOk(Diagnostic*) { return true; }
bool VerifyTargetChanged(Diagnostic* err) {
    if (err) {
        err->code = codes::kTargetChanged;
        err->stage = stages::kConsent;
    }
    return false;
}

std::vector<GateTarget> TwoTargets() {
    return {Target(kA.c_str(), kAreaA), Target(kB.c_str(), kAreaB)};
}

bool CodeIs(const Diagnostic& d, const wchar_t* code) { return d.code == code; }

// ---------------------------------------------------------------------------
// 假弹框驱动：不弹真框，把生产的生命周期状态机（RunMonitoredDialog）整台跑完。
//
// Show() 在调用方线程上执行（生产环境就是 GUI 主线程），看守线程由 RunMonitoredDialog
// 自己创建。这个驱动只控制三件事：框多晚出现（appearAfterMs）、关它的话第几次才生效
// （honorCloseAfter）、收尾带回哪个结局（answer）。判的是生产那份代码，不是它独立抄一遍的
// 副本。驱动只存在于本测试翻译单元 —— 发布版 ECAPTURE.EXE 里没有任何入口能选到它，
// 所以它不构成"accepted 后门"：连 ConsentGate 都到不了的那层假象都没有。
// ---------------------------------------------------------------------------

class FakeDialog final : public DialogDriver {
public:
    // —— 剧本 ——
    ConsentAnswer answer = ConsentAnswer::kDeclined;  // Show 返回的结局
    NativeErrorCode failNativeError = 0;              // 非 0 => "没弹成"
    bool throwInShow = false;                         // 驱动崩了：按初始化失败判
    int appearAfterMs = 0;                            // 窗口 Show 开始这么久之后才找得到
    int honorCloseAfter = 0;                          // 第 N 次有效关框请求之后才真关；INT_MAX = 永不关

    // —— 可观察 ——
    std::atomic<int> closeSlices{0};       // RequestClose 被调了几次（含还没窗口的）
    std::atomic<int> effectiveCloses{0};   // 其中"窗口已在、指令真的送出去了"的次数
    std::atomic<bool> showEntered{false};
    std::atomic<bool> showReturned{false};

    DialogResult Show() override {
        showEntered.store(true);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            showStart_ = Clock::now();
            cv_.wait(lock, [&] { return releasedLocked(); });
        }
        showReturned.store(true);
        if (throwInShow) throw std::runtime_error("fake driver blew up");

        DialogResult r;
        if (failNativeError != 0) {
            r.answer = ConsentAnswer::kInitFailed;
            r.nativeError = failNativeError;
        } else {
            r.answer = answer;
        }
        return r;
    }

    bool RequestClose(DialogCloseReason) override {
        closeSlices.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     Clock::now() - showStart_)
                                     .count();
            if (elapsed < appearAfterMs) return false;
            effectiveCloses.fetch_add(1);
        }
        cv_.notify_all();
        return true;
    }

    void ReleaseNow() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            releaseNow_ = true;
        }
        cv_.notify_all();
    }

    bool WaitShowReturned() {
        for (int i = 0; i < 200; ++i) {
            if (showReturned.load()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

private:
    using Clock = std::chrono::steady_clock;

    bool releasedLocked() const {
        if (releaseNow_) return true;
        return honorCloseAfter > 0 && honorCloseAfter != INT_MAX &&
               effectiveCloses.load() >= honorCloseAfter;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool releaseNow_ = false;
    Clock::time_point showStart_{};
};

struct Tick {
    using Clock = std::chrono::steady_clock;
    Clock::time_point t0 = Clock::now();
    long long Ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    }
};

}  // namespace

int main() {
    SetLanguage(Language::kEn);   // 判据只看 code / stage / 计数，文案跟着英语那份走
    g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};

    // =========================================================================
    Section("1) 路径分类：像素从哪来，与通道名字无关");
    // =========================================================================
    Check(ScopeOf(paths::kWgc) == PixelScope::kWindowContent, "wgc = 窗口内容");
    Check(ScopeOf(paths::kPrintWindow) == PixelScope::kWindowContent, "printwindow = 窗口内容");
    Check(ScopeOf(paths::kDwmThumbnail) == PixelScope::kWindowContent, "dwm.thumbnail = 窗口内容");
    Check(ScopeOf(paths::kDwmScreen) == PixelScope::kDesktop, "dwm.screen = 桌面");
    Check(ScopeOf(paths::kBitBltScreen) == PixelScope::kDesktop, "bitblt.screen = 桌面");
    Check(ScopeOf(paths::kDuplicationFrame) == PixelScope::kDesktop, "duplication.frame = 桌面");
    Check(ScopeOf(paths::kScreenWgc) == PixelScope::kDesktop, "整屏 wgc = 桌面（同样强制确认）");
    Check(ScopeOf(paths::kScreenBitBlt) == PixelScope::kDesktop, "screen.bitblt = 桌面");
    Check(ScopeOf(paths::kScreenDuplication) == PixelScope::kDesktop, "screen.duplication = 桌面");
    Check(ScopeOf(paths::kX11Desktop) == PixelScope::kDesktop,
          "x11.desktop 根窗口像素 = 桌面（显示器/全桌面/区域同源）");
    Check(ScopeOf(L"brand-new-channel") == PixelScope::kDesktop, "没登记过的路径默认按桌面处理");
    Check(ScopeOf(L"") == PixelScope::kDesktop, "空名字默认按桌面处理");
    Check(ScopeOf(static_cast<const wchar_t*>(nullptr)) == PixelScope::kDesktop,
          "nullptr 默认按桌面处理（漏登记不会变松）");
    Check(NeedsHumanConsent(paths::kBitBltScreen, true), "--yes 之下桌面路径仍要确认");
    Check(NeedsHumanConsent(paths::kX11Desktop, true),
          "X11 根窗口桌面像素在 --yes 之下仍必须人工确认");
    Check(!NeedsHumanConsent(paths::kWgc, true), "--yes 免掉窗口内容路径的确认");
    Check(NeedsHumanConsent(paths::kWgc, false), "没有 --yes 时窗口内容路径也要确认");
    Check(NeedsHumanConsent(L"unknown", true), "没有 --yes 也没有 --yes 之外的旁路");
    Check(std::wstring(CaptureMethodName(CaptureMethod::kAuto)) == L"auto", "通道机器名稳定");

    // =========================================================================
    Section("2) 窗口内容路径：--yes 与不带的差别");
    // =========================================================================
    {
        Fixture f(true, TwoTargets());
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "带 --yes：窗口内容路径放行");
        Check(f.prompt.asks == 0, "带 --yes：一次框都没弹");
    }
    {
        Fixture f(false, TwoTargets());
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "不带 --yes：第一次问人之后放行");
        Check(f.prompt.asks == 1, "不带 --yes：弹了一次");
        Check(f.prompt.asked[0].scope == PixelScope::kWindowContent, "问的是窗口内容这一级");
        Check(f.prompt.asked[0].targets == 2, "把整批两个目标都列给人看了");
        Check(f.prompt.asked[0].outputs == 2, "把整批输出名都列给人看了");
        Check(!f.prompt.asked[0].yesGiven, "这一轮没有 --yes");
        Check(f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err, EligibleYes(), VerifyOk),
              "同批次第二个目标 / 另一条窗口路径不再重复弹框");
        Check(f.prompt.asks == 1, "一次确认覆盖本次列出的那一批目标");
    }

    // =========================================================================
    Section("3) 拒绝：稳定诊断 + 停止本次请求 + 不再问第二遍");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kDeclined};
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "答\"否\"就不放行");
        Check(CodeIs(err, codes::kAccessDenied), "拒绝的码是 capture.access_denied");
        Check(err.stage == stages::kConsent, "拒绝的 stage 是 consent");
        Check(err.target == kA, "拒绝带上是哪个目标");
        Check(err.value == paths::kWgc, "拒绝带上是哪条路径（value 里是路径名）");
        Check(err.backend.empty(), "通道名由流水线补，判定器不猜");
        Check(f.gate.Refused(), "判定器记住\"已经被人拒过\"");
        const int before = f.prompt.asks;
        Diagnostic err2;
        Check(!f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err2, EligibleYes(), VerifyOk),
              "被拒之后同批次其它目标也不再截（不换后端、不重试）");
        Check(f.prompt.asks == before, "被拒之后不重复弹框骚扰人");
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err2),
              "被拒之后桌面路径同样不放行");
        Check(f.prompt.asks == before, "被拒之后桌面路径也不弹第二次");
        Check(!permit.has_value(), "被拒之后拿不到桌面凭证");
    }
    {
        // 关对话框（X）与答"否"同义：都是人不同意
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kDeclined};
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "答\"否\"= 关框，都不放行");
    }

    // =========================================================================
    Section("4) 没有交互桌面：与\"人答了否\"分开给码，且绝不自动继续");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kInitFailed};
        f.prompt.initFailedNativeError = 21;   // ERROR_NOT_READY：一个具体的系统原值
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "弹不出框就不放行");
        Check(CodeIs(err, codes::kConsentUnavailable),
              "弹不出框给的是 capture.consent_unavailable，不是\"用户拒绝\"");
        Check(err.win32 == 21, "把\"为什么弹不出来\"的系统原值带出去");
        Check(err.stage == stages::kConsent, "stage 仍是 consent");
        Check(f.gate.Refused(), "弹不出框也停止本次剩下的采集");
    }
    {
        // 同一种情况下，带 --yes 的窗口内容路径本来就不需要桌面，所以完全不受影响
        Fixture f(true, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kInitFailed};
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kDwmThumbnail, kA, &err, EligibleYes(), VerifyOk),
              "没有交互桌面时，带 --yes 的窗口内容路径照旧可以截");
        Check(f.prompt.asks == 0, "这条路一次都不弹框");
    }

    // =========================================================================
    Section("5) 桌面路径：--yes / --quiet / 已确认过窗口 都不能跳过");
    // =========================================================================
    {
        Fixture f(true, TwoTargets());   // 带着 --yes
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "--yes 之下桌面路径问过人之后才放行");
        Check(f.prompt.asks == 1, "桌面路径弹了框（--yes 没把它一起批掉）");
        Check(f.prompt.asked[0].scope == PixelScope::kDesktop, "问的是桌面这一级");
        Check(f.prompt.asked[0].yesGiven, "知道调用方给了 --yes（框上要说清它不生效）");
        Check(f.prompt.asked[0].path == paths::kBitBltScreen, "框上写的是实际那条路径");
        Check(permit.has_value() && permit->Covers(kAreaA), "批下来就有覆盖该区域的凭证");
        Check(permit->TargetKey() == kA, "凭证绑在具体那个目标上");
    }
    {
        // 范围升级：先确认了窗口内容，不等于确认了桌面
        Fixture f(false, TwoTargets());
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "不带 --yes：先确认窗口内容");
        Check(f.prompt.asks == 1, "窗口那一级弹了一次");
        std::optional<DesktopPermit> permit;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "同一批次升级到桌面路径时，问过这一次才放行");
        Check(f.prompt.asks == 2, "窗口那一次与桌面这一次是两个独立的确认（不跨级复用）");
        Check(f.prompt.asked[1].scope == PixelScope::kDesktop, "第二次问的是桌面这一级");
    }
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kAccepted, ConsentAnswer::kDeclined};
        Diagnostic err;
        f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk);
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kScreenWgc, kA, kAreaA, &permit, &err),
              "桌面那一级答\"否\"就不放行");
        Check(CodeIs(err, codes::kAccessDenied), "桌面拒绝的码是 capture.access_denied");
        Check(err.stage == stages::kConsent, "stage=consent");
        Check(!permit.has_value(), "拒绝 = 没有凭证");
    }

    // =========================================================================
    Section("6) 不扩大目标、不扩大区域");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "第一次桌面确认覆盖清单里列出的两个目标");
        Check(f.prompt.asks == 1, "只弹一次");
        f.prompt.answers = {};
        std::optional<DesktopPermit> p2;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kB, kAreaB, &p2, &err),
              "第二个目标用的是同一次确认（清单里已列出）");
        Check(f.prompt.asks == 1, "同一次确认之内不再弹框");
        std::optional<DesktopPermit> p3;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, L"0xCCCC", R(0, 0, 10, 10), &p3, &err),
              "清单上没给人看过的目标拿不到凭证（扩大目标不是确认过的范围）");
        Check(f.prompt.asks == 2, "遇到清单外的目标会重新问，而不是静默批掉");
        Check(!p3.has_value(), "重新问之前那个目标没有凭证");
    }
    {
        // 区域：凭证只认批准过的那一片（严格包含、零容差）
        Fixture f(false, TwoTargets());
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err);
        Check(permit->Covers(R(120, 120, 480, 380)), "批准区域内的子矩形算覆盖");
        Check(!permit->Covers(R(100, 100, 1500, 1000)), "整块屏幕不在那一片之内");
        Check(!permit->Covers(R(-50, -50, 50, 50)), "挪到别处不算");
        Check(!permit->Covers(R(90, 90, 510, 410)),
              "向外扩 10 像素不算覆盖（零容差：容差会把没批准过的画面带进图里）");
        Check(!permit->Covers(R(0, 0, 0, 0)), "空矩形一律不算覆盖");
        Check(!permit->Covers(R(100, 100, 500 + 300, 400)), "明显变大不算覆盖");
        // 目标在确认之后挪了位置：取样矩形超出批准范围 = 不截
        f.gate.SetTargetArea(kA, R(2000, 100, 2400, 400));
        std::optional<DesktopPermit> stale;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, R(2000, 100, 2400, 400), &stale,
                                       &err) ||
                  !stale->Covers(kAreaA),
              "目标挪走之后，旧凭证不再覆盖新位置（要重新确认）");
    }
    {
        // 屏幕拓扑变化使已给出的桌面授权作废
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "先拿到一次桌面授权");
        Check(f.prompt.asks == 1, "弹了一次");
        g_screens.push_back(MakeScreen(L"\\\\.\\DISPLAY2", R(1920, 0, 3840, 1080), false));
        std::optional<DesktopPermit> after;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &after, &err),
              "插上第二块屏之后重新问一次才放行");
        Check(f.prompt.asks == 2, "屏幕拓扑一变，之前那次\"我批准截这一块\"不再成立");
        g_screens.pop_back();
    }

    // =========================================================================
    Section("7) 整屏目标：每一条通道都要问");
    // =========================================================================
    {
        for (const wchar_t* p : {paths::kScreenWgc, paths::kScreenBitBlt, paths::kScreenDuplication}) {
            Fixture f(true, {Target(L"DISPLAY1", R(0, 0, 1920, 1080), true)});
            Diagnostic err;
            std::optional<DesktopPermit> permit;
            Check(f.gate.AuthorizeDesktop(p, L"DISPLAY1", R(0, 0, 1920, 1080), &permit, &err) &&
                      permit.has_value(),
                  "整屏任何通道都要人确认（--yes 无效）");
            Check(f.prompt.asks == 1, "确实弹了框");
        }
    }
    {
        // --monitor all：一次确认，两块屏各一张（各自都能拿到覆盖自己那块的凭证）
        Fixture f(false, {Target(L"DISPLAY1", R(0, 0, 1920, 1080), true),
                          Target(L"DISPLAY2", R(1920, 0, 3840, 1080), true)});
        Diagnostic err;
        std::optional<DesktopPermit> p1, p2;
        Check(f.gate.AuthorizeDesktop(paths::kScreenWgc, L"DISPLAY1", R(0, 0, 1920, 1080), &p1, &err),
              "第一块屏批准");
        Check(f.gate.AuthorizeDesktop(paths::kScreenWgc, L"DISPLAY2", R(1920, 0, 3840, 1080), &p2,
                                      &err),
              "第二块屏用同一次确认");
        Check(f.prompt.asks == 1, "两块屏只问一次");
        Check(p1->Covers(R(0, 0, 1920, 1080)) && !p1->Covers(R(1920, 0, 3840, 1080)),
              "第一块屏的凭证不能拿去截第二块");
    }

    // =========================================================================
    Section("8) 弹框生命周期：六档结局分得开，任何超时路径都可收尾");
    // =========================================================================
    {
        // 答"是"：同意，而且关闭动画缓冲一分不少（它是对用户的承诺，不是可省的重负）
        auto d = std::make_shared<FakeDialog>();
        d->answer = ConsentAnswer::kAccepted;
        d->ReleaseNow();
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kAccepted, "答\"是\" => kAccepted");
        Check(t.Ms() >= 950, "同意路径保留约 1 秒关闭动画缓冲（不被任何超时逻辑削减）");
    }
    {
        // 设了期限但人在期限前答了"是"：缓冲照留 —— 有 --consent-timeout-ms 不等于抄近路
        auto d = std::make_shared<FakeDialog>();
        d->answer = ConsentAnswer::kAccepted;
        d->ReleaseNow();
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 30000);
        Check(r.answer == ConsentAnswer::kAccepted && t.Ms() >= 950,
              "带期限的提前同意：accepted 且缓冲完整");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->answer = ConsentAnswer::kDeclined;
        d->ReleaseNow();
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kDeclined, "答\"否\" => kDeclined");
        Check(t.Ms() < 900, "非同意路径不付关闭动画缓冲");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->answer = ConsentAnswer::kClosed;   // 人自己按 X / Esc 关框
        d->ReleaseNow();
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kClosed, "人关框 => kClosed（与答否分开）");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->failNativeError = 42;   // Show 立刻带回"没弹成"
        d->ReleaseNow();
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kInitFailed && r.nativeError == 42,
              "弹不出框 => kInitFailed 并带出系统原值");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->throwInShow = true;
        d->ReleaseNow();
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kInitFailed,
              "驱动抛异常 => 初始化失败（绝不拖崩进程、也绝不算同意）");
    }
    {
        // 到点没人答、第一次关框就生效：超时
        auto d = std::make_shared<FakeDialog>();
        d->honorCloseAfter = 1;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kTimedOut, "到点关框 => kTimedOut");
        Check(t.Ms() >= 300 && t.Ms() < 3500, "有界返回（期限 + 宽限之内）");
        Check(d->closeSlices.load() >= 1, "关框请求确实交给驱动了");
    }
    {
        // 头几次关框请求被无视 —— 靠宽限期内的重试救回
        auto d = std::make_shared<FakeDialog>();
        d->honorCloseAfter = 10;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kTimedOut, "关框晚生效仍是 kTimedOut");
        Check(d->effectiveCloses.load() >= 10, "宽限期内逐切片重试，第一条没生效不是死路");
        Check(t.Ms() < 300 + 3000 + 500, "晚生效也没撞上放弃线");
    }
    {
        // 框弹得晚：期限到时还没有窗口可关，后来的切片把它补上
        auto d = std::make_shared<FakeDialog>();
        d->appearAfterMs = 900;
        d->honorCloseAfter = 1;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kTimedOut, "弹框延迟后到点仍按超时收尾");
        Check(d->effectiveCloses.load() >= 1, "窗口迟到后关框请求补送成功");
        Check(d->closeSlices.load() >= d->effectiveCloses.load(), "迟到的请求如实计了数");
    }
    {
        // 怎么都不因关框请求而回：状态机不能假装窗口已经销毁。这里从另一条线程跑状态机，
        // 到点 + 宽限之后先确认它仍在等 Show()，再放行；迟到的"是"改不了已锁定的超时。
        auto d = std::make_shared<FakeDialog>();
        d->honorCloseAfter = INT_MAX;
        d->answer = ConsentAnswer::kAccepted;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        ConsentReply r;
        std::thread worker([&] {
            r = RunMonitoredDialog(d, 200);
            {
                std::lock_guard<std::mutex> lock(m);
                done = true;
            }
            cv.notify_all();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200 + 3000 + 300));
        {
            std::lock_guard<std::mutex> lock(m);
            Check(!done, "窗口还没销毁时状态机不提前返回（不把\"放弃\"说成已经收尾）");
        }
        d->ReleaseNow();
        {
            std::unique_lock<std::mutex> lock(m);
            Check(cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; }),
                  "放行之后状态机收尾");
        }
        worker.join();
        Check(r.answer == ConsentAnswer::kTimedOut,
              "线程迟到带回\"是\"也改不了已锁定的超时");
    }
    {
        // 期限到点与"是"同刻落地：保守优先级 —— 超时赢
        auto d = std::make_shared<FakeDialog>();
        d->answer = ConsentAnswer::kAccepted;
        d->honorCloseAfter = 1;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kTimedOut,
              "超时与同意同刻发生：按 kTimedOut，绝不改成同意");
    }
    {
        // 程序取消：与人的回答分开，窗口同样经驱动关闭
        auto d = std::make_shared<FakeDialog>();
        d->honorCloseAfter = 1;
        ConsentCancelToken token;
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            token.Cancel();
        });
        const ConsentReply r = RunMonitoredDialog(d, 0, &token);
        canceller.join();
        Check(r.answer == ConsentAnswer::kCancelled, "取消令牌 => kCancelled");
        Check(d->closeSlices.load() >= 1, "取消也走同一条关框通道");
    }
    {
        // 竞争的另一侧的干净判据：Show 在期限之前带回 code 0 => 报不可用（约定见 Consent.h）
        auto d = std::make_shared<FakeDialog>();
        d->failNativeError = 5;
        d->ReleaseNow();                // Show 立刻就回，期限完全没参与
        const ConsentReply r = RunMonitoredDialog(d, 100000);
        Check(r.answer == ConsentAnswer::kInitFailed && r.nativeError == 5,
              "期限之前弹框就失败：kInitFailed 带原码，不记成超时");
    }

    // =========================================================================
    Section("9) \"到点没人答\"在授权状态机里：独立诊断码 + 粘住整批");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kTimedOut};
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "超时不放行");
        Check(CodeIs(err, codes::kConsentTimeout), "超时的码是 capture.consent_timeout（≠答否）");
        Check(err.stage == stages::kConsent, "stage 仍是 consent");
        Check(f.gate.Refused(), "超时同样停止本次剩下的采集");
        Diagnostic err2;
        Check(!f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err2, EligibleYes(), VerifyOk), "同批第二个目标不再截");
        Check(CodeIs(err2, codes::kConsentTimeout), "粘住的诊断保持超时码，不被改写成\"被拒绝\"");
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err2) &&
                  CodeIs(err2, codes::kConsentTimeout),
              "桌面路径撞同一堵墙：也保持超时码");
    }
    {
        // 窗口那级批了、桌面那级到点没人答：之后一切拒绝都按桌面超时粘住
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kAccepted, ConsentAnswer::kTimedOut};
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "窗口那级先批");
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kScreenWgc, kA, kAreaA, &permit, &err),
              "桌面那级超时拒绝");
        Check(CodeIs(err, codes::kConsentTimeout), "桌面超时的码正确");
        Check(!f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err, EligibleYes(), VerifyOk), "跨级也要停");
        Check(CodeIs(err, codes::kConsentTimeout), "跨级粘住的仍是超时码");
    }

    // =========================================================================
    Section("10) 自动预算：只暂停真正等人的段（F04）");
    // =========================================================================
    {
        // 文档场景走真判定器：总预算 5000；先自动处理 1000，人工等待 20000 + 关闭缓冲
        // 1000；同意之后仍剩 4000 可继续处理；再自动处理 500 后剩 3500。
        BudgetFixture f(false, TwoTargets(), 5000);
        f.clock.ms += 1000;   // 弹框之前的自动处理（匹配 / 等帧）
        int64_t during = -1;
        f.prompt.onAsk = [&] {
            f.clock.ms += 20000 + 1000;   // 真人等待 + 同意后约 1 秒关闭动画，都在 Ask 段内
            during = static_cast<int64_t>(f.dl.RemainingMs());
        };
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "窗口确认：等人很久之后照常放行");
        Check(during == 4000, "弹框在世时剩余预算冻结：人工等待不烧 --timeout-ms");
        Check(f.dl.RemainingMs() == 4000, "等待后同意仍剩 4000 ms：能继续处理（本修复的要害）");
        f.clock.ms += 500;   // 恢复后的自动处理：取帧 / 编码 / 写盘
        Check(f.dl.RemainingMs() == 3500, "确认之后继续烧同一份预算");
        Check(f.dl.ElapsedMs() == 1500, "流逝汇报只算自动处理，不含 21000 ms 人工时间");
    }
    {
        // --yes 直通的那一级不进暂停、也不借机补时间；桌面一级真要弹框才暂停
        BudgetFixture f(true, TwoTargets(), 5000);
        f.prompt.onAsk = [&] { f.clock.ms += 21000; };
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "--yes：窗口路径直接放行");
        Check(f.prompt.asks == 0, "--yes：压根没弹框，也就没进暂停");
        Check(f.dl.RemainingMs() == 5000, "直通分支不给预算白添时间（分毫未动）");
        std::optional<DesktopPermit> permit;
        f.clock.ms += 800;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "--yes 之下桌面路径仍要问人");
        Check(f.prompt.asks == 1 && f.dl.RemainingMs() == 4200,
              "只有真等人的段暂停：烧掉 800 自动段，21000 人工段不烧");
    }
    {
        // 批次复用已给出的许可 = 不弹框 = 不暂停：已经烧穿的预算不会"歇一口气"
        BudgetFixture f(false, TwoTargets(), 5000);
        f.prompt.onAsk = [&] { f.clock.ms += 21000; };
        Diagnostic err;
        std::optional<DesktopPermit> p1;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &p1, &err),
              "第一次桌面确认给出许可");
        Check(f.dl.RemainingMs() == 5000, "第一次确认没烧自动预算");
        f.clock.ms += 5000;   // 后面的真实自动处理把预算烧尽
        std::optional<DesktopPermit> p2;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kB, kAreaB, &p2, &err),
              "第二个目标复用同一份许可（没有第二次弹框）");
        Check(f.prompt.asks == 1, "复用段不弹框");
        Check(f.dl.Spent(), "复用许可不白送一次暂停：烧穿的预算照旧烧穿");
    }
    {
        // 窗口确认后升级桌面、授权失效后再确认：每一次真人等待都各自暂停，
        // 而自动处理跨三次确认累计的份额一分不少地照烧。
        BudgetFixture f(false, TwoTargets(), 5000);
        f.prompt.onAsk = [&] { f.clock.ms += 21000; };
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "窗口那级先问");
        f.clock.ms += 300;
        Check(f.gate.AuthorizeDesktop(paths::kScreenWgc, kA, kAreaA, &permit, &err),
              "同一批次升级到桌面那级再问");
        f.clock.ms += 300;
        Check(f.dl.RemainingMs() == 4400, "两次确认之间只烧了自动段（600 ms）");
        g_screens.push_back(MakeScreen(L"\\\\.\\DISPLAY2", R(1920, 0, 3840, 1080), false));
        f.clock.ms += 200;
        std::optional<DesktopPermit> after;
        Check(f.gate.AuthorizeDesktop(paths::kScreenWgc, kA, kAreaA, &after, &err),
              "拓扑一变授权作废，重新问人");
        g_screens.pop_back();
        f.clock.ms += 100;
        Check(f.prompt.asks == 3 && f.dl.RemainingMs() == 4100,
              "三段人工等待全部排除，自动处理累计 900 ms 照烧");
    }
    {
        // 拒绝（含超时式拒绝）之前的人等同样不烧预算：暂停按的是"在世区间"，不问答案
        BudgetFixture f(false, TwoTargets(), 5000);
        f.prompt.answers = {ConsentAnswer::kTimedOut};
        f.prompt.onAsk = [&] { f.clock.ms += 60000; };
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "答\"否\"/超时：不放行");
        Check(CodeIs(err, codes::kConsentTimeout), "超时拒绝的诊断照旧（F04 不改拒绝语义）");
        Check(f.dl.RemainingMs() == 5000 && !f.dl.Spent(),
              "拒绝前的人等 60 秒也不烧自动预算");
    }
    {
        // 应答器抛异常：暂停作用域在退栈时恢复（RAII），不留欠账也不留下在世暂停
        BudgetFixture f(false, TwoTargets(), 5000);
        f.prompt.throwInAsk = true;
        f.prompt.onAsk = [&] { f.clock.ms += 15000; };
        Diagnostic err;
        bool threw = false;
        try {
            f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk);
        } catch (const std::exception&) {
            threw = true;
        }
        Check(threw, "应答器异常穿出判定器（不吞、不把半成品授权当同意）");
        Check(f.dl.RemainingMs() == 5000, "异常退栈：暂停照常恢复，人工段不烧预算");
        f.prompt.throwInAsk = false;
        f.clock.ms += 700;
        Check(f.dl.RemainingMs() == 4300, "异常恢复后预算继续正常计时（深度没有残留）");
        Diagnostic err2;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err2, EligibleYes(), VerifyOk), "异常之后的再一问照常进行");
        Check(f.dl.RemainingMs() == 4300, "第二次人工段同样不烧：嵌套/异常路径的账都结得干净");
    }

    // =========================================================================
    Section("11) 确认期间发生变化：展示什么，就批准什么（F06）");
    // =========================================================================
    // 这一段判的是同一段时间窗：授权快照在**弹框前**冻结，人点头（含关闭缓冲）之后判定器
    // 重新查询拓扑再与那份快照比。确认期间热插拔 / 拔线 / 改分辨率 / 挪位置 —— 一律停在
    // 签发之前：不授权、不采像素、不把"确认后第一次看到的新布局"追认成旧请求的基线，
    // 也不在同一次调用里对着新布局自动再弹一帧（重来一遍时会在新的快照上重新问）。
    const PhysRect kMovedArea = R(2000, 100, 2400, 400);

    // 正对照：确认期间什么都没变 —— 授权照发，凭证批准的就是框上展示的那一片。
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "确认期间拓扑未变：授权照常签发");
        Check(f.prompt.asks == 1 && permit.has_value() && permit->Covers(kAreaA),
              "问一次、有凭证、批准区域覆盖采样矩形");
    }
    // 确认期间插上第二块屏：这一问作废。
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        f.prompt.answers = {ConsentAnswer::kAccepted};   // 人答的是"旧布局那份"，必须不被沿用
        f.prompt.onAsk = [] {
            g_screens.push_back(MakeScreen(L"\\\\.\\DISPLAY2", R(1920, 0, 3840, 1080), false));
        };
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "Ask 期间多了一块屏：不签发授权（人点头时看到的布局已经不在了）");
        Check(!permit.has_value(), "作废的这一问没有凭证 —— 一像素都不该被采");
        Check(CodeIs(err, codes::kConsentStale), "下场是 capture.consent_stale（不是\"被拒绝\"）");
        Check(err.stage == stages::kConsent, "stage=consent：停在授权这一关");
        Check(!f.gate.Refused(), "基线失效不等于人不同意：整批不因它停止");
        // 重来一遍：在**新快照**上重新问，框上写的是变化后的事实而不是旧文字。
        f.prompt.onAsk = nullptr;
        f.prompt.answers = {};
        std::optional<DesktopPermit> again;
        Diagnostic err2;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &again, &err2) &&
                  f.prompt.asks == 2,
              "重新发起会在新拓扑上再问一次并签发（不无限重试：一次问只验一次基线）");
        Check(again.has_value() && again->Covers(kAreaA), "重问后的凭证覆盖展示过的那一片");
    }
    // 确认期间目标那块屏消失（拔掉），与拓扑查询一路答空（问不出来）：同为基线失效。
    {
        for (int mode = 0; mode < 2; ++mode) {
            g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};
            Fixture f(false, {Target(kA.c_str(), kAreaA)});
            f.prompt.onAsk = [mode] {
                if (mode == 0) g_screens.clear();   // 拔掉：一块都不剩
                else g_screens[0].bounds = R(0, 0, 1280, 720);   // 改了分辨率
            };
            Diagnostic err;
            std::optional<DesktopPermit> permit;
            Check(!f.gate.AuthorizeDesktop(mode == 0 ? paths::kScreenBitBlt
                                                     : paths::kScreenWgc,
                                           kA, kAreaA, &permit, &err) &&
                      CodeIs(err, codes::kConsentStale) && !permit.has_value(),
                      mode == 0 ? "Ask 期间显示器消失：consent_stale、无凭证"
                                : "Ask 期间分辨率变化：consent_stale、无凭证");
        }
    }
    // 确认期间拓扑问不出来（EnumScreens 失败 = 空表）：保守停在基线不符，而不是"没变"。
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        f.prompt.onAsk = [] { g_screens.clear(); };
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kScreenDuplication, kA, kAreaA, &permit, &err) &&
                  CodeIs(err, codes::kConsentStale),
              "拓扑这一问没答案时按\"与批准基线不符\"处理（问不出≠没变，fail-closed）");
    }
    // SetTargetArea 与框上文字同源刷新：给人看的坐标就是将要批准的坐标。
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 3840, 1080), true)};
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        f.gate.SetTargetArea(kA, kMovedArea);   // 流水线在弹框前重新量到的矩形
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kMovedArea, &permit, &err) &&
                  permit.has_value(),
              "矩形刷新后按新矩形问人、按新矩形签发");
        const std::wstring& line = f.prompt.asked[0].targetLines[0];
        Check(line.find(L"2000,100") != std::wstring::npos &&
                  line.find(L"400x300") != std::wstring::npos,
              "框上那行写的是刷新后的区域坐标（不再有\"矩形更新了、文字还是旧的\"）");
        Check(permit->Approved().left == kMovedArea.left &&
                  permit->Approved().top == kMovedArea.top &&
                  permit->Approved().right == kMovedArea.right &&
                  permit->Approved().bottom == kMovedArea.bottom,
              "凭证批准的区域与框上展示的完全相同（展示什么批准什么）");
        Check(!permit->Covers(kAreaA), "旧位置不再被这份凭证覆盖（新授权只批了新那一片）");
    }
    // 多目标批次：其中一个目标的拓扑变了，只有"变了之后仍未重新展示"的旧授权不能用；
    // 重新问的那一轮，清单里两块屏的区域都按当下事实渲染。
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true),
                     MakeScreen(L"\\\\.\\DISPLAY2", R(1920, 0, 3840, 1080), false)};
        const std::wstring kS1 = L"DISPLAY1";
        const std::wstring kS2 = L"DISPLAY2";
        Fixture f(false, {Target(kS1.c_str(), R(0, 0, 1920, 1080), true),
                          Target(kS2.c_str(), R(1920, 0, 3840, 1080), true)});
        Diagnostic err;
        std::optional<DesktopPermit> p1;
        Check(f.gate.AuthorizeDesktop(paths::kScreenWgc, kS1, R(0, 0, 1920, 1080), &p1, &err),
              "批次里第一块屏正常确认");
        // 第二块开工前流水线先重量（SetTargetArea = 弹框前的那次复核答复），再发起授权。
        g_screens[1].bounds = R(1920, 0, 2560, 1440);   // 确认之后、第二次授权之前有人改了布局
        f.gate.SetTargetArea(kS2, g_screens[1].bounds);
        std::optional<DesktopPermit> p2;
        Check(f.gate.AuthorizeDesktop(paths::kScreenWgc, kS2, g_screens[1].bounds, &p2, &err),
              "拓扑一变旧授权作废：第二块屏在新快照上重新问一次才放行");
        Check(f.prompt.asks == 2, "复用只发生在\"仍有效且已明确展示\"的授权上：变了就必再问");
        Check(f.prompt.asked[1].targetLines[1].find(L"640x1440") != std::wstring::npos,
              "重问那一轮清单里写的是变化后的区域，不是旧文字");
        Check(p2.has_value() && p2->Covers(g_screens[1].bounds) &&
                  !p2->Covers(R(1920, 0, 3840, 1080)),
              "新凭证只覆盖新展示过的那一片，旧区域反而不再被批准");
    }
    // 后端回退与 DWM 内部升级走的是同一台判定器：不给特殊通道留旁路。
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk), "窗口内容那一级先确认（--yes 未给）");
        f.prompt.onAsk = [] { g_screens.clear(); };
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kDwmScreen, kA, kAreaA, &permit, &err) &&
                  CodeIs(err, codes::kConsentStale),
              "DWM 升级到桌面像素：同一条确认后复核，确认期间拓扑变了照样停");
        Check(!permit.has_value(), "升级路径也没有凭证可用");
    }


    // =========================================================================
    Section("12) 共用状态机：界面返回 ≠ 授权成立（F08）");
    // =========================================================================
    {
        ConsentStateMachine m;
        Check(m.state() == ConsentState::kNotRequested, "初始 = not_requested（既没许可也没拒绝）");
        Check(!m.granted() && !m.blocks(), "初始既不是许可也不是拒绝");
        m.Resolve(ConsentEvent::kAccepted);
        Check(m.state() == ConsentState::kNotRequested, "没发起请求时一个 accepted 不会凭空造出许可");
        m.Request();
        Check(m.state() == ConsentState::kWaiting, "Request -> waiting");
        m.Resolve(ConsentEvent::kAccepted);
        Check(m.state() == ConsentState::kWaiting && m.accepted_pending(),
              "人答了是：仍是 waiting（界面返回 ≠ 授权成立）");
        Check(!m.granted(), "未过绑定核实之前不授予");
        m.Resolve(ConsentEvent::kVerified);
        Check(m.granted() && m.basis() == ConsentBasis::kHumanAccepted,
              "核实通过才 granted，basis=human_accepted");
        m.Resolve(ConsentEvent::kInvalidated);
        Check(m.state() == ConsentState::kStale && !m.granted(), "绑定失效 -> stale（不再是许可）");
        Check(!m.blocks(), "stale 不停掉整批：允许带新事实重新确认");
        Check(m.can_request(), "stale 可以重新发起");
        m.Request();
        Check(m.waiting(), "stale -> waiting");
        m.Resolve(ConsentEvent::kTimedOut);
        Check(m.state() == ConsentState::kTimedOut && m.blocks(), "超时 -> timed_out 并停批");
        m.Request();
        Check(m.state() == ConsentState::kTimedOut, "超时之后 Request 不能把它洗成新的等待");
        m.Reset();
        Check(m.state() == ConsentState::kNotRequested, "Reset 只给全新请求 / 测试");

        ConsentStateMachine a;
        a.Request();
        a.Resolve(ConsentEvent::kDeclined);
        Check(a.state() == ConsentState::kDenied && a.blocks(), "declined -> denied");
        ConsentStateMachine b;
        b.Request();
        b.Resolve(ConsentEvent::kClosed);
        Check(b.state() == ConsentState::kDenied, "closed 归入 denied（理由另记）");
        ConsentStateMachine c;
        c.Request();
        c.Resolve(ConsentEvent::kCancelled);
        Check(c.state() == ConsentState::kCancelled && c.blocks(), "cancelled -> cancelled");
        ConsentStateMachine d;
        d.Request();
        d.Resolve(ConsentEvent::kFailed);
        Check(d.state() == ConsentState::kError && d.blocks(), "机制失败 -> error");
        ConsentStateMachine e;
        e.GrantWithoutHuman(ConsentBasis::kNone);
        Check(!e.granted(), "GrantWithoutHuman 不接受 kNone（防止忘了交事实就默认放行）");
        e.GrantWithoutHuman(ConsentBasis::kYesWindowContent);
        Check(e.granted() && e.basis() == ConsentBasis::kYesWindowContent,
              "只有 yes_window_content 这一种非人来源能授权");
        Check(std::string(ConsentStateName(ConsentState::kTimedOut)) == "timed_out",
              "状态机器名稳定");
    }
    // 两层授权合并判据：程序确认不能冒充系统授权
    {
        const uint32_t programOnly = kGrantProgramConfirm;
        const uint32_t both = kGrantProgramConfirm | kGrantSystemAuthorization;
        Check(ConsentLayersSatisfy(programOnly, ConsentState::kGranted,
                                   SystemAuthState::kNotRequired),
              "只要程序层时：granted 就满足");
        Check(!ConsentLayersSatisfy(programOnly, ConsentState::kWaiting,
                                    SystemAuthState::kGranted),
              "程序层还在等待时不算授权");
        Check(!ConsentLayersSatisfy(both, ConsentState::kGranted, SystemAuthState::kNotRequired),
              "要求两层时：只有程序确认不算");
        Check(ConsentLayersSatisfy(both, ConsentState::kGranted, SystemAuthState::kGranted),
              "两层都满足才算");
        Check(!ConsentLayersSatisfy(both, ConsentState::kGranted, SystemAuthState::kNotRequested),
              "系统层还没取得（not_requested）也不算");
        Check((RequiredGrantLayers(PlatformKind::kLinuxWayland) & kGrantSystemAuthorization) != 0,
              "Wayland 矩阵要求系统授权层");
        Check((RequiredGrantLayers(PlatformKind::kWindows) & kGrantSystemAuthorization) == 0,
              "Windows 没有每次调用的系统授权层");
        Check(DefaultRequiredGrantLayers() == RequiredGrantLayers(BuildPlatform()),
              "GateConfig 默认要求层来自平台族");
        Check(YesGivenExempts(PixelScope::kWindowContent, true), "--yes 只免程序确认这一层");
    }

    // =========================================================================
    Section("13) --yes 的资格：能力与目标都核实过，才可能跳过程序确认（F08）");
    // =========================================================================
    {
        Fixture f(true, TwoTargets());
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk),
              "--yes + 三件事实齐：直接放行");
        Check(f.prompt.asks == 0, "资格成立时不弹框");
        Check(f.gate.WindowState() == ConsentState::kGranted &&
                  f.gate.WindowConsentBasis() == ConsentBasis::kYesWindowContent,
              "状态机记的依据是 yes_window_content");
        Check(f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err, EligibleYes(), VerifyOk),
              "同批第二个窗口复用这份例外");
        Check(f.prompt.asks == 0, "复用也不弹框");
    }
    {
        struct Case {
            const char* what;
            YesEligibility facts;
        } cases[] = {
            {"能力未确认", YesEligibility{true, false, true}},
            {"身份未复核", YesEligibility{true, true, false}},
            {"不是单窗口目标", YesEligibility{false, true, true}},
        };
        for (const Case& c : cases) {
            Fixture f(true, TwoTargets());
            Diagnostic err;
            const std::string what(c.what);
            Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, c.facts, VerifyOk),
                  (what + "：退回正常确认（人答是之后放行）").c_str());
            Check(f.prompt.asks == 1, (what + "：确实问了人").c_str());
            Check(f.gate.WindowConsentBasis() == ConsentBasis::kHumanAccepted,
                  (what + "：依据记为真人同意").c_str());
        }
    }
    {
        Fixture f(true, TwoTargets());
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(),
                                      VerifyTargetChanged),
              "--yes 资格齐但授权后身份变了：不签发");
        Check(f.gate.WindowState() == ConsentState::kStale, "状态是 stale（不是 granted）");
        Check(!f.gate.Refused(), "stale 不停整批");
        Check(CodeIs(err, codes::kTargetChanged), "保留绑定核实给出的 target_changed");
    }
    {
        Fixture f(true, TwoTargets());
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kBitBltScreen, kA, &err, EligibleYes(), VerifyOk),
              "拿窗口入口放行桌面路径：拒绝");
        Check(f.gate.WindowState() == ConsentState::kError && f.gate.Refused(),
              "入口越界按机制错误停批（fail-closed）");
    }

    // =========================================================================
    Section("14) 人同意之后、签发之前的绑定核实");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kAccepted};
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, {}, VerifyTargetChanged),
              "人答了是但目标变了：不签发");
        Check(f.gate.WindowState() == ConsentState::kStale, "状态是 stale");
        Check(!f.gate.Refused(), "stale 不停整批");
        Check(f.prompt.asks == 1, "只问了一次");
        f.prompt.answers = {};
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, {}, VerifyOk),
              "重新确认并在新的绑定上核实后签发");
        Check(f.prompt.asks == 2, "重新确认要求再问一次");
    }
    {
        Fixture f(false, TwoTargets());
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, {}, {}),
              "没有绑定核实手段时：即使人答是也不签发");
        Check(f.gate.WindowState() == ConsentState::kStale, "按 stale 收尾");
    }
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        f.prompt.onAsk = [] {
            g_screens.push_back(MakeScreen(L"\\\\.\\DISPLAY2", R(1920, 0, 3840, 1080), false));
        };
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err),
              "人答是之后拓扑变了：不签发");
        Check(f.gate.DesktopState() == ConsentState::kStale, "DesktopState = stale");
        Check(!f.gate.Refused(), "stale 不停批");
        g_screens.pop_back();
        f.prompt.onAsk = nullptr;
        std::optional<DesktopPermit> again;
        Check(f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &again, &err),
              "重新确认后放行");
    }

    // =========================================================================
    Section("15) 批量授权范围：窗口级也只覆盖清单里列出的目标");
    // =========================================================================
    {
        Fixture f(false, {Target(kA.c_str(), kAreaA)});
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, {}, VerifyOk),
              "清单里的目标确认一次");
        Check(f.prompt.asks == 1, "弹了一次");
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, {}, VerifyOk),
              "同目标复用该次确认");
        Check(f.prompt.asks == 1, "复用不弹框");
        Check(!f.gate.AuthorizeWindow(paths::kWgc, L"0xCCCC", &err, {}, VerifyOk),
              "清单外目标不会沿用先前那句是");
        Check(f.prompt.asks == 2, "清单外目标触发重新确认");
        Check(CodeIs(err, codes::kAccessDenied), "重新确认仍覆盖不到 -> access_denied");
        Diagnostic err2;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err2, {}, VerifyOk),
              "清单里原来的目标不受影响");
    }

    // =========================================================================
    Section("16) 系统授权层预留（Portal 接入前 fail-closed）");
    // =========================================================================
    {
        const uint32_t both = kGrantProgramConfirm | kGrantSystemAuthorization;
        Fixture f(true, TwoTargets(), both);
        Diagnostic err;
        Check(f.gate.SystemAuthorizationRequired(), "配置要求系统授权层");
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk),
              "只有程序确认 + --yes：不授权");
        Check(f.prompt.asks == 0,
              "系统层缺失时连程序确认都不当成完整授权（不弹一个会误导人的框）");
        Check(CodeIs(err, codes::kConsentUnavailable), "码是 capture.consent_unavailable");
        Check(f.gate.SystemState() == SystemAuthState::kUnavailable, "系统层状态 = unavailable");
        Check(f.gate.Refused(), "系统层不可用会停掉整批");
        std::optional<DesktopPermit> permit;
        Diagnostic err2;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err2),
              "桌面路径同样不授权");
        Check(f.prompt.asks == 0, "连桌面确认也不弹（缺的是系统层，不是人的回答）");
    }
    {
        Fixture f(true, TwoTargets(), kGrantProgramConfirm);
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err, EligibleYes(), VerifyOk),
              "只要程序层的平台不受影响");
        Check(f.gate.SystemState() == SystemAuthState::kNotRequired,
              "Windows / X11：系统层 = not_required");
    }

    // =========================================================================
    Section("17) 弹框内容：可信 / 不可信分开 + 外部文本安全展示");
    // =========================================================================
    {
        using namespace display_text;
        const std::wstring evil = L"安全标题\n伪造：已获得授权\u202Eevil\u200B";
        Check(ContainsUnsafeCodePoint(evil), "夹具本身确实带换行 / 双向 / 零宽");
        const std::wstring safe = SanitizeDisplayLine(evil, 200);
        Check(!ContainsUnsafeCodePoint(safe), "净化后不再含不安全码位");
        Check(safe.find(L'\n') == std::wstring::npos, "换行被换成替换符，不能伪造额外行");
        Check(safe.find(L'\u202E') == std::wstring::npos &&
                  safe.find(L'\u200B') == std::wstring::npos,
              "双向 / 零宽控制符被去掉");
        const std::wstring emoji = L"\U0001F600\U0001F600";
        const std::wstring cut = SanitizeDisplayLine(emoji, 1);
        Check(CodePointCount(cut) == 2 && cut.back() == kEllipsis &&
                  cut.find(L'\uFFFD') == std::wstring::npos,
              "按码位截断：代理对不被劈开，追加省略号");
        const std::wstring many = std::wstring(5000, L'x');
        const std::wstring capped = SanitizeDisplayLine(many, 50);
        Check(CodePointCount(capped) == 51 && capped.back() == kEllipsis,
              "超长文本按上限截断并标出省略号");
        const std::wstring block = L"第一行\r\n第二行\n第三行\n第四行\n第五行\n第六行\n第七行";
        const std::wstring cappedBlock = SanitizeDisplayBlock(block, 4, 20);
        Check(cappedBlock.find(L"第五行") == std::wstring::npos && !cappedBlock.empty(),
              "多行有行数上限，超出部分不展示");
    }
    {
        ConsentQuestion q;
        q.scope = PixelScope::kWindowContent;
        q.path = paths::kWgc;
        q.captureLabel = L"auto";
        q.programPath = L"C:\\Program Files\\EvernightCapture\\ECAPTURE.EXE";
        q.programVersion = L"1.0.0";
        q.programPid = 4242;
        q.requestSource = L"调用方自述第一行\n伪造：允许读取整块屏幕\u202E";
        q.requestSourceGiven = true;
        q.targets = {std::wstring(L"标题安全\n伪造目标 (area 0,0 size 100x100)")};
        q.outputs = {std::wstring(L"D:\\shots\\evil\n伪造.txt")};
        q.historyCopy = true;
        const GuiDialogRequest req = BuildConsentDialogRequest(q, 5000);
        std::wstring all;
        all += req.title + req.heading + req.detailsLabel + req.details + req.footer +
               req.question + req.acceptLabel + req.declineLabel;
        for (const auto& para : req.paragraphs) all += para;
        // details 里我们自己拼的结构性换行是允许的；其余任何控制 / 双向 / 零宽字符都不允许。
        std::wstring flat = all;
        flat.erase(std::remove(flat.begin(), flat.end(), L'\n'), flat.end());
        Check(!display_text::ContainsUnsafeCodePoint(flat),
              "整份弹框内容（除结构性换行）不含控制 / 双向 / 零宽字符");
        Check(req.details.find(L"伪造目标") != std::wstring::npos,
              "目标那一段仍然展示（只是被安全化）");
        Check(req.details.find(L"标题安全\n") == std::wstring::npos, "标题里的换行不生效");
        Check(req.details.find(L'\uFFFD') != std::wstring::npos, "被替换的字符以 U+FFFD 明示");
        bool trusted = false;
        bool untrusted = false;
        for (const auto& para : req.paragraphs) {
            if (para.find(L"ECAPTURE.EXE") != std::wstring::npos &&
                para.find(L"4242") != std::wstring::npos &&
                para.find(L"read by this program itself") != std::wstring::npos) {
                trusted = true;
            }
            if (para.find(L"Caller-provided description") != std::wstring::npos) untrusted = true;
        }
        Check(trusted, "程序核实的事实单独一段（映象 / 版本 / 进程号）");
        Check(untrusted, "调用方自述带未核实标签");
        {
            ConsentQuestion empty = q;
            empty.requestSource.clear();
            empty.requestSourceGiven = false;
            const GuiDialogRequest r2 = BuildConsentDialogRequest(empty, 0);
            bool none = false;
            for (const auto& para : r2.paragraphs) {
                if (para.find(L"did not provide a description") != std::wstring::npos) none = true;
            }
            Check(none, "没有自述时明说没有，不空着也不冒充");
            Check(r2.footer.empty(), "没有 --consent-timeout-ms 时不写期限行");
        }
        {
            ConsentQuestion longq = q;
            longq.requestSource = std::wstring(5000, L'y');
            longq.targets = {std::wstring(5000, L'z')};
            const GuiDialogRequest r3 = BuildConsentDialogRequest(longq, 0);
            size_t zc = 0;
            for (wchar_t ch : r3.details) {
                if (ch == L'z') ++zc;
            }
            Check(zc <= 200, "超长目标行按码位上限截断");
            Check(r3.details.find(display_text::kEllipsis) != std::wstring::npos,
                  "截断有省略号");
        }
    }

    // =========================================================================
    Section("18) 拒绝 / 超时 / 机制失败在状态机上粘住整批");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kTimedOut};
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, {}, VerifyOk), "超时不放行");
        Check(f.gate.WindowState() == ConsentState::kTimedOut, "状态 = timed_out");
        Check(f.gate.Refused(), "超时停批");
        const int asks = f.prompt.asks;
        Diagnostic err2;
        Check(!f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err2, {}, VerifyOk),
              "同批下一个目标不再截");
        Check(f.prompt.asks == asks, "不再弹框");
        Check(CodeIs(err2, codes::kConsentTimeout), "粘住的是超时码");
    }
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kInitFailed};
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err, {}, VerifyOk),
              "GUI 初始化失败不放行");
        Check(f.gate.WindowState() == ConsentState::kError, "状态 = error");
        Check(CodeIs(err, codes::kConsentUnavailable), "码 = consent_unavailable");
        Check(f.gate.Refused(), "机制失败也停批");
    }

    // =========================================================================
    Section("19) X11 根窗口桌面路径：--yes 仍不能跳过人工确认");
    // =========================================================================
    {
        g_screens = {MakeScreen(L"\\\\.\\DISPLAY1", R(0, 0, 1920, 1080), true)};
        const PhysRect desktop = R(0, 0, 1920, 1080);
        Fixture f(true, {Target(L"desktop", desktop, true)});
        std::optional<DesktopPermit> permit;
        Diagnostic err;
        Check(f.gate.AuthorizeDesktop(paths::kX11Desktop, L"desktop", desktop, &permit, &err),
              "x11.desktop 的桌面目标在人工确认后签发凭证");
        Check(f.prompt.asks == 1, "--yes 为真时 x11.desktop 仍然弹了一次确认框");
        Check(f.prompt.asked.size() == 1 &&
                  f.prompt.asked.front().scope == PixelScope::kDesktop &&
                  f.prompt.asked.front().path == paths::kX11Desktop,
              "确认框记录的是 desktop scope 与 x11.desktop 内部路径");
        Check(permit.has_value() && permit->Covers(desktop), "凭证只覆盖批准的全桌面范围");
        Check(f.gate.DesktopState() == ConsentState::kGranted, "桌面确认状态 = granted");
        Check(!f.gate.AuthorizeDesktop(paths::kX11Desktop, L"desktop",
                                       R(0, 0, 2000, 1080), &permit, &err),
              "超出批准范围的区域即使同一个目标也不放行");
    }

    // =========================================================================
    std::printf("\nconsent-state: %d 条通过，%d 条失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
