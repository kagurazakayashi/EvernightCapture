// 【平台层 · Linux】隔离执行的平台服务实现：直接进程创建（fork/execve，绝不经过 shell）、
// abstract Unix socket 的可信 IPC 端点、SO_PEERCRED 端点核验、单调预算内的读写与等待、
// 以及"只收拾我起的那一个进程（和为它专门建的进程组）"的终止收尾。
//
// 声明面（职责、边界与三条安全规矩）在 src/HostProcess.h，这里只做 Linux 侧的兑现：
//   * 端点：abstract socket，名字带本次 PID + 随机 nonce；不进文件系统 ——
//     没有 /tmp 竞态，也没有可被替换的中间路径。
//   * 可信性：accept 后由 SO_PEERCRED 问内核"连上来的是哪个 PID/UID"，必须等于我刚起的那个
//     并且与我同 uid；猜中名字的任意本地进程在这一步被拒，伪造的"结果"一个字节都不消费。
//   * 收尾上下文：为该子进程单独建立的进程组（setpgid 两侧都调，赢竞态）+ PDEATHSIG
//     （父进程一没，内核给孩子发 SIGTERM）。收尸时 killpg 只打这个组 —— 组是我建的，
//     里面只有我播种的进程；绝不按映像名批量收尾，绝不碰别人的进程。
//   * 期限：每个等待入口收 Deadline（单调毫秒预算）；父进程与辅助进程之间传递的只有
//     剩余**时长**，两边的 CLOCK_MONOTONIC 读数从不互相相减（模型见 core/HelperTiming.h）。
//   * 描述符纪律：execve 前关掉除 0/1/2 与端点（固定 3 号）之外继承来的一切描述符，
//     其余全部创建时带 O_CLOEXEC；标准流默认接 /dev/null（与 Windows 的
//     SilenceStandardHandles 同一意图），captureStdio=true 时 stdout/stderr 接进
//     父进程另行**并行**排干的管道。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   // pipe2 / accept4 / struct ucred / SOCK_CLOEXEC / MSG_NOSIGNAL
#endif

#include "HostProcess.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "HostPath.h"           // MonotonicClockMs（CLOCK_MONOTONIC）
#include "core/HelperTiming.h"  // kHelperExitReaped / kHelperExitProtocol
#include "core/TextCoding.h"    // 路径字节 <-> std::wstring 的边界换码

extern char** environ;   // execve 原样带环境：辅助模式不吃任何环境变量开关（没有后门）

