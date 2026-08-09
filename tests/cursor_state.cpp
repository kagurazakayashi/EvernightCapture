// EvernightCapture - 光标包含与排除（--cursor）的离线判据
//
// 这一批判据要的现场是「这台机器的 Windows 版本正好卡在那道门槛两侧」「显式指定的那条通道
// 做不到这个光标要求」「设完读回来是相反的那一件」「某条路径忘了登记」—— 本机只有一台
// Windows 10 22H2（19045）的开发机，降不了级，也绝不能为了看「没有那个开关时会怎样」去
// 动真机的 WGC。所以判据本体（src/CursorControl.cpp 的 FilterChainForCursor /
// GateCaptureChain，以及 src/CursorControl.h 那张按路径登记的表与 MakeCursorReport）
// 在这里被逐条注入判：假版本、假通道链、假帧读数。
//
// 另外两条是「两份表不许各说一套」的现场核对：
//   * 光标表覆盖 src/CaptureScope.cpp 登记表里的每一条路径（漏一条 = 该路径按未登记处理）
//   * 通道级的 ChannelHasCursorSwitch 与路径表的 kSettable 对每个通道、两种目标都对得上
// 还有解析层那一段：直接调 ParseCommandLine，所以「include 配 printwindow 在解析期就拒」
// 这件事不必起进程、也不必真去截图就能判。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/CaptureScope.h"
#include "../src/CursorControl.h"
#include "../src/Lang.h"
#include "../src/SystemCompat.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  PASS  %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

OsVersion Os(uint32_t build, bool known = true) {
    OsVersion v;
    v.major = 10;
    v.minor = 0;
    v.build = build;
    v.known = known;
    return v;
}

CursorRequest Request(CursorMode mode, bool given) {
    CursorRequest r;
    r.mode = mode;
    r.given = given;
    return r;
}

// GateCaptureChain 现在串三道闸门（版本 / 光标 / HDR），这一份判据只管光标那一道，
// 所以 HDR 那一个要求恒给"没写过"（默认值真的不动任何东西，链不会被它筛歪）。
// HDR 那一道自己的矩阵在 tests\hdr_state.cpp 判。
HdrRequest NoHdr() { return HdrRequest(); }

// 把一次闸门结果的链写成 "wgc,dwm" 这种一行形状，断言与汇报都好看。
std::string Brief(const std::vector<CaptureMethod>& chain) {
    std::string s;
    for (const CaptureMethod m : chain) {
        if (!s.empty()) s += ",";
        for (const wchar_t* p = CaptureMethodName(m); *p; ++p)
            s.push_back(static_cast<char>(*p));
    }
    return s;
}

std::string Ascii(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out.push_back(static_cast<char>(c));
    return out;
}

bool HasCode(const std::vector<Diagnostic>& items, const wchar_t* code) {
    for (const auto& d : items)
        if (d.code == code) return true;
    return false;
}

std::vector<CaptureMethod> WindowAll() {
    return {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail, CaptureMethod::kPrintWindow,
            CaptureMethod::kBitBlt};
}

std::vector<CaptureMethod> ScreenAll() {
    return {CaptureMethod::kWgc, CaptureMethod::kDuplication, CaptureMethod::kBitBlt};
}

// 直接调解析层：argv 要的是"活着的 wchar_t* 数组"，所以先把取值存进一个 vector 再取指针。
ParseResult Run(std::initializer_list<std::wstring> args) {
    std::vector<std::wstring> store(args);
    std::vector<wchar_t*> argv;
    argv.reserve(store.size());
    for (auto& s : store) argv.push_back(s.data());
    return ParseCommandLine(static_cast<int>(argv.size()), argv.data());
}

