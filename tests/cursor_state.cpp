// EvernightCapture - 光标包含与排除（--cursor）的离线判据
//
// 这一批判据要的现场是「这台机器的 Windows 版本正好卡在那道门槛两侧」「显式指定的那条通道
// 做不到这个光标要求」「设完读回来是相反的那一件」「某条路径忘了登记」—— 本机只有一台
// Windows 10 22H2（19045）的开发机，降不了级，也绝不能为了看「没有那个开关时会怎样」去
// 动真机的 WGC。所以判据本体（src/CursorControl.cpp 的 FilterChainForCursor /
// GateCaptureChain，以及 src/CursorControl.h 那张按路径登记的表与 MakeCursorReport）
// 在这里被逐条注入判：假版本、假通道链、假帧读数。
//
// 另外三条是「两份表不许各说一套」的现场核对：
//   * 光标表覆盖 src/CaptureScope.cpp 登记表里的每一条路径（漏一条 = 该路径按未登记处理）
//   * 通道级的 ChannelHasCursorSwitch / ChannelGuaranteesCursorExclusion /
//     ChannelPointerMayBeInImage 与路径表的三种取值对每个通道、两种目标都对得上
//   * 解析期那一句与运行期筛链那一句是同一个判据（同一个组合两边都得拒）
// 还有解析层那一段：直接调 ParseCommandLine，所以「include 配 printwindow 在解析期就拒」
// 与「exclude 配桌面复制不再拿到一次无根据的成功」这件事不必起进程、也不必真去截图就能判。
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
    Section("登记表：两条 wgc 有开关，三条来源没有光标，两条桌面复制没有答案，没登记的一律 unregistered");
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
    // 这两条过去登记成 excludes_cursor，凭的是"指针是独立元数据而本工具从不合成它"。
    // 那句证明不了帧里没有指针像素（官方说明允许指针已经画在那幅桌面图像上），所以改成
    // 一个说"这一问没有答案"的状态，而不是把条目删掉让它退回 unregistered（规矩 5）。
    Check(CursorCapabilityOfPath(paths::kDuplicationFrame) == C::kPointerStateUnverified,
          "duplication.frame = pointer_state_unverified（不再声称来源没有光标）");
    Check(CursorCapabilityOfPath(paths::kScreenDuplication) == C::kPointerStateUnverified,
          "screen.duplication = pointer_state_unverified");
    Check(CursorCapabilityOfPath(paths::kUnknown) == C::kUnregistered, "unknown = unregistered");
    Check(CursorCapabilityOfPath(L"brand.new") == C::kUnregistered, "没登记的名字 = unregistered");
    Check(CursorCapabilityOfPath(L"") == C::kUnregistered, "空路径 = unregistered（不是 settable）");
    Check(CursorCapabilityOfPath(nullptr) == C::kUnregistered, "空指针 = unregistered，不崩");
    // 漏登记的症状是"更严"：未登记时 include 与 exclude 都**不敢**声称做到。
    Check(CursorReasonOfPath(L"brand.new") == cursor_reason::kNotRegistered,
          "未登记的路径给 not_registered 这个原因");
    Check(CursorReasonOfPath(paths::kDuplicationFrame) == cursor_reason::kPointerUnverified,
          "桌面复制那条的原因写的是这一问没有答案（不是 pointer_shape_is_separate_metadata 那一句）");
    Check(CursorReasonOfPath(paths::kScreenDuplication) == cursor_reason::kPointerUnverified,
          "整屏那一条与窗口那一条同一个来源原因");
    // 两个 token 分得开"来源这件事"与"被 exclude 要求筛掉时的下场"（与 HDR 那一对同一种分工）。
    Check(std::wstring(cursor_reason::kPointerUnverified) != cursor_reason::kExcludeUnprovable,
          "desktop_frame_pointer_state_unverified 与 duplication_cursor_exclusion_unprovable 是两个 token");
    Check(std::wstring(CursorCapabilityName(C::kSettable)) == L"settable" &&
              std::wstring(CursorCapabilityName(C::kExcludesCursor)) == L"excludes_cursor" &&
              std::wstring(CursorCapabilityName(C::kPointerStateUnverified)) ==
                  L"pointer_state_unverified" &&
              std::wstring(CursorCapabilityName(C::kUnregistered)) == L"unregistered",
          "capability 的四个机器名齐备且互不相同");
    Check(std::wstring(CursorCapabilityName(C::kPointerStateUnverified)) !=
              std::wstring(CursorCapabilityName(C::kExcludesCursor)) &&
              std::wstring(CursorCapabilityName(C::kPointerStateUnverified)) !=
                  std::wstring(CursorCapabilityName(C::kUnregistered)),
          "新状态与 excludes_cursor、unregistered 两种都不重名（调用方要能分开）");
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

    // 通道级那两句新判断同样不许自立一套：对每个通道、两种目标各查一次表。
    //   ChannelGuaranteesCursorExclusion = 这条通道敢不敢声称"交回的图没有光标"
    //     —— 撑得起的是 kSettable（去设开关）与 kExcludesCursor（来源没有），别一种都不行。
    //   ChannelPointerMayBeInImage = 这条通道的来源可不可能已经把指针画在画面里
    //     —— 只许与 kPointerStateUnverified 同真同假。
    bool excludeAgrees = true;
    bool embedAgrees = true;
    int reachable = 0;
    for (const CaptureMethod m : methods) {
        for (const bool screen : {false, true}) {
            const wchar_t* path = screen ? ScreenPathOf(m) : WindowPathOf(m);
            // dwm / printwindow 在屏幕目标上没有路径可走（解析期就报 cap.unsupported_for_screen，
            // ScreenPathOf 那一条兜底名说的正是这件事）。那种组合这里不参与对照，但下面单独钉住
            // "没路径 = 真的被挡在屏幕目标之外"，免得这个跳过把漏登记也一起藏起来。
            if (std::wstring(path) == paths::kUnknown) {
                Check(screen && (m == CaptureMethod::kDwmThumbnail ||
                                 m == CaptureMethod::kPrintWindow),
                      "落到兜底名的只有 dwm / printwindow 的屏幕组合（跳过对照不是因为漏登记）");
                continue;
            }
            ++reachable;
            const CursorCapability cap = CursorCapabilityOfPath(path);
            const bool want = cap == CursorCapability::kSettable ||
                              cap == CursorCapability::kExcludesCursor;
            const bool may = cap == CursorCapability::kPointerStateUnverified;
            if (ChannelGuaranteesCursorExclusion(m) != want) {
                std::printf("        打脸: %s/%s exclude=%d 表里=%d\n",
                            Ascii(CaptureMethodName(m)).c_str(), screen ? "screen" : "window",
                            ChannelGuaranteesCursorExclusion(m) ? 1 : 0, want ? 1 : 0);
                excludeAgrees = false;
            }
            if (ChannelPointerMayBeInImage(m) != may) {
                std::printf("        打脸: %s/%s mayBeInImage=%d 表里=%d\n",
                            Ascii(CaptureMethodName(m)).c_str(), screen ? "screen" : "window",
                            ChannelPointerMayBeInImage(m) ? 1 : 0, may ? 1 : 0);
                embedAgrees = false;
            }
        }
    }
    Check(reachable == 8, "八条真走得通的通道×目标组合都参与了对照（漏登记不会被那个跳过藏住）");
    Check(excludeAgrees,
          "ChannelGuaranteesCursorExclusion 与路径表逐条相同（exclude 敢不敢声称只有一个答案）");
    Check(embedAgrees, "ChannelPointerMayBeInImage 与路径表的 pointer_state_unverified 逐条相同");
    // 两句判断不许同时为真：那样解析期的文案选择就自相矛盾（既说"来源没有光标"又说"可能已含"）。
    bool disjoint = true;
    for (const CaptureMethod m : methods)
        if (ChannelGuaranteesCursorExclusion(m) && ChannelPointerMayBeInImage(m)) disjoint = false;
    Check(disjoint, "同一通道不可能既保证得了 exclude 又属于来源可能已含指针那一种（两句判断互斥）");

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
    // exclude 那一路现在的形状：有开关的那条 + 来源没有光标的那三条放行；桌面复制那条**不放行**
    //（不是"来源没有光标"，而是"这一问没有答案"，所以不能给它一次无根据的成功）；auto 交给闸门。
    Check(CursorRequestPossible(CaptureMethod::kWgc, CursorMode::kExclude), "wgc 的 exclude 放行");
    bool excludeKept = true;
    for (const CaptureMethod m : {CaptureMethod::kDwmThumbnail, CaptureMethod::kPrintWindow,
                                  CaptureMethod::kBitBlt})
        if (!CursorRequestPossible(m, CursorMode::kExclude)) excludeKept = false;
    Check(excludeKept, "printwindow / dwm / bitblt 的 exclude 仍放行（来源确实没有光标）");
    Check(!CursorRequestPossible(CaptureMethod::kDuplication, CursorMode::kExclude),
          "duplication 的 exclude 现在解析期就拒（旧行为是给一次无根据的成功）");
    Check(CursorRequestPossible(CaptureMethod::kAuto, CursorMode::kExclude),
          "auto 的 exclude 也交给闸门筛链，不在这里下结论");
    Check(CursorRequestPossible(CaptureMethod::kDuplication, CursorMode::kDefault) &&
              CursorRequestPossible(CaptureMethod::kBitBlt, CursorMode::kDefault) &&
              CursorRequestPossible(CaptureMethod::kAuto, CursorMode::kDefault),
          "default 对每条通道都放行（它不构成任何要求，图照旧交）");
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
                Ascii(n.message).find(Ascii(cursor_reason::kPointerUnverified)) != std::string::npos)
                pointerReason = true;
        Check(pointerReason,
              "屏幕链里摘掉 duplication 那条写的是这一问没有答案，而不是独立元数据那一句");
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

    Section("exclude：有开关的去设，来源没有光标的靠来源成立，链不缩");
    {
        const CursorChainGate g =
            FilterChainForCursor(WindowAll(), Request(CursorMode::kExclude, true), false, Os(19045));
        Check(Brief(g.chain) == "wgc,dwm,printwindow,bitblt" && g.notes.empty() &&
                  g.error.code.empty(),
              "exclude 在 19045 上一条都不摘（那条窗口链里本来就没有桌面复制）");
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

    Section("exclude + 桌面复制：这一问没有答案的那条不能拿到一次无根据的成功");
    {
        // 屏幕 auto 链（本机形状：wgc -> duplication -> bitblt）里摘掉 duplication，剩下的照旧回退。
        const CursorChainGate s =
            FilterChainForCursor(ScreenAll(), Request(CursorMode::kExclude, true), true, Os(19045));
        Check(Brief(s.chain) == "wgc,bitblt" && s.error.code.empty(),
              "屏幕链里 exclude 把 duplication 摘掉，链没空（不是不截，而是换一条敢声称的）");
        Check(s.notes.size() == 1 && s.notes[0].backend == L"duplication" &&
                  s.notes[0].code == codes::kNoteCursorChannelSkipped &&
                  s.notes[0].option == L"--cursor" && s.notes[0].value == L"exclude" &&
                  s.notes[0].stage == stages::kCapture,
              "摘掉那条留一条形状稳定的 note（value 是 exclude，不是 include）");
        Check(Ascii(s.notes[0].message).find(Ascii(cursor_reason::kExcludeUnprovable)) !=
                  std::string::npos,
              "exclude 那一路写的原因是 duplication_cursor_exclusion_unprovable（与来源 token 分开）");

        // 窗口目标上真点名 duplication（那条通道对窗口也能用，只是不在默认链里）：同样摘不剩 = 错误。
        const CursorChainGate one = FilterChainForCursor({CaptureMethod::kDuplication},
                                                        Request(CursorMode::kExclude, true), false,
                                                        Os(19045));
        Check(one.chain.empty() && one.error.code == codes::kEnvCursorUnsupported &&
                  one.error.option == L"--cursor" && one.error.value == L"exclude" &&
                  one.error.stage == stages::kCapture,
              "显式 duplication + exclude：一条错误、链为空，一张都不截（也不换后端）");
        // 混在链里时同样只减不加：闸门绝不把一条通道换进链，所以"要 exclude"这件事
        // 不会意外把桌面像素的范围放大（这里 kept 恒为 chain 的子集且顺序不变）。
        const std::vector<CaptureMethod> mixed = {CaptureMethod::kDuplication, CaptureMethod::kBitBlt};
        const CursorChainGate m = FilterChainForCursor(mixed, Request(CursorMode::kExclude, true),
                                                       true, Os(19045));
        Check(Brief(m.chain) == "bitblt" && m.error.code.empty() && m.notes.size() == 1,
              "链里只剩一条时也照实收窄（bitblt 那条来源没有光标，敢声称 exclude）");
        // 空链不是"成功"也不是第二条错误：上一步（版本闸门）已经说清楚了，这里原样交出。
        const CursorChainGate empty =
            FilterChainForCursor({}, Request(CursorMode::kExclude, true), true, Os(19045));
        Check(empty.chain.empty() && empty.error.code.empty() && empty.notes.empty(),
              "空候选链：不再补一条光标的错误（错误优先权在上一道闸门）");
        // 没写这条选项时链一条都不动 —— 默认截图行为保持原样，未知只由报告表达。
        const CursorChainGate plain =
            FilterChainForCursor(ScreenAll(), Request(CursorMode::kDefault, false), true, Os(19045));
        Check(Brief(plain.chain) == "wgc,duplication,bitblt" && plain.notes.empty(),
              "没写 --cursor 时屏幕链一条都不摘（duplication 照旧可用）");
        const CursorChainGate def =
            FilterChainForCursor(ScreenAll(), Request(CursorMode::kDefault, true), true, Os(19045));
        Check(Brief(def.chain) == "wgc,duplication,bitblt" && def.notes.empty() &&
                  def.error.code.empty(),
              "显式 --cursor default 也不摘 duplication（它不构成要求，只在结果里报 unverified）");
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

        // 组合那一份也要给出与单道闸门同一个答案：屏幕 auto + exclude 摘掉保证不了的那条。
        const ChannelGate screenExclude = GateCaptureChain(CaptureMethod::kAuto, true, Os(19045),
                                                          Request(CursorMode::kExclude, true),
                                                          NoHdr());
        Check(Brief(screenExclude.chain) == "wgc,bitblt" && screenExclude.error.code.empty(),
              "组合闸门里屏幕 auto + exclude = wgc,bitblt（与 FilterChainForCursor 同源，不两个答案）");
        Check(screenExclude.notes.size() == 1 && screenExclude.notes[0].backend == L"duplication",
              "收窄留的那条 note 也从组合那一份里看得见");

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
                                          paths::kBitBltScreen, paths::kScreenBitBlt};
        bool allPathBasis = true;
        for (const wchar_t* path : noSwitchPaths) {
            const CursorReport r =
                MakeCursorReport(Request(CursorMode::kExclude, true), path, false, false);
            if (!(r.effective == cursor_effective::kExclude && r.basis == cursor_basis::kPathExcludes))
                allPathBasis = false;
        }
        Check(allPathBasis,
              "来源没有光标的那几条：exclude 的结论来自来源这件事，basis 写 path_excludes_cursor");

        // 桌面复制那两条：明确要求 exclude 的调用根本走不到这里（解析期与筛链都挡了），
        // 但报告本体也不许自己兜一次"那就写 exclude"。三种要求 + 两种目标各判一次。
        const wchar_t* dupPaths[] = {paths::kDuplicationFrame, paths::kScreenDuplication};
        for (const wchar_t* path : dupPaths) {
            for (const CursorMode mode : {CursorMode::kDefault, CursorMode::kInclude,
                                          CursorMode::kExclude}) {
                const CursorReport r = MakeCursorReport(Request(mode, true), path, false, false);
                Check(r.written && r.effective == cursor_effective::kUnverified &&
                          r.basis == cursor_basis::kPathPointerUnverified,
                      "桌面复制那两条在任何光标要求下都只写 unverified + path_pointer_state_unverified");
            }
        }
        // fake 帧读数也改不了这件事：那两个值对非 wgc 路径本来就不该被当成"读过开关"，
        // 这里给 true/true 是要钉住"报告不拿一个来历不明的读数冒充设过"。
        const CursorReport dupFakeRead =
            MakeCursorReport(Request(CursorMode::kDefault, true), paths::kScreenDuplication,
                             /*frameStateKnown=*/true, /*frameCursorIn=*/true);
        Check(dupFakeRead.effective == cursor_effective::kUnverified &&
                  dupFakeRead.basis == cursor_basis::kPathPointerUnverified,
              "桌面复制那条即使帧上带了读数也仍写 unverified（登记表说的不是那次问答）");

        const CursorReport unregistered =
            MakeCursorReport(Request(CursorMode::kDefault, true), paths::kUnknown, false, false);
        Check(unregistered.effective == cursor_effective::kUnverified &&
                  unregistered.basis == cursor_basis::kPathNotRegistered,
              "未登记的路径（含帧里没填 path 的兜底名）不作任何断言，且 basis 与上面那条分开");
        // "没查过"与"查过而保证不了"必须落在两个不同的 basis 上（规矩 5）：调用方按它分支时
        // 下一步不一样 —— 前者要补登记，后者是这条路线本身的限制。
        Check(std::wstring(cursor_basis::kPathNotRegistered) != cursor_basis::kPathPointerUnverified,
              "unregistered 与 pointer_state_unverified 的 basis 不重名");

        // 机器名不重复：effective 与 basis 两套取值各自互不相同，调用方能按它们分支。
        Check(std::wstring(cursor_effective::kInclude) != cursor_effective::kExclude &&
                  std::wstring(cursor_effective::kExclude) != cursor_effective::kUnverified &&
                  std::wstring(cursor_basis::kSessionSet) != cursor_basis::kSessionRead &&
                  std::wstring(cursor_basis::kSessionRead) != cursor_basis::kPathExcludes &&
                  std::wstring(cursor_basis::kPathExcludes) != cursor_basis::kPropertyUnavailable &&
                  std::wstring(cursor_basis::kPropertyUnavailable) !=
                      cursor_basis::kPathPointerUnverified &&
                  std::wstring(cursor_basis::kPathPointerUnverified) !=
                      cursor_basis::kPathNotRegistered,
              "三值与五个 basis 的机器名互不相同（只增不改名的前提）");
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
        // 这一条是本次修正的核心现场：旧行为是给一次无根据的成功（图落地、结果里写 exclude），
        // 现在解析期就拒，退出码 1（同一条码），一个像素都不取。
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"exclude", L"--capture", L"duplication", L"out.png"});
        Check(!r.ok && HasCode(r.errors, codes::kCursorUnsupported),
              "exclude 配显式 duplication：解析期 capture.cursor_unsupported（不再无根据放行）");
        Check(!r.errors.empty() && r.errors[0].option == L"--cursor" &&
                  r.errors[0].value == L"exclude" && !r.errors[0].hint.empty() &&
                  r.errors[0].stage.empty(),
              "那条错误的 option/value/hint 齐备，stage 仍是解析期那一条（码与阶段都没变）");
        // 文案与判据同源：这一句用的必须是"这条路线保证不了"那一条，而不是"来源没有光标"那一条。
        const std::wstring want = Msgf(L"cap.cursor_unsupported_unprovable", L"exclude",
                                       L"duplication", L"wgc, dwm, printwindow, bitblt");
        Check(!r.errors.empty() && r.errors[0].message == want,
              "exclude 配 duplication 走 unprovable 那一句文案，建议清单是那四条（与判据同一条）");
        const std::wstring other = Msgf(L"cap.cursor_unsupported", L"exclude", L"duplication",
                                        L"wgc, dwm, printwindow, bitblt");
        Check(!r.errors.empty() && r.errors[0].message != other,
              "同一条码下没把这句假话说出去（来源没有光标那句不适用于桌面复制）");
    }
    {
        // 屏幕目标上被拒的是 duplication，建议里不该出现根本用不了屏幕目标的两条通道。
        const ParseResult r = Run({prog, L"--monitor", L"primary", L"--dry-run", L"--cursor",
                                   L"exclude", L"--capture", L"duplication", L"out.png"});
        Check(!r.ok && HasCode(r.errors, codes::kCursorUnsupported),
              "屏幕目标 + exclude 配 duplication：同一条码在解析期拒（与窗口目标同一个判据）");
        const std::wstring want = Msgf(L"cap.cursor_unsupported_unprovable", L"exclude",
                                       L"duplication", L"wgc, bitblt");
        Check(!r.errors.empty() && r.errors[0].message == want,
              "屏幕目标那条建议里只列屏幕目标真用得了的通道（不推荐用不了的路线）");
    }
    {
        // include 配 duplication 也还是这一条码，而下场与 exclude 一样：都不落地。
        const ParseResult r = Run({prog, L"--class", L"Shell_TrayWnd", L"--dry-run", L"--cursor",
                                   L"include", L"--capture", L"duplication", L"out.png"});
        Check(!r.ok && HasCode(r.errors, codes::kCursorUnsupported) && r.errors.size() == 1,
              "include 配 duplication：一条错误，不叠第二条（改判据时别把同一件事说两遍）");
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
                             L"cap.cursor_unsupported_hint", L"cap.cursor_unsupported_unprovable",
                             L"cap.cursor_unsupported_unprovable_hint", L"cap.cursor_unverifiable",
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
    Check(all, "十一条新键在当前语言都取得到（四语 key 与占位符对齐由 scripts\\check-lang.ps1 判）");
    // 两句"做不到"必须真的不是同一句：同一码下拿"来源没有光标"去说桌面复制那条是假话。
    Check(std::wstring(Msg(L"cap.cursor_unsupported")) !=
              std::wstring(Msg(L"cap.cursor_unsupported_unprovable")),
          "cap.cursor_unsupported 与 _unprovable 是两句不同的话");

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
