#include "WorkerIo.h"

#include <algorithm>
#include <memory>

namespace ecapture {
namespace workerio {
namespace {

// ---------------------------------------------------------------------------
// PendingOperation：一次重叠操作的全部内核可见资源。
// 所有权规则只有一条：终态确认之前，事件、OVERLAPPED、缓冲区一个都不许放手。
// ---------------------------------------------------------------------------
class PendingOperation {
public:
    PendingOperation(IoBackend& backend, HANDLE hFile, uint32_t bufCap)
        : backend_(backend), hFile_(hFile), buf_(bufCap) {}

    PendingOperation(const PendingOperation&) = delete;
    PendingOperation& operator=(const PendingOperation&) = delete;

    ~PendingOperation() {
        // 只有"已确认终态"的对象才允许走到这里：登记表只在 TrySettle 返回 true 之后摘链并删记录，
        // RunOverlappedOp 只在终态分支释放自己那份。未确认终态的对象由永不析构的登记表持到
        // 进程结束，交给系统统一回收 —— 见 Registry 的类注释。
        CloseEventOnce();
    }

    // 被登记表接管时置真，调用方据此不再删除自己这份指针。
    bool registered() const { return registered_; }
    bool terminal() const { return terminal_; }
    HANDLE file() const { return hFile_; }
    IoBackend& backend() { return backend_; }
    const std::vector<uint8_t>& buffer() const { return buf_; }

    // 状态机本体。见头文件注释的分支说明。
    IoStep Execute(DWORD waitMs, DWORD graceMs, const IssueFn& issue) {
        IoStep step;
        DWORD gle = 0;
        event_ = backend_.OpenEvent(&gle);
        if (!event_) {
            // 连事件都建不出来：没有发起过任何 I/O，本来就没有未决资源。
            step.gle = gle;
            terminal_ = true;
            return step;
        }
        ov_.hEvent = event_;   // 生产后端靠这个字段收完成通知；假后端忽略内容

        const StartResult started = issue(ov_, buf_.data(), static_cast<uint32_t>(buf_.size()));
        if (started.state == StartState::kSyncFailed) {
            // 同步失败 = 没有未决操作，直接终态。
            step.gle = started.gle;
            MarkTerminal();
            return step;
        }
        if (started.state == StartState::kAlreadyConnected) {
            // 已连接（ERROR_PIPE_CONNECTED）= 连接早已成立：这次调用没有发起挂起 I/O，事件
            // 不会由系统置起。就地进成功终态 —— 不等事件、不取消、不取结果，把自己建的
            // 事件关掉就走；后续的身份核验与任务交换归调用方。
            step.done = true;
            MarkTerminal();
            return step;
        }

        // kSyncDone 与 kPending 都从等事件起步：同步完成时事件已由系统置起，
        // 这次等待会立即返回（与原实现一致，不多等一轮）。
        const DWORD wait = backend_.WaitFor(event_, waitMs, &gle);
        if (wait == WAIT_OBJECT_0 && FetchConfirmed(step)) {
            MarkTerminal();
            return step;
        }
        if (wait == WAIT_TIMEOUT) {
            step.timedOut = true;
        } else if (wait == WAIT_FAILED) {
            step.gle = gle;   // WAIT_FAILED：这是本条操作的最初故障，后面不许覆盖
        }

        // 到这里必定还没确认终态：要么没等到事件，要么等到了但系统仍报这条没收尾。
        // 请求取消，再确认终态。CancelIoEx 的三种回话：
        //   * TRUE：取消已排入，操作将以 ABORTED / 竞争获胜 / 其他错误收尾，事件必定置起
        //     —— 在宽限内等它；等不到就把整份资源移交登记表，绝不空手放手。
        //   * FALSE + ERROR_NOT_FOUND：只说明"此刻没找到可取消的这条请求"，它**不是完成证据**。
        //     两种可能 —— 操作刚好完成（事件应已置起），或这个 OVERLAPPED 根本不曾真正入队
        //     （历史缺陷：调用方把 ERROR_PIPE_CONNECTED 这种"早已连上"当成发起；现在
        //     ConnectStartOf 已把四类分开，真机走不到幽灵那支，但下面的保守处理不靠这个前提）。
        //     先用一次零等待探事件：置起并确认收尾就按终态走、不白等宽限；探不到就照
        //     "还在飞行"处理 —— 继续走宽限排干那一条，仍然确认不了就整份移交登记表。
        //     代价是疑似幽灵记录也会占一条登记（这条句柄上多一个收养等待者），换来的是
        //     "取消说没这回事"绝不被当成可以放心释放。
        //   * 其他 FALSE：取消没被受理，操作按还在飞行处理 —— 同样走宽限排干那一条。
        DWORD cancelGle = 0;
        const bool cancelRequested = backend_.Cancel(hFile_, &ov_, &cancelGle);
        if (!cancelRequested && cancelGle == ERROR_NOT_FOUND && SettleWithin(0)) return step;
        if (SettleWithin(graceMs)) return step;
        registered_ = true;
        step.unresolved = true;
        return step;
    }

