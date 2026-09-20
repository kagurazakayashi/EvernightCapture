// 异步管道 I/O 生命周期（WorkerIo）的离线判据。
//
// 为什么单独一个编译单元：这里判的是"取消请求返回之后、内核什么时候才真正放手"
// 这类**事件顺序**问题 —— 超时、取消受理、取消与完成竞争、ERROR_NOT_FOUND（它不是完成证据）、
// 等到了事件却仍报未完成、等待失败、对端提前结束，每一条都要精确控制"第几次等待返回什么、
// 取消回了什么错、结果什么时候可取"。
// 真命名管道给不出这种可控顺序（它只给一个真实结局），而发布版 ECAPTURE.EXE 里
// 不许有任何能注入假 I/O 的开关。所以判生产状态机本体（src/WorkerIo.cpp），
// 只把它与 Win32 之间那条接缝换成脚本化的假后端。
// 不在这层判的有两件：
//   * "进程正常退出时谁碰了仍未确认终态的记录"：那要看到 CRT 收尾之后的状态，用本次判据自己的
//     假后端判不了（它本身也是观察对象，先于被测资源析构就读到垃圾），所以由本测试 exe 以
//     --io-exit 模式另起一个子进程记录关闭/归属次序（见判据十三）。
//   * "登记表自己第一次堆分配失败""收养那个集合节点分配失败"这两支：要的是真把内存压干，
//     而给这份进程级单例开一个生产用的注入开关，已经超出本模块唯一的接缝（IoBackend）。
//     判据十二判的是这两支被兜住之后的对外契约（绝不抛出、问不到时的取值依据）。
//
// 每条判据都在三类断言上同时钉住：
//   1) 对外结局（done / timedOut / gle / transferred）与改造前的契约一致；
//   2) 资源顺序：事件在确认终态之前一个都不许关，确认之后恰好关一次（doubleClose == 0）；
//   3) 不多做：成功路径没有多余的取消与等待（unexpected == 0 保证没有脚本外的调用）。
//
// 只用 C 风格的 printf 汇报，判据写在断言里；任何一条不过就返回非 0。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <tuple>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../src/WorkerIo.h"

namespace ecapture {
namespace {

using namespace workerio;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// 脚本化假后端
// ---------------------------------------------------------------------------
class FakeBackend : public IoBackend {
public:
    // ---- 脚本（按调用次序弹出；弹空即"计划外调用"，计一次违例）----
    std::deque<std::pair<DWORD, DWORD>> waits;              // (WAIT_*，WAIT_FAILED 时的 gle)
    std::deque<std::pair<bool, DWORD>> cancels;             // (返回值，FALSE 时的 gle)
    std::deque<std::tuple<bool, size_t, DWORD>> fetches;    // (ok，transferred，FALSE 时的 gle)
    bool openEventOk = true;
    DWORD openEventGle = 0;

    // ---- 观察 ----
    std::vector<DWORD> waitMsSeen;   // 每次等待给的毫秒数：钉住"预算在前、宽限在后"
    int cancelCalls = 0;
    int fetchCalls = 0;
    int unexpected = 0;              // 任何脚本外的调用都必须为零
    int doubleClose = 0;             // 同一事件关第二次
    std::vector<HANDLE> eventsOpened;
    std::vector<HANDLE> eventsClosed;
    std::vector<HANDLE> targetsClosed;

    HANDLE OpenEvent(DWORD* gle) override {
        if (!openEventOk) {
            if (gle) *gle = openEventGle;
            return nullptr;
        }
        static long serial = 1;
        const HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xE000 + serial++));
        eventsOpened.push_back(h);
        return h;
    }
    void CloseEvent(HANDLE ev) override {
        if (std::find(eventsClosed.begin(), eventsClosed.end(), ev) != eventsClosed.end()) {
            ++doubleClose;
        }
        eventsClosed.push_back(ev);
    }
    DWORD WaitFor(HANDLE, DWORD ms, DWORD* gle) override {
        waitMsSeen.push_back(ms);
        if (waits.empty()) { ++unexpected; return WAIT_FAILED; }
        const auto [result, err] = waits.front();
        waits.pop_front();
        if (result == WAIT_FAILED && gle) *gle = err;
        return result;
    }
    bool Cancel(HANDLE, OVERLAPPED* ov, DWORD* gle) override {
        ++cancelCalls;
        if (cancels.empty()) { ++unexpected; return false; }
        // 取消必须带着那个操作的 OVERLAPPED 来：不许把指针丢了让整条句柄上所有 I/O 一起挨取消
        if (ov == nullptr) { ++unexpected; return false; }
        const auto [ok, err] = cancels.front();
        cancels.pop_front();
        if (!ok && gle) *gle = err;
        return ok;
    }
    bool Fetch(HANDLE, OVERLAPPED*, size_t* transferred, DWORD* gle) override {
        ++fetchCalls;
        if (fetches.empty()) { ++unexpected; if (transferred) *transferred = 0; return false; }
        const auto [ok, tr, err] = fetches.front();
        fetches.pop_front();
        if (transferred) *transferred = tr;
        if (!ok && gle) *gle = err;
        return ok;
    }
    void CloseTargetHandle(HANDLE h) override { targetsClosed.push_back(h); }

    int eventsAlive() const {
        return static_cast<int>(eventsOpened.size()) - static_cast<int>(eventsClosed.size());
    }
};

