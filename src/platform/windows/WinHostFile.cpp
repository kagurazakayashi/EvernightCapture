// 【平台层 · Windows】文件服务：整份读取一个小文件。
//
// 判据与 ECAPTURE 一直的 Windows 行为逐字相同：先 GetFileAttributesW 认"有没有 / 是不是目录"，
// 再以共享读写打开，GetFileSizeEx 拿到大小后**在分配之前**判上限，然后一次读到底。
#include "HostFile.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {

HostFileRead ReadWholeFileBounded(const std::wstring& path, uint64_t maxBytes) {
    HostFileRead r;
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD gle = GetLastError();
        r.native = gle;
        r.outcome = (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND)
                        ? ReadOutcome::kNotFound
                        : ReadOutcome::kOther;
        return r;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        r.outcome = ReadOutcome::kDirectory;
        return r;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD gle = GetLastError();
        r.native = gle;
        r.outcome = gle == ERROR_FILE_NOT_FOUND ? ReadOutcome::kNotFound : ReadOutcome::kOther;
        return r;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        r.native = GetLastError();
        r.outcome = ReadOutcome::kOther;
        CloseHandle(h);
        return r;
    }
    // 上限在**分配之前**判：一份超限的文件不该先被整个搬进内存再报错。
    if (static_cast<uint64_t>(size.QuadPart) > maxBytes) {
        r.tooLarge = true;
        r.outcome = ReadOutcome::kOther;
        CloseHandle(h);
        return r;
    }
    r.bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD total = 0;
    while (total < static_cast<DWORD>(r.bytes.size())) {
        DWORD got = 0;
        if (!ReadFile(h, r.bytes.data() + total, static_cast<DWORD>(r.bytes.size() - total), &got,
                      nullptr) ||
            got == 0) {
            r.native = GetLastError();
            r.outcome = ReadOutcome::kOther;
            CloseHandle(h);
            return r;
        }
        total += got;
    }
    CloseHandle(h);
    r.outcome = ReadOutcome::kOk;
    return r;
}

const wchar_t* NativeErrorCodeLabel() { return L"win32"; }

}  // namespace ecapture