namespace ecapture {
namespace hostproc {
namespace {

// 继承给辅助进程的端点描述符编号：SpawnChild 在 execve 前 dup2 到位，token 就是这个数的十进制。
constexpr int kInheritedIpcFd = 3;
// accept 前子进程 connect 的兜底重试（listen 已建立，正常一次命中；只防极端调度抖动）。
constexpr int kConnectRetrySlack = 4;
constexpr unsigned kConnectRetryStepUs = 25000;

std::string ToUtf8(const std::wstring& w) { return EncodeUtf8(w); }

std::wstring FromUtf8(const std::string& s) {
    return DecodeUtf8(s.data(), s.size()).text;
}

// abstract socket 名字里的 nonce 写法：十六进制，与 Windows 管道名同一形状。
std::string HexNonce(uint64_t v) {
    static const char kDigits[] = "0123456789ABCDEF";
    char buf[17];
    for (int i = 15; i >= 0; --i) {
        buf[i] = kDigits[v & 0xFu];
        v >>= 4;
    }
    buf[16] = '\0';
    return std::string(buf);
}

bool FillSockaddr(const std::string& visibleName, struct sockaddr_un* addr, socklen_t* len) {
    // abstract：sun_path[0] = '\0'，后面照抄。名字总长（含前导 NUL）不能超过 sun_path。
    const std::string full = std::string(1, '\0') + visibleName;
    if (full.size() > sizeof(addr->sun_path)) return false;
    std::memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    std::memcpy(addr->sun_path, full.data(), full.size());
    // addrlen 覆盖到名字末尾（前导 NUL 计入），不许按 C 字符串那套提前截断。
    *len = static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + full.size());
    return true;
}

// poll 用的"还剩多少毫秒"：未启用预算交回 -1（不限）；启用值夹进 int 区间。
int PollTimeoutMs(const Deadline& wait) {
    if (!wait.Enabled()) return -1;
    const uint64_t left = wait.RemainingMs();
    if (left == 0) return 0;
    if (left > static_cast<uint64_t>(INT32_MAX)) return INT32_MAX;
    return static_cast<int>(left);
}

ChannelIo FailErr(int e) {
    ChannelIo r;
    r.code = static_cast<NativeErrorCode>(e ? e : EINVAL);
    return r;
}

ChannelIo TimeoutIo() {
    ChannelIo r;
    r.timedOut = true;
    r.code = static_cast<NativeErrorCode>(ETIMEDOUT);
    return r;
}

ChannelIo PeerClosedIo() {
    ChannelIo r;
    r.peerClosed = true;
    r.code = static_cast<NativeErrorCode>(EPIPE);
    return r;
}

// 从 waitpid 的 status 取诊断用退出码：正常退出取原值；被打死按 128+signum（shell 惯例，
// 只作诊断，不进对外契约）。
int ExitCodeFromStatus(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// execve 前：关掉除 keep 之外继承来的一切描述符（0/1/2 与端点已在 keep 里选好）。
void CloseAllExcept(const int* keep, size_t keepCount) {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) {
        // 问不出清单时的保守兜底：按固定上限逐个试关（EBADF 无害）。
        for (int fd = 3; fd < 4096; ++fd) {
            bool keepIt = false;
            for (size_t i = 0; i < keepCount; ++i) {
                if (keep[i] == fd) { keepIt = true; break; }
            }
            if (!keepIt) ::close(fd);
        }
        return;
    }
    std::vector<int> toClose;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(d)) != nullptr) {
        char* endp = nullptr;
        const long fd = std::strtol(ent->d_name, &endp, 10);
        if (endp == ent->d_name || *endp != '\0') continue;
        bool keepIt = fd <= 2;
        for (size_t i = 0; i < keepCount; ++i) {
            if (static_cast<long>(keep[i]) == fd) { keepIt = true; break; }
        }
        if (!keepIt) toClose.push_back(static_cast<int>(fd));
    }
    ::closedir(d);
    for (int fd : toClose) ::close(fd);
}

}  // namespace

// ---------------------------------------------------------------------------
// pimpl 本体（只有这一份 .cpp 看得见）
// ---------------------------------------------------------------------------

struct IpcEndpoint::Impl {
    int listenFd = -1;
    std::wstring token;        // "3"：继承描述符编号（execve 前 dup2 到位）
    std::string abstractName;  // SpawnChild 的孩子在 execve 前用它 connect
};

struct IpcChannel::Impl {
    int fd = -1;
};

struct ChildProcess::Impl {
    pid_t pid = -1;
    bool exited = false;
    int exitCode = -1;
    int stdoutReadFd = -1;   // captureStdio=true 时父进程一侧的排干口
    int stderrReadFd = -1;
};

// ---------------------------------------------------------------------------
// 三个包装类的生命周期（析构 / 移动；两份平台实现各自定义）
// ---------------------------------------------------------------------------

