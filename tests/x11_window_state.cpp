// 【Linux / X11】窗口查询、几何与身份复核判据。
//
// 这一份只使用本任务自建的 override-redirect 测试窗口，不需要真实窗口管理器，也不读取
// 屏幕像素、不弹确认框、不激活或移动任何窗口。它判的是生产本体：
//   * x11::EnumerateClientWindows / QueryClientWindow / QueryDisplaySessionFacts
//   * WindowMatch 的 AND/OR 语义与 WindowQuery 的列表 / inspect 分流
//   * WindowIdentity 的 XID + 显示会话作用域复核
//
// 注：虚拟 X11（Xvfb）只能证明对应 X11 协议行为；真实窗口管理器对 reparent / EWMH
// 属性的处理仍需要在对应桌面上人工验证。本文件不把没有 WM 的测试窗口当成全部桌面行为。

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/Lang.h"
#include "../src/WindowIdentity.h"
#include "../src/WindowMatch.h"
#include "../src/WindowQuery.h"
#include "../src/WindowSource.h"
#include "../src/core/TextCoding.h"
#include "../src/platform/linux/X11Capture.h"

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
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

struct TestWindow {
    Window window = None;
    Window frame = None;
    bool mapped = false;

    TestWindow() = default;
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;
    TestWindow(TestWindow&& other) noexcept { *this = std::move(other); }
    TestWindow& operator=(TestWindow&& other) noexcept {
        window = other.window;
        frame = other.frame;
        mapped = other.mapped;
        other.window = None;
        other.frame = None;
        other.mapped = false;
        return *this;
    }
};

Display* OpenTestDisplay() {
    Display* display = XOpenDisplay(nullptr);
    return display;
}

void SetUtf8Title(Display* display, Window window, const std::string& utf8) {
    const Atom netName = XInternAtom(display, "_NET_WM_NAME", False);
    const Atom utf8String = XInternAtom(display, "UTF8_STRING", False);
    XChangeProperty(display, window, netName, utf8String, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(utf8.data()),
                    static_cast<int>(utf8.size()));
}

void SetClass(Display* display, Window window, const std::string& cls) {
    XClassHint hint{};
    std::string name = cls;
    hint.res_name = name.data();
    hint.res_class = name.data();
    XSetClassHint(display, window, &hint);
}

void SetPid(Display* display, Window window, uint32_t pid) {
    const Atom atom = XInternAtom(display, "_NET_WM_PID", False);
    long value = static_cast<long>(pid);
    XChangeProperty(display, window, atom, XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&value), 1);
}

Window CreateRaw(Display* display, Window parent, int x, int y, unsigned width, unsigned height,
                 bool overrideRedirect) {
    const Window root = DefaultRootWindow(display);
    XSetWindowAttributes attrs{};
    attrs.background_pixel = WhitePixel(display, DefaultScreen(display));
    attrs.border_pixel = BlackPixel(display, DefaultScreen(display));
    attrs.override_redirect = overrideRedirect ? True : False;
    attrs.event_mask = ExposureMask | StructureNotifyMask;
    return XCreateWindow(display, parent ? parent : root, x, y, width, height, 1,
                         CopyFromParent, InputOutput, CopyFromParent,
                         CWBackPixel | CWBorderPixel | CWOverrideRedirect | CWEventMask, &attrs);
}

TestWindow MakeTopLevel(Display* display, const std::string& title, const std::string& cls,
                        uint32_t pid, int x, int y, unsigned width, unsigned height) {
    TestWindow result;
    result.window = CreateRaw(display, None, x, y, width, height, true);
    if (!title.empty()) SetUtf8Title(display, result.window, title);
    if (!cls.empty()) SetClass(display, result.window, cls);
    if (pid != 0) SetPid(display, result.window, pid);
    XMapWindow(display, result.window);
    XSync(display, False);
    result.mapped = true;
    return result;
}