// ---------------------------------------------------------------------------
// 1. 那张按路径登记的表本身
// ---------------------------------------------------------------------------
void TestRegistry() {
    Section("登记表：两条 wgc 有开关，其余按来源判定，没登记的一律 unregistered");
    using C = CursorCapability;
    Check(CursorCapabilityOfPath(paths::kWgc) == C::kSettable, "wgc = settable");
    Check(CursorCapabilityOfPath(paths::kScreenWgc) == C::kSettable, "screen.wgc = settable");
    Check(CursorCapabilityOfPath(paths::kPrintWindow) == C::kExcludesCursor,
          "printwindow = excludes_cursor");
    Check(CursorCapabilityOfPath(paths::kDwmThumbnail) == C::kExcludesCursor,
          "dwm.thumbnail = excludes_cursor");
    Check(CursorCapabilityOfPath(paths::kDwmScreen) == C::kExcludesCursor,
          "dwm.screen = excludes_cursor");
    Check(CursorCapabilityOfPath(paths::kBitBltScreen) == C::kExcludesCursor,
          "bitblt.screen = excludes_cursor");
    Check(CursorCapabilityOfPath(paths::kScreenBitBlt) == C::kExcludesCursor,
          "screen.bitblt = excludes_cursor");
    Check(CursorCapabilityOfPath(paths::kDuplicationFrame) == C::kExcludesCursor,
          "duplication.frame = excludes_cursor");
    Check(CursorCapabilityOfPath(paths::kScreenDuplication) == C::kExcludesCursor,
          "screen.duplication = excludes_cursor");
    Check(CursorCapabilityOfPath(paths::kUnknown) == C::kUnregistered, "unknown = unregistered");
    Check(CursorCapabilityOfPath(L"brand.new") == C::kUnregistered, "没登记的名字 = unregistered");
    Check(CursorCapabilityOfPath(L"") == C::kUnregistered, "空路径 = unregistered（不是 settable）");
    Check(CursorCapabilityOfPath(nullptr) == C::kUnregistered, "空指针 = unregistered，不崩");
    // 漏登记的症状是"更严"：未登记时 include 与 exclude 都**不敢**声称做到。
    Check(CursorReasonOfPath(L"brand.new") == cursor_reason::kNotRegistered,
          "未登记的路径给 not_registered 这个原因");
    Check(CursorReasonOfPath(paths::kDuplicationFrame) == cursor_reason::kPointerMetadata,
          "桌面复制那条的原因是 pointer_shape_is_separate_metadata（不是屏幕 DC 那一条）");
    Check(std::wstring(CursorCapabilityName(C::kSettable)) == L"settable" &&
              std::wstring(CursorCapabilityName(C::kExcludesCursor)) == L"excludes_cursor" &&
              std::wstring(CursorCapabilityName(C::kUnregistered)) == L"unregistered",
          "capability 的三个机器名齐备且互不相同");
}

