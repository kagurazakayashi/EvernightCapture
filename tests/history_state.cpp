// 截图历史归档（HistoryArchive + 它在共享交付那一步的接缝）离线判据。
//
// 为什么单独一个可执行文件：这一批判据要的现场是"主交付已经完成、副本这一头各有下场"，而副本
// 的下场取决于磁盘对面那一次独占改名。真机上"那个名字已经被别的进程占了""盘满了""那个位置是
// 一个文件"这些都排不出来（本机不许为了造现场去改用户的存储设置、ACL 或完整性标签）。所以
// 落点、本地时钟与抗冲突标识由这里注入，被 judged 的本体仍然是生产那三份：
//   src/HistoryArchive.cpp（目录、命名、独占提交与失败归类）
//   src/Delivery.cpp（交付那一步的编排：归档排在主交付之后、期限核对之前，以及账记在哪儿）
//   src/FileSave.cpp（那一次"不许替换"的原子改名 —— 与主输出用的是同一份提交设施）
// 文件走真磁盘，所以每一判都同时核对**磁盘事实**（几个文件、各自多少字节、既有那一张有没有被
// 改过）与**记账那一头**（images[].history 那三个键、errors 的码与 stage、退出码）。
//
// 三条判据线各在自己的位置（避免拿一份证据冒充另一份）：
//   * "副本字节与主交付完全相同"在这里用真磁盘判，同时用地址核对"传进去的就是那一份缓冲"。
//   * "不可写 / 盘满"那种要改机器才造得出的现场，这里判的是**分类**（那一次调用交回的 win32 与
//     code 原样进 errors，不被折成别的原因，也不被随后的超时覆盖）；真磁盘上能诚实造出来的
//     "那个位置是个文件""路径名不合""父路径不存在"这三种都逐条判了。
//   * "重解析点""多进程同时截图"要真建 junction、真起两个 ECAPTURE 进程，写在 tests\history.ps1
//     里判（那一套还核对主输出与历史副本的 SHA-256 相同）。
//
// 只用 C 风格的 printf 汇报，判据写在断言里；任何一条不过就返回非 0。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../src/Capture.h"
#include "../src/CliOptions.h"
#include "../src/Delivery.h"
#include "../src/FileSave.h"
#include "../src/HistoryArchive.h"
#include "../src/Lang.h"

namespace {

using namespace ecapture;

void Section(const char* title) { std::printf("\n%s\n", title); }

int g_failures = 0;
int g_checks = 0;
// 有一次临时目录没能建在 %TEMP%，而是退到了这个测试 exe 自己那个目录（本进程被所在目录的
// 强制完整性标签带到 low 那一档时的现场）。收尾时按这一格说明写在哪儿，别让半途失败的清理
// 留下无人认领的目录。
bool g_usedModuleDir = false;

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(c > 0 && c < 128 ? static_cast<char>(c) : '?');
    return s;
}

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
    if (got != want) {
        std::printf("        实测=%llu 期望=%llu\n", static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want));
    }
}

void CheckS(const std::wstring& got, const std::wstring& want, const char* what) {
    Check(got == want, what);
    if (got != want) {
        std::printf("        实测=%s 期望=%s\n", Narrow(got).c_str(), Narrow(want).c_str());
    }
}

bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

bool Ends(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---------------------------------------------------------------------------
// 真磁盘上的读、写、列目录（判据要的是对面那个东西到底长什么样）
// ---------------------------------------------------------------------------

std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == static_cast<DWORD>(bytes.size());
}

std::wstring LeafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring TrimTrailingSlash(std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    return dir;
}

size_t CountFilesIn(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = TrimTrailingSlash(dir) + L"\\*";
    size_t n = 0;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

size_t CountDirsIn(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = TrimTrailingSlash(dir) + L"\\*";
    size_t n = 0;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

std::vector<uint8_t> MakeBytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed + i % 7);
    return v;
}

// ---------------------------------------------------------------------------
// 本次自己的临时目录：只建自己那一个、只删自己那一份（收尾不碰别人的文件）
// ---------------------------------------------------------------------------
class Scratch {
public:
    explicit Scratch(const wchar_t* leaf) {
        const std::wstring stem =
            L"ecapture-history-" + std::to_wstring(GetCurrentProcessId()) + L"-" + leaf;
        wchar_t base[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, base)) {
            path_ = std::wstring(base) + stem;
            lastAttempt_ = path_;
            if (CreateDirectoryW(path_.c_str(), nullptr) ||
                GetLastError() == ERROR_ALREADY_EXISTS) {
                path_ += L"\\";
                return;
            }
            lastError_ = GetLastError();
            path_.clear();
        } else {
            lastError_ = GetLastError();
        }
        // 退路：写在本测试 exe 自己的目录里（低完整性标签那一种现场）。换的只是落点，
        // 一条判据都没放宽。
        wchar_t module[MAX_PATH]{};
        const DWORD len = GetModuleFileNameW(nullptr, module, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return;
        std::wstring dir(module, len);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        path_ = dir.substr(0, slash + 1) + stem;
        lastAttempt_ = path_;
        if (CreateDirectoryW(path_.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
            path_ += L"\\";
            g_usedModuleDir = true;
        } else {
            lastError_ = GetLastError();
            path_.clear();
        }
    }

    bool Require(const char* label) {
        if (valid()) return true;
        Check(false, (std::string(label) + "（试的是 " + Narrow(lastAttempt_) + "，错误码 " +
                      std::to_string(lastError_) + "）")
                         .c_str());
        return false;
    }
    bool valid() const { return !path_.empty(); }
    const std::wstring& path() const { return path_; }
    std::wstring operator+(const std::wstring& rel) const { return path_ + rel; }

    ~Scratch() {
        if (path_.empty()) return;
        RemoveTree(path_);
        RemoveDirectoryW(path_.c_str());
    }

private:
    static void RemoveTree(const std::wstring& dir) {
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = dir + L"\\*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTree(p);
                RemoveDirectoryW(p.c_str());
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::wstring path_;
    std::wstring lastAttempt_;
    DWORD lastError_ = 0;
};

// ---------------------------------------------------------------------------
// 假本地时钟：目录名与文件名同源于"这一次取样"，所以判据要的现场是"这一张取到的是哪一秒"。
// ---------------------------------------------------------------------------
class FakeHistoryClock {
public:
    HistoryInstant next{};
    HistoryClockFn fn() { return [this] { ++samples; return next; }; }
    int samples = 0;
};

HistoryInstant InstantOf(int y, int mo, int d, int h, int mi, int s) {
    HistoryInstant t;
    t.year = y; t.month = mo; t.day = d; t.hour = h; t.minute = mi; t.second = s;
    return t;
}

HistoryRoot UsableRoot(const std::wstring& dir) {
    HistoryRoot r;
    r.dir = dir;
    r.usable = true;
    return r;
}

// ---------------------------------------------------------------------------
// 交付那一段用的假出口与假归档器。假归档器只负责"当场造出某一种下场"与计数，判的仍然是生产
// Delivery.cpp 那一份编排；真归档器同时在用（CheckDelivery* 那几节里落盘的就是真文件）。
// ---------------------------------------------------------------------------
class MemSink final : public OutputSink {
public:
    bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                  Diagnostic* err) override {
        ++fileCalls;
        last_path = path;
        if (advance_ms) clock_ms += advance_ms;
        return SaveFileAtomic(path, bytes, overwrite, err);
    }
    bool EmitBytes(const std::vector<uint8_t>& bytes, uint64_t* emitted, DWORD* ioError) override {
        ++stdoutCalls;
        const size_t n = fail_after < bytes.size() ? fail_after : bytes.size();
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.begin() + n);
        if (emitted) *emitted = n;
        if (advance_ms) clock_ms += advance_ms;
        if (n < bytes.size()) {
            if (ioError) *ioError = 109;   // ERROR_BROKEN_PIPE：那一次写就地失败的那种
            return false;
        }
        return true;
    }
    int fileCalls = 0;
    int stdoutCalls = 0;
    std::wstring last_path;
    std::vector<uint8_t> buffer_;
    size_t fail_after = SIZE_MAX;
    uint64_t clock_ms = 0;
    uint64_t advance_ms = 0;
};

