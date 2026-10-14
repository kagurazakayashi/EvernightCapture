// 跨平台辅助进程通信与超时取消的 Linux 离线判据。
//
// 判的是生产本体（不是测试里抄的规则）：
//   * src/platform/linux/PosixHostProcess.cpp —— fork/execve 直启、abstract socket 端点、
//     SO_PEERCRED 核验、PDEATHSIG/进程组收尾、单调预算内的读写、并行 stdout/stderr 排干、
//     身份与存活问答（辅助进程绑定校验的原料也走这一份）；
//   * src/core/WorkerFraming.cpp —— 报头编解码、协议版本与尺寸判界（父进程与辅助进程
//     收发的就是这一份本体）；
//   * src/core/HelperTiming.cpp —— 辅助进程自己的三段期限算式（看门狗线程直接调它）；
//   * src/Deadline.cpp + HostPath 的 MonotonicClockMs —— 剩余预算的领取与传递；
//   * src/ImageOps.cpp 的 CheckFrameShape —— 应答帧形状的核对。
// 任务/应答的**载荷**是测试自己的小约定（12 字节作业 + 定形应答），只走生产字节原语；
// 生产载荷编解码（含 RECT / DWORD / HRESULT 的那一份）仍留在 WorkerProtocol.h 的
// 过渡桥接里，去平台类型是后续步骤 —— 本判据不拿它自证，也不为它造第二份实现。
//
// 辅助进程是本测试 exe 自己的 --worker 模式（与 Windows"父进程与辅助进程是同一个 exe"同形）；
// 不启动、不触碰任何用户窗口与桌面内容：阻塞用合成动作，取消只打本次创建的进程组。
// 全程只用 /proc 与内存字节，不写任何文件。
//
// 汇报只用 printf；结尾一行 "worker-ipc: N 条通过，M 条失败"；有失败则退出码非 0。
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/Deadline.h"
#include "../src/HostPath.h"
#include "../src/HostProcess.h"
#include "../src/ImageOps.h"
#include "../src/core/HelperTiming.h"
#include "../src/core/TextCoding.h"
#include "../src/core/WorkerFraming.h"

using namespace ecapture;
using namespace ecapture::worker;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
    std::fflush(stdout);
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); std::fflush(stdout); }

std::wstring W(const char* utf8) { return DecodeUtf8(utf8, std::strlen(utf8)).text; }
std::string S(const std::wstring& w) { return EncodeUtf8(w); }

std::wstring HexOf(uint64_t v) {
    wchar_t buf[24];
    std::swprintf(buf, 24, L"%016llX", static_cast<unsigned long long>(v));
    return buf;
}

bool HexToValue(const std::wstring& text, uint64_t* out) {
    if (text.empty() || text.size() > 16) return false;
    uint64_t v = 0;
    for (wchar_t c : text) {
        if (c >= L'0' && c <= L'9') v = v * 16 + static_cast<uint64_t>(c - L'0');
        else if (c >= L'a' && c <= L'f') v = v * 16 + static_cast<uint64_t>(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') v = v * 16 + static_cast<uint64_t>(c - L'A' + 10);
        else return false;
    }
    *out = v;
    return true;
}

void Msleep(uint64_t ms) {
    const struct timespec ts{static_cast<time_t>(ms / 1000),
                             static_cast<long>((ms % 1000) * 1000000L)};
    nanosleep(&ts, nullptr);
}

// ---------------------------------------------------------------------------
// 测试自己的载荷约定（走生产字节原语；kind=1 固定，应答原样回声）
// ---------------------------------------------------------------------------
constexpr uint16_t kJobKind = 1;
constexpr uint32_t kJobBytes = 12u;   // action + param + budgetMs

enum JobAction : uint32_t {
    kJobEcho = 0,              // 立刻回一条 2x2 的合成帧
    kJobBlock = 1,             // 睡 param 毫秒再回（param=60000 即"卡死"）
    kJobBadExit = 2,           // _exit(param)
    kJobCrash = 3,             // raise(SIGSEGV)
    kJobDescendantBlock = 4,   // fork 一个睡 30 秒的后代，然后自己卡死
    kJobBulkStdio = 5,         // 各写 param 字节到 stdout 与 stderr，再回帧
    kJobGarbageReply = 6,      // 交回一坨连报头都不合的字节
    kJobNoReply = 7,           // 一声不响地退出 0
    kJobBulkFrame = 8,         // 回一帧 param*1024 字节的合成图（走流式通道）
    kJobLateReply = 9,         // 睡过父进程期限之后再试着交回
};

struct Job {
    uint32_t action = 0;
    uint32_t param = 0;
    uint32_t budgetMs = 0;
};

struct JobReply {
    bool decodeOk = false;
    uint32_t status = 0;       // 0 = 正常
    uint32_t echoBudget = 0;   // 解码到的随任务预算（原样回声）
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t selfPid = 0;
    std::vector<uint8_t> pixels;
};

bool EncodeJob(const Job& j, std::vector<uint8_t>* out) {
    out->clear();
    PutU32(out, j.action);
    PutU32(out, j.param);
    PutU32(out, j.budgetMs);
    return out->size() == kJobBytes;
}

// 与生产 DecodeTask 同一纪律：预算 [1, kMaxWorkerBudgetMs]（0 不是"不限"，是"没有期限"）、
// 字节必须正好读完 —— 多一字节少一字节都整条作废。上限常量用的是 WorkerFraming 那一份。
bool DecodeJob(const uint8_t* data, size_t size, Job* out) {
    if (!data || size != kJobBytes) return false;
    const uint8_t* p = data;
    size_t left = size;
    Job j;
    if (!TakeU32(&p, &left, &j.action)) return false;
    if (!TakeU32(&p, &left, &j.param)) return false;
    if (!TakeU32(&p, &left, &j.budgetMs)) return false;
    if (left != 0) return false;
    if (j.budgetMs == 0 || j.budgetMs > kMaxWorkerBudgetMs) return false;
    if (j.action > kJobLateReply) return false;
    *out = j;
    return true;
}

bool EncodeJobReply(const JobReply& r, const std::vector<uint8_t>& pixels,
                    std::vector<uint8_t>* out) {
    out->clear();
    PutU32(out, r.status);
    PutU32(out, r.echoBudget);
    PutU32(out, r.width);
    PutU32(out, r.height);
    PutU32(out, r.stride);
    PutU32(out, r.selfPid);
    const uint32_t len = static_cast<uint32_t>(pixels.size());
    if (len > kMaxReplyPayload) return false;
    PutU32(out, len);
    out->insert(out->end(), pixels.begin(), pixels.end());
    return out->size() <= kMaxReplyPayload;
}

bool DecodeJobReply(const uint8_t* data, size_t size, JobReply* out) {
    if (!data || size > kMaxReplyPayload) return false;
    const uint8_t* p = data;
    size_t left = size;
    JobReply r;
    uint32_t len = 0;
    if (!TakeU32(&p, &left, &r.status)) return false;
    if (!TakeU32(&p, &left, &r.echoBudget)) return false;
    if (!TakeU32(&p, &left, &r.width)) return false;
    if (!TakeU32(&p, &left, &r.height)) return false;
    if (!TakeU32(&p, &left, &r.stride)) return false;
    if (!TakeU32(&p, &left, &r.selfPid)) return false;
    if (!TakeU32(&p, &left, &len)) return false;
    if (left != len) return false;   // 报出来多少字节就必须有多少字节，半张图不算应答
    r.pixels.assign(p, p + left);
    r.decodeOk = true;
    *out = r;
    return true;
}

std::vector<uint8_t> SyntheticPixels(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i % 251);
    return v;
}

uint64_t PixelChecksum(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; }
    return h;
}

// ---------------------------------------------------------------------------
// 辅助进程模式（--worker）：验证绑定 -> 收一条任务 -> 执行 -> 交回一条应答 -> 退出。
// 退出码与 Windows 侧 RunWorkerMode 同一档（core/HelperTiming.h）。
// 这一条内部开关不是公共入口：不出现在 --help、不在选项目录里，且必须核"是谁起的我"。
// ---------------------------------------------------------------------------

