#pragma once
// 平台服务：隔离执行（辅助进程）的**直接进程创建、可信 IPC 端点、带期限的通信、
// 等待与终止**。声明是平台无关的；实现分两侧：
//   * src/platform/windows/WinHostProcess.cpp —— 单实例命名管道 + CreateProcessW(挂起→进作业→恢复)
//     + 重叠 I/O 终态状态机（WorkerIo.cpp 是它的内部实现，不另立第二套）。
//   * src/platform/linux/PosixHostProcess.cpp —— abstract Unix socket + fork/execv（不经 shell）
//     + SO_PEERCRED 端点核验 + PDEATHSIG/独立进程组的收尾上下文。
// 这里不出现任何原生句柄/描述符类型；句柄与 fd 只活在平台实现以内。
//
// 这一层与协议层（core/WorkerFraming.h 与各平台的载荷编解码）的关系：
//   * 本服务只管"字节进出、对端是谁、进程还没退"，不解析任何消息内容；
//     版本（kProtocolVersion）、magic、nonce、长度判界全在帧/载荷解码里，收到字节不等于接受内容。
//   * 期限：本服务的每个等待入口都收 Deadline（单调毫秒预算，见 Deadline.h），到点就走
//     "请求取消 -> 有界排干 -> 仍未终态就移交收养"的固定次序，绝不无限等，也绝不提前松手。
//     父进程与辅助进程之间传递的只有**剩余时长**（budgetMs），从来不是某一方的绝对时钟读数，
//     所以两个时钟域之间不存在直接相减（分工与换算见 core/HelperTiming.h）。
//   * 人工确认不在这条路上：辅助进程永不弹确认框、不读授权（Worker.h 第 1 条边界）；
//     Deadline 的人工暂停作用域只影响父进程自己领取"还剩多少"的那一刻。
//
// 三条安全边界（改动前先对齐）：
//   1. 端点可信性由内核回答，不由名字回答：Accept 时核对"连上来的正是我刚起的那个 PID"
//      （Windows GetNamedPipeClientProcessId / Linux SO_PEERCRED），再核对帧里的本次 nonce。
//      任何本地进程就算猜中端点名，PID 对不上就整条拒绝 —— 不存在"按名字接活"的服务端。
//   2. 命令行只带坐标（端点令牌、nonce、父 PID），不带任何可被执行的文字、路径模板或输出位置；
//      任务本体走管道。参数按位置直接交给 execv / CreateProcessW，全程不拼接、不经过 shell。
//   3. 终止只针对**本次创建且身份核对过**的那一个进程，外加"为它专门创建的收尾上下文"
//      （Windows：带 KILL_ON_JOB_CLOSE 的作业；Linux：为它单独建的那个进程组）。
//      绝不按映像名批量收尾，绝不碰别人的进程，绝不强杀持有 GUI 状态的线程。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Deadline.h"
#include "core/Error.h"

namespace ecapture {
namespace hostproc {

// 请求取消之后，愿意多等一次"确认终态"的宽限（毫秒）。它**不占**自动处理预算：
// 预算在那次超时上已经花掉了，这一段纯粹是把"取消已受理"兑现成"操作确实结束"
//（Windows 的 OVERLAPPED/事件/缓冲在这之前不能松手）。有这条上限就不许把它说成
// "无限等待"；也不许有人拿"预算到点"当理由跳过它。两个入口共用这一个数：
// 每次通道操作内部的取消排干，与交易收尾 FinishTransaction 对残留操作的最后一排。
inline constexpr uint32_t kCancelDrainGraceMs = 2000u;

// 一次通道操作的结局。分支只看这三个布尔与调用方的预算；code 只做核对与原样上报，
// 不做跨平台分支（Windows 交回 GetLastError 的原值，Linux 交回 errno 的原值，
// 两个数值空间互不相通 —— 见 core/Error.h 的约定）。
struct ChannelIo {
    bool ok = false;          // 全部字节按预期完成
    bool timedOut = false;    // 预算内没等到终态（取消已按固定次序处理）
    bool peerClosed = false;  // 对端已关闭（EOF / ERROR_BROKEN_PIPE / EPIPE / ECONNRESET）
    NativeErrorCode code = kNativeErrorNone;
};

// 内核交回的对端身份。uid 在 Windows 命名管道上问不出来（那侧的"是谁连的"由
// 管道名里的 PID+nonce、映像一致性与作业绑定共同保证），如实标 not known。
struct PeerId {
    bool pidKnown = false;
    uint64_t pid = 0;
    bool uidKnown = false;
    uint64_t uid = 0;
};

// 三个 RAII 句柄包装（管道端点 / 通道 / 子进程）。共同的形状：
//   * Impl 是**公开声明但不完整**的嵌套类型 —— 只有两份平台实现里它才有定义，
//     所以 Raw()/Absorb 在 hostproc 之外等于不可用（拿到的指针没法解引用也没法造），
//     平台内部就不用给每条服务函数挂一遍 friend；
//   * 析构、移动与 Close 由两份平台实现各自定义（Impl 不完整，头文件里编不出来）。
class IpcEndpoint {
public:
    struct Impl;
    IpcEndpoint() = default;
    ~IpcEndpoint();
    IpcEndpoint(IpcEndpoint&& other) noexcept;
    IpcEndpoint& operator=(IpcEndpoint&& other) noexcept;
    IpcEndpoint(const IpcEndpoint&) = delete;
    IpcEndpoint& operator=(const IpcEndpoint&) = delete;

