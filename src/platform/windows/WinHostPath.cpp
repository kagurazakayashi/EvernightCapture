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

#include <algorithm>
#include <iterator>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

constexpr wchar_t kSep = L'\\';
constexpr size_t kMaxNameChars = 80;   // %n 的长度上限，沿用旧实现（改这个数字会让调用方的文件名变样）

bool IsPathSep(wchar_t c) { return c == L'\\' || c == L'/'; }

bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

bool IllegalNameChar(wchar_t c) {
    return c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
           c == L'|' || c == L'?' || c == L'*';
}

// Windows 在普通路径里把这些名字当设备而不是文件（连 CON.png 一起拦）。
bool IsReservedDeviceName(const std::wstring& name) {
    static const wchar_t* const kNames[] = {
        L"con", L"prn", L"aux", L"nul",
        L"com1", L"com2", L"com3", L"com4", L"com5", L"com6", L"com7", L"com8", L"com9",
        L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9",
        nullptr};
    std::wstring lower = name;
    for (wchar_t& c : lower) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    const size_t stemEnd = lower.find(L'.');
    const std::wstring stem = lower.substr(0, stemEnd);
    for (const wchar_t* const* p = kNames; *p; ++p) {
        if (stem == *p) return true;
    }
    return false;
}

void TrimTrailingDotsAndSpaces(std::wstring* s) {
    while (!s->empty() && (s->back() == L' ' || s->back() == L'.')) s->pop_back();
}

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

bool ResolveAppPaths(bool portable, AppPaths* out) {
    if (!out) return false;
    AppPaths paths;
    if (!ExeDirectory(&paths.exeDir)) return false;
    paths.portable = portable;
    // Windows 安装版与便携版的既有位置相同：配置与历史都跟着实际运行的 exe 走。
    // --portable 在这里只把模式如实标出来，不改变任何一个落点。
    paths.configDir = paths.exeDir;
    paths.stateDir = paths.exeDir;
    *out = std::move(paths);
    return true;
}

bool TempDirectory(std::wstring* out) {
    if (!out) return false;
    std::wstring buffer(MAX_PATH + 1, L'\0');
    for (;;) {
        const DWORD n = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (n == 0) return false;
        if (n < buffer.size()) {
            buffer.resize(n);
            break;
        }
        if (buffer.size() > 32768) return false;
        buffer.resize(buffer.size() * 2);
    }
    std::wstring absolute;
    if (!NormalizeAbsolutePath(buffer, &absolute)) return false;
    // GetTempPathW 通常带结尾反斜杠；去掉它方便调用方拼接。盘符根 "C:\" 保留原样
    // （去掉会变成盘符相对的 "C:"，落点就跟着该盘的当前目录跑了）。
    while (absolute.size() > 3 && IsPathSep(absolute.back())) absolute.pop_back();
    *out = std::move(absolute);
    return true;
}

bool LocalDateTimeNow(LocalDateTime* out) {
    if (!out) return false;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    out->year = st.wYear;
    out->month = st.wMonth;
    out->day = st.wDay;
    out->hour = st.wHour;
    out->minute = st.wMinute;
    out->second = st.wSecond;
    return true;
}

uint32_t CurrentProcessId() { return static_cast<uint32_t>(GetCurrentProcessId()); }

int64_t MonotonicClockMs() {
    // 与旧 Deadline.cpp 逐字相同的 QPC 换算：频率整机恒定，取一次缓存；
    // 拿不到频率时按 1000 处理（每个计数就是一毫秒），先除后乘避免 ticks*1000 溢出。
    static const int64_t freq = [] {
        LARGE_INTEGER li{};
        if (!QueryPerformanceFrequency(&li) || li.QuadPart <= 0) return static_cast<int64_t>(1000);
        return static_cast<int64_t>(li.QuadPart);
    }();
    LARGE_INTEGER li{};
    if (!QueryPerformanceCounter(&li)) return 0;
    const int64_t t = li.QuadPart > 0 ? li.QuadPart : 0;
    return (t / freq) * 1000 + ((t % freq) * 1000) / freq;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    // Ordinal 只按码位折叠大小写，不跟区域规则走：culture 比较会把某些看似不同的名字
    // 判成相等，而"是不是同一个文件"不能由一台机器的语言设置决定。
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring SanitizeFileNamePart(const std::wstring& raw) {
    std::wstring s;
    s.reserve(raw.size());
    for (wchar_t c : raw) s.push_back(IllegalNameChar(c) ? L'_' : c);
    TrimTrailingDotsAndSpaces(&s);

    if (s.size() > kMaxNameChars) {
        size_t cut = kMaxNameChars;
        // cut 是截断后的长度：切点上的低代理属于前一个高代理时，退一格，别把一个字劈成两半。
        if (cut > 0 && IsLowSurrogate(s[cut]) && IsHighSurrogate(s[cut - 1])) --cut;
        s.resize(cut);
        while (!s.empty() && IsHighSurrogate(s.back())) s.pop_back();  // 落单的高代理同样非法
        TrimTrailingDotsAndSpaces(&s);  // 截断之后可能又露出尾部的点或空格
    }

    if (s.empty()) return L"_";
    if (IsReservedDeviceName(s)) s.insert(s.begin(), L'_');
    return s;
}

}  // namespace ecapture