std::atomic<uint32_t> g_helperPhase{static_cast<uint32_t>(HelperPhase::kAwaitTask)};
std::atomic<uint32_t> g_helperBudgetMs{0};

void RunHelperWatchdogThread() {
    // 与 Windows 的 HelperWatchdogProc 同形：只数"这一段的期限到了没有"，判的是
    // core/HelperTiming.cpp 的 JudgeHelperWatchdog 本体；每段进入时重新锚一次（用本平台
    // 的 MonotonicClockMs），段与段之间不互相侵占，也从不拿父进程时钟的读数来相减。
    auto proc = [] {
        HelperPhase phase = HelperPhase::kAwaitTask;
        uint64_t phaseStart = static_cast<uint64_t>(MonotonicClockMs());
        for (;;) {
            const HelperPhase observed =
                static_cast<HelperPhase>(g_helperPhase.load(std::memory_order_acquire));
            if (observed != phase) {
                // 重新锚点用的是"看到"的那一刻，可能比主线程"进入"的那一刻晚一个小轮询间隔。
                // 晚只会让辅助进程比自己该退的时候多活一会儿，方向是安全的（早才会误杀）。
                phase = observed;
                phaseStart = static_cast<uint64_t>(MonotonicClockMs());
            }
            HelperLimits limits;
            // 这里再夹一次上限：协议解码已经判过界，这一道防的是"哪怕交下来的数被改坏，
            // 辅助进程也不会拿到一个无界的自尽期限"。这几毫秒不进任何对外承诺。
            limits.taskBudgetMs = std::min<uint64_t>(
                g_helperBudgetMs.load(std::memory_order_relaxed), kMaxWorkerBudgetMs);
            const HelperVerdict verdict = JudgeHelperWatchdog(
                phase, limits,
                static_cast<uint64_t>(MonotonicClockMs()) - phaseStart);
            if (verdict.exitNow) _exit(static_cast<int>(verdict.exitCode));
            if (verdict.pollMs == 0) return;   // 应答已交回：进程正在自己退出，本线程收工
            Msleep(verdict.pollMs);
        }
    };
    // 建不出来就当场退出：宁可这一次隔离调用失败，也不带着"没有人在计时"的状态去
    // 等一条可能永远不来的任务（与 Windows 用裸 CreateThread 的同一个理由）。
    try {
        std::thread(proc).detach();
    } catch (...) {
        _exit(kHelperExitProtocol);
    }
}

// 绑定校验：五件事全成立才肯干活，任一条不成立就拒绝执行且不写任何输出。
// 编号与 Windows 的 BindingFailureStep 一一对应；60 + N 作为退出码。
int BindingFailureStep(const std::wstring& token, uint64_t nonce, uint64_t claimedParentPid) {
    if (nonce == 0) return 1;
    if (!hostproc::EndpointTokenLooksOurs(token)) return 1;
    if (claimedParentPid == 0 || claimedParentPid == static_cast<uint64_t>(::getpid())) return 2;
    if (!hostproc::ProcessAlive(claimedParentPid)) return 3;
    if (!hostproc::SameLoginSession(claimedParentPid)) return 4;
    // 起我的必须是同一个可执行映像（Linux 按字节精确比较；Windows 那侧是大小写折叠，
    // 这是各自平台的原生路径语义，不互相搬）——这条把"任何人都能直接跑辅助模式"挡在门外。
    std::wstring parent;
    std::wstring self;
    if (!hostproc::ImagePathOf(claimedParentPid, &parent)) return 5;
    if (!hostproc::SelfImagePath(&self)) return 5;
    if (parent != self) return 6;
    if (hostproc::ImmediateParentPid() != claimedParentPid) return 7;
    if (!hostproc::ParentCleanupArmed()) return 8;
    return 0;
}

void FinishHelperReply(hostproc::IpcChannel* channel, uint64_t nonce, uint16_t kind,
                       const JobReply& reply, const std::vector<uint8_t>& pixels) {
    std::vector<uint8_t> payload;
    if (!EncodeJobReply(reply, pixels, &payload)) {
        // 形状不自洽：交回一条 status=1 的空应答，父进程据此报错，而不是等一张永远不会来的图。
        JobReply tiny;
        tiny.status = 1;
        if (!EncodeJobReply(tiny, std::vector<uint8_t>(), &payload)) _exit(kHelperExitProtocol);
    }
    const Header h{kMagic, kProtocolVersion, kind, static_cast<uint32_t>(payload.size()), nonce};
    std::vector<uint8_t> framed;
    if (!EncodeHeader(h, &framed)) _exit(kHelperExitProtocol);
    framed.insert(framed.end(), payload.begin(), payload.end());
    if (!hostproc::WriteAllBlocking(channel, framed.data(), framed.size()).ok) {
        _exit(kHelperExitProtocol);
    }
}