IpcEndpoint::~IpcEndpoint() { Close(); }
IpcEndpoint::IpcEndpoint(IpcEndpoint&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
IpcEndpoint& IpcEndpoint::operator=(IpcEndpoint&& o) noexcept {
    if (this != &o) { Close(); impl_ = o.impl_; o.impl_ = nullptr; }
    return *this;
}
bool IpcEndpoint::IsOpen() const { return impl_ && impl_->listenFd >= 0; }
const std::wstring& IpcEndpoint::Token() const {
    static const std::wstring kEmpty;
    return impl_ ? impl_->token : kEmpty;
}
void IpcEndpoint::Close() {
    if (impl_) {
        if (impl_->listenFd >= 0) ::close(impl_->listenFd);
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
bool IpcChannel::IsOpen() const { return impl_ && impl_->fd >= 0; }
void IpcChannel::Close() {
    if (impl_) {
        if (impl_->fd >= 0) ::close(impl_->fd);
        delete impl_;
        impl_ = nullptr;
    }
}

namespace {

// 子进程的最终收尾：没退就先按"父进程收尸"这一档收掉（绝不把僵尸留在自己的子进程表里），
// 再关掉父侧的排干口。析构与 Absorb 覆盖旧对象时都走这一条。
void DiscardChild(ChildProcess::Impl* impl) {
    if (!impl) return;
    if (impl->pid > 0 && !impl->exited) {
        ::kill(impl->pid, SIGKILL);
        ::killpg(impl->pid, SIGKILL);
        int status = 0;
        while (::waitpid(impl->pid, &status, 0) < 0 && errno == EINTR) {}
        impl->exited = true;
        impl->exitCode = static_cast<int>(kHelperExitReaped);
    }
    if (impl->stdoutReadFd >= 0) ::close(impl->stdoutReadFd);
    if (impl->stderrReadFd >= 0) ::close(impl->stderrReadFd);
    delete impl;
}

}  // namespace

ChildProcess::~ChildProcess() { DiscardChild(impl_); impl_ = nullptr; }
ChildProcess::ChildProcess(ChildProcess&& o) noexcept : impl_(o.impl_) { o.impl_ = nullptr; }
ChildProcess& ChildProcess::operator=(ChildProcess&& o) noexcept {
    if (this != &o) { DiscardChild(impl_); impl_ = o.impl_; o.impl_ = nullptr; }
    return *this;
}
void ChildProcess::Absorb(Impl* impl) { DiscardChild(impl_); impl_ = impl; }
uint64_t ChildProcess::Pid() const { return impl_ ? static_cast<uint64_t>(impl_->pid) : 0; }
bool ChildProcess::HasChild() const { return impl_ && impl_->pid > 0; }

// ---------------------------------------------------------------------------
// 服务入口
// ---------------------------------------------------------------------------

bool MakeNonce(uint64_t* out) {
    // /dev/urandom 交不出随机数就返回 false：没有 nonce 就不起辅助进程（与 Windows 同一条）。
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint8_t buf[8];
    size_t got = 0;
    while (got < 8) {
        const ssize_t n = ::read(fd, buf + got, 8 - got);
        if (n <= 0) { ::close(fd); return false; }
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    uint64_t v = 0;
    for (uint8_t b : buf) v = (v << 8) | b;
    if (v == 0) return false;   // 0 是保留值（"没有这笔交易"），概率可忽略，判掉最干净
    *out = v;
    return true;
}

bool CreateEndpoint(uint64_t nonce, IpcEndpoint* out, NativeErrorCode* err) {
    *err = kNativeErrorNone;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { *err = static_cast<NativeErrorCode>(errno); return false; }
    IpcEndpoint::Impl* impl = new (std::nothrow) IpcEndpoint::Impl();
    if (!impl) { ::close(fd); *err = static_cast<NativeErrorCode>(ENOMEM); return false; }
    impl->listenFd = fd;
    impl->abstractName = "ecapture-worker-" + std::to_string(static_cast<long>(::getpid())) +
                         "-" + HexNonce(nonce);
    impl->token = FromUtf8(std::to_string(kInheritedIpcFd));

    struct sockaddr_un addr{};
    socklen_t len = 0;
    if (!FillSockaddr(impl->abstractName, &addr, &len)) {
        ::close(fd); delete impl;
        *err = static_cast<NativeErrorCode>(ENAMETOOLONG);
        return false;
    }
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), len) != 0) {
        const int e = errno;
        ::close(fd); delete impl;
        *err = static_cast<NativeErrorCode>(e);
        return false;
    }
    if (::listen(fd, 1) != 0) {
        const int e = errno;
        ::close(fd); delete impl;
        *err = static_cast<NativeErrorCode>(e);
        return false;
    }
    out->Absorb(impl);
    return true;
}

bool SpawnChild(const SpawnOptions& opts, IpcEndpoint* endpoint, ChildProcess* out,
                NativeErrorCode* err) {
    *err = kNativeErrorNone;
    if (!endpoint->IsOpen() || !endpoint->Raw()) {
        *err = static_cast<NativeErrorCode>(EINVAL);
        return false;
    }
    const std::string exe = ToUtf8(opts.exePath);
    if (exe.empty()) { *err = static_cast<NativeErrorCode>(EINVAL); return false; }
    std::vector<std::string> argStrs;
    argStrs.push_back(exe);   // argv[0] 与被执行程序一致（与 Windows 的命令行约定同形）
    for (const std::wstring& a : opts.args) argStrs.push_back(ToUtf8(a));
    std::vector<char*> argv;
    argv.reserve(argStrs.size() + 1);
    for (std::string& s : argStrs) argv.push_back(s.data());
    argv.push_back(nullptr);

    // exec 握手：孩子 execve 成功时这个写端随 exec 关闭（CLOEXEC），父端立刻读到 EOF；
    // execve 失败时孩子把 errno 写回来再退。没有这条管道，"映像不存在"要等到 accept
    // 超时才暴露，而且原生码整个丢掉。
    int syncPipe[2] = {-1, -1};
    if (::pipe2(syncPipe, O_CLOEXEC) != 0) { *err = static_cast<NativeErrorCode>(errno); return false; }

    // captureStdio：父进程先建好两条管道，孩子只拿写端（写端 dup 进 1/2 后随 exec 留在孩子手里）。
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (opts.captureStdio) {
        if (::pipe2(outPipe, O_CLOEXEC) != 0) { *err = static_cast<NativeErrorCode>(errno); return false; }
        if (::pipe2(errPipe, O_CLOEXEC) != 0) {
            const int e = errno;
            ::close(outPipe[0]); ::close(outPipe[1]);
            *err = static_cast<NativeErrorCode>(e);
            return false;
        }
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        ::close(syncPipe[0]); ::close(syncPipe[1]);
        if (opts.captureStdio) {
            ::close(outPipe[0]); ::close(outPipe[1]);
            ::close(errPipe[0]); ::close(errPipe[1]);
        }
        *err = static_cast<NativeErrorCode>(e);
        return false;
    }
    if (pid == 0) {
        // ---- 子进程：fork 与 execve 之间只做 async-signal-safe 动作，失败一律 _exit ----
        ::close(syncPipe[0]);
        const pid_t originalParent = ::getppid();
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() != originalParent) _exit(kHelperExitProtocol);   // 竞态：父进程已经没了
        ::setpgid(0, 0);   // 独立进程组（组号 = 我的 PID）；与父进程侧的 setpgid 互为竞态保险
        const int conn = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (conn < 0) _exit(kHelperExitProtocol);
        struct sockaddr_un addr{};
        socklen_t len = 0;
        const std::string& name = endpoint->Raw()->abstractName;
        if (name.empty() || !FillSockaddr(name, &addr, &len)) _exit(kHelperExitProtocol);
        bool connected = false;
        for (int attempt = 0; attempt < kConnectRetrySlack; ++attempt) {
            if (::connect(conn, reinterpret_cast<struct sockaddr*>(&addr), len) == 0) {
                connected = true;
                break;
            }
            if (errno != ECONNREFUSED && errno != ENOENT) break;
            ::usleep(kConnectRetryStepUs);
        }
        if (!connected) _exit(kHelperExitProtocol);
        if (conn != kInheritedIpcFd) {
            if (::dup2(conn, kInheritedIpcFd) < 0) _exit(kHelperExitProtocol);
            ::close(conn);
        }
        // dup2 出来的 3 号不带 CLOEXEC —— 这正是"把端点交给 exec 后的辅助模式"的唯一通道。
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull < 0) _exit(kHelperExitProtocol);
        if (opts.captureStdio) {
            if (::dup2(outPipe[1], STDOUT_FILENO) < 0) _exit(kHelperExitProtocol);
            if (::dup2(errPipe[1], STDERR_FILENO) < 0) _exit(kHelperExitProtocol);
            if (::dup2(devnull, STDIN_FILENO) < 0) _exit(kHelperExitProtocol);
        } else {
            // 与 Windows 的 SilenceStandardHandles 同一意图：三条标准流都到 /dev/null，
            // 辅助进程写不到调用方的流上。
            if (::dup2(devnull, STDIN_FILENO) < 0) _exit(kHelperExitProtocol);
            if (::dup2(devnull, STDOUT_FILENO) < 0) _exit(kHelperExitProtocol);
            if (::dup2(devnull, STDERR_FILENO) < 0) _exit(kHelperExitProtocol);
        }
        ::close(devnull);
        // exec 握手的写端带着 O_CLOEXEC：execve 成功时它随 exec 关闭，父端读到 EOF；
        // 失败时它必须还开着，才能把 errno 写回去 —— 所以清场时把它留下。
        const int keep[5] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, kInheritedIpcFd,
                             syncPipe[1]};
        CloseAllExcept(keep, 5);
        ::execve(exe.c_str(), argv.data(), environ);
        // 只有 execve 失败才会走到这里：把原生码如实写回父端，再当场退出。
        const int reported = errno != 0 ? errno : ENOEXEC;
        const ssize_t ignored = ::write(syncPipe[1], &reported, sizeof(reported));
        (void)ignored;
        _exit(127);
    }

    // ---- 父进程 ----
    ::setpgid(pid, 0);   // 与孩子那侧互为竞态保险：谁先到都成立
    ::close(syncPipe[1]);
    if (opts.captureStdio) { ::close(outPipe[1]); ::close(errPipe[1]); }

    // 有界地等 exec 的下场（poll 而不是裸 read）：孩子卡在任何一步都不会把父进程拖成无限等。
    int execErr = 0;
    {
        struct pollfd p{};
        p.fd = syncPipe[0];
        p.events = POLLIN;
        int pr = ::poll(&p, 1, 5000);
        if (pr == 1) {
            ssize_t got = ::read(syncPipe[0], &execErr, sizeof(execErr));
            (void)got;
        } else if (pr < 0 && errno == EINTR) {
            pr = ::poll(&p, 1, 100);   // 被信号打断就再探一次
        }
        ::close(syncPipe[0]);
    }
    if (execErr != 0) {
        // execve 失败：把这一个当场收掉（组也是刚建的，只可能有它自己），不进入后续任何交易。
        ::kill(pid, SIGKILL);
        ::killpg(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (opts.captureStdio) { ::close(outPipe[0]); ::close(errPipe[0]); }
        *err = static_cast<NativeErrorCode>(execErr);
        return false;
    }

    ChildProcess::Impl* impl = new (std::nothrow) ChildProcess::Impl();
    if (!impl) {
        if (opts.captureStdio) { ::close(outPipe[0]); ::close(errPipe[0]); }
        ::kill(pid, SIGKILL);
        ::killpg(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        *err = static_cast<NativeErrorCode>(ENOMEM);
        return false;
    }
    impl->pid = pid;
    impl->stdoutReadFd = opts.captureStdio ? outPipe[0] : -1;
    impl->stderrReadFd = opts.captureStdio ? errPipe[0] : -1;
    out->Absorb(impl);
    return true;
}

bool AcceptChannel(IpcEndpoint* endpoint, uint64_t expectedPid, const Deadline& wait,
                   AcceptFault* fault, IpcChannel* out, NativeErrorCode* err) {
    *err = kNativeErrorNone;
    *fault = AcceptFault::kChannel;
    if (!endpoint->IsOpen()) { *err = static_cast<NativeErrorCode>(EINVAL); return false; }
    const int listenFd = endpoint->Raw()->listenFd;
    for (;;) {
        struct pollfd p{};
        p.fd = listenFd;
        p.events = POLLIN;
        const int ms = PollTimeoutMs(wait);
        const int r = ::poll(&p, 1, ms);
        if (r == 0) { *fault = AcceptFault::kTimeout; *err = static_cast<NativeErrorCode>(ETIMEDOUT); return false; }
        if (r < 0) {
            if (errno == EINTR) continue;
            *err = static_cast<NativeErrorCode>(errno);
            return false;
        }
        break;
    }
    const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) { *err = static_cast<NativeErrorCode>(errno); return false; }

    // 内核答的"是谁连的"——不是名字里写的，也不是对方自称的。
    struct ucred cred{};
    socklen_t credLen = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0) {
        const int e = errno;
        ::close(fd);
        *fault = AcceptFault::kPeerUnknown;
        *err = static_cast<NativeErrorCode>(e);
        return false;
    }
    if (static_cast<uint64_t>(cred.pid) != expectedPid ||
        static_cast<uint64_t>(cred.uid) != static_cast<uint64_t>(::getuid())) {
        ::close(fd);   // 伪造的连接：当场关掉，一个字节都不消费
        *fault = AcceptFault::kPeerMismatch;
        *err = static_cast<NativeErrorCode>(EACCES);
        return false;
    }
    IpcChannel::Impl* impl = new (std::nothrow) IpcChannel::Impl();
    if (!impl) {
        ::close(fd);
        *err = static_cast<NativeErrorCode>(ENOMEM);
        return false;
    }
    impl->fd = fd;
    // 单用途端点：监听口使命已完成（辅助进程用的是 accept 出来的连接），就地关闭。
    ::close(endpoint->Raw()->listenFd);
    endpoint->Raw()->listenFd = -1;
    out->Absorb(impl);
    return true;
}