// 固定的假目标句柄：登记表按句柄值归组，各判据用不同值互不串扰。
const HANDLE kH1 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1001));
const HANDLE kH2 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1002));
const HANDLE kH3 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1003));
const HANDLE kH4 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1004));
const HANDLE kH5 = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1005));

// 一次"发起就进入飞行中"的读，并往内核可见的缓冲里预放图案（模拟已完成搬运的数据）。
IssueFn PendingReadIssued(std::vector<uint8_t>* touched, uint8_t fill) {
    return [touched, fill](OVERLAPPED&, uint8_t* buf, uint32_t cap) {
        if (touched) {
            touched->assign(buf, buf + cap);   // 记录这块缓冲归状态机所有
        }
        std::fill(buf, buf + cap, fill);
        return StartResult{StartState::kPending, 0};
    };
}

// ---------------------------------------------------------------------------
// 判据一：同步就失败 —— 没有未决操作，直接终态，一次都不多等、不多取消
// ---------------------------------------------------------------------------
void CheckSyncFailure() {
    FakeBackend be;
    bool issued = false;
    const IoStep s = RunOverlappedOp(
        be, kH1, 64, 1000, 500,
        [&](OVERLAPPED&, uint8_t*, uint32_t) {
            issued = true;
            return StartResult{StartState::kSyncFailed, 5};
        },
        nullptr);
    Check(issued, "同步失败前确实发起了请求");
    Check(!s.done && !s.timedOut && !s.unresolved && s.gle == 5, "同步失败原样上报且不算终态外的状态");
    Check(be.waits.empty() && be.cancelCalls == 0 && be.fetchCalls == 0,
          "同步失败不进入等待、不取消、不取结果");
    Check(be.eventsClosed.size() == 1 && be.doubleClose == 0 && be.eventsAlive() == 0,
          "同步失败后事件恰好关闭一次");
    Check(!HasPendingOps(kH1) && be.unexpected == 0, "同步失败不留登记表记录、无计划外调用");
}

// ---------------------------------------------------------------------------
// 判据二：事件建不出来 —— 没有发起过 I/O，也不许误关不存在的事件
// ---------------------------------------------------------------------------
void CheckEventCreateFailure() {
    FakeBackend be;
    be.openEventOk = false;
    be.openEventGle = 8;
    bool issued = false;
    const IoStep s = RunOverlappedOp(
        be, kH1, 64, 1000, 500,
        [&](OVERLAPPED&, uint8_t*, uint32_t) {
            issued = true;
            return StartResult{StartState::kPending, 0};
        },
        nullptr);
    Check(s.gle == 8 && !s.done && !s.unresolved, "事件创建失败按错误上报");
    Check(!issued, "没有事件就不发起 I/O");
    Check(be.eventsClosed.empty() && be.cancelCalls == 0 && be.unexpected == 0,
          "没有创建的事件不会被关、无计划外调用");
}

// ---------------------------------------------------------------------------
// 判据三：一次到位的正常完成（同步完成同样走这条）—— 不多等、不取消、复制回调用方
// ---------------------------------------------------------------------------
void CheckNormalCompletion() {
    FakeBackend be;
    be.waits.push_back({WAIT_OBJECT_0, 0});
    be.fetches.push_back({true, 40, 0});
    std::vector<uint8_t> out;
    const IoStep s = RunOverlappedOp(be, kH1, 64, 1000, 500, PendingReadIssued(nullptr, 0xAB), &out);
    Check(s.done && s.transferred == 40 && s.gle == 0 && !s.timedOut, "正常完成按原契约上报");
    Check(be.waitMsSeen.size() == 1 && be.waitMsSeen[0] == 1000, "只等了一次预算，没有额外宽限等待");
    Check(be.cancelCalls == 0, "成功路径不发起取消");
    Check(out.size() == 40 && std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0xAB; }),
          "数据在确认终态后才复制回调用方缓冲");
    Check(be.eventsClosed.size() == 1 && be.eventsAlive() == 0 && !HasPendingOps(kH1),
          "完成后事件恰好关闭一次、登记表无残留");
}

// ---------------------------------------------------------------------------
// 判据四：挂起的读超时 —— 取消受理、宽限内等到 ABORTED 终态，资源这才放手
// （写挂起超时是同一条状态机，只是 Issue 换成 WriteFile，这里以读为代表）
// ---------------------------------------------------------------------------
void CheckTimeoutCancelDrain() {
    FakeBackend be;
    be.waits.push_back({WAIT_TIMEOUT, 0});
    be.waits.push_back({WAIT_OBJECT_0, 0});
    be.cancels.push_back({true, 0});
    be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
    std::vector<uint8_t> out;
    const IoStep s = RunOverlappedOp(be, kH2, 64, 1000, 500, PendingReadIssued(nullptr, 0x5A), &out);
    Check(s.timedOut && !s.done && !s.unresolved && s.gle == 0, "超时上报 timedOut 且不算成功");
    Check(be.waitMsSeen.size() == 2 && be.waitMsSeen[0] == 1000 && be.waitMsSeen[1] == 500,
          "先等预算、再等取消收尾宽限（两段各有上限，不是无限等）");
    Check(be.cancelCalls == 1 && be.fetchCalls == 1, "恰好一次取消、一次终态确认");
    Check(out.empty(), "超时不向调用方交回任何数据");
    Check(be.eventsClosed.size() == 1 && be.doubleClose == 0 && !HasPendingOps(kH2),
          "确认终态后事件恰好关闭一次");
    Check(be.unexpected == 0, "取消排干路径没有计划外调用");
}

