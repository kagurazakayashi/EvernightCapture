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

#include <cstdint>
#include <string>
#include <vector>

namespace ecapture {

// ---------------------------------------------------------------------------
// 程序自己的数据位置
// ---------------------------------------------------------------------------
//
// Windows 的既有行为原样保留：配置与历史都在**实际运行的那个 exe 所在目录**。
// Linux 分成两种明确模式，不按"碰巧写得进去"自动切换：
//   * 用户模式（默认）：配置在 $XDG_CONFIG_HOME/evernightcapture/ECAPTURE.ini
//     （未设置时 $HOME/.config/evernightcapture/ECAPTURE.ini），历史在
//     $XDG_STATE_HOME/evernightcapture/history（未设置时
//     $HOME/.local/state/evernightcapture/history）。安装目录通常不可写，用户数据不该放在那里。
//   * 便携模式（明确给出 --portable）：两者都回到 exe 旁，和 Windows 一样。
// 问不出 exe 的真实位置时返回 false。Linux 用户模式下配置基准与状态基准各自解析：一个不可用
// 不会拖累另一个，调用方检查自己需要的 configDir / stateDir 是否为空；两个都不可用时才返回 false。
// 任何情况下都不拿当前目录冒充。
struct AppPaths {
    std::wstring exeDir;      // 实际运行的程序所在目录
    std::wstring configDir;   // ECAPTURE.ini 的默认目录（只读配置就在这一层找）
    std::wstring stateDir;    // 历史等运行期数据的根（下面还有 history/ 与日期层）
    // 首次归档时要按顺序补齐的目录（从最外层到 stateDir）。只包含**应用自己的**状态路径
    // 及它需要的标准父层（Linux 默认的 ~/.local 与 ~/.local/state），不包含配置目录；
    // Windows / 便携模式为空（exe 目录已经存在）。解析本身不创建任何目录。
    std::vector<std::wstring> stateCreateChain;
    bool portable = false;    // true = 显式便携模式（Windows 上位置不变）
};

// portable = true 时两个目录都取 exeDir；Windows 上两种模式的位置相同。
bool ResolveAppPaths(bool portable, AppPaths* out);

// 本平台的临时目录（已绝对化）。Windows = GetTempPathW，Linux = $TMPDIR（须为绝对路径）或 /tmp。
bool TempDirectory(std::wstring* out);

// 本地时间与当前进程号：历史归档的日期目录、文件名与 --out 模板共用同一份来源。
struct LocalDateTime {
    int year = 0;
    int month = 0;   // 1..12
    int day = 0;     // 1..31
    int hour = 0;
    int minute = 0;
    int second = 0;
};

bool LocalDateTimeNow(LocalDateTime* out);
uint32_t CurrentProcessId();

// 单调毫秒时钟（只用于差值）：Windows = QueryPerformanceCounter 换算，Linux = CLOCK_MONOTONIC。
// 取不到读数的极端环境交回 0；调用方按"从某一起点算来"的差值使用，不把它当时刻保存。
int64_t MonotonicClockMs();

// 两条已经绝对化、已展开的路径是不是同一个文件。Windows 不区分大小写（Ordinal），
// POSIX 按字节精确比较 —— 大小写敏感是文件系统规则，不是"看起来像"的猜测。
bool SamePath(const std::wstring& a, const std::wstring& b);

// 把窗口标题 / 设备名变成**本平台合法的一段文件名**。Windows 那一条逐字保留既有规则
//（非法字符、保留设备名、尾部点与空格、代理对边界）；POSIX 只把 '/' 与 NUL 换掉，
// 反斜杠、冒号、问号等都是合法文件名字符，不照搬 Windows 的保留名。
std::wstring SanitizeFileNamePart(const std::wstring& raw);

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