// 父窗口故意不带任何客户端属性：它代表窗口管理器装饰框。真正的客户端是 child。
TestWindow MakeDecorated(Display* display, const std::string& title, const std::string& cls,
                         uint32_t pid, int frameX, int frameY, unsigned frameW,
                         unsigned frameH) {
    TestWindow result;
    result.frame = CreateRaw(display, None, frameX, frameY, frameW, frameH, true);
    result.window = CreateRaw(display, result.frame, 10, 20, frameW > 20 ? frameW - 20 : 1,
                              frameH > 40 ? frameH - 40 : 1, false);
    if (!title.empty()) SetUtf8Title(display, result.window, title);
    if (!cls.empty()) SetClass(display, result.window, cls);
    if (pid != 0) SetPid(display, result.window, pid);
    XMapWindow(display, result.frame);
    XMapWindow(display, result.window);
    XSync(display, False);
    result.mapped = true;
    return result;
}

void WaitForWindows(Display* display) {
    // override-redirect 窗口不经过 WM；XSync 足以让服务端完成映射与属性更新。
    for (int i = 0; i < 3; ++i) XSync(display, False);
}

void Destroy(Display* display, TestWindow* window) {
    if (!display || !window) return;
    if (window->window != None) XDestroyWindow(display, window->window);
    if (window->frame != None) XDestroyWindow(display, window->frame);
    window->window = None;
    window->frame = None;
    window->mapped = false;
    XSync(display, False);
}

WindowQuerySpec ListSpec(bool includeIconic = false) {
    WindowQuerySpec spec;
    spec.action = WindowAction::kList;
    spec.includeIconic = includeIconic;
    spec.limit = 50;
    return spec;
}

WindowQuerySpec InspectSpec() {
    WindowQuerySpec spec;
    spec.action = WindowAction::kInspect;
    spec.inspectPolicy = MultiMatch::kAsk;
    spec.limit = 1;
    return spec;
}

MatchOutcome MatchByTitle(const std::string& utf8Title) {
    MatchOptions match;
    match.titleContains.push_back(DecodeUtf8(utf8Title.data(), utf8Title.size()).text);
    MatchRequest request;
    request.match = match;
    return EnumerateMatches(request);
}

bool HasHit(const MatchOutcome& outcome, Window window) {
    for (const auto& hit : outcome.hits) {
        if (hit.hwnd == static_cast<uint64_t>(window)) return true;
    }
    return false;
}

bool HasIconic(const MatchOutcome& outcome, Window window) {
    for (const auto& hit : outcome.iconic) {
        if (hit.hwnd == static_cast<uint64_t>(window)) return true;
    }
    return false;
}

const WindowInfo* FindHit(const MatchOutcome& outcome, Window window) {
    for (const auto& hit : outcome.hits) {
        if (hit.hwnd == static_cast<uint64_t>(window)) return &hit;
    }
    return nullptr;
}

std::wstring RenderList(const WindowQueryResult& result) {
    // 默认输出已经包含对象 / caveats；verbose 会额外回显输入（那里允许出现 --hwnd
    // 这个选项名本身），这里判的是窗口对象字段，所以不打开 input。
    return RenderWindowQuery(result, kWindowQueryContractName, false, false);
}