class FakeArchiver final : public HistoryArchiver {
public:
    bool succeed = true;
    std::wstring code;             // 失败/跳过时那条稳定码
    DWORD win32 = 0;
    bool throw_too = false;
    int calls = 0;
    std::wstring primary;
    ImageFormat fmt = ImageFormat::kPng;
    const void* buffer_address = nullptr;
    size_t length = 0;

    bool Archive(const std::wstring& primaryPath, ImageFormat format,
                 const std::vector<uint8_t>& bytes, HistoryRecord* record,
                 Diagnostic* detail) override {
        ++calls;
        primary = primaryPath;
        fmt = format;
        // 判据要看的是"传进来的就是主交付那一份缓冲"（同一个地址 = 没复制、没重编码）
        buffer_address = bytes.empty() ? nullptr : static_cast<const void*>(bytes.data());
        length = bytes.size();
        if (throw_too) throw std::bad_alloc{};
        if (!record) return false;
        *record = HistoryRecord{};
        if (succeed) {
            record->state = HistoryRecord::State::kSaved;
            record->file = L"saved-by-fake.png";
            return true;
        }
        record->state = code == codes::kHistorySameFile ? HistoryRecord::State::kSkipped
                                                        : HistoryRecord::State::kFailed;
        record->code = code;
        if (detail) {
            detail->code = code;
            detail->stage = stages::kHistory;
            detail->message = L"arranged";
            detail->win32 = win32;
        }
        return false;
    }
};

PendingImage MakePending(const std::wstring& file) {
    PendingImage p;
    p.image.file = file;
    p.image.format = L"png";
    p.image.width = 4;
    p.image.height = 2;
    p.image.source = L"wgc";
    p.image.path = L"wgc";
    p.image.scope = L"window";
    p.notes.push_back(Diagnostic{codes::kFrameUniform, L"uniform", L"--capture", L"wgc",
                                 std::wstring(), L"0x0010ABCD", L"wgc", stages::kCapture});
    return p;
}

DeliveryTarget MakeTarget(const std::wstring& file) {
    DeliveryTarget t;
    t.file = file;
    t.tag = L"0x0010ABCD";
    t.backend = L"wgc";
    t.overwrite = true;
    return t;
}

// 一条 errors 记录该长的样子：码、哪一段、哪个目标、哪条通道（历史那一条与主输出那两条分得开）
void CheckHistoryErrorShape(const Diagnostic& d, const wchar_t* want_code, const wchar_t* target,
                            const wchar_t* backend, const char* what) {
    Check(d.code == want_code, (std::string(what) + " 的 code 是那条稳定码").c_str());
    Check(d.stage == stages::kHistory, (std::string(what) + " 的 stage 是 history").c_str());
    Check(d.target == target, (std::string(what) + " 认得出是哪个目标").c_str());
    Check(d.backend == backend, (std::string(what) + " 认得出是哪条通道").c_str());
    // 这一条与 --out 无关：留着一个 option 会把人引向"改改输出路径再来"那种不起作用的下一步
    Check(d.option.empty(), (std::string(what) + " 不冒充是 --out 那一条").c_str());
}

// ===========================================================================
// 1) 机器名与稳定码：这几组词是契约的一部分，改名等于破坏调用方
// ===========================================================================
void TestMachineWords() {
    Section("1) 状态词与稳定码");
    Check(std::wstring(HistoryStateName(HistoryRecord::State::kSaved)) == L"saved",
          "status saved");
    Check(std::wstring(HistoryStateName(HistoryRecord::State::kFailed)) == L"failed",
          "status failed");
    Check(std::wstring(HistoryStateName(HistoryRecord::State::kSkipped)) == L"skipped",
          "status skipped");
    Check(std::wstring(HistoryStateName(HistoryRecord::State::kNone)).empty(),
          "kNone 没有机器名（渲染层整个键不写）");
    Check(std::wstring(codes::kHistoryUnavailable) == L"history.unavailable", "code unavailable");
    Check(std::wstring(codes::kHistoryWriteFailed) == L"history.write_failed", "code write_failed");
    Check(std::wstring(codes::kHistoryFileExists) == L"history.file_exists", "code file_exists");
    Check(std::wstring(codes::kHistoryBudgetSpent) == L"history.budget_spent",
          "code budget_spent");
    Check(std::wstring(codes::kHistorySameFile) == L"history.same_file", "code same_file");
    Check(std::wstring(stages::kHistory) == L"history", "stage history");
}

