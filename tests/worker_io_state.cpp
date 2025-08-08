// 异步管道 I/O 生命周期（WorkerIo）的离线判据。
//
// 为什么单独一个编译单元：这里判的是"取消请求返回之后、内核什么时候才真正放手"
// 这类**事件顺序**问题 —— 超时、取消受理、取消与完成竞争、ERROR_NOT_FOUND、等待失败、
// 对端提前结束，每一条都要精确控制"第几次等待返回什么、取消回了什么错、结果什么时候可取"。
// 真命名管道给不出这种可控顺序（它只给一个真实结局），而发布版 ECAPTURE.EXE 里
// 不许有任何能注入假 I/O 的开关。所以判生产状态机本体（src/WorkerIo.cpp），
// 只把它与 Win32 之间那条接缝换成脚本化的假后端。
//
// 每条判据都在三类断言上同时钉住：
//   1) 对外结局（done / timedOut / gle / transferred）与改造前的契约一致；
//   2) 资源顺序：事件在确认终态之前一个都不许关，确认之后恰好关一次（doubleClose == 0）；
//   3) 不多做：成功路径没有多余的取消与等待（unexpected == 0 保证没有脚本外的调用）。
//
// 只用 C 风格的 printf 汇报，判据写在断言里；任何一条不过就返回非 0。
#include <algorithm>
#include <cstdio>
#include <deque>
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
// 判据七：CancelIoEx 回 ERROR_NOT_FOUND —— 必须查完成状态，不能当"没这回事"
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
    {   // 子情形 2：这个 OVERLAPPED 根本不曾入队（对端提前结束的幽灵连接就长这样）
        FakeBackend be;
        be.waits.push_back({WAIT_TIMEOUT, 0});
        be.waits.push_back({WAIT_TIMEOUT, 0});   // 零等待探事件也没置起
        be.cancels.push_back({false, ERROR_NOT_FOUND});
        const IoStep s = RunOverlappedOp(be, kH5, 64, 1000, 500, PendingReadIssued(nullptr, 0x44), nullptr);
        Check(s.timedOut && !s.unresolved,
              "NOT_FOUND 且事件从未置起：内核不引用任何东西，按终态走，不拖幽灵记录");
        Check(!HasPendingOps(kH5), "幽灵操作不进登记表，句柄不会被白白收养");
        Check(be.eventsClosed.size() == 1 && be.doubleClose == 0, "幽灵操作资源照样一次清理");
        Check(be.fetchCalls == 0 && be.unexpected == 0, "对没有入队的操作不去取结果");
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
    AdoptHandle(kAdopted);
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

}  // namespace

int RunWorkerIoStateChecks(int* checksOut, int* failuresOut) {
    std::printf("异步管道 I/O 生命周期（WorkerIo）离线判据\n");
    CheckSyncFailure();
    CheckEventCreateFailure();
    CheckNormalCompletion();
    CheckTimeoutCancelDrain();
    CheckCancelCompletionRace();
    CheckUnresolvedHandoffToRegistry();
    CheckCancelNotFound();
    CheckWaitFailed();
    CheckAdoptedHandle();
    CheckZeroAndClamp();
    CheckAlreadyConnected();
    CheckAlreadyConnectedWithSpentBudget();
    if (checksOut) *checksOut = g_checks;
    if (failuresOut) *failuresOut = g_failures;
    return g_failures;
}

}  // namespace ecapture
