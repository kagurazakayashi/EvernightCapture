// Worker 连接状态的真命名管道时序判据（F03：抢先连接不得被误报成超时）。
//
// 为什么用真管道、又为什么放在这个测试可执行文件里：F03 的缺陷本体是**时序** ——
// 客户端（辅助进程）赶在服务端 ConnectNamedPipe 之前完成 CreateFile 时，
// ConnectNamedPipe 返回 ERROR_PIPE_CONNECTED：这是一次已经成立的连接，没有挂起 I/O，
// 事件不会由系统置起（官方文档 Return value 一节明说 "there is a good connection ...
// even though the function returns zero"）。基线把它映射成 kSyncDone 照样等事件，
// 抢先连接因此要等满整个预算再加取消宽限才被误报成超时。这条时序用假后端判不了
// （判的是映射之后的状态机，不是"谁先谁后"本身），而发布版 ECAPTURE.EXE 里不许有
// 任何控制连接顺序的开关 —— 所以这里用真命名管道 + 测试自建进程做判据，
// 链的是生产状态机本体（WorkerIo.cpp 的 RunOverlappedOp + StartOf/ConnectStartOf +
// Win32Backend），不是它的抄本。
//
// 顺序控制是确定性的，不靠循环碰运气：
//   * "客户端先连上"由测试代码**先**完成 CreateFile 并拿到回话、**再**发起连接来保证 ——
//     ERROR_PIPE_CONNECTED 分支在这一步必然出现（判据把发起阶段看到的状态记下来核对）；
//   * "确有挂起操作"由"当场根本没有客户端"来保证 —— 发起必然回 ERROR_IO_PENDING；
//   * 异端判据起一个真的外部进程（本测试 exe 自己以 --f03-peer 模式再跑一份）连上来，
//     身份查询交回的就是那个 PID，与"我刚起的那个 worker"的比对因此拿到的是非自身值。
// 末尾另有一小段真实并发压力（同一套现场连打若干轮），补确定性判据之外的抖动余量。
//
// 只用 C 风格的 printf 汇报，判据写在断言里；任何一条不过就返回非 0。
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../src/WorkerIo.h"

namespace ecapture {
namespace {

using namespace workerio;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}

// ---------------------------------------------------------------------------
// 真管道的小工具：全部走生产封装（RunOverlappedOp + StartOf/ConnectStartOf +
// Win32Backend），测试只负责安排"谁先谁后"。
// ---------------------------------------------------------------------------
constexpr uint32_t kPipeBufBytes = 4096;
constexpr DWORD kCancelGraceMs = 2000;   // 与生产 Transaction 同一量级的收尾宽限

std::wstring MakePipeName(int serial) {
    wchar_t buf[128];
    swprintf(buf, 128, L"\\\\.\\pipe\\ecapture-f03-%08lx-%d", GetCurrentProcessId(), serial);
    return buf;
}

HANDLE NewServerPipe(const std::wstring& name) {
    return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1, kPipeBufBytes, kPipeBufBytes, 0, nullptr);
}

HANDLE OpenClient(const std::wstring& name) {
    return CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

// 收尾：登记表里还有未进终态的操作时先排干再关（与生产 Finish 同一规矩），
// 免得测试自己制造"带着未决 I/O 关句柄"的假现场。
void CloseServerPipe(HANDLE pipe) {
    if (HasPendingOps(pipe)) DrainPendingOps(pipe, kCancelGraceMs);
    CloseHandle(pipe);
}

// 过生产状态机发起一次连接；seen 记录发起阶段的四态判定（确定性核对用）。
IoStep ConnectViaMachine(HANDLE pipe, DWORD waitMs, StartState* seen) {
    return RunOverlappedOp(
        Win32Backend(), pipe, 0, waitMs, kCancelGraceMs,
        [&](OVERLAPPED& ov, uint8_t*, uint32_t) {
            const StartResult r = ConnectStartOf(ConnectNamedPipe(pipe, &ov));
            if (seen) *seen = r.state;
            return r;
        },
        nullptr);
}

IoStep WriteViaMachine(HANDLE pipe, const std::vector<uint8_t>& data, DWORD waitMs) {
    return RunOverlappedOp(
        Win32Backend(), pipe, static_cast<uint32_t>(data.size()), waitMs, kCancelGraceMs,
        [&](OVERLAPPED& ov, uint8_t* buf, uint32_t cap) {
            std::memcpy(buf, data.data(), cap);
            return StartOf(WriteFile(pipe, buf, cap, nullptr, &ov));
        },
        nullptr);
}

IoStep ReadViaMachine(HANDLE pipe, std::vector<uint8_t>* out, DWORD waitMs) {
    return RunOverlappedOp(
        Win32Backend(), pipe, kPipeBufBytes, waitMs, kCancelGraceMs,
        [&](OVERLAPPED& ov, uint8_t* buf, uint32_t cap) {
            return StartOf(ReadFile(pipe, buf, cap, nullptr, &ov));
        },
        out);
}

bool ReadBlocking(HANDLE h, uint8_t* buf, size_t size) {
    size_t got = 0;
    while (got < size) {
        DWORD one = 0;
        if (!ReadFile(h, buf + got, static_cast<DWORD>(size - got), &one, nullptr)) return false;
        if (one == 0) return false;
        got += one;
    }
    return true;
}

bool WriteBlocking(HANDLE h, const uint8_t* buf, size_t size) {
    size_t put = 0;
    while (put < size) {
        DWORD one = 0;
        if (!WriteFile(h, buf + put, static_cast<DWORD>(size - put), &one, nullptr)) return false;
        put += one;
    }
    return true;
}

// 单调毫秒表：判"已连接分支立刻返回"用，不当判据的判据（真正的定性在状态与 seen 上）。
long long NowMs() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<long long>(t.QuadPart * 1000 / freq.QuadPart);
}

