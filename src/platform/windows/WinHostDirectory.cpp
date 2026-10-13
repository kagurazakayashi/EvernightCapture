// 【平台层 · Windows】目录服务：路径类型与建一层目录。
#include "HostDirectory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {

PathKind QueryPathKind(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD gle = GetLastError();
        return (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND)
                   ? PathKind::kMissing
                   : PathKind::kOther;
    }
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return PathKind::kSymlink;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return PathKind::kDirectory;
    return PathKind::kFile;
}

HostDirectoryResult EnsureDirectory(const std::wstring& path) {
    HostDirectoryResult r;
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
    if (CreateDirectoryW(path.c_str(), nullptr)) {
        r.outcome = DirectoryOutcome::kCreated;
        return r;
    }
    const DWORD gle = GetLastError();
    r.native = gle;
    if (gle == ERROR_ALREADY_EXISTS) {
        // 建的同时别人先建好了：再问一次类型，只有普通目录才算"已经备好"。
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
