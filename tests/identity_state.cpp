// 目标身份复核的离线判据（由 tests\identity.ps1 运行 build\ecapture-identity-tests.exe）。
//
// 测的是生产判据本体：src/WindowIdentity.cpp 的 CheckWindowIdentity / IdentityDiagnostic /
// MakeWindowIdentity。这里注入的是**假查询层**，因为要验的正是那些安排不出来的现场：
//   * 同一个 HWND 值现在属于另一个进程（句柄被复用）
//   * PID 没变，但那个 PID 上是另一个进程（PID 被复用 —— 只有进程创建时间能分开这两件事）
//   * 窗口被销毁了（IsWindow 说没了）
//   * 标题变了：条件仍成立 / 条件不再成立（判据是"当初挑中它的理由"，不是"标题有没有动"）
//   * 每一问各自"问不出来"（必须按无法验证处理，不能当成"没发现不同 = 相同"）
// 真机上这些时序要么安排不出来、要么得杀掉别人的进程（本仓库不碰使用者真实应用），所以在这里
// 逐条注入。真机那一段（同一份脚本的后半）判的是接线：一次都不误伤、目标真没了确实报
// capture.target_gone、条件真失效确实报 capture.target_changed。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/Lang.h"
#include "../src/WindowIdentity.h"
#include "../src/WindowMatch.h"

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

// 一条"什么都没变"的查询层：每一问都照快照原样回答，条件重新求值也说"还在命中列表里"。
// 后面的判据各改其中一问，就能把身份复核的每条分支单独逼出来。
//
// 两处细节都是必要的：
//   * layer() 把回答**按值**捕进每个 lambda。按指针捕的话，写成 `Faithful(id).layer()`
//     这种一行式判据时 FakeLayer 是个临时对象，函数一返回 lambda 就指着回收掉的栈 ——
//     症状是判据随机飘，而不是干净地报错。
//   * 计数器放在 shared_ptr 后面，于是拷贝出来的一份份 lambda 仍然记到调用方手上那个
//     FakeLayer 的账上（"短路时有没有去问后面那一问"就是靠这个判的）。
struct FakeCounters {
    int alive = 0;
    int pid = 0;
    int cls = 0;
    int start = 0;
    int recheck = 0;
};

struct FakeLayer {
    bool alive = true;
    bool pidOk = true;
    bool classOk = true;
    bool startOk = true;
    bool recheckOk = true;
    uint32_t pid = 0;
    uint64_t startTicks = 0;
    std::wstring className;
    bool stillMatches = true;
    std::shared_ptr<FakeCounters> counts = std::make_shared<FakeCounters>();

    WindowQueryLayer layer() const {
        const FakeLayer self = *this;
        std::shared_ptr<FakeCounters> ctr = self.counts;
        WindowQueryLayer q;
        q.alive = [self, ctr](uint64_t) {
            ++ctr->alive;
            return self.alive;
        };
        q.windowPid = [self, ctr](uint64_t, uint32_t* out) {
            ++ctr->pid;
            if (!self.pidOk) return false;
            *out = self.pid;
            return true;
        };
        q.windowClass = [self, ctr](uint64_t, std::wstring* out) {
            ++ctr->cls;
            if (!self.classOk) return false;
            *out = self.className;
            return true;
        };
        q.processStart = [self, ctr](uint32_t, uint64_t* out) {
            ++ctr->start;
            if (!self.startOk) return false;
            *out = self.startTicks;
            return true;
        };
        q.selectionStillMatches = [self, ctr](uint64_t, bool* matched) {
            ++ctr->recheck;
            if (!self.recheckOk) return false;
            *matched = self.stillMatches;
            return true;
        };
        return q;
    }
};

WindowIdentity SampleIdentity(bool needsRecheck = false) {
    WindowIdentity id;
    id.hwnd = 0x001A0B4C;
    id.pid = 4242;
    id.processStartTicks = 133000000000000000ull;   // 一个像样的 FILETIME
    id.className = L"EcSampleClass";
    id.title = L"样本窗口";
    id.selectionNeedsRecheck = needsRecheck;
    return id;
}

FakeLayer Faithful(const WindowIdentity& id) {
    FakeLayer f;
    f.pid = id.pid;
    f.startTicks = id.processStartTicks;
    f.className = id.className;
    return f;
}