bool ConnectToParent(const std::wstring& token, IpcChannel* out, NativeErrorCode* err) {
    // 子进程侧：端点已在 execve 前 dup2 到固定编号；这里核对它确实是条连好的 AF_UNIX 流。
    const std::string s = ToUtf8(token);
    if (s.empty() || s.size() > 7) { *err = static_cast<NativeErrorCode>(EINVAL); return false; }
    for (char c : s) {
        if (c < '0' || c > '9') { *err = static_cast<NativeErrorCode>(EINVAL); return false; }
    }
    const int fd = std::atoi(s.c_str());
    if (fd <= 2 || ::fcntl(fd, F_GETFD) < 0) {
        *err = static_cast<NativeErrorCode>(fd <= 2 ? EINVAL : errno);
        return false;
    }
    struct sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<struct sockaddr*>(&ss), &len) != 0) {
        *err = static_cast<NativeErrorCode>(errno);
        return false;
    }
    if (ss.ss_family != AF_UNIX) { *err = static_cast<NativeErrorCode>(EINVAL); return false; }
    IpcChannel::Impl* impl = new (std::nothrow) IpcChannel::Impl();
    if (!impl) { *err = static_cast<NativeErrorCode>(ENOMEM); return false; }
    impl->fd = fd;
    out->Absorb(impl);
    *err = kNativeErrorNone;
    return true;
}

