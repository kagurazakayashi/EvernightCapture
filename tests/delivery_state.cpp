// 交付那一步（写文件 / 写标准输出）与"期限合规"分开记账的离线判据。
//
// 为什么要单独一个可执行文件：这一批判据要的现场全排在一次真实写入的两端 ——
//   * 预算恰好在"提交之前"就用尽（于是一个字节都不该开始发）；
//   * 文件已经改名到目标名之后预算才跨过（图已经在磁盘上，超时不许把它删掉、
//     也不许让 captured 少一个）；
//   * 标准输出的字节全部到达之后预算才跨过（同一条边界，另一头是那条管道）；
//   * 半段流留在管道里、失败的原子改名、--no-overwrite 撞已存在（都没交付，各留自己的原因）。
// 真机上这些要么安排不出来（本机没有一块能被测试变慢的盘，也不许为了造慢盘去动用户的存储
// 设置），要么得拿别人的应用当管道对面。所以出口与时钟由这里注入，被 judged 的本体仍然是
// 生产的那两份：src/Delivery.cpp 的编排 + src/FileSave.cpp 的原子写。
//
// 于是每一判都同时看两头：磁盘 / 管道里的**交付事实**（文件在不在、多少个字节、旧文件有没有
// 被破坏、半段流到底有多长）与 JSON 那一份**记账**（images 条目、notes、errors 的码与 stage、
// 退出码）。只断言预算的读数（dl.Spent()）判不到这次要修的东西 —— 那些数本来就一直是对的，
// 错的是它们被读的时刻与被记成什么。
//
// 只用 C 风格的 printf 汇报，判据写在断言里；任何一条不过就返回非 0。
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../src/Capture.h"
#include "../src/CliOptions.h"
#include "../src/Deadline.h"
#include "../src/Delivery.h"
#include "../src/FileSave.h"
#include "../src/Lang.h"

namespace {

using namespace ecapture;

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

void CheckEqU(uint64_t got, uint64_t want, const char* what) {
    Check(got == want, what);
    if (got != want) std::printf("        实测=%llu 期望=%llu\n",
                                 static_cast<unsigned long long>(got),
                                 static_cast<unsigned long long>(want));
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// ---------------------------------------------------------------------------
// 假时钟：预算与 elapsedMs 共用这一个读数，于是"提交这一步自己花了多久"与
// "预算在哪一刻跨过"是同一个数轴上两件事，能摆出确定的先后。
// ---------------------------------------------------------------------------
class FakeClock {
public:
    uint64_t ms = 0;
    DeliveryClock delivery() { return [this] { return ms; }; }
    MonotonicNowMs budget() { return [this] { return static_cast<int64_t>(ms); }; }
};

// 真落盘的出口：那一次写走生产的 SaveFileAtomic（原子改名、--no-overwrite 那套保证一并判到），
// 只是把耗时换成假时钟上的一笔推进 —— 生产上那一笔就是慢盘。
class DiskSink final : public OutputSink {
public:
    DiskSink(FakeClock* clock, uint64_t writeMs, uint64_t setAfterMs = 0)
        : clock_(clock), writeMs_(writeMs), setAfterMs_(setAfterMs) {}

    bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                  Diagnostic* err) override {
        ++calls;
        const bool ok = SaveFileAtomic(path, bytes, overwrite, err);
        clock_->ms += writeMs_;
        if (setAfterMs_) clock_->ms = setAfterMs_;
        return ok;
    }
    bool EmitBytes(const std::vector<uint8_t>&, uint64_t* emitted, DWORD*) override {
        ++wrongPathCalls;   // 文件那一路绝不该被叫到标准输出
        if (emitted) *emitted = 0;
        return false;
    }

    int calls = 0;
    int wrongPathCalls = 0;

private:
    FakeClock* clock_;
    uint64_t writeMs_;
    uint64_t setAfterMs_;
};

// 内存管道：buffer 就是"对面真的读走了哪些字节"这一事实；failAfter 让它在半途倒下。
class PipeSink final : public OutputSink {
public:
    PipeSink(FakeClock* clock, uint64_t writeMs, uint64_t failAfter = kNoFail,
             DWORD gle = ERROR_BROKEN_PIPE)
        : clock_(clock), writeMs_(writeMs), failAfter_(failAfter), gle_(gle) {}

