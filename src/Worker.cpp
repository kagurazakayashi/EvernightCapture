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

#include <winrt/base.h>   // 辅助任务里也可能抛出 hresult_error，要换成 ASCII 细节交回

#include "CaptureDwm.h"
#include "CapturePrintWindow.h"
#include "HostProcess.h"
#include "ImageOps.h"
#include "Lang.h"
#include "WorkerProtocol.h"
#include "WorkerTiming.h"

namespace ecapture {
namespace {

using namespace worker;

// 工作模式标记。刻意写成"单斜杠 + 一个不会被当成选项的名字"，并且不写进选项目录：
// 正常命令行解析遇到它会报 cli.unknown_option，只有 wmain 最前面那一次判断才认它。
constexpr const wchar_t* kWorkerSwitch = L"/ecapture-worker";

constexpr uint32_t kReapGraceMs = 2000u;    // 交回结果后给辅助进程的收尾时间
//（取消排干宽限在 HostProcess.h 一处定义；进程/管道/作业这些 OS 动作全部封装在
//  hostproc 平台服务里 —— Windows 实现见 platform/windows/WinHostProcess.cpp，
//  Linux 实现见 platform/linux/PosixHostProcess.cpp，本文件只做交易编排与诊断映射。）
// 上面这段收尾宽限是"父进程愿意多等一会儿"，不是期限；四类期限本身
// （总预算 / 默认隔离上限 / 启动握手段 / 辅助进程执行段）的分工与换算都在 WorkerTiming.h
// 与 core/HelperTiming.h。

// 一次隔离调用的结局。五类失败给五条不同的诊断：调用方要能分清
// "辅助进程没起来"、"起来了却没在握手段之内接上管道"、"接上了而中途断掉"、
// "消息形状不对"和"到点了还没结果"。前四类是本工具自己的机制故障（共用
// capture.worker_failed），最后一条才是期限真的烧光（match.timeout / capture.timeout）
// —— 把前者说成后者，就是教调用方去加大一个根本不解这个问题的数字。
enum class Call { kDone, kTimedOut, kSpawnFailed, kHandshakeFailed, kChannelFailed, kProtocolFailed };

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

// Windows argv 的引号规则已随命令行进服务实现（WinHostProcess.cpp 的 QuoteArg）；
// 这里剩下的两处文本换算是交易编排与诊断映射自己要用的。
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
// 一次交易：建端点 -> 起辅助进程 -> 连上并核对身份 -> 交任务 -> 收应答 -> 收尸
// 进程创建、句柄、管道 I/O、等待与终止全部走 hostproc 平台服务；这里只编排交易，
// 并把服务的结局翻成五种调用级失败（下面 Call 那五条）。
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
    // 值是在收尸等过之后取走的，所以调用方必须先收尸再问（见 Transact 里的大括号）。
    int ChildExit() const { return childExit_; }

private:
    void Finish();