bool EndpointTokenLooksOurs(const std::wstring& token) {
    // Linux 侧的令牌形状就是"十进制描述符编号"：非数字、0 或 1/2 这些不该出现的编号都算不合。
    const std::string s = ToUtf8(token);
    if (s.empty() || s.size() > 7) return false;
    long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    return v == kInheritedIpcFd;
}

PeerId ChannelPeer(const IpcChannel& channel) {
    PeerId p;
    if (!channel.IsOpen()) return p;
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(channel.Raw()->fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        p.pidKnown = true;
        p.pid = static_cast<uint64_t>(cred.pid);
        p.uidKnown = true;
        p.uid = static_cast<uint64_t>(cred.uid);
    }
    return p;
}

// ---------------------------------------------------------------------------
// 带期限 / 阻塞的字节搬运
// ---------------------------------------------------------------------------

ChannelIo WriteAll(IpcChannel* channel, const uint8_t* data, size_t size, const Deadline& wait) {
    if (!channel->IsOpen()) return FailErr(EBADF);
    const int fd = channel->Raw()->fd;
    size_t off = 0;
    while (off < size) {
        struct pollfd p{};
        p.fd = fd;
        p.events = POLLOUT;
        const int ms = PollTimeoutMs(wait);
        const int r = ::poll(&p, 1, ms);
        if (r == 0) return TimeoutIo();
        if (r < 0) {
            if (errno == EINTR) continue;
            return FailErr(errno);
        }
        const ssize_t n = ::send(fd, data + off, size - off, MSG_NOSIGNAL);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) return PeerClosedIo();
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) return PeerClosedIo();
        return FailErr(errno);
    }
    ChannelIo ok;
    ok.ok = true;
    return ok;
}