int RunWorkerMode(int argc, char* const* argv) {
    // 第一件事是装上计时器（与 Windows 同次序）：之后任何一步卡住都有人负责把进程结束掉。
    RunHelperWatchdogThread();
    if (argc != 5) return 60;   // 位置参数一条不多一条不少
    const std::wstring token = DecodeUtf8(argv[2], std::strlen(argv[2])).text;
    uint64_t nonce = 0;
    if (!HexToValue(DecodeUtf8(argv[3], std::strlen(argv[3])).text, &nonce) || nonce == 0) {
        return 60;
    }
    const std::wstring pidText = DecodeUtf8(argv[4], std::strlen(argv[4])).text;
    uint64_t claimedParentPid = 0;
    for (wchar_t c : pidText) {
        if (c < L'0' || c > L'9') return 60;
        claimedParentPid = claimedParentPid * 10ull + static_cast<uint64_t>(c - L'0');
    }
    if (claimedParentPid == 0 || claimedParentPid > 0x7FFFFFFFull) return 60;
    const int step = BindingFailureStep(token, nonce, claimedParentPid);
    if (step != 0) return 60 + step;

    hostproc::IpcChannel channel;
    NativeErrorCode cerr0 = kNativeErrorNone;
    if (!hostproc::ConnectToParent(token, &channel, &cerr0)) _exit(kHelperExitProtocol);

    // 收一条任务（阻塞读；父进程断开就如实"读不下去了"，段的兜底由看门狗数）。
    uint8_t header[kHeaderSize];
    if (!hostproc::ReadExactBlocking(&channel, header, sizeof(header)).ok) {
        _exit(kHelperExitProtocol);
    }
    Header h{};
    if (!DecodeHeader(header, sizeof(header), &h)) _exit(kHelperExitProtocol);
    if (h.nonce != nonce) _exit(kHelperExitProtocol);   // 不是交给我这次的
    if (h.payloadLen > kMaxTaskPayload) _exit(kHelperExitProtocol);
    std::vector<uint8_t> payload(h.payloadLen);
    if (!payload.empty() &&
        !hostproc::ReadExactBlocking(&channel, payload.data(), payload.size()).ok) {
        _exit(kHelperExitProtocol);
    }
    Job job;
    if (!DecodeJob(payload.data(), payload.size(), &job)) _exit(kHelperExitProtocol);

    // 任务到手：执行段的期限 = 父进程交下来的剩余预算 + 交回宽限（预算已判过界，见 DecodeJob）。
    g_helperBudgetMs.store(job.budgetMs, std::memory_order_relaxed);
    g_helperPhase.store(static_cast<uint32_t>(HelperPhase::kRunning), std::memory_order_release);

    JobReply reply;
    reply.echoBudget = job.budgetMs;
    reply.selfPid = static_cast<uint32_t>(::getpid());
    std::vector<uint8_t> pixels;
    auto mark = [&reply, &pixels](uint32_t w, uint32_t hgt, uint32_t stride,
                                  const std::vector<uint8_t>& px) {
        reply.width = w;
        reply.height = hgt;
        reply.stride = stride;
        pixels = px;
        // 交出去之前先过一遍与父进程同一条帧形状判据（ImageOps 本体）：
        // 形状不自洽就降级成 status=1 的空应答，绝不让"半张图"上路。
        const FrameShape shape = CheckFrameShape(
            FrameShapeInfo{w, hgt, stride, static_cast<uint64_t>(px.size())});
        if (shape != FrameShape::kOk) {
            reply.status = 1;
            reply.width = reply.height = reply.stride = 0;
            pixels.clear();
        }
    };

    switch (job.action) {
        case kJobEcho:
            mark(2, 2, 8, SyntheticPixels(16));
            break;
        case kJobBlock:
            Msleep(job.param);
            mark(2, 2, 8, SyntheticPixels(16));
            break;
        case kJobBadExit:
            _exit(static_cast<int>(job.param));
            break;
        case kJobCrash:
            std::raise(SIGSEGV);
            _exit(kHelperExitProtocol);   // 到不了这里
            break;
        case kJobDescendantBlock: {
            // 为"后代也要收干净"造一个真实后代：它不 exec 任何东西，就在本组里睡 30 秒。
            // 取消它的是**本次创建**的那个进程组（PosixHostProcess 收尾时的 killpg），
            // 与"按名字批量杀"没有任何关系。
            const pid_t kid = ::fork();
            if (kid == 0) {
                Msleep(30000);
                _exit(0);
            }
            Msleep(60000);   // 自己卡死，让父进程来收
            break;
        }
        case kJobBulkStdio: {
            // 故意往两条流各灌一大坨：父进程没在并行排干就会堵住（这条判据测的就是它不会）。
            std::vector<uint8_t> chunk(65536, 0x5A);
            uint32_t left = job.param;
            while (left > 0) {
                const size_t take = std::min<uint32_t>(left, static_cast<uint32_t>(chunk.size()));
                ssize_t n = ::write(STDOUT_FILENO, chunk.data(), take);
                (void)n;
                n = ::write(STDERR_FILENO, chunk.data(), take);
                (void)n;
                left -= static_cast<uint32_t>(take);
            }
            mark(2, 2, 8, SyntheticPixels(16));
            break;
        }
        case kJobGarbageReply: {
            const char junk[32] = "GARBAGE-NOT-A-HEADER-XXXXXXXXX";
            hostproc::WriteAllBlocking(&channel,
                                       reinterpret_cast<const uint8_t*>(junk), sizeof(junk));
            _exit(0);
            break;
        }
        case kJobNoReply:
            _exit(0);
            break;
        case kJobBulkFrame: {
            const uint32_t stride = 1024;
            const uint32_t hgt = job.param * 1024u / stride;   // param 以 KiB 计
            mark(256, hgt, stride, SyntheticPixels(static_cast<size_t>(stride) * hgt));
            break;
        }
        case kJobLateReply:
            Msleep(job.param);
            mark(2, 2, 8, SyntheticPixels(16));
            break;
        default:
            reply.status = 1;
            break;
    }

    FinishHelperReply(&channel, nonce, h.kind, reply, pixels);
    g_helperPhase.store(static_cast<uint32_t>(HelperPhase::kDelivered), std::memory_order_release);
    return 0;
}

// ---------------------------------------------------------------------------
// 驱动侧：一次交易（与 Windows 的 Transaction 同一编排：nonce -> 端点 -> 起 -> 接 -> 交 -> 收）
// ---------------------------------------------------------------------------
std::wstring g_selfPath;

struct Tx {
    hostproc::IpcEndpoint endpoint;
    hostproc::ChildProcess child;
    hostproc::IpcChannel channel;
    uint64_t nonce = 0;
};

struct TxStartResult {
    bool spawned = false;
    bool accepted = false;
    bool wrote = false;
    NativeErrorCode err = kNativeErrorNone;
    hostproc::AcceptFault fault = hostproc::AcceptFault::kChannel;
};

TxStartResult TxStart(Tx& tx, const Job& job, const Deadline& acceptWait, const Deadline& ioWait,
                      bool captureStdio) {
    TxStartResult r;
    if (!hostproc::MakeNonce(&tx.nonce)) { r.err = EPERM; return r; }
    if (!hostproc::CreateEndpoint(tx.nonce, &tx.endpoint, &r.err)) return r;
    hostproc::SpawnOptions opts;
    opts.exePath = g_selfPath;
    opts.args = {W("--worker"), tx.endpoint.Token(), HexOf(tx.nonce),
                 std::to_wstring(static_cast<unsigned long>(::getpid()))};
    opts.captureStdio = captureStdio;
    if (!hostproc::SpawnChild(opts, &tx.endpoint, &tx.child, &r.err)) return r;
    r.spawned = true;
    if (!hostproc::AcceptChannel(&tx.endpoint, tx.child.Pid(), acceptWait, &r.fault, &tx.channel,
                                 &r.err)) {
        return r;
    }
    r.accepted = true;
    std::vector<uint8_t> payload;
    if (!EncodeJob(job, &payload)) return r;
    const Header h{kMagic, kProtocolVersion, kJobKind, static_cast<uint32_t>(payload.size()),
                   tx.nonce};
    std::vector<uint8_t> framed;
    if (!EncodeHeader(h, &framed)) return r;
    framed.insert(framed.end(), payload.begin(), payload.end());
    const hostproc::ChannelIo w =
        hostproc::WriteAll(&tx.channel, framed.data(), framed.size(), ioWait);
    r.wrote = w.ok;
    r.err = w.code;
    return r;
}

struct TxReply {
    bool gotHeader = false;     // 读满 24 字节报头
    bool timedOut = false;
    bool peerClosed = false;
    bool headerValid = false;
    bool nonceOk = false;
    bool fullReply = false;     // 完整的一条应答（报头 + 载荷都读到且解码成立）
    JobReply reply;
};

TxReply TxRead(Tx& tx, const Deadline& wait) {
    TxReply out;
    uint8_t header[kHeaderSize];
    const hostproc::ChannelIo hs = hostproc::ReadExact(&tx.channel, header, sizeof(header), wait);
    if (hs.timedOut) { out.timedOut = true; return out; }
    if (!hs.ok) { out.peerClosed = hs.peerClosed; return out; }
    out.gotHeader = true;
    Header h{};
    out.headerValid = DecodeHeader(header, sizeof(header), &h);
    if (!out.headerValid) return out;
    out.nonceOk = (h.nonce == tx.nonce && h.kind == kJobKind);
    if (!out.nonceOk) return out;
    std::vector<uint8_t> payload(h.payloadLen);
    if (!payload.empty()) {
        const hostproc::ChannelIo ps =
            hostproc::ReadExact(&tx.channel, payload.data(), payload.size(), wait);
        if (ps.timedOut) { out.timedOut = true; return out; }
        if (!ps.ok) { out.peerClosed = ps.peerClosed; return out; }
    }
    out.fullReply = DecodeJobReply(payload.data(), payload.size(), &out.reply);
    return out;
}

// 收尾 + 记账：FinishTransaction 之后进程必须已经从桌面消失，fd 不能漏。
void TxFini(Tx& tx, uint32_t reapWaitMs = 2000u) {
    hostproc::FinishTransaction(&tx.child, &tx.channel, reapWaitMs,
                                hostproc::kCancelDrainGraceMs);
}

size_t FdCount() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return 0;
    size_t n = 0;
    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        char* endp = nullptr;
        const long v = std::strtol(e->d_name, &endp, 10);
        if (endp == e->d_name || *endp != '\0' || v < 0) continue;
        ++n;
    }
    ::closedir(d);
    return n;   // 含 opendir 自己那一条：所有测量都含，基线一致即可判泄漏
}