    uint64_t nonce_ = 0;
    uint16_t kind_ = 0;
    hostproc::IpcEndpoint endpoint_;
    hostproc::ChildProcess child_;
    hostproc::IpcChannel channel_;
    DWORD gle_ = 0;
    int childExit_ = -1;
    bool finished_ = false;
};

Call Transaction::Start(uint16_t kind, const std::vector<uint8_t>& task, const Deadline& dl) {
    kind_ = kind;
    NativeErrorCode e = kNativeErrorNone;
    if (!hostproc::MakeNonce(&nonce_)) {
        gle_ = ERROR_NO_TOKEN;   // 拿不到随机数就没法把这次交易与别的区分开：不起进程
        return Call::kSpawnFailed;
    }
    if (!hostproc::CreateEndpoint(nonce_, &endpoint_, &e)) {
        gle_ = e;
        return Call::kSpawnFailed;
    }
    std::wstring self;
    if (!hostproc::SelfImagePath(&self) || self.empty()) {
        gle_ = GetLastError();
        return Call::kSpawnFailed;
    }
    // 任务本体走管道，命令行只带"从哪儿取任务"的坐标：
    // 辅助进程能读到的参数里没有输出路径，也没有任何能被执行的文字。参数按位置直接
    // 交给 CreateProcessW（引号规则在服务实现里），全程不拼接、不经过 shell。
    hostproc::SpawnOptions opts;
    opts.exePath = self;
    opts.args = {std::wstring(kWorkerSwitch), endpoint_.Token(), HexOf(nonce_),
                 std::to_wstring(GetCurrentProcessId())};
    if (!hostproc::SpawnChild(opts, &endpoint_, &child_, &e)) {
        gle_ = e;
        return Call::kSpawnFailed;
    }

    // 把管道交出去之前先确认"连上来的是我刚起的那个进程"（名字里带了本次的 PID 与 nonce，
    // 但"是谁连的"由内核回答，不能靠名字）：连接的四种结局与"抢先连接不算超时"的判据
    // 都在服务实现里（WinHostProcess.cpp 的 AcceptChannel / Linux 的 SO_PEERCRED 核对），
    // 逐条语义与原 Worker.cpp 一致。
    // 连管这一步只肯等"启动/握手"那一段（HandshakeWaitMs 已经把它压进这一笔交易的剩余预算
    // 之内）；把整份剩余预算压在连管上，会让一个根本没起来的辅助进程把用户的期限烧光，
    // 还能报成"期限耗尽"。连接成功与否都不重新领一份预算：这一段花掉的仍然是那一份总预算里的毫秒。
    const Deadline hsWait = Deadline::FromTotalMs(HandshakeWaitMs(dl));
    hostproc::AcceptFault fault = hostproc::AcceptFault::kChannel;
    if (!hostproc::AcceptChannel(&endpoint_, child_.Pid(), hsWait, &fault, &channel_, &e)) {
        gle_ = e;
        switch (fault) {
            case hostproc::AcceptFault::kTimeout:
                // 同一次"没等到连接"分两种下场，各归各的类：预算真的花完了才算期限耗尽，
                // 否则算"辅助进程没接上来"这条机制故障。
                if (ClassifyHandshakeTimeout(dl) == HandshakeFault::kBudgetSpent) return Call::kTimedOut;
                return Call::kHandshakeFailed;   // 握手段到点，预算还剩着
            case hostproc::AcceptFault::kPeerUnknown:
            case hostproc::AcceptFault::kPeerMismatch:
                // 内核答不出"是谁连的"、或连上来的不是我起的那个：都是身份核对不过，
                // 归协议族（与原 Worker.cpp 的分类一致）。
                return Call::kProtocolFailed;
            case hostproc::AcceptFault::kChannel:
            default:
                return Call::kChannelFailed;
        }
    }

    Header h{kMagic, kProtocolVersion, kind, static_cast<uint32_t>(task.size()), nonce_};
    std::vector<uint8_t> framed;
    if (!EncodeHeader(h, &framed)) { gle_ = ERROR_BAD_FORMAT; return Call::kProtocolFailed; }
    framed.insert(framed.end(), task.begin(), task.end());

    const hostproc::ChannelIo w = hostproc::WriteAll(&channel_, framed.data(), framed.size(), dl);
    if (w.timedOut) return Call::kTimedOut;
    if (!w.ok) { gle_ = w.code; return Call::kChannelFailed; }
    return Call::kDone;
}

Call Transaction::ReadReply(Reply* out, uint16_t expectKind, const Deadline& dl) {
    std::vector<uint8_t> header(kHeaderSize);
    const hostproc::ChannelIo hs = hostproc::ReadExact(&channel_, header.data(), header.size(), dl);
    if (hs.timedOut) return Call::kTimedOut;
    if (!hs.ok) { gle_ = hs.code; return Call::kChannelFailed; }

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
        const hostproc::ChannelIo ps =
            hostproc::ReadExact(&channel_, payload.data(), payload.size(), dl);
        if (ps.timedOut) return Call::kTimedOut;
        if (!ps.ok) { gle_ = ps.code; return Call::kChannelFailed; }
    }
    if (!DecodeReply(payload.data(), payload.size(), out)) {
        gle_ = ERROR_BAD_FORMAT;
        return Call::kProtocolFailed;
    }
    return Call::kDone;
}