    bool SaveFile(const std::wstring&, const std::vector<uint8_t>&, bool, Diagnostic*) override {
        ++wrongPathCalls;   // 标准输出那一路绝不该去建文件
        return false;
    }
    bool EmitBytes(const std::vector<uint8_t>& bytes, uint64_t* emitted,
                   DWORD* ioError) override {
        ++calls;
        const size_t n = std::min<size_t>(failAfter_, bytes.size());
        buffer.insert(buffer.end(), bytes.begin(), bytes.begin() + n);
        if (emitted) *emitted = n;
        clock_->ms += writeMs_;
        if (n < bytes.size()) {
            if (ioError) *ioError = gle_;
            return false;
        }
        return true;
    }

    static constexpr uint64_t kNoFail = 0xFFFFFFFFFFFFFFFFull;
    std::vector<uint8_t> buffer;
    int calls = 0;
    int wrongPathCalls = 0;

private:
    FakeClock* clock_;
    uint64_t writeMs_;
    uint64_t failAfter_;
    DWORD gle_;
};

// ---------------------------------------------------------------------------
// 本次自己的临时目录：只建自己那一个、只删自己那一份（收尾不碰别人的文件）
// ---------------------------------------------------------------------------
class Scratch {
public:
    explicit Scratch(const wchar_t* leaf) {
        wchar_t base[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, base)) return;
        path_ = std::wstring(base) + L"ecapture-delivery-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" + leaf;
        if (CreateDirectoryW(path_.c_str(), nullptr) ||
            GetLastError() == ERROR_ALREADY_EXISTS) {
            path_ += L"\\";
        } else {
            path_.clear();
        }
    }
    ~Scratch() {
        if (path_.empty()) return;
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = path_ + L"*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
                const std::wstring p = path_ + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());
                else {
                    SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(p.c_str());
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(path_.c_str());
    }

    bool valid() const { return !path_.empty(); }
    std::wstring Path(const wchar_t* name) const { return path_ + name; }

    // 目录里还剩几个条目：临时文件没清干净时这里就会多出一个（判据要的磁盘事实之一）
    int entries() const {
        int n = 0;
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = path_ + L"*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return -1;
        do {
            if (std::wcscmp(fd.cFileName, L".") && std::wcscmp(fd.cFileName, L"..")) ++n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return n;
    }

private:
    std::wstring path_;
};

bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == static_cast<DWORD>(bytes.size());
}

std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    for (;;) {
        uint8_t buf[4096];
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        out.insert(out.end(), buf, buf + n);
    }
    CloseHandle(h);
    return out;
}

bool Exists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
}

std::vector<uint8_t> MakeBytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i % 7);
    return v;
}

PendingImage MakePending(const std::wstring& file, uint64_t startedClockMs, bool withNote) {
    PendingImage p;
    p.image.file = file;
    p.image.format = L"png";
    p.image.width = 4;
    p.image.height = 2;
    p.image.source = L"wgc";
    p.startedClockMs = startedClockMs;
    if (withNote) {
        p.notes.push_back(Diagnostic{codes::kFrameUniform, L"uniform", L"--capture", L"wgc",
                                     std::wstring(), L"0x1234", L"wgc", stages::kCapture});
    }
    return p;
}

DeliveryTarget MakeTarget(const std::wstring& file, const wchar_t* tag, const wchar_t* backend,
                          bool overwrite = true, bool implicitStdout = false) {
    DeliveryTarget t;
    t.file = file;
    t.tag = tag;
    t.backend = backend;
    t.overwrite = overwrite;
    t.implicitStdout = implicitStdout;
    return t;
}

const wchar_t* kStdout = L"-";