bool Contains(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

// ---------------------------------------------------------------------------
void TestUniqueAmbiguousAndChineseTitle(Display* display) {
    Section("唯一 / 歧义 / 中文标题 / 分页");
    const std::string title = "窗口查询 中文标题 唯一与歧义";
    TestWindow a = MakeTopLevel(display, title, "EcX11PairA", static_cast<uint32_t>(getpid()),
                                100, 120, 360, 240);
    TestWindow b = MakeTopLevel(display, title, "EcX11PairB", static_cast<uint32_t>(getpid()),
                                480, 120, 360, 240);
    WaitForWindows(display);

    const MatchOutcome both = MatchByTitle(title);
    Check(both.status == BlockedStatus::kOk, "按中文标题求值成功");
    Check(HasHit(both, a.window) && HasHit(both, b.window),
          "同类条件命中两扇自建窗口（不是截图那种立即歧义错误）");
    const WindowInfo* ah = FindHit(both, a.window);
    Check(ah != nullptr && ah->title == L"窗口查询 中文标题 唯一与歧义",
          "标题按 UTF-8 原样交回（中文没有丢失或乱码）");

    WindowQuerySnapshot snapshot;
    snapshot.hits = both.hits;
    snapshot.iconic = both.iconic;
    MatchOptions match;
    match.titleContains = {L"窗口查询 中文标题 唯一与歧义"};
    const WindowQueryResult listed =
        BuildWindowQueryResult(snapshot, ListSpec(), match, std::wstring());
    Check(listed.errors.empty() && listed.matched >= 2 && listed.windows.size() >= 2,
          "--list 把多匹配当正常结果分页交回");
    const std::wstring listDoc = RenderList(listed);
    Check(Contains(listDoc, L"\"contract\": \"windowquery\"") &&
              Contains(listDoc, L"\"xid\""),
          "列表契约使用 X11 的 xid 字段");
    Check(!Contains(listDoc, L"\"hwnd\""),
          "X11 列表不把 XID 写成 Windows 的 hwnd");
    Check(Contains(listDoc, L"窗口查询 中文标题 唯一与歧义"),
          "渲染后的中文标题仍原样存在");

    const WindowQueryResult inspected =
        BuildWindowQueryResult(snapshot, InspectSpec(), match, std::wstring());
    Check(inspected.exitCode == EX_AMBIGUOUS && !inspected.errors.empty() &&
              inspected.errors.front().code == codes::kAmbiguousWindow,
          "--inspect 遇到两扇命中报歧义，不替调用方挑一个");
    Check(inspected.windows.empty() && !inspected.hasTarget,
          "歧义时不交半套目标快照");

    Destroy(display, &a);
    Destroy(display, &b);
}

void TestDecorationAndSubwindowSelection(Display* display) {
    Section("客户端窗口 / 装饰框 / 子窗口选择原则");
    const std::string title = "装饰框测试 客户端才是目标";
    TestWindow decorated = MakeDecorated(display, title, "EcX11DecoratedClient",
                                         static_cast<uint32_t>(getpid()), 160, 160, 420, 300);
    XSync(display, False);
    const std::vector<x11::ClientWindowFacts> all = x11::EnumerateClientWindows(display);
    bool foundClient = false;
    bool foundFrame = false;
    for (const auto& facts : all) {
        if (facts.window == decorated.window) foundClient = true;
        if (facts.window == decorated.frame) foundFrame = true;
    }
    Check(foundClient, "枚举交回真正的客户端窗口");
    Check(!foundFrame, "装饰父框不因是 root 直系子窗口而被当成客户端");

    const MatchOutcome outcome = MatchByTitle(title);
    Check(HasHit(outcome, decorated.window), "条件命中客户端窗口");
    Check(!HasHit(outcome, decorated.frame), "条件不会命中装饰框");

    x11::ClientWindowFacts facts;
    Check(x11::QueryClientWindow(display, decorated.window, &facts),
          "单独查询客户端窗口成功");
    Check(facts.rectRead == ReadState::kReadable &&
              facts.frameRectRead == ReadState::kReadable,
          "客户区与外框几何分别可读");
    Check(facts.x == 172 && facts.y == 182 &&
              facts.width == 400 && facts.height == 260,
          "客户区根坐标与客户区尺寸来自 X11 事实");
    Check(facts.frameX == 160 && facts.frameY == 160 && facts.frameWidth == 422 &&
              facts.frameHeight == 302,
          "外框几何来自真实父窗口，而不是客户区冒充");
    Check(facts.frameRectSource == L"x11_parent_window" ||
              facts.frameRectSource == L"ewmh_net_frame_extents",
          "外框来源写清是结构父窗口或 EWMH 声明");
    Check(facts.session.known && facts.session.scope.find(L"x11|display=") == 0,
          "枚举时记录显示连接作用域");

    Destroy(display, &decorated);
}

void TestMissingPropertiesAndProcessAttribution(Display* display) {
    Section("属性缺失 / 声明的 PID / 进程归属不确定性");
    const std::string title = "属性缺失测试 只有标题";
    TestWindow bare = MakeTopLevel(display, title, "", 0, 220, 220, 280, 180);
    WaitForWindows(display);

    x11::ClientWindowFacts facts;
    Check(x11::QueryClientWindow(display, bare.window, &facts), "无类名 / 无 PID 的窗口仍可查询");
    Check(facts.titleRead == ReadState::kReadable && facts.title == L"属性缺失测试 只有标题",
          "存在的标题读取为 readable");
    Check(facts.pidRead == ReadState::kAbsent,
          "缺少 _NET_WM_PID 报 absent，不写成 PID=0 的真值");
    Check(facts.classRead == ReadState::kAbsent,
          "缺少 WM_CLASS 报 absent，不把一个空类名冒充有答案");
    Check(facts.processAttribution == ProcessAttribution::kUnknown,
          "没有 PID 时归属进程为 unknown");

    const MatchOutcome outcome = MatchByTitle(title);
    const WindowInfo* hit = FindHit(outcome, bare.window);
    Check(hit != nullptr, "只靠标题仍可定位属性缺失的窗口");
    if (hit) {
        Check(hit->pidRead == ReadState::kAbsent && hit->classRead == ReadState::kAbsent,
              "WindowInfo 保留逐字段缺失状态");
        Check(hit->processAttribution == ProcessAttribution::kUnknown,
              "WindowInfo 不把缺失 PID 升级成可信归属");
    }

    MatchOptions byProcess;
    byProcess.processes.push_back(L"anything");
    MatchRequest processRequest;
    processRequest.match = byProcess;
    const MatchOutcome processOutcome = EnumerateMatches(processRequest);
    Check(!HasHit(processOutcome, bare.window),
          "没有归属进程路径时 --process 不会误命中");

    WindowQuerySnapshot snapshot;
    snapshot.hits = outcome.hits;
    MatchOptions match;
    match.titleContains = {L"属性缺失测试 只有标题"};
    const WindowQueryResult result =
        BuildWindowQueryResult(snapshot, ListSpec(), match, std::wstring());
    const std::wstring doc = RenderList(result);
    Check(Contains(doc, L"\"pidSource\": \"ewmh_net_wm_pid_self_declared\"") ||
              Contains(doc, L"\"attribution\": \"unknown\""),
          "文档把 PID 来源 / 归属不确定性写成字段");
    Check(Contains(doc, L"x11_pid_is_self_declared_not_ownership_proof"),
          "caveat 明确说声明的 PID 不是归属证明");

    // 有 _NET_WM_PID 的窗口：进程事实能读，但归属仍然只标 declared_unverified。
    TestWindow withPid = MakeTopLevel(display, "声明PID测试", "EcX11DeclaredPid",
                                      static_cast<uint32_t>(getpid()), 520, 220, 280, 180);
    WaitForWindows(display);
    x11::ClientWindowFacts pidFacts;
    Check(x11::QueryClientWindow(display, withPid.window, &pidFacts), "声明 PID 的窗口可查询");
    Check(pidFacts.pidRead == ReadState::kReadable && pidFacts.pid == static_cast<uint32_t>(getpid()),
          "声明的 PID 原样读出");
    Check(pidFacts.processAttribution == ProcessAttribution::kDeclaredUnverified,
          "X11 的 PID 只标 declared_unverified，不冒充系统核实归属");
    Destroy(display, &withPid);
    Destroy(display, &bare);
}

void TestGeometryAndReadOnly(Display* display) {
    Section("几何 / 逻辑与像素坐标 / 查询只读");
    TestWindow w = MakeTopLevel(display, "几何与只读测试", "EcX11Geometry",
                                static_cast<uint32_t>(getpid()), 300, 260, 321, 222);
    WaitForWindows(display);

    XWindowAttributes before{};
    XGetWindowAttributes(display, w.window, &before);
    Window focusBefore = None;
    int revertBefore = 0;
    XGetInputFocus(display, &focusBefore, &revertBefore);

    x11::ClientWindowFacts facts;
    Check(x11::QueryClientWindow(display, w.window, &facts), "几何查询成功");
    Check(facts.rectRead == ReadState::kReadable && facts.x == 301 && facts.y == 261 &&
              facts.width == 321 && facts.height == 222,
          "客户区在根坐标中按 X11 像素给出");
    Check(facts.frameRectRead == ReadState::kReadable,
          "root 直系客户窗口的外框也可以可靠给出");
    Check(facts.frameRectSource == L"client_is_top_level" ||
              facts.frameRectSource == L"ewmh_net_frame_extents",
          "外框来源没有编造");

    PhysRect client{};
    Check(ClientScreenRectOf(w.window, &client) && client.left == 301 && client.top == 261,
          "ClientScreenRectOf 只交客户区");
    const PhysRect screen = WindowScreenRectOf(w.window);
    Check(screen.left == client.left && screen.top == client.top &&
              screen.right == client.right && screen.bottom == client.bottom,
          "WindowScreenRectOf 交的是交付图像对应的客户区，不是外框");
    const PhysRect frame = WindowFullRectOf(w.window);
    Check(frame.right > frame.left && frame.bottom > frame.top,
          "WindowFullRectOf 交的是可读外框，量不到时不会编");
    Check(frame.left < client.left && frame.right > client.right,
          "装饰 border 只进外框，不冒充客户区 / 交付图像矩形");

    XWindowAttributes after{};
    XGetWindowAttributes(display, w.window, &after);
    Window focusAfter = None;
    int revertAfter = 0;
    XGetInputFocus(display, &focusAfter, &revertAfter);
    Check(before.map_state == after.map_state && before.x == after.x && before.y == after.y &&
              before.width == after.width && before.height == after.height,
          "一次只读查询没有移动 / 缩放 / 映射窗口");
    Check(focusBefore == focusAfter && revertBefore == revertAfter,
          "一次只读查询没有抢输入焦点（不激活窗口）");

    const MatchOutcome outcome = MatchByTitle("几何与只读测试");
    WindowQuerySnapshot snapshot;
    snapshot.hits = outcome.hits;
    MatchOptions match;
    match.titleContains = {L"几何与只读测试"};
    const WindowQueryResult result =
        BuildWindowQueryResult(snapshot, ListSpec(), match, std::wstring());
    const std::wstring doc = RenderList(result);
    Check(Contains(doc, L"\"coordinateSpace\": \"x11_root_pixels\"") &&
              Contains(doc, L"\"pixelCoordinates\": true"),
          "文档声明坐标是 X11 根窗口像素坐标");
    Check(Contains(doc, L"\"logical\"") && Contains(doc, L"\"known\": false") &&
              Contains(doc, L"x11_has_no_logical_coordinate_space"),
          "X11 不伪造逻辑坐标；这一问明确写 known=false");
    Check(Contains(doc, L"x11_frame_and_client_are_distinct"),
          "文档区分装饰外框与客户区");
    Destroy(display, &w);
}

void SetHiddenIconic(Display* display, Window window) {
    const Atom state = XInternAtom(display, "_NET_WM_STATE", False);
    const Atom hidden = XInternAtom(display, "_NET_WM_STATE_HIDDEN", False);
    const Atom wmStateAtom = XInternAtom(display, "WM_STATE", False);
    long stateValue = static_cast<long>(hidden);
    XChangeProperty(display, window, state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&stateValue), 1);
    long iconic = IconicState;
    XChangeProperty(display, window, wmStateAtom, wmStateAtom, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&iconic), 1);
    XUnmapWindow(display, window);
    XSync(display, False);
}

