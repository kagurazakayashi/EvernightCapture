// 截图授权状态机的离线判据（由 tests\consent.ps1 编译并运行）。
//
// 为什么在这一层测：授权这件事的分支（升级要重新确认、拒绝之后不再换后端也不再问第二遍、
// 目标挪位置或换屏幕拓扑就作废凭证、--yes 只对窗口内容路径生效……）在真机上每一条都要人
// 去点一次确认框，而发布版里不许留任何"测试旁路"。所以这里注入假的应答器与假的屏幕布局，
// 把 ConsentGate 与路径分类整台跑完；真机那部分判据留在 tests\consent.ps1 与 window_shot.bat。
//
// 这个可执行文件不含任何取帧代码：它只链接纯逻辑那几份源文件（见 CMakeLists.txt）。

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "../src/CaptureScope.h"
#include "../src/CliOptions.h"
#include "../src/Consent.h"
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
};

class FakePrompt final : public IConsentPrompt {
public:
    ConsentReply Ask(const ConsentQuestion& q) override {
        ++asks;
        asked.push_back(Asked{q.scope, q.path, q.yesGiven, q.targets.size(), q.outputs.size()});
        ConsentReply reply;
        reply.answer = ConsentAnswer::kAccepted;   // 脚本没写到的那一次一律答"是"
        const size_t n = asks;   // 第 n 次弹框（1 起）
        if (n <= answers.size()) reply.answer = answers[n - 1];
        reply.win32 = unavailableWin32;
        return reply;
    }

    int asks = 0;
    std::vector<Asked> asked;
    std::vector<ConsentAnswer> answers;   // 空 = 一律答"是"
    DWORD unavailableWin32 = 0;
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
    t.description = key;
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

const RECT kAreaA = R(100, 100, 500, 400);
const RECT kAreaB = R(600, 0, 900, 300);
const std::wstring kA = L"0xAAAA";
const std::wstring kB = L"0xBBBB";

std::vector<GateTarget> TwoTargets() {
    return {Target(kA.c_str(), kAreaA), Target(kB.c_str(), kAreaB)};
}

bool CodeIs(const Diagnostic& d, const wchar_t* code) { return d.code == code; }

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
        // 区域：凭证只认批准过的那一片（允许一点测量误差）
        Fixture f(false, TwoTargets());
        Diagnostic err;
        std::optional<DesktopPermit> permit;
        f.gate.AuthorizeDesktop(paths::kBitBltScreen, kA, kAreaA, &permit, &err);
        Check(permit->Covers(R(120, 120, 480, 380)), "批准区域内的子矩形算覆盖");
        Check(!permit->Covers(R(100, 100, 1500, 1000)), "整块屏幕不在那一片之内");
        Check(!permit->Covers(R(-50, -50, 50, 50)), "挪到别处不算");
        Check(permit->Covers(R(90, 90, 510, 410)), "十几像素的边框测量误差容忍");
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
    std::printf("\nconsent-state: %d 条通过，%d 条失败\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