// ---------------------------------------------------------------------------
// 2. 两份表说的是同一批路径、同一个答案
// ---------------------------------------------------------------------------
void TestTablesAgree() {
    Section("两份表一致性：像素来源表里的每一条都要在光标表里查得到");
    const auto& scope = RegisteredCapturePaths();
    bool everyScopePathRegistered = true;
    for (const auto& e : scope) {
        // paths::kUnknown 是那条兜底名（帧里没填 path 时写它），本来就不是一条真实路径：
        // 它按未登记处理正是这里要的形状，下面单独判它。
        if (std::wstring(e.path) == paths::kUnknown) continue;
        if (CursorCapabilityOfPath(e.path) == CursorCapability::kUnregistered) {
            std::printf("        漏登记: %s\n", Ascii(e.path).c_str());
            everyScopePathRegistered = false;
        }
    }
    Check(!scope.empty(), "来源登记表非空（否则下面那条判据是空对空）");
    Check(everyScopePathRegistered, "每条已登记的内部路径都有光标能力（新增通道忘了登记就红在这里）");

    bool everyCursorPathInScope = true;
    for (const auto& c : RegisteredCursorPaths()) {
        bool found = false;
        for (const auto& e : scope)
            if (std::wstring(e.path) == c.path) found = true;
        if (!found) {
            std::printf("        多余: %s\n", Ascii(c.path).c_str());
            everyCursorPathInScope = false;
        }
    }
    Check(everyCursorPathInScope, "光标表里没有来源表之外的路径（两张表不各写一份清单）");
    Check(CursorCapabilityOfPath(paths::kUnknown) == CursorCapability::kUnregistered,
          "那条兜底名按未登记处理：说不清来路就不对光标作任何断言");

    // 通道级那句判断与路径表对得上：两种目标、每条已实现通道，逐条核。
    const CaptureMethod methods[] = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                     CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt,
                                     CaptureMethod::kDuplication};
    bool switchAgrees = true;
    for (const CaptureMethod m : methods) {
        const bool bySwitch = ChannelHasCursorSwitch(m);
        const bool byPathWindow =
            CursorCapabilityOfPath(WindowPathOf(m)) == CursorCapability::kSettable;
        // 屏幕模式：dwm / printwindow 本来就被挡在屏幕目标之外，但登记表那一问照样要一致。
        const bool byPathScreen =
            CursorCapabilityOfPath(ScreenPathOf(m)) == CursorCapability::kSettable;
        if (bySwitch != byPathWindow || bySwitch != byPathScreen) {
            std::printf("        打脸: %s switch=%d window=%d screen=%d\n",
                        Ascii(CaptureMethodName(m)).c_str(), bySwitch ? 1 : 0,
                        byPathWindow ? 1 : 0, byPathScreen ? 1 : 0);
            switchAgrees = false;
        }
    }
    Check(switchAgrees, "ChannelHasCursorSwitch 与路径表的 kSettable 逐条相同（不存在第二套判据）");
    Check(CursorRequestPossible(CaptureMethod::kWgc, CursorMode::kInclude),
          "wgc 的 include 结构上做得到");
    Check(!CursorRequestPossible(CaptureMethod::kPrintWindow, CursorMode::kInclude),
          "printwindow 的 include 结构上做不到（解析期就要拒）");
    Check(!CursorRequestPossible(CaptureMethod::kBitBlt, CursorMode::kInclude) &&
              !CursorRequestPossible(CaptureMethod::kDuplication, CursorMode::kInclude) &&
              !CursorRequestPossible(CaptureMethod::kDwmThumbnail, CursorMode::kInclude),
          "bitblt / duplication / dwm 的 include 同样做不到");
    Check(CursorRequestPossible(CaptureMethod::kAuto, CursorMode::kInclude),
          "auto 不在这里下结论（它看闸门筛完之后剩什么）");
    bool excludeAlwaysOk = true;
    for (const CaptureMethod m : methods) {
        if (!CursorRequestPossible(m, CursorMode::kExclude) ||
            !CursorRequestPossible(m, CursorMode::kDefault))
            excludeAlwaysOk = false;
    }
    Check(excludeAlwaysOk, "exclude 与 default 对每条通道都在解析期放行（exclude 的差别在 basis，不在能不能）");
}

// ---------------------------------------------------------------------------
// 3. 版本门槛：开关的下限比通道本身的下限高
// ---------------------------------------------------------------------------
void TestVersionFloor() {
    Section("版本门槛：19041 那道线两侧各判一次");
    Check(os_floor::kWgcCursor == 19041, "开关的内部版本 = 19041（文档那条 10.0.19041.0）");
    Check(os_floor::kWgcCursor > os_floor::kWgc,
          "开关的下限高于 wgc 通道自己的下限（18362 能建会话，但问不到那个开关）");
    Check(os_floor::kWgcCursor > os_floor::kSupportedMinBuild,
          "这道门槛比本工具对外声明的下限还高（所以不是所有声明支持的机器都有的说）");
    Check(AssessWgcCursorControl(Os(19040)).support == Support::kBelow, "19040 = kBelow");
    Check(AssessWgcCursorControl(Os(19041)).support == Support::kOk, "19041 = kOk（含端点）");
    Check(AssessWgcCursorControl(Os(18362)).support == Support::kBelow, "18362 = kBelow");
    Check(AssessWgcCursorControl(Os(19045)).support == Support::kOk, "19045 = kOk（开发机那一台）");
    const Capability unknown = AssessWgcCursorControl(Os(0, false));
    Check(unknown.support == Support::kUnknown && unknown.minBuild == os_floor::kWgcCursor,
          "版本问不出来 = kUnknown，而下限那个数照样交得出来");
}