ChannelIo ReadExact(IpcChannel* channel, uint8_t* buf, size_t size, const Deadline& wait) {
    if (!channel->IsOpen()) return FailErr(EBADF);
    const int fd = channel->Raw()->fd;
    size_t off = 0;
    while (off < size) {
        struct pollfd p{};
        p.fd = fd;
        p.events = POLLIN;
        const int ms = PollTimeoutMs(wait);
        const int r = ::poll(&p, 1, ms);
        if (r == 0) return TimeoutIo();
        if (r < 0) {
            if (errno == EINTR) continue;
            return FailErr(errno);
        }
        const ssize_t n = ::read(fd, buf + off, size - off);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n == 0) return PeerClosedIo();   // 对端关闭
        if (errno == ECONNRESET) return PeerClosedIo();
        return FailErr(errno);
    }
    ChannelIo ok;
    ok.ok = true;
    return ok;
}

ChannelIo WriteAllBlocking(IpcChannel* channel, const uint8_t* data, size_t size) {
    // 未启用预算的 Deadline = poll(-1)：只在对端断开或看门狗到点时结束（见 HostProcess.h）。
    return WriteAll(channel, data, size, Deadline());
}

ChannelIo ReadExactBlocking(IpcChannel* channel, uint8_t* buf, size_t size) {
    return ReadExact(channel, buf, size, Deadline());
}