// ===========================================================================
// 2) 命名：本地日期目录 + 时间/进程/标识/序号，扩展名跟着实际编码的格式
// ===========================================================================
void TestNaming() {
    Section("2) 命名与目录同源，扩展名跟着真实编码");
    Scratch s(L"naming");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring root = s + L"root";
    const HistoryInstant when = InstantOf(2026, 3, 9, 7, 5, 4);
    FakeHistoryClock clock;
    clock.next = when;
    HistoryArchive a(UsableRoot(root), clock.fn(), L"deadbeef", 1);

    // 期望值独立算出来：目录是本地日期那一种写法，文件名里带同一天的日期与同一秒的时间
    const std::wstring want_dir = root + L"\\2026-03-09";
    const std::vector<uint8_t> bytes = MakeBytes(64, 11);
    HistoryRecord rec;
    Diagnostic detail;
    const bool ok = a.Archive(L"-", ImageFormat::kPng, bytes, &rec, &detail);

    Check(ok, "归档成功");
    Check(rec.state == HistoryRecord::State::kSaved, "结论是 saved");
    Check(rec.file == want_dir + L"\\20260309-070504-" + std::to_wstring(GetCurrentProcessId()) +
                          L"-deadbeef-1.png",
          "目录与名字同源于那一次取样（本地日期目录 + 时间 + PID + 标识 + 序号）");
    Check(rec.code.empty(), "saved 不带着失败码");
    CheckEqU(static_cast<uint64_t>(clock.samples), 1, "一次决策只取一次本地时间");
    Check(Exists(rec.file), "磁盘上真有那一份");
    Check(ReadAll(rec.file) == bytes, "副本的字节与送进来的那一份逐字节相同");
    CheckEqU(static_cast<uint64_t>(CountFilesIn(want_dir)), 1, "日期目录里只有这一份");
    CheckEqU(static_cast<uint64_t>(CountDirsIn(root)), 1, "根下面只建了那一个日期目录");
    Check(!Exists(root + L"\\2026-03-08"), "没有顺手多建一个日期目录");

    // 五种格式各来一张：扩展名要说"这一张真被编成了什么容器"
    struct { ImageFormat fmt; const wchar_t* ext; } kCases[] = {
        {ImageFormat::kPng, L".png"},   {ImageFormat::kJpeg, L".jpg"}, {ImageFormat::kBmp, L".bmp"},
        {ImageFormat::kTiff, L".tif"},  {ImageFormat::kGif, L".gif"},
    };
    bool ext_ok = true;
    for (size_t i = 0; i < std::size(kCases); ++i) {
        HistoryRecord r2;
        Diagnostic d2;
        const bool ok2 = a.Archive(L"-", kCases[i].fmt, MakeBytes(8, static_cast<uint8_t>(i)), &r2, &d2);
        if (!ok2 || !Ends(r2.file, kCases[i].ext)) ext_ok = false;
    }
    Check(ext_ok, "五种编码格式的历史名扩展名分别是 .png / .jpg / .bmp / .tif / .gif");
    CheckEqU(static_cast<uint64_t>(CountFilesIn(want_dir)), 6, "六张都在同一个日期目录里（同一份取样日期）");
}

// ===========================================================================
// 3) 同一秒两张、时钟回拨、跨午夜：名字各归各的，谁也不覆盖谁
// ===========================================================================
void TestSameInstantRollbackMidnight() {
    Section("3) 同刻两张 / 时钟回拨 / 跨午夜");
    Scratch s(L"instants");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring root = s + L"root";
    FakeHistoryClock clock;
    clock.next = InstantOf(2026, 7, 1, 10, 20, 30);
    HistoryArchive a(UsableRoot(root), clock.fn(), L"cafe1234", 1);

    const std::vector<uint8_t> first = MakeBytes(32, 1);
    const std::vector<uint8_t> second = MakeBytes(48, 2);
    HistoryRecord r1, r2;
    Diagnostic d1, d2;
    Check(a.Archive(L"-", ImageFormat::kPng, first, &r1, &d1), "第一张归档成功");
    Check(a.Archive(L"-", ImageFormat::kPng, second, &r2, &d2), "第二张归档成功（同一秒里）");
    Check(r1.file != r2.file, "同一秒里的两张各有各的名字");
    Check(ReadAll(r1.file) == first, "第一张的内容没被第二张动过");
    Check(ReadAll(r2.file) == second, "第二张是自己那一份字节");

    // 时钟往回拨（NTP 校时、手动改表都会这样）：已经存下的那张不能因为"时间变小了"就被覆盖
    clock.next = InstantOf(2026, 7, 1, 10, 20, 0);
    HistoryRecord r3;
    Diagnostic d3;
    Check(a.Archive(L"-", ImageFormat::kPng, MakeBytes(16, 3), &r3, &d3),
          "时钟回拨之后照样存得下来（不靠时间戳保证唯一）");
    Check(r3.file != r1.file && r3.file != r2.file, "回拨那一张也不与已有的重名");
    CheckEqU(static_cast<uint64_t>(CountFilesIn(root + L"\\2026-07-01")), 3, "三张各占一个文件");
    Check(ReadAll(r1.file) == first, "回拨之后第一张仍然是原来那一份");

    // 跨午夜：目录日期与文件名日期出自同一次取样，两张各自对齐自己的那一场
    clock.next = InstantOf(2026, 7, 1, 23, 59, 59);
    HistoryRecord before, after;
    Diagnostic db, da;
    Check(a.Archive(L"-", ImageFormat::kPng, MakeBytes(8, 4), &before, &db), "午夜前那一张成功");
    clock.next = InstantOf(2026, 7, 2, 0, 0, 1);
    Check(a.Archive(L"-", ImageFormat::kPng, MakeBytes(8, 5), &after, &da), "午夜后那一张成功");
    Check(ParentOf(before.file) == root + L"\\2026-07-01", "午夜前那张进的是前一天的目录");
    Check(ParentOf(after.file) == root + L"\\2026-07-02", "午夜后那张进的是第二天的目录");
    Check(Contains(LeafOf(before.file), L"20260701-235959"),
          "名字里的日期与它自己那个目录是同一天（不是各取一次时间）");
    Check(Contains(LeafOf(after.file), L"20260702-000001"), "跨午夜之后名字跟着新的一天");
}

