#pragma once
// 跨平台核心：把外部来源的文本安全地放进对话框之前的唯一一份净化判据。
//
// 需要它的现场：窗口标题、窗口类名、--purpose、输出路径都是**外部数据**。确认框要展示它们，
// 而一段带换行、双向控制符或几千个字符的标题可以：
//   * 用换行伪造出"额外一行目标 / 额外一句警告"，把一条目伪装成整体界面；
//   * 用 U+202E（RTL Override）或零宽字符让"谁在请求"看起来像另一件程序；
//   * 把对话框撑爆或挤掉真正的问题与按钮。
// 所以这里只做四件事，而且只写一处：
//   1. 把 C0/C1 控制字符、行/段分隔符、双向控制符、零宽/格式字符换成 U+FFFD；
//   2. 把连续的不安全码位并成一个替换符（不至于把一个恶意标题变成几万个替换符）；
//   3. 按**码位**截断到上限并追加省略号（不是按 wchar_t 单元截，Windows 上不会劈开代理对）；
//   4. 多行文本只允许我们自己拼好的换行（SanitizeDisplayBlock），每一行各自净化。
//
// 这不是过滤器也不是权限判断：它只决定"怎么显示"，不改变是否要确认、也不改变授权范围。
// 对外文字仍由调用方按 --lang 取；这里不生成文案。

#include <cstddef>
#include <cstdint>
#include <string>

namespace ecapture {
namespace display_text {

// 安全的省略号（U+2026）。截断只在码位边界上做，不会把一个代理对拆成两个替换符。
inline constexpr wchar_t kEllipsis = L'\u2026';
inline constexpr wchar_t kReplacement = L'\uFFFD';

inline bool IsHighSurrogate(wchar_t ch) {
    return sizeof(wchar_t) == 2 && ch >= 0xD800 && ch <= 0xDBFF;
}
inline bool IsLowSurrogate(wchar_t ch) {
    return sizeof(wchar_t) == 2 && ch >= 0xDC00 && ch <= 0xDFFF;
}

// 从 i 起读一个码位；返回码位并推进 i。非法 / 落单代理按单个码位处理（不是可行码位，
// 会在 IsUnsafeDisplayCodePoint 里被换成替换符）。
inline char32_t NextCodePoint(const std::wstring& text, size_t* i) {
    const wchar_t first = text[*i];
    if (IsHighSurrogate(first) && *i + 1 < text.size() && IsLowSurrogate(text[*i + 1])) {
        const char32_t cp = 0x10000 + ((static_cast<char32_t>(first) - 0xD800u) << 10) +
                            (static_cast<char32_t>(text[*i + 1]) - 0xDC00u);
        *i += 2;
        return cp;
    }
    *i += 1;
    return static_cast<char32_t>(static_cast<uint32_t>(first));
}

inline size_t CodePointCount(const std::wstring& text, size_t limit = static_cast<size_t>(-1)) {
    size_t count = 0;
    for (size_t i = 0; i < text.size();) {
        if (count == limit) return count;
        (void)NextCodePoint(text, &i);
        ++count;
    }
    return count;
}

// 不安全 / 不该出现在单行展示里的码位：
//   * C0 与 DEL、C1（含换行、回车、制表等）；
//   * U+2028 / U+2029（行 / 段分隔符）；
//   * U+202A..U+202E 与 U+2066..U+2069（双向嵌入 / 覆盖 / 隔离）；
//   * U+200B..U+200F、U+00AD、U+FEFF（零宽 / 软连字符 / BOM）；
//   * U+FFF9..U+FFFB、U+E0000..U+E007F（注释 / 标签字符，可用来隐藏文本）；
//   * 非字符码位与代理区。
inline bool IsUnsafeDisplayCodePoint(char32_t cp) {
    if (cp == 0x09 || cp == 0x0A || cp == 0x0D) return true;   // tab / LF / CR
    if (cp < 0x20 || cp == 0x7F) return true;
    if (cp >= 0x80 && cp <= 0x9F) return true;
    if (cp == 0x00AD) return true;
    if (cp >= 0x200B && cp <= 0x200F) return true;
    if (cp == 0x2028 || cp == 0x2029) return true;
    if (cp >= 0x202A && cp <= 0x202E) return true;
    if (cp >= 0x2066 && cp <= 0x2069) return true;
    if (cp == 0xFEFF) return true;
    if (cp >= 0xFFF9 && cp <= 0xFFFB) return true;
    if (cp >= 0xE0000 && cp <= 0xE007F) return true;
    if (cp >= 0xD800 && cp <= 0xDFFF) return true;   // 落单代理（Linux 上也可能是原样码位）
    if (cp > 0x10FFFF) return true;
    // 非字符：U+FDD0..U+FDEF 与每个平面末尾的 FFFE / FFFF。
    if (cp >= 0xFDD0 && cp <= 0xFDEF) return true;
    if ((cp & 0xFFFE) == 0xFFFE) return true;
    return false;
}

inline bool ContainsUnsafeCodePoint(const std::wstring& text) {
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, &i);
        if (IsUnsafeDisplayCodePoint(cp)) return true;
    }
    return false;
}