// ---------------------------------------------------------------------------
// 判据一：客户端抢先连上 —— 已连接分支直接成为完成态，立刻能进入任务交换。
// 顺序是测试安排的：CreateFile 先拿到回话，服务端**之后**才发起 ConnectNamedPipe，
// 所以 ERROR_PIPE_CONNECTED 这一档必然出现（seen 核对）。
// 基线在这会等满 waitMs 再走取消 + 宽限：这里 waitMs 取 2000，判"远小于它"，
// 再钉住状态本身（done、非超时、非未决）—— 状态判定不依赖计时，计时只是外加的哨兵。
// ---------------------------------------------------------------------------
void CheckPreconnectedExchange() {
    const std::wstring name = MakePipeName(1);
    const HANDLE server = NewServerPipe(name);
    Check(server != INVALID_HANDLE_VALUE, "服务端建得出测试管道");
    if (server == INVALID_HANDLE_VALUE) return;
    const HANDLE client = OpenClient(name);   // 先完成 CreateFile：此刻服务端还没调用过连接
    Check(client != INVALID_HANDLE_VALUE, "测试客户端抢先连上（服务端未调用 ConnectNamedPipe）");
    if (client == INVALID_HANDLE_VALUE) { CloseServerPipe(server); return; }

    StartState seen = StartState::kSyncFailed;
    const long long t0 = NowMs();
    const IoStep s = ConnectViaMachine(server, 2000, &seen);
    const long long elapsed = NowMs() - t0;

    Check(seen == StartState::kAlreadyConnected,
          "发起阶段确实落在 ERROR_PIPE_CONNECTED 这一档（确定性时序成立）");
    Check(s.done && !s.timedOut && !s.unresolved && s.gle == 0,
          "已连接分支直接成为完成态：不是超时、不是未决、不是失败");
    Check(elapsed < 1000,
          "已连接分支没有等那个不会置起的事件（基线在这里必然花满 2000ms 预算再叠加取消宽限）");
    Check(!HasPendingOps(server), "已连接分支不留任何登记表记录");

    // 立刻进入任务交换：服务端交任务、客户端交回结果，都走生产状态机/生产字节语义。
    const std::vector<uint8_t> task = {'T', 'A', 'S', 'K', '1', '2', '3', '4'};
    const std::vector<uint8_t> reply = {'R', 'E', 'P', 'L', 'Y', '5', '6', '7'};
    const IoStep w = WriteViaMachine(server, task, 2000);
    Check(w.done && w.transferred == task.size(), "连接成立后服务端立刻写得出去任务");
    uint8_t got[8] = {};
    Check(ReadBlocking(client, got, 8) && std::memcmp(got, task.data(), 8) == 0,
          "客户端收到刚交出的任务");
    Check(WriteBlocking(client, reply.data(), reply.size()), "客户端写得回应答");
    std::vector<uint8_t> back;
    const IoStep r = ReadViaMachine(server, &back, 2000);
    Check(r.done && back == reply, "服务端立刻收得到应答 —— 抢先连接不再需要'先超时一次'");

    CloseHandle(client);
    CloseServerPipe(server);
}