// 一条诊断的骨架是否说清了"哪个目标、哪条通道、哪一段、哪一路输出"。
// option 由那一步自己挑：写失败指 --out，撞名那条指的可执行下一步是 --no-overwrite（生产
// FileSave.cpp 一直这么写），所以它是判据的输入而不是判据里另猜的一份。
void CheckDiagShape(const Diagnostic& d, const wchar_t* code, const wchar_t* stage,
                    const wchar_t* tag, const wchar_t* backend, const wchar_t* option,
                    const char* label) {
    Check(d.code == code, (std::string(label) + " 的稳定码").c_str());
    Check(d.stage == stage, (std::string(label) + " 的 stage").c_str());
    Check(d.target == tag, (std::string(label) + " 认得出是哪个目标").c_str());
    Check(d.backend == backend, (std::string(label) + " 写的是真正出图那条通道").c_str());
    Check(d.option == option, (std::string(label) + " 指的是那一条可执行的下一步").c_str());
}

// ---------------------------------------------------------------------------
// 判据
// ---------------------------------------------------------------------------

// 1) 按时完成：一条错误都不许多，图与磁盘上的文件同一次判定用的是同一个名字与字节数。
void CheckOnTime() {
    std::printf("[1] 按时完成的交付：只记事实，不记期限\n");
    Scratch dir(L"on-time");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring path = dir.Path(L"shot.png");
    const std::vector<uint8_t> bytes = MakeBytes(512, 3);

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    DiskSink sink(&clock, /*writeMs=*/7);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step =
        DeliverImage(run, MakeTarget(path, L"0x1234", L"wgc"), MakePending(path, 0, true), bytes,
                     &out);

    Check(step.delivered && step.recorded, "交付成功且已记账");
    CheckEqU(sink.calls, 1, "那一次写只发了一遍");
    CheckEqU(sink.wrongPathCalls, 0, "文件那一路没去碰标准输出");
    CheckEqU(out.images.size(), 1, "images 里一条");
    CheckEqU(out.errors.size(), 0, "一条错误都不许多（按时完工不该被说成超时）");
    CheckEqU(out.notes.size(), 1, "质量提示随这张图一起送出");
    CheckEqU(out.images.empty() ? 0 : out.images[0].bytes, bytes.size(), "bytes = 交出去的字节数");
    CheckEqU(out.images.empty() ? 0 : out.images[0].elapsedMs, 7, "elapsedMs 量在写完之后");
    CheckEqU(OutcomeExitCode(out), EX_OK, "退出码 0");
    // 磁盘事实与 JSON 同时一致
    const std::vector<uint8_t> onDisk = ReadAll(path);
    Check(onDisk == bytes, "磁盘上的内容就是那一段字节");
    CheckEqU(dir.entries(), 1, "目录里只剩目标那一个条目（临时文件已清）");
    CheckEqU(out.images.size(), static_cast<uint64_t>(dir.entries()),
             "images 计数与磁盘上的文件数一致");
}

// 2) 开工之前预算已经用尽：一个字节都不发，这一张不落地。
void CheckSpentBeforeStart() {
    std::printf("[2] 开工前预算已尽：不开始输出\n");
    Scratch dir(L"pre-start");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring path = dir.Path(L"late.png");
    const std::vector<uint8_t> bytes = MakeBytes(512, 5);

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    clock.ms = 1000;   // 取帧与编码把预算花光了
    DiskSink sink(&clock, 0);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step =
        DeliverImage(run, MakeTarget(path, L"0x1234", L"wgc"), MakePending(path, 200, true), bytes,
                     &out);

    Check(!step.delivered && step.recorded, "没交付，但账已经记过（不重复入账）");
    CheckEqU(sink.calls, 0, "那一次写根本没开始");
    CheckEqU(out.images.size(), 0, "images 里没有这一张");
    CheckEqU(out.notes.size(), 0, "图没落地就不提示");
    CheckEqU(out.errors.size(), 1, "一条错误");
    if (!out.errors.empty()) {
        CheckDiagShape(out.errors[0], codes::kIoTimeout, stages::kWrite, L"0x1234", L"wgc",
                       L"--out", "开工前那条 io.timeout");
        Check(out.errors[0].value == path, "io.timeout 说的是哪一路输出");
        Check(Contains(out.errors[0].message, L"1000"), "message 里带着预算本身");
    }
    CheckEqU(OutcomeExitCode(out), EX_IO_FAILED, "一张都没落地的输出超时 → 8");
    Check(!Exists(path), "磁盘上没有这个文件");
}

