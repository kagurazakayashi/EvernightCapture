// 截图授权状态机的离线判据（由 tests\consent.ps1 编译并运行）。
//
// 为什么在这一层测：授权这件事的分支（升级要重新确认、拒绝之后不再换后端也不再问第二遍、
// 目标挪位置或换屏幕拓扑就作废凭证、--yes 只对窗口内容路径生效……）在真机上每一条都要人
// 去点一次确认框，而发布版里不许留任何"测试旁路"。所以这里注入假的应答器与假的屏幕布局，
// 把 ConsentGate 与路径分类整台跑完；真机那部分判据留在 tests\consent.ps1 与 window_shot.bat。
//
// 这个可执行文件不含任何取帧代码：它只链接纯逻辑那几份源文件（见 CMakeLists.txt）。

#include <atomic>
#include <climits>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/CaptureScope.h"
#include "../src/CliOptions.h"
#include "../src/Consent.h"
#include "../src/Deadline.h"
#include "../src/Lang.h"
#include "../src/ScreenMatch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

RECT R(int l, int t, int r, int b) { return RECT{l, t, r, b}; }

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
        // "到点没人答"与"人答了否"同为拒绝，但要能分开带出去（timedOuts 按次给）
        if (reply.answer == ConsentAnswer::kDeclined && n <= timedOuts.size()) {
            reply.timedOut = timedOuts[n - 1];
        }
        reply.win32 = unavailableWin32;
        return reply;
    }

    int asks = 0;
    std::vector<Asked> asked;
    std::vector<ConsentAnswer> answers;   // 空 = 一律答"是"
    std::vector<bool> timedOuts;
    DWORD unavailableWin32 = 0;
    std::function<void()> onAsk;          // 弹框在世时执行：推进假时钟 / 探测预算冻结
    bool throwInAsk = false;              // 应答器抛异常：判暂停作用域的异常退栈恢复
};

// ---------------------------------------------------------------------------
// 假屏幕拓扑：判据要能自己改布局，看看已给出的授权还作不作数
// ---------------------------------------------------------------------------

std::vector<ScreenInfo> g_screens;

std::vector<ScreenInfo> FakeTopology() { return g_screens; }

ScreenInfo MakeScreen(const wchar_t* device, RECT bounds, bool primary) {
    ScreenInfo s;
    s.deviceName = device;
    s.bounds = bounds;
    s.primary = primary;
    s.ordinal = 1;
    return s;
}