// /proc 扫描：进程组 pgid 里现在有几个进程（stat 的字段 5 = pgrp）。
size_t GroupMembers(pid_t pgid) {
    size_t n = 0;
    DIR* d = ::opendir("/proc");
    if (!d) return 0;
    struct dirent* e = nullptr;
    while ((e = ::readdir(d)) != nullptr) {
        char* endp = nullptr;
        const long pid = std::strtol(e->d_name, &endp, 10);
        if (endp == e->d_name || *endp != '\0') continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        char buf[2048];
        if (std::fgets(buf, sizeof(buf), f)) {
            char* p = std::strrchr(buf, ')');
            char state = '\0';
            int ppid = 0, pgrp = 0;
            // stat 的字段：state(字符) ppid pgrp —— state 不是数字，必须按 %c 读。
            if (p && std::sscanf(p + 1, " %c %d %d", &state, &ppid, &pgrp) == 3 &&
                pgrp == static_cast<int>(pgid) && state != 'Z') {
                ++n;
            }
        }
        std::fclose(f);
    }
    ::closedir(d);
    return n;
}

bool WaitPidGone(pid_t pid, uint64_t budgetMs) {
    const int64_t until = MonotonicClockMs() + static_cast<int64_t>(budgetMs);
    while (MonotonicClockMs() < until) {
        if (!hostproc::ProcessAlive(static_cast<uint64_t>(pid))) return true;
        Msleep(20);
    }
    return !hostproc::ProcessAlive(static_cast<uint64_t>(pid));
}