    // 登记表的收尾通道：确认终态则关闭事件并返回 true（记录随即可删）。
    // 取回的结果（多半是 ERROR_OPERATION_ABORTED，竞争获胜时是正常完成）都不再改写
    // 对外结局 —— 预算在那次超时就已经花掉了，这里只负责把资源安全放手。
    bool TrySettle(DWORD waitMs) { return SettleWithin(waitMs); }

private:
    // 有界地确认一次终态：等到完成事件 -> 取回完成状态。返回 true 表示内核已确认这条请求
    // 收尾（正常完成、ERROR_OPERATION_ABORTED 或其他明确失败都算 —— 请求不再被内核引用），
    // 于是关闭事件、OVERLAPPED 与缓冲区这才允许被回收。
    // 等到超时、等待失败、或 GetOverlappedResult 仍报未完成（ERROR_IO_INCOMPLETE /
    // ERROR_IO_PENDING：事件被别的来源置起，或取消还在排队）都不算确认，返回 false，
    // 资源继续由调用方或登记表看管。ERROR_IO_INCOMPLETE 从来不是终态。
    bool SettleWithin(DWORD waitMs) {
        if (terminal_) return true;
        DWORD gle = 0;
        if (backend_.WaitFor(event_, waitMs, &gle) != WAIT_OBJECT_0) return false;
        size_t transferred = 0;
        const bool ok = backend_.Fetch(hFile_, &ov_, &transferred, &gle);
        if (!ok && (gle == ERROR_IO_INCOMPLETE || gle == ERROR_IO_PENDING)) return false;
        MarkTerminal();
        return true;
    }

    void MarkTerminal() {
        terminal_ = true;
        CloseEventOnce();
    }

    void CloseEventOnce() {
        if (event_) {
            backend_.CloseEvent(event_);
            event_ = nullptr;
        }
    }

    // 事件已置起之后取回结果，并把"这条请求是否已经收尾"回报给调用方。
    // 只用于取消之前那次首段等待：之后的确认走 SettleWithin，那里的取回结果不再改写
    // 对外结局（预算已尽，不把取消边缘上回来的半截数据冒充成功交付）。
    // 返回 true = 系统确认请求已收尾（成功，或 ABORTED / 断管这类明确失败），资源可以释放；
    // false = 仍报未完成（ERROR_IO_INCOMPLETE / ERROR_IO_PENDING），这不是终态证据。
    bool FetchConfirmed(IoStep& step) {
        size_t transferred = 0;
        DWORD gle = 0;
        const bool ok = backend_.Fetch(hFile_, &ov_, &transferred, &gle);
        if (ok) {
            step.done = true;
            // 防御性钳制：真实 API 承诺不超过给定缓冲；假后端或未来的调用方给错时，
            // 宁可少报也不能让 transferred 指向缓冲区之外的内存。
            step.transferred = std::min(transferred, buf_.size());
            return true;
        }
        if (step.gle == 0) step.gle = gle;   // 首错如实带上，之后不许被收尾覆盖
        return gle != ERROR_IO_INCOMPLETE && gle != ERROR_IO_PENDING;
    }