// ---------------------------------------------------------------------------
// 判据五：取消与正常完成竞争 —— 完成获胜也只算超时，但资源必须在终态后一次清理
// ---------------------------------------------------------------------------
void CheckCancelCompletionRace() {
    FakeBackend be;
    be.waits.push_back({WAIT_TIMEOUT, 0});
    be.waits.push_back({WAIT_OBJECT_0, 0});
    be.cancels.push_back({true, 0});
    be.fetches.push_back({true, 9, 0});   // 取消没赶上：操作正常完成了
    std::vector<uint8_t> out;
    const IoStep s = RunOverlappedOp(be, kH2, 64, 1000, 500, PendingReadIssued(nullptr, 0x11), &out);
    Check(s.timedOut && !s.done && s.transferred == 0,
          "竞争获胜的数据不冒充成功（预算已尽，对外仍是超时）");
    Check(out.empty(), "竞争获胜也不向调用方交出半截应答");
    Check(be.eventsClosed.size() == 1 && !HasPendingOps(kH2), "竞争获胜确认后照样一次清理");
}

// ---------------------------------------------------------------------------
// 判据六：取消受理但宽限内等不到终态 —— 外层必须能走，资源整份移交登记表，
// 之后由收尾排干认领、恰好清理一次
// ---------------------------------------------------------------------------
void CheckUnresolvedHandoffToRegistry() {
    FakeBackend be;
    be.waits.push_back({WAIT_TIMEOUT, 0});
    be.waits.push_back({WAIT_TIMEOUT, 0});   // 取消已受理，宽限也没等到
    be.cancels.push_back({true, 0});
    const IoStep s = RunOverlappedOp(be, kH3, 64, 1000, 500, PendingReadIssued(nullptr, 0x77), nullptr);
    Check(s.timedOut && s.unresolved && !s.done, "宽限内无终态：如实上报 unresolved，外层照走");
    Check(HasPendingOps(kH3), "未决操作整份（事件+OVERLAPPED+缓冲）移交登记表");
    Check(be.eventsClosed.empty() && be.eventsAlive() == 1,
          "未进终态之前事件一个都不关 —— 这正是内核还可能引用资源的证明");
    Check(be.fetchCalls == 0, "没等到事件就绝不取结果");

    // 外层请求结束后（比如辅助进程被收尸，对面没了），收尾排干认领它：
    be.waits.push_back({WAIT_OBJECT_0, 0});
    be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
    const DWORD left = DrainPendingOps(kH3, 777);
    Check(left == 0 && !HasPendingOps(kH3), "排干认领后登记表清空");
    Check(be.waitMsSeen.back() == 777, "收尾用的是收尾自己的等待上限");
    Check(be.eventsClosed.size() == 1 && be.doubleClose == 0 && be.eventsAlive() == 0,
          "认领之后恰好清理一次");
    Check(be.unexpected == 0, "整个移交-认领过程没有计划外调用");
}

// ---------------------------------------------------------------------------
// 判据七：CancelIoEx 回 ERROR_NOT_FOUND —— 它只是"没找到可取消的请求"，不是完成证据。
// 三个子情形按"完成通知到底来没来"分开钉住：来了就确认收尾，没来就绝不就地释放。
// ---------------------------------------------------------------------------
void CheckCancelNotFound() {
    {   // 子情形 1：请求刚好完成（事件已置起）
        FakeBackend be;
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.cancels.push_back({false, ERROR_NOT_FOUND});
        be.fetches.push_back({true, 12, 0});
        const IoStep s = RunOverlappedOp(be, kH4, 64, 1000, 500, PendingReadIssued(nullptr, 0x33), nullptr);
        Check(s.timedOut && !s.done && !s.unresolved,
              "NOT_FOUND 且事件已置起：确认完成收尾，对外仍按超时");
        Check(be.waitMsSeen.size() == 2 && be.waitMsSeen[1] == 0,
              "NOT_FOUND 分支用一次零等待探事件，不白等宽限");
        Check(be.eventsClosed.size() == 1 && !HasPendingOps(kH4), "该情形资源一次清理、不留记录");
    }
    {   // 子情形 2：零等待探不到的那一瞬间还没完成，宽限内才置起 —— 仍然确认得到终态
        FakeBackend be;
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.waits.push_back({WAIT_TIMEOUT, 0});    // 零等待探测：事件还没置起
        be.waits.push_back({WAIT_OBJECT_0, 0});   // 宽限内等到了
        be.cancels.push_back({false, ERROR_NOT_FOUND});
        be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
        const IoStep s = RunOverlappedOp(be, kH5, 64, 1000, 500, PendingReadIssued(nullptr, 0x44), nullptr);
        Check(s.timedOut && !s.done && !s.unresolved,
              "NOT_FOUND 后完成通知迟到：宽限内确认后照样就地收尾，不拖进登记表");
        Check(be.waitMsSeen.size() == 3 && be.waitMsSeen[1] == 0 && be.waitMsSeen[2] == 500,
              "先零等待探一次、再按宽限上限确认：两段各有上限，不是无限等");
        Check(be.eventsClosed.size() == 1 && be.doubleClose == 0 && !HasPendingOps(kH5),
              "该情形资源一次清理、不留记录");
    }
    {   // 子情形 3（本次缺陷本体）：NOT_FOUND 且完成通知始终没到。
        // 基线把 DrainOnce(0) 的失败结果丢掉就 MarkTerminal —— 等于拿"取消说没这回事"
        // 当完成证据，把内核可能还在引用的 OVERLAPPED 与缓冲区当场释放。
        FakeBackend be;
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.waits.push_back({WAIT_TIMEOUT, 0});   // 零等待探不到
        be.waits.push_back({WAIT_TIMEOUT, 0});   // 宽限也没等到
        be.cancels.push_back({false, ERROR_NOT_FOUND});
        const IoStep s = RunOverlappedOp(be, kH4, 64, 1000, 500, PendingReadIssued(nullptr, 0x55), nullptr);
        Check(s.timedOut && s.unresolved && !s.done,
              "NOT_FOUND 且没等到完成：如实 unresolved，外层仍照预算走人不空等");
        Check(HasPendingOps(kH4), "取消返回值不是完成证据：整份资源移交登记表继续看管");
        Check(be.eventsClosed.empty() && be.eventsAlive() == 1,
              "未确认终态前事件一个都不关（资源仍可能被内核引用）");
        Check(be.fetchCalls == 0 && be.cancelCalls == 1, "一次取消、没等到事件就不去取结果");
        // 完成通知终于来了：登记表认领、确认终态、一次清理（不是把每条操作都变成积压）
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
        Check(DrainPendingOps(kH4, 500) == 0 && !HasPendingOps(kH4),
              "之后的完成通知一到，登记表确认终态并清空");
        Check(be.eventsClosed.size() == 1 && be.doubleClose == 0 && be.eventsAlive() == 0,
              "认领之后恰好清理一次");
        Check(be.unexpected == 0, "整条 NOT_FOUND 保守路径没有计划外调用");
    }
}