// ---------------------------------------------------------------------------
// 判据二：服务端先等待、客户端晚到 —— 连接照样完成，且能马上交换数据。
// 分支本身（挂起完成 / 恰好又抢了先）由调度决定，两条都是合法成功；
// "确有挂起操作"这一档的确定性起点在判据三钉死，完成与取消的处置由离线假后端判据判顺序。
// ---------------------------------------------------------------------------
void CheckLateClientCompletes() {
    const std::wstring name = MakePipeName(2);
    const HANDLE server = NewServerPipe(name);
    if (server == INVALID_HANDLE_VALUE) { Check(false, "判据二建不出服务端管道"); return; }

    // seen/s 由服务端线程写、主线程在 join 之后读：join 就是同步点，不需要原子量。
    StartState seen = StartState::kSyncFailed;
    IoStep s;
    std::thread srv([&] { s = ConnectViaMachine(server, 8000, &seen); });
    Sleep(30);   // 测试侧的调度余量（发布版没有这条）：让服务端先进入 ConnectNamedPipe
    const HANDLE client = OpenClient(name);
    srv.join();

    Check(client != INVALID_HANDLE_VALUE, "晚到的客户端连得上");
    Check(s.done && !s.timedOut && !s.unresolved, "服务端先等待：连接最终进入完成态");
    std::printf("  （判据二落在：%s）\n",
                seen == StartState::kPending ? "挂起后由客户端触发完成" : "客户端抢在先（同判据一）");
    const std::vector<uint8_t> ping = {'P', 'I', 'N', 'G'};
    Check(WriteViaMachine(server, ping, 2000).done, "完成态之后服务端写得出去");
    uint8_t got[4] = {};
    Check(ReadBlocking(client, got, 4) && std::memcmp(got, ping.data(), 4) == 0, "客户端收到");
    CloseHandle(client);
    CloseServerPipe(server);
}

// ---------------------------------------------------------------------------
// 判据三：连接超时 —— 只有确有挂起操作才等待，到点按 F02 的机制安全取消。
// 现场是"当场没有任何客户端"：发起必然回 ERROR_IO_PENDING（seen 核对），
// 到点必须走"请求取消 -> 宽限内确认终态"，而不是提前 SetEvent 把假连接混过去。
// ---------------------------------------------------------------------------
void CheckConnectTimeoutCancels() {
    const std::wstring name = MakePipeName(3);
    const HANDLE server = NewServerPipe(name);
    if (server == INVALID_HANDLE_VALUE) { Check(false, "判据三建不出服务端管道"); return; }

    StartState seen = StartState::kSyncFailed;
    const IoStep s = ConnectViaMachine(server, 200, &seen);
    Check(seen == StartState::kPending,
          "没有客户端时发起必为挂起（这才是'等待内核通知'的那条路）");
    Check(s.timedOut && !s.done && !s.unresolved,
          "连接超时：如实 timedOut，取消已在宽限内确认终态");
    Check(!HasPendingOps(server), "超时取消后不留登记表记录，句柄可安全关闭（F02 生命周期回归）");

    // 取消过的那条管道不该"诈尸"连上后来的客户端之外的东西：这里补一刀 ——
    // 新客户端连上来后服务端**重新**发起连接仍应正常完成（上一次取消没把实例弄坏）。
    const HANDLE client = OpenClient(name);
    StartState seen2 = StartState::kSyncFailed;
    const IoStep again = ConnectViaMachine(server, 2000, &seen2);
    Check(client != INVALID_HANDLE_VALUE && again.done && !again.timedOut,
          "取消后同一管道实例重新连接仍正常（取消只结束了那一次等待）");
    if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
    CloseServerPipe(server);
}