// 驱动自己的裸 fork + execv（不经服务）：用来造"没有收尾上下文"与"中继父进程"两类拒绝现场。
// 返回子进程退出码（问不出来给 -1）。
int RawSpawnAndReap(const std::vector<std::string>& argvv) {
    std::vector<char*> argv;
    for (const std::string& s : argvv) argv.push_back(const_cast<char*>(s.data()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execv(argv[0], argv.data());
        _exit(127);
    }
    if (pid < 0) return -1;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// 驱动：逐节判
// ---------------------------------------------------------------------------
namespace {

void SectionFramingUnits() {
    Section("1) 报头编解码判界（生产本体 core/WorkerFraming.cpp）");
    Header good{kMagic, kProtocolVersion, kJobKind, 64, 0x0123456789ABCDEFull};
    std::vector<uint8_t> bytes;
    Check(EncodeHeader(good, &bytes) && bytes.size() == kHeaderSize, "合法报头能编出来");
    Header back{};
    Check(DecodeHeader(bytes.data(), bytes.size(), &back), "编出去的报头读得回来");
    Check(back.magic == good.magic && back.version == good.version && back.kind == good.kind &&
              back.payloadLen == good.payloadLen && back.nonce == good.nonce,
          "读回来的字段逐字一致");
    Check(!DecodeHeader(nullptr, 0, &back), "空指针整条作废");
    bytes.resize(kHeaderSize - 1);
    Check(!DecodeHeader(bytes.data(), bytes.size(), &back), "截断的报头整条作废");
    bytes = std::vector<uint8_t>(kHeaderSize, 0);
    EncodeHeader(good, &bytes);
    bytes[0] ^= 0xFF;
    Check(!DecodeHeader(bytes.data(), bytes.size(), &back), "magic 不对整条作废");
    EncodeHeader(good, &bytes);
    bytes[4] = static_cast<uint8_t>(kProtocolVersion + 1);
    Check(!DecodeHeader(bytes.data(), bytes.size(), &back), "版本对不上整条作废（不半收）");
    EncodeHeader(good, &bytes);
    bytes[12] = 0x01;   // 保留段第一字节非 0
    Check(!DecodeHeader(bytes.data(), bytes.size(), &back), "保留段非 0 整条作废");
    EncodeHeader(good, &bytes);
    bytes[8] = 0xFF; bytes[9] = 0xFF; bytes[10] = 0xFF; bytes[11] = 0xFF;
    Check(!DecodeHeader(bytes.data(), bytes.size(), &back), "payloadLen 超上限整条作废");
    Header huge = good;
    huge.payloadLen = kMaxReplyPayload + 1;
    Check(!EncodeHeader(huge, &bytes), "编码侧也拒收超限 payloadLen");
    EncodeHeader(good, &bytes);
    bytes.push_back(0);   // 尾部多一字节：报头层按 24 字节读，语义交给载荷层的"正好读完"
    Check(DecodeHeader(bytes.data(), bytes.size(), &back), "报头按固定 24 字节读，不靠尾部定长");
    Header zero = good;
    zero.payloadLen = 0;
    Check(EncodeHeader(zero, &bytes) && DecodeHeader(bytes.data(), bytes.size(), &back) &&
              back.payloadLen == 0,
          "payloadLen=0 合法（空载荷另有载荷层判）");
}

void SectionWatchdogUnits() {
    Section("2) 三段期限算式（生产本体 core/HelperTiming.cpp）");
    HelperLimits limits;
    limits.handshakeMs = 1000;
    limits.taskBudgetMs = 300;
    limits.deliverGraceMs = 50;
    auto awaitV = [](const HelperLimits& l, uint64_t e) {
        return JudgeHelperWatchdog(HelperPhase::kAwaitTask, l, e);
    };
    auto runV = [](const HelperLimits& l, uint64_t e) {
        return JudgeHelperWatchdog(HelperPhase::kRunning, l, e);
    };
    auto v = awaitV(limits, 999);
    Check(!v.exitNow && v.pollMs > 0 && v.pollMs <= kWatchdogPollMs, "握手段未到期：继续等，轮询有界");
    v = awaitV(limits, 1000);
    Check(v.exitNow && v.exitCode == kHelperExitNoTask, "握手段到点：自己退（没人交任务的兜底）");
    v = runV(limits, 349);
    Check(!v.exitNow, "执行段 = 预算 + 交回宽限；349ms 还没到 350");
    v = runV(limits, 350);
    Check(v.exitNow && v.exitCode == kHelperExitOverBudget, "执行段到点：自己退（超预算档）");
    HelperLimits huge;
    huge.taskBudgetMs = UINT64_MAX;
    huge.deliverGraceMs = UINT64_MAX;
    v = runV(huge, 0);
    Check(!v.exitNow, "预算与宽限都拉满：加法饱和也不绕回成'立刻到点'");
    v = runV(huge, UINT64_MAX);
    Check(v.exitNow, "真到 UINT64_MAX 才退（饱和算术的终态仍可判）");
    HelperLimits del;
    v = JudgeHelperWatchdog(HelperPhase::kDelivered, del, 0);
    Check(!v.exitNow && v.pollMs == 0, "应答已交回：不再挂第四个自尽时钟");
    HelperLimits def;
    v = awaitV(def, kHandshakeMs - 1);
    Check(!v.exitNow, "默认握手段 = kHandshakeMs（父进程侧连管与子进程侧同一段含义）");
    v = runV(def, kIsolatedCallMs);   // 默认预算 0：执行段兜底 = 0 + 交回宽限
    Check(v.exitNow, "没交出预算（0）：执行段只剩交回宽限的兜底也照样到点");
}

void SectionHappyPath() {
    Section("3) 真父子：双向通信 + 身份配对（生产服务 + 生产帧层）");
    const size_t fdBefore = FdCount();
    Job job;
    job.action = kJobEcho;
    job.budgetMs = 5000;
    Tx a;
    TxStartResult ra = TxStart(a, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(3000),
                               false);
    Check(ra.spawned && ra.accepted && ra.wrote, "任务交进来了（起、接、写全部成立）");
    TxReply xa = TxRead(a, Deadline::FromTotalMs(3000));
    Check(xa.fullReply && xa.reply.status == 0, "应答解码成立且状态正常");
    Check(xa.reply.echoBudget == 5000, "随任务交下去的预算原样到达辅助进程（时长，不是时刻）");
    Check(xa.reply.selfPid != static_cast<uint32_t>(::getpid()), "应答确实出自另一个进程");
    Check(xa.reply.width == 2 && xa.reply.height == 2 && xa.reply.stride == 8 &&
              xa.reply.pixels.size() == 16,
          "帧形状自洽");
    const FrameShape shape = CheckFrameShape(FrameShapeInfo{
        xa.reply.width, xa.reply.height, xa.reply.stride,
        static_cast<uint64_t>(xa.reply.pixels.size())});
    Check(shape == FrameShape::kOk, "应答帧过生产 CheckFrameShape（与父进程交付前同一道）");
    hostproc::PeerId peer = hostproc::ChannelPeer(a.channel);
    Check(peer.pidKnown && peer.pid == a.child.Pid(), "对端身份就是我刚起的那个（内核答的）");
    Check(peer.uidKnown && peer.uid == static_cast<uint64_t>(::getuid()),
          "同 uid 核对成立（Linux 侧端点可信性的第三条腿）");
    TxFini(a);
    Check(hostproc::ChildExitCode(a.child) == 0, "正常交易：辅助进程自己退成 0");
    Check(!hostproc::ProcessAlive(a.child.Pid()), "进程已从 /proc 消失（无僵尸）");

    // 两笔并发交易互不冒领：各自的应答只认自己的 nonce。
    Tx b, c;
    Job j2;
    j2.action = kJobBlock;
    j2.param = 200;
    j2.budgetMs = 4000;
    TxStartResult rb = TxStart(b, j2, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(3000),
                               false);
    Job j3;
    j3.action = kJobEcho;
    j3.budgetMs = 4000;
    TxStartResult rc2 = TxStart(c, j3, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(3000),
                                false);
    Check(rb.accepted && rc2.accepted, "两笔并发交易都接上了自己的辅助进程");
    Check(b.nonce != c.nonce, "两笔的 nonce 不同");
    TxReply xb = TxRead(b, Deadline::FromTotalMs(3000));
    TxReply xc = TxRead(c, Deadline::FromTotalMs(3000));
    Check(xb.fullReply && xc.fullReply && xb.reply.selfPid != xc.reply.selfPid,
          "应答各归各的进程");
    TxFini(b);
    TxFini(c);
    Check(FdCount() == fdBefore, "本节结束：描述符数回到基线（无泄漏）");
}

void SectionBulkFrame() {
    Section("4) 双向大载荷：4MiB 帧走流式通道");
    Job job;
    job.action = kJobBulkFrame;
    job.param = 4096;   // KiB
    job.budgetMs = 8000;
    Tx tx;
    TxStartResult r = TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(10000),
                              false);
    Check(r.wrote, "大载荷交易的任务交进去了");
    TxReply x = TxRead(tx, Deadline::FromTotalMs(10000));
    Check(x.fullReply, "4MiB 应答完整读回（流式分片不影响'正好读完'的判据）");
    Check(x.reply.width == 256 && x.reply.stride == 1024 && x.reply.height == 4096 &&
              x.reply.pixels.size() == 1024ull * 4096ull,
          "帧尺寸与行距逐字对得上");
    Check(PixelChecksum(x.reply.pixels) == PixelChecksum(SyntheticPixels(1024ull * 4096ull)),
          "字节内容逐字节一致（没有截断/错位）");
    Check(CheckFrameShape(FrameShapeInfo{x.reply.width, x.reply.height, x.reply.stride,
                                         static_cast<uint64_t>(x.reply.pixels.size())}) ==
              FrameShape::kOk,
          "大帧仍过同一条形状判据（上限内的自洽）");
    TxFini(tx);
}

void SectionIdentityRefusals() {
    Section("5) 辅助进程拒收不属于本次交易的现场（身份与绑定）");
    // 5a nonce 不对：任务帧报头写着别人的 nonce —— 辅助进程不接、不写、退 9。
    {
        Tx tx;
        tx.nonce = 0;
        hostproc::MakeNonce(&tx.nonce);
        NativeErrorCode e = 0;
        Check(hostproc::CreateEndpoint(tx.nonce, &tx.endpoint, &e), "为 5a 建好端点");
        hostproc::SpawnOptions opts;
        opts.exePath = g_selfPath;
        // 辅助进程的绑定校验比的是 argv 里自报的 parentPid 与内核答案；nonce 不匹配发生在
        // 收任务那一步（报头里的 nonce 与我这次的 nonce 对不上）。这里把 argv 里的 nonce 放对、
        // 帧里写错的 nonce，判的就是后一条。
        opts.args = {W("--worker"), tx.endpoint.Token(), HexOf(tx.nonce),
                     std::to_wstring(static_cast<unsigned long>(::getpid()))};
        Check(hostproc::SpawnChild(opts, &tx.endpoint, &tx.child, &e), "5a 起进程");
        Deadline wait = Deadline::FromTotalMs(3000);
        hostproc::AcceptFault fault = hostproc::AcceptFault::kChannel;
        Check(hostproc::AcceptChannel(&tx.endpoint, tx.child.Pid(), wait, &fault, &tx.channel, &e),
              "5a 连上了");
        Job job;
        job.action = kJobEcho;
        job.budgetMs = 2000;
        std::vector<uint8_t> payload;
        EncodeJob(job, &payload);
        const Header bad{kMagic, kProtocolVersion, kJobKind, static_cast<uint32_t>(payload.size()),
                         tx.nonce ^ 0xA5A5A5A5ull};
        std::vector<uint8_t> framed;
        EncodeHeader(bad, &framed);
        framed.insert(framed.end(), payload.begin(), payload.end());
        Check(hostproc::WriteAll(&tx.channel, framed.data(), framed.size(), wait).ok, "5a 发出去了");
        TxReply x = TxRead(tx, wait);
        Check(!x.gotHeader && x.peerClosed, "5a：伪造 nonce 的任务不会换回任何应答");
        TxFini(tx);
        Check(hostproc::ChildExitCode(tx.child) == kHelperExitProtocol, "5a 退 9（不接别人的交易）");
    }
    // 5b 端点可信性由内核回答：accept 核对的 PID 与连上来的不一致时，整条拒绝、
    // 不产生通道、不消费任何字节。这里把"预期对端"故意写成一个绝不可能连上来的 PID
    // （驱动自己的），真正连上来的 --worker 辅助进程就会被内核答案拒下。
    {
        Tx tx;
        NativeErrorCode e = 0;
        hostproc::MakeNonce(&tx.nonce);
        Check(hostproc::CreateEndpoint(tx.nonce, &tx.endpoint, &e), "5b 建端点");
        hostproc::SpawnOptions forger;
        forger.exePath = g_selfPath;
        forger.args = {W("--worker"), tx.endpoint.Token(), HexOf(tx.nonce),
                       std::to_wstring(static_cast<unsigned long>(::getpid()))};
        Tx helper;
        Check(hostproc::SpawnChild(forger, &tx.endpoint, &helper.child, &e), "5b 起真实连接者");
        hostproc::AcceptFault fault = hostproc::AcceptFault::kChannel;
        Deadline wait = Deadline::FromTotalMs(3000);
        const bool accepted =
            hostproc::AcceptChannel(&tx.endpoint, static_cast<uint64_t>(::getpid()), wait, &fault,
                                    &tx.channel, &e);
        Check(!accepted, "5b：连上来的不是预期对端 -> accept 拒绝");
        Check(fault == hostproc::AcceptFault::kPeerMismatch &&
                  e == static_cast<NativeErrorCode>(EACCES),
              "5b：拒绝的原因是内核答的 PID 对不上（EACCES 记账）");
        Check(!tx.channel.IsOpen(), "5b：伪造者没拿到通道，一个字节都没被消费");
        // 被关掉的那头：辅助进程读不到任务 -> 退 9，绝不带着半条连接继续干活。
        TxFini(helper, 2000);
        Check(hostproc::ChildExitCode(helper.child) == kHelperExitProtocol, "5b：冒名连接退 9");
        Check(!hostproc::ProcessAlive(helper.child.Pid()), "5b：它收干净了");
        tx.endpoint.Close();
    }
    // 5c 绑定阶梯：token 形状不合 / 自称父进程是自己 / 父进程不存在 / 中继父进程冒名 /
    //    没有收尾上下文（PDEATHSIG 没装上）。
    {
        const std::string self = S(g_selfPath);
        const std::string nonceHex = S(HexOf(0x0123456789ABCDEFull));
        const std::string pid = std::to_string(static_cast<long>(::getpid()));
        Check(RawSpawnAndReap({self, "--worker", "banana", nonceHex, pid}) == 61,
              "5c-1：端点令牌形状不合 -> 拒执行（61）");
        Check(RawSpawnAndReap({self, "--worker", "3", nonceHex, pid}) == 68,
              "5c-2：手动 exec（无 PDEATHSIG）的辅助模式 -> 收尾上下文没装上 -> 拒（68）");
        Check(RawSpawnAndReap({self, "--worker", "3", nonceHex, "0"}) == 60,
              "5c-3：父 PID 自称 0 -> 参数约定层就拒（60，与 Windows ParseWorkerArgs 同一判界）");
        Check(RawSpawnAndReap({self, "--worker", "3", nonceHex, "999999"}) == 63,
              "5c-4：自称的父进程不存在 -> 拒（63）");
        Check(RawSpawnAndReap({self, "--worker", "3", nonceHex, pid, "extra"}) == 60,
              "5c-5：参数多一条 -> 约定不合，一条都不干（60）");
    }
    // 5d 中继冒名：真正的直接父进程是一个中继子进程，argv 里却自称驱动 —— 第 7 步拒（67）。
    {
        const std::string self = S(g_selfPath);
        const std::string pid = std::to_string(static_cast<long>(::getpid()));
        const int got = RawSpawnAndReap({self, "--relay-worker", pid});
        Check(got == 67 || got == 63, "5d：直接父进程对不上（或中继已退）都按不成立处理");
    }
}

void SectionMalformedMessages() {
    Section("6) 畸形消息整条作废（不产生'看起来成功'的半结果）");
    // builder 拿到本次交易的 nonce 来造一条畸形消息：辅助进程会先核 nonce 再往下读，
    // 所以"载荷层坏"的那些现场必须带对的 nonce，才能验到载荷层本身。
    auto sendRawAndExpectExit9 = [](const char* label,
                                    const std::function<std::vector<uint8_t>(uint64_t)>& build,
                                    bool halfCloseAfterWrite) {
        Tx tx;
        NativeErrorCode e = 0;
        hostproc::MakeNonce(&tx.nonce);
        if (!hostproc::CreateEndpoint(tx.nonce, &tx.endpoint, &e)) { Check(false, label); return; }
        hostproc::SpawnOptions opts;
        opts.exePath = g_selfPath;
        opts.args = {W("--worker"), tx.endpoint.Token(), HexOf(tx.nonce),
                     std::to_wstring(static_cast<unsigned long>(::getpid()))};
        if (!hostproc::SpawnChild(opts, &tx.endpoint, &tx.child, &e)) { Check(false, label); return; }
        hostproc::AcceptFault fault = hostproc::AcceptFault::kChannel;
        Deadline wait = Deadline::FromTotalMs(3000);
        if (!hostproc::AcceptChannel(&tx.endpoint, tx.child.Pid(), wait, &fault, &tx.channel, &e)) {
            Check(false, label);
            TxFini(tx, 0);
            return;
        }
        const std::vector<uint8_t> bytes = build(tx.nonce);
        static_cast<void>(hostproc::WriteAll(&tx.channel, bytes.data(), bytes.size(), wait));
        if (halfCloseAfterWrite) tx.channel.Close();   // 模拟"发到一半撒手"
        TxReply x = TxRead(tx, Deadline::FromTotalMs(3000));
        Check(!x.fullReply, label);
        TxFini(tx, 3000);
        Check(hostproc::ChildExitCode(tx.child) == kHelperExitProtocol,
              (std::string(label) + " -> 辅助进程退 9").c_str());
    };

    // 6a 截断报头（10 字节后撒手）
    sendRawAndExpectExit9("6a：截断的报头不成为任务",
                          [](uint64_t) { return std::vector<uint8_t>(10, 0x11); }, true);

    // 6b 版本不对（完整 24 字节 + 合法载荷，version+1）
    sendRawAndExpectExit9("6b：版本对不上整条作废（不'向后兼容'地试读）",
                          [](uint64_t nonce) {
                              Header h{kMagic, kProtocolVersion + 1, kJobKind, kJobBytes, nonce};
                              std::vector<uint8_t> framed;
                              EncodeHeader(h, &framed);
                              Job job;
                              job.budgetMs = 2000;
                              std::vector<uint8_t> payload;
                              EncodeJob(job, &payload);
                              framed.insert(framed.end(), payload.begin(), payload.end());
                              return framed;
                          }, false);

    // 6c 载荷超上限声明（payloadLen 谎报 1MiB 但只发 12 字节，随后撒手）
    sendRawAndExpectExit9("6c：报头与实发字节数不符 -> 整条作废（谎报的 1MiB 不会变成缓冲）",
                          [](uint64_t nonce) {
                              Header h{kMagic, kProtocolVersion, kJobKind, 1024u * 1024u, nonce};
                              std::vector<uint8_t> framed;
                              EncodeHeader(h, &framed);
                              Job job;
                              job.budgetMs = 2000;
                              std::vector<uint8_t> payload;
                              EncodeJob(job, &payload);
                              framed.insert(framed.end(), payload.begin(), payload.end());
                              return framed;
                          }, true);

    // 6d 载荷多一字节（尾随垃圾 = 两边不是同一份格式）
    sendRawAndExpectExit9("6d：'正好读完'判据挡住尾随垃圾",
                          [](uint64_t nonce) {
                              Job job;
                              job.budgetMs = 2000;
                              std::vector<uint8_t> payload;
                              EncodeJob(job, &payload);
                              payload.push_back(0x7F);
                              Header h{kMagic, kProtocolVersion, kJobKind,
                                       static_cast<uint32_t>(payload.size()), nonce};
                              std::vector<uint8_t> framed;
                              EncodeHeader(h, &framed);
                              framed.insert(framed.end(), payload.begin(), payload.end());
                              return framed;
                          }, false);

    // 6e 预算为 0 / 超上限：判界用的是 WorkerFraming 那一份生产上限常量
    sendRawAndExpectExit9("6e-1：budget=0 不是'不限'，整条作废",
                          [](uint64_t nonce) {
                              std::vector<uint8_t> payload;
                              PutU32(&payload, kJobEcho);
                              PutU32(&payload, 0);
                              PutU32(&payload, 0);
                              Header h{kMagic, kProtocolVersion, kJobKind,
                                       static_cast<uint32_t>(payload.size()), nonce};
                              std::vector<uint8_t> framed;
                              EncodeHeader(h, &framed);
                              framed.insert(framed.end(), payload.begin(), payload.end());
                              return framed;
                          }, false);
    sendRawAndExpectExit9("6e-2：budget 超 --timeout-ms 上限整条作废（兜底不进无界区间）",
                          [](uint64_t nonce) {
                              std::vector<uint8_t> payload;
                              PutU32(&payload, kJobEcho);
                              PutU32(&payload, 0);
                              PutU32(&payload, kMaxWorkerBudgetMs + 1);
                              Header h{kMagic, kProtocolVersion, kJobKind,
                                       static_cast<uint32_t>(payload.size()), nonce};
                              std::vector<uint8_t> framed;
                              EncodeHeader(h, &framed);
                              framed.insert(framed.end(), payload.begin(), payload.end());
                              return framed;
                          }, false);

    // 6f 应答侧畸形：辅助进程交回一坨非报头字节 -> 驱动按协议不合处理，且不留僵尸
    {
        Job job;
        job.action = kJobGarbageReply;
        job.budgetMs = 3000;
        Tx tx;
        TxStartResult r = TxStart(tx, job, Deadline::FromTotalMs(3000),
                                  Deadline::FromTotalMs(3000), false);
        Check(r.wrote, "6f：任务交进去了");
        TxReply x = TxRead(tx, Deadline::FromTotalMs(3000));
        Check(x.gotHeader && !x.headerValid, "6f：畸形应答在读满 24 字节后被判整条作废");
        Check(!x.fullReply, "6f：畸形应答不产生任何'结果'");
        TxFini(tx);
        Check(!hostproc::ProcessAlive(tx.child.Pid()), "6f：收尸后进程消失");
    }
}

void SectionTimeoutCancel() {
    Section("7) 阻塞 -> 期限耗尽 -> 取消只打本次创建的进程");
    const size_t fdBefore = FdCount();
    // 对照组：一个与本次交易无关的裸 sleep（驱动自己的进程组），验证"不误杀"。
    const pid_t control = ::fork();
    if (control == 0) {
        const char* argvv[3] = {"/bin/sleep", "20", nullptr};
        ::execv("/bin/sleep", const_cast<char**>(argvv));
        _exit(127);
    }
    {
        Job job;
        job.action = kJobBlock;
        job.param = 60000;
        job.budgetMs = 20000;   // 辅助进程自己的兜底远在 21 秒后：先动手的必然是父进程
        Tx tx;
        TxStartResult r = TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(300),
                                  false);
        Check(r.wrote, "卡死任务交进去了");
        TxReply x = TxRead(tx, Deadline::FromTotalMs(300));
        Check(x.timedOut, "到点拿不到结果就是到点（不猜、不'部分成功'）");
        const pid_t helperPid = static_cast<pid_t>(tx.child.Pid());
        Check(GroupMembers(helperPid) >= 1, "取消之前：那个进程组确实还在（证明判据看得见）");
        TxFini(tx, 50);
        Check(hostproc::ChildExitCode(tx.child) == kHelperExitReaped,
              "父进程收尸打的退出码记成'收尸'档（与辅助进程自己的判断分得开）");
        Check(!hostproc::ProcessAlive(tx.child.Pid()), "被取消的辅助进程已从 /proc 消失");
        Check(GroupMembers(helperPid) == 0, "本次创建的进程组已经空了");
    }
    Check(hostproc::ProcessAlive(static_cast<uint64_t>(control)), "无关的对照组进程活得好好的（不按名字批量杀）");
    ::kill(control, SIGKILL);
    ::waitpid(control, nullptr, 0);
    Check(FdCount() == fdBefore, "本节结束：描述符回到基线（管道/监听口/子进程都收干净）");
}

void SectionDescendantCleanup() {
    Section("8) 辅助进程留下的后代一并收掉");
    Job job;
    job.action = kJobDescendantBlock;
    job.budgetMs = 200;
    Tx tx;
    TxStartResult r = TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(400),
                              false);
    Check(r.wrote, "带后代的任务交进去了");
    // 给后代一点时间落进进程组（fork 就在执行段开头）。
    Msleep(150);
    const pid_t helperPid = static_cast<pid_t>(tx.child.Pid());
    const size_t members = GroupMembers(helperPid);
    Check(members >= 2, "取消之前：组里有辅助进程 + 它留下的后代");
    TxReply x = TxRead(tx, Deadline::FromTotalMs(400));
    Check(x.timedOut || x.peerClosed, "父进程按预算放弃了这条结果");
    TxFini(tx, 50);
    Check(GroupMembers(helperPid) == 0, "收尾之后：组内一个进程都不剩（后代一并收掉）");
    Check(hostproc::ChildExitCode(tx.child) == kHelperExitReaped, "辅助进程按'收尸'档记档");
}

