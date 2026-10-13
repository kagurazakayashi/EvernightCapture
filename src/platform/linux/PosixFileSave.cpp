// 【平台层 · Linux】原子保存：同目录唯一临时文件 + 原子提交。
//
// 与 Windows 那一份同一组结论。动作是按 POSIX 规则做的：
//   * 临时文件在目标同目录以 O_CREAT|O_EXCL|O_NOFOLLOW 建立，名字带 PID、单调时钟与进程内计数；
//     预先摆在那个名字上的符号链接只会让 open 失败，不会被跟随；
//   * 写完 fsync 后关闭，再用 rename(2)（允许替换）或 link(2)（不许替换）提交；
//   * link(2) 在目标已存在时返回 EEXIST，既有文件一个字节都不动；成功后删掉临时名字。
//     最终文件因此是一份独立的普通文件，不是临时文件的硬链接（临时名随即被 unlink）。
//   * 全程不换目录、不提权、不 chmod；权限、磁盘已满、中间目录消失都按 errno 如实报告。
#include "FileSave.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "HostDirectory.h"
#include "Lang.h"
#include "core/TextCoding.h"

namespace ecapture {
namespace {

constexpr size_t kWriteChunk = 1u << 20;
constexpr int kTempAttempts = 8;

bool NativePathBytes(const std::wstring& path, std::string* out) {
    if (!out) return false;
    for (wchar_t c : path) {
        if (c == 0) return false;
    }
    *out = EncodeUtf8(path);
    return true;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'/');
    if (slash == std::wstring::npos) return std::wstring();
    return path.substr(0, slash == 0 ? 1 : slash);   // "/" 根目录也带上
}

std::wstring BaseOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L'/');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

class TempFile {
public:
    ~TempFile() { Discard(); }
    void Reset(std::wstring path) { path_ = std::move(path); }
    const std::wstring& path() const { return path_; }
    void Dismiss() { path_.clear(); }

private:
    void Discard() {
        if (path_.empty()) return;
        std::string native;
        if (NativePathBytes(path_, &native)) ::unlink(native.c_str());
        path_.clear();
    }
    std::wstring path_;
};

const wchar_t* PrecheckDetail(WriteBlocker b) {
    switch (b) {
        case WriteBlocker::kNotAbsolute: return L"precheck:not_absolute";
        case WriteBlocker::kParentMissing: return L"precheck:parent_missing";
        case WriteBlocker::kParentNotDirectory: return L"precheck:parent_not_directory";
        case WriteBlocker::kTargetDirectory: return L"precheck:target_is_directory";
        case WriteBlocker::kTargetSymlink: return L"precheck:target_is_link";
        case WriteBlocker::kDenied: return L"precheck:permission_denied";
        case WriteBlocker::kUnknown: return L"precheck:unverified";
        case WriteBlocker::kNone: break;
    }
    return L"";
}

Diagnostic IoError(const std::wstring& path, const wchar_t* messageKey, int err,
                   const WritePrecheck& pre) {
    Diagnostic d{codes::kWriteFailed, Msg(messageKey), L"--out", path,
                 Msgf(L"err.win32_code", static_cast<unsigned long long>(err))};
    d.stage = stages::kWrite;
    d.win32 = static_cast<uint32_t>(err);
    if (pre.blocker != WriteBlocker::kNone) d.detail = PrecheckDetail(pre.blocker);
    return d;
}

}  // namespace

WritePrecheck ProbeWriteTarget(const std::wstring& path) {
    WritePrecheck r;
    if (path.empty() || path[0] != L'/') {
        r.blocker = WriteBlocker::kNotAbsolute;
        return r;
    }
    const std::wstring dir = DirectoryOf(path);
    std::string nativeDir;
    if (dir.empty() || !NativePathBytes(dir, &nativeDir)) {
        r.blocker = WriteBlocker::kUnknown;
        return r;
    }
    // 父目录用 stat（跟随中间层链接）：~/.local/state 本身可以是一个指向别处的链接，
    // 那是用户目录布局，不是历史归档那棵树下"最后一级被链接顶替"的情形。
    struct stat st{};
    if (::stat(nativeDir.c_str(), &st) != 0) {
        r.native = static_cast<NativeErrorCode>(errno);
        r.blocker = (errno == ENOENT || errno == ENOTDIR) ? WriteBlocker::kParentMissing
                                                         : WriteBlocker::kDenied;
        return r;
    }
    if (!S_ISDIR(st.st_mode)) {
        r.blocker = WriteBlocker::kParentNotDirectory;
        return r;
    }
    // 目标本身用 lstat：一个预先摆好的符号链接要如实标出来（覆盖时替换的是链接本身）。
    std::string nativeTarget;
    if (NativePathBytes(path, &nativeTarget)) {
        struct stat target{};
        if (::lstat(nativeTarget.c_str(), &target) == 0) {
            if (S_ISLNK(target.st_mode)) r.blocker = WriteBlocker::kTargetSymlink;
            else if (S_ISDIR(target.st_mode)) r.blocker = WriteBlocker::kTargetDirectory;
        }
    }
    if (::access(nativeDir.c_str(), W_OK | X_OK) != 0) {
        r.native = static_cast<NativeErrorCode>(errno);
        if (r.blocker == WriteBlocker::kNone) r.blocker = WriteBlocker::kDenied;
    }
    return r;
}