// ---------------------------------------------------------------------------
// 等待与收尾
// ---------------------------------------------------------------------------

bool WaitChild(const ChildProcess& child, uint32_t waitMs, bool* exited) {
    *exited = false;
    if (!child.HasChild()) return false;
    ChildProcess::Impl* impl = child.Raw();
    if (impl->exited) { *exited = true; return true; }
    const int64_t until = MonotonicClockMs() + static_cast<int64_t>(waitMs);
    for (;;) {
        int status = 0;
        const pid_t r = ::waitpid(impl->pid, &status, WNOHANG);
        if (r == impl->pid) {
            impl->exited = true;
            impl->exitCode = ExitCodeFromStatus(status);
            *exited = true;
            return true;
        }
        if (r < 0 && errno == ECHILD) {
            // 不归我收（例如已被别处收过）：如实"问不到退出码"，绝不编一个。
            impl->exited = true;
            impl->exitCode = -1;
            *exited = true;
            return true;
        }
        if (r < 0 && errno != EINTR) return false;
        if (MonotonicClockMs() >= until) return true;   // 等待到点而它还在：exited=false 照实交回
        ::usleep(2000);
    }
}

void ReapChild(ChildProcess* child, uint32_t waitMs, uint32_t reapedCode) {
    if (!child->HasChild()) return;
    ChildProcess::Impl* impl = child->Raw();
    bool exited = false;
    WaitChild(*child, waitMs, &exited);
    if (!exited) {
        // 只结束我起的这一个，外加我为它建的进程组（组号 == 它的 PID，里面只有我播种的进程）。
        ::kill(impl->pid, SIGKILL);
        ::killpg(impl->pid, SIGKILL);   // ESRCH（组已空）无害
        bool done = false;
        WaitChild(*child, 2000u, &done);
        if (!done) {
            int status = 0;
            while (::waitpid(impl->pid, &status, 0) < 0 && errno == EINTR) {}
            impl->exited = true;
            impl->exitCode = ExitCodeFromStatus(status);
        }
        // 被我们打死的：退出码记档换成"父进程收尸"那一档，与辅助进程自己的判断分得开
        // （core/HelperTiming.h 的约定；Linux 上信号本身带不回退出码，这一档由父进程写）。
        if (impl->exitCode == 128 + SIGKILL) impl->exitCode = static_cast<int>(reapedCode);
    }
    // 常规路径也要扫一遍组：辅助进程正常交回应答后，它留下的后代（如果有）不该继续在桌面上活着。
    // 这条对空组是 ESRCH，无害；组只可能是本次创建专属的那一个。
    ::killpg(impl->pid, SIGKILL);
}

int ChildExitCode(const ChildProcess& child) {
    return child.Raw() ? child.Raw()->exitCode : -1;
}

void FinishTransaction(ChildProcess* child, IpcChannel* channel, uint32_t reapWaitMs,
                       uint32_t drainGraceMs) {
    // Linux 的通道 I/O 全在 poll 预算里当场进终态，没有"取消后排不干净、需收养"的那一档
    //（那是 Windows OVERLAPPED 的事）；drainGraceMs 在这里没有对应的动作，如实收下不用。
    (void)drainGraceMs;
    if (channel->IsOpen()) channel->Close();
    if (child->HasChild()) ReapChild(child, reapWaitMs, static_cast<uint32_t>(kHelperExitReaped));
}

// ---------------------------------------------------------------------------
// stdout/stderr 并行排干（单 poll 循环同时看两条流，一条堵不住另一条）
// ---------------------------------------------------------------------------

