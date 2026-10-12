#pragma once
// 跨平台核心：文本编码约定，以及只依赖这一份约定的编解码与 ASCII 判定。
//
// 【全仓的文字约定】唯一写在这里，别处不再各说一遍：
//   * 内存里是 std::wstring。Windows 构建上 wchar_t 是 16 位，一份 wstring 就是 UTF-16 码元
//     序列（非 BMP 字符占一对代理）；Linux 构建上 wchar_t 是 32 位，一个 wchar_t 就是一个
//     Unicode 码位。两者都是"同一条 Unicode 文本"，差别只在码元宽度，所以凡按"字符数"算的
//     上限都要走 CodePointCount()，不拿 wstring::size() 当字符数。
//   * 落到字节一律是 UTF-8（无 BOM）：命令行参数、resources/strings-*.txt 文案、ECAPTURE.ini、
//     标准输出与标准错误、JSON 里的非 ASCII，全按 UTF-8。
//   * 换码只发生在边界：命令行入口、嵌入文案的取出、文件读写、标准流写出。中间的解析、判定、
//     渲染只搬 wstring，不在中途换编码（换一次就多一次损坏的机会）。
//
// 【非法编码的处理】三条，一条都不许在调用点各自发明：
//   1. 不换个代码页再试一遍，也不丢掉非法字节之后的内容 —— 按 Unicode 推荐转义法把一段非法
//      字节换成一个 U+FFFD，并**如实计数**交回调用方（invalidGroups）。
//   2. 计数不是装饰：调用方据此决定"这一条整条拒掉"还是"带着诊断继续"，绝不把 U+FFFD 当成
//      一次合法的读取成功。
//   3. 编码方向反过来也一样：无法表示的码元（落单的代理、超出 U+10FFFF）换成 U+FFFD，
//      交出去的字节序列恒为合法 UTF-8，不会写出半个序列让下游读到乱码。
//
// 【大小写与空白】选项名、枚举取值、INI 的 section/key 这些"合同写着大小写不敏感"的地方
// 只折叠 ASCII（AsciiLower）：那条不敏感性说的是 ASCII 字母，跟系统语言与 C 运行期 locale
// 无关。窗口标题、类名、进程名那一路的忽略大小写属于**匹配语义**，各平台自己实现
// （Windows 的文件系统/进程名不分大小写，POSIX 分），不在这一层预设立场。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ecapture {

// 一次 UTF-8 解码的下场：文本 + 非法段数。
struct Utf8Decoded {
    std::wstring text;
    std::size_t invalidGroups = 0;  // 非法字节序列的段数，0 = 整份都合
    // 第一处非法序列里"第一个不可用字节"的偏移；整份合时为 0（这一格没有意义，
    // 所以判它要一起看 invalidGroups，不能拿 0 当"坏在开头"）。
    std::size_t firstBadOffset = 0;

    bool WellFormed() const { return invalidGroups == 0; }
};

// 严格按 UTF-8 解码（最长子部分规则）：过长编码、代理区码位、超过 U+10FFFF、
// 尾部残缺、以及多字节序列中间撞上不该出现的字节，都算**一段**非法，
// 换成一个 U+FFFD 后从第一个不该出现的字节重新起头。
Utf8Decoded DecodeUtf8(const char* data, std::size_t size);
Utf8Decoded DecodeUtf8(const std::vector<uint8_t>& bytes);
inline Utf8Decoded DecodeUtf8(const std::string& text) {
    return DecodeUtf8(text.data(), text.size());
}

// 整份合，或者整份拒 —— 给"读不懂就不读"的那一路输入用（ECAPTURE.ini）。
struct StrictUtf8Text {
    std::wstring text;
    bool ok = false;
    // ok 为 false 时，这是**第一处**不可用字节的偏移；语义与逐字节扫描器一致：
    // 残缺的尾部报在序列起点，坏的续字节报在那一个字节上。
    std::size_t badOffset = 0;
};
// NUL 是合法 UTF-8，但它在文本文件里把内容截断、又在编辑器里看不见，所以这一路的判据
// 是"能不能当作一份人写的文本读"，rejectNul 由调用方按那一条规矩给定。
StrictUtf8Text DecodeUtf8Strict(const std::vector<uint8_t>& bytes, bool rejectNul);

// 编成 UTF-8，返回值恒为合法序列（见上面第 3 条）。
std::string EncodeUtf8(const std::wstring& text);

// 整份是不是合法的 UTF-8。判据与 DecodeUtf8 同一条，不另写一份状态机。
bool IsWellFormedUtf8(const char* data, std::size_t size);
inline bool IsWellFormedUtf8(const std::vector<uint8_t>& bytes) {
    return IsWellFormedUtf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// 码位数（不是码元数，也不是字节数）：长度上限用它算，两个平台对同一句人话得到同一个数。
std::size_t CodePointCount(const std::wstring& text);

// 四条一条都不依赖 locale，也不碰非 ASCII：折叠只管 A-Z，空白只管那六个 ASCII 控制空白，
// 字母与数字只管 a-zA-Z0-9。写在这里而不是各调用点手抄一遍，是因为"合同里那句忽略大小写"
// 到底覆盖哪几个字符，全仓只能有一个答案。
inline constexpr wchar_t AsciiLowerChar(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}
inline constexpr bool IsAsciiAlpha(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}
inline constexpr bool IsAsciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
inline constexpr bool IsAsciiSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\v' || c == L'\f' || c == L'\r';
}

std::wstring AsciiLower(const std::wstring& text);
bool EqualsAsciiInsensitive(const std::wstring& a, const std::wstring& b);

}  // namespace ecapture