bool SaveFileAtomic(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                    Diagnostic* err) {
    const std::wstring dir = DirectoryOf(path);
    if (dir.empty()) {
        if (err) {
            *err = Diagnostic{codes::kWriteFailed, Msg(L"io.open_failed"), L"--out", path,
                              Msg(L"io.not_absolute"), std::wstring(), std::wstring(),
                              stages::kWrite};
            err->detail = L"precheck:not_absolute";
        }
        return false;
    }

    // 只读预检：只用来解释失败，不据此提前返回。
    const WritePrecheck pre = ProbeWriteTarget(path);

    const std::wstring base = BaseOf(path).substr(0, 32);
    static std::atomic<unsigned long long> sTempCounter{0};
    const std::wstring prefix = dir + L"/~" + base + L".ecapture-" +
                                std::to_wstring(::getpid()) + L"-";
    TempFile temp;
    int fd = -1;
    int createErr = 0;
    for (int attempt = 0; attempt < kTempAttempts; ++attempt) {
        const auto tick = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::wstring candidate =
            prefix + std::to_wstring(tick & 0xFFFF) + L"-" +
            std::to_wstring(sTempCounter.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
        std::string native;
        if (!NativePathBytes(candidate, &native)) {
            createErr = EINVAL;
            break;
        }
        fd = ::open(native.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (fd >= 0) {
            temp.Reset(candidate);
            break;
        }
        createErr = errno;
        if (createErr != EEXIST) break;   // 没权限、目录层面等等：换名字也没用
    }
    if (fd < 0) {
        if (err) {
            const std::wstring hint =
                createErr == ENOENT
                    ? Msg(L"io.dir_missing")
                    : Msgf(L"io.open_failed_hint", Msgf(L"err.win32_code",
                                                        static_cast<unsigned long long>(createErr)));
            *err = Diagnostic{codes::kWriteFailed, Msg(L"io.temp_failed"), L"--out", path, hint,
                              std::wstring(), std::wstring(), stages::kWrite};
            err->win32 = static_cast<uint32_t>(createErr);
            if (pre.blocker != WriteBlocker::kNone) err->detail = PrecheckDetail(pre.blocker);
        }
        return false;
    }

    size_t offset = 0;
    while (offset < bytes.size()) {
        const size_t chunk = std::min<size_t>(bytes.size() - offset, kWriteChunk);
        const ssize_t n = ::write(fd, bytes.data() + offset, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            if (err) *err = IoError(path, L"io.write_interrupted", e, pre);
            return false;   // temp 析构，只删本次的临时名字
        }
        if (n == 0) {
            ::close(fd);
            if (err) *err = IoError(path, L"io.write_interrupted", EIO, pre);
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const int e = errno;
        ::close(fd);
        if (err) *err = IoError(path, L"io.write_interrupted", e, pre);
        return false;
    }
    if (::close(fd) != 0) {
        const int e = errno;
        if (err) *err = IoError(path, L"io.write_interrupted", e, pre);
        return false;
    }

    std::string nativeTemp;
    std::string nativeTarget;
    if (!NativePathBytes(temp.path(), &nativeTemp) || !NativePathBytes(path, &nativeTarget)) {
        if (err) *err = IoError(path, L"io.commit_failed", EINVAL, pre);
        return false;
    }

    if (overwrite) {
        // rename(2) 替换目标**名字**本身：目标是符号链接时替换链接，不会跟到链接目标去写。
        if (::rename(nativeTemp.c_str(), nativeTarget.c_str()) == 0) {
            temp.Dismiss();
            return true;
        }
        const int e = errno;
        if (err) *err = IoError(path, L"io.commit_failed", e, pre);
        return false;
    }

    // 不许替换：link(2) 在目标已存在时原子地返回 EEXIST（符号链接也算存在），
    // 既有文件一个字节都不动。成功后立即删掉临时名，最终文件只剩一个链接计数。
    if (::link(nativeTemp.c_str(), nativeTarget.c_str()) == 0) {
        ::unlink(nativeTemp.c_str());   // 只是清临时名；失败也已在目标上提交，不改结论
        temp.Dismiss();
        return true;
    }
    const int e = errno;
    if (e == EEXIST) {
        if (err) {
            *err = Diagnostic{codes::kFileExists, Msg(L"io.file_exists"), L"--no-overwrite", path,
                              Msg(L"io.file_exists_hint"), std::wstring(), std::wstring(),
                              stages::kWrite};
            err->win32 = static_cast<uint32_t>(e);
        }
        return false;
    }
    if (err) *err = IoError(path, L"io.commit_failed", e, pre);
    return false;
}

}  // namespace ecapture
