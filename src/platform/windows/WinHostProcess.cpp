// 【平台层 · Windows】隔离执行的平台服务实现：单实例命名管道端点、挂起创建→进作业→恢复的
// 直接进程创建、重叠 I/O 终态状态机（WorkerIo.cpp 是它的内部实现，不另立第二套）、
// 以及"只收拾我起的那一个进程（和为它专门建的作业）"的终止收尾。
//
// 声明面与三条安全边界在 src/HostProcess.h；Linux 对应实现是
// src/platform/linux/PosixHostProcess.cpp。这一份的正文是从 Worker.cpp 的 Transaction
// **原样搬过来的**（句柄次序、KILL_ON_JOB_CLOSE、FIRST_PIPE_INSTANCE、ConnectNamedPipe 的
// 四态判定、取消排干与收养次序都逐字保留），语义改动必须先改声明面再两端同改。
//
// 与 Linux 侧的对应关系：管道名里的 PID+nonce ↔ abstract 名字里的 PID+nonce；
// GetNamedPipeClientProcessId ↔ SO_PEERCRED；作业（KILL_ON_JOB_CLOSE）↔ 独立进程组+PDEATHSIG；
// OVERLAPPED 未决操作的"收养"档在 Linux 上不存在（poll 当场进终态），FinishTransaction 里如实降级。

#define _CRT_RAND_S   // 必须在 <stdlib.h> 之前定义，rand_s 才会被声明
#include "HostProcess.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <new>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <tlhelp32.h>

#include "WorkerIo.h"
#include "core/HelperTiming.h"   // kHelperExitReaped（收尸档）
#include "platform/windows/WinBridge.h"   // WaitTimeout：Deadline 到 Win32 等待参数的换算