// ---------------------------------------------------------------------------
// 4. 通道链闸门
// ---------------------------------------------------------------------------
void TestChainGate() {
    Section("没写 --cursor 与写 default：链一条都不动，note 一条都不发");
    {
        const CursorChainGate g =
            FilterChainForCursor(WindowAll(), Request(CursorMode::kDefault, false), false, Os(19045));
        Check(Brief(g.chain) == "wgc,dwm,printwindow,bitblt", "未给出：链原样");
        Check(g.notes.empty() && g.error.code.empty(), "未给出：不发 note、不报错");
        const CursorChainGate d =
            FilterChainForCursor(WindowAll(), Request(CursorMode::kDefault, true), false, Os(19045));
        Check(Brief(d.chain) == "wgc,dwm,printwindow,bitblt" && d.notes.empty() &&
                  d.error.code.empty(),
              "显式 default：也一条都不动（default = 不作要求）");
    }

    Section("include：做不到的那几条摘掉，各留一条 note");
    {
        const CursorChainGate g =
            FilterChainForCursor(WindowAll(), Request(CursorMode::kInclude, true), false, Os(19045));
        Check(Brief(g.chain) == "wgc", "窗口 auto 链只剩 wgc");
        Check(g.notes.size() == 3 && g.error.code.empty(), "被摘掉的三条各一条 note，不报错");
        bool allSkipped = true;
        for (const auto& n : g.notes) {
            if (n.code != codes::kNoteCursorChannelSkipped || n.option != L"--cursor" ||
                n.value != L"include" || n.backend.empty() || n.stage != stages::kCapture)
                allSkipped = false;
        }
        Check(allSkipped, "note 的形状：code/option/value/backend/stage 齐备且稳定");
        // 摘掉 printwindow 那一条写的是它自己的来源原因，不是笼统的一句"不支持"。
        const std::string first = Ascii(g.notes[0].message);
        Check(g.notes[0].backend == CaptureMethodName(CaptureMethod::kDwmThumbnail) &&
                  first.find(Ascii(cursor_reason::kDwmSurface)) != std::string::npos,
              "摘掉 dwm 那条写的原因是 dwm_redirection_surface");
        const std::string third = Ascii(g.notes[2].message);
        Check(g.notes[2].backend == CaptureMethodName(CaptureMethod::kBitBlt) &&
                  third.find(Ascii(cursor_reason::kScreenDc)) != std::string::npos,
              "摘掉 bitblt 那条写的原因是 screen_dc_has_no_pointer");

        const CursorChainGate s =
            FilterChainForCursor(ScreenAll(), Request(CursorMode::kInclude, true), true, Os(19045));
        Check(Brief(s.chain) == "wgc" && s.notes.size() == 2,
              "屏幕 auto 链同样只剩 wgc（摘掉 duplication 与 bitblt 两条）");
        bool pointerReason = false;
        for (const auto& n : s.notes)
            if (n.backend == L"duplication" &&
                Ascii(n.message).find(Ascii(cursor_reason::kPointerMetadata)) != std::string::npos)
                pointerReason = true;
        Check(pointerReason, "屏幕链里摘掉 duplication 那条说的是指针形状是独立元数据这件事");
    }

    Section("include + 显式指定做不到的那条：报错、链为空、绝不换成别的通道");
    {
        const CaptureMethod ones[] = {CaptureMethod::kBitBlt, CaptureMethod::kDuplication,
                                      CaptureMethod::kPrintWindow, CaptureMethod::kDwmThumbnail};
        bool allBlocked = true;
        for (const CaptureMethod m : ones) {
            const CursorChainGate g = FilterChainForCursor(
                std::vector<CaptureMethod>{m}, Request(CursorMode::kInclude, true), false, Os(19045));
            if (!(g.chain.empty() && g.error.code == codes::kEnvCursorUnsupported &&
                  g.error.stage == stages::kCapture && g.error.option == L"--cursor" &&
                  g.error.value == L"include"))
                allBlocked = false;
        }
        Check(allBlocked, "显式那条做不到 -> 一条错误、链为空（不换后端，也不交一张光标不对的图）");
    }

    Section("exclude：有开关的去设，没开关的靠来源成立，链不缩");
    {
        const CursorChainGate g =
            FilterChainForCursor(WindowAll(), Request(CursorMode::kExclude, true), false, Os(19045));
        Check(Brief(g.chain) == "wgc,dwm,printwindow,bitblt" && g.notes.empty() &&
                  g.error.code.empty(),
              "exclude 在 19045 上一条都不摘（那几条本来就没有光标，wgc 那条去设开关）");
        // 但"有开关却这台机器问不到开关"的那条不敢声称排除成立：wgc 单独指定时会被挡。
        const CursorChainGate w = FilterChainForCursor({CaptureMethod::kWgc},
                                                       Request(CursorMode::kExclude, true), false,
                                                       Os(18362));
        Check(w.chain.empty() && w.error.code == codes::kEnvCursorUnsupported,
              "exclude 配 wgc 在 18362 也是错误：问不到开关就等于不敢说这张图里没有光标");
        Check(!w.notes.empty() &&
                  Ascii(w.notes[0].message).find(Ascii(cursor_reason::kOsBelowMin)) !=
                      std::string::npos &&
                  Ascii(w.notes[0].message).find("19041") != std::string::npos,
              "那条 note 写的原因就是 os_below_min_build，并把那道门槛的数字一起带出来");
    }

    Section("版本门槛两侧各判一次（include 与 exclude 都要那个开关）");
    {
        const CursorChainGate below = FilterChainForCursor({CaptureMethod::kWgc},
                                                            Request(CursorMode::kInclude, true),
                                                            false, Os(19040));
        Check(below.chain.empty() && below.error.code == codes::kEnvCursorUnsupported,
              "19040 + include 配 wgc：挡在开工之前");
        const CursorChainGate at = FilterChainForCursor({CaptureMethod::kWgc},
                                                        Request(CursorMode::kInclude, true), false,
                                                        Os(19041));
        Check(Brief(at.chain) == "wgc" && at.error.code.empty(), "19041（含端点）放行");
        const CursorChainGate unknown = FilterChainForCursor(WindowAll(),
                                                             Request(CursorMode::kInclude, true),
                                                             false, Os(0, false));
        Check(Brief(unknown.chain) == "wgc",
              "版本问不出来时不按版本筛，但照结构筛：链里仍只剩 wgc（不放宽成四条）");
        const CursorChainGate unknownExclude = FilterChainForCursor(WindowAll(),
                                                                   Request(CursorMode::kExclude, true),
                                                                   false, Os(0, false));
        Check(Brief(unknownExclude.chain) == "wgc,dwm,printwindow,bitblt",
              "exclude 在版本问不出来时不摘 wgc（那一步自己交回真码，与 note.os_unverifiable 同源）");
    }

    Section("没登记的那条路径：include 与 exclude 都不敢声称");
    {
        // 现在没有通道落到未登记的路径上，所以这里判的是"一旦漏登记会怎样"：
        // 表查询那一问的下场 + 上面两条一致性判据（漏登记时先红在判据里，而不是红在一个
        // 静默放宽的默认值上）。
        const CursorReport askedUnregistered = MakeCursorReport(
            Request(CursorMode::kExclude, true), L"capture.brandnew", false, false);
        Check(CursorCapabilityOfPath(L"capture.brandnew") == CursorCapability::kUnregistered,
              "未登记的路径查出来是 unregistered");
        Check(askedUnregistered.written &&
                  askedUnregistered.effective == cursor_effective::kUnverified,
              "未登记 + 明确要求：effective 是 unverified，而不是假装照要求办了");
    }
}