    bool IsOpen() const;
    // 放进辅助进程命令行的坐标（Windows = 管道名；Linux = 继承描述符的编号）。
    // 它只是"从哪儿取任务"的地址，不含任何可执行语义。
    const std::wstring& Token() const;
    void Close();

    Impl* Raw() const { return impl_; }                     // 平台实现内部出入口
    void Absorb(Impl* impl) { Close(); impl_ = impl; }      // 接管所有权（先关旧的）

private:
    Impl* impl_ = nullptr;
};

class IpcChannel {
public:
    struct Impl;
    IpcChannel() = default;
    ~IpcChannel();
    IpcChannel(IpcChannel&& other) noexcept;
    IpcChannel& operator=(IpcChannel&& other) noexcept;
    IpcChannel(const IpcChannel&) = delete;
    IpcChannel& operator=(const IpcChannel&) = delete;

    bool IsOpen() const;
    void Close();

    Impl* Raw() const { return impl_; }
    void Absorb(Impl* impl) { Close(); impl_ = impl; }

private:
    Impl* impl_ = nullptr;
};

class ChildProcess {
public:
    struct Impl;
    ChildProcess() = default;
    ~ChildProcess();   // 没收过尸就按"父进程收尸"这一档收尾：绝不把僵尸或孤儿留给桌面
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    uint64_t Pid() const;
    bool HasChild() const;