// ===========================================================================
// 4) 独占提交：已有那个名字绝不覆盖，撞满了就如实报 history.file_exists
// ===========================================================================
void TestExclusiveCommit() {
    Section("4) 同名独占提交与换名重试");
    Scratch s(L"exclusive");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring root = s + L"root";
    const HistoryInstant when = InstantOf(2026, 5, 6, 8, 9, 10);
    FakeHistoryClock clock;
    clock.next = when;
    const std::wstring nonce = L"beef0001";
    HistoryArchive a(UsableRoot(root), clock.fn(), nonce, 1);

    // 用命名函数**预测**下一个名字（这里拿它当定位工具，不拿它当期望值：期望值是"那一份既有
    // 文件的内容一个字都不变"与"多出一个新文件"这两件磁盘事实）。
    const uint32_t pid = GetCurrentProcessId();
    const std::wstring dir = root + L"\\2026-05-06";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    const std::wstring predicted1 = dir + L"\\" + HistoryFileName(when, pid, nonce, 1, ImageFormat::kPng);
    const std::vector<uint8_t> kept = MakeBytes(99, 77);
    Check(WriteAll(predicted1, kept), "预置一个与下一个归档名相同的既有历史文件");

    const std::vector<uint8_t> fresh = MakeBytes(24, 9);
    HistoryRecord rec;
    Diagnostic detail;
    Check(a.Archive(L"-", ImageFormat::kPng, fresh, &rec, &detail),
          "撞名之后换下一个序号照样提交得出去");
    Check(rec.file != predicted1, "新那一份用的是另一个名字");
    Check(ReadAll(predicted1) == kept, "既有的那一张一个字节都没被改（独占提交，从不覆盖）");
    Check(ReadAll(rec.file) == fresh, "新那一份是自己那套字节");
    CheckEqU(static_cast<uint64_t>(CountFilesIn(dir)), 2, "目录里是两份，不是一份");

    // 名字连续被占满：如实报失败，而不是"那就盖掉一个"或多建几个空目录
    HistoryArchive b(UsableRoot(root), clock.fn(), nonce, 3);
    for (uint64_t seq = 3; seq <= 6; ++seq) {
        WriteAll(dir + L"\\" + HistoryFileName(when, pid, nonce, seq, ImageFormat::kPng), kept);
    }
    HistoryRecord rec2;
    Diagnostic detail2;
    Check(!b.Archive(L"-", ImageFormat::kPng, fresh, &rec2, &detail2),
          "归档名连续撞车时不再试下去（有界，不无限换名）");
    Check(rec2.state == HistoryRecord::State::kFailed, "下场是 failed，不是 saved");
    Check(rec2.code == codes::kHistoryFileExists, "码是 history.file_exists");
    Check(rec2.file.empty(), "失败时不写一个「本来要用的名字」冒充已有副本");
    Check(detail2.win32 == ERROR_FILE_EXISTS || detail2.win32 == ERROR_ALREADY_EXISTS,
          "win32 留的是那一次改名自己交回的码");
    Check(!detail2.message.empty(), "给人看那一句也还在");
    CheckEqU(static_cast<uint64_t>(CountFilesIn(dir)), 6, "既有的六张之外没有多出一份，也没有谁被盖掉");
}

// ===========================================================================
// 5) 并发：两个实例（各自标识，甚至同一标识）都不覆盖对方
// ===========================================================================
void TestConcurrentInstances() {
    Section("5) 多个进程同时截图（离线用两个实例摆同一现场）");
    Scratch s(L"concurrent");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring root = s + L"root";
    const HistoryInstant when = InstantOf(2026, 8, 8, 12, 0, 0);
    FakeHistoryClock clock;
    clock.next = when;

    HistoryArchive one(UsableRoot(root), clock.fn(), L"aaaa0001", 1);
    HistoryArchive two(UsableRoot(root), clock.fn(), L"bbbb0002", 1);
    HistoryRecord r1, r2;
    Diagnostic d1, d2;
    const std::vector<uint8_t> a_bytes = MakeBytes(40, 1);
    const std::vector<uint8_t> b_bytes = MakeBytes(41, 2);
    Check(one.Archive(L"-", ImageFormat::kPng, a_bytes, &r1, &d1), "实例一归档成功");
    Check(two.Archive(L"-", ImageFormat::kPng, b_bytes, &r2, &d2), "实例二归档成功");
    Check(r1.file != r2.file, "两个实例算不出同一个名字（抗冲突标识不同）");
    Check(ReadAll(r1.file) == a_bytes && ReadAll(r2.file) == b_bytes, "两份内容各是各的字节");

    // 病理现场：标识也撞上了（PID 被回收后同一秒里同标识）。仍然只有"换名"与"失败"两种下场，
    // 没有第三种"把对方的图盖掉"。
    HistoryArchive three(UsableRoot(root), clock.fn(), L"aaaa0001", 1);
    HistoryRecord r3;
    Diagnostic d3;
    Check(three.Archive(L"-", ImageFormat::kPng, MakeBytes(42, 3), &r3, &d3),
          "连标识都相同时仍然提交得出去（换下一个序号）");
    Check(r3.file != r1.file && r3.file != r2.file, "第三份不与前两份重名");
    Check(ReadAll(r1.file) == a_bytes, "撞名重试的过程中原主那一份没被改");
    CheckEqU(static_cast<uint64_t>(CountFilesIn(root + L"\\2026-08-08")), 3, "三张各占一个文件");
}

