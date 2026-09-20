#pragma once
// 一次重叠（异步）管道 I/O 的生命周期状态机。
//
// 这里解决的是一个 Win32 契约问题：CancelIoEx **只请求**取消，它返回时操作可能仍在飞行中。
// 官方文档明确：取消成功后应用必须等到操作真正完成（用 GetOverlappedResult 观察）之前，
// 不得释放或复用与之关联的 OVERLAPPED 结构和缓冲区；被取消的操作有三种终态
// （正常完成竞争获胜 / ERROR_OPERATION_ABORTED / 其他错误），必须检查完成状态而不能猜。
// CancelIoEx 返回 FALSE 且 GetLastError() 为 ERROR_NOT_FOUND 时，含义只是"没找到可取消的
// 请求"，它不是完成证据：操作可能刚好完成（事件此时应已置起），也可能这个 OVERLAPPED 根本
// 不曾入队。两种都要走到终态确认才算数 —— 先用一次零等待探事件，探不到就照"还在飞行"处理，
// 不许把"取消说没这回事"当成"可以放心释放"。
//
// 因此本模块把一次操作的全部内核可见资源（事件、OVERLAPPED、以及内核唯一会触碰的缓冲区副本）
// 收进一个 PendingOperation：
//   * 进入终态 -> 就地释放，且事件恰好关闭一次；
//   * 排干宽限内没等到终态 -> 整份资源（含缓冲区副本，不是原始指针）移交进程级登记表，
//     由后续 DrainPendingOps / 交易收尾继续认领。登记表只在确认终态后才删除记录；登记表
//     自身是"故意永不析构"的进程生命周期对象，所以进程正常退出时也不会去销毁仍未确认
//     终态的记录 —— 那一份连同它的事件、OVERLAPPED 与缓冲区由系统随进程统一回收。
//     这份对象第一次被取用时才堆分配，所以取用点前移到发起任何 I/O 之前（见 RunOverlappedOp）：
//     移交点上不再有分配动作，"调用方已松手却登记不上"这条失联路径因此不存在。
//   * 等到完成事件却仍被报为未完成（ERROR_IO_INCOMPLETE / ERROR_IO_PENDING）同样不算终态。
// 调用方的栈与堆内存因此永远不与未决的 IRP 共用：即使超时走人，内核后续只写到登记表里的副本。
//
// IoBackend 是状态机与真实 Win32 之间唯一的接缝：离线判据（tests\worker_io_state.cpp）注入
// 脚本化的假后端来控制事件顺序；发布版只用 Win32Backend()，没有任何测试开关。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {
namespace workerio {

// 发起阶段的结局。与 ReadFile/WriteFile/ConnectNamedPipe 的 BOOL + GetLastError 约定一一对应。
enum class StartState {
    kSyncDone,          // 同步完成（重叠调用返回非 0；事件此时已置起，等待会立即返回）
    kAlreadyConnected,  // 已连接（ConnectNamedPipe 的 ERROR_PIPE_CONNECTED）：客户端在调用之前
                        // 就连上了 —— 这是一次成功，但**没有发起任何挂起 I/O**，事件不会由系统
                        // 置起（官方文档：even though the function returns zero, there is a good
                        // connection）。所以它直接进成功终态，绝不能按"等待内核通知的异步请求"
                        // 去等那个永不置起的事件 —— 那正是抢先连接被误报成超时的来源。
    kPending,           // ERROR_IO_PENDING：操作进入飞行中
    kSyncFailed,        // 其他错误：没有未决操作，可以直接走终态
};

struct StartResult {
    StartState state = StartState::kSyncFailed;
    DWORD gle = 0;   // 仅 kSyncFailed 有意义
};

// ReadFile/WriteFile 的 BOOL + GetLastError -> 发起阶段结局。重叠调用返回非 0 是"同步完成"，
// 事件已由系统置起；ERROR_IO_PENDING 才是飞行中。这两个调用没有"已连接"那一档。
StartResult StartOf(BOOL r);

// ConnectNamedPipe 专用：在 StartOf 的三档之外，把 ERROR_PIPE_CONNECTED 单独判成
// kAlreadyConnected。只有这一种成功是"没有挂起操作"的；其余错误一律照实进 kSyncFailed，
// 不许拿 ERROR_PIPE_CONNECTED 当免检通行证。
StartResult ConnectStartOf(BOOL r);

// 发起一次重叠请求。buf/cap 是 PendingOperation 拥有的缓冲区：内核只会读写这块内存，
// 绝不直接触碰调用方的内存；读取到的数据在确认终态之后由状态机复制回调用方。
using IssueFn = std::function<StartResult(OVERLAPPED& ov, uint8_t* buf, uint32_t cap)>;

// 一次操作的对外结局。字段含义与 Worker.cpp 原来的 Step 一致，另加 unresolved：
// unresolved = 排干宽限内没等到终态，资源仍在登记表里活着（此时 done/timedOut 描述的是等待结果，
// 不代表内核已经放手）。
struct IoStep {
    bool done = false;
    bool timedOut = false;
    bool unresolved = false;
    DWORD gle = 0;
    size_t transferred = 0;
};

// 状态机与操作系统之间的适配层。生产实现见 Win32Backend()；测试注入假实现。
class IoBackend {
public:
    virtual ~IoBackend() = default;
    // 失败时返回 nullptr 并写 *gle。
    virtual HANDLE OpenEvent(DWORD* gle) = 0;
    virtual void CloseEvent(HANDLE ev) = 0;
    // 返回 WAIT_*；WAIT_FAILED 时写 *gle。
    virtual DWORD WaitFor(HANDLE ev, DWORD ms, DWORD* gle) = 0;
    // CancelIoEx：false 时写 *gle（ERROR_NOT_FOUND 有专门含义，见文件头）。
    virtual bool Cancel(HANDLE hFile, OVERLAPPED* ov, DWORD* gle) = 0;
    // GetOverlappedResult(bWait = FALSE)：false 时写 *gle。
    virtual bool Fetch(HANDLE hFile, OVERLAPPED* ov, size_t* transferred, DWORD* gle) = 0;
    // 登记表收养目标句柄后，在确认该句柄上所有操作都进入终态时调用。
    virtual void CloseTargetHandle(HANDLE hFile) = 0;
};

// 发布版使用的真实实现。
IoBackend& Win32Backend();

// 一次完整的重叠操作：发起 -> 按预算等待 -> 未到终态则请求取消并在宽限内排干 ->
// 仍未终态则整份移交登记表并标记 unresolved。无论走哪条分支，返回时都保证：
// 事件、OVERLAPPED、缓冲区与"内核是否还在访问它们"的状态严格一致 —— 绝不提前放手。
// backend 参数就是那条可注入接缝；waitMs 用 INFINITE 表示"不限"（沿用 Deadline 的约定）。
// outBuf 非空时，成功读取的数据在完成确认之后复制进去（写操作传 nullptr）。
// 登记表在这一步发起任何 I/O 之前就取好（它第一次被取用时才堆分配，那一次可能失败）：拿不到
// 登记表就一次都不发起，直接以 ERROR_NOT_ENOUGH_MEMORY 的非终态结局返回。留到移交点上才第一次
// 取用，等于在调用方已经松手之后赌那次分配 —— 赌输这条操作就再也查不到了。
IoStep RunOverlappedOp(IoBackend& backend, HANDLE hFile, uint32_t bufCap, DWORD waitMs,
                       DWORD cancelGraceMs, const IssueFn& issue, std::vector<uint8_t>* outBuf);

// 非阻塞探测：登记表里是否还有挂在这个句柄上、未进入终态的操作。
// 关闭该句柄之前必须先问这个 —— 有未决操作时关句柄属于文档不保证的行为。
// 本层的三个入口都不抛出，因为调用它们的是交易收尾链路（Transaction::Finish 还会从析构里
// 再走一遍），异常从默认 noexcept 的析构逸出就是终止进程。问不到状态时按保守值回答：
// 登记表从未建立（它第一次被取用时堆分配失败）→ 确实没有任何已移交的记录，报"没有未决"；
// 登记表在却问不到（内部加锁失败）→ 报"还有未决"，绝不含糊成"可以放心关句柄"。
bool HasPendingOps(HANDLE hFile) noexcept;

// 对该句柄上所有未决操作做一轮有界排干，返回仍进不了终态的条数。
// waitMs 是每条操作各自的收尾等待上限（0 = 只探一次，不等）。
// 排不动时（内部加锁失败）报"至少还剩一条"，让调用方走收养遗留而不是关句柄。
DWORD DrainPendingOps(HANDLE hFile, DWORD waitMs) noexcept;

// 把 hFile 的关闭责任交给登记表：所有挂在其上的操作进入终态后由登记表关掉它。
// 调用方必须同时放弃自己对这个句柄的关闭责任（句柄值不能重复注册两次收养）。
// 返回 false = 这条收养没能记下（登记表没建立起来，或那个记录节点分配不出来）：登记表此后
// 不会碰这个句柄，而调用方仍然要放弃关闭责任 —— 带着未决 I/O 关它依旧是文档不保证的行为，
// 宁可让这个句柄随进程遗留，也不许在析构链路上关出未定义行为。
bool AdoptHandle(HANDLE hFile) noexcept;

}  // namespace workerio
}  // namespace ecapture