    IoBackend& backend_;
    HANDLE hFile_ = nullptr;
    OVERLAPPED ov_{};   // hEvent 走 event_ 字段管理，其余成员恒为 0
    HANDLE event_ = nullptr;
    std::vector<uint8_t> buf_;
    bool terminal_ = false;
    bool registered_ = false;

public:
    // 登记链节点：登记表用它把这条操作挂进一条**不需要任何堆分配**的侵入式双向链表。
    // 只有登记表读写这两个指针（挂链、摘链都在它的锁内完成）；未登记时恒为 nullptr。
    // 正是这两个字段让"登记"这一步不再有容量上限、也不再有分配失败档 —— 见 Registry 注释。
    PendingOperation* regPrev_ = nullptr;
    PendingOperation* regNext_ = nullptr;
};

// ---------------------------------------------------------------------------
// 进程级登记表：排干宽限内进不了终态的操作住在这里。
// 记录只在确认终态后才删除。登记表本身**故意永不析构**：函数内静态对象会在进程正常退出时
// 跑析构，把仍未确认终态的记录一并销毁 —— 那等于关掉内核可能还在等的事件、释放内核可能
// 还在写的 OVERLAPPED 与缓冲区，正是本模块要防的缺陷（光改注释拦不住 CRT 的收尾次序）。
// 换成堆上这份"进程生命周期对象"之后，退出路径上没有任何代码再去碰这些资源：未确认终态
// 的那份随进程由系统统一回收，已确认终态的记录照旧在收尾排干与成功路径里即时回收，
// 不会把每条操作都变成积压。
//
// 登记模型（本轮修复的要点）：登记**不再需要任何堆分配**。每条 PendingOperation 对象自身
// 就带着登记链的前后指针，登记表只是把这条操作挂进一条侵入式双向链表（O(1)，见 Add）。
// 于是"登记"这一步没有容量上限、也没有分配失败档 —— 过去那份"8 个全局兜底槽位 + 那张表
// 分配失败"的双重降级被整体移除：那条路径会在槽位占满且分配失败时把第 9 条未决操作静默
// 丢弃，让它带着 unresolved=true 消失在 HasPending/Drain 之外 —— 查询随后谎报"无未决"，
// 收尾据此去关一个仍有飞行 I/O 的句柄（文档不保证的行为）。现在每个已发起且未确认终态的
// 操作，其事件、OVERLAPPED、缓冲区与句柄关闭责任都不可能失联：只要它没进终态，就必然在
// 这条链上，查得到、排得干。
// 保留的"有界"含义是"只装未进终态的操作"：确认终态即刻摘链并析构（事件恰好关一次），
// 日常使用不积压，链长自然被"真正未决的操作数"界住，而不是被一个拍出来的常数界住。
// 登记表本身第一次被取用时才堆分配（那份分配是为了让它不被析构），那一次可能抛出；
// 因此取用点仍必须建在"手上还没有任何内核可见资源"的时候（见 TryRegistry 与
// RunOverlappedOp 开头）—— 拿不到登记表就一次 I/O 都不发起地失败。
// ---------------------------------------------------------------------------
// SRWLOCK 的独占锁 RAII：Acquire/ReleaseSRWLockExclusive 不抛异常，因此登记表内的所有
// 临界区都是 noexcept 的 —— 交接点建在调用方已经松手之后，绝不允许异常把一条已发起的操作
// 变成没人认领的孤儿。
class SrwExclusive {
public:
    explicit SrwExclusive(SRWLOCK& lock) noexcept : lock_(&lock) {
        AcquireSRWLockExclusive(lock_);
    }
    ~SrwExclusive() { ReleaseSRWLockExclusive(lock_); }
    SrwExclusive(const SrwExclusive&) = delete;
    SrwExclusive& operator=(const SrwExclusive&) = delete;

private:
    SRWLOCK* lock_;
};

class Registry {
public:
    static Registry& Get() {
        static Registry* const r = new Registry();   // 故意不 delete，理由见类注释
        return *r;
    }

    // 接管一份还没确认终态的资源，所有权在这里绝不会遗失、也绝不会顺手释放。
    // 挂链是 O(1) 且不分配，所以这里没有旧实现那种"表分配失败 + 兜底槽位占满"的弃管档：
    // 只要控制权走到这里，这条操作就一定在链上，HasPending 查得到、Drain 认领得了。
    void Add(PendingOperation* op) noexcept {
        const SrwExclusive lock(lock_);
        op->regPrev_ = tail_;
        op->regNext_ = nullptr;
        if (tail_ != nullptr) {
            tail_->regNext_ = op;
        } else {
            head_ = op;
        }
        tail_ = op;
    }

    bool HasPending(HANDLE hFile) noexcept {
        const SrwExclusive lock(lock_);
        for (PendingOperation* op = head_; op != nullptr; op = op->regNext_) {
            if (op->file() == hFile) return true;
        }
        return false;
    }