IdentityVerdict Run(const WindowIdentity& id, const WindowQueryLayer& q, IdentityScope scope,
                    IdentityFault* fault) {
    return CheckWindowIdentity(id, q, scope, fault);
}

const IdentityScope kCheap = IdentityScope::kCheap;
const IdentityScope kFull = IdentityScope::kFull;

// ---------------------------------------------------------------------------
// 一致 / 已销毁 / 短路顺序
// ---------------------------------------------------------------------------
void TestSameAndGone() {
    Section("同一个目标与已经销毁");
    const WindowIdentity id = SampleIdentity();
    IdentityFault fault;

    Check(Run(id, Faithful(id).layer(), kCheap, &fault) == IdentityVerdict::kSame,
          "每一问都照快照答：判同一个目标");
    Check(Run(id, Faithful(id).layer(), kFull, &fault) == IdentityVerdict::kSame,
          "kFull 同样通过（这个快照没带需要重问的条件）");
    Check(fault.detail.empty(), "判为同一个目标时不留下故障细节");

    FakeLayer dead = Faithful(id);
    dead.alive = false;
    Check(Run(id, dead.layer(), kFull, &fault) == IdentityVerdict::kGone,
          "IsWindow 说没了：判已销毁，不是判身份改变");
    Check(fault.detail.find("IsWindow") != std::string::npos,
          "已销毁那条把问法写进细节（IsWindow(...) = FALSE）");
    Check(dead.counts->pid == 0 && dead.counts->recheck == 0,
          "短路：句柄已失效时不再去问归属进程，更不再重跑条件求值（最贵的那一问留在最后）");
}

// ---------------------------------------------------------------------------
// 句柄复用 / PID 复用 / 类名变了
// ---------------------------------------------------------------------------
void TestReusedHandles() {
    Section("句柄与 PID 被复用");
    const WindowIdentity id = SampleIdentity();
    IdentityFault fault;

    FakeLayer otherProcess = Faithful(id);
    otherProcess.pid = 99999;
    Check(Run(id, otherProcess.layer(), kCheap, &fault) == IdentityVerdict::kChanged,
          "同一个 HWND 值现在属于另一个进程：判身份已变");
    Check(fault.detail.find("99999") != std::string::npos,
          "细节里给出两边的 PID（调用方由此看得见是句柄被复用）");

    // PID 相同但那是另一个进程：只有进程创建时间能把这两件事分开。
    FakeLayer pidReused = Faithful(id);
    pidReused.startTicks = id.processStartTicks + 1;
    Check(Run(id, pidReused.layer(), kCheap, &fault) == IdentityVerdict::kChanged,
          "PID 没变而进程创建时间变了：判身份已变（PID 被复用）");
    Check(fault.detail.find("pid reused") != std::string::npos, "细节里写明是 PID 复用");

    FakeLayer otherClass = Faithful(id);
    otherClass.className = L"Chrome_WidgetWin_1";
    Check(Run(id, otherClass.layer(), kCheap, &fault) == IdentityVerdict::kChanged,
          "类名不同：判身份已变（同一进程重建了另一类窗口也算）");

    FakeLayer otherCase = Faithful(id);
    otherCase.className = L"ecsAMPLEcLASS";   // 类名在注册时不区分大小写
    Check(Run(id, otherCase.layer(), kCheap, &fault) == IdentityVerdict::kSame,
          "类名只差大小写：仍是同一个目标（与 --class 那条条件的比法一致）");

    // 快照当时就读不到进程创建时间（0）：这一条整个跳过。反过来当成失败的话，
    // 别人以管理员身份开的那些窗口就再也截不了了 —— 那不是这次要修的东西。
    WindowIdentity noTicks = SampleIdentity();
    noTicks.processStartTicks = 0;
    FakeLayer unreadable = Faithful(noTicks);
    unreadable.startOk = false;
    Check(Run(noTicks, unreadable.layer(), kCheap, &fault) == IdentityVerdict::kSame,
          "基线当时就没有创建时间：跳过这一条，不判失败");
    FakeLayer otherNow = Faithful(noTicks);
    otherNow.startTicks = 777;
    Check(Run(noTicks, otherNow.layer(), kCheap, &fault) == IdentityVerdict::kSame,
          "基线为 0 时即使当场读到别的值也不作判定（没有基线就没有可比的那一对）");
}