// 3) 文件已经提交之后预算才跨过：图留着、另记一条 io.timeout，磁盘上一个字节都不许少。
void CheckFileCommittedLate() {
    std::printf("[3] 文件提交后跨限：交付事实与期限合规各记各的\n");
    Scratch dir(L"committed-late");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring path = dir.Path(L"slow.png");
    const std::vector<uint8_t> bytes = MakeBytes(2048, 11);

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    clock.ms = 300;   // 前面那几步已经用掉 300
    // 这一次写自己花掉 1200 ms：跨过预算的那一段正是提交本身
    DiskSink sink(&clock, 1200);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step =
        DeliverImage(run, MakeTarget(path, L"0x1234", L"wgc"), MakePending(path, 300, true), bytes,
                     &out);

    Check(step.delivered && step.recorded, "交付成功（预算跨没跨是另一件事）");
    CheckEqU(out.images.size(), 1, "跨限也不许把已经落地的图从 images 里抹掉");
    CheckEqU(out.notes.size(), 1, "质量提示跟着已交付的那一张走");
    CheckEqU(out.errors.size(), 1, "另记一条期限错误");
    if (!out.errors.empty()) {
        const Diagnostic& d = out.errors[0];
        CheckDiagShape(d, codes::kIoTimeout, stages::kWrite, L"0x1234", L"wgc", L"--out",
                       "提交后跨限那条 io.timeout");
        Check(Contains(d.message, L"2048"), "message 里带着已交出去的字节数");
        Check(d.message != Msgf(L"cap.timeout", 1000ull, 1500ull),
              "这条说的不是'还没开工'那一种");
        Check(!d.hint.empty(), "说的是下一步能动的输出那一头");
    }
    CheckEqU(out.images.empty() ? 0 : out.images[0].bytes, bytes.size(),
             "bytes 与实际写出去的字节数一致");
    CheckEqU(out.images.empty() ? 0 : out.images[0].elapsedMs, 1200,
             "elapsedMs 含提交那一次调用的等待");
    CheckEqU(dl.ElapsedMs(), 1500, "预算读数照旧是自动处理那一段（旁证，不是判据本体）");
    // 磁盘事实：文件在、内容完整、没有被超时删掉
    Check(Exists(path), "超时没有把已经提交的文件删掉");
    Check(ReadAll(path) == bytes, "磁盘上的字节一个都没少");
    CheckEqU(dir.entries(), 1, "目录里没有留下半成品");
    CheckEqU(out.images.size(), static_cast<uint64_t>(dir.entries()),
             "images 计数与磁盘上的文件数一致");
    CheckEqU(OutcomeExitCode(out), EX_CAPTURE_FAILED,
             "已有交付 + 有错误 = 部分成功（不为一个超时谎报 captured=0）");
}

// 4) 标准输出全部字节到达之后预算才跨过：同一条边界，另一头是那条管道。
void CheckStdoutEmittedLate() {
    std::printf("[4] 标准输出完整写后跨限：字节到达就是交付\n");
    const std::vector<uint8_t> bytes = MakeBytes(4096, 23);
    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    clock.ms = 100;
    PipeSink sink(&clock, /*writeMs=*/1500);   // 对面读得慢，把管道挤满的那一段等待
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step = DeliverImage(run, MakeTarget(kStdout, L"0x4321", L"dwm.thumbnail"),
                                           MakePending(kStdout, 100, false), bytes, &out);

    Check(step.delivered && step.recorded, "全部字节到达 = 已交付");
    CheckEqU(sink.wrongPathCalls, 0, "标准输出那一路没去建文件");
    CheckEqU(sink.buffer.size(), bytes.size(), "对面真的读走了全部字节");
    CheckEqU(out.images.size(), 1, "captured 与实际到达的字节不打脸");
    CheckEqU(out.errors.size(), 1, "期限不合规另记一条");
    if (!out.errors.empty()) {
        CheckDiagShape(out.errors[0], codes::kIoTimeout, stages::kStdout, L"0x4321",
                       L"dwm.thumbnail", L"--out", "标准输出那一条 io.timeout");
        Check(out.errors[0].value == kStdout, "value 说的是标准输出那一路");
    }
    // 这一条就是这次要修的地方：写出之前的那次取样量不到管道上那段等待。
    CheckEqU(out.images.empty() ? 0 : out.images[0].elapsedMs, 1500,
             "elapsedMs 含写标准输出的等待（修的是这条）");
    CheckEqU(out.images.empty() ? 0 : out.images[0].bytes, sink.buffer.size(),
             "images[].bytes = 实际到达的字节数");
    CheckEqU(OutcomeExitCode(out), EX_CAPTURE_FAILED, "图已交付 → 部分成功码");
}

