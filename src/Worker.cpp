#define _CRT_RAND_S   // 必须在 <stdlib.h> 之前定义，rand_s 才会被声明
#include "Worker.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <tlhelp32.h>

#include <winrt/base.h>   // 辅助任务里也可能抛出 hresult_error，要换成 ASCII 细节交回

#include "CaptureDwm.h"
#include "CapturePrintWindow.h"
#include "ImageOps.h"
#include "Lang.h"
#include "WorkerIo.h"
#include "WorkerProtocol.h"

namespace ecapture {
namespace {

using namespace worker;

// 工作模式标记。刻意写成"单斜杠 + 一个不会被当成选项的名字"，并且不写进选项目录：
// 正常命令行解析遇到它会报 cli.unknown_option，只有 wmain 最前面那一次判断才认它。
constexpr const wchar_t* kWorkerSwitch = L"/ecapture-worker";

// 管道名的固定前缀：辅助进程按名字形状 + nonce + 是谁起的我，三重核对。
constexpr const wchar_t* kPipePrefix = L"\\\\.\\pipe\\ecapture-worker-";

constexpr uint32_t kPipeBufferBytes = 64u * 1024u;
constexpr uint32_t kReapGraceMs = 2000u;    // 交回结果后给辅助进程的收尾时间
// 请求取消之后，愿意多等一次"确认终态"的宽限（毫秒）。它**不占**自动处理预算：
// 预算在那次超时上已经花掉了，这一段纯粹是把 CancelIoEx 的"已排入取消"兑现成
// "操作确实结束"，否则 OVERLAPPED/事件/缓冲就成了悬空引用。有这条上限，
// 就不许有人把它说成"无限等待"；也不许有人拿"预算到点"当理由跳过它。
constexpr uint32_t kCancelDrainGraceMs = 2000u;
// 上面这两段是"父进程愿意多等一会儿"的收尾宽限，不是期限；四类期限本身
// （总预算 / 默认隔离上限 / 启动握手段 / 辅助进程执行段）的分工与换算都在 WorkerTiming.h。
// 辅助进程自己那两段的等待上限用 GetTickCount64 数：它不受显示语言、时区与校时影响，
// 而执行段那一截本来就带 kDeliverGraceMs 抵消与父进程 QPC 时钟之间的粒度差。

// 一次隔离调用的结局。五类失败给五条不同的诊断：调用方要能分清
// "辅助进程没起来"、"起来了却没在握手段之内接上管道"、"接上了而中途断掉"、
// "消息形状不对"和"到点了还没结果"。前四类是本工具自己的机制故障（共用
// capture.worker_failed），最后一条才是期限真的烧光（match.timeout / capture.timeout）
// —— 把前者说成后者，就是教调用方去加大一个根本不解这个问题的数字。
enum class Call { kDone, kTimedOut, kSpawnFailed, kHandshakeFailed, kChannelFailed, kProtocolFailed };

uint64_t MakeNonce() {
    // rand_s 是加密质量的随机数；拿不到时用 QPC + PID + 栈地址兜底（同一次运行里仍唯一，
    // 只是不再"猜不到"，所以那条兜底只用于把管道名区分开，不作为安全边界）。
    uint32_t a = 0, b = 0;
    if (rand_s(&a) != 0) return 0;
    if (rand_s(&b) != 0) return 0;
    LARGE_INTEGER tick{};
    QueryPerformanceCounter(&tick);
    uint64_t n = (static_cast<uint64_t>(a) << 32) | b;
    n ^= static_cast<uint64_t>(tick.QuadPart);
    n ^= (static_cast<uint64_t>(GetCurrentProcessId()) << 16);
    n ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&a));
    return n;
}

std::wstring HexOf(uint64_t v) {
    wchar_t buf[24];
    swprintf(buf, 24, L"%016llX", static_cast<unsigned long long>(v));
    return buf;
}

bool HexToValue(const std::wstring& text, uint64_t* out) {
    if (text.empty() || text.size() > 16) return false;
    uint64_t v = 0;
    for (wchar_t c : text) {
        const wchar_t low = std::towlower(c);
        const int d = iswdigit(c) ? (c - L'0') : (low - L'a' + 10);
        if (d < 0 || d > 15) return false;
        v = v * 16ull + static_cast<uint64_t>(d);
    }
    *out = v;
    return true;
}

// Windows argv 的引号规则（与 tests\harness.psm1 的 Format-EcWindowsArgv 同一套）：
// 只在真的需要时加引号，引号前的反斜杠加倍，结尾的反斜杠也加倍。
// 这里递出去的三条取值（管道名、十六进制 nonce、十进制 PID）都不含空格，
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

std::string AsciiFromWide(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) {
        const unsigned int u = static_cast<unsigned int>(c);
        if (u < 128) out.push_back(static_cast<char>(u));
        else out.push_back('?');   // 协议里不该出现非 ASCII；出现就明说，而不是悄悄丢字节
    }
    return out;
}