// ---------------------------------------------------------------------------
// 问不出来 = 无法验证，绝不"没发现不同 = 相同"
// ---------------------------------------------------------------------------
void TestUnverifiable() {
    Section("有一道判据问不出来");
    const WindowIdentity id = SampleIdentity();
    IdentityFault fault;

    FakeLayer noPid = Faithful(id);
    noPid.pidOk = false;
    Check(Run(id, noPid.layer(), kCheap, &fault) == IdentityVerdict::kUnverifiable,
          "归属进程问不出来：判无法验证（IsWindow 说还在，不等于知道是谁的）");

    FakeLayer noClass = Faithful(id);
    noClass.classOk = false;
    Check(Run(id, noClass.layer(), kCheap, &fault) == IdentityVerdict::kUnverifiable,
          "类名问不出来：判无法验证");

    FakeLayer noStart = Faithful(id);
    noStart.startOk = false;
    Check(Run(id, noStart.layer(), kCheap, &fault) == IdentityVerdict::kUnverifiable,
          "基线有创建时间、现在读不到：判无法验证，而不是猜一个");

    Check(Run(id, WindowQueryLayer{}, kCheap, &fault) == IdentityVerdict::kUnverifiable,
          "查询层整层都没装：无法验证，而不是当成通过");

    // 重跑条件这一问只有 kFull + 快照确实带条件时才需要。
    const WindowIdentity titled = SampleIdentity(true);
    FakeLayer recheckBroken = Faithful(titled);
    recheckBroken.recheckOk = false;
    Check(Run(titled, recheckBroken.layer(), kFull, &fault) == IdentityVerdict::kUnverifiable,
          "条件重问不出来（辅助进程没起来 / 期限到了）：判无法验证，不去猜成不成立");
    Check(!fault.detail.empty(), "无法验证时留下【为什么问不出来】的细节");

    WindowQueryLayer noReselect = Faithful(titled).layer();
    noReselect.selectionStillMatches = nullptr;
    Check(Run(titled, noReselect, kCheap, &fault) == IdentityVerdict::kSame,
          "kCheap 不需要那一问：查询层没装它也照样判（所以每条通道之前都问得起）");
    Check(Run(titled, noReselect, kFull, &fault) == IdentityVerdict::kUnverifiable,
          "kFull 少了那一问就是没判完：按无法验证处理，不是跳过那条条件");
}

// ---------------------------------------------------------------------------
// 易变属性：标题与按屏过滤，按"当初的条件是否仍成立"判
// ---------------------------------------------------------------------------
void TestSelectionConditions() {
    Section("易变属性按原始条件判");
    IdentityFault fault;

    // 条件仍成立：标题被应用正常刷新过（重新求值仍然命中）=> 同一个目标。
    const WindowIdentity titled = SampleIdentity(true);
    FakeLayer refreshed = Faithful(titled);
    refreshed.stillMatches = true;
    Check(Run(titled, refreshed.layer(), kFull, &fault) == IdentityVerdict::kSame,
          "标题刷新过而条件仍成立：同一个目标，不判成换进程");

    // 条件不再成立：当初把它挑出来的理由已经不属于它了。
    FakeLayer broken = Faithful(titled);
    broken.stillMatches = false;
    Check(Run(titled, broken.layer(), kFull, &fault) == IdentityVerdict::kChanged,
          "标题（或所在那块屏）不再满足当初那条条件：判身份已变");
    Check(fault.detail.find("no longer satisfies") != std::string::npos,
          "细节说明是原始条件不再成立，而不是标题动了");

    // kCheap 不重跑条件：这是"每一档各自用在哪"的分界，也是成本的分界。
    FakeLayer brokenCheap = Faithful(titled);
    brokenCheap.stillMatches = false;
    Check(Run(titled, brokenCheap.layer(), kCheap, &fault) == IdentityVerdict::kSame,
          "kCheap 不重跑条件：回退链乘四遍也不会多枚举四次全部窗口");
    Check(brokenCheap.counts->recheck == 0, "kCheap 那一级一次都没去重跑条件求值");

    // 没靠易变条件挑中（--hwnd / --class / --pid 那类）：连重跑的入口都不需要。
    FakeLayer byHandle = Faithful(SampleIdentity(false));
    Check(Run(SampleIdentity(false), byHandle.layer(), kFull, &fault) == IdentityVerdict::kSame &&
              byHandle.counts->recheck == 0,
          "条件里没有易变那一条时不重跑求值（kFull 也一样省掉最贵的一问）");
}

