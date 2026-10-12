// 【平台层 · Linux】文件服务：整份读取一个小文件。
//
// 与 Windows 那一份同一套结论（kNotFound / kDirectory / kOther / tooLarge），
// 动作换成本平台的：路径按 UTF-8 编成原生字节，stat 认"有没有 / 是不是目录"，
// open + fstat 拿大小后**在分配之前**判上限，然后读到尽头。
// 不用 std::ifstream：它拿不到"为什么打不开"的那个 errno，而这一层的三条下场要靠它分开。
#include "HostFile.h"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

// 路径 → 原生字节。带 NUL 码位的路径在本平台根本没法表示（它截断字符串），
// 一律当成"这条路径说不通"，不编出一个悄悄少了一截的路径名去打开。
bool NativePathBytes(const std::wstring& path, std::string* out) {
    if (!out) return false;
    for (wchar_t c : path) {
        if (c == 0) return false;
    }
    *out = EncodeUtf8(path);
    return true;
}

}  // namespace

HostFileRead ReadWholeFileBounded(const std::wstring& path, uint64_t maxBytes) {
    HostFileRead r;
    std::string native;
    if (!NativePathBytes(path, &native)) {
        r.outcome = ReadOutcome::kOther;
        r.native = static_cast<NativeErrorCode>(EINVAL);
        return r;
    }

    struct stat st{};
    if (::stat(native.c_str(), &st) != 0) {
        r.native = static_cast<NativeErrorCode>(errno);
        // 只有"这一级根本没有它"和"中间某一级不是目录"算 not-found；
        // 符号链接成环（ELOOP）是另一件事，不能报成"没这个文件"。
        r.outcome = (errno == ENOENT || errno == ENOTDIR) ? ReadOutcome::kNotFound
                                                          : ReadOutcome::kOther;
        return r;
    }
    if (S_ISDIR(st.st_mode)) {
        r.outcome = ReadOutcome::kDirectory;
        return r;
    }
    if (!S_ISREG(st.st_mode)) {
        // 设备、管道、socket：这些不是"配置文件读不出来"，是问错了对象，如实报别的。
        r.outcome = ReadOutcome::kOther;
        r.native = static_cast<NativeErrorCode>(EINVAL);
        return r;
    }
    // 上限在**分配之前**判：与 Windows 那一份同一条立场。
    if (static_cast<uint64_t>(st.st_size) > maxBytes) {
        r.tooLarge = true;
        r.outcome = ReadOutcome::kOther;
        return r;
    }

    const int fd = ::open(native.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        r.native = static_cast<NativeErrorCode>(errno);
        r.outcome = (errno == ENOENT || errno == ENOTDIR) ? ReadOutcome::kNotFound
                                                          : ReadOutcome::kOther;
        return r;
    }
    r.bytes.resize(static_cast<size_t>(st.st_size));
    size_t total = 0;
    while (total < r.bytes.size()) {
        const ssize_t got = ::read(fd, r.bytes.data() + total, r.bytes.size() - total);
        if (got < 0) {
            if (errno == EINTR) continue;  // 信号打断不算失败，接着读
            r.native = static_cast<NativeErrorCode>(errno);
            r.outcome = ReadOutcome::kOther;
            ::close(fd);
            return r;
        }
        if (got == 0) {
            // 读到尽头却没读满：文件在两步之间变小了。与 Windows 那一路一样按失败处理，
            // 不交回一份"半份文件"当成完整内容。
            r.native = static_cast<NativeErrorCode>(EIO);
            r.outcome = ReadOutcome::kOther;
            ::close(fd);
            return r;
        }
        total += static_cast<size_t>(got);
    }
    ::close(fd);
    r.outcome = ReadOutcome::kOk;
    return r;
}

const wchar_t* NativeErrorCodeLabel() { return L"errno"; }

}  // namespace ecapture