// ---------------------------------------------------------------------------
// 判据七之二：等待到了事件，GetOverlappedResult 却仍报未完成 —— ERROR_IO_INCOMPLETE
// 不是终态。既不许当场释放资源，也不许把之后迟到的取回结果冒充成功交付。
// （真机上这一档只在"完成事件被别的来源置起"这类病理事件顺序下出现，
//   假后端是用来判保守行为的模型，不是真机实测到的时序 —— 见判据报告。）
// ---------------------------------------------------------------------------
void CheckIoIncompleteIsNotTerminal() {
    {   // 首段等待就到了却仍报未完成：继续请求取消，宽限内确认才算终态
        FakeBackend be;
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.cancels.push_back({true, 0});
        be.fetches.push_back({false, 0, ERROR_IO_INCOMPLETE});
        be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
        std::vector<uint8_t> out;
        const IoStep s = RunOverlappedOp(be, kH1, 64, 1000, 500, PendingReadIssued(nullptr, 0xC1), &out);
        Check(!s.done && !s.timedOut && !s.unresolved && s.gle == ERROR_IO_INCOMPLETE,
              "IO_INCOMPLETE 不被写成成功、也不冒充超时：如实带上最初那个未完成码");
        Check(out.empty(), "未完成的取回不向调用方交出任何数据");
        Check(be.cancelCalls == 1 && be.fetchCalls == 2,
              "第一次确认没到终态就请求取消、再确认一次（不是一次事件就收尾）");
        Check(be.eventsClosed.size() == 1 && be.doubleClose == 0 && !HasPendingOps(kH1),
              "第二次确认（ABORTED）才算终态：一次清理、不留记录");
    }
    {   // 取消之后仍报未完成：不是终态，整份移交登记表
        FakeBackend be;
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.cancels.push_back({true, 0});
        be.fetches.push_back({false, 0, ERROR_IO_INCOMPLETE});
        const IoStep s = RunOverlappedOp(be, kH2, 64, 1000, 500, PendingReadIssued(nullptr, 0xC2), nullptr);
        Check(s.timedOut && s.unresolved && !s.done, "取消后仍报未完成：unresolved，资源不外放");
        Check(HasPendingOps(kH2) && be.eventsClosed.empty(),
              "没确认终态就事件不关、记录留在登记表");
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
        Check(DrainPendingOps(kH2, 500) == 0 && be.eventsClosed.size() == 1 && !HasPendingOps(kH2),
              "之后真正收尾时登记表认领并一次清理");
    }
    {   // 对照组：取回明确失败（断管）就是终态，允许就地释放 —— 别把保守做过头变成积压
        FakeBackend be;
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.fetches.push_back({false, 0, ERROR_BROKEN_PIPE});
        const IoStep s = RunOverlappedOp(be, kH3, 64, 1000, 500, PendingReadIssued(nullptr, 0xC3), nullptr);
        Check(!s.done && !s.unresolved && s.gle == ERROR_BROKEN_PIPE,
              "明确的失败（断管）就是终态：原样上报、不当未决拖着");
        Check(be.eventsClosed.size() == 1 && !HasPendingOps(kH3), "该路径就地一次清理");
    }
}