namespace ecapture {
namespace hostproc {
namespace {

using Step = workerio::IoStep;

// 管道名的固定前缀：辅助进程按名字形状 + nonce + 是谁起的我，三重核对。
constexpr const wchar_t* kPipePrefix = L"\\\\.\\pipe\\ecapture-worker-";
constexpr uint32_t kPipeBufferBytes = 64u * 1024u;
//（取消排干宽限 kCancelDrainGraceMs 在 HostProcess.h 一处定义，收尾与每次操作用同一个数。）

std::wstring HexOf(uint64_t v) {
    wchar_t buf[24];
    swprintf(buf, 24, L"%016llX", static_cast<unsigned long long>(v));
    return buf;
}

// Windows argv 的引号规则（与 tests\harness.psm1 的 Format-EcWindowsArgv 同一套）：
// 只在真的需要时加引号，引号前的反斜杠加倍，结尾的反斜杠也加倍。
// 这里递出去的坐标（管道名、十六进制 nonce、十进制 PID）都不含空格，
// 但可执行文件路径可能含空格与中文，所以这条规则照样要写对。
std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') { out.append(slashes * 2 + 1, L'\\'); out.push_back(L'"'); }
        else out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

ChannelIo OkIo() {
    ChannelIo r;
    r.ok = true;
    return r;
}

ChannelIo StepToIo(const Step& s) {
    ChannelIo r;
    r.ok = s.done;
    r.timedOut = s.timedOut;
    r.code = s.gle;
    if (s.unresolved && !s.timedOut) {
        // 取消宽限内没等到终态：资源已在登记表里活着，对调用方这就是"这次操作不能算完成"。
        r.code = s.gle ? s.gle : ERROR_IO_PENDING;
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// pimpl 本体
// ---------------------------------------------------------------------------

struct IpcEndpoint::Impl {
    HANDLE pipe = nullptr;   // 服务端管道句柄（Accept 成功后移交通道）
    std::wstring name;
};

struct IpcChannel::Impl {
    HANDLE pipe = nullptr;   // 与端点同一个句柄：Accept 时移交，本层是唯一关闭责任人
};

struct ChildProcess::Impl {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE job = nullptr;
    DWORD pid = 0;
    bool waited = false;     // 已进终态（等过或收过）
    int exitCode = -1;
};

// ---------------------------------------------------------------------------
// 包装类生命周期
// ---------------------------------------------------------------------------

IpcEndpoint::~IpcEndpoint() { Close(); }
IpcEndpoint::IpcEndpoint(IpcEndpoint&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
IpcEndpoint& IpcEndpoint::operator=(IpcEndpoint&& o) noexcept {
    if (this != &o) { Close(); impl_ = o.impl_; o.impl_ = nullptr; }
    return *this;
}
bool IpcEndpoint::IsOpen() const {
    return impl_ && impl_->pipe != nullptr && impl_->pipe != INVALID_HANDLE_VALUE;
}
const std::wstring& IpcEndpoint::Token() const {
    static const std::wstring kEmpty;
    return impl_ ? impl_->name : kEmpty;
}
void IpcEndpoint::Close() {
    if (impl_) {
        if (impl_->pipe && impl_->pipe != INVALID_HANDLE_VALUE) {
            // 从未 Accept 过的端点：上面不可能挂业务 I/O，直接关。
            CloseHandle(impl_->pipe);
        }
        delete impl_;
        impl_ = nullptr;
    }
}

IpcChannel::~IpcChannel() { Close(); }
IpcChannel::IpcChannel(IpcChannel&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
IpcChannel& IpcChannel::operator=(IpcChannel&& o) noexcept {
    if (this != &o) { Close(); impl_ = o.impl_; o.impl_ = nullptr; }
    return *this;
}
bool IpcChannel::IsOpen() const {
    return impl_ && impl_->pipe != nullptr && impl_->pipe != INVALID_HANDLE_VALUE;
}
void IpcChannel::Close() {
    if (impl_) {
        if (impl_->pipe && impl_->pipe != INVALID_HANDLE_VALUE) {
            // 带着未决 I/O 关句柄属于文档不保证的行为：先问登记表，排不干净就移交收养
            //（与 FinishTransaction 同一条例外；这里的兜底只防"没走 Finish 就松手"的调用方）。
            if (!workerio::HasPendingOps(impl_->pipe)) {
                CloseHandle(impl_->pipe);
            } else {
                static_cast<void>(workerio::AdoptHandle(impl_->pipe));
            }
        }
        delete impl_;
        impl_ = nullptr;
    }
}

namespace {
void ReapOrFail(ChildProcess::Impl* impl, uint32_t waitMs, uint32_t reapedCode) {
    if (impl && impl->process && !impl->waited) {
        if (WaitForSingleObject(impl->process, waitMs) != WAIT_OBJECT_0) {
            TerminateProcess(impl->process, reapedCode);
            WaitForSingleObject(impl->process, 1000);
        }
        DWORD code = 0;
        impl->exitCode = GetExitCodeProcess(impl->process, &code) ? static_cast<int>(code) : -1;
        impl->waited = true;
    }
    if (impl) {
        if (impl->process) CloseHandle(impl->process);
        if (impl->thread) CloseHandle(impl->thread);
        if (impl->job) CloseHandle(impl->job);   // KILL_ON_JOB_CLOSE：没退完的后代由内核带走
        impl->process = impl->thread = impl->job = nullptr;
    }
}
}  // namespace

ChildProcess::~ChildProcess() {
    if (impl_) { ReapOrFail(impl_, 0, static_cast<uint32_t>(kHelperExitReaped)); delete impl_; }
    impl_ = nullptr;
}
ChildProcess::ChildProcess(ChildProcess&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
ChildProcess& ChildProcess::operator=(ChildProcess&& o) noexcept {
    if (this != &o) {
        if (impl_) { ReapOrFail(impl_, 0, static_cast<uint32_t>(kHelperExitReaped)); delete impl_; }
        impl_ = o.impl_; o.impl_ = nullptr;
    }
    return *this;
}
void ChildProcess::Absorb(Impl* impl) {
    if (impl_) { ReapOrFail(impl_, 0, static_cast<uint32_t>(kHelperExitReaped)); delete impl_; }
    impl_ = impl;
}
uint64_t ChildProcess::Pid() const { return impl_ ? static_cast<uint64_t>(impl_->pid) : 0; }
bool ChildProcess::HasChild() const { return impl_ && impl_->process != nullptr; }

// ---------------------------------------------------------------------------
// 服务入口
// ---------------------------------------------------------------------------

bool MakeNonce(uint64_t* out) {
    // rand_s 是加密质量的随机数；拿不到时交回 false（迁自原 Worker.cpp 的 MakeNonce，
    // 逐字保留：成功时用 QPC + PID + 栈地址再搅一遍，把同一次运行里的唯一性钉死；
    // 那份兜底搅拌不是安全边界，安全边界是 rand_s 本身）。
    uint32_t a = 0, b = 0;
    if (rand_s(&a) != 0) return false;
    if (rand_s(&b) != 0) return false;
    LARGE_INTEGER tick{};
    QueryPerformanceCounter(&tick);
    uint64_t n = (static_cast<uint64_t>(a) << 32) | b;
    n ^= static_cast<uint64_t>(tick.QuadPart);
    n ^= (static_cast<uint64_t>(GetCurrentProcessId()) << 16);
    n ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&a));
    if (n == 0) return false;   // 0 是保留值（"没有这笔交易"）
    *out = n;
    return true;
}

bool CreateEndpoint(uint64_t nonce, IpcEndpoint* out, NativeErrorCode* err) {
    *err = kNativeErrorNone;
    IpcEndpoint::Impl* impl = new (std::nothrow) IpcEndpoint::Impl();
    if (!impl) { *err = ERROR_NOT_ENOUGH_MEMORY; return false; }
    impl->name = std::wstring(kPipePrefix) + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 HexOf(nonce);
    // 只开一份实例，且带 FILE_FLAG_FIRST_PIPE_INSTANCE：同名已经存在就直接失败，
    // 不去接一个别人提前建好、"等着我交任务"的服务端。
    impl->pipe = CreateNamedPipeW(impl->name.c_str(),
                                  PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                      FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  1, kPipeBufferBytes, kPipeBufferBytes, NMPWAIT_USE_DEFAULT_WAIT,
                                  nullptr);
    if (impl->pipe == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        delete impl;
        return false;
    }
    out->Absorb(impl);
    return true;
}

bool SpawnChild(const SpawnOptions& opts, IpcEndpoint* endpoint, ChildProcess* out,
                NativeErrorCode* err) {
    *err = kNativeErrorNone;
    if (!endpoint->IsOpen()) { *err = ERROR_INVALID_HANDLE; return false; }

    // 作业带 KILL_ON_JOB_CLOSE：父进程不管是正常退出还是被人结束，句柄一关，
    // 内核就把辅助进程一起带走。"辅助进程长期遗留"这件事因此不依赖我们自己记得收尾。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) { *err = GetLastError(); return false; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // KILL_ON_CLOSE 是"不遗留"的机制；ACTIVE_PROCESS=1 与不许 breakaway 是"辅助进程里
    // 不会再长出一个不受这份作业管的进程"。BREAKAWAY_OK / SILENT_BREAKAWAY_OK 都不给，
    // 所以它自己也没法逃出这份作业。
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const DWORD e = GetLastError();
        CloseHandle(job);
        *err = e;
        return false;
    }

    // 任务本体走管道，命令行只带"从哪儿取任务"的坐标：
    // 辅助进程能读到的参数里没有输出路径，也没有任何能被执行的文字。
    std::wstring cmd = QuoteArg(opts.exePath);
    for (const std::wstring& a : opts.args) cmd += L" " + QuoteArg(a);
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // 先挂起再进作业：否则辅助进程有几微秒不属于任何作业，
    // 那条"父进程退了我就没电"的保证就有窗口期能漏掉。
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        const DWORD e = GetLastError();
        CloseHandle(job);
        *err = e;
        return false;
    }
    ChildProcess::Impl* impl = new (std::nothrow) ChildProcess::Impl();
    if (!impl) {
        TerminateProcess(pi.hProcess, static_cast<uint32_t>(kHelperExitReaped));
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        *err = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    impl->job = job;
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD e = GetLastError();
        // 挂不进作业就绝不放它跑：那条"父进程退了我就没电"的保证对它不成立，只能当场收回。
        TerminateProcess(pi.hProcess, static_cast<uint32_t>(kHelperExitReaped));
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        delete impl;
        *err = e;
        return false;
    }
    impl->process = pi.hProcess;
    impl->thread = pi.hThread;
    impl->pid = pi.dwProcessId;
    ResumeThread(impl->thread);
    out->Absorb(impl);
    return true;
}

bool AcceptChannel(IpcEndpoint* endpoint, uint64_t expectedPid, const Deadline& wait,
                   AcceptFault* fault, IpcChannel* out, NativeErrorCode* err) {
    *err = kNativeErrorNone;
    *fault = AcceptFault::kChannel;
    if (!endpoint->IsOpen()) { *err = ERROR_INVALID_HANDLE; return false; }
    const HANDLE pipe = endpoint->Raw()->pipe;
    // 把管道交出去之前先确认"连上来的是我刚起的那个进程"：
    // 名字里带了本次的 PID 与 nonce，但"是谁连的"要由内核回答，不能靠名字。
    // 连接有四类结局，逐类分开判（见 workerio::ConnectStartOf）；本层把它翻成：
    //   * 成功（含"抢先连接"这条无挂起操作的成功）-> 继续核对 PID；
    //   * 到点 -> ERROR_TIMEOUT（调用方按预算是否烧光分类）；
    //   * 其他错误 -> 原样上报。
    const Step connect = workerio::RunOverlappedOp(
        workerio::Win32Backend(), pipe, 0, WaitTimeout(wait), kCancelDrainGraceMs,
        [&](OVERLAPPED& ov, uint8_t*, uint32_t) {
            return workerio::ConnectStartOf(ConnectNamedPipe(pipe, &ov));
        },
        nullptr);
    if (connect.timedOut) { *fault = AcceptFault::kTimeout; *err = ERROR_TIMEOUT; return false; }
    if (!connect.done) { *err = connect.gle; return false; }

    DWORD peerPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &peerPid)) {
        *fault = AcceptFault::kPeerUnknown;   // 内核答不出是谁连的：拒绝，不"照着看起来能跑的样子"继续
        *err = GetLastError();
        return false;
    }
    if (static_cast<uint64_t>(peerPid) != expectedPid) {
        // 连上来的不是我起的那个：任务一个字节都不交。
        *fault = AcceptFault::kPeerMismatch;
        *err = ERROR_INVALID_WINDOW_HANDLE;
        return false;
    }
    IpcChannel::Impl* impl = new (std::nothrow) IpcChannel::Impl();
    if (!impl) { *fault = AcceptFault::kChannel; *err = ERROR_NOT_ENOUGH_MEMORY; return false; }
    impl->pipe = pipe;                 // 句柄移交：此后关闭责任在通道（含收养那条例外）
    endpoint->Raw()->pipe = nullptr;   // 端点不再持有，Close 不会重复关
    out->Absorb(impl);
    return true;
}

bool ConnectToParent(const std::wstring& token, IpcChannel* out, NativeErrorCode* err) {
    const HANDLE pipe = CreateFileW(token.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    IpcChannel::Impl* impl = new (std::nothrow) IpcChannel::Impl();
    if (!impl) { CloseHandle(pipe); *err = ERROR_NOT_ENOUGH_MEMORY; return false; }
    impl->pipe = pipe;
    out->Absorb(impl);
    *err = kNativeErrorNone;
    return true;
}

bool EndpointTokenLooksOurs(const std::wstring& name) {
    const std::wstring prefix(kPipePrefix);
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return false;
    const std::wstring tail = name.substr(prefix.size());
    if (tail.size() > 40) return false;
    bool dash = false;
    for (const wchar_t c : tail) {
        if (c == L'-') { if (dash) return false; dash = true; continue; }
        if (!iswdigit(c) && !iswalpha(c)) return false;
    }
    return dash;
}

PeerId ChannelPeer(const IpcChannel& channel) {
    PeerId p;
    if (!channel.IsOpen()) return p;
    DWORD pid = 0;
    if (GetNamedPipeClientProcessId(channel.Raw()->pipe, &pid)) {
        p.pidKnown = true;
        p.pid = static_cast<uint64_t>(pid);
    }
    // 命名管道上问不出对端 uid：如实标 not known（Windows 侧的"是谁"由
    // 管道名 + 映像一致性 + 作业绑定共同保证，见 Worker.cpp 的绑定校验）。
    return p;
}

// ---------------------------------------------------------------------------
// 带期限 / 阻塞的字节搬运
// ---------------------------------------------------------------------------

ChannelIo WriteAll(IpcChannel* channel, const uint8_t* data, size_t size, const Deadline& wait) {
    if (!channel->IsOpen()) { ChannelIo r; r.code = ERROR_INVALID_HANDLE; return r; }
    const HANDLE pipe = channel->Raw()->pipe;
    const DWORD timeout = WaitTimeout(wait);
    size_t off = 0;
    while (off < size) {
        const uint32_t chunk =
            static_cast<uint32_t>(std::min<size_t>(size - off, kPipeBufferBytes));
        const uint8_t* at = data + off;
        // 内核只从操作对象自有的缓冲里读：调用方的 framed 缓冲区在任何超时路径上
        // 都可以安全失效，不会因为一次没赶上的写请求而变成悬空引用。
        const Step s = workerio::RunOverlappedOp(
            workerio::Win32Backend(), pipe, chunk, timeout, kCancelDrainGraceMs,
            [&](OVERLAPPED& ov, uint8_t* buf, uint32_t cap) {
                std::memcpy(buf, at, chunk);
                return workerio::StartOf(WriteFile(pipe, buf, cap, nullptr, &ov));
            },
            nullptr);
        if (!s.done) return StepToIo(s);
        off += s.transferred;
    }
    return OkIo();
}

ChannelIo ReadExact(IpcChannel* channel, uint8_t* buf, size_t size, const Deadline& wait) {
    if (!channel->IsOpen()) { ChannelIo r; r.code = ERROR_INVALID_HANDLE; return r; }
    const HANDLE pipe = channel->Raw()->pipe;
    const DWORD timeout = WaitTimeout(wait);
    size_t off = 0;
    while (off < size) {
        const uint32_t chunk =
            static_cast<uint32_t>(std::min<size_t>(size - off, kPipeBufferBytes));
        // 读取同样落进操作对象的缓冲，确认终态后才复制回调用方内存：
        // 一次超时的挂起读不会再往调用方已经释放的堆块里写像素。
        std::vector<uint8_t> got;
        const Step s = workerio::RunOverlappedOp(
            workerio::Win32Backend(), pipe, chunk, timeout, kCancelDrainGraceMs,
            [&](OVERLAPPED& ov, uint8_t* dst, uint32_t cap) {
                return workerio::StartOf(ReadFile(pipe, dst, cap, nullptr, &ov));
            },
            &got);
        if (!s.done) return StepToIo(s);
        if (s.transferred == 0) {
            // 0 字节 = 对方已经把写完的那一段交回并关了管道：这次调用拿不到应答。
            ChannelIo broken;
            broken.peerClosed = true;
            broken.code = ERROR_BROKEN_PIPE;
            return broken;
        }
        std::memcpy(buf + off, got.data(), s.transferred);
        off += s.transferred;
    }
    return OkIo();
}

ChannelIo WriteAllBlocking(IpcChannel* channel, const uint8_t* data, size_t size) {
    if (!channel->IsOpen()) { ChannelIo r; r.code = ERROR_INVALID_HANDLE; return r; }
    const HANDLE pipe = channel->Raw()->pipe;
    size_t off = 0;
    while (off < size) {
        const uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(size - off, 0xFFFFu));
        DWORD written = 0;
        if (!WriteFile(pipe, data + off, chunk, &written, nullptr)) {
            ChannelIo r;
            r.code = GetLastError();
            if (r.code == ERROR_BROKEN_PIPE || r.code == ERROR_NO_DATA) r.peerClosed = true;
            return r;
        }
        if (written == 0) { ChannelIo r; r.peerClosed = true; r.code = ERROR_BROKEN_PIPE; return r; }
        off += written;
    }
    return OkIo();
}

// 子进程侧的阻塞读（父进程已经把写完的那段交回并关闭时如实返回"读不下去了"）。
namespace {
bool ReadAllBlocking(HANDLE pipe, uint8_t* buf, size_t size) {
    size_t got = 0;
    while (got < size) {
        DWORD one = 0;
        if (!ReadFile(pipe, buf + got, static_cast<DWORD>(size - got), &one, nullptr)) return false;
        if (one == 0) return false;   // 管道断了：父进程已经走了，没有任务可等
        got += one;
    }
    return true;
}
}  // namespace

ChannelIo ReadExactBlocking(IpcChannel* channel, uint8_t* buf, size_t size) {
    if (!channel->IsOpen()) { ChannelIo r; r.code = ERROR_INVALID_HANDLE; return r; }
    if (!ReadAllBlocking(channel->Raw()->pipe, buf, size)) {
        ChannelIo r;
        r.peerClosed = true;
        r.code = ERROR_BROKEN_PIPE;
        return r;
    }
    return OkIo();
}

// ---------------------------------------------------------------------------
// 等待与收尾
// ---------------------------------------------------------------------------

bool WaitChild(const ChildProcess& child, uint32_t waitMs, bool* exited) {
    *exited = false;
    if (!child.HasChild()) return false;
    ChildProcess::Impl* impl = const_cast<ChildProcess&>(child).Raw();
    if (impl->waited) { *exited = true; return true; }
    const DWORD r = WaitForSingleObject(impl->process, waitMs);
    if (r == WAIT_OBJECT_0) {
        DWORD code = 0;
        impl->exitCode = GetExitCodeProcess(impl->process, &code) ? static_cast<int>(code) : -1;
        impl->waited = true;
        *exited = true;
    }
    return true;
}

void ReapChild(ChildProcess* child, uint32_t waitMs, uint32_t reapedCode) {
    if (!child->HasChild()) return;
    ChildProcess::Impl* impl = child->Raw();
    if (!impl->waited) {
        if (WaitForSingleObject(impl->process, waitMs) != WAIT_OBJECT_0) {
            // 只结束我自己起的这一个进程；退出码写成"父进程收尸"这一档，
            // 好让 hint 里的数与辅助进程自己的判断分得开。
            TerminateProcess(impl->process, reapedCode);
            WaitForSingleObject(impl->process, 1000);
        }
        DWORD code = 0;
        impl->exitCode = GetExitCodeProcess(impl->process, &code) ? static_cast<int>(code) : -1;
        impl->waited = true;
    }
}

int ChildExitCode(const ChildProcess& child) {
    return child.Raw() ? child.Raw()->exitCode : -1;
}

void FinishTransaction(ChildProcess* child, IpcChannel* channel, uint32_t reapWaitMs,
                       uint32_t drainGraceMs) {
    // 顺序是固定的（搬自原 Transaction::Finish，逐字保留）：
    //   1. 通道句柄上没有未决异步操作 -> 先关管道，提示辅助进程退出，不多等一次；
    //   2. 收尸：等 -> 到点只结束这一个进程 -> 取退出码（先收尸再问码，反了拿到的是 259）；
    //   3. 管道句柄还有未决操作（取消已受理但宽限内等不到终态的病理场景）：带着未决 I/O
    //      关句柄是文档不保证的行为，所以先收完尸再排干；仍进不了终态就把句柄连同它的
    //      OVERLAPPED/事件/缓冲交给登记表收养，随进程退出由系统回收 ——
    //      代价是一份有界缓冲与一个句柄值的遗留，换来的是不再有"内核往已释放内存写"；
    //   4. 最后关作业句柄：KILL_ON_JOB_CLOSE 是"我没能好好收尸"时的兜底，不是常规手段。
    // 这三个 workerio 入口都不抛出（见 WorkerIo.h），收尾链路不会把异常带进析构。
    IpcChannel::Impl* ch = channel->Raw();
    if (ch && ch->pipe && !workerio::HasPendingOps(ch->pipe)) {
        CloseHandle(ch->pipe);
        ch->pipe = nullptr;
    }
    if (child->HasChild()) ReapChild(child, reapWaitMs, static_cast<uint32_t>(kHelperExitReaped));
    if (ch && ch->pipe) {
        if (workerio::DrainPendingOps(ch->pipe, drainGraceMs) == 0) {
            CloseHandle(ch->pipe);
        } else {
            // 收养成不成功都要把关闭责任交出去：这条句柄上还挂着没确认终态的操作，
            // 由调用方自己关它依旧是文档不保证的行为。
            static_cast<void>(workerio::AdoptHandle(ch->pipe));
        }
        ch->pipe = nullptr;
    }
    ChildProcess::Impl* ci = child->Raw();
    if (ci) {
        if (ci->process) { CloseHandle(ci->process); ci->process = nullptr; }
        if (ci->thread) { CloseHandle(ci->thread); ci->thread = nullptr; }
        if (ci->job) { CloseHandle(ci->job); ci->job = nullptr; }
    }
}

// ---------------------------------------------------------------------------
// stdout/stderr 并行排干 —— Windows 如实报"不支持"
// ---------------------------------------------------------------------------

bool CaptureChildStdio(ChildProcess* child, const Deadline& wait, std::string* outStdout,
                       std::string* outStderr, size_t capBytes, NativeErrorCode* err) {
    // Windows 的辅助进程在入口就把三条标准句柄换成 NUL（Worker.cpp 的 SilenceStandardHandles，
    // 既有行为，含 ConPTY 上 FreeConsole 卡死的实测原因），不存在第二份可读的流；
    // 死锁风险由"根本没有这两条流"消除。这里不假装支持。
    (void)child; (void)wait; (void)outStdout; (void)outStderr; (void)capBytes;
    *err = ERROR_NOT_SUPPORTED;
    return false;
}

// ---------------------------------------------------------------------------
// 身份与存活问答（逐字迁自原 Worker.cpp 的辅助进程侧）
// ---------------------------------------------------------------------------

namespace {
std::wstring ModulePathOf(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return std::wstring();
    wchar_t buffer[8192] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buffer, &size)) result.assign(buffer, size);
    CloseHandle(proc);
    return result;
}
}  // namespace