void Transaction::Finish() {
    if (finished_) return;
    finished_ = true;
    // 固定次序（关管道 -> 收尸 -> 排干/收养 -> 关作业）整个搬进服务实现，见
    // hostproc::FinishTransaction 与 WinHostProcess.cpp 里那份逐字保留的正文。
    hostproc::FinishTransaction(&child_, &channel_, kReapGraceMs, hostproc::kCancelDrainGraceMs);
    childExit_ = hostproc::ChildExitCode(child_);
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

// 绑定校验的原料（"谁起的我 / 我还活着吗 / 同一会话吗 / 直接父进程是谁 /
// '父进程一关作业我就一起结束'装上了没有"）全部走 hostproc 平台服务的身份问答；
// 任何一条问不出来都按"不成立"处理（宁可拒绝执行，不猜）。

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
    if (!hostproc::EndpointTokenLooksOurs(a.pipe)) return 1;
    const DWORD myPid = GetCurrentProcessId();
    if (a.parentPid == 0 || a.parentPid == myPid) return 2;
    if (!hostproc::ProcessAlive(a.parentPid)) return 3;
    if (!hostproc::SameLoginSession(a.parentPid)) return 4;
    // 起我的必须是同一个 ECAPTURE.EXE —— 这条把"任何人都能直接跑辅助模式"挡在门外
    std::wstring parent;
    std::wstring self;
    if (!hostproc::ImagePathOf(a.parentPid, &parent) || !hostproc::SelfImagePath(&self)) return 5;
    if (parent.empty() || self.empty()) return 5;
    if (_wcsicmp(parent.c_str(), self.c_str()) != 0) return 6;
    if (hostproc::ImmediateParentPid() != a.parentPid) return 7;
    if (!hostproc::ParentCleanupArmed()) return 8;
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

bool ReadTaskBlocking(hostproc::IpcChannel* channel, uint64_t nonce, Task* task,
                      uint16_t* kind) {
    uint8_t header[kHeaderSize] = {};
    if (!hostproc::ReadExactBlocking(channel, header, sizeof(header)).ok) return false;
    Header h{};
    if (!DecodeHeader(header, sizeof(header), &h)) return false;
    if (h.nonce != nonce) return false;   // 不是交给我这次的
    if (h.payloadLen > kMaxTaskPayload) return false;
    std::vector<uint8_t> payload(h.payloadLen);
    if (!payload.empty() &&
        !hostproc::ReadExactBlocking(channel, payload.data(), payload.size()).ok) return false;
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
            req.onScreens = FromNativeRects(task.onScreens);
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

    // 连回父进程走平台服务（命名管道句柄只活在 hostproc 实现以内）。
    hostproc::IpcChannel channel;
    NativeErrorCode channelErr = kNativeErrorNone;
    if (!hostproc::ConnectToParent(args.pipe, &channel, &channelErr)) ExitWithoutReply();

    Task task;
    uint16_t kind = 0;
    if (!ReadTaskBlocking(&channel, args.nonce, &task, &kind)) {
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
            return kHelperExitProtocol;
        }
    }
    const Header h{kMagic, kProtocolVersion, kind, static_cast<uint32_t>(payload.size()),
                   args.nonce};
    std::vector<uint8_t> framed;
    if (!EncodeHeader(h, &framed)) {
        return kHelperExitProtocol;
    }
    framed.insert(framed.end(), payload.begin(), payload.end());

    // 阻塞写回（辅助进程没有第二份预算；父进程断开时如实写不进去）。
    // 通道句柄随作用域结束关闭，与原先在出口处 CloseHandle(pipe) 同一效果。
    return hostproc::WriteAllBlocking(&channel, framed.data(), framed.size()).ok
               ? 0
               : kHelperExitProtocol;
}

}  // namespace ecapture
