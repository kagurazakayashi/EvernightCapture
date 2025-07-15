#define _CRT_RAND_S   // 必须在 <stdlib.h> 之前定义，rand_s 才会被声明
#include "Worker.h"

#include <algorithm>
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
#include "Lang.h"
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
// 辅助进程没人交任务（或卡住）时自己退出的时限。必须明显大于 kMaxTaskWaitMs：
// 父进程还在等的那条任务不能被这道兜底掐掉，否则"期限还没到，结果先没了"。
constexpr uint32_t kWorkerIdleMs = 30000u;

// 一次隔离调用的结局。四类失败给四条不同的诊断：调用方要能分清
// "辅助进程没起来"、"起来了但管道没接上"、"接上了而消息不对"和"到点了还没结果"。
enum class Call { kDone, kTimedOut, kSpawnFailed, kChannelFailed, kProtocolFailed };

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
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// ---------------------------------------------------------------------------
// 带期限的管道 I/O
// ---------------------------------------------------------------------------
struct Step {
    bool done = false;
    bool timedOut = false;
    DWORD gle = 0;
    // 实际搬运的字节数。重叠 I/O 下这个数只能取 GetOverlappedResult 的那份：
    // ReadFile/WriteFile 的"写了几个字节"出参在异步完成路径上不作承诺
    // （按它判断会把"读完了"误认成"对方关了管道"，实测就是这个坑）。
    size_t transferred = 0;
};

// 发一个重叠 I/O 并等它，等待本身盯着剩余预算。
// 期限到点就 CancelIoEx 之后走人：不"先等到有结果再判断是不是超时"，那条路根本没有期限可言。
template <typename Op>
Step RunOverlapped(HANDLE handle, const Deadline& dl, Op op) {
    Step step;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        step.gle = GetLastError();
        return step;
    }
    const BOOL started = op(&ov);
    if (!started) {
        const DWORD gle = GetLastError();
        if (gle != ERROR_IO_PENDING) {
            step.gle = gle;
            CloseHandle(ov.hEvent);
            return step;
        }
    }
    const DWORD wait = WaitForSingleObject(ov.hEvent, WaitTimeout(dl));
    if (wait == WAIT_OBJECT_0) {
        DWORD transferred = 0;
        if (GetOverlappedResult(handle, &ov, &transferred, FALSE)) {
            step.done = true;
            step.transferred = transferred;
        } else {
            step.gle = GetLastError();
        }
    } else if (wait == WAIT_TIMEOUT) {
        step.timedOut = true;
        CancelIoEx(handle, &ov);
    } else {
        step.gle = GetLastError();
    }
    CloseHandle(ov.hEvent);
    return step;
}

Step WriteAll(HANDLE pipe, const uint8_t* data, size_t size, const Deadline& dl) {
    size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - offset, kPipeBufferBytes));
        const uint8_t* at = data + offset;
        const Step s = RunOverlapped(pipe, dl, [&](LPOVERLAPPED ov) {
            return WriteFile(pipe, at, chunk, nullptr, ov);
        });
        if (!s.done) return s;
        offset += s.transferred;
    }
    return Step{true, false, 0, 0};
}

Step ReadExact(HANDLE pipe, uint8_t* buf, size_t size, const Deadline& dl) {
    size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - offset, kPipeBufferBytes));
        const Step s = RunOverlapped(pipe, dl, [&](LPOVERLAPPED ov) {
            return ReadFile(pipe, buf + offset, chunk, nullptr, ov);
        });
        if (!s.done) return s;
        if (s.transferred == 0) {
            // 0 字节 = 对方已经把写完的那一段交回并关了管道：这次调用拿不到应答
            return Step{false, false, ERROR_BROKEN_PIPE, 0};
        }
        offset += s.transferred;
    }
    return Step{true, false, 0, offset};
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
        TerminateProcess(process_.get(), 9);   // 是我起的进程，结束它不碰任何人家的
        return Call::kSpawnFailed;
    }
    ResumeThread(thread_.get());

    // 把管道交出去之前先确认"连上来的是我刚起的那个进程"：
    // 名字里带了本次的 PID 与 nonce，但"是谁连的"要由内核回答，不能靠名字。
    const Step connect = RunOverlapped(pipe_.get(), dl, [&](LPOVERLAPPED ov) {
        const BOOL r = ConnectNamedPipe(pipe_.get(), ov);
        return r == TRUE || GetLastError() == ERROR_PIPE_CONNECTED;
    });
    if (connect.timedOut) return Call::kTimedOut;
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
    pipe_.Close();
    if (process_) {
        if (WaitForSingleObject(process_.get(), kReapGraceMs) != WAIT_OBJECT_0) {
            TerminateProcess(process_.get(), 9);   // 只结束我自己起的这一个进程
            WaitForSingleObject(process_.get(), 1000);
        }
    }
    DWORD code = 0;
    childExit_ = (process_ && GetExitCodeProcess(process_.get(), &code)) ? static_cast<int>(code) : -1;
    process_.Close();
    thread_.Close();
    job_.Close();
}

