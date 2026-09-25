#include "HistoryArchive.h"

#include <cstdio>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "FileSave.h"
#include "Lang.h"
#include "OutputPlan.h"   // ExtensionFor / SameWindowsPath：扩展名与"是不是同一个文件"各只有一份判据

// ---------------------------------------------------------------------------
// 保证边界（与 FileSave.h 顶部那一段同一读法，别当成万能药）
//
// * 提交走的是生产 SaveFileAtomic(path, bytes, overwrite=false)：同目录唯一临时文件 + 一次
//   "不许替换"的原子改名。目标已存在时那一次改名报 ERROR_FILE_EXISTS，本次临时文件被清掉，
//   磁盘上原有的那一份历史一个字节都没动过。
// * 网络共享上的 MOVEFILE 语义由服务端决定（同 FileSave 那条），所以"绝不覆盖"这条在 SMB /
//   WebDAV 上仍然由那一次改名当场裁决，这里没有"先看一眼在不在"的预检 —— 预检有竞态，
//   而且会把"问不出"和"没有"混成一种。
// * 只跟随**真目录**：那一个位置是个文件、或是一个重解析点（junction / 符号链接）时这里停下，
//   因为照着它写等于把截图放进谁也没批准过的另一个地方。停下就是如实报一条失败，既不换个目录
//   再试，也不建自己的重解析点，更不改标签与 ACL。
// * 目录不存在不是失败信号：ResolveHistoryRoot 只解析，"这一路写不写得下去"由真去建的那一次
//   CreateDirectoryW 交回错误码回答（没权限、盘满、父路径不合在这里各说各的）。
// ---------------------------------------------------------------------------

namespace ecapture {
namespace {

// 撞名就换下一个序号再试。次数是有界的：反复撞名说明有别的东西在按同一条规则起名字，
// 那是"这一份不该由我提交"的信号，不是"再多试几次"的理由。
constexpr int kHistoryAttempts = 4;

// 与 OutputPlan 同一道数字宽度写法（这里自己排一次，因为归档名要的是定宽可排序，
// 而 --out 模板那一份是给人看的）。
std::wstring Padded(int value, int width) {
    wchar_t buf[24];
    swprintf(buf, 24, L"%0*d", width, value);
    return buf;
}

std::wstring AbsoluteNormalized(const std::wstring& path) {
    const DWORD cap = 8 * MAX_PATH;
    std::vector<wchar_t> buffer(cap);
    const DWORD n = GetFullPathNameW(path.c_str(), cap, buffer.data(), nullptr);
    if (n == 0 || n >= cap) return path;   // 问不出来就原样交回：比较那一步自己会说不合
    return std::wstring(buffer.data());
}

// 本进程的抗冲突标识：8 个十六进制位，混的是 PID、启动时刻、QPC 与一个栈变量的地址。
// 它**不是**随机数也不是安全凭证，作用只有一个 —— PID 被回收之后（进程退出、号发给下一个）
// 同一秒里的归档名仍然大概率不同，配合独占改名把"绝不覆盖既有历史"钉死。
std::wstring ProcessNonce() {
    static const std::wstring nonce = [] {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const unsigned long long stackMark =
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&qpc));
        unsigned long long mixed = static_cast<unsigned long long>(GetCurrentProcessId()) * 0x9E3779B9ull;
        mixed ^= static_cast<unsigned long long>(GetTickCount64()) << 17;
        mixed ^= static_cast<unsigned long long>(qpc.LowPart) + (static_cast<unsigned long long>(qpc.HighPart) << 32);
        mixed ^= stackMark << 7;
        wchar_t buf[16];
        swprintf(buf, 16, L"%08llx", mixed);
        return std::wstring(buf);
    }();
    return nonce;
}