std::wstring WideFromAscii(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (const char c : s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return w;
}

// ---------------------------------------------------------------------------
// 句柄的小包装：每条失败路径都要把它们关掉，尤其是作业句柄 ——
// 辅助进程的"父进程一退就一起结束"是靠关闭这个句柄实现的。
// ---------------------------------------------------------------------------
class OwnedHandle {
public:
    OwnedHandle() = default;
    ~OwnedHandle() { Close(); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    void Reset(HANDLE h) { Close(); h_ = h; }
    void Close() { if (h_) { CloseHandle(h_); h_ = nullptr; } }
    // 放弃所有权、把句柄原样交出去：只用于"仍有未决异步 I/O 挂在它上面，提前关闭
    // 是文档不保证的行为"的收尾场景（见 Transaction::Finish 的注释）。
    HANDLE Release() { const HANDLE h = h_; h_ = nullptr; return h; }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// ---------------------------------------------------------------------------
// 带期限的管道 I/O
// ---------------------------------------------------------------------------
// 状态机本体在 WorkerIo.{h,cpp}：那里保证"确认 I/O 进入终态之后才释放 OVERLAPPED、
// 事件与内核触碰的缓冲"，本层只把它翻成交易用的结局。
// 实际搬运的字节数只能取 GetOverlappedResult 的那份（由状态机负责）：
// ReadFile/WriteFile 的"写了几个字节"出参在异步完成路径上不作承诺
// （按它判断会把"读完了"误认成"对方关了管道"，实测就是这个坑）。
using Step = workerio::IoStep;

// ReadFile/WriteFile/ConnectNamedPipe 的 BOOL + GetLastError 约定 -> 发起阶段结局：
// 映射本体在 WorkerIo（StartOf / ConnectStartOf），生产与判据共用的就是这四态的判据。
// 重叠调用返回非 0 是"同步完成"，此时事件已由系统置起，后续等待会立即返回。

Step WriteAll(HANDLE pipe, const uint8_t* data, size_t size, const Deadline& dl) {
    size_t offset = 0;
    while (offset < size) {
        const uint32_t chunk =
            static_cast<uint32_t>(std::min<size_t>(size - offset, kPipeBufferBytes));
        const uint8_t* at = data + offset;
        // 内核只从操作对象自有的缓冲里读：调用方的 framed 缓冲区在任何超时路径上
        // 都可以安全失效，不会因为一次没赶上的写请求而变成悬空引用。
        const Step s = workerio::RunOverlappedOp(
            workerio::Win32Backend(), pipe, chunk, WaitTimeout(dl), kCancelDrainGraceMs,
            [&](OVERLAPPED& ov, uint8_t* buf, uint32_t cap) {
                std::memcpy(buf, at, chunk);
                return workerio::StartOf(WriteFile(pipe, buf, cap, nullptr, &ov));
            },
            nullptr);
        if (!s.done) return s;
        offset += s.transferred;
    }
    Step ok;
    ok.done = true;
    return ok;
}

Step ReadExact(HANDLE pipe, uint8_t* buf, size_t size, const Deadline& dl) {
    size_t offset = 0;
    while (offset < size) {
        const uint32_t chunk =
            static_cast<uint32_t>(std::min<size_t>(size - offset, kPipeBufferBytes));
        // 读取同样落进操作对象的缓冲，确认终态后才复制回调用方内存：
        // 一次超时的挂起读不会再往调用方已经释放的堆块里写像素。
        std::vector<uint8_t> got;
        const Step s = workerio::RunOverlappedOp(
            workerio::Win32Backend(), pipe, chunk, WaitTimeout(dl), kCancelDrainGraceMs,
            [&](OVERLAPPED& ov, uint8_t* dst, uint32_t cap) {
                return workerio::StartOf(ReadFile(pipe, dst, cap, nullptr, &ov));
            },
            &got);
        if (!s.done) return s;
        if (s.transferred == 0) {
            // 0 字节 = 对方已经把写完的那一段交回并关了管道：这次调用拿不到应答
            Step broken;
            broken.gle = ERROR_BROKEN_PIPE;
            return broken;
        }
        std::memcpy(buf + offset, got.data(), s.transferred);
        offset += s.transferred;
    }
    Step ok;
    ok.done = true;
    ok.transferred = offset;
    return ok;
}

// ---------------------------------------------------------------------------
// 一次交易：建管道 -> 建作业 -> 起辅助进程 -> 交任务 -> 收应答 -> 收尸
// ---------------------------------------------------------------------------
class Transaction {
public:
    Transaction() = default;
    ~Transaction() { Finish(); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Call Start(uint16_t kind, const std::vector<uint8_t>& task, const Deadline& dl);
    Call ReadReply(Reply* out, uint16_t expectKind, const Deadline& dl);
    DWORD Win32() const { return gle_; }

    // 收尸：结束/等待辅助进程并关掉作业与管道。析构里还会再调一次（幂等），
    // 但调用方要在读退出码**之前**先收一次，否则拿到的是 STILL_ACTIVE(259) 而不是它真正的结局。
    void Reap() { Finish(); }

    // 辅助进程最后的退出码（拿不到时给 -1）。管道断了的时候，这个数比
    // "ERROR_BROKEN_PIPE" 有用得多：它说的是"它自己拒绝执行、崩了、还是被人结束了"。
    // 值是在 Finish() 里等过之后取走的，所以调用方必须先收尸再问（见 Transact 里的大括号）。
    int ChildExit() const { return childExit_; }

private:
    void Finish();

    std::wstring pipeName_;
    uint64_t nonce_ = 0;
    uint16_t kind_ = 0;
    OwnedHandle pipe_;
    OwnedHandle job_;
    OwnedHandle process_;
    OwnedHandle thread_;
    DWORD childPid_ = 0;
    DWORD gle_ = 0;
    int childExit_ = -1;
};

Call Transaction::Start(uint16_t kind, const std::vector<uint8_t>& task, const Deadline& dl) {
    kind_ = kind;
    nonce_ = MakeNonce();
    if (nonce_ == 0) {
        gle_ = ERROR_NO_TOKEN;   // 拿不到随机数就没法把这次交易与别的区分开：不起进程
        return Call::kSpawnFailed;
    }
    pipeName_ = std::wstring(kPipePrefix) + std::to_wstring(GetCurrentProcessId()) + L"-" +
                HexOf(nonce_);

    // 只开一份实例，且带 FILE_FLAG_FIRST_PIPE_INSTANCE：同名已经存在就直接失败，
    // 不去接一个别人提前建好、"等着我交任务"的服务端。
    HANDLE pipe = CreateNamedPipeW(pipeName_.c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                       FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, kPipeBufferBytes, kPipeBufferBytes, NMPWAIT_USE_DEFAULT_WAIT,
                                   nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        gle_ = GetLastError();
        return Call::kSpawnFailed;
    }
    pipe_.Reset(pipe);

    // 作业带 KILL_ON_JOB_CLOSE：父进程不管是正常退出还是被人结束，句柄一关，
    // 内核就把辅助进程一起带走。"辅助进程长期遗留"这件事因此不依赖我们自己记得收尾。
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        gle_ = GetLastError();
        return Call::kSpawnFailed;
    }
    job_.Reset(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // KILL_ON_CLOSE 是"不遗留"的机制；ACTIVE_PROCESS=1 与不许 breakaway 是"辅助进程里
    // 不会再长出一个不受这份作业管的进程"。BREAKAWAY_OK / SILENT_BREAKAWAY_OK 都不给，
    // 所以它自己也没法逃出这份作业。
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        gle_ = GetLastError();
        return Call::kSpawnFailed;
    }

    wchar_t self[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (n == 0 || n >= std::size(self)) {
        gle_ = GetLastError();
        return Call::kSpawnFailed;
    }
    // 任务本体走管道，命令行只带"从哪儿取任务"的坐标：
    // 辅助进程能读到的参数里没有输出路径，也没有任何能被执行的文字。
    std::wstring cmd = QuoteArg(std::wstring(self, n)) + L" " + kWorkerSwitch + L" " + pipeName_ +
                       L" " + HexOf(nonce_) + L" " + std::to_wstring(GetCurrentProcessId());
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // 先挂起再进作业：否则辅助进程有几微秒不属于任何作业，
    // 那条"父进程退了我就没电"的保证就有窗口期能漏掉。
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        gle_ = GetLastError();
        return Call::kSpawnFailed;
    }
    process_.Reset(pi.hProcess);
    thread_.Reset(pi.hThread);
    childPid_ = pi.dwProcessId;

    if (!AssignProcessToJobObject(job_.get(), process_.get())) {
        gle_ = GetLastError();
        // 挂不进作业就绝不放它跑：那条"父进程退了我就没电"的保证对它不成立，只能当场收回。
        TerminateProcess(process_.get(), static_cast<uint32_t>(kHelperExitReaped));
        return Call::kSpawnFailed;
    }
    ResumeThread(thread_.get());

    // 把管道交出去之前先确认"连上来的是我刚起的那个进程"：
    // 名字里带了本次的 PID 与 nonce，但"是谁连的"要由内核回答，不能靠名字。
    // 连接有四类结局，逐类分开判（见 workerio::ConnectStartOf）：
    //   * 已连接（ERROR_PIPE_CONNECTED）：辅助进程赶在 ConnectNamedPipe 之前就连上了。
    //     这是一次成立且没有挂起 I/O —— 状态机就地进完成态，直接往下走任务交换，
    //     绝不等那个不会由系统置起的事件（等它就是"抢先连接被误报成超时"）；
    //   * 同步成功：事件已由系统置起，等待立即返回；
    //   * ERROR_IO_PENDING：确有挂起操作，按剩余预算等，到点按 F02 的机制安全取消；
    //   * 其他错误：如实上报通道故障，不做免检。
    // 连接成功与否都不重新领一份预算：这一段花掉的仍然是那一份总预算里的毫秒。
    // 连管这一步只肯等"启动/握手"那一段（HandshakeWaitMs 已经把它压进这一笔交易的剩余预算之内）。
    // 把整份剩余预算压在连管上有两个错：一个根本没起来的辅助进程会烧光用户的期限，
    // 而且报出来的还是"期限耗尽"——调用方照这个码只会去加大 --timeout-ms，而加大它不解这件事。
    const Step connect = workerio::RunOverlappedOp(
        workerio::Win32Backend(), pipe_.get(), 0, HandshakeWaitMs(dl), kCancelDrainGraceMs,
        [&](OVERLAPPED& ov, uint8_t*, uint32_t) {
            return workerio::ConnectStartOf(ConnectNamedPipe(pipe_.get(), &ov));
        },
        nullptr);
    if (connect.timedOut) {
        // 同一次"没等到连接"分两种下场，各归各的类：预算真的花完了才算期限耗尽，
        // 否则算"辅助进程没接上来"这条机制故障。
        if (ClassifyHandshakeTimeout(dl) == HandshakeFault::kBudgetSpent) return Call::kTimedOut;
        gle_ = ERROR_TIMEOUT;   // 握手段到点，预算还剩着：下面翻成 handshake_failed 那条诊断
        return Call::kHandshakeFailed;
    }
    if (!connect.done) { gle_ = connect.gle; return Call::kChannelFailed; }

    DWORD peerPid = 0;
    if (!GetNamedPipeClientProcessId(pipe_.get(), &peerPid)) {
        gle_ = GetLastError();
        return Call::kProtocolFailed;
    }
    if (peerPid != childPid_) {
        gle_ = ERROR_INVALID_WINDOW_HANDLE;   // 连上来的不是我起的那个：任务一个字节都不交
        return Call::kProtocolFailed;
    }

    Header h{kMagic, kProtocolVersion, kind, static_cast<uint32_t>(task.size()), nonce_};
    std::vector<uint8_t> framed;
    if (!EncodeHeader(h, &framed)) { gle_ = ERROR_BAD_FORMAT; return Call::kProtocolFailed; }
    framed.insert(framed.end(), task.begin(), task.end());

    const Step w = WriteAll(pipe_.get(), framed.data(), framed.size(), dl);
    if (w.timedOut) return Call::kTimedOut;
    if (!w.done) { gle_ = w.gle; return Call::kChannelFailed; }
    return Call::kDone;
}

Call Transaction::ReadReply(Reply* out, uint16_t expectKind, const Deadline& dl) {
    std::vector<uint8_t> header(kHeaderSize);
    const Step hs = ReadExact(pipe_.get(), header.data(), header.size(), dl);
    if (hs.timedOut) return Call::kTimedOut;
    if (!hs.done) { gle_ = hs.gle; return Call::kChannelFailed; }

    Header h{};
    if (!DecodeHeader(header.data(), header.size(), &h)) {
        gle_ = ERROR_BAD_FORMAT;
        return Call::kProtocolFailed;
    }
    // nonce 与 kind 都对上才算"这是我这次要的答案"。对不上的应答（哪怕内容看着合法）
    // 一律按协议不合处理，绝不拿别人的结果去填这一次的目标，也不填半条。
    if (h.nonce != nonce_ || h.kind != expectKind || h.kind != kind_) {
        gle_ = ERROR_INVALID_DATA;
        return Call::kProtocolFailed;
    }

    std::vector<uint8_t> payload(h.payloadLen);
    if (!payload.empty()) {
        const Step ps = ReadExact(pipe_.get(), payload.data(), payload.size(), dl);
        if (ps.timedOut) return Call::kTimedOut;
        if (!ps.done) { gle_ = ps.gle; return Call::kChannelFailed; }
    }
    if (!DecodeReply(payload.data(), payload.size(), out)) {
        gle_ = ERROR_BAD_FORMAT;
        return Call::kProtocolFailed;
    }
    return Call::kDone;
}

void Transaction::Finish() {
    // 顺序是固定的：先收尾辅助进程，再关作业句柄。
    // 关作业句柄会触发 KILL_ON_JOB_CLOSE，那是"我没能好好收尸"时的兜底，不是常规手段。
    //
    // 管道句柄什么时候能关，取决于登记表里有没有挂着它的未决异步操作：
    //   * 没有（一切成功路径，外加绝大多数超时/取消 —— 状态机在返回前就已排干）：
    //     照旧先关管道、提示辅助进程退出，不多等一次；
    //   * 有（取消已受理但宽限内等不到完成，属于驱动级卡死的病理场景）：带着未决 I/O
    //     关句柄是文档不保证的行为，所以先收尸 —— 对面进程一没，飞行中的管道 I/O
    //     基本立刻能进终态 —— 再让登记表确认一次。确认得了就关；确认不了就把这个
    //     句柄连同它的 OVERLAPPED/事件/缓冲一起交给登记表收养，随进程退出由系统回收。
    //     代价是一份有界缓冲与一个句柄的遗留，换来的是不再有"内核往已释放内存写"。
    if (pipe_ && !workerio::HasPendingOps(pipe_.get())) {
        pipe_.Close();
    }
    if (process_) {
        if (WaitForSingleObject(process_.get(), kReapGraceMs) != WAIT_OBJECT_0) {
            // 只结束我自己起的这一个进程；退出码写成"父进程收尸"这一档，
            // 好让 hint 里的数与辅助进程自己的判断分得开。
            TerminateProcess(process_.get(), static_cast<uint32_t>(kHelperExitReaped));
            WaitForSingleObject(process_.get(), 1000);
        }
    }
    DWORD code = 0;
    childExit_ = (process_ && GetExitCodeProcess(process_.get(), &code)) ? static_cast<int>(code) : -1;
    process_.Close();
    thread_.Close();
    if (pipe_) {
        if (workerio::DrainPendingOps(pipe_.get(), kCancelDrainGraceMs) == 0) {
            pipe_.Close();
        } else {
            // 收养成不成功都要把关闭责任交出去：这条句柄上还挂着没确认终态的操作，
            // 由 Transaction 自己关它仍然是文档不保证的行为。收养没记下来（登记表的节点
            // 分配不出来）时就让它随进程遗留 —— 代价是一个句柄值，换掉的是一次未定义行为。
            // 这三个入口都不抛出（见 WorkerIo.h），这条收尾路径不会把异常带进析构函数。
            static_cast<void>(workerio::AdoptHandle(pipe_.get()));
            pipe_.Release();   // 关闭责任移交登记表（登记表在操作全数终态后替它关）
        }
    }
    job_.Close();
}

// ---------------------------------------------------------------------------
// 结局 / 状态码 -> 诊断
// ---------------------------------------------------------------------------
Diagnostic CallToDiagnostic(Call call, const Deadline& dl, const wchar_t* timeoutCode,
                            const wchar_t* stage, const wchar_t* backend, DWORD gle, int childExit) {
    if (call == Call::kTimedOut) return BudgetSpent(dl, timeoutCode, stage, backend);

    Diagnostic d;
    // 这四类失败都是"本工具自己的执行环境坏了"（起不来 / 起来了却没连上来 / 管道断 / 消息不合），
    // 与"目标窗口不肯给"、"通道取不到画面"、"期限真的烧光"是几种不同的下一步。它们共用
    // capture.worker_failed 这一条稳定码（调用方据此分支"机制故障"），文案各说各的那一段。
    d.code = codes::kWorkerFailed;
    d.option = L"--capture";
    d.value = backend ? backend : std::wstring();
    d.backend = d.value;
    d.stage = stage;
    d.win32 = gle;
    switch (call) {
        case Call::kSpawnFailed:
            d.message = Msg(L"cap.worker.spawn_failed");
            d.hint = Msgf(L"cap.worker.spawn_failed_hint", Win32ErrorText(gle));
            break;
        case Call::kHandshakeFailed:
            // 进程起来了（管道都建好了），但它在握手段之内没把连接建起来。
            // 这条绝不是 capture.timeout：加大 --timeout-ms 对一件根本没发生的事没有帮助。
            d.message = Msg(L"cap.worker.handshake_failed");
            d.hint = Msgf(L"cap.worker.handshake_failed_hint", kHandshakeMs);
            break;
        case Call::kChannelFailed:
            d.message = Msg(L"cap.worker.channel_failed");
            // %1 是辅助进程最后的退出码：60 起步 = 它按绑定校验拒绝执行，
            // 1 = 被人结束（ExitProcess(1) 之外的正常退出不会是 1... 见 RunWorkerMode）
            d.hint = Msgf(L"cap.worker.channel_failed_hint", childExit);
            break;
        case Call::kProtocolFailed:
            d.message = Msg(L"cap.worker.protocol_failed");
            d.hint = Msg(L"cap.worker.protocol_failed_hint");
            break;
        default:
            d.message = Msg(L"cap.no_detail");
            break;
    }
    return d;
}

}  // namespace

Diagnostic BlockedToDiagnostic(BlockedStatus status, DWORD gle, HRESULT hr,
                               const std::string& detail, const wchar_t* backend,
                               const wchar_t* stage) {
    Diagnostic d;
    d.code = codes::kCaptureFailed;
    d.option = L"--capture";
    d.value = backend ? backend : std::wstring();
    d.backend = d.value;
    d.stage = stage;
    d.win32 = gle;
    if (FAILED(hr)) d.hresult = HResultText(hr);

    switch (status) {
        case BlockedStatus::kRectEmpty:
            d.code = codes::kWindowGone;
            d.message = Msg(L"cap.rect_empty_draw");
            d.hint = Msg(L"cap.window_gone");
            break;
        case BlockedStatus::kDibCreate:
            d.message = Msg(L"cap.create_dib_section");
            d.hint = Msgf(L"err.win32_code", gle);
            break;
        case BlockedStatus::kPrintWindowFailed:
            d.message = Msg(L"cap.pw.failed");
            d.hint = Msgf(L"cap.pw.failed_hint", Win32ErrorText(gle));
            break;
        case BlockedStatus::kHostPrintWindowFailed:
            d.message = Msg(L"cap.dwm.pw_failed");
            d.hint = Msgf(L"err.win32_code", gle);
            break;
        case BlockedStatus::kHostClass:
            d.message = Msg(L"cap.dwm.register_class");
            d.hint = Msgf(L"err.win32_code", gle);
            break;
        case BlockedStatus::kHostCreate:
            d.message = Msg(L"cap.dwm.create_host");
            d.hint = Msgf(L"err.win32_code", gle);
            break;
        case BlockedStatus::kRegisterThumb:
            d.message = Msg(L"cap.dwm.register_thumb");
            d.hint = Msgf(L"cap.dwm.register_thumb_hint", Msgf(L"cap.hresult", HResultText(hr)));
            break;
        case BlockedStatus::kUpdateProps:
            d.message = Msg(L"cap.dwm.update_props");
            d.hint = Msgf(L"cap.hresult", HResultText(hr));
            break;
        case BlockedStatus::kHostRectEmpty:
            d.message = Msg(L"cap.dwm.host_zero");
            break;
        case BlockedStatus::kRegexInvalid:
            // 本机正则库拒绝这条模式——语法就是在这里（受约束的匹配执行层）第一次判的，
            // 解析层不构造正则。码沿用 cli.invalid_regex、退出码仍是 1，stage/backend 说 match。
            d.code = codes::kInvalidRegex;
            d.option = L"--title-regex";
            d.value = std::wstring();
            d.message = Msg(L"cli.regex_late");
            d.hint = Msg(L"cli.regex_hint") + L" (" + WideFromAscii(detail) + L")";
            break;
        case BlockedStatus::kRegexTooComplex:
            // 语法没错，是这台机器的正则库不肯把它跑完。加大 --timeout-ms 不会有帮助
            // （它不是慢，是被上限挡下），所以文案要指着"改模式"说，而不是"再等久一点"。
            d.code = codes::kInvalidRegex;
            d.option = L"--title-regex";
            d.value = std::wstring();
            d.message = Msg(L"cli.regex_too_complex");
            d.hint = Msgf(L"cli.regex_too_complex_hint", WideFromAscii(detail));
            break;
        case BlockedStatus::kBadTask:
            d.code = codes::kWorkerFailed;
            d.message = Msg(L"cap.worker.bad_task");
            d.hint = Msg(L"cap.worker.bad_task_hint");
            break;
        case BlockedStatus::kInternal:
            d.message = Msgf(L"err.exception", WideFromAscii(detail));
            break;
        case BlockedStatus::kOk:
        default:
            d.message = Msg(L"cap.no_detail");
            break;
    }
    return d;
}

// IsolatedWaitFor / HandshakeWaitMs / TaskBudgetMs / 辅助进程自己的阶段模型
// 都在 WorkerTiming.cpp：那几条是纯换算，离线判据判的就是它们本体。

namespace {

// 一次完整的隔离调用：起辅助进程、交任务、收应答。失败原因已经翻成本地化诊断。
Call Transact(uint16_t kind, Task task, const Deadline& dl, Reply* reply, Diagnostic* err,
              const wchar_t* timeoutCode, const wchar_t* stage, const wchar_t* backend) {
    // 随任务交下去的那一笔预算 = 这一笔交易此刻还剩的预算（换算与判界见 WorkerTiming.h）。
    // 取"剩余"而不是"总额"：一次运行仍然只有一份预算，辅助进程不会在这里重新领到一整份。
    task.budgetMs = TaskBudgetMs(dl);
    std::vector<uint8_t> payload;
    if (!EncodeTask(task, &payload)) {
        *err = CallToDiagnostic(Call::kProtocolFailed, dl, timeoutCode, stage, backend,
                                ERROR_BAD_FORMAT, -1);
        return Call::kProtocolFailed;
    }
    Transaction tx;
    Call call = tx.Start(kind, payload, dl);
    if (call == Call::kDone) call = tx.ReadReply(reply, kind, dl);
    if (call != Call::kDone) {
        // 先收尸再问退出码：顺序反过来拿到的是"还在跑"，不是它到底为什么走
        const DWORD gle = tx.Win32();
        tx.Reap();
        *err = CallToDiagnostic(call, dl, timeoutCode, stage, backend, gle, tx.ChildExit());
    }
    return call;
}

// 应答里那条帧搬到 CapturedFrame 上。source / path 由父进程盖（父进程知道这次走的是哪条通道，
// 而辅助进程只负责把像素搬回来）。
bool FrameFromReply(const Reply& reply, const wchar_t* backend, CapturedFrame* out,
                    Diagnostic* err) {
    if (reply.status != BlockedStatus::kOk) {
        *err = BlockedToDiagnostic(reply.status, reply.win32, reply.hresult,
                                   AsciiFromWide(reply.detail), backend,
                                   stages::kCapture);
        return false;
    }
    if (reply.width == 0 || reply.height == 0 || reply.pixels.empty()) {
        *err = BlockedToDiagnostic(BlockedStatus::kOk, reply.win32, S_OK, std::string(), backend,
                                   stages::kCapture);
        return false;
    }
    // 辅助进程那头的消息格式已经限过一次单边与整帧字节数（WorkerProtocol.h），这里按**帧自己的
    // 形状**再核一次：行距装不装得下一行像素、缓冲区够不够 stride*height。少这一道，
    // 后面裁剪与编码都按调用方给的形状算偏移，帧一坏就是读越界。
    const FrameShapeInfo intent{reply.width, reply.height, reply.stride,
                                 static_cast<uint64_t>(reply.pixels.size())};
    const FrameShape shape = CheckFrameShape(intent);
    if (shape != FrameShape::kOk) {
        FrameShapeError(shape, intent, backend, stages::kCapture, err);
        return false;
    }
    out->width = reply.width;
    out->height = reply.height;
    out->stride = reply.stride;
    out->pixels = reply.pixels;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 父进程侧的三个入口
// ---------------------------------------------------------------------------

bool IsolatedPrintWindow(uint64_t hwnd, const Deadline& dl, CapturedFrame* out, Diagnostic* err) {
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    Task task;
    task.kind = kTaskPrintWindow;
    task.hwnd = hwnd;
    Reply reply;
    const Deadline wait = IsolatedWaitFor(dl);
    if (Transact(task.kind, task, wait, &reply, err, codes::kCaptureTimeout, stages::kCapture,
                 L"printwindow") != Call::kDone) {
        return false;
    }
    if (!FrameFromReply(reply, L"printwindow", out, err)) return false;
    out->source = L"printwindow";
    out->path = paths::kPrintWindow;
    return true;
}

bool IsolatedDwmThumbnail(uint64_t hwnd, uint32_t waitMs, const Deadline& dl, CapturedFrame* out,
                          Diagnostic* err) {
    out->pixels.clear();
    out->width = out->height = out->stride = 0;

    Task task;
    task.kind = kTaskDwmThumbnail;
    task.hwnd = hwnd;
    task.waitMs = waitMs;
    Reply reply;
    const Deadline wait = IsolatedWaitFor(dl);
    if (Transact(task.kind, task, wait, &reply, err, codes::kCaptureTimeout, stages::kCapture,
                 L"dwm") != Call::kDone) {
        return false;
    }
    if (!FrameFromReply(reply, L"dwm", out, err)) return false;
    out->source = L"dwm";
    out->path = paths::kDwmThumbnail;
    return true;
}

bool IsolatedMatch(const MatchOptions& match, const std::vector<RECT>& onScreens,
                   const Deadline& dl, std::vector<WindowInfo>* hits,
                   std::vector<WindowInfo>* iconic, Diagnostic* err) {
    Task task;
    task.kind = kTaskMatchWindows;
    task.match = match;
    task.onScreens = onScreens;
    Reply reply;
    const Deadline wait = IsolatedWaitFor(dl);
    const Call call =
        Transact(task.kind, task, wait, &reply, err, codes::kMatchTimeout, stages::kMatch, L"match");
    if (call != Call::kDone) return false;
    if (reply.status != BlockedStatus::kOk) {
        *err = BlockedToDiagnostic(reply.status, reply.win32, reply.hresult,
                                   AsciiFromWide(reply.detail), L"match",
                                   stages::kMatch);
        return false;
    }
    *hits = std::move(reply.hits);
    *iconic = std::move(reply.iconic);
    return true;
}

// ---------------------------------------------------------------------------
// 辅助进程侧
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

std::wstring SelfPath() {
    wchar_t buffer[8192] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) return std::wstring();
    return std::wstring(buffer, n);
}

bool ProcessAlive(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE;
    CloseHandle(proc);
    return alive;
}

bool SameSession(DWORD pid) {
    DWORD mine = 0, theirs = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &mine)) return false;
    if (!ProcessIdToSessionId(pid, &theirs)) return false;
    return mine == theirs;
}

// 我的直接父进程是谁：这条判断和"父进程说它是谁"是两个独立来源，两者要一致。
// 只对 CreateToolhelp32Snapshot 的整张表负责，拿不到就当作不成立（宁可拒绝执行）。
DWORD ImmediateParentPid() {
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
    return parent;
}

// "我在一个'父进程一关作业就一起结束'的作业里" —— 这条既是"不会长期遗留"的机制本身，
// 也是辅助进程肯干活的前提之一：正常shell里直接跑辅助模式时通常没有这么一份作业。
bool InKillOnCloseJob() {
    BOOL inJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob) return false;
    JOBOBJECT_BASIC_LIMIT_INFORMATION basic{};
    if (!QueryInformationJobObject(nullptr, JobObjectBasicLimitInformation, &basic, sizeof(basic),
                                   nullptr)) {
        return false;
    }
    return (basic.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
}

bool PipeNameLooksOurs(const std::wstring& name) {
    const std::wstring prefix(kPipePrefix);
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return false;
    const std::wstring tail = name.substr(prefix.size());
    if (tail.size() > 40) return false;
    bool dash = false;
    for (const wchar_t c : tail) {
        if (c == L'-') { if (dash) return false; dash = true; continue; }
        if (!iswdigit(c) && !std::iswalpha(c)) return false;
    }
    return dash;
}

struct WorkerArgs {
    std::wstring pipe;
    uint64_t nonce = 0;
    DWORD parentPid = 0;
};

bool ParseWorkerArgs(int argc, wchar_t* const* argv, WorkerArgs* out) {
    // 位置参数，且一条不多一条不少：多出来或少了参数，说明发起方用的是别的（或更新的）约定，
    // 这里不猜它想干什么。
    if (argc != 5 || !argv) return false;
    out->pipe = argv[2];
    if (!HexToValue(argv[3], &out->nonce) || out->nonce == 0) return false;
    const std::wstring pidText = argv[4];
    if (pidText.empty() || pidText.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
    const unsigned long pid = std::wcstoul(pidText.c_str(), nullptr, 10);
    if (pid == 0 || pid == 0xFFFFFFFFul) return false;
    out->parentPid = static_cast<DWORD>(pid);
    return true;
}

// 绑定校验：五件事全成立才肯干活。任一条不成立就拒绝执行，且不写任何输出。
// 返回值是给排障用的内部编号（0 = 通过）。辅助模式的退出码不是对外契约的一部分，
// 所以这里把"哪一条不成立"编进退出码里（60 起步），而不是一条笼统的"拒绝"。
int BindingFailureStep(const WorkerArgs& a) {
    if (!PipeNameLooksOurs(a.pipe)) return 1;
    const DWORD myPid = GetCurrentProcessId();
    if (a.parentPid == 0 || a.parentPid == myPid) return 2;
    if (!ProcessAlive(a.parentPid)) return 3;
    if (!SameSession(a.parentPid)) return 4;
    // 起我的必须是同一个 ECAPTURE.EXE —— 这条把"任何人都能直接跑辅助模式"挡在门外
    const std::wstring parent = ModulePathOf(a.parentPid);
    const std::wstring self = SelfPath();
    if (parent.empty() || self.empty()) return 5;
    if (_wcsicmp(parent.c_str(), self.c_str()) != 0) return 6;
    if (ImmediateParentPid() != a.parentPid) return 7;
    if (!InKillOnCloseJob()) return 8;
    return 0;
}

void SetInternalFailure(Reply* reply) {
    std::string detail;
    DetailFromCurrentException(&detail);
    reply->status = BlockedStatus::kInternal;
    reply->detail = WideFromAscii(detail);
    reply->hresult = S_OK;
    reply->width = reply->height = reply->stride = 0;
    reply->pixels.clear();
    reply->hits.clear();
    reply->iconic.clear();
}

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

bool ReadTaskBlocking(HANDLE pipe, uint64_t nonce, Task* task, uint16_t* kind) {
    uint8_t header[kHeaderSize] = {};
    if (!ReadAllBlocking(pipe, header, sizeof(header))) return false;
    Header h{};
    if (!DecodeHeader(header, sizeof(header), &h)) return false;
    if (h.nonce != nonce) return false;   // 不是交给我这次的
    if (h.payloadLen > kMaxTaskPayload) return false;
    std::vector<uint8_t> payload(h.payloadLen);
    if (!payload.empty() && !ReadAllBlocking(pipe, payload.data(), payload.size())) return false;
    if (!DecodeTask(payload.data(), payload.size(), task)) return false;
    if (task->kind != h.kind) return false;   // 报头说一套、任务里写一套
    *kind = h.kind;
    return true;
}

// 执行一条任务。三条任务都是"读某个窗口自己的画面"或"把窗口列一遍"：
// 没有一条会读桌面像素，所以这一步不需要、也不允许有任何授权旁路。
void RunTask(const Task& task, Reply* reply) {
    switch (task.kind) {
        case kTaskPrintWindow: {
            const RenderOutcome o = RenderPrintWindowContent(task.hwnd);
            reply->status = o.status;
            reply->win32 = o.win32;
            reply->hresult = o.hresult;
            reply->detail = WideFromAscii(o.detail);
            reply->width = o.frame.width;
            reply->height = o.frame.height;
            reply->stride = o.frame.stride;
            reply->pixels = std::move(o.frame.pixels);
            return;
        }
        case kTaskDwmThumbnail: {
            const RenderOutcome o = RenderDwmThumbnailContent(task.hwnd, task.waitMs);
            reply->status = o.status;
            reply->win32 = o.win32;
            reply->hresult = o.hresult;
            reply->detail = WideFromAscii(o.detail);
            reply->width = o.frame.width;
            reply->height = o.frame.height;
            reply->stride = o.frame.stride;
            reply->pixels = std::move(o.frame.pixels);
            return;
        }
        case kTaskMatchWindows: {
            MatchRequest req;
            req.match = task.match;
            req.onScreens = task.onScreens;
            const MatchOutcome m = EnumerateMatches(req);
            reply->status = m.status;
            reply->detail = WideFromAscii(m.detail);
            reply->hits = std::move(m.hits);
            reply->iconic = std::move(m.iconic);
            return;
        }
        default:
            reply->status = BlockedStatus::kBadTask;
            return;
    }
}

// "一个字节都没能交回"那条出口：绑定校验之外的一切管道/协议故障都走这里。
// 退出码只给排障的人看（父进程把它写进 hint），不是对外契约（见 Worker.h）。
[[noreturn]] void ExitWithoutReply() { ExitProcess(static_cast<uint32_t>(kHelperExitProtocol)); }

// ---------------------------------------------------------------------------
// 辅助进程自己的兜底看门狗：把 WorkerTiming.h 那条三段期限的纯算式接在真线程与真时钟上。
// 它挡的是"父进程已经卡死、没人来收尸"这一种遗留。父进程正常在等的那一段不会被它掐掉：
// 执行段用的那一笔预算取的是父进程交出任务**之前**的剩余量，而它从看到任务才开始数，
// 所以它只会晚于父进程自己放弃的那一刻，永远不会更早。
// 三段里只有"等任务"那一段与预算无关（那一刻手里还没有预算可谈），它的上限就是 kHandshakeMs，
// 与父进程肯等连接的时长是同一个常数 —— 同一段含义、两个观察者、一个数，不是两套互相覆盖的常量。
// ---------------------------------------------------------------------------

// 看门狗线程与主线程之间共享的只有这两样：当前阶段，以及进入执行段时发布的那笔预算。
// 用静态存储而不是线程参数：RunWorkerMode 返回到进程真正退出之间还有一小段，把参数放在调用方
// 栈上等于让那条线程往已经失效的栈上读。这两个数没有生命周期问题，也不出本进程。
std::atomic<HelperPhase> g_helperPhase{HelperPhase::kAwaitTask};
std::atomic<uint32_t> g_helperBudgetMs{0};

DWORD CALLBACK HelperWatchdogProc(LPVOID) {
    HelperPhase phase = HelperPhase::kAwaitTask;
    uint64_t phaseTick = GetTickCount64();   // 进入当前这一段时的读数：换段就重新锚一次
    for (;;) {
        const HelperPhase observed = g_helperPhase.load(std::memory_order_acquire);
        if (observed != phase) {
            // 重新锚点用的是"看到"的那一刻，可能比主线程"进入"的那一刻晚一个小轮询间隔。
            // 晚只会让辅助进程比自己该退的时候多活一会儿，方向是安全的（早才会误杀）。
            phase = observed;
            phaseTick = GetTickCount64();
        }
        HelperLimits limits;
        // 这里再夹一次上限：协议解码已经判过界，这一道防的是"哪怕交下来的数被改坏，
        // 辅助进程也不会拿到一个无界的自尽期限"。这几毫秒不进任何对外承诺。
        limits.taskBudgetMs = std::min<uint64_t>(g_helperBudgetMs.load(std::memory_order_relaxed),
                                                 kMaxWorkerBudgetMs);
        const HelperVerdict verdict =
            JudgeHelperWatchdog(phase, limits, GetTickCount64() - phaseTick);
        if (verdict.exitNow) {
            // 到点就把整个辅助进程结束掉：结束的是我自己，不是目标应用
            // （目标窗口只是收到过一次没人等的绘制请求，它自己不受影响）。
            ExitProcess(static_cast<uint32_t>(verdict.exitCode));
        }
        if (verdict.pollMs == 0) return 0;   // 应答已经交回：本进程正在自己退出，这条线程收工
        Sleep(verdict.pollMs);
    }
}

void RunHelperWatchdog() {
    // 只用 Win32 建这条线程，不用 std::thread：后者在建不出来时抛 std::system_error，而 worker
    // 模式最开头没有东西接得住它，没人接就是 terminate。建不出来当场退出 —— 宁可这一次隔离调用
    // 失败，也不带着"没有人在计时"的状态去等一个可能永远不来的任务。
    const HANDLE thread = CreateThread(nullptr, 0, HelperWatchdogProc, nullptr, 0, nullptr);
    if (!thread) ExitWithoutReply();
    CloseHandle(thread);   // 关句柄不影响线程本身，进程活着时它就一直数到自己那一段的期限
}

// 任务到手 -> 执行段；应答交回（或交回失败）-> 不再挂任何自尽期限。
// 先写预算再写阶段（release），看门狗按 acquire 读阶段：它看到新阶段时，预算一定已经看得见。
// 收尾用 RAII：RunWorkerMode 里每一条 return 与异常退栈都算"交回这一步已经走完"，
// 不需要人在每个出口前记得补一句，也不会在还没交回时就先把时钟拆掉。
class DeliveryScope {
public:
    explicit DeliveryScope(uint32_t budgetMs) {
        g_helperBudgetMs.store(budgetMs, std::memory_order_relaxed);
        g_helperPhase.store(HelperPhase::kRunning, std::memory_order_release);
    }
    ~DeliveryScope() {
        g_helperPhase.store(HelperPhase::kDelivered, std::memory_order_release);
    }
    DeliveryScope(const DeliveryScope&) = delete;
    DeliveryScope& operator=(const DeliveryScope&) = delete;
};

// 辅助进程不许往调用方的标准流写一个字节（父进程可能正用同一条 stdout 交付 PNG）。
// 这里把三条标准句柄换成 NUL，而**不**用 FreeConsole()：实测从 ConPTY 型终端（Windows Terminal
// 与现在这些 AI 终端）里起的子进程调 FreeConsole() 会不定期卡在 csrss 上 —— 20 次里 4~10 次，
// 一卡就是几十秒，而那时候看门狗线程都还没来得及建。换 NUL 是一条不经过控制台子系统的本地操作，
// 而且挡得更严：连"以后某条生成路径把句柄传下来"也写不到调用方的流上。
// 任何一步没换成就当场退出，绝不带着原句柄继续干活。
void SilenceStandardHandles() {
    const DWORD kinds[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (size_t i = 0; i < 3; ++i) {
        const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
        if (nul == INVALID_HANDLE_VALUE) ExitWithoutReply();
        if (!SetStdHandle(kinds[i], nul)) ExitWithoutReply();
        // 这份句柄就此交给进程生命周期：不 CloseHandle（关了等于把刚设好的标准句柄也关掉，
        // 之后一次误写反而可能落到别的对象上）。辅助进程由 ExitProcess 一次带走所有句柄。
    }
    // CRT 的 stdin/stdout/stderr 在进程启动时就把句柄抄进了自己的 fd 表，只换 Win32 那份不够
    FILE* replaced = nullptr;
    if (freopen_s(&replaced, "NUL", "r", stdin) != 0 || freopen_s(&replaced, "NUL", "w", stdout) != 0 ||
        freopen_s(&replaced, "NUL", "w", stderr) != 0) {
        ExitWithoutReply();
    }
}

}  // namespace

bool LooksLikeWorkerInvocation(const wchar_t* firstArg) {
    return firstArg && std::wcscmp(firstArg, kWorkerSwitch) == 0;
}

int RunWorkerMode(int argc, wchar_t* const* argv) {
    // 第一件事是给自己装上计时器：从这里往后的任何一步卡住，都有人负责把进程结束掉。
    // 装上的那条线只数"这一段的期限到了没有"，各段管什么见上面的看门狗与 WorkerTiming.h。
    RunHelperWatchdog();
    // 唯一的输出通道是那条绑定过的管道；标准流先接到 NUL（原因见 SilenceStandardHandles）。
    SilenceStandardHandles();

    WorkerArgs args;
    if (!ParseWorkerArgs(argc, argv, &args)) return 60;
    const int bindingStep = BindingFailureStep(args);
    if (bindingStep != 0) return 60 + bindingStep;

    HANDLE pipe = CreateFileW(args.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) ExitWithoutReply();

    Task task;
    uint16_t kind = 0;
    if (!ReadTaskBlocking(pipe, args.nonce, &task, &kind)) {
        CloseHandle(pipe);
        ExitWithoutReply();
    }

    // 任务到手：看门狗从"等任务那一段"换到"执行那一段"，执行段的期限就是父进程随任务
    // 交下来的那一笔剩余预算（DecodeTask 已经判过 [1, 上限]）加上交回宽限。
    // 握手段那 30 秒到这一刻让位 —— 一笔用户显式接受的 60 秒任务，不会在第 30 秒被自己掐掉。
    // 作用域结束（含异常退栈）就把阶段记为"应答已经交回"，此后不再挂任何自尽期限。
    const DeliveryScope delivery{task.budgetMs};

    // 矩形坐标必须与物理像素一致，否则 PrintWindow 之后的裁剪会截偏（与父进程同一套规矩）
    EnsureDpiAware();

    Reply reply;
    try {
        RunTask(task, &reply);
    } catch (const winrt::hresult_error& e) {
        SetInternalFailure(&reply);
        reply.hresult = e.code();
    } catch (...) {
        SetInternalFailure(&reply);
    }

    std::vector<uint8_t> payload;
    if (!EncodeReply(reply, &payload)) {
        // 结果太大或形状不自洽（比如窗口被拉到 3 万像素宽）：交回一条"任务不合法"的空应答，
        // 父进程据此报错，而不是等一个永远不会来的帧。
        Reply tiny;
        tiny.status = BlockedStatus::kBadTask;
        if (!EncodeReply(tiny, &payload)) {
            CloseHandle(pipe);
            return kHelperExitProtocol;
        }
    }
    const Header h{kMagic, kProtocolVersion, kind, static_cast<uint32_t>(payload.size()),
                   args.nonce};
    std::vector<uint8_t> framed;
    if (!EncodeHeader(h, &framed)) {
        CloseHandle(pipe);
        return kHelperExitProtocol;
    }
    framed.insert(framed.end(), payload.begin(), payload.end());

    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written,
                              nullptr);
    CloseHandle(pipe);
    return (ok && written == framed.size()) ? 0 : kHelperExitProtocol;
}

}  // namespace ecapture