    Impl* Raw() const { return impl_; }
    void Absorb(Impl* impl);   // 先按析构同样的规矩收掉旧的

private:
    Impl* impl_ = nullptr;
};

// 本次交易的随机数（管道名/端点名与报头 nonce 用同一条：本次任务 <-> 本次应答的配对凭据）。
// 失败（操作系统交不出随机数）返回 false：没有 nonce 就不起辅助进程。
bool MakeNonce(uint64_t* out);

// 建好服务端点（Windows：FIRST_PIPE_INSTANCE 的单实例管道 —— 同名已有就直接失败，
// 不去接谁提前建好的"等着我交任务"的服务端；Linux：只在本进程持有的 abstract socket 监听端）。
bool CreateEndpoint(uint64_t nonce, IpcEndpoint* out, NativeErrorCode* err);

// 直接创建辅助进程（execv / CreateProcessW）。args 原样按位置传出，不经过任何 shell。
// Windows 实现内部走"挂起创建 -> 放进带 KILL_ON_JOB_CLOSE 的作业 -> 恢复"，挂不进作业
// 就当场收回，绝不放它裸跑；Linux 实现内部走"独立进程组 -> PDEATHSIG -> 端点连接 ->
// execv"，并关掉除 0/1/2 与端点之外继承来的描述符。
struct SpawnOptions {
    std::wstring exePath;                 // 要执行的映像本身（生产路径 = 同一个程序自己）
    std::vector<std::wstring> args;       // 端点令牌、nonce、父 PID 这类坐标
    bool captureStdio = false;            // 仅 Linux 兑现（见 CaptureChildStdio）
};
bool SpawnChild(const SpawnOptions& opts, IpcEndpoint* endpoint, ChildProcess* out,
                NativeErrorCode* err);

// 父进程侧：在 wait 之内等连接建立，并用内核答案核对"连上来的就是 expectedPid"。
// 对不上就关掉那条连接、返回 false —— 伪造的"结果"一个字节都不会被当成应答消费。
// 成功时端点的底层资源移交进 channel（此后由通道与收尾次序负责）。
// fault 交回失败属于哪一类（调用方据此分"期限耗尽 / 机制故障 / 身份核不上"三条诊断，
// 与旧的 Worker.cpp 五类结局一一对应）；nativeErr 带各平台原生的错误码，只作核对。
enum class AcceptFault {
    kTimeout,        // 预算内没等到连接（调用方继续分"预算烧光"还是"辅助进程没接上来"）
    kChannel,        // 连接层错误（accept/管道自己坏了），原生码进 nativeErr
    kPeerUnknown,    // 内核答不出"是谁连的"：身份无法核对 = 拒绝，原生码进 nativeErr
    kPeerMismatch,   // 连上来的不是 expectedPid：nativeErr = 身份不符的标记码
};
bool AcceptChannel(IpcEndpoint* endpoint, uint64_t expectedPid, const Deadline& wait,
                   AcceptFault* fault, IpcChannel* out, NativeErrorCode* err);

// 子进程侧：连回父进程（Windows 打开管道名；Linux 取 SpawnChild 事先连好并继承下来的描述符）。
bool ConnectToParent(const std::wstring& token, IpcChannel* out, NativeErrorCode* err);

// 端点令牌"形状像不像我们自己的"（辅助进程绑定校验的第 1 步原料）：
// Windows 查命名管道前缀 + 本次 PID-nonce 的形状；Linux 查"十进制描述符编号"。
// 名字形状只是第一道过滤 —— "是谁连的"始终由内核答案与帧里的 nonce 共同核对。
bool EndpointTokenLooksOurs(const std::wstring& token);

// 对端身份（内核给的，不是对方自称的）。
PeerId ChannelPeer(const IpcChannel& channel);

// 带期限的双向搬运（父进程侧用）。到点走"取消 -> 有界排干 -> 移交收养"，
// 调用方内存不会与未决操作共用；写完不等于对方读到，读完以实际搬运数为准。
ChannelIo WriteAll(IpcChannel* channel, const uint8_t* data, size_t size, const Deadline& wait);
ChannelIo ReadExact(IpcChannel* channel, uint8_t* buf, size_t size, const Deadline& wait);
// 子进程侧的阻塞版本：辅助进程没有第二份预算可传 —— 它活多久由 core/HelperTiming.h 的
// 看门狗算式判（等任务段兜底 kHandshakeMs，执行段兜底随任务交下来的预算 + 交回宽限），
// 通道只在对端断开时把"读不下去了"如实交回。
ChannelIo WriteAllBlocking(IpcChannel* channel, const uint8_t* data, size_t size);
ChannelIo ReadExactBlocking(IpcChannel* channel, uint8_t* buf, size_t size);

// 有界等待（不消费预算之外的时间；waitMs = 0 是只探一次）。
bool WaitChild(const ChildProcess& child, uint32_t waitMs, bool* exited);

// 收尸：先等 waitMs；到点没退就**只**结束这一个进程（连同为它创建的收尾上下文里的后代），
// 退出码按 reapedCode 记档（kHelperExitReaped 那一档，与辅助进程自己的判断分得开）。
// 返回时保证：没有僵尸留在本进程的子进程表里。Linux 侧若进程是被信号打死的，
// ChildExitCode 交回 128+signum（shell 惯例，只作诊断，不进对外契约）。
void ReapChild(ChildProcess* child, uint32_t waitMs, uint32_t reapedCode);
int ChildExitCode(const ChildProcess& child);   // -1 = 问不出来

// 交易收尾的固定次序（Windows 现状逐字保留）：通道没有未决操作就先关；收尸；
// 再排干通道上残留的取消；仍进不了终态就把句柄连同其事件/缓冲移交登记表收养；
// 最后关收尾上下文。带着未决 I/O 关句柄属于文档不保证的行为，宁可遗留也不关。
void FinishTransaction(ChildProcess* child, IpcChannel* channel, uint32_t reapWaitMs,
                       uint32_t drainGraceMs);

// 并行取回辅助进程的 stdout/stderr：单循环同时看两条流，一条堵不住另一条（这就是
// "并行读取、避免死锁"的兑现方式）；超过 capBytes 的部分照旧排干但不留档。
// 仅 Linux 实现：Windows 的辅助进程在入口就把三条标准句柄换成 NUL
// （SilenceStandardHandles，既有行为），不存在第二份可读的流 —— Windows 侧本函数
// 如实返回 false 并写"不支持"的原生码，不假装。captureStdio=false 时 Linux 也把
// 标准流接去 /dev/null，辅助进程写不到调用方的流上。
bool CaptureChildStdio(ChildProcess* child, const Deadline& wait, std::string* outStdout,
                       std::string* outStderr, size_t capBytes, NativeErrorCode* err);

// ---------------------------------------------------------------------------
// 身份与存活问答（辅助进程侧绑定校验的原料）。任何一条问不出来，调用方都按"不成立"处理。
// ---------------------------------------------------------------------------

// 本进程映像路径（Windows GetModuleFileNameW / Linux /proc/self/exe）。
bool SelfImagePath(std::wstring* out);
// 某个 PID 的映像路径；进程已退或问不出来返回 false。
bool ImagePathOf(uint64_t pid, std::wstring* out);
// 进程还在（不含"已退未收尸"的僵尸冒充活着）。
bool ProcessAlive(uint64_t pid);
// 与 pid 处在同一登录会话（Windows = ProcessIdToSessionId；Linux = 同一会话首进程所带的会话）。
bool SameLoginSession(uint64_t pid);
// 我的直接父进程是谁（与"父进程说它是谁"是两个独立来源，两者要一致）。问不出来返回 0。
uint64_t ImmediateParentPid();
// "父进程一退，我会被一起带走"这一条在当前进程身上是否已经装上：
// Windows = 我在一份带 KILL_ON_JOB_CLOSE 的作业里；Linux = PDEATHSIG 已挂上。
bool ParentCleanupArmed();

}  // namespace hostproc
}  // namespace ecapture
