// 【跨平台核心】文本编码约定与 UTF-8 编解码。这一份一条都不 include 平台头，
// 两个平台编译同一份源码，判据也只有一份。
#include "core/TextCoding.h"

#include <cstddef>

namespace ecapture {
namespace {

// 一段合法序列：首字节决定长度与码位区间。表按 Unicode 的 Table 3-7 写：
// 下界用来挡过长编码（0xC0 / 0xC1 起手那种），上界用来挡代理区与超范围。
struct LeadInfo {
    std::size_t length;
    uint32_t minCodePoint;
    uint32_t maxCodePoint;
};

bool LeadOf(unsigned char b, LeadInfo* out) {
    if (b < 0x80u) {
        *out = {1u, 0u, 0x7Fu};
        return true;
    }
    if (b >= 0xC2u && b <= 0xDFu) {
        *out = {2u, 0x80u, 0x7FFu};
        return true;
    }
    if (b >= 0xE0u && b <= 0xEFu) {
        // 0xED 的上界收到 D7FF，代理区就进不来；0xE0 的下界与其他三字节首字节相同（0x800）。
        *out = {3u, 0x800u, b == 0xEDu ? 0xD7FFu : 0xFFFFu};
        return true;
    }
    if (b >= 0xF0u && b <= 0xF4u) {
        // 下界 0x10000 挡住 F0 80 80 80 这类过长编码；上界 0x10FFFF 挡住 F4 90.. 那种超范围。
        *out = {4u, 0x10000u, 0x10FFFFu};
        return true;
    }
    return false;  // 0x80..0xBF、0xC0、0xC1、0xF5..0xFF：首字节本身就不合法
}

constexpr uint32_t kReplacement = 0xFFFDu;

void AppendCodePoint(std::wstring* out, uint32_t cp) {
    if constexpr (sizeof(wchar_t) == 2) {
        // UTF-16：非 BMP 拆成一对代理。
        if (cp >= 0x10000u) {
            const uint32_t v = cp - 0x10000u;
            out->push_back(static_cast<wchar_t>(0xD800u + (v >> 10)));
            out->push_back(static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)));
            return;
        }
        out->push_back(static_cast<wchar_t>(cp));
    } else {
        // UTF-32：一个码位一个 wchar_t。
        out->push_back(static_cast<wchar_t>(cp));
    }
}

// 把码位写成 UTF-8。调用方保证 cp 落在 [0, 0x10FFFF] 且不在代理区。
void PushUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80u) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out->push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out->push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out->push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// 两条解码路线共用的那一个扫描器：每次问"从 i 起这一段合不合"。
//   kOk  —— 这一段合法，\*codePoint 是码位，\*advance 是它占用的字节数
//   kBad —— 这一段不合：\*badOffset 是第一处不可用字节，\*advance 是按最长子部分
//           已经吃进去的那几个字节（宽松模式跳过它继续读，严格模式用它定位）
enum class ScanVerdict { kOk, kBad };

ScanVerdict ScanOne(const unsigned char* bytes, std::size_t size, std::size_t i, bool rejectNul,
                    uint32_t* codePoint, std::size_t* advance, std::size_t* badOffset) {
    LeadInfo lead{};
    if (!LeadOf(bytes[i], &lead)) {
        *badOffset = i;
        *advance = 1u;  // 首字节就不合法：坏的就是这一个字节
        return ScanVerdict::kBad;
    }
    if (rejectNul && bytes[i] == 0x00u) {
        *badOffset = i;
        *advance = 1u;
        return ScanVerdict::kBad;
    }
    if (lead.length == 1u) {
        *codePoint = bytes[i];
        *advance = 1u;
        return ScanVerdict::kOk;
    }
    // 先按"能凑齐几个续字节"走一遍：凑不齐是残缺（坏在序列起点），
    // 中途撞上不该出现的字节则坏在那一个字节上 —— 两种报法与逐字节扫描器一致。
    std::size_t got = 1u;
    while (got < lead.length) {
        if (i + got >= size) {
            *badOffset = i;  // 尾部残缺：整段算一处
            *advance = got;
            return ScanVerdict::kBad;
        }
        if ((bytes[i + got] & 0xC0u) != 0x80u) {
            *badOffset = i + got;
            *advance = got;
            return ScanVerdict::kBad;
        }
        ++got;
    }
    uint32_t cp = bytes[i] & (0xFFu >> lead.length);
    for (std::size_t k = 1u; k < lead.length; ++k) {
        cp = (cp << 6) | (bytes[i + k] & 0x3Fu);
    }
    if (cp < lead.minCodePoint || cp > lead.maxCodePoint) {
        // 过长编码 / 超范围：整段（含那几个合法的续字节）算一处，不把续字节再当首字节，
        // 那会把一处坏说成好几处。
        *badOffset = i;
        *advance = lead.length;
        return ScanVerdict::kBad;
    }
    *codePoint = cp;
    *advance = lead.length;
    return ScanVerdict::kOk;
}

}  // namespace