// 5) 半段流：一段字节已经脏了管道，但这一张不算交付；原因归那一次写自己。
void CheckStdoutPartial() {
    std::printf("[5] 半段标准输出：不算交付，但要说清发出去了多远\n");
    const std::vector<uint8_t> bytes = MakeBytes(100, 41);
    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    PipeSink sink(&clock, /*writeMs=*/20, /*failAfter=*/10, ERROR_BROKEN_PIPE);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step = DeliverImage(run, MakeTarget(kStdout, L"0x4321", L"wgc"),
                                           MakePending(kStdout, 0, true), bytes, &out);

    Check(!step.delivered && step.recorded, "半段流不算交付");
    CheckEqU(sink.buffer.size(), 10, "管道里留下的就是那 10 个字节（收不回来）");
    CheckEqU(out.images.size(), 0, "images 里不许有一条解码不出来的图");
    CheckEqU(out.notes.size(), 0, "没交付就不提示");
    CheckEqU(out.errors.size(), 1, "一条错误");
    if (!out.errors.empty()) {
        const Diagnostic& d = out.errors[0];
        CheckDiagShape(d, codes::kWriteFailed, stages::kStdout, L"0x4321", L"wgc", L"--out",
                       "半段流那条写失败");
        Check(d.win32 == ERROR_BROKEN_PIPE, "Win32 原值留得住（断管与磁盘满是两种故障）");
        Check(d.message != Msg(L"io.stdout_failed"), "半段流与一个字节都没出去是两句话");
        Check(Contains(d.message, L"100"), "message 里带着总共要发多少");
    }
    CheckEqU(OutcomeExitCode(out), EX_IO_FAILED, "没交付的写失败 → 8");
}

// 6) 一个字节都没出去：与第 5 节同一条码，但说的是另一句话。
void CheckStdoutNothingEmitted() {
    std::printf("[6] 标准输出一个字节都没出去\n");
    const std::vector<uint8_t> bytes = MakeBytes(100, 41);
    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    PipeSink sink(&clock, 0, /*failAfter=*/0, ERROR_INVALID_HANDLE);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    // 本次根本没写 --out（图片只能整张挤过那条管道）：hint 说的就是"补一条输出路径"那一步。
    DeliverImage(run, MakeTarget(kStdout, L"0x4321", L"wgc", true, /*implicitStdout=*/true),
                 MakePending(kStdout, 0, false), bytes, &out);

    CheckEqU(sink.buffer.size(), 0, "管道是干净的");
    CheckEqU(out.images.size(), 0, "images 里没有这一张");
    CheckEqU(out.errors.size(), 1, "一条错误");
    if (!out.errors.empty()) {
        Check(out.errors[0].win32 == ERROR_INVALID_HANDLE, "句柄取不到时把那个码原样交回去");
        Check(out.errors[0].message == Msg(L"io.stdout_failed"), "说的就是'写标准输出失败'");
        // 本次没写 --out 时，"补一条输出路径"是确实可执行的下一步；给了 --out - 就不是。
        Check(out.errors[0].hint == Msg(L"cli.missing_output_hint"),
              "没给 --out 时才补那一条 hint");
    }
    CaptureOutcome explicitStdout;
    DeliverImage(run, MakeTarget(kStdout, L"0x4321", L"wgc", true, /*implicitStdout=*/false),
                 MakePending(kStdout, 0, false), bytes, &explicitStdout);
    Check(explicitStdout.errors.empty() || explicitStdout.errors[0].hint.empty(),
          "写了 --out - 的人本来就走这条道，不补那句话");
}