void TestMinimizedVisibility(Display* display) {
    Section("可见性与最小化是两个维度");
    TestWindow hidden = MakeTopLevel(display, "最小化测试 隐藏窗口", "EcX11Iconic",
                                     static_cast<uint32_t>(getpid()), 620, 260, 280, 180);
    WaitForWindows(display);
    SetHiddenIconic(display, hidden.window);

    x11::ClientWindowFacts facts;
    Check(x11::QueryClientWindow(display, hidden.window, &facts), "最小化窗口仍可查询属性");
    Check(!facts.viewable && facts.iconic,
          "最小化窗口不可见，但 iconic 为真（两个事实同时保留）");

    const MatchOutcome outcome = MatchByTitle("最小化测试 隐藏窗口");
    Check(HasIconic(outcome, hidden.window), "最小化窗口进入 iconic 列表");
    Check(!HasHit(outcome, hidden.window), "最小化窗口不冒充普通可见命中");

    WindowQuerySnapshot snapshot;
    snapshot.hits = outcome.hits;
    snapshot.iconic = outcome.iconic;
    MatchOptions match;
    match.titleContains = {L"最小化测试 隐藏窗口"};
    const WindowQueryResult excluded =
        BuildWindowQueryResult(snapshot, ListSpec(false), match, std::wstring());
    Check(excluded.windows.empty() && excluded.iconicExcluded >= 1,
          "默认 --list 排除最小化窗口但报告被挡下的条数");
    const WindowQueryResult included =
        BuildWindowQueryResult(snapshot, ListSpec(true), match, std::wstring());
    Check(included.windows.size() >= 1, "--list=all 交回最小化窗口");
    bool sawHidden = false;
    for (const auto& record : included.windows) {
        if (record.hwndHex == [&] {
                wchar_t buf[24];
                std::swprintf(buf, 24, L"0x%08X", static_cast<unsigned>(hidden.window));
                return std::wstring(buf);
            }()) {
            sawHidden = true;
            Check(!record.visible && record.iconic,
                  "最小化条目的 visible=false / minimized=true");
        }
    }
    Check(sawHidden, "最小化窗口确实出现在 --list=all 结果里");
    Destroy(display, &hidden);
}