// ---------------------------------------------------------------------------
// 判据八：等待本身失败（WAIT_FAILED）—— 最初故障必须保留，清理错误不许覆盖；
// 操作仍未终态时照样走取消排干，等不到就移交登记表
// ---------------------------------------------------------------------------
void CheckWaitFailed() {
    {   // 取消受理且排干成功：首错保留
        FakeBackend be;
        be.waits.push_back({WAIT_FAILED, 87});
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.cancels.push_back({true, 0});
        be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
        const IoStep s = RunOverlappedOp(be, kH1, 64, 1000, 500, PendingReadIssued(nullptr, 0x55), nullptr);
        Check(s.gle == 87 && !s.timedOut && !s.done && !s.unresolved,
              "WAIT_FAILED 的最初错误码不被取消收尾的 ABORTED 覆盖");
        Check(be.eventsClosed.size() == 1 && !HasPendingOps(kH1), "等待失败路径资源一次清理");
    }
    {   // 取消受理但排干等不到：unresolved，之后再认领
        FakeBackend be;
        be.waits.push_back({WAIT_FAILED, 6});
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.cancels.push_back({true, 0});
        const IoStep s = RunOverlappedOp(be, kH2, 64, 1000, 500, PendingReadIssued(nullptr, 0x66), nullptr);
        Check(s.gle == 6 && s.unresolved, "等待失败且取消未完成：如实 unresolved");
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
        Check(DrainPendingOps(kH2, 0) == 0 && be.eventsClosed.size() == 1,
              "等待失败留下的记录同样能被收尾认领、一次清理");
    }
    {   // 取消被拒（其他错误码）：按还在飞行处理，走宽限与登记表
        FakeBackend be;
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.cancels.push_back({false, ERROR_INVALID_PARAMETER});
        const IoStep s = RunOverlappedOp(be, kH3, 64, 1000, 500, PendingReadIssued(nullptr, 0x88), nullptr);
        Check(s.unresolved && HasPendingOps(kH3),
              "取消被拒不等于操作结束了：资源移交登记表继续看管");
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
        Check(DrainPendingOps(kH3, 500) == 0 && !HasPendingOps(kH3), "被拒记录照样可认领清理");
    }
}

// ---------------------------------------------------------------------------
// 判据九：多条未决记录共享一个句柄 + 句柄收养 —— 全数终态之前句柄一个都不许多关
// ---------------------------------------------------------------------------
void CheckAdoptedHandle() {
    const HANDLE kAdopted = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xAD07));
    FakeBackend be;
    for (int i = 0; i < 2; ++i) {
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.cancels.push_back({true, 0});
        const IoStep s =
            RunOverlappedOp(be, kAdopted, 64, 1000, 500, PendingReadIssued(nullptr, 0x99), nullptr);
        Check(s.unresolved, "两条挂起读都超时进登记表");
    }
    Check(AdoptHandle(kAdopted), "收养记上时如实回 true（记不上时调用方仍要放弃关闭责任）");
    // 先只排干得动一条：句柄必须还开着（还有未决 I/O 挂在它上面）
    be.waits.push_back({WAIT_OBJECT_0, 0});   // 第一条：确认终态
    be.waits.push_back({WAIT_TIMEOUT, 0});    // 第二条：这轮还是等不到
    be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
    Check(DrainPendingOps(kAdopted, 500) == 1, "一轮排干后还剩一条未决");
    Check(be.targetsClosed.empty(), "还有未决操作时收养的句柄不会被关");
    Check(be.eventsClosed.size() == 1, "先终态的那条事件先被正确回收");
    // 第二条也终态：句柄由登记表关掉，恰好一次
    be.waits.push_back({WAIT_OBJECT_0, 0});
    be.fetches.push_back({false, 0, ERROR_OPERATION_ABORTED});
    Check(DrainPendingOps(kAdopted, 500) == 0, "全数终态");
    Check(be.targetsClosed.size() == 1 && be.targetsClosed[0] == kAdopted,
          "收养的句柄被关闭恰好一次");
    Check(be.eventsClosed.size() == 2 && be.doubleClose == 0 && be.eventsAlive() == 0,
          "两条记录的事件各关一次、无重复");
    Check(!HasPendingOps(kAdopted) && be.unexpected == 0, "登记表清空且全程无计划外调用");
}

// ---------------------------------------------------------------------------
// 判据十：对端提前结束的字节语义 —— 0 字节完成照样是终态完成（ReadExact 拿它判断管），
// 以及 transferred 说谎时的防御钳制（内核可见缓冲绝不被越界交回）
// ---------------------------------------------------------------------------
void CheckZeroAndClamp() {
    {
        FakeBackend be;
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.fetches.push_back({true, 0, 0});   // 真管道里这就是"对方关了管道"
        std::vector<uint8_t> out;
        const IoStep s = RunOverlappedOp(be, kH1, 64, 1000, 500, PendingReadIssued(nullptr, 0xAA), &out);
        Check(s.done && s.transferred == 0 && out.empty(),
              "0 字节完成原样上报（断管判定留给调用方，不在状态机里吞掉）");
        Check(be.eventsClosed.size() == 1 && be.cancelCalls == 0, "该路径一次清理、不多取消");
    }
    {
        FakeBackend be;
        be.waits.push_back({WAIT_OBJECT_0, 0});
        be.fetches.push_back({true, 9999, 0});   // 恶意的谎报：比缓冲大得多
        std::vector<uint8_t> out;
        const IoStep s = RunOverlappedOp(be, kH2, 64, 1000, 500, PendingReadIssued(nullptr, 0xBB), &out);
        Check(s.done && s.transferred == 64 && out.size() == 64,
              "transferred 超过内核可见缓冲时防御性钳制，绝不越界交回");
    }
}