// 7) --no-overwrite 撞已存在：旧文件一个字节都不变，目录里不留临时文件。
void CheckNoOverwriteKeepsOldFile() {
    std::printf("[7] --no-overwrite：提交那一步失败，旧文件原样留着\n");
    Scratch dir(L"no-overwrite");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring path = dir.Path(L"old.png");
    const std::vector<uint8_t> oldBytes = MakeBytes(64, 100);
    const std::vector<uint8_t> bytes = MakeBytes(512, 7);
    Check(WriteAll(path, oldBytes), "先把旧文件摆好");

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    DiskSink sink(&clock, 5);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step = DeliverImage(
        run, MakeTarget(path, L"0x1234", L"wgc", /*overwrite=*/false), MakePending(path, 0, false),
        bytes, &out);

    Check(!step.delivered && step.recorded, "没提交成功");
    CheckEqU(out.images.size(), 0, "images 里没有这一张");
    CheckEqU(out.errors.size(), 1, "一条错误");
    if (!out.errors.empty()) {
        // 撞名那一条指的是 --no-overwrite：那是 FileSave 一直以来的写法，也是这里唯一的下一步。
        CheckDiagShape(out.errors[0], codes::kFileExists, stages::kWrite, L"0x1234", L"wgc",
                       L"--no-overwrite", "撞名那条");
    }
    Check(ReadAll(path) == oldBytes, "旧文件内容逐字节没变");
    CheckEqU(dir.entries(), 1, "只清掉本次自己的临时文件");
    CheckEqU(OutcomeExitCode(out), EX_IO_FAILED, "没交付的 I/O 失败 → 8");
}

// 8) 写失败与期限同时成立：原因以那一次调用交回来的为准，不许被超时覆盖。
void CheckIoErrorNotOverwrittenByTimeout() {
    std::printf("[8] 写失败之后才看见期限已过：真实 I/O 原因不被覆盖\n");
    Scratch dir(L"fail-and-late");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring path = dir.Path(L"keep.png");
    const std::vector<uint8_t> oldBytes = MakeBytes(64, 200);
    Check(WriteAll(path, oldBytes), "先把旧文件摆好");

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    // 那一次写在预算已经跨过之后才返回：错误是 io.file_exists，而不是 io.timeout
    DiskSink sink(&clock, /*writeMs=*/0, /*setAfterMs=*/1500);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    DeliverImage(run, MakeTarget(path, L"0x1234", L"wgc", /*overwrite=*/false),
                 MakePending(path, 0, false), oldBytes, &out);

    Check(dl.Spent(), "此刻预算确实已经用尽（前提成立才判得动下面那条）");
    CheckEqU(out.images.size(), 0, "失败的写不记交付");
    CheckEqU(out.errors.size(), 1, "只有一条错误：没交付就不另补一条超时");
    if (!out.errors.empty()) {
        Check(out.errors[0].code == codes::kFileExists, "留的是实际 I/O 原因");
        Check(out.errors[0].code != codes::kIoTimeout, "没被随后看见的超时改写掉");
    }
    CheckEqU(OutcomeExitCode(out), EX_IO_FAILED, "退出码跟着那条实际原因");
    Check(ReadAll(path) == oldBytes, "旧文件照原样在");
}