WindowQueryLayer MakeQueryLayer(const MatchRequest& request) {
    WindowQueryLayer q = SystemWindowQueryLayer();
    q.selectionStillMatches = [request](uint64_t hwnd, bool* matched) {
        const MatchOutcome fresh = EnumerateMatches(request);
        if (fresh.status != BlockedStatus::kOk) return false;
        bool found = false;
        for (const auto& hit : fresh.hits) {
            if (hit.hwnd == hwnd) found = true;
        }
        *matched = found;
        return true;
    };
    return q;
}

void TestIdentityRecheck(Display* display) {
    Section("身份复核：销毁 / XID 复用 / 属性变化 / 条件失效 / 会话变化");
    const std::string title = "身份复核测试 原始标题";
    TestWindow w = MakeTopLevel(display, title, "EcX11IdentityOriginal",
                                static_cast<uint32_t>(getpid()), 180, 420, 300, 200);
    WaitForWindows(display);

    MatchOptions match;
    match.titleContains = {L"身份复核测试"};
    MatchRequest request;
    request.match = match;
    const MatchOutcome outcome = EnumerateMatches(request);
    const WindowInfo* selected = FindHit(outcome, w.window);
    Check(selected != nullptr, "身份测试窗口先被条件枚举出来");
    if (!selected) {
        Destroy(display, &w);
        return;
    }
    WindowIdentity identity = MakeWindowIdentity(*selected, match, false);
    Check(identity.native.sessionKnown && identity.native.scopeRequired &&
              identity.native.idForm == L"xid",
          "身份快照把 XID 与显示会话作用域一起记下");
    Check(identity.processAttribution == ProcessAttribution::kDeclaredUnverified,
          "X11 身份快照不把声明 PID 当成系统核实归属");

    WindowQueryLayer q = MakeQueryLayer(request);
    IdentityFault fault;
    Check(CheckWindowIdentity(identity, q, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kSame,
          "未变化时 kCheap 判同一个目标");
    Check(CheckWindowIdentity(identity, q, IdentityScope::kFull, &fault) ==
              IdentityVerdict::kSame,
          "未变化时 kFull 重跑条件仍成立");

    // 同一个 XID 值、不同类名：模拟句柄被复用 / 客户端重建。
    SetClass(display, w.window, "EcX11IdentityReused");
    XSync(display, False);
    Check(CheckWindowIdentity(identity, q, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kChanged,
          "同一 XID 的类名变化判 target_changed");
    Check(fault.detail.find("class") != std::string::npos,
          "变化细节指出类名这一项");
    SetClass(display, w.window, "EcX11IdentityOriginal");
    XSync(display, False);

    // 同一个 XID、声明 PID 改成另一个值：判 changed；但这条证据本身仍只是声明。
    SetPid(display, w.window, static_cast<uint32_t>(getpid()) + 1);
    XSync(display, False);
    Check(CheckWindowIdentity(identity, q, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kChanged,
          "声明 PID 变化判 target_changed（XID 复用的一种可见形状）");
    SetPid(display, w.window, static_cast<uint32_t>(getpid()));
    XSync(display, False);

    // 标题变了而原条件不再成立：kFull 拒；kCheap 只看便宜事实，不重跑条件。
    SetUtf8Title(display, w.window, "已经完全改掉的标题");
    XSync(display, False);
    Check(CheckWindowIdentity(identity, q, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kSame,
          "kCheap 不重跑条件，因此不做标题判断（成本分界）");
    Check(CheckWindowIdentity(identity, q, IdentityScope::kFull, &fault) ==
              IdentityVerdict::kChanged,
          "标题条件不再成立时 kFull 判 target_changed");
    SetUtf8Title(display, w.window, title);
    XSync(display, False);

    // 会话作用域不同：XID 不可跨作用域比较，必须是 unverifiable 而不是 changed / same。
    WindowIdentity otherSession = identity;
    otherSession.native.sessionScope += L"|different";
    Check(CheckWindowIdentity(otherSession, q, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kUnverifiable,
          "显示会话作用域不同：XID 不可比，按无法验证处理");
    WindowIdentity noBaseline = identity;
    noBaseline.native.sessionKnown = false;
    Check(CheckWindowIdentity(noBaseline, q, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kUnverifiable,
          "X11 基线没有会话作用域：fail-closed，不拿裸 XID 当身份");

    // 纯判据注入：同一个 XID 被另一进程取得（本进程无法安全强制造出这种系统竞态）。
    WindowQueryLayer reused;
    reused.alive = [](uint64_t) { return true; };
    reused.windowPid = [](uint64_t, uint32_t* pid) {
        *pid = 999999;
        return true;
    };
    reused.windowClass = [](uint64_t, std::wstring* cls) {
        *cls = L"EcX11IdentityOriginal";
        return true;
    };
    reused.processStart = [ticks = identity.processStartTicks](uint32_t, uint64_t* out) {
        *out = ticks;
        return true;
    };
    reused.nativeIdentity = [&identity](uint64_t, WindowNativeIdentity* out) {
        *out = identity.native;
        return true;
    };
    Check(CheckWindowIdentity(identity, reused, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kChanged,
          "同一 XID 现在归另一 PID：判 target_changed（XID 复用）");

    const uint64_t deadXid = identity.hwnd;
    Destroy(display, &w);
    Check(CheckWindowIdentity(identity, q, IdentityScope::kCheap, &fault) ==
              IdentityVerdict::kGone,
          "目标销毁后判 target_gone");
    const Diagnostic diagnostic =
        IdentityDiagnostic(IdentityVerdict::kGone, fault.detail, deadXid);
    Check(diagnostic.code == codes::kTargetGone,
          "销毁诊断使用稳定码 capture.target_gone");
}

void TestSessionScope(Display* display) {
    Section("显示连接 / 会话事实");
    x11::DisplaySessionFacts first;
    x11::DisplaySessionFacts second;
    Check(x11::QueryDisplaySessionFacts(display, &first), "当前显示连接作用域可读");
    Check(x11::QueryDisplaySessionFacts(display, &second), "同一连接再次询问作用域可读");
    Check(first.known && first.read == ReadState::kReadable && first.rootWindow != 0,
          "会话事实包含 root XID 与 readable 状态");
    Check(first.scope == second.scope && first.scope.find(L"|root=0x") != std::wstring::npos,
          "同一显示作用域得到相同的比较串（XID 不是裸 32 位身份）");
    Check(first.scope.find(L"|xwayland=") != std::wstring::npos,
          "作用域记录 XWAYLAND 扩展是否存在，保留 XWayland 能见范围限制");
}

}  // namespace

int main() {
    SetLanguage(Language::kEn);
    Display* display = OpenTestDisplay();
    if (!display) {
        std::printf("x11-window-query: 无法连接 X11（DISPLAY 未设置或不可达）。\n");
        std::printf("本套需要隔离 X11 或可见的 X11 会话；没有运行不等于通过。\n");
        std::printf("x11-window-query: 0 条通过，1 条失败（前置不成立）\n");
        return 1;
    }
    std::printf("x11-window-query: 使用 DISPLAY=%s\n", DisplayString(display));
    TestUniqueAmbiguousAndChineseTitle(display);
    TestDecorationAndSubwindowSelection(display);
    TestMissingPropertiesAndProcessAttribution(display);
    TestGeometryAndReadOnly(display);
    TestMinimizedVisibility(display);
    TestIdentityRecheck(display);
    TestSessionScope(display);
    XCloseDisplay(display);
    std::printf("\nx11-window-query: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
