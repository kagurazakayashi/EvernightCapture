#pragma once
// 平台服务：路径的形状与绝对化。
//
// 声明共用，实现分平台（src/platform/windows/WinHostPath.cpp 与
// src/platform/linux/PosixHostPath.cpp）。为什么单独一层：路径"长什么样"是系统的事，不是判据的事。
//   * 盘符（`C:`）、盘符相对（`C:shot.png`）、根相对（`\shot.png`）、UNC（`\\server\share\x`）
//     与扩展路径（`\\?\C:\shots\a.png`）这五种形状只有 Windows 有；
//   * POSIX 上 '/' 才是分隔符，'\' 只是文件名里的一个普通字符，而 `C:shot.png` 是一个完全合法的
//     相对文件名（就在当前目录下，名字叫 "C:shot.png"）。把 Windows 的分类硬套过来，会把一句
//     正常写法误判成歧义；把 Windows 的"以 / 起手也算选项前缀"套过来，会把 `/tmp/out.png`
//     这个位置参数读成一个叫 "tmp/out.png" 的选项。
// 所以共用层只共用**结论词表**（ConfigPathFate）与"定一条绝对路径"这个动作，
// 各平台按自己的规则得出结论。判断规则不许在调用点各写一份。

#include <string>

namespace ecapture {

// 配置文件里那一条输出路径的形状。两类实现的注释各自写清"这一类在本平台是怎么定出来的"，
// 合并与报错读的都是这里那一个结论（判据只写一处）。
enum class ConfigPathFate {
    kQualified,      // 完全限定：与调用方的当前目录、当前盘无关，原样交出
    kRelativeToIni,  // 一般相对写法：基准 = 被选中的那份 INI 自己所在的目录
    kDriveRelative,  // 歧义：`C:shot.png` 说的是"C 盘当前目录下"（POSIX 上永远不会得出这一类）
    kRootRelative,   // 歧义：`\shot.png` 说的是"当前盘的根下"（POSIX 上永远不会得出这一类）
};

// 分类一条用户写下的路径。
ConfigPathFate ClassifyConfigPath(const std::wstring& raw);

// 这一条参数里出现了本平台的路径分隔符吗？用于"长得像路径就当位置参数"那一判。
// Windows 认 \ 与 / 两种（历史习惯），POSIX 只认 / —— 反斜杠在那儿不是分隔符。
bool LooksLikePath(const std::wstring& token);

// 本平台要不要把以 '/' 起手的写法当选项（Windows 的 cmd 习惯：/foo=bar）。
// POSIX 上返回 false：以 '/' 起手的只能是路径。
bool AcceptsSlashOptionPrefix();

// 定出一条绝对路径。**只**在真的定得出时返回 true：语法坏、缓冲区不够、问不出当前目录时
// 返回 false 且不写 \*out 的可用结论，绝不交回一份"看着顺眼的猜测值"当绝对路径。
bool NormalizeAbsolutePath(const std::wstring& in, std::wstring* out);

// 相对写法相对**调用方当前目录**定绝对路径；定不出时原样交回，由对面如实报错。
std::wstring AbsoluteFromCwd(const std::wstring& raw);

// 本平台路径分隔符的字符集（Windows 是 "\\/"，POSIX 只有 "/"）。给"这个点是不是文件名里
// 的那个点"这类切分判据用：在 POSIX 上反斜杠是文件名的合法字符，拿它当分隔符会把
// `/a.b/c\d.png` 这类路径的扩展名判错。
const wchar_t* PathSeparatorSet();

// 把"目录 + 相对写法"拼成一条待绝对化的路径。分隔符由平台决定，不在共用层写死反斜杠。
std::wstring JoinDirAndFile(const std::wstring& dir, const std::wstring& file);

// 把一条路径拆成"所在目录"与"最后一节"。拆不出目录（整条里没有本平台的分隔符）返回 false，
// 调用方据此报"基准问不出来"，不许退回当前目录或源码树。
// 为什么共用这一条：基准点在哪是**本平台**的分隔符说了算的。共用层写死 `\\` 与 `/` 两种都算，
// 在 POSIX 上就把文件名里的一个反斜杠当成分隔符，切出另一个目录。
bool SplitDirAndFile(const std::wstring& path, std::wstring* dir, std::wstring* file);

// 实际运行的程序所在目录。问不出来时返回 false：不猜当前目录、不猜源码树、不猜安装位置。
bool ExeDirectory(std::wstring* out);

}  // namespace ecapture