// 单行净化：不安全码位并成一个 U+FFFD；最多 maxCodePoints 个码位（不含省略号），
// 超出时截断并追加省略号。空串原样返回（"空"与"被清空"是两件事）。
inline std::wstring SanitizeDisplayLine(const std::wstring& text,
                                        size_t maxCodePoints = 200) {
    std::wstring out;
    out.reserve(text.size() < maxCodePoints * 2 ? text.size() : maxCodePoints * 2);
    size_t written = 0;
    bool lastWasReplacement = false;
    bool truncated = false;
    for (size_t i = 0; i < text.size();) {
        const char32_t cp = NextCodePoint(text, &i);
        const bool unsafe = IsUnsafeDisplayCodePoint(cp);
        if (unsafe && lastWasReplacement) continue;   // 连续的不安全码位并成一个替换符
        if (written >= maxCodePoints) {
            truncated = true;
            break;
        }
        const wchar_t ch = unsafe ? kReplacement : static_cast<wchar_t>(cp);
        out.push_back(ch);
        lastWasReplacement = unsafe;
        ++written;
    }
    if (truncated) out.push_back(kEllipsis);
    return out;
}

// 路径 / 名称这类"一行一个事实"的展示：上限更小，避免一个超长路径挤掉别的字段。
inline std::wstring SanitizeDisplayValue(const std::wstring& text,
                                         size_t maxCodePoints = 160) {
    return SanitizeDisplayLine(text, maxCodePoints);
}

// 多行文本：只允许 \n 作为行分隔（\r\n / \r 先归一成 \n），每行单独净化，行数有上限；
// 超出时最后补一行省略号。用于"调用方自述"这类可能本身带多行的文本。
inline std::wstring SanitizeDisplayBlock(const std::wstring& text, size_t maxLines = 6,
                                         size_t maxCodePointsPerLine = 200) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            normalized.push_back(L'\n');
        } else {
            normalized.push_back(text[i]);
        }
    }
    std::wstring out;
    size_t lineIndex = 0;
    size_t start = 0;
    bool truncated = false;
    while (start <= normalized.size()) {
        size_t end = normalized.find(L'\n', start);
        if (end == std::wstring::npos) end = normalized.size();
        if (lineIndex >= maxLines) {
            truncated = true;
            break;
        }
        const std::wstring line = normalized.substr(start, end - start);
        if (lineIndex != 0) out.push_back(L'\n');
        out += SanitizeDisplayLine(line, maxCodePointsPerLine);
        ++lineIndex;
        if (end == normalized.size()) break;
        start = end + 1;
    }
    if (truncated) {
        if (!out.empty()) out.push_back(L'\n');
        out.push_back(kEllipsis);
    }
    return out;
}

}  // namespace display_text
}  // namespace ecapture