GateTarget Target(const wchar_t* key, RECT area, bool screen = false) {
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

    Fixture(bool yes, std::vector<GateTarget> targets)
        : gate(Config(yes, std::move(targets)), prompt, FakeTopology) {}
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

const RECT kAreaA = R(100, 100, 500, 400);
const RECT kAreaB = R(600, 0, 900, 300);
const std::wstring kA = L"0xAAAA";
const std::wstring kB = L"0xBBBB";

std::vector<GateTarget> TwoTargets() {
    return {Target(kA.c_str(), kAreaA), Target(kB.c_str(), kAreaB)};
}

bool CodeIs(const Diagnostic& d, const wchar_t* code) { return d.code == code; }

// ---------------------------------------------------------------------------
// 假弹框驱动：不弹真框，把生产的生命周期状态机（RunMonitoredDialog）整台跑完。
//
// 弹框线程由状态机自己创建，这个驱动只控制三件事：框多晚出现（appearAfterMs）、
// 关它的话第几次才生效（honorCloseAfter）、生效后回哪个按钮（answer）。判的是生产
// 那份代码，不是它独立抄一遍的副本。驱动只存在于本测试翻译单元 —— 发布版
// ECAPTURE.EXE 里没有任何入口能选到它（没有运行时开关，能选的只有编译进谁的身体），
// 所以它不构成"accepted 后门"：连 ConsentGate 都到不了的那层假象都没有。
// ---------------------------------------------------------------------------

class FakeDialog final : public DialogDriver {
public:
    // —— 剧本 ——
    int answer = IDNO;           // Show 返回的按钮值
    DWORD failWin32 = 0;         // 非 0 => "没弹成"（MessageBoxW 返回 0 的同一约定）
    bool throwInShow = false;    // 驱动崩了：按"不可用"判，异常不许跑出线程
    int appearAfterMs = 0;       // 窗口 Show 开始这么久之后才找得到（弹框创建延迟）
    int honorCloseAfter = 0;     // 第 N 次有效关框请求之后框才真关；INT_MAX = 永不关
    std::atomic<bool> releaseNow{false};   // 测试线程手动放行（立即应答 / 放弃后观察）

    // —— 可观察 ——
    std::atomic<int> closeSlices{0};       // RequestClose 被调了几次（含还没窗口的）
    std::atomic<int> effectiveCloses{0};   // 其中"窗口已在、指令真的送出去了"的次数
    std::atomic<bool> showEntered{false};
    std::atomic<bool> showReturned{false};
    std::atomic<DWORD> seenThreadId{0};

    DialogResult Show() override {
        showStart_ = GetTickCount64();
        showEntered.store(true);
        while (!released()) Sleep(10);
        showReturned.store(true);
        if (throwInShow) throw std::runtime_error("fake driver blew up");
        DialogResult r;
        if (failWin32 != 0) {
            r.code = 0;
            r.win32 = failWin32;
        } else {
            r.code = answer;
        }
        return r;
    }

    bool RequestClose(DWORD dialogThreadId) override {
        closeSlices.fetch_add(1);
        if (dialogThreadId != 0) seenThreadId.store(dialogThreadId);
        if (GetTickCount64() - showStart_ < ULONGLONG(appearAfterMs)) return false;
        effectiveCloses.fetch_add(1);
        return true;
    }

    bool WaitShowReturned() {
        for (int i = 0; i < 200; ++i) {
            if (showReturned.load()) return true;
            Sleep(10);
        }
        return false;
    }

private:
    bool released() {
        if (releaseNow.load()) return true;
        return honorCloseAfter > 0 && honorCloseAfter != INT_MAX &&
               effectiveCloses.load() >= honorCloseAfter;
    }

    ULONGLONG showStart_ = 0;
};

struct Tick {
    ULONGLONG t0 = GetTickCount64();
    ULONGLONG Ms() const { return GetTickCount64() - t0; }
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
    Check(ScopeOf(L"brand-new-channel") == PixelScope::kDesktop, "没登记过的路径默认按桌面处理");
    Check(ScopeOf(L"") == PixelScope::kDesktop, "空名字默认按桌面处理");
    Check(ScopeOf(static_cast<const wchar_t*>(nullptr)) == PixelScope::kDesktop,
          "nullptr 默认按桌面处理（漏登记不会变松）");
    Check(NeedsHumanConsent(paths::kBitBltScreen, true), "--yes 之下桌面路径仍要确认");
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
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "带 --yes：窗口内容路径放行");
        Check(f.prompt.asks == 0, "带 --yes：一次框都没弹");
    }
    {
        Fixture f(false, TwoTargets());
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "不带 --yes：第一次问人之后放行");
        Check(f.prompt.asks == 1, "不带 --yes：弹了一次");
        Check(f.prompt.asked[0].scope == PixelScope::kWindowContent, "问的是窗口内容这一级");
        Check(f.prompt.asked[0].targets == 2, "把整批两个目标都列给人看了");
        Check(f.prompt.asked[0].outputs == 2, "把整批输出名都列给人看了");
        Check(!f.prompt.asked[0].yesGiven, "这一轮没有 --yes");
        Check(f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err),
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
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "答\"否\"就不放行");
        Check(CodeIs(err, codes::kAccessDenied), "拒绝的码是 capture.access_denied");
        Check(err.stage == stages::kConsent, "拒绝的 stage 是 consent");
        Check(err.target == kA, "拒绝带上是哪个目标");
        Check(err.value == paths::kWgc, "拒绝带上是哪条路径（value 里是路径名）");
        Check(err.backend.empty(), "通道名由流水线补，判定器不猜");
        Check(f.gate.Refused(), "判定器记住\"已经被人拒过\"");
        const int before = f.prompt.asks;
        Diagnostic err2;
        Check(!f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err2),
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
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "答\"否\"= 关框，都不放行");
    }

    // =========================================================================
    Section("4) 没有交互桌面：与\"人答了否\"分开给码，且绝不自动继续");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kUnavailable};
        f.prompt.unavailableWin32 = ERROR_NOT_READY;
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "弹不出框就不放行");
        Check(CodeIs(err, codes::kConsentUnavailable),
              "弹不出框给的是 capture.consent_unavailable，不是\"用户拒绝\"");
        Check(err.win32 == ERROR_NOT_READY, "把\"为什么弹不出来\"的 Win32 码带出去");
        Check(err.stage == stages::kConsent, "stage 仍是 consent");
        Check(f.gate.Refused(), "弹不出框也停止本次剩下的采集");
    }
    {
        // 同一种情况下，带 --yes 的窗口内容路径本来就不需要桌面，所以完全不受影响
        Fixture f(true, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kUnavailable};
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kDwmThumbnail, kA, &err),
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
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "不带 --yes：先确认窗口内容");
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
        f.gate.AuthorizeWindow(paths::kWgc, kA, &err);
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
    Section("8) 弹框生命周期：任何超时路径都可收尾（假驱动跑生产状态机）");
    // =========================================================================
    {
        // 答"是"：同意，而且关闭动画缓冲一分不少（它是对用户的承诺，不是可省的重负）
        auto d = std::make_shared<FakeDialog>();
        d->answer = IDYES;
        d->releaseNow = true;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kAccepted && !r.timedOut, "答\"是\" => kAccepted");
        Check(t.Ms() >= 950, "同意路径保留约 1 秒关闭动画缓冲（不被任何超时逻辑削减）");
        Check(d->WaitShowReturned(), "弹框线程正常收尾");
    }
    {
        // 设了期限但人在期限前答了"是"：缓冲照留 —— 有 --consent-timeout-ms 不等于抄近路
        auto d = std::make_shared<FakeDialog>();
        d->answer = IDYES;
        d->releaseNow = true;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 30000);
        Check(r.answer == ConsentAnswer::kAccepted && t.Ms() >= 950,
              "带期限的提前同意：accepted 且缓冲完整");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->answer = IDNO;
        d->releaseNow = true;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kDeclined && !r.timedOut, "答\"否\" => kDeclined");
        Check(t.Ms() < 900, "非同意路径不付关闭动画缓冲");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->answer = IDCANCEL;   // 人自己按 X 关框
        d->releaseNow = true;
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kDeclined, "人关框 = 不同意");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->failWin32 = 42;      // Show 立刻带回"没弹成"
        d->releaseNow = true;
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kUnavailable && r.win32 == 42,
              "弹不出框 => kUnavailable 并带出原 Win32 码");
    }
    {
        auto d = std::make_shared<FakeDialog>();
        d->throwInShow = true;
        d->releaseNow = true;
        const ConsentReply r = RunMonitoredDialog(d, 0);
        Check(r.answer == ConsentAnswer::kUnavailable,
              "驱动抛异常 => 不可用（绝不拖崩进程、也绝不算同意）");
        Check(d->WaitShowReturned(), "异常线程照样收尾");
    }
    {
        // 到点没人答、第一次关框就生效：拒绝 + timedOut
        auto d = std::make_shared<FakeDialog>();
        d->answer = IDNO;
        d->honorCloseAfter = 1;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kDeclined && r.timedOut, "到点关框 => 拒绝 + timedOut");
        Check(t.Ms() >= 300 && t.Ms() < 3500, "有界返回（期限 + 宽限之内）");
        Check(d->seenThreadId.load() != 0, "关框请求拿到了弹框线程 id");
        Check(d->WaitShowReturned(), "关掉的线程被收工，没有抛弃");
    }
    {
        // 本机实测场景：头几次关框消息被无视（WM_CLOSE 处理了但不关）——靠重试救回
        auto d = std::make_shared<FakeDialog>();
        d->honorCloseAfter = 10;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kDeclined && r.timedOut, "关框晚生效仍是拒绝 + timedOut");
        Check(d->effectiveCloses.load() >= 10, "宽限期内逐切片重试，第一条消息没生效不是死路");
        Check(t.Ms() < 300 + 3000 + 500, "晚生效也没撞上放弃线");
    }
    {
        // 框弹得晚：期限到时还没有窗口可关，后来的切片把它补上
        auto d = std::make_shared<FakeDialog>();
        d->appearAfterMs = 900;
        d->honorCloseAfter = 1;
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kDeclined && r.timedOut, "弹框延迟后到点仍按拒绝收尾");
        Check(d->effectiveCloses.load() >= 1, "窗口迟到后关框请求补送成功");
        Check(d->closeSlices.load() >= d->effectiveCloses.load(), "迟到的请求如实计了数");
    }
    {
        // 极端：怎么都关不掉。放弃线必须兑现，而且放弃之后线程回来写结果不落空。
        auto d = std::make_shared<FakeDialog>();
        d->honorCloseAfter = INT_MAX;   // 永不因关框请求而回
        d->answer = IDYES;              // 之后"人"才点了是：答案已经交不回同意了
        Tick t;
        const ConsentReply r = RunMonitoredDialog(d, 200);
        Check(r.answer == ConsentAnswer::kDeclined && r.timedOut,
              "关不掉的框按到点拒绝收尾，绝不无限等");
        Check(t.Ms() >= 200 + 3000 && t.Ms() < 200 + 3000 + 1000, "放弃线：期限 + 宽限 + 一个切片");
        Check(!d->showReturned.load(), "放弃时线程确实还在弹框里（没有 TerminateThread）");
        d->releaseNow = true;           // 线程之后才返回：往共享状态里写，谁都没悬空
        Check(d->WaitShowReturned(), "被放弃的线程自己安全收尾（共享生命周期覆盖到它归还）");
        Check(r.answer == ConsentAnswer::kDeclined,
              "线程迟到带回\"是\"也改不了已交出的拒绝（没人再消费它）");
    }
    {
        // 竞争：期限到点的同一个瞬间"是"才落地 —— 保守优先级：超时赢
        auto d = std::make_shared<FakeDialog>();
        d->answer = IDYES;
        d->honorCloseAfter = 1;
        const ConsentReply r = RunMonitoredDialog(d, 300);
        Check(r.answer == ConsentAnswer::kDeclined && r.timedOut,
              "超时与同意同刻发生：按拒绝 + timedOut，绝不改成同意");
    }
    {
        // 竞争的另一侧的干净判据：Show 在期限之前带回 code 0 => 报不可用（约定见 Consent.h）
        auto d = std::make_shared<FakeDialog>();
        d->failWin32 = 5;
        d->releaseNow = true;           // Show 立刻就回，期限完全没参与
        const ConsentReply r = RunMonitoredDialog(d, 100000);
        Check(r.answer == ConsentAnswer::kUnavailable && r.win32 == 5,
              "期限之前弹框就失败：kUnavailable 带原码，不记成超时");
    }

    // =========================================================================
    Section("9) \"到点没人答\"在授权状态机里：独立诊断码 + 粘住整批");
    // =========================================================================
    {
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kDeclined};
        f.prompt.timedOuts = {true};
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "超时不放行");
        Check(CodeIs(err, codes::kConsentTimeout), "超时的码是 capture.consent_timeout（≠答否）");
        Check(err.stage == stages::kConsent, "stage 仍是 consent");
        Check(f.gate.Refused(), "超时同样停止本次剩下的采集");
        Diagnostic err2;
        Check(!f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err2), "同批第二个目标不再截");
        Check(CodeIs(err2, codes::kConsentTimeout), "粘住的诊断保持超时码，不被改写成\"被拒绝\"");
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err2) &&
                  CodeIs(err2, codes::kConsentTimeout),
              "桌面路径撞同一堵墙：也保持超时码");
    }
    {
        // 窗口那级批了、桌面那级到点没人答：之后一切拒绝都按桌面超时粘住
        Fixture f(false, TwoTargets());
        f.prompt.answers = {ConsentAnswer::kAccepted, ConsentAnswer::kDeclined};
        f.prompt.timedOuts = {false, true};
        Diagnostic err;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "窗口那级先批");
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kScreenWgc, kA, kAreaA, &permit, &err),
              "桌面那级超时拒绝");
        Check(CodeIs(err, codes::kConsentTimeout), "桌面超时的码正确");
        Check(!f.gate.AuthorizeWindow(paths::kPrintWindow, kB, &err), "跨级也要停");
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
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "窗口确认：等人很久之后照常放行");
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
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "--yes：窗口路径直接放行");
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
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "窗口那级先问");
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
        f.prompt.answers = {ConsentAnswer::kDeclined};
        f.prompt.timedOuts = {true};
        f.prompt.onAsk = [&] { f.clock.ms += 60000; };
        Diagnostic err;
        Check(!f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "答\"否\"/超时：不放行");
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
            f.gate.AuthorizeWindow(paths::kWgc, kA, &err);
        } catch (const std::exception&) {
            threw = true;
        }
        Check(threw, "应答器异常穿出判定器（不吞、不把半成品授权当同意）");
        Check(f.dl.RemainingMs() == 5000, "异常退栈：暂停照常恢复，人工段不烧预算");
        f.prompt.throwInAsk = false;
        f.clock.ms += 700;
        Check(f.dl.RemainingMs() == 4300, "异常恢复后预算继续正常计时（深度没有残留）");
        Diagnostic err2;
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err2), "异常之后的再一问照常进行");
        Check(f.dl.RemainingMs() == 4300, "第二次人工段同样不烧：嵌套/异常路径的账都结得干净");
    }

    // =========================================================================
    Section("11) 确认期间发生变化：展示什么，就批准什么（F06）");
    // =========================================================================
    // 这一段判的是同一段时间窗：授权快照在**弹框前**冻结，人点头（含关闭缓冲）之后判定器
    // 重新查询拓扑再与那份快照比。确认期间热插拔 / 拔线 / 改分辨率 / 挪位置 —— 一律停在
    // 签发之前：不授权、不采像素、不把"确认后第一次看到的新布局"追认成旧请求的基线，
    // 也不在同一次调用里对着新布局自动再弹一帧（重来一遍时会在新的快照上重新问）。
    const RECT kMovedArea = R(2000, 100, 2400, 400);

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
        Check(f.gate.AuthorizeWindow(paths::kWgc, kA, &err), "窗口内容那一级先确认（--yes 未给）");
        f.prompt.onAsk = [] { g_screens.clear(); };
        std::optional<DesktopPermit> permit;
        Check(!f.gate.AuthorizeDesktop(paths::kDwmScreen, kA, kAreaA, &permit, &err) &&
                  CodeIs(err, codes::kConsentStale),
              "DWM 升级到桌面像素：同一条确认后复核，确认期间拓扑变了照样停");
        Check(!permit.has_value(), "升级路径也没有凭证可用");
    }

    // =========================================================================
    std::printf("\nconsent-state: %d 条通过，%d 条失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