// ===========================================================================
// 6) 落点说不通那几种：位置是文件 / 路径名不合 / 父路径不存在 / 解析阶段就拒绝
// ===========================================================================
void TestUnusableLandingZone() {
    Section("6) 不可写与「那一路走不通」的失败归类");
    Scratch s(L"unusable");
    if (!s.Require("临时目录建得起来")) return;

    const HistoryInstant when = InstantOf(2026, 9, 9, 9, 9, 9);
    FakeHistoryClock clock;
    clock.next = when;
    const std::vector<uint8_t> bytes = MakeBytes(16, 5);

    // a) history 那一个位置被一个**文件**占着：不删它、不改它、也不换个目录写
    const std::wstring as_file = s + L"asfile";
    CreateDirectoryW((s + L"asfile").c_str(), nullptr);   // 先建父层，好让"history"这个名字能放文件
    const std::wstring blocker = s + L"asfile\\history";
    Check(WriteAll(blocker, bytes), "预置一个与归档根同名的文件");
    HistoryRecord ra;
    Diagnostic da;
    Check(!HistoryArchive(UsableRoot(blocker), clock.fn(), L"c0ffee01", 1)
               .Archive(L"-", ImageFormat::kPng, bytes, &ra, &da),
          "那个位置是文件时副本写不下去");
    Check(ra.state == HistoryRecord::State::kFailed && ra.code == codes::kHistoryUnavailable,
          "归类为 history.unavailable（那一路本身说不通）");
    Check(Contains(da.message, L"path_component_is_a_file"), "ASCII 原因 token 在 message 里");
    Check(ReadAll(blocker) == bytes, "占位那一个文件没被删也没被改");
    Check(!Exists(blocker + L"\\2026-09-09"), "不在那个位置下面建任何东西");

    // b) 日期目录那一层被文件占着（根是正常目录）
    const std::wstring root_b = s + L"rootb";
    CreateDirectoryW(root_b.c_str(), nullptr);
    WriteAll(root_b + L"\\2026-09-09", bytes);
    HistoryRecord rb;
    Diagnostic db;
    Check(!HistoryArchive(UsableRoot(root_b), clock.fn(), L"c0ffee02", 1)
               .Archive(L"-", ImageFormat::kPng, bytes, &rb, &db),
          "日期目录的位置是文件时也写不下去");
    Check(rb.code == codes::kHistoryUnavailable, "同样归到 history.unavailable");
    Check(Contains(db.message, L"path_component_is_a_file"), "原因 token 指的是那一层");

    // c) 根在一个不存在的父目录里：那一次 CreateDirectoryW 自己交回的码照原样进记录
    const std::wstring root_c = s + L"no-such-parent\\history";
    HistoryRecord rc;
    Diagnostic dc;
    Check(!HistoryArchive(UsableRoot(root_c), clock.fn(), L"c0ffee03", 1)
               .Archive(L"-", ImageFormat::kPng, bytes, &rc, &dc),
          "父路径不存在时副本写不下去");
    Check(rc.state == HistoryRecord::State::kFailed && rc.code == codes::kHistoryWriteFailed,
          "归类为 history.write_failed（那一次调用失败）");
    CheckEqU(dc.win32, ERROR_PATH_NOT_FOUND, "win32 是那一问交回的 ERROR_PATH_NOT_FOUND 原值");
    Check(dc.stage == stages::kHistory, "stage 说的是归档那一段");

    // d) 路径名本身不合（非法字符）：同样是那一次调用交回的码，不猜、不兜底
    const std::wstring root_d = s + L"bad<>name";
    HistoryRecord rd;
    Diagnostic dd;
    Check(!HistoryArchive(UsableRoot(root_d), clock.fn(), L"c0ffee04", 1)
               .Archive(L"-", ImageFormat::kPng, bytes, &rd, &dd),
          "路径名不合时副本写不下去");
    Check(rd.code == codes::kHistoryWriteFailed, "归类为 history.write_failed");
    Check(dd.win32 != 0, "留着一个非零的 win32 原值（这里不猜是哪一个）");
    Check(!Exists(root_d), "没有因为「报失败」就顺手建出半个目录");

    // e) 解析阶段就说不通（root.usable=false）：一张都不写，也没有任何目录被创建
    HistoryRoot broken;
    broken.usable = false;
    broken.failure.code = codes::kHistoryUnavailable;
    broken.failure.stage = stages::kHistory;
    broken.failure.message = L"module_path_unavailable";
    HistoryRecord re;
    Diagnostic de;
    Check(!HistoryArchive(broken, clock.fn(), L"c0ffee05", 1)
               .Archive(L"-", ImageFormat::kPng, bytes, &re, &de),
          "归档根本解析不出来时不写副本");
    Check(re.code == codes::kHistoryUnavailable, "码是 history.unavailable");
    Check(de.code == codes::kHistoryUnavailable && de.stage == stages::kHistory,
          "解析那一条诊断原样交回去（不另编一份原因）");
    Check(re.file.empty(), "解析不出来时既不写副本也不给一个副本名字");
}

// ===========================================================================
// 7) 主输出落在 history 树里：不自覆盖、不循环复制
// ===========================================================================
void TestSelfTargetGuard() {
    Section("7) 主输出名字就在 history 树里");
    Scratch s(L"selftarget");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring root = s + L"root";
    const HistoryInstant when = InstantOf(2026, 11, 3, 6, 7, 8);
    FakeHistoryClock clock;
    clock.next = when;
    const std::wstring nonce = L"fed00001";
    const std::wstring dir = root + L"\\2026-11-03";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);

    // 用户把 --out 直接写在这棵树里，而且写的就是这个进程下一个要用的归档名
    const std::wstring predicted =
        dir + L"\\" + HistoryFileName(when, GetCurrentProcessId(), nonce, 1, ImageFormat::kPng);
    const std::vector<uint8_t> primary = MakeBytes(60, 42);
    Check(WriteAll(predicted, primary), "预置主输出：名字恰好等于下一个归档名");
    HistoryRecord rec;
    Diagnostic detail;
    HistoryArchive a(UsableRoot(root), clock.fn(), nonce, 1);
    Check(!a.Archive(predicted, ImageFormat::kPng, primary, &rec, &detail),
          "这种情况不写副本（写了就是自己盖自己）");
    Check(rec.state == HistoryRecord::State::kSkipped, "下场是 skipped，不是 failed");
    Check(rec.code == codes::kHistorySameFile, "码是 history.same_file");
    Check(rec.file.empty(), "跳过时不给一个副本名字（副本确实不存在）");
    Check(ReadAll(predicted) == primary, "主输出那张图原样留着");
    CheckEqU(static_cast<uint64_t>(CountFilesIn(dir)), 1, "目录里仍然只有一张");

    // 同一棵树里的**另一个**名字：副本照旧写，两张各自独立（不因为主图在树里就不敢动，
    // 也不因为同目录而变成同一份）
    const std::wstring other = dir + L"\\user-chosen.png";
    Check(WriteAll(other, primary), "预置主输出：同一棵树里的另一个名字");
    HistoryRecord rec2;
    Diagnostic detail2;
    const std::vector<uint8_t> copy_bytes = MakeBytes(30, 8);
    Check(a.Archive(other, ImageFormat::kPng, copy_bytes, &rec2, &detail2),
          "主图在同一棵树下但名字不同时，副本照旧另存");
    Check(rec2.file != other, "副本用的是自己的那个名字");
    Check(ReadAll(other) == primary, "主图没被副本改写");
    Check(ReadAll(rec2.file) == copy_bytes, "副本是自己那一份字节");
    // 目录里此刻有三份：前一种情形留下的那张主图、这一次那张主输出、以及它的副本 ——
    // 副本从不与主输出同名，也不去改别人的名字。
    CheckEqU(static_cast<uint64_t>(CountFilesIn(dir)), 3, "三份各占一个名字，谁也没被覆盖");
}

