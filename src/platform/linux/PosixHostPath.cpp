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
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

constexpr wchar_t kSep = L'/';

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

}  // namespace ecapture