// ---------------------------------------------------------------------------
// 稳定诊断：三条码各自成形
// ---------------------------------------------------------------------------
void TestDiagnostics() {
    Section("复核结果换成的稳定诊断");
    const uint64_t hwnd = 0x001A0B4C;

    const Diagnostic gone =
        IdentityDiagnostic(IdentityVerdict::kGone, "IsWindow(0x001A0B4C) = FALSE", hwnd);
    Check(gone.code == codes::kTargetGone, "已销毁 -> capture.target_gone");
    Check(gone.stage == stages::kCapture, "身份复核出在 capture 阶段（还没读像素）");
    Check(gone.target == L"0x001A0B4C", "诊断里指出是哪个目标（与 images[].hwnd 同形）");
    Check(!gone.message.empty() && !gone.hint.empty(), "三条文案都取得到（四份资源里都有）");
    Check(gone.option.empty() && gone.backend.empty(),
          "这不是某个选项写错了，也不是某条通道失败：那两个字段整个省略");

    const Diagnostic changed =
        IdentityDiagnostic(IdentityVerdict::kChanged, "pid 4242 -> 99999 (handle reused)", hwnd);
    Check(changed.code == codes::kTargetChanged, "换人 -> capture.target_changed");
    Check(changed.message.find(L"pid 4242 -> 99999") != std::wstring::npos,
          "机器细节进了 message（ASCII，四种语言共用同一条模板）");
    Check(!changed.hint.empty(), "换人那条也得给出下一步做什么的 hint");

    const Diagnostic unverifiable =
        IdentityDiagnostic(IdentityVerdict::kUnverifiable, "GetClassNameW failed", hwnd);
    Check(unverifiable.code == codes::kTargetUnverifiable, "问不出来 -> capture.target_unverifiable");
    Check(unverifiable.code != changed.code && unverifiable.code != gone.code,
          "三种结果是三条不同的码（调用方要能分开分支）");

    // 三条码都是新加的：按字符串比，别按指针比（指针相等只是巧合，不是一支可信的判据）。
    Check(std::wstring(codes::kTargetGone) != std::wstring(codes::kWindowGone) &&
              std::wstring(codes::kTargetChanged) != std::wstring(codes::kCaptureFailed),
          "新码没有顶掉任何一条旧码（capture.window_gone 讲的是取帧途中）");
}

// ---------------------------------------------------------------------------
// 快照来自枚举那一刻
// ---------------------------------------------------------------------------
void TestSnapshot() {
    Section("选定那一刻的快照");
    WindowInfo w;
    w.hwnd = 0x1234;
    w.pid = 555;
    w.processStartTicks = 999988;
    w.className = L"EcSampleClass";
    w.title = L"标题";

    MatchOptions titleCase;
    titleCase.titleContains = {L"标"};
    const WindowIdentity byTitle = MakeWindowIdentity(w, titleCase, false);
    Check(byTitle.hwnd == w.hwnd && byTitle.pid == w.pid &&
              byTitle.processStartTicks == w.processStartTicks,
          "句柄 / PID / 进程创建时间都取自枚举那一条记录（不是复核当时补问的）");
    Check(byTitle.className == w.className && byTitle.title == w.title,
          "类名与标题同样取自那一条记录");
    Check(byTitle.selectionNeedsRecheck, "靠标题挑中的目标：快照里记下【要重跑条件】这件事");

    MatchOptions byHandle;
    byHandle.hwnds = {0x1234};
    Check(!MakeWindowIdentity(w, byHandle, false).selectionNeedsRecheck,
          "只按 --hwnd 挑中：没有易变条件，不必重跑求值");

    MatchOptions byClass;
    byClass.classes = {L"ecs"};
    Check(!MakeWindowIdentity(w, byClass, false).selectionNeedsRecheck,
          "只按 --class 挑中：类名是 kCheap 那一问，不必重跑求值");

    MatchOptions none;
    Check(MakeWindowIdentity(w, none, true).selectionNeedsRecheck,
          "给了 --monitor（按屏过滤）：窗口挪到别的屏上就不再满足当初的条件");
}

}  // namespace

int main() {
    SetLanguage(Language::kEn);   // 判据只看 code / stage / 分支，文案跟着英文那份走
    TestSameAndGone();
    TestReusedHandles();
    TestUnverifiable();
    TestSelectionConditions();
    TestDiagnostics();
    TestSnapshot();

    std::printf("\nwindow-identity: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
