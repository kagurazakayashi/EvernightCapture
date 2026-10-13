// 【平台层 · Linux】路径服务：POSIX 的路径形状与绝对化。
//
// 这一份刻意**不**照抄 Windows 那套形状判据（AGENTS.md：不把 Windows 的规则机械搬到 Linux）：
//   * 分隔符只有 '/'。反斜杠在 POSIX 文件名里是一个普通字符，`a\b.png` 就是一个文件的原名。
//   * 没有盘符、没有盘符相对、也没有根相对这一类：`C:shot.png` 是当前目录下一个合法的相对文件名，
//     属于"一般相对写法"，不是歧义写法。所以 ClassifyConfigPath 永远不会交回
//     kDriveRelative / kRootRelative —— 不是漏了，是这两个概念在本平台不存在。
//   * 绝对化是**纯字面**的（折叠 //、/. 与 /..，补当前目录），不查文件系统、不解析符号链接、
//     不展开环境变量：Windows 那一路的 GetFullPathNameW 同样是字面处理，两条立场要对齐。
//     为什么不用 std::filesystem：它在 wchar_t ↔ 原生字节之间要过一层 locale 相关的编码转换，
//     而本项目的文字约定是"字节一律 UTF-8"（见 src/core/TextCoding.h），不交给运行期 locale 决定。
#include "HostPath.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

constexpr wchar_t kSep = L'/';
constexpr const wchar_t* kAppDirName = L"evernightcapture";

std::wstring LexicalNormalize(const std::wstring& in);

bool EnvUtf8(const char* name, std::wstring* out) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return false;
    auto decoded = DecodeUtf8(raw, std::strlen(raw));
    if (!decoded.WellFormed()) return false;
    *out = std::move(decoded.text);
    return true;
}

// XDG 规定这两个变量必须是绝对路径；设了相对值时按未设置处理，不用当前目录去猜。
// fromEnv 说明这一份是用户显式给的基准，还是从 HOME 推出来的（决定首次建目录时要补哪几层）。
bool XdgBase(const char* var, const wchar_t* homeRelative, std::wstring* out, bool* fromEnv) {
    if (fromEnv) *fromEnv = false;
    std::wstring value;
    if (EnvUtf8(var, &value) && !value.empty() && value[0] == kSep) {
        *out = LexicalNormalize(value);
        if (fromEnv) *fromEnv = true;
        return true;
    }
    std::wstring home;
    if (!EnvUtf8("HOME", &home) || home.empty() || home[0] != kSep) return false;
    *out = LexicalNormalize(JoinDirAndFile(home, homeRelative));
    return true;
}

bool CurrentDirectory(std::wstring* out) {
    if (!out) return false;
    std::vector<char> buf(1024);
    for (;;) {
        if (getcwd(buf.data(), buf.size())) {
            // 目录名本身不合 UTF-8 时如实失败：交回一份带 U+FFFD 的"当前目录"，
            // 拼出来的绝对路径永远对不上真实目录，那比报不出错更坏。
            auto decoded = DecodeUtf8(buf.data(), std::strlen(buf.data()));
            if (!decoded.WellFormed()) return false;
            *out = std::move(decoded.text);
            return true;
        }
        if (errno == ERANGE && buf.size() < (1u << 16u)) {
            buf.resize(buf.size() * 2u);
            continue;
        }
        return false;
    }
}

// 字面归一：按 '/' 切节，丢掉空节与 "."，遇到 ".." 退一节（退过根就停在根）。
std::wstring LexicalNormalize(const std::wstring& in) {
    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (pos <= in.size()) {
        const size_t sep = in.find(kSep, pos);
        const std::wstring piece =
            in.substr(pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos);
        if (piece.empty() || piece == L".") {
            // 空节来自重复分隔符或首尾分隔符，跳过
        } else if (piece == L"..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(piece);
        }
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }
    std::wstring out;
    for (const std::wstring& p : parts) {
        out.push_back(kSep);
        out += p;
    }
    return out.empty() ? std::wstring(1, kSep) : out;
}

}  // namespace

ConfigPathFate ClassifyConfigPath(const std::wstring& raw) {
    // 以 '/' 起手就是完全限定；其余一律是"相对基准待定"的一般写法。
    // 盘符相对与根相对这两类在本平台不存在，所以这里不返回它们，也不把它们当成歧义来拒。
    if (!raw.empty() && raw[0] == kSep) return ConfigPathFate::kQualified;
    return ConfigPathFate::kRelativeToIni;
}

bool LooksLikePath(const std::wstring& token) { return token.find(kSep) != std::wstring::npos; }

bool AcceptsSlashOptionPrefix() { return false; }

bool NormalizeAbsolutePath(const std::wstring& in, std::wstring* out) {
    if (!out || in.empty()) return false;
    std::wstring base = in;
    if (base[0] != kSep) {
        std::wstring cwd;
        if (!CurrentDirectory(&cwd) || cwd.empty()) return false;
        base = LexicalNormalize(cwd + kSep + base);
    }
    *out = LexicalNormalize(base);
    return true;
}

std::wstring AbsoluteFromCwd(const std::wstring& raw) {
    if (raw.empty()) return raw;
    std::wstring abs;
    // 定不出当前目录（已被删除、权限）时原样交回去，由对面如实报错 —— 与 Windows 那一路同一条立场。
    return NormalizeAbsolutePath(raw, &abs) ? abs : raw;
}

const wchar_t* PathSeparatorSet() { return L"/"; }

std::wstring JoinDirAndFile(const std::wstring& dir, const std::wstring& file) {
    std::wstring out = dir;
    if (!out.empty() && out.back() == kSep) out.pop_back();  // 根目录 "/" 拼出来不该是 "//x"
    out.push_back(kSep);
    out += file;
    return out;
}