void SectionBudgetSelfExit() {
    Section("9) 辅助进程自己的预算兜底（生产三段期限算式接在真时钟上）");
    // 预算 200ms + 交回宽限 1000ms：父进程故意等 3000ms，先到的必然是辅助进程自己的看门狗。
    const int64_t t0 = MonotonicClockMs();
    Job job;
    job.action = kJobBlock;
    job.param = 60000;
    job.budgetMs = 200;
    Tx tx;
    TxStartResult r = TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(3000),
                              false);
    Check(r.wrote, "超长阻塞任务交进去了");
    TxReply x = TxRead(tx, Deadline::FromTotalMs(3000));
    const int64_t t1 = MonotonicClockMs();
    Check(x.peerClosed && !x.fullReply, "通道自己断了：辅助进程按时自尽，不是父进程打的");
    Check(t1 - t0 < 2800, "自尽发生在预算 + 交回宽限之内（不是父进程的 3 秒）");
    TxFini(tx, 2000);
    Check(hostproc::ChildExitCode(tx.child) == kHelperExitOverBudget,
          "辅助进程按自己的判断退 11（超预算档，与收尸档分得开）");
}

void SectionParentDeath() {
    Section("10) 父进程先退：PDEATHSIG 兜底，不等任何时钟");
    // 驱动 fork 出一个"迷你父进程"：它完成一次正常的任务交付后立刻 _exit(0)。
    // 辅助进程正卡在 60 秒的阻塞里，自己的看门狗要到 30 秒 + 1 秒才打；
    // 它若在 ~5 秒内消失，唯一的解释就是 PDEATHSIG（父进程一没，内核发 SIGTERM）。
    Job job;
    job.action = kJobBlock;
    job.param = 60000;
    job.budgetMs = 30000;
    int reportPipe[2] = {-1, -1};
    if (::pipe2(reportPipe, O_CLOEXEC) != 0) { Check(false, "pipe2 失败"); return; }
    const pid_t mini = ::fork();
    if (mini == 0) {
        ::close(reportPipe[0]);
        // 迷你父进程：在自己的地址空间副本里做交易（fork 继承的服务对象各拿各的内核引用）。
        // fork 出的驱动副本里没有任何监听端点，这份交易从建端点起全部归迷你进程；
        // 它把"我起了哪个辅助进程"报给驱动后立刻 _exit(0) —— 从此没人管它了。
        Tx local;
        TxStartResult r = TxStart(local, job, Deadline::FromTotalMs(3000),
                                  Deadline::FromTotalMs(3000), false);
        const uint64_t helperPid = local.child.Pid();
        const uint64_t wire = r.wrote ? helperPid : 0ull;
        static_cast<void>(::write(reportPipe[1], &wire, sizeof(wire)));
        _exit(r.wrote ? 0 : 1);
    }
    ::close(reportPipe[1]);
    if (mini > 0) {
        uint64_t helperPid = 0;
        ssize_t got = ::read(reportPipe[0], &helperPid, sizeof(helperPid));
        ::close(reportPipe[0]);
        int status = 0;
        while (::waitpid(mini, &status, 0) < 0 && errno == EINTR) {}
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "迷你父进程正常完成任务后退出");
        Check(got == static_cast<ssize_t>(sizeof(helperPid)) && helperPid > 0,
              "拿到辅助进程的 PID");
        Check(WaitPidGone(static_cast<pid_t>(helperPid), 5000),
              "5 秒内它就消失了（30+1 秒的看门狗还没到，唯一解释是 PDEATHSIG）");
    } else {
        ::close(reportPipe[0]);
        Check(false, "fork 迷你父进程失败");
    }
}