    // 返回该句柄上仍未进终态的操作数；全清空且句柄已被收养时，顺带关掉句柄。
    // 不标 noexcept：TrySettle 会调到可注入后端，理论上可能抛出；公共入口 DrainPendingOps
    // 把它兜住并回保守值（至少还剩一条）。除了那次外部调用，函数体只做不抛的链表操作。
    DWORD Drain(HANDLE hFile, DWORD waitMs) {
        const SrwExclusive lock(lock_);
        // 先记下这条句柄用的是哪个后端：记录清完之后就问不到了。被收养的句柄要由
        // 同一个后端关闭（生产即 CloseHandle，测试以计数核对"恰好一次"）。
        IoBackend* be = nullptr;
        for (PendingOperation* op = head_; op != nullptr; op = op->regNext_) {
            if (op->file() == hFile) {
                be = &op->backend();
                break;
            }
        }
        DWORD left = 0;
        PendingOperation* op = head_;
        while (op != nullptr) {
            PendingOperation* const next = op->regNext_;   // 摘链会改 op->regNext_，先取下一跳
            if (op->file() == hFile) {
                if (op->TrySettle(waitMs)) {
                    Unlink(op);
                    delete op;   // 析构只发生在确认终态之后：事件、OVERLAPPED、缓冲区
                } else {
                    ++left;
                }
            }
            op = next;
        }
        if (left == 0) {
            const bool wasAdopted = RemoveAdopted(hFile);
            // 问不到归属后端（这条句柄上一条记录都没有）时宁可什么都不关，绝不拿别的句柄的
            // 后端去关一个自己认不出的值。
            if (wasAdopted && be != nullptr) be->CloseTargetHandle(hFile);
        }
        return left;
    }

    // 记下"这条句柄由登记表负责关闭"。收养记录住在一段免分配的定长数组里，不进堆。
    // 回 false 只发生在同时收养的句柄数超过这段定长容量时 —— 本函数绝不抛出：它的调用点
    // 在交易收尾链路上，而收尾还会从 Transaction 的析构里再走一遍，异常从默认 noexcept
    // 的析构逸出就是终止进程。
    // 回 false 之后这条句柄谁也管不着：Drain 全数终态时不会去关它。调用方据此仍然要放弃
    // 自己的关闭责任 —— 带着未决 I/O 关句柄始终是文档不保证的行为，宁可遗留一个句柄值。
    // 这是本模块唯一保留的保守降级，且它是**可观测的返回值**，不是静默丢弃：调用方拿得到
    // "没记下"这个事实，也不会因此谎报"已经排干"。
    bool Adopt(HANDLE hFile) noexcept {
        const SrwExclusive lock(lock_);
        for (size_t i = 0; i < adoptedCount_; ++i) {
            if (adopted_[i] == hFile) return true;   // 重复收养同一个句柄是幂等的
        }
        if (adoptedCount_ >= kMaxAdopted) return false;
        adopted_[adoptedCount_] = hFile;
        ++adoptedCount_;
        return true;
    }

private:
    // 同时挂起的"待关闭句柄"上限。收养只发生在"排干后还剩记录"的收尾路径，正常请求下一两条。
    // 满了就回 false（可观测），调用方照旧放弃关闭责任。
    static constexpr size_t kMaxAdopted = 32;

    Registry() = default;

    // 前提：已持锁。把 op 从登记链上摘下来（不改 op 自身以外的任何东西）。
    void Unlink(PendingOperation* op) noexcept {
        if (op->regPrev_ != nullptr) {
            op->regPrev_->regNext_ = op->regNext_;
        } else {
            head_ = op->regNext_;
        }
        if (op->regNext_ != nullptr) {
            op->regNext_->regPrev_ = op->regPrev_;
        } else {
            tail_ = op->regPrev_;
        }
        op->regPrev_ = nullptr;
        op->regNext_ = nullptr;
    }

    // 前提：已持锁。
    bool RemoveAdopted(HANDLE hFile) noexcept {
        for (size_t i = 0; i < adoptedCount_; ++i) {
            if (adopted_[i] == hFile) {
                adopted_[i] = adopted_[adoptedCount_ - 1];
                --adoptedCount_;
                return true;
            }
        }
        return false;
    }