bool CaptureChildStdio(ChildProcess* child, const Deadline& wait, std::string* outStdout,
                       std::string* outStderr, size_t capBytes, NativeErrorCode* err) {
    *err = kNativeErrorNone;
    if (outStdout) outStdout->clear();
    if (outStderr) outStderr->clear();
    ChildProcess::Impl* impl = child->Raw();
    if (!impl || (impl->stdoutReadFd < 0 && impl->stderrReadFd < 0)) {
        *err = static_cast<NativeErrorCode>(EINVAL);
        return false;
    }
    bool eofOut = impl->stdoutReadFd < 0;
    bool eofErr = impl->stderrReadFd < 0;
    uint8_t scratch[8192];
    while (!eofOut || !eofErr) {
        struct pollfd pfds[2];
        int fds[2] = {impl->stdoutReadFd, impl->stderrReadFd};
        bool* eofs[2] = {&eofOut, &eofErr};
        std::string* sinks[2] = {outStdout, outStderr};
        nfds_t n = 0;
        int idxOut = -1;
        int idxErr = -1;
        for (int i = 0; i < 2; ++i) {
            if (*eofs[i]) continue;
            if (i == 0) idxOut = static_cast<int>(n); else idxErr = static_cast<int>(n);
            pfds[n].fd = fds[i];
            pfds[n].events = POLLIN;
            pfds[n].revents = 0;
            ++n;
        }
        const int ms = PollTimeoutMs(wait);
        const int r = ::poll(pfds, n, ms);
        if (r == 0) { *err = static_cast<NativeErrorCode>(ETIMEDOUT); return false; }
        if (r < 0) {
            if (errno == EINTR) continue;
            *err = static_cast<NativeErrorCode>(errno);
            return false;
        }
        for (nfds_t i = 0; i < n; ++i) {
            int which = -1;
            if (static_cast<int>(i) == idxOut) which = 0;
            else if (static_cast<int>(i) == idxErr) which = 1;
            if (which < 0) continue;
            if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                const ssize_t got = ::read(pfds[i].fd, scratch, sizeof(scratch));
                if (got > 0) {
                    // 超过 capBytes 的部分照旧**排干但不留档** —— 排干是"不堵孩子"的手段，
                    // 留档上限只是父进程自己的内存边界。
                    if (sinks[which]) {
                        const size_t room = capBytes - sinks[which]->size();
                        const size_t take = got > static_cast<ssize_t>(room)
                                                ? room : static_cast<size_t>(got);
                        sinks[which]->append(reinterpret_cast<const char*>(scratch), take);
                    }
                } else if (got == 0) {
                    *eofs[which] = true;   // EOF
                } else if (errno != EINTR) {
                    *eofs[which] = true;   // 读不通按流终局处理，错误原样在下面报
                }
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 身份与存活问答
// ---------------------------------------------------------------------------

bool SelfImagePath(std::wstring* out) {
    char buf[8192];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    *out = FromUtf8(std::string(buf, static_cast<size_t>(n)));
    return true;
}

bool ImagePathOf(uint64_t pid, std::wstring* out) {
    if (pid == 0 || pid > static_cast<uint64_t>(INT32_MAX)) return false;
    char link[64];
    std::snprintf(link, sizeof(link), "/proc/%llu/exe", static_cast<unsigned long long>(pid));
    char buf[8192];
    const ssize_t n = ::readlink(link, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    *out = FromUtf8(std::string(buf, static_cast<size_t>(n)));
    return true;
}

bool ProcessAlive(uint64_t pid) {
    if (pid == 0 || pid > static_cast<uint64_t>(INT32_MAX)) return false;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%llu/stat", static_cast<unsigned long long>(pid));
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[512];
    const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    // /proc/<pid>/stat 的字段 3 是状态；comm 段可能带空格，先定位最后一个 ')' 再往后一格。
    // 僵尸（'Z'）不算活着 —— 它已经退场，只是还没人收尸。
    const char* p = std::strrchr(buf, ')');
    if (!p) return false;
    ++p;
    while (*p == ' ') ++p;
    return *p != '\0' && *p != 'Z';
}

bool SameLoginSession(uint64_t pid) {
    if (pid == 0 || pid > static_cast<uint64_t>(INT32_MAX)) return false;
    const pid_t mine = ::getsid(0);
    const pid_t theirs = ::getsid(static_cast<pid_t>(pid));
    if (mine == static_cast<pid_t>(-1) || theirs == static_cast<pid_t>(-1)) return false;
    return mine == theirs;
}

uint64_t ImmediateParentPid() { return static_cast<uint64_t>(::getppid()); }

bool ParentCleanupArmed() {
    int sig = 0;
    if (::prctl(PR_GET_PDEATHSIG, &sig) != 0) return false;
    return sig != 0;
}

}  // namespace hostproc
}  // namespace ecapture
