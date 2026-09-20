#include "WorkerIo.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>

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
        // 只有"已确认终态"的对象才允许走到这里：登记表只在 TrySettle 返回 true 之后删除记录，
        // RunOverlappedOp 只在终态分支释放自己那份。未确认终态的对象由永不析构的登记表
        // （或它那段免分配的兜底槽位）持到进程结束，交给系统统一回收 —— 见 Registry 的类注释。
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
};

// ---------------------------------------------------------------------------
// 进程级登记表：排干宽限内进不了终态的操作住在这里。
// 记录只在确认终态后才删除。登记表本身**故意永不析构**：函数内静态对象会在进程正常退出时
// 跑析构，把仍未确认终态的记录一并销毁 —— 那等于关掉内核可能还在等的事件、释放内核可能
// 还在写的 OVERLAPPED 与缓冲区，正是本模块要防的缺陷（光改注释拦不住 CRT 的收尾次序）。
// 换成堆上这份"进程生命周期对象"之后，退出路径上没有任何代码再去碰这些资源：未确认终态
// 的那份随进程由系统统一回收，已确认终态的记录照旧在收尾排干与成功路径里即时回收，
// 不会把每条操作都变成积压。规模有界：每条记录至多一份块缓冲加一个事件句柄，
// 且这里只装"还没确认终态"的操作。
// 代价是这份对象**第一次被取用时才堆分配**，那一次分配可能抛出。因此取用点必须建在
// "手上还没有任何内核可见资源"的时候（见 TryRegistry 与 RunOverlappedOp 开头），
// 绝不能留到交接点上 —— 那时调用方已经松手，分配一失败这条操作就再也查不到了。
// ---------------------------------------------------------------------------
class Registry {
public:
    static Registry& Get() {
        static Registry* const r = new Registry();   // 故意不 delete，理由见类注释
        return *r;
    }

    // 接管一份还没确认终态的资源，所有权在这里绝不会遗失、也绝不会顺手释放：
    //   1) 优先登记进按句柄归组的表（正常路径，之后由 Drain 认领）；
    //   2) 那张表要堆分配，极端内存压力下可能装不下 —— 退化为占用一段免分配的固定槽位，
    //      照样保住"有人在管"：HasPending 查得到、Drain 认领得了、收养的句柄照样等它；
    //   3) 连槽位都占满时才故意弃管 —— 依然绝不释放，只是没人再替它确认终态，
    //      HasPending 也会漏报这一条。
    // 3) 的真实条件要看清容量归属：那 kRetainedCapacity 段槽位归**整个登记表共用**，
    // 不是每条句柄各一份 —— 别的句柄上没进终态的操作照样占着它们。所以走到 3) 要同时满足
    // "全进程已占满这几段槽位"与"此刻那张表又分配失败"，跟单条句柄积了多少条没有直接关系。
    // 这是内存耗尽下唯一不造成"内核往已释放内存写"的选择，如实记录在这里。
    // 本函数绝不抛出（加锁也兜在里面）：交接点建在调用方已经松手之后，异常一逸出就等于
    // 把这条操作变成没人认领的孤儿 —— 所以宁可原地弃管，也不许它穿到那一步之外。
    void Add(PendingOperation* op) noexcept {
        try {
            std::lock_guard<std::mutex> lk(m_);
            if (TryRegister(op)) return;
            for (PendingOperation*& slot : retained_) {
                if (slot == nullptr) {
                    slot = op;
                    return;
                }
            }
        } catch (...) {
            // 能到这里只剩加锁失败（TryRegister 自己把分配失败退回 false 了）。
            // 与槽位占满同一档处理：弃管，但绝不释放。
        }
    }

    bool HasPending(HANDLE hFile) {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = ops_.find(hFile);
        if (it != ops_.end() && !it->second.empty()) return true;
        return CountRetained(hFile) != 0;
    }

    // 返回该句柄上仍未进终态的操作数（表里的加上兜底槽位里的）；
    // 全清空且句柄已被收养时，顺带关掉句柄。
    DWORD Drain(HANDLE hFile, DWORD waitMs) {
        std::lock_guard<std::mutex> lk(m_);
        // 先记下这条句柄用的是哪个后端：记录清完之后就问不到了。被收养的句柄要由
        // 同一个后端关闭（生产即 CloseHandle，测试以计数核对"恰好一次"）。
        IoBackend* be = BackendFor(hFile);
        const auto it = ops_.find(hFile);
        if (it != ops_.end()) {
            auto& list = it->second;
            for (auto entry = list.begin(); entry != list.end();) {
                if ((*entry)->TrySettle(waitMs)) {
                    delete *entry;   // 析构只发生在确认终态之后：事件、OVERLAPPED、缓冲区
                    entry = list.erase(entry);
                } else {
                    ++entry;
                }
            }
            if (list.empty()) ops_.erase(it);
        }
        for (PendingOperation*& slot : retained_) {
            PendingOperation* op = slot;
            if (op == nullptr || op->file() != hFile) continue;
            if (op->TrySettle(waitMs)) {
                slot = nullptr;
                delete op;
            }
        }
        DWORD left = 0;
        const auto rest = ops_.find(hFile);
        if (rest != ops_.end()) left += static_cast<DWORD>(rest->second.size());
        left += static_cast<DWORD>(CountRetained(hFile));
        if (left == 0) {
            const bool wasAdopted = adopted_.erase(hFile) != 0;
            backendOf_.erase(hFile);
            if (wasAdopted && be != nullptr) be->CloseTargetHandle(hFile);
        }
        return left;
    }