// 归档根说不通那一条：ASCII 原因 token 跟在文案后面（message 会随 --lang 变，而调用方排障
// 要能拿到一条不变的标识 —— 与 crop / cursor 那几处同一做法）。
Diagnostic Unavailable(const std::wstring& dir, const wchar_t* message_key,
                       const std::wstring& reason) {
    Diagnostic d;
    d.code = codes::kHistoryUnavailable;
    d.message = Msgf(message_key, reason);
    d.value = dir;
    d.hint = Msg(L"history.unavailable_hint");
    d.stage = stages::kHistory;
    return d;
}

// 那一次目录创建 / 改名自己交回来的原因：码换成历史这一族的、stage 换成 history，
// 并且**不保留** --out 那一个 option —— 这一条与用户写的那条输出路径无关，留着它会把人
// 引向"改改 --out 再来一次"这种根本不起作用的下一步（主输出那条路有它自己的一条记录）。
Diagnostic ToHistoryError(Diagnostic err) {
    if (err.code == codes::kFileExists) {
        err.code = codes::kHistoryFileExists;
        if (err.hint.empty()) err.hint = Msg(L"history.file_exists_hint");
    } else {
        err.code = codes::kHistoryWriteFailed;
        if (err.hint.empty()) err.hint = Msg(L"history.write_failed_hint");
    }
    err.stage = stages::kHistory;
    err.option.clear();
    return err;
}

// 那一个位置此刻是不是一个"跟着走下去不安全"的占用者。这里只问属性，不创建、不试写，也不
// "顺手改它的标签/权限/名字"。问不出来（还没建、父层不让看、路径名不合到问不出）一律算 kNone：
// 那一路的下场由真去建目录 / 真去改名那一次调用交回错误码回答，这里不替它猜一个结论 ——
// 尤其不会把"没问到"写成"没问题"或"有问题"。
enum class Blocker { kNone, kFile, kReparse };

Blocker ProbeBlocker(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return Blocker::kNone;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return Blocker::kReparse;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? Blocker::kNone : Blocker::kFile;
}

}  // namespace

// ---------------------------------------------------------------------------
// 时钟与命名
// ---------------------------------------------------------------------------

HistoryInstant LocalHistoryInstant() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    HistoryInstant when;
    when.year = st.wYear;
    when.month = st.wMonth;
    when.day = st.wDay;
    when.hour = st.wHour;
    when.minute = st.wMinute;
    when.second = st.wSecond;
    return when;
}

std::wstring HistoryDirectoryName(const HistoryInstant& when) {
    return Padded(when.year, 4) + L"-" + Padded(when.month, 2) + L"-" + Padded(when.day, 2);
}

std::wstring HistoryFileName(const HistoryInstant& when, uint32_t pid, const std::wstring& nonce,
                             uint64_t seq, ImageFormat format) {
    std::wstring name = Padded(when.year, 4) + Padded(when.month, 2) + Padded(when.day, 2) + L"-" +
                        Padded(when.hour, 2) + Padded(when.minute, 2) + Padded(when.second, 2) +
                        L"-" + std::to_wstring(pid) + L"-" + nonce + L"-" + std::to_wstring(seq);
    return name + ExtensionFor(format);
}

// ---------------------------------------------------------------------------
// 归档根
// ---------------------------------------------------------------------------

HistoryRoot ResolveHistoryRoot() {
    HistoryRoot root;

    wchar_t buffer[8 * MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        // 问不出自己在哪里，就**不猜**"那大概就是这个当前目录 / 那个默认安装位置"。
        // 猜错一次的后果是把截图存进一个谁也没找得到的地方，而它看上去像"归档成功了"。
        root.usable = false;
        root.failure = Unavailable(std::wstring(), L"history.unavailable_reason", L"module_path_unavailable");
        return root;
    }

    std::wstring module_path(buffer, n);
    const size_t slash = module_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        root.usable = false;
        root.failure = Unavailable(std::wstring(), L"history.unavailable_reason", L"module_path_unavailable");
        return root;
    }
    root.dir = AbsoluteNormalized(module_path.substr(0, slash + 1) + L"history");

    // 那一个位置此刻是什么：只问属性，不创建、不试写。只有两种现场现在就能确定"这一路说不通"：
    // 那个名字被一个文件占着、或者它是一个重解析点（跟着写等于写进谁也没批准过的别处）。
    // "还没建出来"与"这一问本身没答案"都不在这里下结论 —— 前者本来就是正常起点，后者的下场由
    // 真去建目录那一次调用交回错误码回答（这里任何一条都不预测"这一次一定写得出去"）。
    switch (ProbeBlocker(root.dir)) {
        case Blocker::kFile:
            root.failure = Unavailable(root.dir, L"history.unavailable_reason", L"root_is_a_file");
            return root;
        case Blocker::kReparse:
            root.failure = Unavailable(root.dir, L"history.unavailable_reason",
                                       L"root_is_reparse_point");
            return root;
        case Blocker::kNone:
            break;
    }
    root.usable = true;
    return root;
}

