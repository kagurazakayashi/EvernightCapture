// 只读屏幕枚举与按标识选屏（--screens / --monitor=device: / --monitor=id:）的离线判据
//（由 tests\screens.ps1 运行 build\ecapture-screens-tests.exe）。
//
// 为什么单独一个可执行文件：这一批判据要的现场在这台开发机上造不出来 ——
//   * 只接了一块屏，于是"副屏在负坐标""竖屏旋转""两块面板共享一个桌面"都没有第二条可对照
//   * 不能为了看"名字被系统重新发给另一块面板"真的去拔线，也不能改用户的显示设置
//   * 不能为了看"标识命中两块"接两台型号与 EDID 完全相同的监视器
//   * 显示配置整路问不出来（QueryDisplayConfig 失败）在本机更是没法安排
// 判据本体（src/ScreenIdentity.cpp 的 SelectScreenCandidates / CompareScreenIdentity 与
// src/ScreenQuery.cpp 的 BuildScreenQuery / RenderScreenQuery）是纯函数：注入一张假候选表
// 就能逐条判，连"读不到的那一项有没有被写成 0""选择器有没有凭空多出一条"都判得到。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。文案按英文跑（与其他 state 测试同一约定），
// 断言判的是机器可读的字段与取值，不判人话文字。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/Lang.h"
#include "../src/ScreenIdentity.h"
#include "../src/ScreenMatch.h"
#include "../src/ScreenQuery.h"
#include "../src/WindowQuery.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
    std::fflush(stdout);   // 崩了也看得见崩在哪一条
}