// ---------------------------------------------------------------------------
// 5. 三道闸门串起来（版本优先，note 各份都留；这里只注入光标那一道）
// ---------------------------------------------------------------------------
void TestComposedGate() {
    Section("GateCaptureChain：先按版本筛，再按光标筛，最后按 HDR 筛；版本那条错误优先");
    {
        const ChannelGate g = GateCaptureChain(CaptureMethod::kAuto, false, Os(10240),
                                               Request(CursorMode::kInclude, true), NoHdr());
        Check(g.error.code == codes::kEnvCursorUnsupported && g.chain.empty(),
              "10240 上 auto + include：wgc 被版本挡掉，剩下的三条做不到光标 -> 光标那条错误");
        bool hasUnavailable = false, hasSkipped = false;
        for (const auto& n : g.notes) {
            if (n.code == codes::kNoteChannelUnavailable) hasUnavailable = true;
            if (n.code == codes::kNoteCursorChannelSkipped) hasSkipped = true;
        }
        Check(hasUnavailable && hasSkipped,
              "两份 note 都在：哪条被版本挡掉、哪条被光标要求摘掉，分开说得清");

        const ChannelGate lowOs = GateCaptureChain(CaptureMethod::kWgc, false, Os(9200),
                                                   Request(CursorMode::kInclude, true), NoHdr());
        Check(lowOs.error.code == codes::kEnvChannelUnsupported,
              "wgc 在 9200 连通道本身都不行 -> 版本那条错误优先（不另立一条光标的错）");
        Check(lowOs.chain.empty() && lowOs.notes.empty(), "错误那条链为空，也不补 note");

        const ChannelGate ok = GateCaptureChain(CaptureMethod::kAuto, false, Os(19045),
                                                Request(CursorMode::kDefault, false), NoHdr());
        Check(ok.error.code.empty() && Brief(ok.chain) == "wgc,dwm,printwindow,bitblt" &&
                  ok.notes.empty(),
              "没写 --cursor 时组合结果与 GateChannels 逐字相同（默认值真的不动任何东西）");

        const ChannelGate screen = GateCaptureChain(CaptureMethod::kAuto, true, Os(19045),
                                                   Request(CursorMode::kInclude, true), NoHdr());
        Check(Brief(screen.chain) == "wgc", "屏幕目标的 auto 链同样只剩 wgc");

        const ChannelGate explicitWgc = GateCaptureChain(CaptureMethod::kWgc, false, Os(19045),
                                                         Request(CursorMode::kExclude, true), NoHdr());
        Check(Brief(explicitWgc.chain) == "wgc" && explicitWgc.error.code.empty(),
              "显式 wgc + exclude 在 19045 放行（开关设得进去）");
    }
}