    SRWLOCK lock_ = SRWLOCK_INIT;
    // 裸指针 + 不 delete：未确认终态的对象住在"永不析构"的登记表里，退出时没人碰这条链，
    // 里面未确认终态的资源就由系统随进程统一回收。已确认终态的条目在 Drain/成功路径里即时删除。
    PendingOperation* head_ = nullptr;
    PendingOperation* tail_ = nullptr;
    HANDLE adopted_[kMaxAdopted] = {};
    size_t adoptedCount_ = 0;
};

// 免抛地取得登记表。登记表第一次被取用时才堆分配（那份分配的存在是为了让它不被析构，
// 见类注释），那一次可能抛出，所以这里兜住并如实回 nullptr。
// nullptr 的含义是确定的："这份登记表从来没存在过" —— 而登记任何操作都要求它先存在，
// 于是 nullptr 就等于"此刻没有任何已移交的记录"。下面三个公共入口据此给出的保守值
// 是有依据的，不是猜的。分配失败不会留下半个静态对象：初始化被异常打断时守卫变量不置位，
// 下一次取用会重新尝试。
Registry* TryRegistry() noexcept {
    try {
        return &Registry::Get();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

StartResult StartOf(const BOOL r) {
    if (r) return {StartState::kSyncDone, 0};
    const DWORD gle = GetLastError();
    if (gle == ERROR_IO_PENDING) return {StartState::kPending, gle};
    return {StartState::kSyncFailed, gle};
}

StartResult ConnectStartOf(const BOOL r) {
    if (r) return {StartState::kSyncDone, 0};
    const DWORD gle = GetLastError();
    if (gle == ERROR_IO_PENDING) return {StartState::kPending, gle};
    // 官方文档（ConnectNamedPipe，Return value）：客户端在 CreateNamedPipe 与
    // ConnectNamedPipe 之间的间隙里连上时，函数返回 0 且 GetLastError 为
    // ERROR_PIPE_CONNECTED —— "there is a good connection between client and server"。
    // 这是一次成功而不是挂起操作：没有 IRP 在飞，事件也不会由系统置起。
    // 其余错误（ERROR_NO_DATA、ERROR_PIPE_LISTENING、拒绝访问……）照实进 kSyncFailed，
    // 这一档不是"可以忽略所有错误"的通行证。
    if (gle == ERROR_PIPE_CONNECTED) return {StartState::kAlreadyConnected, 0};
    return {StartState::kSyncFailed, gle};
}

IoStep RunOverlappedOp(IoBackend& backend, HANDLE hFile, uint32_t bufCap, DWORD waitMs,
                       DWORD cancelGraceMs, const IssueFn& issue, std::vector<uint8_t>* outBuf) {
    // 登记表必须在任何东西可能飞行之前先拿到手：它第一次被取用时才堆分配，而那一次分配会抛。
    // 留到下面的交接点才第一次取用就成了赌博 —— 那时调用方已经松手，分配一失败这条操作就再也
    // 查不到，"登记表里没有未决操作，可以放心关句柄"的判断随之失去依据。在这里取就没有这个
    // 包袱：此刻连事件都还没建，拿不到登记表只能一次 I/O 都不发起，按机制故障如实上报，
    // 谈不上提前释放什么。往后交接点上的 Add 已是免分配、免抛路径。
    Registry* const reg = TryRegistry();
    if (reg == nullptr) {
        IoStep noRegistry;
        noRegistry.gle = ERROR_NOT_ENOUGH_MEMORY;
        return noRegistry;
    }

    // 本地这份所有权由 unique_ptr 兜住，异常路径也不会把资源丢掉。能交给它析构的前提是
    // "这条操作已确认终态"：Execute 的分支穷尽保证了没确认终态的那条一定带着 registered()
    // = true 回来，走下面的移交；done = true 的那条必然已确认终态（复制回调用方所需的
    // 内存分配失败时也还在这一档里），所以这里既不会早放、也不会漏放。
    // 构造这一步要分配操作对象与其块缓冲：分不出来时**一次 I/O 都不发起**地按机制故障上报
    // （capacity 不足在发起前明确失败，而不是发起后丢失登记）。
    std::unique_ptr<PendingOperation> owned;
    try {
        owned.reset(new PendingOperation(backend, hFile, bufCap));
    } catch (...) {
        IoStep noMemory;
        noMemory.gle = ERROR_NOT_ENOUGH_MEMORY;
        return noMemory;
    }
    PendingOperation* op = owned.get();
    const IoStep step = op->Execute(waitMs, cancelGraceMs, issue);
    IoStep result = step;
    if (result.done && result.transferred > 0 && outBuf) {
        outBuf->assign(op->buffer().begin(),
                       op->buffer().begin() + static_cast<long long>(result.transferred));
    }
    if (op->registered()) {
        // 没确认终态：整份（事件 + OVERLAPPED + 缓冲区）交给登记表继续看管。
        // 先松手再接管，任何一步抛出都不会出现两个所有者同时想释放它；反过来先接管后松手
        // 会让 unique_ptr 在抛出时析构掉一个登记表还认得的对象。这一交接不会抛出：登记表
        // 已在函数开头拿到，而 Add 只是把操作对象挂进一条免分配的侵入式链表（见 Add）。
        owned.release();
        reg->Add(op);
    }
    return result;
}

// 这三个入口一律不抛出：调用它们的是交易收尾链路（Transaction::Finish，还会从析构里再走一遍），
// 异常从默认 noexcept 的析构函数逸出就是终止进程。登记表内部的临界区用 SRWLOCK，本身就不抛。
bool HasPendingOps(HANDLE hFile) noexcept {
    // 登记表取不到（它第一次被取用时堆分配失败）→ false：登记任何操作都要求它先存在，所以
    // "它从没存在过"确实推出"此刻没有任何已移交的记录"。登记表存在时查询不会失败：
    // 挂链/查链都在不抛的 SRWLOCK 临界区里，不存在“问不到状态”的中间档。
    Registry* const reg = TryRegistry();
    if (reg == nullptr) return false;
    return reg->HasPending(hFile);
}

DWORD DrainPendingOps(HANDLE hFile, DWORD waitMs) noexcept {
    // 0 与 HasPendingOps 的 false 同一条依据：没有登记表就没有已移交的记录，也就没有未决操作。
    // 登记表在却排不动（后端调用抛出，属于病理情形）时报"至少还剩一条"，让调用方走收养
    // 遗留而不是关句柄 —— 宁可保守，也不把未知终态当成安全。
    Registry* const reg = TryRegistry();
    if (reg == nullptr) return 0;
    try {
        return reg->Drain(hFile, waitMs);
    } catch (...) {
        return 1;
    }
}

bool AdoptHandle(HANDLE hFile) noexcept {
    // 返回 false 只有一种情形：同时收养的句柄数超过了免分配定长容量。此时调用方仍然要
    // 放弃对这个句柄的关闭责任，到它依旧是文档不保证的行为，宁可让句柄随进程遗留。
    Registry* const reg = TryRegistry();
    if (reg == nullptr) return false;
    return reg->Adopt(hFile);
}

// ---------------------------------------------------------------------------
// 真实 Win32 后端
// ---------------------------------------------------------------------------
namespace {

class Win32IoBackend final : public IoBackend {
public:
    HANDLE OpenEvent(DWORD* gle) override {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev && gle) *gle = GetLastError();
        return ev;
    }
    void CloseEvent(HANDLE ev) override { CloseHandle(ev); }
    DWORD WaitFor(HANDLE ev, DWORD ms, DWORD* gle) override {
        const DWORD r = WaitForSingleObject(ev, ms);
        if (r == WAIT_FAILED && gle) *gle = GetLastError();
        return r;
    }
    bool Cancel(HANDLE hFile, OVERLAPPED* ov, DWORD* gle) override {
        const BOOL ok = CancelIoEx(hFile, ov);
        if (!ok && gle) *gle = GetLastError();
        return ok != FALSE;
    }
    bool Fetch(HANDLE hFile, OVERLAPPED* ov, size_t* transferred, DWORD* gle) override {
        DWORD bytes = 0;
        const BOOL ok = GetOverlappedResult(hFile, ov, &bytes, FALSE);
        if (!ok && gle) *gle = GetLastError();
        if (transferred) *transferred = bytes;
        return ok != FALSE;
    }
    void CloseTargetHandle(HANDLE hFile) override { CloseHandle(hFile); }
};

}  // namespace

IoBackend& Win32Backend() {
    static Win32IoBackend be;
    return be;
}

}  // namespace workerio
}  // namespace ecapture