// ---------------------------------------------------------------------------
// 归档那一步
// ---------------------------------------------------------------------------

HistoryArchive::HistoryArchive() : root_(ResolveHistoryRoot()), clock_(LocalHistoryInstant),
                                   nonce_(ProcessNonce()) {}

HistoryArchive::HistoryArchive(HistoryRoot root, HistoryClockFn clock, std::wstring nonce,
                               uint64_t first_seq)
    : root_(std::move(root)), clock_(std::move(clock)),
      nonce_(nonce.empty() ? ProcessNonce() : std::move(nonce)), next_seq_(first_seq ? first_seq : 1) {}

bool HistoryArchive::EnsureDateDirectory(const HistoryInstant& when, std::wstring* dir,
                                         Diagnostic* err) {
    const std::wstring date_dir = root_.dir + L"\\" + HistoryDirectoryName(when);
    *dir = date_dir;

    // 根与日期目录各建一次：CreateDirectoryW 不建中间层，所以少一层都不算"已经备好"。
    const std::wstring steps[] = {root_.dir, date_dir};
    for (const std::wstring& step : steps) {
        if (CreateDirectoryW(step.c_str(), nullptr)) continue;
        const DWORD gle = GetLastError();   // 先取走：下面那一问属性会覆盖它

        switch (ProbeBlocker(step)) {
            case Blocker::kFile:
                // 那个名字被一个文件占着。不删它、不改它，也不"那就换个地方存"。
                if (err) {
                    *err = Unavailable(step, L"history.unavailable_reason",
                                       L"path_component_is_a_file");
                    err->win32 = gle;
                }
                return false;
            case Blocker::kReparse:
                // junction / 符号链接：照着它写下去，图落在哪儿就不是这个程序目录了。
                if (err) {
                    *err = Unavailable(step, L"history.unavailable_reason",
                                       L"path_component_is_reparse_point");
                    err->win32 = gle;
                }
                return false;
            case Blocker::kNone:
            default:
                break;
        }

        // 已经有一个真目录在那里（别的进程先建好了同一个日期目录）：这不算失败，走下去。
        if (gle == ERROR_ALREADY_EXISTS) continue;

        // 没权限、盘满、父路径不存在、路径里有非法字符 —— 都照那一次创建自己交回的码报，
        // 不换个目录再试一次，也不悄悄塞进临时目录。
        if (err) {
            Diagnostic d;
            d.code = codes::kHistoryWriteFailed;
            d.message = Msgf(L"history.dir_failed_reason", gle);
            d.value = step;
            d.hint = Msg(L"history.write_failed_hint");
            d.stage = stages::kHistory;
            d.win32 = gle;
            *err = std::move(d);
        }
        return false;
    }
    return true;
}