// ---------------------------------------------------------------------------
// 6. requested / effective / basis 那三个键
// ---------------------------------------------------------------------------
void TestCursorReport() {
    Section("结果里那三个键：各说一件事，问不出来就写 unverified");
    {
        // 没写 --cursor：三个键都不出现（与这条选项存在之前逐字节相同）。
        const CursorReport none =
            MakeCursorReport(Request(CursorMode::kDefault, false), paths::kWgc, false, false);
        Check(!none.written && none.requested.empty() && none.effective.empty() &&
                  none.basis.empty(),
              "没写 --cursor：written=false，三个值全空");

        const CursorReport dIn = MakeCursorReport(Request(CursorMode::kDefault, true), paths::kWgc,
                                                  true, true);
        Check(dIn.written && dIn.requested == L"default" &&
                  dIn.effective == cursor_effective::kInclude && dIn.basis == cursor_basis::kSessionRead,
              "default + wgc 读到画：effective=include，basis 说的是只读没设");

        const CursorReport dOut = MakeCursorReport(Request(CursorMode::kDefault, true), paths::kWgc,
                                                   true, false);
        Check(dOut.effective == cursor_effective::kExclude && dOut.basis == cursor_basis::kSessionRead,
              "default + wgc 读到不画：照实报 exclude（不折成任何一种）");

        const CursorReport dUnknown = MakeCursorReport(Request(CursorMode::kDefault, true), paths::kWgc,
                                                      false, false);
        Check(dUnknown.effective == cursor_effective::kUnverified &&
                  dUnknown.basis == cursor_basis::kPropertyUnavailable,
              "default + 那一问没答案：unverified，不是 exclude 也不是 include");

        const CursorReport setOut = MakeCursorReport(Request(CursorMode::kExclude, true), paths::kWgc,
                                                     true, false);
        Check(setOut.requested == L"exclude" && setOut.effective == cursor_effective::kExclude &&
                  setOut.basis == cursor_basis::kSessionSet,
              "exclude + wgc 设过并读回：basis 说的是设过这件事");

        const CursorReport setIn = MakeCursorReport(Request(CursorMode::kInclude, true), paths::kWgc,
                                                    true, true);
        Check(setIn.effective == cursor_effective::kInclude && setIn.basis == cursor_basis::kSessionSet,
              "include + wgc 设过并读回：include");

        // 明确要求过却问不到开关：这一张根本不会落地（闸门与通道那条先报错），但报告本体
        // 也要能诚实地说"不知道"，不许在渲染层再兜一次"那就写 exclude"。
        const CursorReport askedUnknown =
            MakeCursorReport(Request(CursorMode::kExclude, true), paths::kWgc, false, false);
        Check(askedUnknown.effective == cursor_effective::kUnverified &&
                  askedUnknown.basis == cursor_basis::kPropertyUnavailable,
              "exclude + 问不到开关：unverified（不冒充已排除）");

        const wchar_t* noSwitchPaths[] = {paths::kPrintWindow, paths::kDwmThumbnail, paths::kDwmScreen,
                                          paths::kBitBltScreen, paths::kScreenBitBlt,
                                          paths::kDuplicationFrame, paths::kScreenDuplication};
        bool allPathBasis = true;
        for (const wchar_t* path : noSwitchPaths) {
            const CursorReport r =
                MakeCursorReport(Request(CursorMode::kExclude, true), path, false, false);
            if (!(r.effective == cursor_effective::kExclude && r.basis == cursor_basis::kPathExcludes))
                allPathBasis = false;
        }
        Check(allPathBasis,
              "没有开关的那几条：exclude 的结论来自来源这件事，basis 写 path_excludes_cursor");

        const CursorReport unregistered =
            MakeCursorReport(Request(CursorMode::kDefault, true), paths::kUnknown, false, false);
        Check(unregistered.effective == cursor_effective::kUnverified &&
                  unregistered.basis == cursor_basis::kPropertyUnavailable,
              "未登记的路径（含帧里没填 path 的兜底名）不作任何断言");

        // 机器名不重复：effective 与 basis 两套取值各自互不相同，调用方能按它们分支。
        Check(std::wstring(cursor_effective::kInclude) != cursor_effective::kExclude &&
                  std::wstring(cursor_effective::kExclude) != cursor_effective::kUnverified &&
                  std::wstring(cursor_basis::kSessionSet) != cursor_basis::kSessionRead &&
                  std::wstring(cursor_basis::kSessionRead) != cursor_basis::kPathExcludes &&
                  std::wstring(cursor_basis::kPathExcludes) != cursor_basis::kPropertyUnavailable,
              "三值与四个 basis 的机器名互不相同（只增不改名的前提）");
    }
}