// ---------------------------------------------------------------------------
// 判据十一：已连接分支（ERROR_PIPE_CONNECTED）—— 连接早已成立、没有挂起 I/O，
// 必须就地进成功终态：不等事件、不取消、不取结果，也绝不伪装成等待内核通知的异步请求。
// 这条判据钉住 F03 的缺陷本体：原实现把它映射成 kSyncDone 去等一个永不置起的事件，
// 抢先连接因此被误报成超时（在假后端上表现为"多等了一次预算"）。
// ---------------------------------------------------------------------------
void CheckAlreadyConnected() {
    FakeBackend be;
    bool issued = false;
    const IoStep s = RunOverlappedOp(
        be, kH1, 0, 1000, 500,
        [&](OVERLAPPED&, uint8_t*, uint32_t) {
            issued = true;
            return StartResult{StartState::kAlreadyConnected, 0};
        },
        nullptr);
    Check(issued, "已连接分支照样先走完发起这一步");
    Check(s.done && !s.timedOut && !s.unresolved && s.gle == 0 && s.transferred == 0,
          "已连接就地判定为成功终态（不是超时、不是未决）");
    Check(be.waitMsSeen.empty() && be.cancelCalls == 0 && be.fetchCalls == 0,
          "已连接不等待、不取消、不取结果 —— 没有挂起操作可等");
    Check(be.eventsClosed.size() == 1 && be.doubleClose == 0 && be.eventsAlive() == 0,
          "已连接分支把自己建的事件恰好关闭一次");
    Check(!HasPendingOps(kH1) && be.unexpected == 0,
          "已连接不进登记表、全程无计划外调用");
}

// 同一状态机对"零预算 + 已连接"也不许多做一次等待：预算烧尽了连接照样立刻算成功。
void CheckAlreadyConnectedWithSpentBudget() {
    FakeBackend be;
    const IoStep s = RunOverlappedOp(
        be, kH2, 0, 0, 500,
        [](OVERLAPPED&, uint8_t*, uint32_t) {
            return StartResult{StartState::kAlreadyConnected, 0};
        },
        nullptr);
    Check(s.done && !s.timedOut && be.waitMsSeen.empty(),
          "预算已尽时已连接分支不进入任何等待（等待只对确有挂起操作的路径有意义）");
}

// ---------------------------------------------------------------------------
// 判据十二：登记表的对外契约 —— 绝不抛出，且"问不到"时的取值各有依据
//   1) 这三个入口由 Transaction::Finish 直接调用，而 Finish 还会从它默认 noexcept 的析构里
//      再走一遍：异常一逸出就是终止进程，原本的结构化错误与收尾路径全丢。这一条用编译期
//      断言钉住 —— 源码把 noexcept 拿掉（或把收养的返回值签名改回 void）就直接编不过。
//   2) 登记表还空着的时候（下面用的句柄值没被任何判据用过），三个入口回的是"确有依据"的值：
//      没有未决、还剩 0 条、收养记上了；而登记表问不到这条句柄归哪个后端时，它宁可什么都不关。
//      依据是"登记任何操作都要求登记表先存在"，不是猜的。
// 登记表自己第一次堆分配失败、以及收养那个集合节点分配失败这两支，要的是真把内存压干；
// 给匿名命名空间里的这份单例开一个生产用的注入开关，不在本模块的接缝（IoBackend）之内，
// 所以这里判的是这两支被兜住之后的对外契约与收尾路径，不判那两次分配本身。
// ---------------------------------------------------------------------------
static_assert(noexcept(HasPendingOps(kH1)), "HasPendingOps 必须不抛出：它会被 noexcept 析构链调用");
static_assert(noexcept(DrainPendingOps(kH1, 0)), "DrainPendingOps 必须不抛出：同上");
static_assert(noexcept(AdoptHandle(kH1)), "AdoptHandle 必须不抛出：同上");

void CheckRegistryContract() {
    FakeBackend be;
    const HANDLE kCold = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xC01D));
    Check(!HasPendingOps(kCold), "空登记表：如实回'没有未决'（登记要求登记表先存在）");
    Check(DrainPendingOps(kCold, 0) == 0, "空登记表：一轮排干剩 0 条");
    Check(be.targetsClosed.empty() && be.eventsOpened.empty() && be.unexpected == 0,
          "这三次问询一个后端调用都不做，更不会顺手去关句柄");
    Check(AdoptHandle(kCold), "收养记上时如实回 true");
    Check(DrainPendingOps(kCold, 0) == 0, "本来就没有记录的收养句柄：排干仍算 0 条待收尾");
    // 这条句柄上一条记录都没有，登记表问不到它归哪个后端（后端是跟着记录登记的），于是
    // 宁可什么都不关 —— 绝不拿别的句柄的后端去关一个自己认不出的值。生产里收养只发生在
    // "排干后还剩记录"的那一支（见 Transaction::Finish），那种场合后端问得到、由判据九钉住。
    Check(be.targetsClosed.empty() && be.unexpected == 0,
          "问不到归属后端时不关句柄：不猜、也不误用别人的后端");
    Check(!HasPendingOps(kCold), "该句柄始终没有未决记录");
}