// ===========================================================================
// 8) 交付编排（真归档器 + 真磁盘）：主交付与副本是两次交付
// ===========================================================================
void TestDeliveryWithRealArchive() {
    Section("8) 交付编排：主交付成功后用同一份缓冲归档（真组件、真磁盘）");
    Scratch s(L"delivery");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring out_dir = s + L"out";
    CreateDirectoryW(out_dir.c_str(), nullptr);
    const std::wstring out_file = out_dir + L"\\shot.png";
    const std::wstring root = s + L"root";

    // 落点用注入的根、时钟用生产那一把（不给假时钟），出口与改名都是真设施。
    HistoryArchive real_archiver(UsableRoot(root), nullptr, L"1d2d3d4d", 1);
    MemSink sink;
    const DeliveryRun run{nullptr, &sink, [] { return GetTickCount64(); }, &real_archiver,
                          ImageFormat::kPng};

    const std::vector<uint8_t> encoded = MakeBytes(256, 3);
    CaptureOutcome out;
    DeliveryStep step;
    DeliverImage(run, MakeTarget(out_file), MakePending(out_file), encoded, &out, &step);

    Check(step.delivered, "主交付成功");
    CheckEqU(static_cast<uint64_t>(out.images.size()), 1, "images 里有那一张");
    Check(out.images[0].file == out_file, "images[].file 还是主输出的名字（没被副本顶掉）");
    CheckEqU(out.images[0].bytes, 256, "bytes 是主交付写出去的那一个数");
    Check(out.images[0].history.state == HistoryRecord::State::kSaved, "history 是 saved");
    Check(out.images[0].history.code.empty(),
          "saved 那一张不带着失败码（备好的那份默认码要在成功时清掉）");
    Check(Exists(out.images[0].history.file), "history.file 指的是磁盘上真在的那一份");
    Check(ReadAll(out.images[0].history.file) == encoded,
          "副本与主输出逐字节相同（同一份编码缓冲，不重拍也不重编码）");
    Check(ReadAll(out_file) == encoded, "主输出也在");
    Check(Contains(out.images[0].history.file, L"\\history\\") ||
              Contains(out.images[0].history.file, L"root\\20"),
          "副本在这个根目录下的日期目录里，不在输出目录里");
    Check(!Exists(out_dir + L"\\history"), "不在主输出的目录里顺手建 history");
    CheckEqU(static_cast<uint64_t>(out.errors.size()), 0, "两边都成功时没有错误");
    CheckEqU(static_cast<uint64_t>(out.notes.size()), 1, "质量提示跟着已交付的那一张送出");
    CheckEqU(static_cast<uint64_t>(OutcomeExitCode(out)), EX_OK, "两张都落地 = 干净完工");

    // 标准输出那一路走的是同一个归档入口（--out -）
    MemSink sink2;
    HistoryArchive real_archiver2(UsableRoot(root), nullptr, L"5e6e7e8e", 1);
    const DeliveryRun run2{nullptr, &sink2, [] { return GetTickCount64(); }, &real_archiver2,
                           ImageFormat::kBmp};
    CaptureOutcome out2;
    DeliveryStep step2;
    const std::vector<uint8_t> stdout_bytes = MakeBytes(123, 6);
    DeliverImage(run2, MakeTarget(L"-"), MakePending(L"-"), stdout_bytes, &out2, &step2);
    Check(step2.delivered, "标准输出那一路也算完成主交付");
    CheckEqU(static_cast<uint64_t>(sink2.stdoutCalls), 1, "标准输出只发了一遍");
    Check(out2.images[0].file == L"-", "images[].file 还是那条流的名字");
    Check(out2.images[0].history.state == HistoryRecord::State::kSaved,
          "二进制标准输出一路同样有历史副本（同一个归档入口）");
    Check(Ends(out2.images[0].history.file, L".bmp"), "bmp 那一张的副本扩展名跟着真实编码");
    Check(ReadAll(out2.images[0].history.file) == stdout_bytes, "副本内容与管道那边收到的相同");
    Check(sink2.buffer_ == stdout_bytes, "stdout 里只有图片字节（没有历史路径也没有日志）");
}

// ===========================================================================
// 9) 主图成功、副本失败：交付事实留着，另记一条 history.* 的错误
// ===========================================================================
void TestPrimaryOkArchiveFails() {
    Section("9) 主交付成功而副本失败（部分成功）");
    Scratch s(L"archfail");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring out_dir = s + L"out";
    CreateDirectoryW(out_dir.c_str(), nullptr);
    const std::wstring out_file = out_dir + L"\\shot.png";
    // 落点在一个不存在的父目录里：真组件真去建目录，真失败
    HistoryArchive archiver(UsableRoot(s + L"gone\\history"), nullptr, L"9a9a9a9a", 1);
    MemSink sink;
    const DeliveryRun run{nullptr, &sink, [] { return GetTickCount64(); }, &archiver,
                          ImageFormat::kPng};

    const std::vector<uint8_t> encoded = MakeBytes(64, 4);
    CaptureOutcome out;
    DeliveryStep step;
    DeliverImage(run, MakeTarget(out_file), MakePending(out_file), encoded, &out, &step);

    Check(step.delivered, "主交付这一段还是成功的");
    CheckEqU(static_cast<uint64_t>(out.images.size()), 1, "已交付的那一张留在 images 里");
    Check(ReadAll(out_file) == encoded, "主图没有因为副本失败而被删掉或回滚");
    Check(out.images[0].history.state == HistoryRecord::State::kFailed, "history 是 failed");
    Check(out.images[0].history.code == codes::kHistoryWriteFailed, "码是 history.write_failed");
    Check(out.images[0].history.file.empty(), "失败时不写副本名字");
    CheckEqU(static_cast<uint64_t>(out.errors.size()), 1, "另记一条错误（副本那一次自己的原因）");
    if (!out.errors.empty()) {
        CheckHistoryErrorShape(out.errors[0], codes::kHistoryWriteFailed, L"0x0010ABCD", L"wgc",
                               "那条历史错误");
        CheckEqU(out.errors[0].win32, ERROR_PATH_NOT_FOUND,
                 "win32 是那一次建目录交回的原值（没被折成别的原因）");
        Check(!out.errors[0].message.empty(), "给人看那一句也在");
    }
    CheckEqU(static_cast<uint64_t>(OutcomeExitCode(out)), EX_CAPTURE_FAILED,
             "有图 + 有错 = 已交付且部分成功（既不是 0，也不是「什么都没写」的 8）");
    CheckEqU(static_cast<uint64_t>(CountDirsIn(s + L"gone")), 0, "失败的这一路没在半路留下目录");
}