void Section(const char* title) {
    std::printf("\n=== %s ===\n", title);
    std::fflush(stdout);   // 崩了也看得见崩在哪一节（判据本身不许依赖这个）
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// 有时要比的是算出来的值（limits 那一条就是要跟实现里那个常量对字符串），所以两个重载都留着。
bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

RECT RectOf(int l, int t, int r, int b) {
    RECT rc{};
    rc.left = l;
    rc.top = t;
    rc.right = r;
    rc.bottom = b;
    return rc;
}

// 一条「什么都问得出来」的候选：设备名带前缀、devnode 路径与适配器路径都有真值、
// DPI 与旋转都读到了。每条用例只改自己那一项，失败时才知道是谁动的。
ScreenCandidate Healthy(uint32_t ordinal, const wchar_t* device, RECT bounds, bool primary,
                        const wchar_t* path) {
    ScreenCandidate c;
    c.screen.ordinal = ordinal;
    c.screen.deviceName = device;
    c.screen.bounds = bounds;
    c.screen.primary = primary;
    c.screen.monitor = 0x1000 + ordinal;
    c.facts.hasFacts = true;
    c.facts.config.read = ReadState::kReadable;
    c.facts.hasPath = true;
    c.facts.pathsMatched = 1;
    c.facts.monitorPath = path;
    c.facts.monitorPathQ.read = ReadState::kReadable;
    c.facts.adapterLuid = L"0x000000000000c2d3";
    c.facts.adapterPath = L"\\\\?\\PCI#VEN_10DE&DEV_2486#00000101";
    c.facts.adapterPathQ.read = ReadState::kReadable;
    c.facts.targetId = 2;
    c.facts.targetAvailable = true;
    c.facts.outputTechnology = L"displayport_external";
    c.facts.friendlyName = L"Test Panel";
    c.facts.edidManufactureId = 0x1e6d;
    c.facts.edidProductCode = 16806;
    c.facts.edidIdsValid = true;
    c.facts.panelRotation = L"identity";
    c.facts.dpi.read = ReadState::kReadable;
    c.facts.dpiEffectiveX = 109;
    c.facts.dpiEffectiveY = 109;
    c.facts.dpiRaw.read = ReadState::kReadable;
    c.facts.dpiRawX = 96;
    c.facts.dpiRawY = 96;
    c.facts.rotation.read = ReadState::kReadable;
    c.facts.rotationDegrees = 0;
    return c;
}

// 没有 facts 的那一档：编号 / 主屏 / all / 设备名这四种选择器都不需要显示配置，
// 这时每一问都是"没问过"，而不是"问出来是空"。
ScreenCandidate WithoutFacts(uint32_t ordinal, const wchar_t* device, RECT bounds, bool primary) {
    ScreenCandidate c = Healthy(ordinal, device, bounds, primary, L"");
    c.facts = ScreenFacts{};
    c.facts.dpi.read = ReadState::kFailed;
    c.facts.dpiRaw.read = ReadState::kFailed;
    c.facts.rotation.read = ReadState::kFailed;
    c.facts.rotationDegrees = -1;
    return c;
}

MonitorSelector SelPrimary() {
    MonitorSelector s;
    s.given = true;
    return s;
}

MonitorSelector SelOrdinal(uint32_t n) {
    MonitorSelector s;
    s.given = true;
    s.kind = MonitorSelector::Kind::kOrdinal;
    s.ordinal = n;
    s.written = std::to_wstring(n);
    return s;
}

MonitorSelector SelAll() {
    MonitorSelector s;
    s.given = true;
    s.kind = MonitorSelector::Kind::kAll;
    s.written = L"all";
    return s;
}

MonitorSelector SelDevice(const wchar_t* id) {
    MonitorSelector s;
    s.given = true;
    s.kind = MonitorSelector::Kind::kDevice;
    s.id = id;
    s.written = std::wstring(L"device:") + id;
    return s;
}

MonitorSelector SelId(const wchar_t* id) {
    MonitorSelector s;
    s.given = true;
    s.kind = MonitorSelector::Kind::kPath;
    s.id = id;
    s.written = std::wstring(L"id:") + id;
    return s;
}

std::vector<ScreenCandidate> TwoScreens() {
    std::vector<ScreenCandidate> v;
    // 主屏在原点，副屏在主屏**左边**（负坐标是常态而不是意外）
    v.push_back(Healthy(1, L"\\\\.\\DISPLAY1", RectOf(0, 0, 2560, 1440), true,
                        L"\\\\?\\DISPLAY#GSM41A2#5&1c6638c9&0&UID8388688"));
    v.push_back(Healthy(2, L"\\\\.\\DISPLAY2", RectOf(-1920, 300, 0, 1220), false,
                        L"\\\\?\\DISPLAY#DEL4096#5&1c6638c9&0&UID4352"));
    v[1].facts.rotationDegrees = 90;
    v[1].facts.panelRotation = L"rotate90";
    return v;
}

const std::vector<size_t> kNoPicked;

// 选择器判据的快捷调用：只关心"选到哪一条"或"报哪一条码"。
ScreenSelect Pick(const MonitorSelector& sel, const std::vector<ScreenCandidate>& all) {
    return SelectScreenCandidates(sel, all).outcome;
}

std::wstring FirstCode(const ScreenSelectResult& r) {
    return r.errors.empty() ? std::wstring() : r.errors.front().code;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 老写法的语义一条都不动（编号 / 主屏 / all），新写法的失败也绝不退化成老写法
// ---------------------------------------------------------------------------
namespace {

void TestLegacySelectorsUnchanged() {
    const std::vector<ScreenCandidate> all = TwoScreens();

    Check(Pick(SelOrdinal(2), all) == ScreenSelect::kFound &&
              SelectScreenCandidates(SelOrdinal(2), all).picked.front() == 1,
          "编号 2 还是选到本次枚举顺序里的第 2 块");
    Check(FirstCode(SelectScreenCandidates(SelOrdinal(9), all)) == codes::kMonitorOutOfRange,
          "编号越界照旧是 match.monitor_out_of_range（不改码，老调用方按同一条分支走）");
    Check(Pick(SelPrimary(), all) == ScreenSelect::kFound &&
              SelectScreenCandidates(SelPrimary(), all).picked.front() == 0,
          "不给取值 = 主屏，选到 primary 那块");

    // 没有哪块被标成主屏（远程会话里见过）：照旧用第一块，不新增失败。
    std::vector<ScreenCandidate> noPrimary = TwoScreens();
    noPrimary[0].screen.primary = false;
    Check(Pick(SelPrimary(), noPrimary) == ScreenSelect::kFound &&
              SelectScreenCandidates(SelPrimary(), noPrimary).picked.front() == 0,
          "本机没有主屏标记时仍按旧行为取第一块（不把老现场改成一条新错误）");

    Check(SelectScreenCandidates(SelAll(), all).picked.size() == 2,
          "all 是全部命中，条数与候选表一致");

    const std::vector<ScreenCandidate> none;
    Check(FirstCode(SelectScreenCandidates(SelOrdinal(1), none)) == codes::kMonitorOutOfRange &&
              FirstCode(SelectScreenCandidates(SelId(L"whatever"), none)) ==
                  codes::kMonitorOutOfRange,
          "一块屏都没有：两种写法都报同一条越界（这是本机没有屏，不是标识写错了）");
}

// ---------------------------------------------------------------------------
// 2. 设备名那条：形状宽容、比对不区分大小写、绝不替你挑
// ---------------------------------------------------------------------------
void TestDeviceSelector() {
    const std::vector<ScreenCandidate> all = TwoScreens();

    Check(Pick(SelDevice(L"DISPLAY2"), all) == ScreenSelect::kFound &&
              SelectScreenCandidates(SelDevice(L"DISPLAY2"), all).picked.front() == 1,
          "device:DISPLAY2（裸设备名，从 --screens 直接抄的形状）");
    Check(Pick(SelDevice(L"\\\\.\\DISPLAY2"), all) == ScreenSelect::kFound,
          "device: 也认带前缀的写法（两种形状在解析层归一，比对那一层只有一种键）");
    Check(Pick(SelDevice(L"display2"), all) == ScreenSelect::kFound,
          "设备名逐字符不区分大小写 —— 与 Windows 自己看设备名的办法一致");

    const ScreenSelectResult gone = SelectScreenCandidates(SelDevice(L"DISPLAY9"), all);
    Check(gone.outcome == ScreenSelect::kNoMatch && gone.picked.empty() &&
              gone.errors.front().code == codes::kMonitorUnknownId &&
              gone.errors.front().option == L"--monitor" &&
              gone.errors.front().stage == stages::kMatch &&
              gone.errors.front().value == L"device:DISPLAY9" && !gone.errors.front().hint.empty(),
          "设备名不在本机的此刻 → match.monitor_unknown_id + 完整定位字段，而不是退化成某块屏");

    // 同一份桌面列表里出现两个同名条目（系统把名字重新发出去了）时不替你挑一块。
    std::vector<ScreenCandidate> dup = TwoScreens();
    dup[1].screen.deviceName = dup[0].screen.deviceName;
    const ScreenSelectResult amb = SelectScreenCandidates(SelDevice(L"DISPLAY1"), dup);
    Check(amb.outcome == ScreenSelect::kAmbiguous && amb.picked.size() == 2 &&
              amb.errors.front().code == codes::kMonitorAmbiguousId,
          "同一标识命中两块屏 → match.monitor_ambiguous_id，绝不静默取第一块");
}

// ---------------------------------------------------------------------------
// 3. devnode 那条：问得出来才点得名，问不出来是 unverifiable 而不是"没找到"
// ---------------------------------------------------------------------------
void TestPathSelector() {
    const std::vector<ScreenCandidate> all = TwoScreens();
    Check(Pick(SelId(all[1].facts.monitorPath.c_str()), all) == ScreenSelect::kFound &&
              SelectScreenCandidates(SelId(all[1].facts.monitorPath.c_str()), all).picked.front() ==
                  1,
          "id:<监视器设备路径> 选到那块屏（跨会话那一条有选择器写法）");
    Check(SelectScreenCandidates(SelId(L"\\\\?\\DISPLAY#NOPE#0"), all).outcome ==
              ScreenSelect::kNoMatch,
          "路径对不上任何一块 → match.monitor_unknown_id");

    // 整张表都没问到 devnode：这一问没答案，不能折成"没有这块屏"，也不能折成"那就主屏"。
    std::vector<ScreenCandidate> unknown = TwoScreens();
    for (ScreenCandidate& c : unknown) {
        c.facts.monitorPathQ.read = ReadState::kDenied;
        c.facts.monitorPathQ.win32 = 5;
        c.facts.monitorPath.clear();
        c.facts.hasPath = false;
    }
    const ScreenSelectResult unv = SelectScreenCandidates(SelId(L"\\\\?\\DISPLAY#GSM41A2#x"),
                                                          unknown);
    Check(unv.outcome == ScreenSelect::kUnverifiable &&
              unv.errors.front().code == codes::kMonitorIdUnverifiable &&
              unv.errors.front().win32 == 5,
          "显示配置答不上来 → match.monitor_id_unverifiable + 那一问的错误码，与「没找到」分开");

    // 只有**一块**问得到时仍然点得名：不因为另一条没答案就整体判无法验证。
    std::vector<ScreenCandidate> partial = TwoScreens();
    partial[0].facts.monitorPathQ.read = ReadState::kFailed;
    partial[0].facts.monitorPath.clear();
    Check(Pick(SelId(partial[1].facts.monitorPath.c_str()), partial) == ScreenSelect::kFound,
          "另一块问不到不影响这一块的对号（字段级 unknown，不做整表否决）");
}

// ---------------------------------------------------------------------------
// 4. 选择器标签与种类名：三处回显共用的那一份
// ---------------------------------------------------------------------------
void TestSelectorLabels() {
    Check(MonitorSelectorLabel(SelPrimary()) == L"primary" &&
              MonitorSelectorLabel(SelAll()) == L"all" && MonitorSelectorLabel(SelOrdinal(3)) == L"3",
          "老写法的标签形状不变（primary / all / 十进制编号）");
    Check(MonitorSelectorLabel(SelDevice(L"DISPLAY1")) == L"device:DISPLAY1" &&
              MonitorSelectorLabel(SelId(L"\\\\?\\DISPLAY#A#B")) == L"id:\\\\?\\DISPLAY#A#B",
          "新写法的标签带上各自的前缀，与 --screens 交回的选择器逐字相同");
    Check(std::wstring(MonitorSelectorKindName(SelOrdinal(3))) == L"ordinal" &&
              std::wstring(MonitorSelectorKindName(SelPrimary())) == L"primary" &&
              std::wstring(MonitorSelectorKindName(SelAll())) == L"all" &&
              std::wstring(MonitorSelectorKindName(SelDevice(L"D"))) == L"device" &&
              std::wstring(MonitorSelectorKindName(SelId(L"D"))) == L"id",
          "「是哪一种」单独一个字段：调用方不必猜 input.monitor 是数字还是字符串");
    Check(MonitorSelectorNeedsIdentity(SelId(L"x")) &&
              !MonitorSelectorNeedsIdentity(SelDevice(L"x")) &&
              !MonitorSelectorNeedsIdentity(SelOrdinal(1)) &&
              !MonitorSelectorNeedsIdentity(SelPrimary()) &&
              !MonitorSelectorNeedsIdentity(SelAll()),
          "只有 id: 那一条要问显示配置：按名字点名的路不为一块屏去跑一遍 QueryDisplayConfig");
}

// ---------------------------------------------------------------------------
// 5. 取帧之前的身份复核（含"名字被重新发给另一块面板"这一条）
// ---------------------------------------------------------------------------
void TestIdentityRecheck() {
    const std::vector<ScreenCandidate> all = TwoScreens();
    ScreenCandidate fresh{};

    Check(CompareScreenIdentity(all[1], all, &fresh) == ScreenIdentityCheck::kSame,
          "同一条 devnode + 同几何 = 还是那块屏");

    std::vector<ScreenCandidate> moved = TwoScreens();
    moved[1].screen.bounds = RectOf(-1920, 0, 0, 1080);
    Check(CompareScreenIdentity(all[1], moved, &fresh) == ScreenIdentityCheck::kMoved &&
              fresh.screen.bounds.bottom == 1080,
          "同一块屏改了矩形 → kMoved + 交回新那份（换成新矩形重新确认，旧授权不沿用）");

    std::vector<ScreenCandidate> unplugged = {all[0]};
    Check(CompareScreenIdentity(all[1], unplugged, &fresh) == ScreenIdentityCheck::kGone,
          "那块屏不在桌面里了 → kGone，一个像素都不读");

    // 核心判据：设备名没变，但那个名字已经发给了**另一块面板**（devnode 不同）。
    // 只看名字的旧做法在这里会把另一块屏当成原来那块截下去。
    std::vector<ScreenCandidate> renamed = TwoScreens();
    renamed[1].facts.monitorPath = L"\\\\?\\DISPLAY#ACR0020#5&1c6638c9&0&UID9999";
    Check(CompareScreenIdentity(all[1], renamed, &fresh) == ScreenIdentityCheck::kGone,
          "同名不同面板 → kGone：绝不按名字截那块接管了名字的新屏");

    // 这一问没答案时不作断言，也不退回"照名字截"：那是"没给出答案"，不是"没变"。
    std::vector<ScreenCandidate> unreadable = TwoScreens();
    for (ScreenCandidate& c : unreadable) {
        c.facts.monitorPathQ.read = ReadState::kFailed;
        c.facts.monitorPath.clear();
    }
    Check(CompareScreenIdentity(all[1], unreadable, &fresh) == ScreenIdentityCheck::kUnverifiable,
          "重新核对时问不出身份 → kUnverifiable（不是 kSame，也不是 kGone）");

    // 基线当时就没问到 devnode（老写法、或那一问失败）：照旧按名字核，不新增失败。
    const ScreenCandidate legacy = WithoutFacts(1, L"\\\\.\\DISPLAY1", RectOf(0, 0, 2560, 1440),
                                                true);
    std::vector<ScreenCandidate> legacyCurrent;
    legacyCurrent.push_back(WithoutFacts(1, L"\\\\.\\DISPLAY1", RectOf(0, 0, 2560, 1440), true));
    Check(CompareScreenIdentity(legacy, legacyCurrent, &fresh) == ScreenIdentityCheck::kSame,
          "基线没有跨会话标识时按设备名核对，行为与旧实现一致（没做出来的判定不算失败）");
    std::vector<ScreenCandidate> legacyGone;
    legacyGone.push_back(WithoutFacts(1, L"\\\\.\\DISPLAY3", RectOf(0, 0, 800, 600), true));
    Check(CompareScreenIdentity(legacy, legacyGone, &fresh) == ScreenIdentityCheck::kGone,
          "老那一档里名字不见了仍是 kGone（这一档的判据没被换掉）");
}

// ---------------------------------------------------------------------------
// 5b. 确认之后、采样之前的采样入口复核（F06）
//
// 弹框前那一问（上面第 5 节）定下授权快照；这一节判的是拿到 DesktopPermit 之后、
// 那条通道真去读像素之前的第二次核对 —— 人在框上点头的这几秒里，屏可能被拔掉、
// 名字可能被重新发给另一块面板、分辨率/位置可能改。三种漂移都要停在采样之前。
// ---------------------------------------------------------------------------
void TestSamplingRecheck() {
    const std::vector<ScreenCandidate> all = TwoScreens();
    const ScreenCandidate& wanted = all[1];
    const RECT approved = wanted.screen.bounds;   // 判定器刚批准的那一片（= 弹框前那一刻的矩形）
    ScreenCandidate fresh{};

    Check(RecheckScreenSampling(wanted, approved, all, &fresh) == SamplingRecheck::kOk &&
              fresh.screen.monitor == all[1].screen.monitor,
          "身份与形状都没变 → kOk，采样对象换成当下问到的那一份（含当下的 HMONITOR）");

    // 采样用的是当下句柄，不是选定那一刻的旧句柄：热插拔重建布局后旧句柄不可信。
    std::vector<ScreenCandidate> rebuilt = TwoScreens();
    rebuilt[1].screen.monitor = 0x9ABC;
    Check(RecheckScreenSampling(wanted, approved, rebuilt, &fresh) == SamplingRecheck::kOk &&
              fresh.screen.monitor == 0x9ABC,
          "同一块屏重建后 → kOk 且交回的是新句柄（旧句柄不拿去做采样证据）");

    std::vector<ScreenCandidate> moved = TwoScreens();
    moved[1].screen.bounds = RectOf(-1920, 0, 0, 1080);
    Check(RecheckScreenSampling(wanted, approved, moved, &fresh) == SamplingRecheck::kStale,
          "确认之后那块屏改了矩形/位置 → kStale：一像素不采，旧授权不追认到新布局");

    std::vector<ScreenCandidate> unplugged = {all[0]};
    Check(RecheckScreenSampling(wanted, approved, unplugged, &fresh) == SamplingRecheck::kGone,
          "那块屏被拔掉 → kGone：不采，也不替它挑另一块屏");

    std::vector<ScreenCandidate> renamed = TwoScreens();
    renamed[1].facts.monitorPath = L"\\\\?\\DISPLAY#ACR0020#5&1c6638c9&0&UID9999";
    Check(RecheckScreenSampling(wanted, approved, renamed, &fresh) == SamplingRecheck::kGone,
          "同名设备换成另一身份（名字被重新发出去）→ kGone：接管名字的是一块没批准过的屏");

    std::vector<ScreenCandidate> unreadable = TwoScreens();
    for (ScreenCandidate& c : unreadable) {
        c.facts.monitorPathQ.read = ReadState::kFailed;
        c.facts.monitorPath.clear();
    }
    Check(RecheckScreenSampling(wanted, approved, unreadable, &fresh) ==
              SamplingRecheck::kUnverifiable,
          "采样这一问问不出身份 → kUnverifiable：不能按\"大概没变吧\"放行");

    // 调用点自洽的负对照：身份说没变，但批准区域装不下当下矩形 = 调用方给错了区域，
    // 采样照样要停（这条兜住的是判定器与采样入口之间被写错的接线，不放宽成容差）。
    Check(RecheckScreenSampling(wanted, RectOf(-50, -50, 50, 50), all, &fresh) ==
              SamplingRecheck::kStale,
          "当下矩形越出批准区域 → kStale（零容差，与 DesktopPermit::Covers 同一判据）");

    // 老那一档（基线没有跨会话标识）：按名字核，同名同形状就放行，不新增失败。
    const ScreenCandidate legacy = WithoutFacts(1, L"\\\\.\\DISPLAY1", RectOf(0, 0, 2560, 1440),
                                                true);
    std::vector<ScreenCandidate> legacyCurrent;
    legacyCurrent.push_back(WithoutFacts(1, L"\\\\.\\DISPLAY1", RectOf(0, 0, 2560, 1440), true));
    Check(RecheckScreenSampling(legacy, legacy.screen.bounds, legacyCurrent, &fresh) ==
              SamplingRecheck::kOk,
          "基线没有 devnode 时按设备名核对放行（与旧行为一致）");
    std::vector<ScreenCandidate> legacyMoved;
    legacyMoved.push_back(WithoutFacts(1, L"\\\\.\\DISPLAY1", RectOf(0, 0, 1920, 1080), true));
    Check(RecheckScreenSampling(legacy, legacy.screen.bounds, legacyMoved, &fresh) ==
              SamplingRecheck::kStale,
          "老那一档里名字对上了但矩形变了 = 那块屏改了样子：确认之后变的按 kStale 停");
}

// ---------------------------------------------------------------------------
// 6. 文档级判据：并集矩形、复制模式、对不上路径的条数
// ---------------------------------------------------------------------------
void TestTopologyFacts() {
    const ScreenQueryResult two = BuildScreenQuery(TwoScreens());
    Check(two.virtualKnown && two.virtualX == -1920 && two.virtualY == 0 &&
              two.virtualWidth == 2560 + 1920 && two.virtualHeight == 1440,
          "虚拟屏幕并集含负坐标：副屏在主屏左边时 x 是负的、宽度是两边之和");
    Check(two.screens.size() == 2 && two.unmatchedPaths == 0 && two.clonedScreens == 0,
          "两块都对上了路径：没有 screensWithoutDisplayPath，也没有克隆计数");
    Check(two.exitCode == 0 && !two.notes.empty() &&
              two.notes.front().code == codes::kScreenQueryStale,
          "文档出完就是 0，并固定带一条「此刻的快照会过期」");

    const ScreenQueryResult none = BuildScreenQuery({});
    Check(!none.virtualKnown && none.screens.empty() && none.exitCode == 0,
          "一块屏都没有：virtualScreenKnown=false 而不是报一个 0x0 的假尺寸");

    std::vector<ScreenCandidate> clone = TwoScreens();
    clone[0].facts.pathsMatched = 2;   // 两条路径共享同一个桌面 = 复制模式，还是一个桌面里的一块屏
    const ScreenQueryResult cloned = BuildScreenQuery(clone);
    Check(cloned.clonedScreens == 1 && cloned.screens.size() == 2,
          "复制模式计数不改 screens 条数：几块面板共享一个桌面不等于几块屏");

    std::vector<ScreenCandidate> unmatched = TwoScreens();
    unmatched[1].facts.hasPath = false;
    unmatched[1].facts.pathsMatched = 0;
    unmatched[1].facts.monitorPath.clear();
    unmatched[1].facts.monitorPathQ.read = ReadState::kFailed;
    Check(BuildScreenQuery(unmatched).unmatchedPaths == 1,
          "在显示配置里对不上路径的有几块，单独记一条（不等于它不存在）");
}

// ---------------------------------------------------------------------------
// 7. 渲染：身份稳定性、字段级 unknown、隐私自述、以及这份文档不是截图那份
// ---------------------------------------------------------------------------
void TestRenderDocument() {
    std::vector<ScreenCandidate> all = TwoScreens();
    all[0].facts.dpi.read = ReadState::kDenied;
    all[0].facts.dpi.win32 = 5;
    all[0].facts.monitorPathQ.read = ReadState::kFailed;
    all[0].facts.monitorPathQ.win32 = 2;
    all[0].facts.monitorPath.clear();
    const std::wstring doc = RenderScreenQuery(BuildScreenQuery(all), false, false);

    Check(Contains(doc, L"\"contract\": \"screens\"") && Contains(doc, L"\"contractVersion\": 1"),
          "这是一份**另外的契约**（带 contract / contractVersion）");
    Check(!Contains(doc, L"\"captured\"") && !Contains(doc, L"\"images\""),
          "不塞进截图那份精简 JSON 的形状：截图的顶层键一个都不出现在这里");

    // 段落的**层级**也要判：少写一次 End 会把后面几段整段吞进上一段里，
    // 而键名与取值全都在、只有缩进不对 —— 那种文档调用方按顶层读就读不到。
    // 渲染器一层缩进两个空格，所以顶层段落恒在 "\n  \"键名\""。
    Check(Contains(doc, L"\n  \"authorization\"") && Contains(doc, L"\n  \"identity\"") &&
              Contains(doc, L"\n  \"topology\"") && Contains(doc, L"\n  \"screens\"") &&
              Contains(doc, L"\n  \"caveats\"") && Contains(doc, L"\n  \"privacy\"") &&
              Contains(doc, L"\n  \"limits\""),
          "七个顶层段落各在顶层（少一次 End 会把后面整段吞进上一段，键名却全都还在）");
    Check(Contains(doc, L"\n      \"desktopPixelsAlwaysAsk\"") &&
              !Contains(doc, L"\n  \"desktopPixelsAlwaysAsk\""),
          "整屏一定要问人那两条写在 authorization 里面，不冒充顶层字段");

    // 四种身份各稳在哪一层，只有两条有选择器 —— 编号与 LUID 能对上号不等于能点名。
    Check(Contains(doc, L"\"stableAcross\": \"this_invocation\"") &&
              Contains(doc, L"\"stableAcross\": \"this_desktop_attach\"") &&
              Contains(doc, L"\"stableAcross\": \"cross_session_expected\"") &&
              Contains(doc, L"\"stableAcross\": \"this_session\""),
          "四种稳定性各写一条：本次枚举 / 本次连接 / 跨会话 / 本次会话，不互相冒充");
    Check(Contains(doc, L"\"adapterLuid\"") && Contains(doc, L"\"usableAsSelector\": false"),
          "适配器 LUID 只作关联信息：它在这次会话之外就不唯一，所以没有选择器写法");
    Check(Contains(doc, L"\"selectorForm\": \"device:\"") &&
              Contains(doc, L"\"selectorForm\": \"id:\""),
          "两条有选择器的身份各写出自己的写法，调用方不必读源码才知道该怎么写回 --monitor");

    // 问不到的那一项：值整个键不出现，而下场写在 readability 里 —— 不拿 0 或空串冒充答案。
    Check(!Contains(doc, L"\"effectiveX\": 0") && Contains(doc, L"\"dpi\"") &&
              Contains(doc, L"\"state\": \"denied\""),
          "DPI 问不到时不写 0，只写那一问的下场（readable / denied / failed）");
    Check(Contains(doc, L"\"win32\": 2"),
          "失败点当场取走的错误码一并交回，调用方能分辨被挡下与问过而失败");
    Check(Contains(doc, L"\"monitorDevicePath\"") && Contains(doc, L"\"id\": "),
          "问得到的那块屏有 id: 选择器，问不到的那条只有 readability 说得出为什么少");

    // 隐私与只读的自述是判据，不是文案：--quiet 也不许把它们抑制掉。
    Check(Contains(doc, L"\"pixelsRead\": 0") && Contains(doc, L"\"consentDialogShown\": false") &&
              Contains(doc, L"\"displaySettingsChanged\": false") &&
              Contains(doc, L"\"yesAffectsResult\": false") &&
              Contains(doc, L"\"desktopPixelsAlwaysAsk\": true") &&
              Contains(doc, L"\"yesSkipsThisLevel\": false"),
          "只读与授权自述齐备：真去截整屏一定问人，--yes 对桌面像素不生效");
    Check(Contains(doc, L"\"includesDevicePaths\": true") &&
              Contains(doc, L"\"includesFileSystemPaths\": false") &&
              Contains(doc, L"\"includesUsernames\": false"),
          "这份文档与 --capabilities 的差别只有一条：这里有设备路径，没有文件系统路径与用户名");

    Check(Contains(doc, L"\"cross_session_stability_not_tested\"") &&
              Contains(doc, L"\"screen_capture_always_asks\"") &&
              Contains(doc, L"\"device_names_are_not_persistent\""),
          "不作过头断言：跨会话稳定是没实测过的道理、截屏一定要问人、名字会被重新发出去");

    const std::wstring quiet = RenderScreenQuery(BuildScreenQuery(all), false, true);
    Check(!Contains(quiet, L"\"notes\"") && Contains(quiet, L"\"caveats\"") &&
              Contains(quiet, L"\"readability\"") && Contains(quiet, L"\"identity\"") &&
              Contains(quiet, L"\"authorization\""),
          "--quiet 只去掉 notes：稳定性自述、字段级可读性与隐私判据一条都不藏");

    const std::wstring verbose = RenderScreenQuery(BuildScreenQuery(all), true, false);
    Check(Contains(verbose, L"\"input\"") && Contains(verbose, L"\"maxOrdinal\""),
          "-v 追加规范化后的这一次查询；limits 与实现常量同源");
    const std::wstring limits = std::to_wstring(static_cast<unsigned long long>(
        cli_limits::kMaxOrdinal));
    Check(Contains(doc, L"\"maxOrdinal\": " + limits), "文档里的上限就是解析层那一个数");

    // 竖屏那一条：旋转与面板朝向各写各的（90 度时不是"宽高互换"这种说法）。
    Check(Contains(doc, L"\"degrees\": 90") && Contains(doc, L"\"panel\": \"rotate90\""),
          "旋转屏：人看到的朝向与相对面板原生朝向两条分开写");
}

// ---------------------------------------------------------------------------
// 8. 三条新码的退出码：与窗口那一份同一张映射表
// ---------------------------------------------------------------------------
void TestExitCodeMapping() {
    Check(WindowQueryExitCodeFor(codes::kMonitorUnknownId) == EX_NO_MATCH,
          "标识不在本机的此刻 = 4（没有对上目标），与 match.no_window 同一条下一步");
    Check(WindowQueryExitCodeFor(codes::kMonitorAmbiguousId) == EX_AMBIGUOUS,
          "一个标识命中多块 = 5，与多扇窗口没消歧同一条下一步");
    Check(WindowQueryExitCodeFor(codes::kMonitorIdUnverifiable) == EX_CAPTURE_FAILED,
          "这一问没答案 = 7，与 match.timeout 同类（要说的是「没能问完」）");
    Check(WindowQueryExitCodeFor(codes::kMonitorOutOfRange) == EX_USAGE,
          "编号越界照旧 = 1（写错了编号还是用法错）");
}

// ---------------------------------------------------------------------------
// 用例总入口
// ---------------------------------------------------------------------------
void RunAll() {
    Section("老写法的语义不变");
    TestLegacySelectorsUnchanged();
    Section("按设备名点名一块屏");
    TestDeviceSelector();
    Section("按跨会话标识点名一块屏");
    TestPathSelector();
    Section("选择器的标签与种类");
    TestSelectorLabels();
    Section("取帧之前的身份复核");
    TestIdentityRecheck();
    Section("确认之后、采样之前的采样入口复核");
    TestSamplingRecheck();
    Section("文档级的拓扑判据");
    TestTopologyFacts();
    Section("渲染：稳定性、unknown、隐私");
    TestRenderDocument();
    Section("退出码与窗口那一份同源");
    TestExitCodeMapping();
}

}  // namespace

int main() {
    // 渲染不读文案资源（机器取值全 ASCII），但判据会经过选择器诊断那一路，
    // 那里要读 .rc 里编进的四份资源，所以整份判据按英文跑，与其他 state 测试同一约定。
    SetLanguage(Language::kEn);
    RunAll();
    std::printf("\nscreens-state: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