bool HistoryArchive::CommitCopy(const std::wstring& dir, const HistoryInstant& when,
                                ImageFormat format, const std::vector<uint8_t>& bytes,
                                const std::wstring& primaryPath, HistoryRecord* record,
                                Diagnostic* detail) {
    const uint32_t pid = GetCurrentProcessId();
    // 主输出走标准输出时没有路径可比；文件名同不同源都由同一条 AbsoluteNormalized 折一次，
    // 免得 "\\.\" 前缀、8.3 短名以外那些 . / .. 与盘符写法在两边各算一套。
    const std::wstring normalized_primary =
        (primaryPath.empty() || primaryPath == L"-") ? std::wstring() : AbsoluteNormalized(primaryPath);

    Diagnostic last_err;
    for (int attempt = 0; attempt < kHistoryAttempts; ++attempt) {
        const uint64_t seq = next_seq_++;   // 本次决策用掉一个序号：同一进程连截多张也各有各的名
        const std::wstring path = dir + L"\\" + HistoryFileName(when, pid, nonce_, seq, format);

        // 主输出恰好算在这棵 history 树里、而且名字与这一次要用的归档名相同：那就**不写副本**。
        // 写下去是把刚交出去的那一张自己盖掉，而"另存一份独立副本"说的从来不是这一件事。
        if (!normalized_primary.empty() && SameWindowsPath(path, normalized_primary)) {
            record->state = HistoryRecord::State::kSkipped;
            record->code = codes::kHistorySameFile;
            if (detail) {
                Diagnostic d;
                d.code = codes::kHistorySameFile;
                d.message = Msg(L"history.same_file");
                d.value = path;
                d.hint = Msg(L"history.same_file_hint");
                d.stage = stages::kHistory;
                *detail = std::move(d);
            }
            return false;
        }

        Diagnostic err;
        if (SaveFileAtomic(path, bytes, /*overwrite=*/false, &err)) {
            record->state = HistoryRecord::State::kSaved;
            record->file = std::move(path);   // 只有真提交了才把这个名字交出去（搬移不抛）
            record->code.clear();
            return true;
        }
        last_err = ToHistoryError(std::move(err));
        // 撞名值得换下一个序号再来一次；别的原因（没权限、盘满、目标是目录）换名字也没用。
        if (last_err.code != codes::kHistoryFileExists) break;
    }

    record->state = HistoryRecord::State::kFailed;
    record->code = last_err.code.empty() ? codes::kHistoryWriteFailed : last_err.code;
    if (last_err.code.empty()) {
        last_err.code = record->code;
        last_err.stage = stages::kHistory;
        last_err.message = Msg(L"history.commit_failed");
        last_err.hint = Msg(L"history.write_failed_hint");
    }
    if (detail) *detail = std::move(last_err);
    return false;
}

bool HistoryArchive::Archive(const std::wstring& primaryPath, ImageFormat format,
                             const std::vector<uint8_t>& bytes, HistoryRecord* record,
                             Diagnostic* detail) {
    if (!record) return false;
    *record = HistoryRecord{};
    if (detail) *detail = Diagnostic{};

    if (!root_.usable) {
        record->state = HistoryRecord::State::kFailed;
        record->code = root_.failure.code;
        if (detail) *detail = root_.failure;
        return false;
    }

    // 一次取样，目录与文件名同源于它：跨午夜时不会出现"目录写着今天、文件名写着明天"。
    const HistoryInstant when = clock_ ? clock_() : LocalHistoryInstant();

    std::wstring dir;
    Diagnostic dir_err;
    if (!EnsureDateDirectory(when, &dir, &dir_err)) {
        record->state = HistoryRecord::State::kFailed;
        record->code = dir_err.code;
        if (detail) *detail = std::move(dir_err);
        return false;
    }
    return CommitCopy(dir, when, format, bytes, primaryPath, record, detail);
}

const wchar_t* HistoryStateName(HistoryRecord::State state) {
    switch (state) {
        case HistoryRecord::State::kSaved: return L"saved";
        case HistoryRecord::State::kFailed: return L"failed";
        case HistoryRecord::State::kSkipped: return L"skipped";
        case HistoryRecord::State::kNone: break;
    }
    return L"";   // kNone 由渲染层整个键不写（见 Report.cpp）
}

}  // namespace ecapture
