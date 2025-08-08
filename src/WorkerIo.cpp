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
        CloseEventOnce();
        // buf_ 与 ov_ 随对象一起析构：能走到这里说明操作已终态（登记表只在终态后删除记录）。
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
        if (wait == WAIT_OBJECT_0) {
            FetchInto(step);
            MarkTerminal();
            return step;
        }
        if (wait == WAIT_TIMEOUT) {
            step.timedOut = true;
        } else {
            step.gle = gle;   // WAIT_FAILED：这是本条操作的最初故障，后面不许覆盖
        }

        // 到这里操作必定未终态。请求取消，再确认终态。CancelIoEx 的三种回话：
        //   * TRUE：取消已排入，操作将以 ABORTED / 竞争获胜 / 其他错误收尾，事件必定置起
        //     —— 在宽限内等它；等不到就把整份资源移交登记表，绝不空手放手。
        //   * FALSE + ERROR_NOT_FOUND：没找到可取消的请求。两种可能 —— 操作刚好完成
        //     （事件应已置起），或这个 OVERLAPPED 根本不曾真正入队（历史缺陷：调用方把
        //     ERROR_PIPE_CONNECTED 这种"早已连上"当成发起；现在 ConnectStartOf 已把四类
        //     分开，此分支留作防御）。各用一次 0 等待探一下事件：置起→取结果收尾；
        //     没置起→内核不引用任何东西，同样按终态处理。不许把"取消没成"直接当"结束了"，
        //     也不许让幽灵记录进登记表白白收养句柄。
        //   * 其他 FALSE：取消没被受理，操作按还在飞行处理 —— 走宽限排干那一条。
        DWORD cancelGle = 0;
        const bool cancelRequested = backend_.Cancel(hFile_, &ov_, &cancelGle);
        if (!cancelRequested && cancelGle == ERROR_NOT_FOUND) {
            DrainOnce(0);   // 结果不进对外结局：预算在那次超时/失败上已经定性
            MarkTerminal();
            return step;
        }
        if (DrainOnce(graceMs)) {
            MarkTerminal();
            return step;
        }
        registered_ = true;
        step.unresolved = true;
        return step;
    }

    // 登记表的收尾通道：确认终态则关闭事件并返回 true（记录随即可删）。
    // 取回的结果（多半是 ERROR_OPERATION_ABORTED，竞争获胜时是正常完成）都不再改写
    // 对外结局 —— 预算在那次超时就已经花掉了，这里只负责把资源安全放手。
    bool TrySettle(DWORD waitMs) {
        if (terminal_) return true;
        DWORD gle = 0;
        if (backend_.WaitFor(event_, waitMs, &gle) != WAIT_OBJECT_0) return false;
        size_t transferred = 0;
        backend_.Fetch(hFile_, &ov_, &transferred, &gle);
        MarkTerminal();
        return true;
    }

private:
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

    // 事件已置起之后取结果。只用于"取消之前"那条首次完成路径：
    // 超时/取消之后的确认走 DrainOnce + TrySettle，那里的取回结果不再改写对外结局
    // （预算已尽，不把取消边缘上回来的半截数据冒充成功），但只要确认了终态就允许安全释放。
    void FetchInto(IoStep& step) {
        size_t transferred = 0;
        DWORD gle = 0;
        const bool ok = backend_.Fetch(hFile_, &ov_, &transferred, &gle);
        if (ok) {
            step.done = true;
            // 防御性钳制：真实 API 承诺不超过给定缓冲；假后端或未来的调用方给错时，
            // 宁可少报也不能让 transferred 指向缓冲区之外的内存。
            step.transferred = std::min(transferred, buf_.size());
        } else if (step.gle == 0) {
            step.gle = gle;
        }
    }

    bool DrainOnce(DWORD graceMs) {
        DWORD gle = 0;
        if (backend_.WaitFor(event_, graceMs, &gle) != WAIT_OBJECT_0) return false;
        size_t transferred = 0;
        backend_.Fetch(hFile_, &ov_, &transferred, &gle);
        return true;
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
// 记录只在确认终态后才删除；进程退出时仍进不了终态的记录**故意不删** ——
// 那是内核仍在引用的 OVERLAPPED/事件/缓冲，提前释放就是本次要修的缺陷本身。
// 每条记录上限一份 64 KiB 块缓冲加一个事件句柄，进程退出时由系统统一回收，规模有界。
// ---------------------------------------------------------------------------
class Registry {
public:
    static Registry& Get() {
        static Registry r;
        return r;
    }

    void Add(PendingOperation* op) {
        std::lock_guard<std::mutex> lk(m_);
        ops_[op->file()].push_back(std::unique_ptr<PendingOperation>(op));
        // 记下这条记录用的后端：被收养句柄最终清空时，要经由同一个后端关闭句柄
        // （生产即 CloseHandle，测试以计数核对"恰好一次"）。同一句柄只可能挂一个后端。
        backendOf_[op->file()] = &op->backend();
    }

    bool HasPending(HANDLE hFile) {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = ops_.find(hFile);
        return it != ops_.end() && !it->second.empty();
    }

    // 返回该句柄上仍未进终态的操作数；全清空且句柄已被收养时，顺带关掉句柄。
    DWORD Drain(HANDLE hFile, DWORD waitMs) {
        std::lock_guard<std::mutex> lk(m_);
        const auto it = ops_.find(hFile);
        if (it == ops_.end()) return 0;
        auto& list = it->second;
        for (auto entry = list.begin(); entry != list.end();) {
            if ((*entry)->TrySettle(waitMs)) {
                entry = list.erase(entry);   // 析构只发生在确认终态之后
            } else {
                ++entry;
            }
        }
        if (list.empty()) {
            const bool wasAdopted = adopted_.erase(hFile) != 0;
            auto be = backendOf_.extract(hFile);
            ops_.erase(it);
            if (wasAdopted && be) be.mapped()->CloseTargetHandle(hFile);
            return 0;
        }
        return static_cast<DWORD>(list.size());
    }

    void Adopt(HANDLE hFile) {
        std::lock_guard<std::mutex> lk(m_);
        adopted_.insert(hFile);
    }

private:
    Registry() = default;

    std::mutex m_;
    std::map<HANDLE, std::vector<std::unique_ptr<PendingOperation>>> ops_;
    std::map<HANDLE, IoBackend*> backendOf_;
    std::set<HANDLE> adopted_;
};

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
    PendingOperation* op = new PendingOperation(backend, hFile, bufCap);
    const IoStep step = op->Execute(waitMs, cancelGraceMs, issue);
    IoStep result = step;
    if (result.done && result.transferred > 0 && outBuf) {
        outBuf->assign(op->buffer().begin(),
                       op->buffer().begin() + static_cast<long long>(result.transferred));
    }
    if (op->registered()) {
        Registry::Get().Add(op);   // 后端与句柄的登记在 Add 内完成
    } else {
        delete op;   // 终态：事件已在 MarkTerminal 关过，这里只回收内存
    }
    return result;
}

bool HasPendingOps(HANDLE hFile) { return Registry::Get().HasPending(hFile); }

DWORD DrainPendingOps(HANDLE hFile, DWORD waitMs) { return Registry::Get().Drain(hFile, waitMs); }

void AdoptHandle(HANDLE hFile) { Registry::Get().Adopt(hFile); }

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