// ---------------------------------------------------------------------------
// 判据四：异端对端 —— 连接成立不代表身份可信。
// 用本测试 exe 以 --f03-peer 再起一个真进程连上来：GetNamedPipeClientProcessId 交回的
// 是那个进程的 PID。生产 Transaction::Start 拿它比对"我刚起的 worker"，不等就拒绝 ——
// 这里判的是身份查询这条前置事实在"已连接/晚到"两条路上都拿得到、且拿到的不是自己人。
// ---------------------------------------------------------------------------
void CheckForeignPeerIsNotTrusted() {
    const std::wstring name = MakePipeName(4);
    const HANDLE server = NewServerPipe(name);
    if (server == INVALID_HANDLE_VALUE) { Check(false, "判据四建不出服务端管道"); return; }

    wchar_t self[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    std::wstring cmd;
    if (n == 0 || n >= std::size(self)) {
        Check(false, "判据四拿不到测试 exe 自身路径");
        CloseServerPipe(server);
        return;
    }
    cmd = L"\"" + std::wstring(self, n) + L"\" --f03-peer " + name;   // 管道名不含空格
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW：外部进程是个纯测试傀儡，不弹任何窗口，更不碰任何桌面内容。
    const bool spawned = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!spawned) {
        Check(false, "判据四起不了外部客户端进程");
        CloseServerPipe(server);
        return;
    }

    StartState seen = StartState::kSyncFailed;
    const IoStep s = ConnectViaMachine(server, 8000, &seen);
    Check(s.done && !s.timedOut, "外部进程连上后进入完成态（无论挂起等待还是抢先一档）");

    DWORD peer = 0;
    const bool queried = GetNamedPipeClientProcessId(server, &peer);
    Check(queried && peer == pi.dwProcessId,
          "内核交回的对端 PID 就是那个外部进程（连接成立 ≠ 身份可信的前提是能问出是谁）");
    Check(peer != GetCurrentProcessId(),
          "对端不是本进程：生产代码里它与'我刚起的 worker PID'的比对将判否 —— 任务一个字节都不交");

    // 对端握手字节：证明这条连接确实归那个进程在用（它在等待期间一直持着句柄）。
    std::vector<uint8_t> hello;
    const IoStep r = ReadViaMachine(server, &hello, 3000);
    Check(r.done && hello.size() == 1 && hello[0] == 0xE7, "对端握手字节送达");

    TerminateProcess(pi.hProcess, 0);   // 只结束本测试自己起的这个进程
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseServerPipe(server);
}

// ---------------------------------------------------------------------------
// 判据五：连接后断管 —— 已连接分支交出的完成态与"对端走了"的字节语义互不混淆。
// 真管道对"客户端已关句柄"有两种都合法的表现：0 字节完成（Worker.cpp 的 ReadExact 拿它
// 翻成 ERROR_BROKEN_PIPE），或发起就同步回 ERROR_BROKEN_PIPE。两种都必须被认成断管，
// 都不许伪装成成功数据、也不许变成一次假超时。
// ---------------------------------------------------------------------------
void CheckBrokenPipeAfterConnect() {
    const std::wstring name = MakePipeName(5);
    const HANDLE server = NewServerPipe(name);
    if (server == INVALID_HANDLE_VALUE) { Check(false, "判据五建不出服务端管道"); return; }
    const HANDLE client = OpenClient(name);
    if (client == INVALID_HANDLE_VALUE) {
        Check(false, "判据五客户端连不上");
        CloseServerPipe(server);
        return;
    }
    StartState seen = StartState::kSyncFailed;
    Check(ConnectViaMachine(server, 2000, &seen).done && seen == StartState::kAlreadyConnected,
          "判据五的连接同样落在已连接档（先连后接）");

    CloseHandle(client);   // 连接成立之后对端关管
    std::vector<uint8_t> got;
    const IoStep r = ReadViaMachine(server, &got, 2000);
    const bool zeroComplete = r.done && r.transferred == 0 && got.empty();
    const bool syncBroken = !r.done && !r.timedOut && r.gle == ERROR_BROKEN_PIPE;
    Check(zeroComplete || syncBroken,
          "断管被认成断管（0 字节完成或同步 ERROR_BROKEN_PIPE），不伪装成功也不变假超时");
    Check(!r.timedOut && !HasPendingOps(server), "断管路径不烧预算、不留登记表记录");
    CloseServerPipe(server);
}