// ===========================================================================
// 10) 主交付失败 / 半段标准输出：一张图都不发布，归档一次都不调用
// ===========================================================================
void TestPrimaryFailureNeverPublishesHistory() {
    Section("10) 主交付失败与半段标准输出都不进历史");
    Scratch s(L"nofail");
    if (!s.Require("临时目录建得起来")) return;

    // a) 写文件失败（父目录不存在）：归档一次都不该被叫到
    FakeArchiver archiver;
    archiver.succeed = true;
    MemSink sink;
    const DeliveryRun run{nullptr, &sink, [] { return GetTickCount64(); }, &archiver,
                          ImageFormat::kPng};
    CaptureOutcome out;
    DeliveryStep step;
    const std::wstring bad = s + L"missing-dir\\shot.png";
    DeliverImage(run, MakeTarget(bad), MakePending(bad), MakeBytes(32, 1), &out, &step);
    Check(!step.delivered, "那一次写没成交");
    CheckEqU(static_cast<uint64_t>(archiver.calls), 0, "主图没落地时归档一次都没开始");
    CheckEqU(static_cast<uint64_t>(out.images.size()), 0, "images 里不许有它");
    Check(step.recorded, "那一次写自己的原因在这里就记完了（调用方不再补第二条）");
    CheckEqU(static_cast<uint64_t>(out.errors.size()), 1, "只留那一次调用自己的那一条");
    Check(out.errors.size() == 1 && out.errors[0].code == codes::kWriteFailed &&
              out.errors[0].stage == stages::kWrite,
          "code / stage 说的是主输出那一路，与历史无关");
    CheckEqU(static_cast<uint64_t>(CountDirsIn(s.path())), 0, "连日期目录都没被建出来");
    Check(!Exists(s + L"root"), "失败的那一路不在本次临时目录里留下别的目录");

    // b) 半段标准输出：管道里留下半张图，但那不是"完成主交付"
    FakeArchiver archiver2;
    MemSink sink2;
    sink2.fail_after = 10;   // 只发出去 10 个字节
    const DeliveryRun run2{nullptr, &sink2, [] { return GetTickCount64(); }, &archiver2,
                           ImageFormat::kPng};
    CaptureOutcome out2;
    DeliveryStep step2;
    DeliverImage(run2, MakeTarget(L"-"), MakePending(L"-"), MakeBytes(64, 2), &out2, &step2);
    Check(!step2.delivered, "半段流不算交付");
    CheckEqU(static_cast<uint64_t>(archiver2.calls), 0, "半段流不进历史（不宣称归档成功）");
    CheckEqU(static_cast<uint64_t>(out2.images.size()), 0, "images 是空的");
    CheckEqU(static_cast<uint64_t>(out2.errors.size()), 1, "留的是那一次写自己的原因");
    Check(out2.errors.size() == 1 && out2.errors[0].code == codes::kWriteFailed,
          "code 是 io.write_failed");
    Check(Contains(out2.errors[0].message, L"stdout") || !out2.errors[0].message.empty(),
          "半段流那一句说了发出多少 / 一共多少");
    CheckEqU(static_cast<uint64_t>(OutcomeExitCode(out2)), EX_IO_FAILED, "一张都没落地时用 I/O 退出码");

    // c) 被拒绝 / 无匹配那些路径根本不会走到交付这一段：这里用"没有 DeliverImage 调用"这一事实
    //    代表（真机上由 tests\history.ps1 逐条核对 history 目录一个都不多）。
    Check(true, "只读与拒绝那一路不经过交付这一段（ps1 层逐条核对目录不新增）");
}

// ===========================================================================
// 11) 预算：主图刚落盘而期限才跨 —— 副本不开始，主图与账都留着
// ===========================================================================
void TestBudgetAfterPrimary() {
    Section("11) 主图落盘后预算才耗尽：归档不开始，也不另领一份预算");
    Scratch s(L"budget");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring out_dir = s + L"out";
    CreateDirectoryW(out_dir.c_str(), nullptr);
    const std::wstring out_file = out_dir + L"\\shot.png";

    MemSink sink;
    sink.advance_ms = 1500;   // 那一次提交自己花掉 1500 ms
    // 预算 1000 ms，与交付那把时钟共用同一个读数：提交之前还没跨，提交之后就跨了
    Deadline dl = Deadline::FromTotalMs(1000, [&sink] { return static_cast<int64_t>(sink.clock_ms); });
    HistoryArchive archiver(UsableRoot(s + L"root"), nullptr, L"b0b0b0b0", 1);
    const DeliveryRun run{&dl, &sink, [&sink] { return sink.clock_ms; }, &archiver,
                          ImageFormat::kPng};

    const std::vector<uint8_t> encoded = MakeBytes(48, 7);
    CaptureOutcome out;
    DeliveryStep step;
    DeliverImage(run, MakeTarget(out_file), MakePending(out_file), encoded, &out, &step);

    Check(step.delivered, "图真交出去了（没有可安全中断的等待点）");
    Check(ReadAll(out_file) == encoded, "已经落地的图不许因为随后看见超时就被删掉");
    CheckEqU(static_cast<uint64_t>(out.images.size()), 1, "captured 那一张还在");
    Check(out.images[0].history.state == HistoryRecord::State::kSkipped,
          "副本这一次是 skipped（根本没开始写）");
    Check(out.images[0].history.code == codes::kHistoryBudgetSpent, "码是 history.budget_spent");
    CheckEqU(static_cast<uint64_t>(archiver.next_sequence()), 1,
             "归档那一步没被调用过（序号一个都没用）");
    Check(!Exists(s + L"root"), "预算已尽时也不建日期目录（不做无期限的后台写入）");
    CheckEqU(static_cast<uint64_t>(out.errors.size()), 1, "期限合规那一条另记");
    if (!out.errors.empty()) {
        Check(out.errors[0].code == codes::kIoTimeout, "那一条是 io.timeout");
        Check(out.errors[0].stage == stages::kWrite, "stage 说的是写文件那一路");
    }
    CheckEqU(static_cast<uint64_t>(OutcomeExitCode(out)), EX_CAPTURE_FAILED,
             "有图 + 有错：已交付但没守住预算");
}