bool SplitDirAndFile(const std::wstring& path, std::wstring* dir, std::wstring* file) {
    if (path.empty() || !dir || !file) return false;
    const size_t sep = path.find_last_of(kSep);
    if (sep == std::wstring::npos || sep + 1 >= path.size()) return false;
    *dir = sep == 0 ? std::wstring(1, kSep) : path.substr(0, sep);
    *file = path.substr(sep + 1);
    return true;
}

bool ExeDirectory(std::wstring* out) {
    if (!out) return false;
    std::vector<char> buf(1024);
    for (;;) {
        // /proc/self/exe 是内核给出的**真实**可执行文件：如果程序是通过符号链接启动的，
        // 这里拿到的是链接目标所在的目录。便携模式因此把数据放在真正的程序旁边，
        // 而不是放在某人随手建的那个链接旁边。
        const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size());
        if (n < 0) return false;
        if (static_cast<size_t>(n) >= buf.size()) {
            if (buf.size() >= (1u << 16u)) return false;  // 长到六十四KB 还不是：不当它是可执行文件路径
            buf.resize(buf.size() * 2u);
            continue;
        }
        auto decoded = DecodeUtf8(buf.data(), static_cast<size_t>(n));
        if (!decoded.WellFormed()) return false;  // 同 CurrentDirectory：不合 UTF-8 就说不清自己在哪
        std::wstring dir;
        std::wstring exeName;
        if (!SplitDirAndFile(decoded.text, &dir, &exeName)) return false;
        *out = std::move(dir);
        return true;
    }
}

bool ResolveAppPaths(bool portable, AppPaths* out) {
    if (!out) return false;
    AppPaths paths;
    if (!ExeDirectory(&paths.exeDir)) return false;
    paths.portable = portable;
    if (portable) {
        // 便携模式必须由调用方明确给出（--portable），不因为程序目录恰好可写就自动进入。
        paths.configDir = paths.exeDir;
        paths.stateDir = paths.exeDir;
        *out = std::move(paths);
        return true;
    }
    std::wstring configBase;
    std::wstring stateBase;
    bool stateFromEnv = false;
    // 配置与状态的基准各自解析：一个不可用不该把另一个也变成"问不出来"。
    // 调用方各自检查自己需要的那个目录，缺失时按各自的诊断报错。
    const bool configOk = XdgBase("XDG_CONFIG_HOME", L".config", &configBase, nullptr);
    const bool stateOk = XdgBase("XDG_STATE_HOME", L".local/state", &stateBase, &stateFromEnv);
    if (!configOk && !stateOk) return false;
    if (configOk) paths.configDir = LexicalNormalize(JoinDirAndFile(configBase, kAppDirName));
    if (stateOk) {
        paths.stateDir = LexicalNormalize(JoinDirAndFile(stateBase, kAppDirName));

        // 首次归档时按顺序补状态目录。XDG_STATE_HOME 是用户明确给的基准，补它与应用目录；
        // 默认基准 ~/.local/state 还要补标准的 ~/.local 这一层。HOME 不存在时第一层就建不出来，
        // 于是如实报 history.unavailable，而不是把用户主目录也顺手造出来。
        std::vector<std::wstring> chain;
        if (stateFromEnv) {
            chain.push_back(stateBase);
        } else {
            std::wstring localBase;
            std::wstring stateLeaf;
            if (SplitDirAndFile(stateBase, &localBase, &stateLeaf)) chain.push_back(localBase);
            chain.push_back(stateBase);
        }
        chain.push_back(paths.stateDir);
        paths.stateCreateChain = std::move(chain);
    }
    *out = std::move(paths);
    return true;
}

bool TempDirectory(std::wstring* out) {
    if (!out) return false;
    std::wstring value;
    if (EnvUtf8("TMPDIR", &value) && !value.empty() && value[0] == kSep) {
        *out = LexicalNormalize(value);
        return true;
    }
    *out = L"/tmp";
    return true;
}

bool LocalDateTimeNow(LocalDateTime* out) {
    if (!out) return false;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (localtime_r(&now, &local) == nullptr) return false;
    out->year = local.tm_year + 1900;
    out->month = local.tm_mon + 1;
    out->day = local.tm_mday;
    out->hour = local.tm_hour;
    out->minute = local.tm_min;
    out->second = local.tm_sec;
    return true;
}

uint32_t CurrentProcessId() { return static_cast<uint32_t>(::getpid()); }

int64_t MonotonicClockMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    // POSIX 文件系统区分大小写：/tmp/A.png 与 /tmp/a.png 不是同一个文件。
    // 调用方传进来的是已经字面归一过的绝对路径；这里不做 Windows 式折叠。
    return a == b;
}

std::wstring SanitizeFileNamePart(const std::wstring& raw) {
    // POSIX 只有 '/' 与 NUL 不能出现在文件名里；反斜杠、冒号、问号、保留设备名都是普通字符。
    // 不照搬 Windows 的非法字符表，否则用户会得到一个他并没有要求的改名。
    std::wstring s;
    s.reserve(raw.size());
    for (wchar_t c : raw) s.push_back((c == kSep || c == 0) ? L'_' : c);
    // "." 与 ".." 是目录项而不是文件名，不能原样当组件使用。
    if (s == L"." || s == L"..") return L"_";
    // 长度按码位截断（Linux 的 wchar_t 一码位一个），不把多字节字符劈成半个。
    constexpr size_t kMaxNameChars = 80;
    if (s.size() > kMaxNameChars) s.resize(kMaxNameChars);
    return s.empty() ? L"_" : s;
}

}  // namespace ecapture