// ---------------------------------------------------------------------------
// 7. 解析层：直接调 ParseCommandLine，不起进程也不截图
// ---------------------------------------------------------------------------
void TestParseLayer() {
    Section("解析层：取值、最后一个生效、与查询互斥、include 配做不到的通道");
    SetLanguage(Language::kZhCn);
    const std::wstring prog{L"ecapture"};

    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"include", L"--capture", L"bitblt", L"out.png"});
        Check(!r.ok && HasCode(r.errors, codes::kCursorUnsupported),
              "include 配显式 bitblt：解析期 capture.cursor_unsupported（退出码 1，不换后端）");
        const bool shaped = !r.errors.empty() && r.errors[0].option == L"--cursor" &&
                            r.errors[0].value == L"include" && !r.errors[0].hint.empty() &&
                            r.errors[0].message.find(L"bitblt") != std::wstring::npos &&
                            r.errors[0].message.find(L"wgc") != std::wstring::npos;
        Check(shaped, "那条错误带 option/value/hint，message 里有实际通道与能做到的那条");
    }
    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"include", L"--capture", L"auto", L"out.png"});
        Check(r.ok && r.options.cursor.mode == CursorMode::kInclude,
              "include 配 auto 在解析期放行（做不到的由闸门从链里摘）");
    }
    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"exclude", L"--capture", L"printwindow", L"out.png"});
        Check(r.ok && r.options.cursor.mode == CursorMode::kExclude,
              "exclude 配 printwindow 放行（那条来源本来就没有光标）");
    }
    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"watery", L"out.png"});
        Check(!r.ok && HasCode(r.errors, codes::kInvalidValue) && !r.errors.empty() &&
                  r.errors[0].option == L"--cursor" && r.errors[0].value == L"watery" &&
                  r.errors[0].hint.find(L"default") != std::wstring::npos &&
                  r.errors[0].hint.find(L"exclude") != std::wstring::npos,
              "非法取值：cli.invalid_value + 原样 value + hint 列全三种，不退化成 default");
    }
    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor=INCLUDE",
                                   L"out.png"});
        Check(r.ok && r.options.cursor.given && r.options.cursor.mode == CursorMode::kInclude,
              "内联写法与大写取值都认（与 --capture / --format 同一套写法）");
    }
    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"include", L"--cursor", L"exclude", L"out.png"});
        Check(r.ok && r.options.cursor.mode == CursorMode::kExclude && r.options.cursor.given,
              "重复给出时最后一个生效");
    }
    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"default", L"out.png"});
        Check(r.ok && r.options.cursor.given && r.options.cursor.mode == CursorMode::kDefault,
              "显式 default 记 given=true（结果里要报读到的状态），但它不构成任何要求");
    }
    {
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"out.png"});
        Check(!r.options.cursor.given && r.options.cursor.mode == CursorMode::kDefault,
              "整条选项没写：given=false / mode=default（这就是兼容的默认值）");
    }
    {
        const ParseResult r = Run({prog, L"--cursor"});
        Check(!r.ok && HasCode(r.errors, codes::kMissingValue),
              "--cursor 是必带取值的选项：argv 到头就报 cli.missing_value");
    }
    {
        const ParseResult r = Run({prog, L"--capabilities", L"--cursor", L"include"});
        Check(!r.ok && HasCode(r.errors, codes::kQueryConflict),
              "--capabilities + --cursor = cli.query_conflict（环境查询只接受 --lang / -v / -q）");
        const ParseResult w = Run({prog, L"--list", L"--cursor", L"exclude"});
        Check(!w.ok && HasCode(w.errors, codes::kWindowQueryConflict),
              "--list + --cursor = cli.window_query_conflict（窗口查询不截图，没有光标可要求）");
    }
    {
        // 位置参数与取值的抢占：--cursor 必带走值，所以 --cursor out.png 里 out.png 被吃掉，
        // 结果是"取值不合法"而不是"悄悄少了一个输出路径"。
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"out.png"});
        Check(!r.ok && HasCode(r.errors, codes::kInvalidValue) && !r.errors.empty() &&
                  r.errors[0].value == L"out.png",
              "取值要吃值：--cursor out.png 里的 out.png 被当取值并当场报错（不变成输出路径）");
    }
}