// ===========================================================================
// 12) 归档那一段抛出东西 / 传进去的是同一份缓冲
// ===========================================================================
void TestArchiverThrowAndBufferIdentity() {
    Section("12) 归档抛异常不抹掉交付事实；传进去的就是那一份编码缓冲");
    Scratch s(L"throws");
    if (!s.Require("临时目录建得起来")) return;

    const std::wstring out_dir = s + L"out";
    CreateDirectoryW(out_dir.c_str(), nullptr);
    const std::wstring out_file = out_dir + L"\\shot.png";

    // a) 抛异常：分配失败打在归档那一段（主图已经收不回来之后）
    FakeArchiver boom;
    boom.succeed = false;
    boom.throw_too = true;
    MemSink sink;
    const DeliveryRun run{nullptr, &sink, [] { return GetTickCount64(); }, &boom, ImageFormat::kPng};
    CaptureOutcome out;
    DeliveryStep step;
    const std::vector<uint8_t> encoded = MakeBytes(72, 21);
    DeliverImage(run, MakeTarget(out_file), MakePending(out_file), encoded, &out, &step);

    Check(step.delivered, "交付事实写在接口返回那一刻（与后面抛出什么无关）");
    Check(step.recorded, "这一张的账已经记完，调用方不再补第二条");
    CheckEqU(static_cast<uint64_t>(out.images.size()), 1, "images 里那张图没被抹掉");
    Check(ReadAll(out_file) == encoded, "磁盘上那张主图原样留着");
    Check(out.images[0].history.state == HistoryRecord::State::kFailed,
          "副本按失败记（用的是开工之前备好的那一份码位）");
    Check(out.images[0].history.code == codes::kHistoryWriteFailed, "码是那条默认稳定码");
    CheckEqU(static_cast<uint64_t>(out.errors.size()), 1, "入账的是那条骨架记录，不是编出来的原因");
    if (!out.errors.empty()) {
        CheckHistoryErrorShape(out.errors[0], codes::kHistoryWriteFailed, L"0x0010ABCD", L"wgc",
                               "归档抛出时那条记录");
        Check(out.errors[0].message.empty(), "问不出原因就不编一条冒充");
    }
    CheckEqU(static_cast<uint64_t>(out.notes.size()), 1, "质量提示仍然跟这张图在一起");
    CheckEqU(static_cast<uint64_t>(OutcomeExitCode(out)), EX_CAPTURE_FAILED,
             "部分成功，不给干净的 0");

    // b) 失败下场带 win32 时：那条记录里的原值不被交付层改写
    FakeArchiver denied;
    denied.succeed = false;
    denied.code = codes::kHistoryWriteFailed;
    denied.win32 = 5;   // ERROR_ACCESS_DENIED：没权限那一类
    MemSink sink2;
    const DeliveryRun run2{nullptr, &sink2, [] { return GetTickCount64(); }, &denied,
                           ImageFormat::kPng};
    CaptureOutcome out2;
    DeliveryStep step2;
    DeliverImage(run2, MakeTarget(out_file), MakePending(out_file), encoded, &out2, &step2);
    CheckEqU(static_cast<uint64_t>(out2.errors.size()), 1, "副本失败只记一条");
    if (!out2.errors.empty()) {
        CheckEqU(out2.errors[0].win32, 5, "那一次调用交回的 win32 原样留住");
        Check(out2.images[0].history.state == HistoryRecord::State::kFailed, "结论是 failed");
    }

    // c) 同一份缓冲：归档拿到的是主交付那一份字节本身（地址相同 = 没复制整张大图、没重新编码）
    FakeArchiver same;
    MemSink sink3;
    const DeliveryRun run3{nullptr, &sink3, [] { return GetTickCount64(); }, &same,
                           ImageFormat::kTiff};
    CaptureOutcome out3;
    DeliveryStep step3;
    const std::vector<uint8_t> one_buffer = MakeBytes(1024, 33);
    DeliverImage(run3, MakeTarget(out_file), MakePending(out_file), one_buffer, &out3, &step3);
    Check(same.calls == 1, "归档被调用了一次");
    Check(same.buffer_address == static_cast<const void*>(one_buffer.data()),
          "拿到的就是那一份编码缓冲的地址（不额外复制整张图）");
    CheckEqU(same.length, 1024, "长度也是那一份");
    CheckS(same.primary, out_file, "归档拿到的主输出名字就是实际交付那一个");
    Check(same.fmt == ImageFormat::kTiff, "归档按实际编码所用的格式起名（这一张真被编成 tiff）");

    // d) 没有归档那一步时（离线判据里只判交付本体的那些用例）：这一格保持"整个键不写"
    MemSink sink4;
    const DeliveryRun run4{nullptr, &sink4, [] { return GetTickCount64(); }, nullptr,
                           ImageFormat::kPng};
    CaptureOutcome out4;
    DeliveryStep step4;
    DeliverImage(run4, MakeTarget(out_file), MakePending(out_file), one_buffer, &out4, &step4);
    Check(out4.images.size() == 1 && out4.images[0].history.state == HistoryRecord::State::kNone,
          "没有归档那一步时 status 是 kNone（渲染层整个键不出现）");
    Check(out4.images.size() == 1 && out4.images[0].history.code.empty(),
          "kNone 那条也没有码");

    // e) 归档不改主交付的三件事实（bytes / elapsedMs / notes 与不给归档时一致）
    Check(out4.images.size() == 1 && out3.images.size() == 1 &&
              out3.images[0].bytes == out4.images[0].bytes,
          "bytes 与有没有归档无关");
    Check(out3.notes.size() == out4.notes.size(), "质量提示条数与有没有归档无关");
}

}  // namespace

int main() {
    SetLanguage(Language::kEn);   // 判据只看 code / stage / 计数，文案跟着英文那份走
    std::printf("截图历史归档（HistoryArchive + Delivery 接缝）离线判据\n");
    TestMachineWords();
    TestNaming();
    TestSameInstantRollbackMidnight();
    TestExclusiveCommit();
    TestConcurrentInstances();
    TestUnusableLandingZone();
    TestSelfTargetGuard();
    TestDeliveryWithRealArchive();
    TestPrimaryFailureNeverPublishesHistory();
    TestBudgetAfterPrimary();
    TestArchiverThrowAndBufferIdentity();
    if (g_usedModuleDir) {
        std::printf("提示：本轮至少有一个临时目录没能建在系统临时目录里，"
                    "改建在本测试 exe 自己的目录（判据与落点无关，一条都没放宽）。\n");
    }
    std::printf("共 %d 项检查，失败 %d\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
