#include "FileSave.h"

#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Lang.h"

// ---------------------------------------------------------------------------
// 保证边界（都写进文档，别当成万能药）
//
// * 同一卷上的重命名是原子的，所以"看不到半个文件"这条在本机文件系统上成立。
//   网络共享（SMB / WebDAV）上 MOVEFILE_REPLACE_EXISTING 的语义由服务端决定，
//   只能保证"要么旧内容要么失败"，不能保证别的应用不会恰好看到提交那一下。
// * 权限与属性：临时文件在目标目录下创建，继承该目录的 ACL，与直接建目标文件的差别只在名字。
//   属性用 FILE_ATTRIBUTE_NORMAL（不带 TEMPORARY / HIDDEN，否则改名后的目标会保留这些属性）。
//   作为代价：覆盖一个只读或带特殊属性的旧文件时，改名会像直接写那样报 ACCESS_DENIED，
//   旧文件保持原样。
// * 预检（OutputPlan）认不出来的别名：8.3 短名、硬链接、目录 junction / 符号链接、
//   UNC 与盘符两种写法。这些只能由"最终那次不许替换的创建/改名"当场判掉，
//   所以本文件里没有任何"先看一眼在不在"的预检。
// ---------------------------------------------------------------------------

namespace ecapture {
namespace {

constexpr DWORD kWriteChunk = 1u << 20;   // 1 MiB：一次 WriteFile 别太大，免得整块缓冲被吃掉
constexpr int kTempAttempts = 8;          // 临时文件撞名就换一个，撞够次数就认输

// 文件句柄：RAII，出作用域必关（失败路径上漏关会让后面的改名撞 ACCESS_DENIED）
class Handle {
public:
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Close(); }

    void Reset(HANDLE h) { Close(); handle_ = h; }
    HANDLE get() const { return handle_; }
    bool valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    void Close() {
        if (valid()) CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

// 本次拥有的临时文件：析构时删掉自己那一个；改名成功后交还（不再删）。
class TempFile {
public:
    TempFile() = default;
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile() { Discard(); }

    void Reset(std::wstring path) { path_ = std::move(path); }
    const std::wstring& path() const { return path_; }
    void Dismiss() { path_.clear(); }

private:
    void Discard() {
        if (path_.empty()) return;
        DeleteFileW(path_.c_str());
        path_.clear();
    }
    std::wstring path_;
};

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return path.substr(0, slash + 1);   // 带上结尾的分隔符，拼接时不用再判
}

std::wstring BaseOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// 出问题时把 Win32 错误码同时写成给人看的提示与机器可判的字段。
// gle 必须在失败点立刻取走（任何 Msg / 资源读取都会把它覆盖掉），所以由调用方传进来。
Diagnostic IoError(const std::wstring& path, const wchar_t* messageKey, DWORD gle) {
    Diagnostic d{codes::kWriteFailed, Msg(messageKey), L"--out", path,
                 Msgf(L"err.win32_code", gle)};
    d.stage = stages::kWrite;
    d.win32 = gle;
    return d;
}

}  // namespace

bool SaveFileAtomic(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                    Diagnostic* err) {
    const std::wstring dir = DirectoryOf(path);
    if (dir.empty()) {
        // 调用方给的不是绝对路径，落到哪由当前目录决定，也就无法保证同卷改名。
        if (err) {
            *err = Diagnostic{codes::kWriteFailed, Msg(L"io.open_failed"), L"--out", path,
                              Msg(L"io.not_absolute"), std::wstring(), std::wstring(),
                              stages::kWrite};
        }
        return false;
    }

    // ---- 1) 同目录唯一临时文件 ----
    // 名字里带上本次 PID 与两个变化段（进程内计数 + 启动时刻）：并发写同一路径的进程各有各的
    // 临时文件，同一进程连写多张（--all）也各用各的，谁也不会踩谁；CREATE_NEW 保证不覆盖既有文件。
    const std::wstring base = BaseOf(path).substr(0, 32);
    static long sTempCounter = 0;
    const std::wstring prefix =
        dir + L"~" + base + L".ecapture-" + std::to_wstring(GetCurrentProcessId()) + L"-";
    TempFile temp;
    Handle handle;
    DWORD createGle = 0;
    for (int attempt = 0; attempt < kTempAttempts; ++attempt) {
        const std::wstring candidate =
            prefix + std::to_wstring(static_cast<unsigned long long>(GetTickCount64() & 0xFFFF)) +
            L"-" + std::to_wstring(InterlockedIncrement(&sTempCounter)) + L".tmp";
        HANDLE h = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        createGle = GetLastError();
        if (h != INVALID_HANDLE_VALUE) {
            temp.Reset(candidate);
            handle.Reset(h);
            break;
        }
        // 只有"名字已存在"值得再试一次；目录不存在、没权限这些换个名字也是白换
        if (createGle != ERROR_FILE_EXISTS && createGle != ERROR_ALREADY_EXISTS) break;
    }
    if (!handle.valid()) {
        if (err) {
            const std::wstring hint = createGle == ERROR_PATH_NOT_FOUND
                                          ? Msg(L"io.dir_missing")
                                          : Msgf(L"io.open_failed_hint", Msgf(L"err.win32_code", createGle));
            *err = Diagnostic{codes::kWriteFailed, Msg(L"io.temp_failed"), L"--out", path, hint,
                              std::wstring(), std::wstring(), stages::kWrite};
            err->win32 = createGle;
        }
        return false;
    }

    // ---- 2) 写全 + 短写检查 + 刷新 ----
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, kWriteChunk));
        DWORD written = 0;
        const BOOL ok = WriteFile(handle.get(), bytes.data() + offset, chunk, &written, nullptr);
        const DWORD gle = GetLastError();
        if (!ok || written != chunk) {
            handle.Close();
            if (err) *err = IoError(path, L"io.write_interrupted", gle);
            return false;   // temp 析构，只删自己的临时文件，目标名一个字都没动
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle.get())) {
        const DWORD gle = GetLastError();
        handle.Close();
        if (err) *err = IoError(path, L"io.write_interrupted", gle);
        return false;
    }
    handle.Close();   // 必须先进入关闭状态，改名才不会撞自己的句柄

    // ---- 3) 提交：目标是否已存在，由这一次重命名原子决定 ----
    const DWORD flags =
        MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0u);
    if (MoveFileExW(temp.path().c_str(), path.c_str(), flags)) {
        temp.Dismiss();
        return true;
    }
    const DWORD gle = GetLastError();   // 先取码：下面拼文案要读资源，错误码会被覆盖
    if (!overwrite && (gle == ERROR_FILE_EXISTS || gle == ERROR_ALREADY_EXISTS)) {
        if (err) {
            *err = Diagnostic{codes::kFileExists, Msg(L"io.file_exists"), L"--no-overwrite", path,
                              Msg(L"io.file_exists_hint"), std::wstring(), std::wstring(),
                              stages::kWrite};
            err->win32 = gle;
        }
        return false;
    }
    if (err) *err = IoError(path, L"io.commit_failed", gle);
    return false;   // temp 析构删掉本次的临时文件；目标保持原样
}

}  // namespace ecapture