// ---------------------------------------------------------------------------
// 判据十三：进程正常退出这条路径上，登记表里仍未确认终态的记录不会被谁析构掉。
//
// 为什么用独立子进程：这件事判的是"CRT 收尾时静态对象的析构次序"，在同一次判据里用
// 普通的假后端判不了 —— 那个假后端本身也是这次判据的观察对象，它先于被测资源销毁时，
// 读到的计数根本分不清是谁干的（甚至可能是已销毁对象上的垃圾）。所以这里由测试 exe
// 自己以 --io-exit 模式再跑一份子进程：
//   * 后端是一段只有标量成员的静态存储对象，没有析构函数，进程收尾时仍然可读；
//     文件里带一个魔数当哨兵，读出来不对就说明观察点落在了已析构的对象上；
//   * 观察点写在 atexit 回调里，且这条回调在任何 WorkerIo 使用之前登记 —— MSVC 的收尾
//     按登记逆序执行，静态对象的析构是在各自构造时登记的，因此"最早登记"的这个回调
//     最后跑，看到的就是"进程收尾全部做完"之后的状态；
//   * 子进程正常从入口返回（不是被 TerminateProcess），才走得到这一段收尾。
// 现场由两条操作组成：一条正常完成（证明修复没把每条操作都变成积压），一条始终确认不了
// 终态（留在登记表里）。父进程只核对子进程写出的关闭/归属次序，不参与它的内存管理。
// ---------------------------------------------------------------------------
constexpr int kExitMagic = 0x517ACE;
// 登记表按句柄值归组，这里用两个假句柄值：一条正常完成、一条故意留成未确认终态。
const HANDLE kExitTerminalHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xEE01));
const HANDLE kExitRetainedHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xEE02));

// 活到进程最后一刻的后端：只有标量成员，可平凡析构，收尾之后读它依然是定义良好的。
class ExitProbeBackend final : public IoBackend {
public:
    int magic = kExitMagic;
    int opened = 0;
    int closes = 0;
    int targetsClosed = 0;
    int immediateWaitsLeft = 0;   // 大于 0 时等待回"事件已置起"，回完就一律超时
    int closesRunEnd = 0;
    int pendingRunEnd = 0;

    HANDLE OpenEvent(DWORD*) override {
        ++opened;
        return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xF000 + opened));
    }
    void CloseEvent(HANDLE) override { ++closes; }
    DWORD WaitFor(HANDLE, DWORD, DWORD*) override {
        if (immediateWaitsLeft > 0) {
            --immediateWaitsLeft;
            return WAIT_OBJECT_0;
        }
        return WAIT_TIMEOUT;
    }
    bool Cancel(HANDLE, OVERLAPPED*, DWORD*) override { return true; }
    bool Fetch(HANDLE, OVERLAPPED*, size_t* transferred, DWORD*) override {
        if (transferred) *transferred = 0;
        return true;
    }
    void CloseTargetHandle(HANDLE) override { ++targetsClosed; }
};

// 静态存储期 + 可平凡析构：CRT 收尾不会销毁它，atexit 回调读它才站得住。
ExitProbeBackend g_exitProbe;
const wchar_t* g_exitEvidencePath = nullptr;

void WriteExitEvidence(const char* line) {
    if (g_exitEvidencePath == nullptr) return;
    const HANDLE h = CreateFileW(g_exitEvidencePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(h, line, static_cast<DWORD>(std::strlen(line)), &put, nullptr);
    CloseHandle(h);
}

// 进程收尾全部做完之后跑的最后一个观察点：直接问生产入口"那条记录还归登记表管吗"，
// 以及"收尾阶段有没有又多关一个事件"。
void DumpExitEvidence() {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "magic=%d opened=%d closesRunEnd=%d pendingRunEnd=%d closesTeardown=%d "
                  "pendingTeardown=%d targetsClosed=%d\n",
                  g_exitProbe.magic, g_exitProbe.opened, g_exitProbe.closesRunEnd,
                  g_exitProbe.pendingRunEnd, g_exitProbe.closes,
                  HasPendingOps(kExitRetainedHandle) ? 1 : 0, g_exitProbe.targetsClosed);
    WriteExitEvidence(line);
}

// 子进程的下场（wmain 的 --io-exit 模式经薄封装调进来）：不打印、不起判据，
// 只造出现场然后正常退出。
int ExitPeerMain(const wchar_t* evidencePath) {
    g_exitEvidencePath = evidencePath;
    // 必须在任何 WorkerIo 使用之前登记，理由见上面那段说明。
    if (std::atexit(&DumpExitEvidence) != 0) return 7;

    // 现场一：一条正常完成的操作 —— 事件该在这场运行里就关掉，收尾不留积压。
    g_exitProbe.immediateWaitsLeft = 1;
    RunOverlappedOp(g_exitProbe, kExitTerminalHandle, 64, 1000, 500,
                    PendingReadIssued(nullptr, 0xE1), nullptr);

    // 现场二：一条确认不了终态的操作 —— 取消已受理、宽限（这里是零）也等不到，整份移交登记表，
    // 然后什么都不主动做：让进程正常退出，看谁会在收尾时碰它。
    const IoStep s = RunOverlappedOp(g_exitProbe, kExitRetainedHandle, 64, 0, 0,
                                     PendingReadIssued(nullptr, 0xE2), nullptr);
    if (!s.unresolved) return 6;
    g_exitProbe.pendingRunEnd = HasPendingOps(kExitRetainedHandle) ? 1 : 0;
    g_exitProbe.closesRunEnd = g_exitProbe.closes;
    return 0;   // 正常返回：走 CRT 收尾，静态析构与 atexit 都会跑
}