// 9) 原子改名本身失败（目标是目录）：io.write_failed 带真码，那个目录没被破坏。
void CheckCommitFailureKeepsTarget() {
    std::printf("[9] 提交那一步失败：改名撞上一个同名的目录\n");
    Scratch dir(L"commit-fail");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring blocked = dir.Path(L"blocked");
    Check(CreateDirectoryW(blocked.c_str(), nullptr) != FALSE, "先摆一个与目标同名的目录");

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    DiskSink sink(&clock, 5);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step =
        DeliverImage(run, MakeTarget(blocked, L"0x1234", L"wgc"), MakePending(blocked, 0, false),
                     MakeBytes(512, 9), &out);

    Check(!step.delivered, "没交出去");
    CheckEqU(out.images.size(), 0, "images 里没有这一张");
    CheckEqU(out.errors.size(), 1, "一条错误");
    if (!out.errors.empty()) {
        const Diagnostic& d = out.errors[0];
        CheckDiagShape(d, codes::kWriteFailed, stages::kWrite, L"0x1234", L"wgc", L"--out",
                       "改名失败那条");
        Check(d.win32 != 0, "Win32 原值留着（那是文件系统给的答案）");
        Check(d.message != Msg(L"io.file_exists"), "这不是 --no-overwrite 撞名那种可预期的拒绝");
    }
    Check(Exists(blocked), "那个目录还在：失败没把目标那一头弄坏");
    CheckEqU(dir.entries(), 1, "临时文件只删自己那一个");
}

// 10) 一批两张：第一张已经交付，第二张开工之前就超时。
void CheckFirstDeliveredSecondRefused() {
    std::printf("[10] 第一张已交付、第二张开工前超时\n");
    Scratch dir(L"two-targets");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring first = dir.Path(L"a.png");
    const std::wstring second = dir.Path(L"b.png");
    const std::vector<uint8_t> bytes = MakeBytes(512, 31);

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    DiskSink sink(&clock, 400);   // 第一张写 400，写完再往里推就到第二张的开工前
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;

    clock.ms = 0;
    DeliverImage(run, MakeTarget(first, L"0xAAAA", L"wgc"), MakePending(first, 0, true), bytes,
                 &out);
    clock.ms = 1000;   // 第二张的目标选择 / 取帧 / 编码把剩下的预算花完了
    DeliverImage(run, MakeTarget(second, L"0xBBBB", L"dwm.thumbnail"),
                 MakePending(second, 600, true), bytes, &out);

    CheckEqU(sink.calls, 1, "第二张那一次写根本没开始（不领新预算）");
    CheckEqU(out.images.size(), 1, "captured 只算真落地的那一张");
    CheckEqU(out.notes.size(), 1, "只有第一张的质量提示送得出去");
    CheckEqU(out.errors.size(), 1, "第二张一条 io.timeout");
    if (!out.errors.empty()) {
        Check(out.errors[0].code == codes::kIoTimeout, "第二张说的是期限");
        Check(out.errors[0].target == L"0xBBBB", "错误里认得出是哪一张没落地");
    }
    Check(Exists(first) && !Exists(second), "磁盘上只有第一张");
    CheckEqU(out.images.size(), static_cast<uint64_t>(dir.entries()),
             "images 计数与磁盘上的文件数一致");
    CheckEqU(OutcomeExitCode(out), EX_CAPTURE_FAILED, "有交付 + 有错误 = 部分成功");
}

// 11) 最后一个目标慢写：两张都落地，其中第二张跨过预算。
void CheckLastTargetSlowWrite() {
    std::printf("[11] 最后一张慢写：两张都在磁盘上，期限那条只说第二张\n");
    Scratch dir(L"slow-last");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring first = dir.Path(L"c.png");
    const std::wstring second = dir.Path(L"d.png");
    const std::vector<uint8_t> bytes = MakeBytes(1024, 47);

    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    CaptureOutcome out;

    DiskSink quick(&clock, 200);
    const DeliveryRun runQuick{&dl, &quick, clock.delivery()};
    DeliverImage(runQuick, MakeTarget(first, L"0xAAAA", L"wgc"), MakePending(first, 0, false),
                 bytes, &out);
    // 第二张：慢盘上的那一次提交自己就花了 900 ms，跨过剩下的预算
    DiskSink slow(&clock, 900);
    const DeliveryRun runSlow{&dl, &slow, clock.delivery()};
    DeliverImage(runSlow, MakeTarget(second, L"0xBBBB", L"wgc"), MakePending(second, 200, true),
                 bytes, &out);

    CheckEqU(out.images.size(), 2, "两张都算交付");
    CheckEqU(out.errors.size(), 1, "只有第二张带一条期限错误");
    if (!out.errors.empty()) {
        Check(out.errors[0].target == L"0xBBBB", "那条错误挂在第二张上");
        Check(out.errors[0].stage == stages::kWrite, "stage 是写文件那一段");
    }
    Check(out.images.size() == 2 && out.images[0].elapsedMs == 200, "第一张的 elapsedMs 干净");
    Check(out.images.size() == 2 && out.images[1].elapsedMs == 900,
          "第二张的 elapsedMs 含那一次慢提交");
    Check(Exists(first) && Exists(second), "两张都在磁盘上");
    Check(ReadAll(second) == bytes, "跨过预算的那一张内容仍然完整");
    CheckEqU(dir.entries(), 2, "没有多余的条目");
    CheckEqU(out.images.size(), static_cast<uint64_t>(dir.entries()),
             "images 计数与磁盘上的文件数一致");
    CheckEqU(OutcomeExitCode(out), EX_CAPTURE_FAILED, "有交付 + 有错误 = 部分成功");
}