// ---------------------------------------------------------------------------
// 判据六：ConnectStartOf 的四态分派表 —— 已连接、同步成功、挂起、真实失败逐类分开；
// ERROR_PIPE_CONNECTED 不是"可以忽略所有错误"的通行证。
// 直接在调用约定那一层钉：先立好 GetLastError，再喂给封装，核对落进哪一档。
// ---------------------------------------------------------------------------
void CheckConnectStartOfTable() {
    {
        SetLastError(ERROR_SUCCESS);
        const StartResult r = ConnectStartOf(TRUE);
        Check(r.state == StartState::kSyncDone && r.gle == 0, "返回非 0 = 同步完成档");
    }
    {
        SetLastError(ERROR_IO_PENDING);
        const StartResult r = ConnectStartOf(FALSE);
        Check(r.state == StartState::kPending, "ERROR_IO_PENDING = 确有挂起操作档");
    }
    {
        SetLastError(ERROR_PIPE_CONNECTED);
        const StartResult r = ConnectStartOf(FALSE);
        Check(r.state == StartState::kAlreadyConnected && r.gle == 0,
              "ERROR_PIPE_CONNECTED = 已连接档（成功，但不是挂起操作）");
    }
    {   // 其余错误一律照实失败：这一档不能顺手把别的东西也放行。
        SetLastError(ERROR_NO_DATA);
        const StartResult a = ConnectStartOf(FALSE);
        SetLastError(ERROR_ACCESS_DENIED);
        const StartResult b = ConnectStartOf(FALSE);
        SetLastError(ERROR_INVALID_PARAMETER);
        const StartResult c = ConnectStartOf(FALSE);
        Check(a.state == StartState::kSyncFailed && a.gle == ERROR_NO_DATA &&
                  b.state == StartState::kSyncFailed && b.gle == ERROR_ACCESS_DENIED &&
                  c.state == StartState::kSyncFailed && c.gle == ERROR_INVALID_PARAMETER,
              "其他错误码逐条落进真实失败档，错误值原样带上");
    }
    {   // StartOf（读写那条路）没有"已连接"档：就算 GetLastError 里躺着 ERROR_PIPE_CONNECTED
        // 也不许把读写调用判成已连接 —— 两把钥匙各开各的锁。
        SetLastError(ERROR_PIPE_CONNECTED);
        const StartResult r = StartOf(FALSE);
        Check(r.state == StartState::kSyncFailed && r.gle == ERROR_PIPE_CONNECTED,
              "StartOf 不认 ERROR_PIPE_CONNECTED：读写路径的真失败不被连接档吞掉");
    }
}

// ---------------------------------------------------------------------------
// 判据七：少量真实并发压力 —— 同一套"客户端抢先"现场连打多轮（真管道、真状态机）。
// 这是补刀不是主判据：确定性判据已经把分支钉死，这里只收调度抖动下的稳定性。
// ---------------------------------------------------------------------------
void CheckPreconnectedStress() {
    constexpr int kRounds = 24;
    std::atomic<int> ok{0};
    std::atomic<int> bad{0};
    std::vector<std::thread> workers;
    workers.reserve(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        workers.emplace_back([i, &ok, &bad] {
            const std::wstring name = MakePipeName(100 + i);
            const HANDLE server = NewServerPipe(name);
            if (server == INVALID_HANDLE_VALUE) { ++bad; return; }
            const HANDLE client = OpenClient(name);
            if (client == INVALID_HANDLE_VALUE) { ++bad; CloseServerPipe(server); return; }
            const IoStep s = ConnectViaMachine(server, 2000, nullptr);
            const std::vector<uint8_t> ping = {'x'};
            bool pass = s.done && !s.timedOut && WriteViaMachine(server, ping, 2000).done;
            uint8_t got = 0;
            if (pass) pass = ReadBlocking(client, &got, 1) && got == 'x';
            if (pass) {   // 各轮之间互不串扰：这一轮的连接与交换全部自己收干净
                ++ok;
            } else {
                ++bad;
            }
            CloseHandle(client);
            CloseServerPipe(server);
        });
    }
    for (auto& t : workers) t.join();
    Check(bad == 0 && ok == kRounds,
          "抢先连接现场连打 24 轮全部立刻完成（真并发压力，非碰运气主判据）");
}

}  // namespace

// isolation_state.cpp 的 main 在 --f03-peer 模式下调进来：判据四的那个"外部客户端"
// 就是本测试 exe 自己。只连管、写一个握手字节、然后持着句柄等测试方结束我们（有界兜底）。
int RunWorkerConnectPeerMode(const wchar_t* pipeName) {
    const HANDLE h = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 9;
    const uint8_t hello = 0xE7;
    DWORD written = 0;
    if (!WriteFile(h, &hello, 1, &written, nullptr) || written != 1) {
        CloseHandle(h);
        return 8;
    }
    // 正常下场是判据四核对完身份后把我们结束掉；这条兜底保证测试崩溃时不遗留进程。
    Sleep(20000);
    CloseHandle(h);
    return 0;
}

int RunWorkerConnectStateChecks(int* checksOut, int* failuresOut) {
    std::printf("Worker 连接时序（真命名管道）判据\n");
    CheckPreconnectedExchange();
    CheckLateClientCompletes();
    CheckConnectTimeoutCancels();
    CheckForeignPeerIsNotTrusted();
    CheckBrokenPipeAfterConnect();
    CheckConnectStartOfTable();
    CheckPreconnectedStress();
    if (checksOut) *checksOut = g_checks;
    if (failuresOut) *failuresOut = g_failures;
    return g_failures;
}

}  // namespace ecapture
