// 【平台层 · Linux】目录服务：路径类型与建一层目录。
//
// 用 lstat 而不是 stat：最后一级是不是符号链接必须由这一层回答，不能让系统先跟过去。
// 中间层级的链接照常由路径解析跟随（与 Windows 那边对中间 junction 的既有立场一致）。
#include "HostDirectory.h"

#include <cerrno>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

bool NativePathBytes(const std::wstring& path, std::string* out) {
    if (!out) return false;
    for (wchar_t c : path) {
        if (c == 0) return false;
    }
    *out = EncodeUtf8(path);
    return true;
}

PathKind KindFromStat(const struct stat& st) {
    if (S_ISLNK(st.st_mode)) return PathKind::kSymlink;
    if (S_ISDIR(st.st_mode)) return PathKind::kDirectory;
    if (S_ISREG(st.st_mode)) return PathKind::kFile;
    return PathKind::kOther;
}

}  // namespace

PathKind QueryPathKind(const std::wstring& path) {
    std::string native;
    if (!NativePathBytes(path, &native)) return PathKind::kOther;
    struct stat st{};
    if (::lstat(native.c_str(), &st) != 0) {
        return (errno == ENOENT || errno == ENOTDIR) ? PathKind::kMissing : PathKind::kOther;
    }
    return KindFromStat(st);
}

HostDirectoryResult EnsureDirectory(const std::wstring& path) {
    HostDirectoryResult r;
    std::string native;
    if (!NativePathBytes(path, &native)) {
        r.outcome = DirectoryOutcome::kFailed;
        r.native = static_cast<NativeErrorCode>(EINVAL);
        return r;
    }
    switch (QueryPathKind(path)) {
        case PathKind::kDirectory:
            r.outcome = DirectoryOutcome::kAlreadyExists;
            return r;
        case PathKind::kFile:
            r.outcome = DirectoryOutcome::kBlockedFile;
            return r;
        case PathKind::kSymlink:
            r.outcome = DirectoryOutcome::kBlockedSymlink;
            return r;
        case PathKind::kMissing:
        case PathKind::kOther:
            break;
    }
    if (::mkdir(native.c_str(), 0777) == 0) {
        r.outcome = DirectoryOutcome::kCreated;
        return r;
    }
    const int err = errno;
    r.native = static_cast<NativeErrorCode>(err);
    if (err == EEXIST) {
        switch (QueryPathKind(path)) {
            case PathKind::kDirectory: r.outcome = DirectoryOutcome::kAlreadyExists; break;
            case PathKind::kFile: r.outcome = DirectoryOutcome::kBlockedFile; break;
            case PathKind::kSymlink: r.outcome = DirectoryOutcome::kBlockedSymlink; break;
            default: r.outcome = DirectoryOutcome::kFailed; break;
        }
        return r;
    }
    r.outcome = DirectoryOutcome::kFailed;
    return r;
}

}  // namespace ecapture