// 12) 没设预算：写多久都不该冒出 io.timeout。
void CheckNoBudgetNeverTimesOut() {
    std::printf("[12] 没有 --timeout-ms：再慢也不报期限\n");
    Scratch dir(L"no-budget");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring path = dir.Path(L"any.png");
    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(0, clock.budget());   // 0 = 不限
    DiskSink sink(&clock, 60000);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    const DeliveryStep step = DeliverImage(
        run, MakeTarget(path, L"0x1234", L"wgc"), MakePending(path, 0, false), MakeBytes(128, 3),
        &out);

    Check(step.delivered, "写了一分钟也算交付");
    CheckEqU(out.errors.size(), 0, "一条错误都没有（没设期限就不该有期限错误）");
    CheckEqU(OutcomeExitCode(out), EX_OK, "退出码 0");
    CheckEqU(out.images.empty() ? 0 : out.images[0].elapsedMs, 60000, "耗时照实记着");
}

// 13) 同一条图记录只入列一次，且说的是它自己那一行。
void CheckNoDuplicateRecord() {
    std::printf("[13] 入账只一次：不重复插入、也不先插入再撤销\n");
    Scratch dir(L"single-record");
    if (!dir.valid()) { Check(false, "临时目录建得起来"); return; }
    const std::wstring path = dir.Path(L"one.png");
    FakeClock clock;
    const Deadline dl = Deadline::FromTotalMs(1000, clock.budget());
    DiskSink sink(&clock, 30);
    const DeliveryRun run{&dl, &sink, clock.delivery()};
    CaptureOutcome out;
    PendingImage pending = MakePending(path, 0, false);
    pending.image.hwndHex = L"0x1234";
    DeliverImage(run, MakeTarget(path, L"0x1234", L"wgc"), std::move(pending), MakeBytes(96, 1),
                 &out);

    CheckEqU(out.images.size(), 1, "一条记录");
    if (!out.images.empty()) {
        Check(out.images[0].file == path, "记的是实际写入的那个名字");
        Check(out.images[0].hwndHex == L"0x1234", "元数据原样带过来（不是重造的一条）");
        CheckEqU(out.images[0].bytes, 96, "bytes 已补");
    }
    CheckEqU(out.notes.size(), 0, "没有质量提示时也不凭空补一条");
}

}  // namespace

int main() {
    std::printf("交付那一步与期限合规分开记账（Delivery + FileSave）离线判据\n");
    CheckOnTime();
    CheckSpentBeforeStart();
    CheckFileCommittedLate();
    CheckStdoutEmittedLate();
    CheckStdoutPartial();
    CheckStdoutNothingEmitted();
    CheckNoOverwriteKeepsOldFile();
    CheckIoErrorNotOverwrittenByTimeout();
    CheckCommitFailureKeepsTarget();
    CheckFirstDeliveredSecondRefused();
    CheckLastTargetSlowWrite();
    CheckNoBudgetNeverTimesOut();
    CheckNoDuplicateRecord();
    std::printf("共 %d 项检查，失败 %d\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