// 从证据文本里取一个十进制字段（不用 scanf 家族：MSVC 把它们标成弃用，会污染构建输出）。
bool ReadEvidenceField(const char* text, const char* key, long* out) {
    const std::string prefix = std::string(key) + "=";
    const char* at = std::strstr(text, prefix.c_str());
    if (at == nullptr) return false;
    const char* digits = at + prefix.size();
    char* end = nullptr;
    *out = std::strtol(digits, &end, 10);
    return end != digits;
}

void CheckExitOrderInSubprocess() {
    wchar_t exe[MAX_PATH] = {};
    const DWORD nameLen = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Check(nameLen > 0 && nameLen < MAX_PATH, "退出次序判据：取到测试 exe 自己的路径");
    if (nameLen == 0 || nameLen >= MAX_PATH) return;

    wchar_t dir[MAX_PATH] = {};
    wchar_t file[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, dir) == 0 || GetTempFileNameW(dir, L"ecio", 0, file) == 0) {
        Check(false, "退出次序判据：建不出临时证据文件");
        return;
    }

    std::wstring cmd = std::wstring(L"\"") + exe + L"\" --io-exit \"" + file + L"\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        Check(false, "退出次序判据：子进程起不来");
        DeleteFileW(file);
        return;
    }
    // 有界等待：子进程只做两次状态机调用就正常退出。等不到就是它卡住了，只结束我们自己起的这个。
    const DWORD wr = WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    if (wr == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    if (wr != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 9);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Check(wr == WAIT_OBJECT_0 && code == 0,
          "退出次序判据：子进程自己正常退出（没卡住、没在收尾阶段崩溃）");

    char buf[512] = {};
    size_t got = 0;
    const HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD rd = 0;
        if (ReadFile(h, buf, static_cast<DWORD>(sizeof(buf) - 1), &rd, nullptr)) got = rd;
        CloseHandle(h);
    }
    DeleteFileW(file);
    Check(got > 0, "退出次序判据：读得到子进程在收尾之后写出的关闭/归属次序");

    long magic = -1, opened = -1, closesRunEnd = -1, pendingRunEnd = -1;
    long closesTeardown = -1, pendingTeardown = -1, targetsClosed = -1;
    int fields = 0;
    fields += ReadEvidenceField(buf, "magic", &magic) ? 1 : 0;
    fields += ReadEvidenceField(buf, "opened", &opened) ? 1 : 0;
    fields += ReadEvidenceField(buf, "closesRunEnd", &closesRunEnd) ? 1 : 0;
    fields += ReadEvidenceField(buf, "pendingRunEnd", &pendingRunEnd) ? 1 : 0;
    fields += ReadEvidenceField(buf, "closesTeardown", &closesTeardown) ? 1 : 0;
    fields += ReadEvidenceField(buf, "pendingTeardown", &pendingTeardown) ? 1 : 0;
    fields += ReadEvidenceField(buf, "targetsClosed", &targetsClosed) ? 1 : 0;
    Check(fields == 7, "退出次序判据：证据文件的字段齐全");
    Check(magic == kExitMagic,
          "退出次序判据：观察点用的后端在收尾时仍未析构（读到的不是已销毁对象上的垃圾）");
    Check(pendingRunEnd == 1,
          "负向对照：运行末尾确实有一条未确认终态的记录留在登记表里");
    Check(closesRunEnd == 1 && opened == 2,
          "正常完成那条在运行中就关掉了事件：修复没把每条操作都变成积压");
    Check(closesTeardown == closesRunEnd,
          "收尾之后再没关过事件：登记表没在析构里销毁那条未决记录（内核可能还在引用的资源一个都没放手）");
    Check(pendingTeardown == 1,
          "收尾之后那条记录仍归登记表所有，等系统随进程统一回收");
    Check(targetsClosed == 0, "没被收养的句柄不会被登记表关掉");
}

}  // namespace

// isolation_state.cpp 的 wmain 在 --io-exit 模式下调进来：那一份子进程就是判据十三的现场。
int RunWorkerIoExitPeerMode(const wchar_t* evidencePath) { return ExitPeerMain(evidencePath); }

int RunWorkerIoStateChecks(int* checksOut, int* failuresOut) {
    std::printf("异步管道 I/O 生命周期（WorkerIo）离线判据\n");
    CheckSyncFailure();
    CheckEventCreateFailure();
    CheckNormalCompletion();
    CheckTimeoutCancelDrain();
    CheckCancelCompletionRace();
    CheckUnresolvedHandoffToRegistry();
    CheckCancelNotFound();
    CheckIoIncompleteIsNotTerminal();
    CheckWaitFailed();
    CheckAdoptedHandle();
    CheckZeroAndClamp();
    CheckAlreadyConnected();
    CheckAlreadyConnectedWithSpentBudget();
    CheckRegistryContract();
    CheckExitOrderInSubprocess();
    if (checksOut) *checksOut = g_checks;
    if (failuresOut) *failuresOut = g_failures;
    return g_failures;
}

}  // namespace ecapture