bool SelfImagePath(std::wstring* out) {
    wchar_t buffer[8192] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) return false;
    out->assign(buffer, n);
    return true;
}

bool ImagePathOf(uint64_t pid, std::wstring* out) {
    if (pid == 0 || pid > 0xFFFFFFFFull) return false;
    const std::wstring path = ModulePathOf(static_cast<DWORD>(pid));
    if (path.empty()) return false;
    *out = path;
    return true;
}

bool ProcessAlive(uint64_t pid) {
    if (pid == 0 || pid > 0xFFFFFFFFull) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                              static_cast<DWORD>(pid));
    if (!proc) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE;
    CloseHandle(proc);
    return alive;
}

bool SameLoginSession(uint64_t pid) {
    if (pid == 0 || pid > 0xFFFFFFFFull) return false;
    DWORD mine = 0, theirs = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &mine)) return false;
    if (!ProcessIdToSessionId(static_cast<DWORD>(pid), &theirs)) return false;
    return mine == theirs;
}

// 我的直接父进程是谁：这条判断和"父进程说它是谁"是两个独立来源，两者要一致。
// 只对 CreateToolhelp32Snapshot 的整张表负责，拿不到就当作不成立（宁可拒绝执行）。
uint64_t ImmediateParentPid() {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == GetCurrentProcessId()) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return static_cast<uint64_t>(parent);
}

// "我在一个'父进程一关作业就一起结束'的作业里" —— 这条既是"不会长期遗留"的机制本身，
// 也是辅助进程肯干活的前提之一：正常 shell 里直接跑辅助模式时通常没有这么一份作业。
bool ParentCleanupArmed() {
    BOOL inJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob) return false;
    JOBOBJECT_BASIC_LIMIT_INFORMATION basic{};
    if (!QueryInformationJobObject(nullptr, JobObjectBasicLimitInformation, &basic, sizeof(basic),
                                   nullptr)) {
        return false;
    }
    return (basic.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
}

}  // namespace hostproc
}  // namespace ecapture
