// 【平台层 · Windows】路径服务：Win32 的路径形状与绝对化。
//
// 这一份的判据就是 ECAPTURE 在 Windows 上一直的行为，逐字保留：
//   * 分隔符认 \ 与 / 两种（cmd 与资源管理器都接受 /）；
//   * 盘符、盘符相对、根相对、UNC 与扩展路径这五种形状都按 GetFullPathNameW 的语义判；
//   * 绝对化交给 GetFullPathNameW（纯字面处理，不碰文件系统，也不展开环境变量）。
#include "HostPath.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <iterator>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

constexpr wchar_t kSep = L'\\';

bool IsPathSep(wchar_t c) { return c == L'\\' || c == L'/'; }

}  // namespace

ConfigPathFate ClassifyConfigPath(const std::wstring& raw) {
    if (raw.size() >= 2 && raw[1] == L':' && IsAsciiAlpha(raw[0])) {
        // 第三个字符是分隔符才是盘符绝对路径；`C:` / `C:shot.png` 是盘符相对。
        return raw.size() >= 3 && IsPathSep(raw[2]) ? ConfigPathFate::kQualified
                                                    : ConfigPathFate::kDriveRelative;
    }
    if (IsPathSep(raw[0])) {
        // 两条分隔符起手 = UNC 或扩展路径（完全限定）；单条 = 当前盘的根（歧义）。
        return (raw.size() >= 2 && IsPathSep(raw[1])) ? ConfigPathFate::kQualified
                                                      : ConfigPathFate::kRootRelative;
    }
    return ConfigPathFate::kRelativeToIni;
}

bool LooksLikePath(const std::wstring& token) { return token.find_first_of(L"\\/") != std::wstring::npos; }

bool AcceptsSlashOptionPrefix() { return true; }

bool NormalizeAbsolutePath(const std::wstring& in, std::wstring* out) {
    // GetFullPathNameW 的动态缓冲区版：要多少给多少，放不下或说不通就**如实失败**。
    const DWORD need = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    if (need == 0) return false;
    std::wstring buf(static_cast<size_t>(need), L'\0');
    // 成功返回的是写进去的字符数（不含终止符）；返回原需求长度 = 缓冲区不够。
    const DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(buf.size()), buf.data(),
                                     nullptr);
    if (n == 0 || n >= buf.size()) return false;
    buf.resize(n);
    *out = std::move(buf);
    return true;
}

std::wstring AbsoluteFromCwd(const std::wstring& raw) {
    if (raw.empty()) return raw;
    wchar_t buffer[8192];
    const DWORD n = GetFullPathNameW(raw.c_str(), static_cast<DWORD>(std::size(buffer)), buffer,
                                     nullptr);
    if (n == 0 || n >= std::size(buffer)) return raw;  // 说不通就原样交回去，由对面如实报错
    return std::wstring(buffer, n);
}

const wchar_t* PathSeparatorSet() { return L"\\/"; }

std::wstring JoinDirAndFile(const std::wstring& dir, const std::wstring& file) {
    std::wstring out = dir;
    out += kSep;
    out += file;
    return out;
}

bool SplitDirAndFile(const std::wstring& path, std::wstring* dir, std::wstring* file) {
    if (path.empty() || !dir || !file) return false;
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos || sep + 1 >= path.size()) return false;
    *dir = path.substr(0, sep);
    *file = path.substr(sep + 1);
    return true;
}

bool ExeDirectory(std::wstring* out) {
    if (!out) return false;
    std::wstring buffer(4096, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) return false;
    buffer.resize(n);
    std::wstring dir;
    std::wstring exeName;
    if (!SplitDirAndFile(buffer, &dir, &exeName)) return false;
    *out = std::move(dir);
    return true;
}

}  // namespace ecapture