    // 记下"这条句柄由登记表负责关闭"。那条记录本身要堆分配一个集合节点，分不出来（或加锁
    // 失败）就如实回 false —— 本函数绝不抛出：它的调用点在交易收尾链路上，而收尾还会从
    // Transaction 的析构里再走一遍，异常从默认 noexcept 的析构逸出就是终止进程。
    // 回 false 之后这条句柄谁也管不着：Drain 全数终态时不会去关它。调用方据此仍然要放弃
    // 自己的关闭责任 —— 带着未决 I/O 关句柄始终是文档不保证的行为，宁可遗留一个句柄值。
    bool Adopt(HANDLE hFile) noexcept {
        try {
            std::lock_guard<std::mutex> lk(m_);
            adopted_.insert(hFile);
            return true;
        } catch (...) {
            return false;
        }
    }

private:
    // 免分配兜底槽位的容量：登记表堆分配失败时仍要保住未决资源的所有权与可查性。
    // 这份容量是**全进程共用**的，不是每条句柄各一份：任何句柄上没进终态的操作都占同一批槽位。
    static constexpr size_t kRetainedCapacity = 8;

    Registry() = default;

    // 前提：已持锁。成功接管返回 true；所需的堆内存分配不出来时，把已经插进去的那条退回、
    // 返回 false —— 两种结果都不释放 op，也不留下"表里认得它但没人负责"的中间状态。
    bool TryRegister(PendingOperation* op) {
        const HANDLE h = op->file();
        try {
            ops_[h].push_back(op);
            backendOf_[h] = &op->backend();
            return true;
        } catch (...) {
            const auto it = ops_.find(h);
            if (it != ops_.end()) {
                if (!it->second.empty() && it->second.back() == op) it->second.pop_back();
                if (it->second.empty()) ops_.erase(it);
            }
            return false;
        }
    }

    // 前提：已持锁。
    size_t CountRetained(HANDLE hFile) const {
        size_t n = 0;
        for (PendingOperation* op : retained_) {
            if (op != nullptr && op->file() == hFile) ++n;
        }
        return n;
    }

    // 前提：已持锁。表里没有（例如这条住在兜底槽位）时，向还没清空的记录本身问后端。
    IoBackend* BackendFor(HANDLE hFile) {
        const auto be = backendOf_.find(hFile);
        if (be != backendOf_.end()) return be->second;
        for (PendingOperation* op : retained_) {
            if (op != nullptr && op->file() == hFile) return &op->backend();
        }
        return nullptr;
    }

    std::mutex m_;
    std::map<HANDLE, std::vector<PendingOperation*>> ops_;
    std::map<HANDLE, IoBackend*> backendOf_;
    std::set<HANDLE> adopted_;
    // 裸指针 + 不 new 不 delete：对象住在"永不析构"的登记表里，退出时没人碰这些槽位，
    // 里面未确认终态的资源就照上面的方案由系统随进程回收。
    PendingOperation* retained_[kRetainedCapacity] = {};
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
    // 谈不上提前释放什么。往后交接点上的 Add 已是免抛路径（登记表存在 + 自己兜尽分配失败）。
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
    std::unique_ptr<PendingOperation> owned(new PendingOperation(backend, hFile, bufCap));
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
        // 已在函数开头拿到，Add 自己把分配失败退回免分配槽位、把加锁失败也兜住（见 Add）。
        owned.release();
        reg->Add(op);
    }
    return result;
}

// 这三个入口一律不抛出：调用它们的是交易收尾链路（Transaction::Finish，还会从析构里再走一遍），
// 异常从默认 noexcept 的析构函数逸出就是终止进程。各自的保守取值理由写在旁边。
bool HasPendingOps(HANDLE hFile) noexcept {
    // 登记表取不到（它第一次被取用时堆分配失败）→ false：登记任何操作都要求它先存在，所以
    // "它从没存在过"确实推出"此刻没有任何已移交的记录"。登记表在、却问不到状态（加锁失败）
    // → true：宁可当成还有未决，也不让调用方在没确认终态的句柄上动手。
    Registry* const reg = TryRegistry();
    if (reg == nullptr) return false;
    try {
        return reg->HasPending(hFile);
    } catch (...) {
        return true;
    }
}

DWORD DrainPendingOps(HANDLE hFile, DWORD waitMs) noexcept {
    // 0 与 HasPendingOps 的 false 同一条依据：没有登记表就没有已移交的记录，也就没有未决操作。
    // 反过来，登记表在却排不动（加锁失败）时报"至少还剩一条"，让调用方走收养遗留而不是关句柄。
    Registry* const reg = TryRegistry();
    if (reg == nullptr) return 0;
    try {
        return reg->Drain(hFile, waitMs);
    } catch (...) {
        return 1;
    }
}

bool AdoptHandle(HANDLE hFile) noexcept {
    // 返回 false = 这条收养没能记下（登记表不存在，或那个集合节点分配不出来）：调用方仍然要
    // 放弃对这个句柄的关闭责任，此时关它依旧是文档不保证的行为，宁可让句柄随进程遗留。
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