void SectionAbnormalExit() {
    Section("11) 异常退出如实分档（自杀 / 崩溃 / 静音）");
    {
        Job job;
        job.action = kJobBadExit;
        job.param = 42;
        job.budgetMs = 3000;
        Tx tx;
        Check(TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(3000), false)
                  .wrote,
              "自杀任务交进去了");
        TxReply x = TxRead(tx, Deadline::FromTotalMs(3000));
        Check(!x.fullReply && x.peerClosed, "它没交回任何东西，通道如实断了");
        TxFini(tx, 2000);
        Check(hostproc::ChildExitCode(tx.child) == 42, "退出码 42 如实报（比'管道断了'有用）");
    }
    {
        Job job;
        job.action = kJobCrash;
        job.budgetMs = 3000;
        Tx tx;
        Check(TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(3000), false)
                  .wrote,
              "崩溃任务交进去了");
        TxReply x = TxRead(tx, Deadline::FromTotalMs(3000));
        Check(!x.fullReply && x.peerClosed, "崩溃同样只留下断掉的通道");
        TxFini(tx, 2000);
        const int code = hostproc::ChildExitCode(tx.child);
        Check(code == 128 + SIGSEGV, "被打死按 128+signum 记档（信号死与退出码分得开）");
    }
    {
        Job job;
        job.action = kJobNoReply;
        job.budgetMs = 3000;
        Tx tx;
        Check(TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(3000), false)
                  .wrote,
              "静音任务交进去了");
        TxReply x = TxRead(tx, Deadline::FromTotalMs(3000));
        Check(!x.fullReply && x.peerClosed, "一声不响地退出 = 通道 EOF");
        TxFini(tx, 2000);
        Check(hostproc::ChildExitCode(tx.child) == 0, "它自己退成 0 —— 但驱动绝不会报截图成功");
    }
}

