// 结构化窗口发现与检查（--list / --inspect）的离线判据（由 tests\windows.ps1 运行
// build\ecapture-windows-tests.exe）。
//
// 为什么单独一个可执行文件：这一批判据要的现场在真机上要么安排不出来、要么没有阴性对照——
//   * 归属进程开不到句柄（要造就得动别人的进程，仓库规矩是不碰使用者真实应用）
//   * 命中 100 扇而只交回 3 扇（本机没有 100 扇可控制的窗口）
//   * 窗口矩形那一问失败了（GetWindowRect 对活窗口问不出来，本机造不出）
//   * 一个都没命中 / 同一个条件命中三扇而没人消歧（要凑就得改用户的桌面）
// 判据本体（src/WindowQuery.cpp）全是纯函数：注入一份快照与几条假候选就能逐条判，
// 连「渲染出来的文档有没有把完整路径漏进默认那一份」都判得到，不必真去截图、也不必真去枚举窗口。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。文案按英文跑（与其他 state 测试同一约定），
// 断言判的是机器可读的字段与取值，不判人话文字。
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/Lang.h"
#include "../src/Version.h"
#include "../src/WindowIdentity.h"
#include "../src/WindowMatch.h"
#include "../src/WindowMatchInternal.h"
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
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// 判据里有时要比的不是常量而是算出来的值（标题回显那条就是要比用户写的那一串原文）。
bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// 一条「什么都问得出来」的候选：各条问句的下场都是 readable，路径与创建时间都有真值。
// 每条用例只改自己那一项，其余照这份，失败时才知道是谁动的。
WindowInfo HealthyWindow(uint64_t hwnd, int z) {
    WindowInfo w;
    w.hwnd = hwnd;
    w.pid = 4000 + static_cast<uint32_t>(z);
    w.processStartTicks = 130000000000000000ull + static_cast<uint64_t>(z);
    w.title = L"Title";
    w.className = L"TestClass";
    w.imageName = L"app.exe";
    w.imagePath = L"C:\\Users\\someone\\App\\app.exe";
    w.pathRead = ReadState::kReadable;
    w.startRead = ReadState::kReadable;
    w.rectRead = ReadState::kReadable;
    w.x = 100 + z;
    w.y = 200;
    w.width = 800;
    w.height = 600;
    w.zOrder = z;
    return w;
}

WindowQuerySnapshot Snapshot(std::vector<WindowInfo> hits, std::vector<WindowInfo> iconic = {}) {
    WindowQuerySnapshot s;
    s.hits = std::move(hits);
    s.iconic = std::move(iconic);
    return s;
}

WindowQuerySpec ListSpec(uint64_t offset = 0, uint64_t limit = 0, bool iconic = false,
                         bool exePath = false) {
    WindowQuerySpec s;
    s.action = WindowAction::kList;
    s.offset = offset;
    s.limit = limit;
    s.includeIconic = iconic;
    s.exePath = exePath;
    return s;
}

WindowQuerySpec InspectSpec(MultiMatch policy = MultiMatch::kAsk, int index = 1) {
    WindowQuerySpec s;
    s.action = WindowAction::kInspect;
    s.inspectPolicy = policy;
    s.inspectIndex = index;
    return s;
}

// 一批候选：句柄按 0x100+i 排，Z 序就是数组顺序（判分页与首尾策略时好对号）。
std::vector<WindowInfo> ManyWindows(size_t n) {
    std::vector<WindowInfo> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i) v.push_back(HealthyWindow(0x100 + i, static_cast<int>(i)));
    return v;
}