Utf8Decoded DecodeUtf8(const char* data, std::size_t size) {
    Utf8Decoded r;
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    std::size_t i = 0;
    while (i < size) {
        uint32_t cp = 0;
        std::size_t advance = 0;
        std::size_t badOffset = 0;
        if (ScanOne(bytes, size, i, /*rejectNul=*/false, &cp, &advance, &badOffset) ==
            ScanVerdict::kBad) {
            AppendCodePoint(&r.text, kReplacement);
            i += advance;
            if (r.invalidGroups == 0u) r.firstBadOffset = badOffset;
            ++r.invalidGroups;
            continue;
        }
        AppendCodePoint(&r.text, cp);
        i += advance;
    }
    return r;
}

Utf8Decoded DecodeUtf8(const std::vector<uint8_t>& bytes) {
    return DecodeUtf8(bytes.empty() ? nullptr : reinterpret_cast<const char*>(bytes.data()),
                      bytes.size());
}

StrictUtf8Text DecodeUtf8Strict(const std::vector<uint8_t>& bytes, bool rejectNul) {
    StrictUtf8Text r;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t i = 0;
    while (i < bytes.size()) {
        uint32_t cp = 0;
        std::size_t advance = 0;
        std::size_t badOffset = 0;
        if (ScanOne(p, bytes.size(), i, rejectNul, &cp, &advance, &badOffset) ==
            ScanVerdict::kBad) {
            r.ok = false;
            r.badOffset = badOffset;
            r.text.clear();  // 整份拒：不留半份可用的文本给调用方当结论
            return r;
        }
        AppendCodePoint(&r.text, cp);
        i += advance;
    }
    r.ok = true;
    r.badOffset = 0;
    return r;
}

std::string EncodeUtf8(const std::wstring& text) {
    std::string out;
    out.reserve(text.size() * 2u);
    if constexpr (sizeof(wchar_t) == 2) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const uint16_t unit = static_cast<uint16_t>(text[i]);
            if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1 < text.size()) {
                const uint16_t next = static_cast<uint16_t>(text[i + 1]);
                if (next >= 0xDC00u && next <= 0xDFFFu) {
                    const uint32_t cp =
                        0x10000u + (static_cast<uint32_t>(unit - 0xD800u) << 10) +
                        static_cast<uint32_t>(next - 0xDC00u);
                    PushUtf8(&out, cp);
                    ++i;
                    continue;
                }
            }
            // 落单的高/低代理：一个 U+FFFD，不写出半个代理序列。
            if (unit >= 0xD800u && unit <= 0xDFFFu) {
                PushUtf8(&out, kReplacement);
                continue;
            }
            PushUtf8(&out, unit);
        }
    } else {
        for (wchar_t ch : text) {
            const uint32_t cp = static_cast<uint32_t>(ch);
            const bool surrogate = cp >= 0xD800u && cp <= 0xDFFFu;
            PushUtf8(&out, (cp > 0x10FFFFu || surrogate) ? kReplacement : cp);
        }
    }
    return out;
}

bool IsWellFormedUtf8(const char* data, std::size_t size) {
    return DecodeUtf8(data, size).WellFormed();
}

std::size_t CodePointCount(const std::wstring& text) {
    if constexpr (sizeof(wchar_t) == 2) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const uint16_t unit = static_cast<uint16_t>(text[i]);
            // 高代理后面紧跟低代理才算一个字符；其余码元（含落单的代理）各算一个。
            if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1 < text.size()) {
                const uint16_t next = static_cast<uint16_t>(text[i + 1]);
                if (next >= 0xDC00u && next <= 0xDFFFu) ++i;
            }
            ++n;
        }
        return n;
    } else {
        return text.size();
    }
}

std::wstring AsciiLower(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t c : text) out.push_back(AsciiLowerChar(c));
    return out;
}

bool EqualsAsciiInsensitive(const std::wstring& a, const std::wstring& b) {
    return AsciiLower(a) == AsciiLower(b);
}

}  // namespace ecapture