// ---------------------------------------------------------------------------
// 结局 / 状态码 -> 诊断
// ---------------------------------------------------------------------------
Diagnostic CallToDiagnostic(Call call, const Deadline& dl, const wchar_t* timeoutCode,
                            const wchar_t* stage, const wchar_t* backend, DWORD gle, int childExit) {
    if (call == Call::kTimedOut) return BudgetSpent(dl, timeoutCode, stage, backend);

    Diagnostic d;
    // 这三类失败都是"本工具自己的执行环境坏了"（起不来 / 管道断 / 消息不合），
    // 与"目标窗口不肯给"、"通道取不到画面"是三种不同的下一步，所以各给一条码。
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
            d.code = codes::kInvalidRegex;
            d.option = L"--title-regex";
            d.value = std::wstring();
            d.message = Msg(L"cli.regex_late");
            d.hint = WideFromAscii(detail);
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

Deadline IsolatedWaitFor(const Deadline& dl) {
    // 给了 --timeout-ms 就照剩余预算等（用户明确要多少就给他多少，不再另加一道内置上限）；
    // 没给就用内置上限 —— 这条上限顶替的正是过去被忽略的那个等待参数。
    return dl.Enabled() ? dl : Deadline::FromTotalMs(kIsolatedCallMs);
}

namespace {

// 一次完整的隔离调用：起辅助进程、交任务、收应答。失败原因已经翻成本地化诊断。
Call Transact(uint16_t kind, const Task& task, const Deadline& dl, Reply* reply, Diagnostic* err,
              const wchar_t* timeoutCode, const wchar_t* stage, const wchar_t* backend) {
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

// 兜底看门狗：管道断了、父进程被结束、或者卡在任何一步上太久，自己都退出。
// 有了它，"辅助进程遗留"就不只依赖作业那一条机制。
DWORD CALLBACK IdleWatchdogProc(LPVOID) {
    Sleep(kWorkerIdleMs);
    ExitProcess(9);   // 到点就把整个辅助进程结束掉：这条线程不存在"正常返回"
}

void RunIdleWatchdog() {
    // 只用 Win32 建这条线程，不用 std::thread：后者在建不出来时抛 std::system_error，而 worker
    // 模式最开头没有东西接得住它，没人接就是 terminate。建不出来当场退出 —— 宁可这一次隔离调用
    // 失败，也不带着"没有人在计时"的状态去等一个可能永远不来的任务。
    const HANDLE thread = CreateThread(nullptr, 0, IdleWatchdogProc, nullptr, 0, nullptr);
    if (!thread) ExitProcess(9);
    CloseHandle(thread);   // 关句柄不影响线程本身，进程活着时它就一直睡到点
}

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
        if (nul == INVALID_HANDLE_VALUE) ExitProcess(9);
        if (!SetStdHandle(kinds[i], nul)) ExitProcess(9);
        // 这份句柄就此交给进程生命周期：不 CloseHandle（关了等于把刚设好的标准句柄也关掉，
        // 之后一次误写反而可能落到别的对象上）。辅助进程由 ExitProcess 一次带走所有句柄。
    }
    // CRT 的 stdin/stdout/stderr 在进程启动时就把句柄抄进了自己的 fd 表，只换 Win32 那份不够
    FILE* replaced = nullptr;
    if (freopen_s(&replaced, "NUL", "r", stdin) != 0 || freopen_s(&replaced, "NUL", "w", stdout) != 0 ||
        freopen_s(&replaced, "NUL", "w", stderr) != 0) {
        ExitProcess(9);
    }
}

}  // namespace

bool LooksLikeWorkerInvocation(const wchar_t* firstArg) {
    return firstArg && std::wcscmp(firstArg, kWorkerSwitch) == 0;
}

int RunWorkerMode(int argc, wchar_t* const* argv) {
    // 第一件事是给自己装上计时器：从这里往后的任何一步卡住，都有人负责把进程结束掉。
    RunIdleWatchdog();
    // 唯一的输出通道是那条绑定过的管道；标准流先接到 NUL（原因见 SilenceStandardHandles）。
    SilenceStandardHandles();

    WorkerArgs args;
    if (!ParseWorkerArgs(argc, argv, &args)) return 60;
    const int bindingStep = BindingFailureStep(args);
    if (bindingStep != 0) return 60 + bindingStep;

    HANDLE pipe = CreateFileW(args.pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 9;

    Task task;
    uint16_t kind = 0;
    if (!ReadTaskBlocking(pipe, args.nonce, &task, &kind)) {
        CloseHandle(pipe);
        return 9;
    }

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
            return 9;
        }
    }
    const Header h{kMagic, kProtocolVersion, kind, static_cast<uint32_t>(payload.size()),
                   args.nonce};
    std::vector<uint8_t> framed;
    if (!EncodeHeader(h, &framed)) {
        CloseHandle(pipe);
        return 9;
    }
    framed.insert(framed.end(), payload.begin(), payload.end());

    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, framed.data(), static_cast<DWORD>(framed.size()), &written,
                              nullptr);
    CloseHandle(pipe);
    return (ok && written == framed.size()) ? 0 : 9;
}

}  // namespace ecapture