bool HasCode(const std::vector<Diagnostic>& items, const wchar_t* code) {
    for (const auto& d : items) {
        if (d.code == code) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 组合筛选判据本体的脚手架：条件按 MatchOptions 的形状给（用户写的那些取值），
// 候选是一条假窗口。判的是生产枚举用的那一对函数本体（CompileConditions /
// MatchesWindow，src/WindowMatch.cpp），不是测试另抄的一份匹配算法。
// ---------------------------------------------------------------------------
struct Predicate {
    CompiledConditions c;
    bool compiled = false;
    BlockedStatus status = BlockedStatus::kOk;
    std::string detail;
};

Predicate Compile(const MatchOptions& m) {
    MatchRequest req;
    req.match = m;
    Predicate p;
    p.compiled = CompileConditions(req, &p.c, &p.status, &p.detail);
    return p;
}

// 这一条候选会不会被当成命中。条件编译不出来（本机标准库拒绝那条正则）时按"没命中"算，
// 那条路另有判据（见 TestCombinedConditions 末尾：编译失败要交回机器码，而不是当成不匹配）。
bool Hit(const Predicate& p, const WindowInfo& w) {
    if (!p.compiled) return false;
    return MatchesWindow(p.c, w, nullptr);
}

WindowInfo WithTitle(uint64_t hwnd, const wchar_t* title) {
    WindowInfo w = HealthyWindow(hwnd, 0);
    w.title = title;
    return w;
}

std::wstring Render(const WindowQueryResult& r, WindowAction action, bool verbose = false,
                    bool quiet = false) {
    const std::wstring contract =
        action == WindowAction::kInspect ? L"windowinspect" : kWindowQueryContractName;
    return RenderWindowQuery(r, contract, verbose, quiet);
}

// ---------------------------------------------------------------------------
// 组合筛选：跨类 AND、同类 OR —— 每一类必须自己成立
//
// 回归的起点是那份被多类共用的 bool any：hwnd / pid / process / exe / title /
// title-contains 里任何一类命中后，那个真值会一路留到 --title-regex 那一段，
// 于是"正则全部不匹配"仍被前面那类的命中放行（`if (!any) return false` 判的是别人的结果）。
// 反例的形状是标题 PRIVATE、正则 ^PUBLIC$、再叠一条命中的 --process：
// 单独的正则该拒绝，组合起来却放行。下面每一组"别的类成立 + 正则不成立"都是那一次。
// ---------------------------------------------------------------------------
void TestCombinedConditions() {
    Section("组合筛选：每一类各判各的（跨类 AND、同类 OR）");

    // 三条假候选：进程/类名用 HealthyWindow 那份，标题各管标题那一类。
    const WindowInfo priv = WithTitle(0x777, L"PRIVATE");
    const WindowInfo pub = WithTitle(0x778, L"PUBLIC");
    const WindowInfo blank = WithTitle(0x779, L"");

    MatchOptions onlyRegex;
    onlyRegex.titleRegexes.push_back(L"^PUBLIC$");
    const Predicate regexOnly = Compile(onlyRegex);
    Check(Hit(regexOnly, pub), "只有正则：标题 PUBLIC 命中");
    Check(!Hit(regexOnly, priv), "只有正则：标题 PRIVATE 拒绝（这一条本来就对，钉住它）");

    // 每一类各给一个"这一条候选确实满足"的取值，再叠一条对它不成立的正则。
    // --process 写 APP（不带 .exe）与 --class 写 TestCLASS：顺带钉住大小写折叠与补 .exe 那两条写法。
    MatchOptions byHwnd;
    byHwnd.hwnds.push_back(priv.hwnd);
    MatchOptions byPid;
    byPid.pids.push_back(priv.pid);
    MatchOptions byProcess;
    byProcess.processes.push_back(L"APP");
    MatchOptions byExe;
    byExe.exePaths.push_back(L"C:\\Users\\someone\\App\\app.exe");
    MatchOptions byTitle;
    byTitle.titles.push_back(L"PRIVATE");
    MatchOptions byContains;
    byContains.titleContains.push_back(L"IVATE");
    MatchOptions byClass;
    byClass.classes.push_back(L"TestCLASS");

    const std::vector<std::pair<MatchOptions, const char*>> categories = {
        {byHwnd, "--hwnd"}, {byPid, "--pid"}, {byProcess, "--process"}, {byExe, "--exe"},
        {byTitle, "--title"}, {byContains, "--title-contains"}, {byClass, "--class"},
    };
    for (const auto& one : categories) {
        MatchOptions rejecting = one.first;
        rejecting.titleRegexes.push_back(L"^PUBLIC$");
        Check(!Hit(Compile(rejecting), priv),
              (std::string("命中的 ") + one.second + " + 不成立的正则：拒绝").c_str());

        MatchOptions accepting = one.first;
        accepting.titleRegexes.push_back(L"^PRIVATE$");
        Check(Hit(Compile(accepting), priv),
              (std::string("命中的 ") + one.second + " + 成立的正则：命中（AND 没有整体作废）").c_str());
    }

    // 多条正则：同类是 OR，所以"其中一条成立"就该命中，"全部不成立"必须拒绝。
    MatchOptions allRegexMiss = byProcess;
    allRegexMiss.titleRegexes.push_back(L"^PUBLIC$");
    allRegexMiss.titleRegexes.push_back(L"\\d+");
    Check(!Hit(Compile(allRegexMiss), priv), "命中的 --process + 两条正则全不成立：拒绝");
    MatchOptions oneRegexHits = byProcess;
    oneRegexHits.titleRegexes.push_back(L"^PUBLIC$");
    oneRegexHits.titleRegexes.push_back(L"IVATE");
    Check(Hit(Compile(oneRegexHits), priv), "命中的 --process + 两条正则里有一条成立：命中");

    // 反过来那一半也得判：正则成立而别的类不成立，同样不能放行。
    MatchOptions wrongProcess = onlyRegex;
    wrongProcess.processes.push_back(L"notepad.exe");
    Check(!Hit(Compile(wrongProcess), pub), "正则命中 + 进程不命中：拒绝");
    MatchOptions wrongTitle = onlyRegex;
    wrongTitle.titles.push_back(L"OTHER");
    Check(!Hit(Compile(wrongTitle), pub), "正则命中 + 精确标题不命中：拒绝");
    MatchOptions wrongClass = onlyRegex;
    wrongClass.classes.push_back(L"OtherClass");
    Check(!Hit(Compile(wrongClass), pub), "正则命中 + 类名不命中：拒绝");
    MatchOptions wrongHwnd = onlyRegex;
    wrongHwnd.hwnds.push_back(0x8888);
    Check(!Hit(Compile(wrongHwnd), pub), "正则命中 + 句柄不命中：拒绝");

    // 同类多个取值的 OR 没有因为这次改动而变严：进程写两个、其中一个成立，仍是命中。
    MatchOptions processOr = byProcess;
    processOr.processes.push_back(L"notepad.exe");
    processOr.titleRegexes.push_back(L"^PUBLIC$");
    Check(!Hit(Compile(processOr), priv),
          "同类 OR（进程两个取值命中一个）也不许替正则说话：拒绝");
    MatchOptions processOrOk = byProcess;
    processOrOk.processes.push_back(L"notepad.exe");
    processOrOk.titleRegexes.push_back(L"^PRIVATE$");
    Check(Hit(Compile(processOrOk), priv), "同类 OR：进程两个取值里命中一个 + 正则成立 -> 命中");

    // 选项书写顺序不影响结论：同一条 AND 判据，先写正则与先写进程必须给同一个答案。
    MatchOptions regexFirst;
    regexFirst.titleRegexes.push_back(L"^PUBLIC$");
    regexFirst.processes.push_back(L"APP");
    MatchOptions processFirst;
    processFirst.processes.push_back(L"APP");
    processFirst.titleRegexes.push_back(L"^PUBLIC$");
    Check(Hit(Compile(regexFirst), priv) == Hit(Compile(processFirst), priv) &&
              !Hit(Compile(regexFirst), priv),
          "正则写在进程前面与写在进程后面结果一致（都是拒绝）");
    MatchOptions regexFirstOk;
    regexFirstOk.titleRegexes.push_back(L"^PRIVATE$");
    regexFirstOk.pids.push_back(priv.pid);
    MatchOptions pidFirstOk;
    pidFirstOk.pids.push_back(priv.pid);
    pidFirstOk.titleRegexes.push_back(L"^PRIVATE$");
    Check(Hit(Compile(regexFirstOk), priv) && Hit(Compile(pidFirstOk), priv),
          "同样两种书写顺序：都成立时都是命中");

    // 空标题是真值，不是"没答案"：^$ 要能命中，^x 叠一条命中的进程仍要拒绝。
    MatchOptions emptyRegex;
    emptyRegex.titleRegexes.push_back(L"^$");
    emptyRegex.processes.push_back(L"APP");
    Check(Hit(Compile(emptyRegex), blank), "空标题 + ^$ + 命中的进程：命中");
    MatchOptions emptyReject;
    emptyReject.titleRegexes.push_back(L"^x");
    emptyReject.processes.push_back(L"APP");
    Check(!Hit(Compile(emptyReject), blank), "空标题 + ^x + 命中的进程：拒绝");

    // 大小写约定：标题那一类按用户写的原样比（精确与子串都不折叠），正则照 ECMAScript 默认。
    MatchOptions titleCase;
    titleCase.titles.push_back(L"private");
    Check(!Hit(Compile(titleCase), priv), "--title 区分大小写：小写的 private 不命中 PRIVATE");
    MatchOptions regexCase;
    regexCase.titleRegexes.push_back(L"^private$");
    regexCase.processes.push_back(L"app");
    Check(!Hit(Compile(regexCase), priv), "正则默认区分大小写：^private$ 不命中 PRIVATE");

    // 一条都没给时不作限制（这一条与本次修复无关，但它是 AND 的零元素那一端）。
    Check(Hit(Compile(MatchOptions()), priv), "一个条件都没给：所有候选都算命中");

    // 本机标准库拒绝编译的那条正则：交回机器码与 ASCII 细节，不是"判它不成立"。
    MatchOptions badRegex;
    badRegex.titleRegexes.push_back(L"[bad(");
    const Predicate bad = Compile(badRegex);
    Check(!bad.compiled, "编不出来的正则：编译这一步失败（而不是当成不匹配）");
    Check(bad.status == BlockedStatus::kRegexInvalid, "失败的原因是 kRegexInvalid 这个机器码");
    Check(!bad.detail.empty(), "失败细节是 ASCII 的 what()，交回父进程拼文案");

    // 与截图那一路的衔接：用正则认出的目标，复核要重跑条件（而不是比标题快照）。
    // 判的是这一件事没有被这次改动漏掉——修复让正则真的能筛掉窗口，而"当初凭什么挑中它"
    // 仍然要靠同一份条件在取像素之前再问一次。
    MatchOptions recheckOpt;
    recheckOpt.titleRegexes.push_back(L"^PUBLIC$");
    Check(MakeWindowIdentity(pub, recheckOpt, false).selectionNeedsRecheck,
          "含 --title-regex 的条件：身份复核要重跑条件（kFull 那一问）");
    MatchOptions recheckCheap;
    recheckCheap.pids.push_back(pub.pid);
    Check(!MakeWindowIdentity(pub, recheckCheap, false).selectionNeedsRecheck,
          "只靠 PID/句柄/类名认出的目标：不重跑条件（kCheap 那四问已覆盖）");
}

// ---------------------------------------------------------------------------
// 正则编译的职责划分（R02）：解析层不碰正则库，编译只发生在受约束的匹配执行层
//
// 旧实现在解析期（父进程、任何 Deadline 建立之前）就完整构造一遍 std::wregex 来
// "验语法"：那一步既没有期限也没有隔离，--timeout-ms 与内置隔离上限都管不到它。
// 现在解析层只记原文与判重，这里判的是这份拆分的两端：
//   * 语法不合的模式**不是**解析错误（父进程没编译的最好证据就是它照常通过解析），
//     而取值原文、判重 note 与同类多条的规范化职责一切照旧；
//   * CompileConditions（生产函数本体，也就是隔离调用里跑的那一份）把语法不合交回
//     kRegexInvalid 与带机器码的 ASCII 细节——"编不出来"是整个求值作废，而不是
//     "当成不匹配"。
// 危险模式"编得慢/跑得凶"的那条现场不拿墙钟数字当判据（本仓库不伪造性能复现）：
// 它由 tests\windows.ps1 与 tests\timeout.ps1 在带外层时限的受控子进程里判。
// ---------------------------------------------------------------------------
ParseResult ParseArgs(std::initializer_list<std::wstring> args) {
    // ParseCommandLine 要的是"活着的 wchar_t* 数组"：先把取值存下来再取指针。
    std::vector<std::wstring> store(args);
    std::vector<wchar_t*> argv;
    argv.reserve(store.size());
    for (auto& s : store) argv.push_back(s.data());
    return ParseCommandLine(static_cast<int>(argv.size()), argv.data());
}

void TestParseDefersRegexCompile() {
    Section("正则编译职责：解析层记原文，匹配层判语法");

    const ParseResult bad = ParseArgs({L"ECAPTURE", L"--title-regex", L"[bad(", L"out.png"});
    Check(bad.ok && bad.errors.empty(),
          "语法不合的模式不再是解析错误（父进程不构造正则）");
    Check(bad.options.match.titleRegexes.size() == 1 &&
              bad.options.match.titleRegexes[0] == L"[bad(",
          "取值原样落进 MatchOptions（解析层只管 token 与规范化）");

    // 同一条模式在匹配执行层被拒绝：编译点只有这一处，而它的下场是整个求值作废。
    const Predicate rejected = Compile(bad.options.match);
    Check(!rejected.compiled && rejected.status == BlockedStatus::kRegexInvalid,
          "匹配层编不出来：kRegexInvalid，而不是\"当成不匹配\"");
    Check(rejected.detail.rfind("regex_error code=", 0) == 0,
          "机器细节里带着 regex_error 的 code（旧解析期文案给出的那份信息没有丢）");

    // 规范化职责一样没动：判重仍是一条取值 + 一条 note，同类多条仍全部记下（OR）。
    const ParseResult dup = ParseArgs({L"ECAPTURE", L"--title-regex", L"a", L"--title-regex", L"a",
                                       L"out.png"});
    Check(dup.ok && dup.options.match.titleRegexes.size() == 1,
          "同一条正则写两次：只留一条");
    Check(HasCode(dup.warnings, codes::kDuplicateValue),
          "重复取值仍记 note.duplicate_value");
    const ParseResult pair = ParseArgs({L"ECAPTURE", L"--title-regex", L"a", L"--title-regex", L"b",
                                        L"out.png"});
    Check(pair.ok && pair.options.match.titleRegexes.size() == 2,
          "同类两条正则：都记下（同类 OR 语义不变）");

    // --help / --version 的早返回不因模式语法而变：解析层本来就不编译，执行层还没开工。
    const ParseResult help = ParseArgs({L"ECAPTURE", L"--help", L"--title-regex", L"[bad("});
    Check(help.options.showHelp && help.errors.empty(), "--help 早返回：语法不合也不是参数错");
    const ParseResult ver = ParseArgs({L"ECAPTURE", L"--version", L"--title-regex", L"[bad("});
    Check(ver.options.showVersion && ver.errors.empty(), "--version 早返回：同样不判语法");

    // 编译成立而**求值**被本机正则库上限挡下的那一类：原因码与"编译被拒"分开，
    // 半套命中列表不许交回（整次作废由 MatchesWindow 置 fault、EnumerateMatches 清空命中，
    // 真机那一条见 tests\timeout.ps1 第 9(b) 节——这里判的是码的形状，不是墙钟数字）。
    MatchOptions nested;
    nested.titleRegexes.push_back(L"(a+)+$");
    const Predicate compiled = Compile(nested);
    Check(compiled.compiled, "(a+)+$ 编得出来（失控发生在求值，不在编译）");
    RegexFault fault;
    const WindowInfo bomb = WithTitle(0x9AB, L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!");
    Check(!MatchesWindow(compiled.c, bomb, &fault), "失控模式：这一条候选不算命中");
    Check(fault.hit && fault.status == BlockedStatus::kRegexTooComplex,
          "被正则库的上限挡下：原因码是 kRegexTooComplex（与 kRegexInvalid 分开）");

    // ParseCommandLine 会把语言切回系统显示语言；这份套件按英文跑，恢复它。
    SetLanguage(Language::kEn);
}

// ---------------------------------------------------------------------------
// 机器词：三条问句的下场与契约名，不随 --lang 变
// ---------------------------------------------------------------------------
void TestMachineWords() {
    Section("机器词（readable / denied / failed 与契约名）");
    Check(std::wstring(ReadStateName(ReadState::kReadable)) == L"readable",
          "readable 的机器名固定");
    Check(std::wstring(ReadStateName(ReadState::kDenied)) == L"denied", "denied 的机器名固定");
    Check(std::wstring(ReadStateName(ReadState::kFailed)) == L"failed", "failed 的机器名固定");
    Check(std::wstring(kWindowQueryContractName) == L"windowquery", "列表文档的契约名固定");
    Check(kWindowQueryContractVersion == 1, "窗口查询文档的契约版本是 1");
    // 契约名与环境查询那两份不同：四份文档各自演进，不共用一个名字。
    Check(std::wstring(kWindowQueryContractName) != L"capabilities" &&
              std::wstring(kWindowQueryContractName) != L"diagnostics",
          "窗口查询文档不冒充环境查询那两份");
}

// ---------------------------------------------------------------------------
// --list：空列表与多匹配都不是错误
// ---------------------------------------------------------------------------
void TestListIsNeverAmbiguous() {
    Section("--list：多匹配是正常答复，无匹配是空列表");
    MatchOptions none;   // 一个条件都没给（--list 本身就是明确意图，不参与「零条件=帮助」）

    const WindowQueryResult empty =
        BuildWindowQueryResult(Snapshot({}), ListSpec(), none, std::wstring());
    Check(empty.errors.empty() && empty.exitCode == 0, "一个都没命中：没有 errors，退出码 0");
    Check(empty.matched == 0 && empty.windows.empty(), "空列表的形状是 []，不是缺字段");
    Check(empty.returned == 0 && !empty.truncated, "空列表不算被截断");

    const WindowQueryResult many =
        BuildWindowQueryResult(Snapshot(ManyWindows(7)), ListSpec(), none, std::wstring());
    Check(many.errors.empty(), "命中 7 扇而没人消歧：--list 一条错误都不报");
    Check(!HasCode(many.errors, codes::kAmbiguousWindow), "--list 绝不报 match.ambiguous_window");
    Check(many.matched == 7 && many.returned == 7, "7 扇全部交回（未达默认条数）");
    Check(!many.truncated && many.limitDefaulted,
          "没截断也可以是用了默认条数：limitDefaulted 说的是这批按哪份预算分页，不是有没有分页剩余");
}

// ---------------------------------------------------------------------------
// --inspect：唯一目标，判据与截图同一条选择策略
// ---------------------------------------------------------------------------
void TestInspectNeedsUniqueTarget() {
    Section("--inspect：无匹配 / 歧义 / 越界都是稳定诊断，不替人选一个");
    MatchOptions none;

    const WindowQueryResult gone =
        BuildWindowQueryResult(Snapshot({}), InspectSpec(), none, std::wstring());
    Check(HasCode(gone.errors, codes::kNoWindow), "命中 0 扇：match.no_window（不是空快照）");
    Check(gone.exitCode == EX_NO_MATCH, "命中 0 扇：退出码 4，与截图那一次同数");
    Check(gone.windows.empty() && !gone.hasTarget, "报无匹配时不交任何窗口");
    Check(!gone.errors.empty() && gone.errors.front().stage == stages::kMatch,
          "stage 补成 match（那三条诊断由选择策略给出，它只填 code/option/value/hint）");

    const WindowQueryResult amb =
        BuildWindowQueryResult(Snapshot(ManyWindows(3)), InspectSpec(), none, std::wstring());
    Check(HasCode(amb.errors, codes::kAmbiguousWindow),
          "命中 3 扇且没消歧：match.ambiguous_window");
    Check(amb.exitCode == EX_AMBIGUOUS, "歧义：退出码 5");
    Check(!amb.hasTarget && amb.windows.empty(), "歧义时绝不随便选一个交回去");
    Check(!amb.errors.empty() && amb.errors.front().value == L"3", "歧义的 value 是实际命中数");
    Check(amb.matched == 3 && amb.returned == 0,
          "命中总数仍写 3：调用方据此知道要消歧的是三个候选规模");

    // 与截图同源这一件事要能被程序核对：同一次快照、同一条策略，inspect 选中的必须与
    // SelectFromHits（截图用的那一份）逐条一致。
    Options likeCapture;
    likeCapture.multi = MultiMatch::kIndex;
    likeCapture.index = 2;
    std::vector<Diagnostic> selErr;
    const std::vector<WindowInfo> picked =
        SelectFromHits(likeCapture, ManyWindows(4), {}, std::wstring(), &selErr);
    const WindowQueryResult byIndex = BuildWindowQueryResult(
        Snapshot(ManyWindows(4)), InspectSpec(MultiMatch::kIndex, 2), none, std::wstring());
    wchar_t want[24];
    swprintf(want, 24, L"0x%08X", static_cast<unsigned>(picked.front().hwnd));
    Check(picked.size() == 1 && byIndex.windows.size() == 1 &&
              byIndex.windows.front().hwndHex == want,
          "--index 2 选中的句柄与截图那一条完全相同（同一套消歧，不另立第二份规则）");
    Check(byIndex.targetHwnd == picked.front().hwnd, "结果里的数字句柄与那个 0x 形状是同一个值");
    Check(byIndex.matched == 4 && byIndex.returned == 1 && byIndex.limit == 1,
          "inspect 交回 1 扇而命中总数仍是 4（不把分页概念混进去）");

    const WindowQueryResult oob = BuildWindowQueryResult(
        Snapshot(ManyWindows(2)), InspectSpec(MultiMatch::kIndex, 5), none, std::wstring());
    Check(HasCode(oob.errors, codes::kIndexOutOfRange), "--index 越界：match.index_out_of_range");
    Check(oob.exitCode == EX_USAGE, "--index 越界：退出码 1（写错了编号，与截图一致）");

    const std::vector<WindowInfo> four = ManyWindows(4);
    const WindowQueryResult top =
        BuildWindowQueryResult(Snapshot(four), InspectSpec(MultiMatch::kTopmost), none,
                               std::wstring());
    const WindowQueryResult bottom =
        BuildWindowQueryResult(Snapshot(four), InspectSpec(MultiMatch::kBottommost), none,
                               std::wstring());
    Check(top.hasTarget && top.windows.front().zOrder == 0, "--topmost-match 取 Z 序第一");
    Check(bottom.hasTarget && bottom.windows.front().zOrder == 3, "--bottommost-match 取 Z 序最后");

    // --all 在解析期就与 --inspect 互斥；判据这层仍不猜：多于一条就是歧义，不顺手交回整份列表。
    const WindowQueryResult withAll =
        BuildWindowQueryResult(Snapshot(four), InspectSpec(MultiMatch::kAll), none, std::wstring());
    Check(HasCode(withAll.errors, codes::kAmbiguousWindow) && !withAll.hasTarget,
          "即使策略是 kAll，inspect 也不会顺手交回整份列表");
    Check(withAll.exitCode == EX_AMBIGUOUS, "那条路的退出码也是 5");
}

// ---------------------------------------------------------------------------
// 分页：offset / limit / 默认条数 / 边界
// ---------------------------------------------------------------------------
void TestPagination() {
    Section("--list 的分页：默认条数、翻页边界与还有没有下一条");
    MatchOptions none;
    const std::vector<WindowInfo> hundred = ManyWindows(100);

    const WindowQueryResult dflt =
        BuildWindowQueryResult(Snapshot(hundred), ListSpec(), none, std::wstring());
    Check(dflt.limit == cli_limits::kDefaultWindowListLimit, "不给 --limit 时用默认条数");
    Check(dflt.limitDefaulted && dflt.returned == 50 && dflt.matched == 100,
          "默认那一批：交回 50 条而命中总数照实写 100");
    Check(dflt.truncated, "命中没交完就是被截断（调用方据此知道要翻页）");

    const WindowQueryResult p2 =
        BuildWindowQueryResult(Snapshot(hundred), ListSpec(50, 10), none, std::wstring());
    Check(p2.matched == 100 && p2.returned == 10 && !p2.limitDefaulted,
          "--offset 50 --limit 10：第二页 10 条");
    Check(p2.windows.front().zOrder == 50 && p2.windows.back().zOrder == 59,
          "第二页正是 Z 序 50..59，不是重头再数");

    const WindowQueryResult tail =
        BuildWindowQueryResult(Snapshot(hundred), ListSpec(98, 10), none, std::wstring());
    Check(tail.returned == 2 && !tail.truncated,
          "尾页本批不满也不算截断：判的是窗口那边还有没有下一条");

    const WindowQueryResult over =
        BuildWindowQueryResult(Snapshot(hundred), ListSpec(100, 5), none, std::wstring());
    Check(over.windows.empty() && !over.truncated && over.matched == 100,
          "--offset 正好等于命中数：空批次而不是回绕到第一条");

    const WindowQueryResult wayOut =
        BuildWindowQueryResult(Snapshot(hundred), ListSpec(5000, 5), none, std::wstring());
    Check(wayOut.windows.empty() && wayOut.errors.empty(),
          "--offset 远超命中数：仍是空列表，不是越界错误（列表没有第几个这回事）");
    // 变量名不叫 far：windows.h 在别处会把 far 定义成宏，与仓库里 near/far 那条坑同源。

    const WindowQueryResult one =
        BuildWindowQueryResult(Snapshot(hundred), ListSpec(0, 1), none, std::wstring());
    Check(one.returned == 1 && one.truncated, "--limit 1 只交一条而后面还有");

    // 报告里那三个数必须能互相核对：交回条数 = windows 长度，且偏移加交回不超过命中总数。
    const std::vector<const WindowQueryResult*> batches = {&dflt, &p2, &tail, &over, &wayOut, &one};
    for (const WindowQueryResult* r : batches) {
        Check(r->returned == r->windows.size() &&
              std::min(r->offset, r->matched) + r->returned <= r->matched,
              "交回条数、偏移与命中总数自相一致");
    }
    // 默认上限与辅助进程回传条数那道线同源（数字只在 cli_limits 写一次）。
    Check(cli_limits::kMaxWindowListItems == 8192ull,
          "窗口查询条数上限与一次求值本来能拿到多少条那道线一致");
}

// ---------------------------------------------------------------------------
// 可见性策略：不可见/零尺寸已被枚举挡掉，最小化靠 --include-iconic
// ---------------------------------------------------------------------------
void TestVisibilityPolicy() {
    Section("可见性策略：默认排除什么、--include-iconic 补进什么");
    MatchOptions none;
    std::vector<WindowInfo> hits = ManyWindows(3);
    // 可见那三扇占 Z 序 0..2，最小化那两扇占 3 与 5：并入之后必须是 0,1,2,3,5，
    // 而不是「可见的排前面再补最小化」（那种形状在 3 与 5 之间看不出差别，所以特意让
    // 最小化那两扇插在与可见同一根 Z 序轴上）。
    WindowInfo m1 = HealthyWindow(0x900, 3);
    m1.iconic = true;
    WindowInfo m2 = HealthyWindow(0x901, 5);
    m2.iconic = true;
    std::vector<WindowInfo> iconic = {m1, m2};

    const WindowQueryResult dflt =
        BuildWindowQueryResult(Snapshot(hits, iconic), ListSpec(), none, std::wstring());
    Check(dflt.matched == 3, "默认策略：最小化那两扇不计入交回的命中数");
    Check(dflt.iconicExcluded == 2,
          "被挡掉的最小化条数照实报告（别让「没有」被读成「不存在」）");
    Check(dflt.windows.size() == 3, "默认策略下最小化窗口不出现在列表里");

    const WindowQueryResult with = BuildWindowQueryResult(
        Snapshot(hits, iconic), ListSpec(0, 0, true), none, std::wstring());
    Check(with.matched == 5 && with.iconicExcluded == 0,
          "--include-iconic：并入后是 5 条，再也没有「被挡掉」这一说");
    Check(with.windows[0].zOrder == 0 && with.windows[1].zOrder == 1 &&
              with.windows[2].zOrder == 2 && with.windows[3].zOrder == 3 &&
              with.windows[4].zOrder == 5,
          "并入按当下 Z 序，不是可见的排前面再补最小化");
    Check(with.windows[3].iconic && with.windows[4].iconic,
          "Z 序 3 那一条就是最小化那扇（顺序与身份都来自同一次排序）");
    Check(!with.windows[3].visible && with.windows[3].iconic,
          "最小化那一条的 visible 是假而 iconic 是真（同一件事在两个字段上不许打脸）");
    for (const auto& w : with.windows) {
        Check(w.visible != w.iconic, "visible 与 minimized 恒为互斥的两个写法，不许同真同假");
    }
    Check(!with.truncated, "并入之后仍按 offset/limit 判截断（这一批没满默认条数，后面也没东西）");

    // 不可见与零尺寸窗口根本不进快照（与截图链路的枚举同一条策略），所以这里能判的是
    // 文档把这件事说清楚了，而不是列表里少了谁。
    const std::wstring doc = Render(dflt, WindowAction::kList);
    Check(Contains(doc, L"\"invisibleExcluded\": true") &&
              Contains(doc, L"\"zeroSizedExcluded\": true") &&
              Contains(doc, L"\"minimizedIncluded\": false") &&
              Contains(doc, L"\"minimizedExcluded\": 2"),
          "policy 段把「默认排除什么」与「这次挡掉几扇」写成机器字段");
    Check(Contains(doc, L"invisible_and_zero_sized_excluded"),
          "caveats 里有一条专门钉住这个策略");
    Check(Contains(doc, L"\"systemWindowAssertion\": false"),
          "系统窗口没有可判的身份，文档不声称能区分它们");
}

// ---------------------------------------------------------------------------
// 字段级可读性：读不到 ≠ 空值，被挡下 ≠ 建议提权
// ---------------------------------------------------------------------------
void TestFieldReadability() {
    Section("字段级可读性：denied / failed 与系统原因码");
    MatchOptions none;

    WindowInfo limited = HealthyWindow(0x1234, 0);
    limited.imagePath.clear();
    limited.imageName.clear();
    limited.pathRead = ReadState::kDenied;
    limited.pathWin32 = 5;   // ERROR_ACCESS_DENIED

    WindowRecord rec = MakeWindowRecord(limited, ListSpec(0, 0, false, true));
    Check(rec.processRead == L"denied", "归属进程这一层综合成 denied（两条问句取更糟的那个）");
    Check(rec.imagePathRead == L"denied" && rec.imagePathWin32 == 5,
          "imagePath 这一问写 denied 并带上系统原因码");
    Check(rec.exePathRequested && !rec.exePathReadable && !rec.exePathIncluded,
          "给了 --exe-path 而这一问没答案：报告里看得见「想写而没写成」");
    Check(rec.imagePath.empty(), "没答案时不塞一个空路径冒充值");
    Check(rec.startRead == L"readable", "另一问（进程创建时间）不受这一问影响：逐条各判各的");

    // 开句柄就没成功：两条问句同时没有答案，原因码同源但不许被折成「可读」。
    WindowInfo unopenable = HealthyWindow(0x1235, 0);
    unopenable.imagePath.clear();
    unopenable.imageName.clear();
    unopenable.processStartTicks = 0;
    unopenable.pathRead = ReadState::kFailed;
    unopenable.pathWin32 = 87;
    unopenable.startRead = ReadState::kFailed;
    unopenable.startWin32 = 87;
    WindowRecord rec2 = MakeWindowRecord(unopenable, ListSpec());
    Check(rec2.processRead == L"failed" && rec2.startRead == L"failed",
          "问过而失败：两条都写 failed，不是 readable");
    Check(rec2.pid == unopenable.pid,
          "PID 是窗口自己那一问的结果，不受归属进程开不到句柄影响");

    // 窗口矩形问不出来：那四个数是初值，报告必须靠 rectRead 说明那不是 0，是没量到。
    WindowInfo noRect = HealthyWindow(0x1236, 0);
    noRect.x = 0; noRect.y = 0; noRect.width = 0; noRect.height = 0;
    noRect.rectRead = ReadState::kFailed;
    noRect.rectWin32 = 1400;
    WindowRecord rec3 = MakeWindowRecord(noRect, ListSpec());
    Check(rec3.rectRead == L"failed" && rec3.rectWin32 == 1400 && rec3.width == 0,
          "矩形问不出来：值留 0 而 rectRead 写 failed 并带原因码");

    // 标题为空是真值（无标题窗口本来就有），与「读不到」必须能区分。
    WindowInfo untitled = HealthyWindow(0x1237, 0);
    untitled.title.clear();
    WindowRecord rec4 = MakeWindowRecord(untitled, ListSpec());
    Check(rec4.title.empty() && rec4.processRead == L"readable",
          "空标题 + 各问都 readable：那是真的没有标题，不是问答失败");

    const WindowQueryResult r = BuildWindowQueryResult(
        Snapshot({limited, unopenable, noRect}), ListSpec(0, 0, false, true), none,
        std::wstring());
    const std::wstring doc = Render(r, WindowAction::kList);
    Check(Contains(doc, L"\"state\": \"denied\"") && Contains(doc, L"\"win32\": 5"),
          "readability 那一段真把 state 与原因码写进文档");
    Check(Contains(doc, L"\"exePathRequested\": true") &&
              Contains(doc, L"\"exePathReadable\": false"),
          "想写完整路径而写不成：文档里两个字段一起说清");
    Check(Contains(doc, L"unreadable_fields_are_not_a_prediction"),
          "caveats 里钉住「读不到不预测截不截得到」，也就不构成提权的理由");
}

// ---------------------------------------------------------------------------
// 隐私：完整映像路径默认不写
// ---------------------------------------------------------------------------
void TestPathPrivacy() {
    Section("隐私：完整路径只在 --exe-path 时出现");
    MatchOptions none;
    const std::vector<WindowInfo> one = {HealthyWindow(0x1238, 0)};

    const WindowQueryResult dflt =
        BuildWindowQueryResult(Snapshot(one), ListSpec(), none, std::wstring());
    const std::wstring d = Render(dflt, WindowAction::kList);
    Check(!Contains(d, L"someone"), "默认那份不含路径里的用户名目录那一段");
    Check(!Contains(d, L"exePath"), "默认那份连 exePath 这个键都不出现（不是写一个空值）");
    Check(!Contains(d, L"C:\\"), "默认那份不含任何带盘符的绝对路径");
    Check(Contains(d, L"\"image\": \"app.exe\""),
          "映像文件名照旧交付：那是匹配与识别要用的最小信息");

    const WindowQueryResult full =
        BuildWindowQueryResult(Snapshot(one), ListSpec(0, 0, false, true), none, std::wstring());
    Check(!full.windows.front().imagePath.empty() && full.windows.front().exePathIncluded,
          "--exe-path 给了才把完整路径放进记录");
    const std::wstring f = Render(full, WindowAction::kList);
    Check(Contains(f, L"\"exePath\""), "--exe-path 那份真写了这个键（显式要求才多交信息）");
}

// ---------------------------------------------------------------------------
// 身份约束字段：形状与截图那一次一致，且明说不是凭证
// ---------------------------------------------------------------------------
void TestIdentityConstraints() {
    Section("身份约束字段：不是凭证、复核照旧、问不出来写 unknown");
    MatchOptions byTitle;
    byTitle.titles.push_back(L"Title");

    const WindowQueryResult r = BuildWindowQueryResult(
        Snapshot(ManyWindows(2)), InspectSpec(MultiMatch::kTopmost), byTitle, L"1");
    Check(r.hasTarget && r.windows.size() == 1, "取到唯一目标");
    const WindowRecord& rec = r.windows.front();
    Check(rec.identity.hwnd == 0x100, "身份快照的句柄就是这一条候选");
    Check(rec.identity.pid == rec.pid, "身份里的 PID 与顶层 pid 是同一个值（不是两处各问一次）");
    Check(rec.identity.className == L"TestClass", "身份里带类名");
    Check(rec.identity.processStartTicks != 0, "进程创建时间取的是枚举那一刻的值");
    // 条件里有标题、又给了 --monitor：复核要靠重跑当初那份条件，这一件事必须交出去。
    Check(rec.identity.selectionNeedsRecheck, "含标题条件或按屏过滤时写 selectionNeedsRecheck=true");

    const std::wstring doc = Render(r, WindowAction::kInspect);
    Check(Contains(doc, L"\"verificationRequired\": true"), "文档写明截图时仍要复核");
    Check(Contains(doc, L"\"isAuthorizationToken\": false"), "文档写明这不是凭证");
    Check(Contains(doc, L"\"raceWindowReducedNotEliminated\": true"),
          "文档写明只缩小竞态窗口、不声称消除");
    Check(Contains(doc, L"snapshot_expires") && Contains(doc, L"identity_fields_are_not_a_token"),
          "caveats 里两条同源 token 恒在");
    Check(Contains(doc, L"\"query\": \"inspect\"") &&
              Contains(doc, L"\"contract\": \"windowinspect\""),
          "inspect 那份用另一个契约名，调用方不会拿列表的解析器去读它");

    // 只给标题条件、没给 --monitor：仍要靠重跑条件认（易变属性按条件判）。
    const WindowQueryResult t2 = BuildWindowQueryResult(
        Snapshot(ManyWindows(1)), ListSpec(), byTitle, std::wstring());
    Check(t2.windows.front().identity.selectionNeedsRecheck, "标题条件 -> 复核要重跑条件");
    MatchOptions byClassOnly;
    byClassOnly.classes.push_back(L"TestClass");
    const WindowQueryResult t3 = BuildWindowQueryResult(
        Snapshot(ManyWindows(1)), ListSpec(), byClassOnly, std::wstring());
    Check(!t3.windows.front().identity.selectionNeedsRecheck,
          "只靠类名/句柄/PID 认出目标时不需要重跑条件（kCheap 那四问已覆盖）");

    // 问不出来的那一条（进程创建时间没基线）：写 unknown，而不是留 0 冒充一个真值。
    WindowInfo noStart = HealthyWindow(0x1239, 0);
    noStart.processStartTicks = 0;
    noStart.startRead = ReadState::kFailed;
    const WindowQueryResult r2 =
        BuildWindowQueryResult(Snapshot({noStart}), ListSpec(), MatchOptions(), std::wstring());
    const std::wstring d2 = Render(r2, WindowAction::kList);
    Check(Contains(d2, L"\"processStartTicks\": \"unknown\""),
          "没有基线时写 unknown（截图复核会整个跳过这一条判据，但那是一次没做出来的判定）");
    Check(!Contains(d2, L"\"processStartTicks\": 0"), "不写 0 冒充一个真值");
}

// ---------------------------------------------------------------------------
// 标题等窗口内容原样交付，不拼成需要再解析的描述串
// ---------------------------------------------------------------------------
void TestRawFieldsNotProse() {
    Section("标题/类名是独立字段，不拼成人话");
    MatchOptions none;
    WindowInfo tricky = HealthyWindow(0x1240, 0);
    tricky.title = L"a | b \"q\" \\path\\ \u4E2D";   // 竖线、引号、反斜杠、中日文都在里面
    const WindowQueryResult r =
        BuildWindowQueryResult(Snapshot({tricky}), ListSpec(), none, L"primary");
    Check(r.windows.front().title == tricky.title,
          "标题逐字交回：不截断、不替换竖线、不加引号、不 trim");

    const std::wstring doc = Render(r, WindowAction::kList);
    Check(Contains(doc, L"\"title\":"), "标题是独立的一个键");
    Check(Contains(doc, L"\"class\":"), "类名是独立的一个键");
    Check(!Contains(doc, L"\"input\""), "默认那份不含 input 段（参数回显只在 -v）");

    // -v 的回显判据要用**真的给了条件**那一次：input 段里那些数组是按用户写的取值填的。
    MatchOptions withTitle;
    withTitle.titles.push_back(tricky.title);
    const WindowQueryResult rv =
        BuildWindowQueryResult(Snapshot({tricky}), ListSpec(), withTitle, L"primary");
    const std::wstring vd = Render(rv, WindowAction::kList, /*verbose*/ true);
    Check(Contains(vd, L"\"monitor\": \"primary\"") && Contains(vd, L"\"target\": \"window\""),
          "-v 才回显这一次查询按哪块屏限缩（与截图那份的 input 段同一形状）");
    // input 段的数组是按缩进多行输出的，所以判「键在、值按原样在」，不判单行形状。
    // 数组那一行是「键 + 换行 + 缩进 + 值」的形状，值里的引号与反斜杠按 JSON 转义，
    // 所以这里只判键在、且标题原文那一段未被折叠的前缀逐字出现（大小写与空格都不许动）。
    Check(Contains(vd, L"\"title\":") && Contains(vd, L"a | b"),
          "-v 的条件回显里标题按用户写的那个值原样出现（不大小写折叠、不改写）");
    Check(rv.windows.front().title == tricky.title,
          "交回的窗口标题与条件里那一个逐字相同（同一份值走两条路不许各改一次）");
}

// ---------------------------------------------------------------------------
// 渲染的取舍：notes / caveats / 只读自述
// ---------------------------------------------------------------------------
void TestRenderChoices() {
    Section("渲染：notes 可抑制，caveats 与只读自述不可抑制");
    MatchOptions none;
    WindowQueryResult r = BuildWindowQueryResult(Snapshot(ManyWindows(2)), ListSpec(), none,
                                                 std::wstring());
    r.notes.push_back(Diagnostic{codes::kWindowQueryStale, L"stale message", L"", L"",
                                 L"stale hint"});

    const std::wstring normal = Render(r, WindowAction::kList);
    const std::wstring quiet = Render(r, WindowAction::kList, false, true);
    Check(Contains(normal, L"\"notes\""), "默认交付 notes（快照会过期那一条）");
    Check(!Contains(quiet, L"\"notes\""), "-q 抑制 notes：与截图那份契约同一规矩");
    Check(Contains(quiet, L"\"caveats\"") && Contains(quiet, L"snapshot_expires"),
          "caveats 不受 -q 影响：它是判据而不是礼貌性提示");
    Check(Contains(quiet, L"\"matched\": 2"), "分页那几个数也不被 -q 影响");
    Check(!Contains(normal, L"\"input\""), "默认不回显输入（别把参数回显塞进默认输出）");
    Check(Contains(Render(r, WindowAction::kList, true), L"\"input\""), "-v 才追加 input 段");

    Check(Contains(normal, L"\"pixelsRead\": 0") &&
              Contains(normal, L"\"consentDialogShown\": false") &&
              Contains(normal, L"\"filesWritten\": false"),
          "authorization 段把「没取像素/没弹框/没写文件」写成机器字段");
    Check(Contains(normal, L"\"yesAffectsResult\": false"),
          "--yes 对窗口查询的结果没有任何影响（含「哪些字段读得到」这一层）");
    Check(Contains(normal, L"\"identityFieldsAreNotConsent\": true"),
          "写明拿这份快照不能代替确认");
    Check(Contains(normal, L"no_window_touched"),
          "caveats 里写明不动任何窗口（不恢复、不激活、不移动）");
    Check(Contains(normal, L"no_capture_performed") && Contains(normal, L"no_consent_dialog_shown"),
          "caveats 里那几条只读承诺各占一个 token");

    // 空列表也要出一份完整的文档：字段齐不齐不能取决于命中数。
    const std::wstring emptyDoc =
        Render(BuildWindowQueryResult(Snapshot({}), ListSpec(), none, std::wstring()),
               WindowAction::kList);
    for (const wchar_t* key : {L"\"contract\"", L"\"contractVersion\"", L"\"query\"",
                               L"\"authorization\"", L"\"policy\"", L"\"pagination\"",
                               L"\"windows\"", L"\"caveats\"", L"\"matched\"", L"\"truncated\""}) {
        Check(Contains(emptyDoc, key), "空列表那份也带这个段落或字段");
    }
    Check(Contains(emptyDoc, L"\"windows\": []"), "空列表写成 []，不是缺这个键");
    // 翻页提示：只有真还有下一条时才给 nextOffset。
    Check(!Contains(emptyDoc, L"\"nextOffset\""), "没有剩下的东西时不写 nextOffset");
    const std::wstring paged =
        Render(BuildWindowQueryResult(Snapshot(ManyWindows(60)), ListSpec(), none, std::wstring()),
               WindowAction::kList);
    Check(Contains(paged, L"\"nextOffset\": 50") && Contains(paged, L"list_may_be_partial"),
          "被默认条数截断时给 nextOffset 并留一条「这份列表不是全集」");
}

// ---------------------------------------------------------------------------
// 退出码映射：与截图那一条同源
// ---------------------------------------------------------------------------
void TestExitCodeMapping() {
    Section("退出码映射：与截图那一条同源");
    Check(WindowQueryExitCodeFor(codes::kAmbiguousWindow) == EX_AMBIGUOUS, "歧义 -> 5");
    Check(WindowQueryExitCodeFor(codes::kNoWindow) == EX_NO_MATCH, "无匹配 -> 4");
    Check(WindowQueryExitCodeFor(codes::kIndexOutOfRange) == EX_USAGE, "索引越界 -> 1");
    Check(WindowQueryExitCodeFor(codes::kMonitorOutOfRange) == EX_USAGE, "屏幕编号越界 -> 1");
    Check(WindowQueryExitCodeFor(codes::kInvalidRegex) == EX_USAGE, "正则不合本机上限 -> 1");
    Check(WindowQueryExitCodeFor(codes::kMatchTimeout) == EX_CAPTURE_FAILED, "求值期限到点 -> 7");
    Check(WindowQueryExitCodeFor(codes::kWorkerFailed) == EX_CAPTURE_FAILED, "辅助进程故障 -> 7");
    Check(WindowQueryExitCodeFor(codes::kCaptureFailed) == EX_CAPTURE_FAILED, "求值本身失败 -> 7");
    // 没登记过的码不许折成 0：那会把一次失败说成「文档出完了」。
    Check(WindowQueryExitCodeFor(L"capture.brand_new_code") == EX_NO_MATCH,
          "没登记过的码按「没对上目标」处理，而不是当成功");
}

// ---------------------------------------------------------------------------
// 判据层的自相一致性：errors 非空就不交窗口
// ---------------------------------------------------------------------------
void TestNoPartialDocument() {
    Section("判据层的自相一致性");
    MatchOptions none;
    const WindowQueryResult amb =
        BuildWindowQueryResult(Snapshot(ManyWindows(3)), InspectSpec(), none, std::wstring());
    Check(!amb.errors.empty() && amb.windows.empty() && !amb.hasTarget,
          "歧义那份没有半套结果：一条窗口都不交");

    // --list 永不清空：即使命中很多也只分页，不产生「看起来像失败的空列表」。
    const WindowQueryResult many =
        BuildWindowQueryResult(Snapshot(ManyWindows(3)), ListSpec(), none, std::wstring());
    Check(many.errors.empty() && many.returned == 3,
          "同样的快照走 --list 就是 3 条 + 零错误（差别只在查询种类，不在求值）");

    // 句柄形状与截图那一份同形：调用方拿 images[].hwnd 与这里的 hwnd 能直接对上号。
    const WindowQueryResult single =
        BuildWindowQueryResult(Snapshot({HealthyWindow(0x001A0B4C, 0)}), ListSpec(), none,
                               std::wstring());
    Check(single.windows.front().hwndHex == L"0x001A0B4C",
          "句柄写成 0x 加 8 位十六进制，与 images[].hwnd / errors[].target 同一个格式");
}

}  // namespace

int main() {
    // 渲染不读文案资源（机器取值全 ASCII），但判据会经过 WindowMatch 的诊断那一路，
    // 那里要读 .rc 里编进的四份资源，所以整份判据按英文跑，与其他 state 测试同一约定。
    SetLanguage(Language::kEn);

    TestMachineWords();
    TestCombinedConditions();
    TestParseDefersRegexCompile();
    TestListIsNeverAmbiguous();
    TestInspectNeedsUniqueTarget();
    TestPagination();
    TestVisibilityPolicy();
    TestFieldReadability();
    TestPathPrivacy();
    TestIdentityConstraints();
    TestRawFieldsNotProse();
    TestRenderChoices();
    TestExitCodeMapping();
    TestNoPartialDocument();

    std::printf("\nwindows-state: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