void SectionStdioParallel() {
    Section("12) stdout/stderr 并行排干（一条流堵不住另一条）");
    Job job;
    job.action = kJobBulkStdio;
    job.param = 1024u * 1024u;   // 两条流各 1MiB：管道的 64KiB 缓冲早就写满，只能靠并行排干
    job.budgetMs = 8000;
    Tx tx;
    TxStartResult r = TxStart(tx, job, Deadline::FromTotalMs(3000), Deadline::FromTotalMs(8000),
                              true);
    Check(r.wrote, "灌流任务交进去了");
    std::string outBytes, errBytes;
    NativeErrorCode e = 0;
    const int64_t t0 = MonotonicClockMs();
    const bool drained = hostproc::CaptureChildStdio(
        &tx.child, Deadline::FromTotalMs(6000), &outBytes, &errBytes,
        static_cast<size_t>(job.param), &e);
    const int64_t t1 = MonotonicClockMs();
    Check(drained, "两条流在预算内都排干净（串行读这里必死锁）");
    Check(outBytes.size() == job.param && errBytes.size() == job.param,
          "各自 1MiB，一个字节不多一个字节不少（超出留档上限的部分照排不误存）");
    Check(t1 - t0 < 6000, "排干不是靠运气也不是靠无限等");
    TxReply x = TxRead(tx, Deadline::FromTotalMs(5000));
    Check(x.fullReply, "灌完流之后应答照旧走同一条通道");
    TxFini(tx, 2000);
}

void SectionBudgetPassing() {
    Section("13) 剩余预算的传递：时长不时刻，一次交易一份账");
    // 一笔 500ms 的总预算；先烧掉 ~150ms 再领这一笔隔离调用的预算 ——
    // 交下去的 budgetMs 必须反映"还剩多少"，绝不重新领一份 500。
    Deadline run = Deadline::FromTotalMs(500);
    Msleep(150);
    const uint64_t leftAtSpawn = run.RemainingMs();
    Job job;
    job.action = kJobEcho;
    job.budgetMs = static_cast<uint32_t>(std::min<uint64_t>(leftAtSpawn > 0 ? leftAtSpawn : 1,
                                                            kMaxWorkerBudgetMs));
    Tx tx;
    Check(TxStart(tx, job, Deadline::FromTotalMs(1000), run, false).wrote,
          "预算随任务交下去了");
    TxReply x = TxRead(tx, run);
    Check(x.fullReply, "在剩余预算之内等到了结果");
    Check(x.reply.echoBudget == job.budgetMs && job.budgetMs <= 350 && job.budgetMs >= 1,
          "辅助进程数到的是那段**剩余时长**（两个时钟域各数各的，从不互减）");
    TxFini(tx);
    Check(run.Spent() || run.RemainingMs() < 500, "同一笔交易花的仍是那一份总预算的毫秒");
}

void SectionSpawnFailureHygiene() {
    Section("14) 启动失败/握手段的卫生账（不僵尸、不漏 fd、不漏监听口）");
    const size_t fdBefore = FdCount();
    {
        Tx tx;
        NativeErrorCode e = 0;
        hostproc::MakeNonce(&tx.nonce);
        hostproc::CreateEndpoint(tx.nonce, &tx.endpoint, &e);
        hostproc::SpawnOptions opts;
        opts.exePath = W("/nonexistent/ecapture-worker-test-does-not-exist");
        opts.args = {W("--worker"), tx.endpoint.Token(), HexOf(tx.nonce),
                     std::to_wstring(static_cast<unsigned long>(::getpid()))};
        Check(!hostproc::SpawnChild(opts, &tx.endpoint, &tx.child, &e),
              "映像不存在：当场失败，不起进程、不试别的名字");
        Check(e == static_cast<NativeErrorCode>(ENOENT), "原生码如实带 ENOENT（只作核对）");
        tx.endpoint.Close();
    }
    {
        // 没人连（Linux 侧服务的连接动作在任何 exec 之前由孩子做出，所以"端点空等"只会
        // 出现在辅助进程压根没起的现场）：accept 到点 -> kTimeout，监听口收干净。
        Tx tx;
        NativeErrorCode e = 0;
        hostproc::MakeNonce(&tx.nonce);
        Check(hostproc::CreateEndpoint(tx.nonce, &tx.endpoint, &e), "建了一个没人会连的端点");
        hostproc::AcceptFault fault = hostproc::AcceptFault::kChannel;
        Check(!hostproc::AcceptChannel(&tx.endpoint, 4242u,
                                       Deadline::FromTotalMs(200), &fault, &tx.channel, &e) &&
                  fault == hostproc::AcceptFault::kTimeout,
              "握手段到点如实报 kTimeout（预算分类继续由调用方判，见 7/10 节）");
        Check(!tx.channel.IsOpen(), "到点之后没有半条通道留在手里");
        tx.endpoint.Close();
        Check(!tx.endpoint.IsOpen(), "监听口被明确关掉（不漏 fd）");
    }
    Check(FdCount() == fdBefore, "本节结束：描述符回到基线");
}

int RunDriver() {
    if (!hostproc::SelfImagePath(&g_selfPath) || g_selfPath.empty()) {
        std::printf("前置不成立：问不出本测试 exe 的路径（/proc/self/exe）\n");
        return 2;
    }
    std::printf("辅助进程通信与超时取消判据：%s\n", S(g_selfPath).c_str());
    SectionFramingUnits();
    SectionWatchdogUnits();
    SectionHappyPath();
    SectionBulkFrame();
    SectionIdentityRefusals();
    SectionMalformedMessages();
    SectionTimeoutCancel();
    SectionDescendantCleanup();
    SectionBudgetSelfExit();
    SectionParentDeath();
    SectionAbnormalExit();
    SectionStdioParallel();
    SectionBudgetPassing();
    SectionSpawnFailureHygiene();
    std::printf("\nworker-ipc: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

// --relay-worker <claimedParentPid>：驱动 fork 出来的中继父进程 —— 它自己起一个辅助进程
// （辅助进程的直接父进程 = 中继），但 argv 里自称驱动的 PID；辅助进程的第 7 步必须拒（67）。
// 中继把辅助进程的退出码原样作为自己的退出码转给驱动。
static int RunRelayMode(const char* claimedPid) {
    std::wstring self;
    if (!hostproc::SelfImagePath(&self)) _exit(120);
    Tx tx;
    NativeErrorCode e = 0;
    if (!hostproc::MakeNonce(&tx.nonce)) _exit(121);
    if (!hostproc::CreateEndpoint(tx.nonce, &tx.endpoint, &e)) _exit(122);
    hostproc::SpawnOptions opts;
    opts.exePath = self;
    opts.args = {W("--worker"), tx.endpoint.Token(), HexOf(tx.nonce), W(claimedPid)};
    if (!hostproc::SpawnChild(opts, &tx.endpoint, &tx.child, &e)) _exit(123);
    // 不 accept 也不发任务：辅助进程会先在绑定校验那一步退。等它进终态，转告退出码。
    hostproc::ReapChild(&tx.child, 4000, kHelperExitReaped);
    const int code = hostproc::ChildExitCode(tx.child);
    _exit(code < 0 ? 124 : code);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--worker") == 0) {
        return RunWorkerMode(argc, argv);
    }
    if (argc == 3 && std::strcmp(argv[1], "--relay-worker") == 0) {
        return RunRelayMode(argv[2]);
    }
    return RunDriver();
}