// ---------------------------------------------------------------------------
// 8. 文案：新键在四种语言都渲染得出、没有 ?key 残留
// ---------------------------------------------------------------------------
void TestStrings() {
    Section("文案：新增的键都渲染得出，占位符都代入了");
    const wchar_t* keys[] = {L"opt.cursor", L"cli.cursor_value", L"cap.cursor_unsupported",
                             L"cap.cursor_unsupported_hint", L"cap.cursor_unverifiable",
                             L"cap.cursor_unverifiable_hint", L"env.cursor_unsupported",
                             L"env.cursor_unsupported_hint", L"note.cursor_channel_skipped"};
    bool all = true;
    for (const wchar_t* k : keys) {
        const std::wstring m = Msg(k);
        if (m.empty() || m == L"?" + std::wstring(k)) {
            std::printf("        缺文案: %s\n", Ascii(k).c_str());
            all = false;
        }
    }
    Check(all, "九条新键在当前语言都取得到（四语 key 与占位符对齐由 scripts\\check-lang.ps1 判）");

    const std::wstring m1 = Msgf(L"cap.cursor_unverifiable", L"exclude", L"read_back_mismatch",
                                 L"include");
    const std::wstring m2 = Msgf(L"note.cursor_channel_skipped", L"dwm", L"include",
                                 cursor_reason::kDwmSurface);
    const std::wstring m3 = Msgf(L"env.cursor_unsupported", L"include", L"wgc, dwm",
                                 L"os_below_min_build:19041");
    Check(m1.find(L"exclude") != std::wstring::npos &&
              m1.find(L"read_back_mismatch") != std::wstring::npos && m1.find(L'%') == std::wstring::npos,
          "cap.cursor_unverifiable 的三个占位符都代入，没有残留 %");
    Check(m2.find(L"dwm") != std::wstring::npos && m2.find(L'%') == std::wstring::npos,
          "note.cursor_channel_skipped 的三个占位符都代入");
    Check(m3.find(L"19041") != std::wstring::npos && m3.find(L'%') == std::wstring::npos,
          "env.cursor_unsupported 把那道门槛的数字也带出来了");
}

}  // namespace

int main() {
    SetLanguage(Language::kZhCn);
    TestRegistry();
    TestTablesAgree();
    TestVersionFloor();
    TestChainGate();
    TestComposedGate();
    TestCursorReport();
    TestParseLayer();
    TestStrings();
    std::printf("\ncursor: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
