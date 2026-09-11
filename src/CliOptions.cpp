#include "CliOptions.h"

#include <algorithm>
#include <cwctype>
#include <functional>
#include <iterator>
#include <limits>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ScreenMatch.h"   // 只要那条 inline 的 StripScreenDevicePrefix：设备名前缀的形状只写一处
#include "CursorControl.h"  // 只要 inline 的那张光标登记表：解析期判"这条通道做不做得到"
#include "HdrColor.h"       // 只要 inline 的那张 HDR 登记表：解析期判"这条通道带不带得回广色域帧"

namespace ecapture {
namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

bool EqualsInsensitive(const std::wstring& a, const std::wstring& b) {
    return ToLower(a) == ToLower(b);
}

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::iswspace(s[b])) ++b;
    while (e > b && std::iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool HasPathSeparator(const std::wstring& s) {
    return s.find(L'\\') != std::wstring::npos || s.find(L'/') != std::wstring::npos;
}

// Levenshtein 距离，用于 "你是不是想输入 --title？" 这类提示。
size_t EditDistance(const std::wstring& a, const std::wstring& b, size_t limit) {
    if (a.size() < b.size()) return EditDistance(b, a, limit);
    if (a.size() - b.size() > limit) return limit + 1;
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        size_t rowMin = cur[0];
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t cost = (std::towlower(a[i - 1]) == std::towlower(b[j - 1])) ? 0u : 1u;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            rowMin = std::min(rowMin, cur[j]);
        }
        if (rowMin > limit) return limit + 1;
        prev = cur;
    }
    return prev[b.size()];
}

// ---------------------------------------------------------------------------
// 数值解析
// ---------------------------------------------------------------------------
//
// 旧实现只有一个 ParseNumber，它会自己猜进制：串里只要出现 a-f 就按十六进制解释，于是
// --pid 1e3 变成 483、--quality 1e 变成 30；而它最后交给 wcstoull，那个函数认正负号，
// 于是 --hwnd -1 得到 UINT64_MAX —— 非法值被强转成了合法值。现在按"每个选项对外承诺过什么"
// 分成两套，两套都不再猜：
//
//   * 十进制类（--pid / --index / --monitor 的编号 / --quality / --timeout-ms /
//     --consent-timeout-ms）只认 [0-9]+：不要正负号、不要空白、不要小数点、不要指数写法
//     （1e3）、不要下划线分隔、不要 0x 前缀，也不接受任何非 ASCII 数字。区间在同一次调用里
//     判完，调用点没机会"忘了判上界"。
//   * 句柄（--hwnd）保留文档里那三种写法，但正负号与溢出照样拒绝（见 ParseHandleValue）。

constexpr bool IsAsciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

constexpr bool IsAsciiHexDigit(wchar_t c) {
    return IsAsciiDigit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

constexpr bool IsAsciiHexLetter(wchar_t c) {
    return (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

constexpr int HexDigitValue(wchar_t c) {
    if (IsAsciiDigit(c)) return static_cast<int>(c - L'0');
    return static_cast<int>((c >= L'A' && c <= L'F') ? (c - L'A' + 10) : (c - L'a' + 10));
}

// 累进一位并当场判溢出。不用 wcstoull：它认正负号、按 locale 认数字、越界时要么回绕要么只
// 设 errno（而 errno 在别人调过任何函数之后就不再是那条信息了）。
bool AccumulateDigit(wchar_t c, uint32_t base, uint64_t* acc) {
    const int d = base == 10 ? static_cast<int>(c - L'0') : HexDigitValue(c);
    if (d < 0 || static_cast<uint64_t>(d) >= base) return false;
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
    if (*acc > (kMax - static_cast<uint64_t>(d)) / base) return false;
    *acc = *acc * base + static_cast<uint64_t>(d);
    return true;
}

// 严格的十进制字面量：非空、全 ASCII 数字、装得进 64 位。空白与正负号在这里就已经非法，
// 所以"1 2"、" 1"、"+1"、"-1"、"1e3"、"1_000"、"1.5"、全角数字统统不收。
// 它同时也是 --monitor "下一个参数算不算取值"的判据之一，因此这份语法只写这一处。
bool DecimalLiteral(const std::wstring& raw, uint64_t* out) {
    if (raw.empty()) return false;
    uint64_t acc = 0;
    for (wchar_t c : raw) {
        if (!IsAsciiDigit(c)) return false;
        if (!AccumulateDigit(c, 10, &acc)) return false;
    }
    if (out) *out = acc;
    return true;
}

// 语法与区间一次判完（上下界都由调用点写明，0 是不是合法取值因此永远是显式决定）。
bool ParseDecimal(const std::wstring& raw, uint64_t minInclusive, uint64_t maxInclusive,
                  uint64_t* out) {
    uint64_t v = 0;
    if (!DecimalLiteral(raw, &v)) return false;
    if (v < minInclusive || v > maxInclusive) return false;
    *out = v;
    return true;
}

// --hwnd 的三种写法，逐个定清楚（旧实现靠 wcstoull 猜，正负号与溢出都漏了）：
//   1. 纯 ASCII 数字            => 十进制
//   2. 0x / 0X 前缀             => 十六进制，前缀之后至少有一位十六进制数字
//   3. 裸写而且含 a-f           => 十六进制（Spy++ 那种 001A0B4C，文档一直这么承诺）
// 一律拒绝：正负号、任何空白、小数点、非 ASCII 数字、超过 64 位（溢出就是溢出，不回绕）。
// 下划线只在十六进制那两种写法里合法，且必须夹在两位十六进制数字之间：不打头、不收尾、
// 不连写，也不紧跟在 0x 之后（所以 0x_1A 与 1A__2B 都算写错了）。
bool ParseHandleValue(const std::wstring& raw, uint64_t* out) {
    if (raw.empty()) return false;
    size_t body0 = 0;
    if (raw.size() >= 2 && raw[0] == L'0' && (raw[1] == L'x' || raw[1] == L'X')) body0 = 2;
    const std::wstring body = raw.substr(body0);
    if (body.empty()) return false;  // 光写了个 "0x"
    // 数字部分每个字符都得是十六进制数字或下划线：正负号、空白、小数点、非 ASCII 数字
    // 以及 g-z 这些字母统统在这里拒掉。
    for (wchar_t c : body) {
        if (c == L'_' || IsAsciiHexDigit(c)) continue;
        return false;
    }
    // 进制按写法定，不猜：有 0x 前缀、或者裸写里含 a-f，才是十六进制；纯数字是十进制。
    const bool hexadecimal = body0 != 0 || std::any_of(body.begin(), body.end(), IsAsciiHexLetter);
    const uint32_t base = hexadecimal ? 16u : 10u;
    if (!hexadecimal) {
        // 十进制写法里没有定义过下划线，所以算写错而不是算可读性。
        for (wchar_t c : body) {
            if (!IsAsciiDigit(c)) return false;
        }
    } else if (body.find(L'_') != std::wstring::npos) {
        if (!IsAsciiHexDigit(body.front()) || !IsAsciiHexDigit(body.back())) return false;
        for (size_t k = 1; k < body.size(); ++k) {
            if (!IsAsciiHexDigit(body[k]) && !IsAsciiHexDigit(body[k - 1])) return false;
        }
    }
    uint64_t acc = 0;
    for (wchar_t c : body) {
        if (c == L'_') continue;
        if (!AccumulateDigit(c, base, &acc)) return false;
    }
    *out = acc;
    return true;
}

bool ParseBool(const std::wstring& raw, bool* out) {
    const std::wstring v = ToLower(Trim(raw));
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"y" || v == L"on") { *out = true; return true; }
    if (v == L"0" || v == L"false" || v == L"no" || v == L"n" || v == L"off") { *out = false; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// --roi 的取值：x,y,w,h 四个十进制数，逗号分隔
// ---------------------------------------------------------------------------
//
// 每一段都走 DecimalLiteral 那同一套语法：不认空白、正负号、小数点、指数、下划线、0x 前缀，
// 也不认非 ASCII 数字。所以 "1, 2,3,4" 是**写坏了**，而不是被宽容地读成 1/2/3/4 ——
// 与仓库里每一条数字选项同一条规矩（"这段带个空格"与"这条数写成了十六进制"都不该被静默接受）。
// 四个数各自不超过 kRoiMaxValue（= 帧的单边上限），x/y 允许 0，w/h 至少 1：
// 空矩形在解析期就拒，不留到取帧之后才说"这块什么都没裁到"。
// 段数也要正好四条：三段（少写一条）与五段（多写了逗号）都是这一条错，不猜第第四个是什么。
bool ParseRoiValue(const std::wstring& raw, CropRequest* out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    size_t index = 0;
    size_t pos = 0;
    for (;;) {
        const size_t comma = raw.find(L',', pos);
        const std::wstring field =
            comma == std::wstring::npos ? raw.substr(pos) : raw.substr(pos, comma - pos);
        if (index >= 4) return false;   // 第四个逗号之后还剩东西 = 多写了一段
        uint64_t v = 0;
        // 前两条是偏移（0 合法），后两条是宽高（至少 1）
        if (!ParseDecimal(field, index < 2 ? 0 : 1, cli_limits::kRoiMaxValue, &v)) return false;
        parts[index] = static_cast<uint32_t>(v);
        ++index;
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    if (index != 4) return false;
    out->x = parts[0];
    out->y = parts[1];
    out->width = parts[2];
    out->height = parts[3];
    return true;
}

// ---------------------------------------------------------------------------
// --scale 的取值：key=N 的清单，逗号分隔（max-width / max-height / max-pixels）
// ---------------------------------------------------------------------------
//
// 与 --roi 同一套写法规矩：**不宽容**。键名大小写不敏感（与 --capture 那类取值同一做法），
// 但键名与等号两侧一个空白都不吃；值只认 [0-9]+（用 ParseDecimal，区间一次判完）。
// 一条里可以写多个 key=N；也可以把 --scale 写多次 —— 每条天花板各记各的，重复给同一条时
// 最后一个生效（与仓库里其它取值选项的顺序语义一致，但它们不互相覆盖，因为它们是三件事）。
// 一个键都不认得、缺等号、值为 0 / 非十进制 / 超上限，都算写坏了：整条不生效，不"留下认得的那半"。
bool ParseScaleValue(const std::wstring& raw, ScaleRequest* out) {
    bool any = false;
    size_t pos = 0;
    for (;;) {
        const size_t comma = raw.find(L',', pos);
        const std::wstring token =
            comma == std::wstring::npos ? raw.substr(pos) : raw.substr(pos, comma - pos);
        const size_t eq = token.find(L'=');
        if (eq == std::wstring::npos) return false;   // 缺等号（或空段）= 整条写坏
        const std::wstring key = ToLower(token.substr(0, eq));
        const std::wstring value = token.substr(eq + 1);
        uint64_t v = 0;
        if (key == L"max-width") {
            if (!ParseDecimal(value, 1, cli_limits::kScaleMaxSide, &v)) return false;
            out->maxWidth = static_cast<uint32_t>(v);
            out->hasMaxWidth = true;
        } else if (key == L"max-height") {
            if (!ParseDecimal(value, 1, cli_limits::kScaleMaxSide, &v)) return false;
            out->maxHeight = static_cast<uint32_t>(v);
            out->hasMaxHeight = true;
        } else if (key == L"max-pixels") {
            if (!ParseDecimal(value, 1, cli_limits::kScaleMaxPixels, &v)) return false;
            out->maxPixels = v;
            out->hasMaxPixels = true;
        } else {
            return false;   // 认不得的键名不猜（"是不是想写 max-width"不在这里代替判据）
        }
        any = true;
        if (comma == std::wstring::npos) break;
        pos = comma + 1;
    }
    return any;
}

// ---------------------------------------------------------------------------
// --monitor 的取值语法（取值可省略，所以"要不要吃掉下一个参数"必须与实际解析同源）
// ---------------------------------------------------------------------------

bool MonitorKeyword(const std::wstring& raw) {
    const std::wstring v = ToLower(Trim(raw));
    return v == L"all" || v == L"primary";
}

// "有人想写个数字，只是写坏了"。这一条故意比 DecimalLiteral 宽：宽到能把 1e3 / -1 / 1.5 /
// 全角数字这些写法一并吃掉，交给 Apply 报 cli.invalid_number，而不是让它们悄悄变成输出路径
// 上的文件名（旧实现只判"全是数字"才吃，于是 --monitor 1e3 被读成"没给取值 + 输出叫 1e3"）。
// 判据是"每个字符都还像数字、而且至少有一位数字"：十六进制的 a-f 与 x、指数写法用的 e、
// 小数点、千分位逗号、下划线分隔、正负号都算；一旦冒出 p / v 或路径分隔符这类字符就不像数字
// 了。所以 out.png、2.png、v2、D:\a 照旧是输出路径（--monitor 取值可省略那条约定没动）。
bool LooksLikeNumberAttempt(const std::wstring& raw) {
    const std::wstring v = Trim(raw);
    bool hasDigit = false;
    for (wchar_t c : v) {
        if (c == L'\\' || c == L'/' || c == L':') return false;
        const bool digit = IsAsciiDigit(c) || std::iswdigit(c) != 0;
        hasDigit = hasDigit || digit;
        const bool numberish = digit || IsAsciiHexLetter(c) || c == L'x' || c == L'X' ||
                               c == L'+' || c == L'-' || c == L'_' || c == L'.' || c == L',';
        if (!numberish) return false;
    }
    return hasDigit;
}

// ---------------------------------------------------------------------------
// --monitor 的标识写法：device:<设备名> 与 id:<监视器 devnode 路径>
// ---------------------------------------------------------------------------
//
// 为什么要这一层：在此之前调用方只能拿"本次枚举顺序里的第 n 块屏"来指定目标，而那个 n
// 既不等于「显示设置」里的标识号、也不保证插拔或改分辨率之后还指同一块屏。让调用方（含 AI）
// 去猜一个数字然后拍到另一块屏上，是这条链里最坏的失败形状 —— 它截到的画面没人批准过。
// 于是把 --screens 交回的两种标识原样收回来做选择器，一次选屏不必再靠顺序号。
//
// 前缀只认这两个词（大小写不敏感），而且**只在这两个词之后有冒号时**才算这条选项的取值：
// 冒号在 Windows 里还有别的用处（盘符 `D:\`、备用数据流），把任意 `x:y` 都当成标识会让
// `--monitor D:\shots\a.png` 里的输出路径被吃掉。认不得的前缀照旧不是取值。
// 取值本体只去首尾空白，内部一字不改 —— devnode 路径是要逐字符比对的。
enum class MonitorIdKind { kNone, kDevice, kPath, kUnknownKind };

MonitorIdKind SplitMonitorSelector(const std::wstring& raw, std::wstring* body) {
    const size_t colon = raw.find(L':');
    if (colon == std::wstring::npos) return MonitorIdKind::kNone;
    const std::wstring prefix = ToLower(Trim(raw.substr(0, colon)));
    const std::wstring rest = Trim(raw.substr(colon + 1));
    if (prefix == L"device") {
        if (body) *body = rest;
        return MonitorIdKind::kDevice;
    }
    if (prefix == L"id") {
        if (body) *body = rest;
        return MonitorIdKind::kPath;
    }
    return MonitorIdKind::kUnknownKind;
}

// 裸设备名的前缀是 GDI 的视图设备名那一套（`\\.\DISPLAY1`）。写选择器的人多半从
// --screens 里直接抄那一条，所以这里把前缀去掉，剩下的"DISPLAY1"才是标识本体。
// 去掉前缀这一条与 %n、与比对那一路用的是同一个 inline 函数（ScreenMatch.h），
// 不在三处各写一遍形状。判据只看写法，不看它真对应哪块屏：形状不合就原样交给
// 选屏那一步去判"没匹配上"。

// 取值可省略的选项要靠这个判断"下一个参数是不是我的取值"，否则会把输出路径吃掉。
// 判据 = 关键字 + 合十进制语法的编号 + 写坏了的数字 + 那两个标识前缀，四者都属于"这条选项的取值位"。
bool LooksLikeMonitorValue(const std::wstring& raw) {
    // kUnknownKind（认不得的前缀）这里**不**放行：那是别人的 token（盘符路径之类），
    // 吃掉它就等于替用户改了一个输出路径。内联写法 --monitor=foo:1 才会走到报错那一条。
    const MonitorIdKind kind = SplitMonitorSelector(raw, nullptr);
    return MonitorKeyword(raw) || DecimalLiteral(raw, nullptr) ||
           LooksLikeNumberAttempt(raw) || kind == MonitorIdKind::kDevice ||
           kind == MonitorIdKind::kPath;
}

// --list / --inspect 的取值：各自只有一个关键字，写成内联形式（--list=all）。
// 取值可省略，所以"下一个参数要不要吃"必须与实际解析同源 —— 这两条一律不吃后面的裸参数，
// 只有 =取值 那种内联写法才算给了取值（于是 --list out.png 里的 out.png 仍是位置参数）。
bool ListQueryValue(const std::wstring& raw, bool* flag) {
    const std::wstring v = ToLower(Trim(raw));
    if (v.empty()) return true;             // 省略取值 = 默认策略
    if (v == L"all" || v == L"path") { *flag = true; return true; }
    return false;
}

bool LooksLikeOptionalValue(const std::wstring& name, const std::wstring& raw) {
    if (name == L"monitor") return LooksLikeMonitorValue(raw);
    return false;
}

// --lang 的预扫描与 argv 扫描器写在选项目录之后（它们要按 kOptions 判断"这个选项吃不吃值"），
// 见本文件里 ScanArgv / SelectLanguageFromCommandLine 那一段。

// ---------------------------------------------------------------------------
// 选项目录（CLI 契约的唯一来源：解析、--help 的 JSON 与文本都由它生成）
// ---------------------------------------------------------------------------

constexpr const wchar_t* kFormatValues[] = {
    L"png", L"jpg", L"jpeg", L"bmp", L"tiff", L"gif", nullptr};

// --timeout-ms / --consent-timeout-ms、--index 与 --monitor 的编号、--pid 的上限：
// 数字本体在 CliOptions.h 的 cli_limits 里（--capabilities 报告的 limits 段读的是同一份），
// 这里只是把名字引进来，不在这儿再抄一份数，否则两处迟早打脸。
using cli_limits::kMaxTimeoutMs;
using cli_limits::kMaxOrdinal;
using cli_limits::kMaxPid;
// --offset / --limit 的上限与 --capabilities 那份 limits 同源（数字只在 cli_limits 写一次）。
using cli_limits::kMaxWindowListItems;
constexpr const wchar_t* kCaptureValues[] = {
    L"wgc", L"dwm", L"printwindow", L"bitblt", L"duplication", L"auto", nullptr};

// --cursor 的取值。default 是这条选项的默认值，含义是"本工具对光标这件事一个字都不改"
// （不调任何通道的开关，结果里也不出现那三个键）；判据本体在 src/CursorControl.h。
constexpr const wchar_t* kCursorValues[] = {L"default", L"include", L"exclude", nullptr};

// --hdr 的取值。auto 是这条选项的默认值，含义是"本工具对 HDR 色彩这件事一个字都不改"
// （照 B8G8R8A8 那条路线取帧，结果里也不出现色彩那组键）；判据本体在 src/HdrColor.h。
constexpr const wchar_t* kHdrValues[] = {L"auto", L"tonemap", L"refuse", nullptr};

// --lang 的规范写法。宽容输入（zh_TW / zh-Hant / cht / jp）由 LanguageFromTag 负责，
// 这里只列推荐值，用于 --help 与取值非法时的提示。
constexpr const wchar_t* kLangValues[] = {
    L"auto", L"zh-CN", L"zh-TW", L"en", L"ja", nullptr};

struct OptionSpec {
    const wchar_t* name;        // 规范名（不含前导 -）
    const wchar_t* shortName;   // 单字母别名，可为空
    bool takesValue;            // false = 开关
    const wchar_t* group;       // target / match / pick / output / behavior
    const wchar_t* valueHint;   // 人读的取值占位符，开关为空（ASCII，各语言共用）
    const wchar_t* const* allowed;    // 枚举取值，nullptr 结尾；没有则 nullptr
    const wchar_t* messageKey;  // 说明文案的资源 key，见 resources/strings-*.txt
    bool optionalValue = false; // 取值可省略（--monitor 不给编号 = 主屏）；仅 takesValue 时有意义
    // 名字带 no- 的开关：写出去的字段是"开关取反后的值"，所以 --no-overwrite=true 与裸开关同义，
    // --no-overwrite=false 才是取消禁止覆盖。见解析循环里对 inlineValue 的处理。
    bool inverted = false;
    // 正向布尔开关也要接收 =false 写法（值仍然落到 Apply，重复给出时最后一个生效）。
    // 不加这个标记的开关写 --flag=false 等于没写，那是仓库里既有的普通开关语义。
    bool valueAlways = false;
};

constexpr OptionSpec kOptions[] = {
    // ---- 截图目标 ----
    // valueHint 用方括号表示"取值可省略"：省略时下一个参数不会被吞掉，
    // 所以 `ECAPTURE --monitor out.png` 里的 out.png 仍是输出路径。
    // 两条标识写法（device: / id:）的取值本体可能长达一百多字符，写在 valueHint 里会把
    // 整张选项目录的列宽拉起来（每一行都要跟着补空格），所以这里只列前缀，
    // 本体形状放在 --screens 那一条与 README 里。
    {L"monitor", L"m", true, L"target", L"[<n|primary|all|device:|id:>]", nullptr, L"opt.monitor",
     true},
    // ---- 窗口匹配条件（同类 OR，跨类 AND）----
    {L"hwnd", L"", true, L"match", L"<handle>", nullptr, L"opt.hwnd"},
    {L"pid", L"", true, L"match", L"<pid>", nullptr, L"opt.pid"},
    {L"process", L"p", true, L"match", L"<image-name>", nullptr, L"opt.process"},
    {L"exe", L"", true, L"match", L"<full-path>", nullptr, L"opt.exe"},
    {L"title", L"t", true, L"match", L"<exact-title>", nullptr, L"opt.title"},
    {L"title-contains", L"T", true, L"match", L"<text>", nullptr, L"opt.title-contains"},
    {L"title-regex", L"R", true, L"match", L"<regex>", nullptr, L"opt.title-regex"},
    {L"class", L"c", true, L"match", L"<class-name>", nullptr, L"opt.class"},
    // ---- 匹配到多个窗口时的选择策略（互斥）----
    // 这一组选的都是**当下的 Z 序**位置，与创建时间无关（窗口创建时间没有公开 API 可取）。
    // --newest / --oldest 是旧名字：它们说的是叠放次序、写的却是"新旧"，所以各有一个说清楚
    // 语义的新名字，旧写法继续有效并留一条 note.deprecated_option。
    {L"index", L"i", true, L"pick", L"<n>", nullptr, L"opt.index"},
    {L"topmost-match", L"", false, L"pick", L"", nullptr, L"opt.topmost-match"},
    {L"bottommost-match", L"", false, L"pick", L"", nullptr, L"opt.bottommost-match"},
    {L"newest", L"", false, L"pick", L"", nullptr, L"opt.newest"},
    {L"oldest", L"", false, L"pick", L"", nullptr, L"opt.oldest"},
    {L"all", L"a", false, L"pick", L"", nullptr, L"opt.all"},
    // ---- 取图方式 ----
    {L"capture", L"C", true, L"capture", L"<method>", kCaptureValues, L"opt.capture"},
    // 光标要不要画进画面。取值写在 valueHint 里（这三个字本身就是判据），说明只写行为差别：
    // 判据本体在 src/CursorControl.h 那张按路径登记的表，不在文案里另写一份"哪条通道支持"。
    {L"cursor", L"", true, L"capture", L"<default|include|exclude>", kCursorValues, L"opt.cursor"},
    // HDR 来源怎么处理。判据本体在 src/HdrColor.h 那张按路径登记的表（这条路径交回的帧带不带
    // 得回广色域/高亮范围），取值写在 valueHint 里（那三个词本身就是判据），说明只写行为差别。
    // auto(默认，本工具一个字都不改) / tonemap(把 HDR 映射成 SDR 交付) / refuse(遇到 HDR 就拒绝)。
    {L"hdr", L"", true, L"capture", L"<auto|tonemap|refuse>", kHdrValues, L"opt.hdr"},
    // ---- 窗口内部裁剪（--roi 与 --client-area 互斥）----
    // 坐标系写在说明里（判据本体在 src/CropGeometry.h）：那四个数是**交付的整窗图像自己的像素
    // 坐标**，不是桌面绝对坐标；单位是物理像素，不按窗口所在屏的 DPI 缩放。
    // valueHint 保持 ASCII，不翻译（与其它选项同一做法）；数字上限见 cli_limits::kRoiMaxValue。
    {L"roi", L"", true, L"crop", L"<x,y,w,h>", nullptr, L"opt.roi"},
    {L"client-area", L"", false, L"crop", L"", nullptr, L"opt.client-area"},
    // 等比缩小（--scale）：只缩小、只在超过天花板时动手，绝不放大。取值是 key=N 的清单
    // （max-width / max-height / max-pixels），可以一条里用逗号串、也可以把这一条写多次；
    // 三条天花板互相独立。valueHint 保持 ASCII，不翻译（与其它选项同一做法）；
    // 数字上限见 cli_limits::kScaleMaxSide / kScaleMaxPixels。
    {L"scale", L"", true, L"crop", L"<key=N>", nullptr, L"opt.scale"},
    // ---- 截图授权 ----
    // --yes 只免掉窗口内容路径的确认框；会拍到桌面像素的那几条永远问人（见 src/Consent.h）。
    {L"yes", L"y", false, L"consent", L"", nullptr, L"opt.yes", false, false, true},
    // ---- 期限 ----
    // --timeout-ms 是自动处理阶段的总预算，不是"每一步各得一份"；确认框的等待另算
    // （--consent-timeout-ms，到点按拒绝处理）。见 src/Deadline.h 与 src/Worker.h。
    {L"timeout-ms", L"", true, L"timeout", L"<ms>", nullptr, L"opt.timeout-ms"},
    {L"consent-timeout-ms", L"", true, L"timeout", L"<ms>", nullptr, L"opt.consent-timeout-ms"},
    // ---- 输出 ----
    {L"out", L"o", true, L"output", L"<path|->", nullptr, L"opt.out"},
    {L"format", L"f", true, L"output", L"<name>", kFormatValues, L"opt.format"},
    {L"quality", L"", true, L"output", L"<1-100>", nullptr, L"opt.quality"},
    {L"no-overwrite", L"", false, L"output", L"", nullptr, L"opt.no-overwrite", false, true},
    // ---- 行为 ----
    {L"dry-run", L"d", false, L"behavior", L"", nullptr, L"opt.dry-run"},
    {L"json", L"j", false, L"behavior", L"", nullptr, L"opt.json"},
    {L"verbose", L"v", false, L"behavior", L"", nullptr, L"opt.verbose"},
    {L"quiet", L"q", false, L"behavior", L"", nullptr, L"opt.quiet"},
    {L"lang", L"l", true, L"behavior", L"<language>", kLangValues, L"opt.lang"},
    {L"help", L"h", false, L"behavior", L"", nullptr, L"opt.help"},
    {L"version", L"", false, L"behavior", L"", nullptr, L"opt.version"},
    // ---- 只读查询 ----
    // 这两条既不截图也不弹框，也不写文件，问的是"这台机器现在能让哪几条路线走"。
    // 与截图意图那一整套选项互斥（cli.query_conflict）：它们对流与输出的约定都不一样，
    // 一起给出时替用户挑一个执行，不如把这条用法当场说清楚。见 src/EnvReport.h。
    {L"capabilities", L"", false, L"query", L"", nullptr, L"opt.capabilities"},
    {L"diagnostics", L"", false, L"query", L"", nullptr, L"opt.diagnostics"},
    // 只读的屏幕枚举：本机现在有哪几块屏、每块屏的三种身份各稳在哪一层。
    // 与上面两条同一家族（不取像素、不弹框、不写文件），所以冲突与允许清单也走同一条判据。
    // 判据与渲染在 src/ScreenQuery.h，屏幕身份的问答在 src/ScreenIdentity.h。
    {L"screens", L"", false, L"query", L"", nullptr, L"opt.screens"},
    // ---- 只读的窗口发现与检查 ----
    // 与上面两条查询同样不截图、不弹框、不写文件，但走的是与截图同源的条件求值：
    // --list 把命中的窗口列成机器可读的列表（多匹配不是截图歧义），
    // --inspect 把一个明确选择器对应的那扇窗口逐项查清楚（多匹配照实报歧义）。
    // 判据与渲染在 src/WindowQuery.h；真机问答在 src/WindowQueryRun.cpp。
    // 取值可省略（与 --monitor 同一条规矩）：省略时不吃后面的参数，所以 `--list out.png`
    // 里的 out.png 仍是输出路径（在这一路算冲突），而 `--list=all` 才是"把最小化也列进来"。
    {L"list", L"", true, L"query", L"[<all>]", nullptr, L"opt.list", true},
    {L"inspect", L"", true, L"query", L"[<path>]", nullptr, L"opt.inspect", true},
    {L"offset", L"", true, L"query", L"<n>", nullptr, L"opt.offset"},
    {L"limit", L"", true, L"query", L"<n>", nullptr, L"opt.limit"},
};

std::vector<OptionInfo> BuildCatalog() {
    std::vector<OptionInfo> catalog;
    catalog.reserve(std::size(kOptions));
    for (const auto& spec : kOptions) {
        OptionInfo info;
        info.name = spec.name;
        info.shortName = spec.shortName;
        info.takesValue = spec.takesValue;
        info.group = spec.group;
        info.valueHint = spec.valueHint;
        info.messageKey = spec.messageKey;
        for (const wchar_t* const* p = spec.allowed; p && *p; ++p) info.allowedValues.emplace_back(*p);
        catalog.push_back(std::move(info));
    }
    return catalog;
}

const OptionSpec* FindOption(const std::wstring& name) {
    for (const auto& spec : kOptions) {
        if (EqualsInsensitive(name, spec.name)) return &spec;
    }
    return nullptr;
}

const OptionSpec* FindShortOption(const std::wstring& name) {
    if (name.size() != 1) return nullptr;
    for (const auto& spec : kOptions) {
        if (spec.shortName[0] && name[0] == spec.shortName[0]) return &spec;
    }
    return nullptr;
}

std::optional<std::wstring> ClosestOption(const std::wstring& name) {
    if (name.size() < 2) return std::nullopt;
    const OptionSpec* best = nullptr;
    size_t bestDistance = 3;  // 只提示距离 <= 2 的
    for (const auto& spec : kOptions) {
        const size_t d = EditDistance(name, spec.name, bestDistance);
        if (d < bestDistance) { bestDistance = d; best = &spec; }
    }
    if (!best) return std::nullopt;
    return std::wstring(L"--") + best->name;
}

std::optional<ImageFormat> ParseFormat(const std::wstring& raw) {
    const std::wstring v = ToLower(Trim(raw));
    if (v == L"png") return ImageFormat::kPng;
    if (v == L"jpg" || v == L"jpeg") return ImageFormat::kJpeg;
    if (v == L"bmp") return ImageFormat::kBmp;
    if (v == L"tif" || v == L"tiff") return ImageFormat::kTiff;
    if (v == L"gif") return ImageFormat::kGif;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// argv 扫描器：token 消费规则只有一份
// ---------------------------------------------------------------------------
//
// 语言必须在任何一条诊断产生之前就定下来（解析期的报错文案本身要用调用方指定的语言），
// 所以在这段完整解析之前还得先扫一遍 argv 找 --lang。问题是"再扫一遍"很容易写成第二份
// 规则漂移的解析器 —— 旧实现正是这样：它按"以 - 或 / 开头"认选项，于是
// `--title --lang ja` 里被 --title 吃掉的那个 "--lang" 被它当成了语言开关，
// 而正式解析根本没把它当选项。现在两趟走同一个 ScanArgv，消费规则不可能分叉。
struct ScannedOption {
    const OptionSpec* spec = nullptr;
    std::wstring value;         // 实际取值：inline 写法，或吃掉的下一个参数；省略时为空
    bool hasValue = false;      // 用户是否写出过取值（--flag= 这种空取值也算写过）
    bool missingValue = false;  // 该选项要吃值，但 argv 已经到头了
};

struct ScannedItem {
    enum class Kind { kOption, kPositional, kUnknownOption };
    Kind kind = Kind::kPositional;
    ScannedOption option;    // kOption
    std::wstring name;       // kUnknownOption：去掉前导 - 与 =取值 的那个名字（给"你是不是想输入"用）
    std::wstring token;      // kPositional / kUnknownOption：原始那一条参数
};

// 逐条吐出 argv 里的条目。规矩与正式解析一直相同的那一套：
//   * 裸 -- 之后的所有参数都是位置参数（-- 本身丢掉，不进位置参数）
//   * 吃值的选项把下一个参数消费掉，哪怕它长得像另一个选项（--title --lang 就是标题为 "--lang"）
//   * 取值可省略的选项（--monitor）只在下一个参数合它那套取值语法时才吃它
//   * -vq 这种布尔开关簇展开成多条 kOption
// sink 会被调用任意次；返回值只是"扫完了"。
void ScanArgv(int argc, wchar_t* const* argv,
              const std::function<void(const ScannedItem&)>& sink) {
    auto positional = [&](std::wstring tok) {
        ScannedItem it;
        it.kind = ScannedItem::Kind::kPositional;
        it.token = std::move(tok);
        sink(it);
    };
    bool stopParsing = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (stopParsing || arg.empty()) { positional(arg); continue; }
        if (arg == L"--") { stopParsing = true; continue; }

        std::wstring body;
        bool longForm = false;
        if (StartsWith(arg, L"--")) { body = arg.substr(2); longForm = true; }
        else if (arg[0] == L'-' || arg[0] == L'/') { body = arg.substr(1); }
        else { positional(arg); continue; }
        if (body.empty()) { positional(arg); continue; }

        std::optional<std::wstring> inlineValue;
        std::wstring name = body;
        if (const size_t eq = body.find(L'='); eq != std::wstring::npos) {
            name = body.substr(0, eq);
            inlineValue = body.substr(eq + 1);
        }

        const OptionSpec* spec = FindOption(name);
        if (!spec && !longForm) spec = FindShortOption(name);

        auto emitOption = [&](const OptionSpec& s, std::wstring value, bool hasValue,
                              bool missingValue = false) {
            ScannedItem it;
            it.kind = ScannedItem::Kind::kOption;
            it.option = ScannedOption{&s, std::move(value), hasValue, missingValue};
            sink(it);
        };

        // -abc 形式的布尔开关组合
        if (!spec && !longForm && !inlineValue && name.size() > 1) {
            std::vector<const OptionSpec*> cluster;
            bool allBool = true;
            for (wchar_t ch : name) {
                const std::wstring one(1, ch);
                const OptionSpec* s = FindShortOption(one);
                if (!s || s->takesValue) { allBool = false; break; }
                cluster.push_back(s);
            }
            if (allBool && !cluster.empty()) {
                for (const OptionSpec* s : cluster) emitOption(*s, L"", false);
                continue;
            }
        }

        if (!spec) {
            // 像路径（含 \ 或 /）就当位置参数，其余按误写的选项处理
            if (!longForm && HasPathSeparator(arg)) { positional(arg); continue; }
            ScannedItem it;
            it.kind = ScannedItem::Kind::kUnknownOption;
            it.name = name;
            it.token = arg;
            sink(it);
            continue;
        }

        if (!spec->takesValue) {
            emitOption(*spec, inlineValue ? *inlineValue : std::wstring(), inlineValue.has_value());
            continue;
        }
        if (inlineValue) {
            emitOption(*spec, *inlineValue, true);
            continue;
        }
        if (spec->optionalValue) {
            // 取值可省略：只在下一个参数明显就是本选项的取值时才吃掉它，否则当开关用
            // （--monitor = 主屏），剩下的照常按位置参数处理。判据与实际解析同源。
            std::wstring next;
            if (i + 1 < argc) next = Trim(argv[i + 1]);
            if (LooksLikeOptionalValue(std::wstring(spec->name), next)) {
                emitOption(*spec, argv[++i], true);
            } else {
                emitOption(*spec, L"", false);
            }
            continue;
        }
        if (i + 1 < argc) {
            emitOption(*spec, argv[++i], true);
            continue;
        }
        emitOption(*spec, L"", false, true);
    }
}

// --lang 取值的语义，预扫描与正式解析共用一份：
//   kReset —— 没写取值、或写了 auto：明确回到系统显示语言（不是"保持上一次的语言"）
//   kSet   —— 认得的标签，*out 已填
//   kInvalid —— 不认。正式解析要按当前已生效的语言报 cli.unknown_language；预扫描按"这次没指定"跳过
enum class LangToken { kReset, kSet, kInvalid };

LangToken ResolveLangToken(const std::wstring& raw, Language systemLang, Language* out) {
    const std::wstring v = Trim(raw);
    if (v.empty() || EqualsInsensitive(v, L"auto")) {
        *out = systemLang;
        return LangToken::kReset;
    }
    if (const auto lang = LanguageFromTag(v)) {
        *out = *lang;
        return LangToken::kSet;
    }
    return LangToken::kInvalid;
}

// 把本次命令里的 --lang 全部过一遍，定下最终语言。重复给出以最后一个**有效**的指定为准
// （--lang ja --lang auto 回到系统语言，--lang ja --lang bogus 停在 ja 并在正式解析报错）。
void SelectLanguageFromCommandLine(int argc, wchar_t* const* argv) {
    const Language systemLang = DetectSystemLanguage();
    Language resolved = systemLang;
    ScanArgv(argc, argv, [&](const ScannedItem& it) {
        if (it.kind != ScannedItem::Kind::kOption || !it.option.spec) return;
        if (!EqualsInsensitive(it.option.spec->name, L"lang")) return;
        Language next = resolved;
        const LangToken token = ResolveLangToken(it.option.value, systemLang, &next);
        if (token != LangToken::kInvalid) resolved = next;
    });
    SetLanguage(resolved);
}

}  // namespace

// 以下为 ecapture 命名空间的公开辅助函数（供 main.cpp 复用）
const wchar_t* FormatName(ImageFormat f) {
    switch (f) {
        case ImageFormat::kPng: return L"png";
        case ImageFormat::kJpeg: return L"jpeg";
        case ImageFormat::kBmp: return L"bmp";
        case ImageFormat::kTiff: return L"tiff";
        case ImageFormat::kGif: return L"gif";
    }
    return L"?";
}

const wchar_t* MultiKey(MultiMatch m) {
    switch (m) {
        case MultiMatch::kAsk: return L"ask";
        case MultiMatch::kIndex: return L"index";
        case MultiMatch::kTopmost: return L"topmost";
        case MultiMatch::kBottommost: return L"bottommost";
        case MultiMatch::kAll: return L"all";
    }
    return L"?";
}

const wchar_t* CaptureMethodName(CaptureMethod m) {
    switch (m) {
        case CaptureMethod::kWgc: return L"wgc";
        case CaptureMethod::kDwmThumbnail: return L"dwm";
        case CaptureMethod::kPrintWindow: return L"printwindow";
        case CaptureMethod::kBitBlt: return L"bitblt";
        case CaptureMethod::kDuplication: return L"duplication";
        case CaptureMethod::kAuto: return L"auto";
    }
    return L"?";
}

const wchar_t* CropModeName(CropMode m) {
    switch (m) {
        case CropMode::kNone: return L"none";
        case CropMode::kRoi: return L"roi";
        case CropMode::kClientArea: return L"client-area";
    }
    return L"?";
}

const wchar_t* CursorModeName(CursorMode m) {
    switch (m) {
        case CursorMode::kDefault: return L"default";
        case CursorMode::kInclude: return L"include";
        case CursorMode::kExclude: return L"exclude";
    }
    return L"?";
}

const wchar_t* HdrPolicyName(HdrPolicy p) {
    switch (p) {
        case HdrPolicy::kAuto: return L"auto";
        case HdrPolicy::kToneMap: return L"tonemap";
        case HdrPolicy::kRefuse: return L"refuse";
    }
    return L"?";
}

bool MatchOptions::IsEmpty() const {
    return hwnds.empty() && pids.empty() && processes.empty() && exePaths.empty() &&
           titles.empty() && titleContains.empty() && titleRegexes.empty() && classes.empty();
}

std::size_t MatchOptions::CountGroups() const {
    std::size_t n = 0;
    if (!hwnds.empty()) ++n;
    if (!pids.empty()) ++n;
    if (!processes.empty()) ++n;
    if (!exePaths.empty()) ++n;
    if (!titles.empty()) ++n;
    if (!titleContains.empty()) ++n;
    if (!titleRegexes.empty()) ++n;
    if (!classes.empty()) ++n;
    return n;
}

std::size_t MatchOptions::CountValues() const {
    return hwnds.size() + pids.size() + processes.size() + exePaths.size() + titles.size() +
           titleContains.size() + titleRegexes.size() + classes.size();
}

namespace {

template <typename T>
std::wstring ToDisplay(const T& value) { return std::to_wstring(value); }
inline std::wstring ToDisplay(const std::wstring& value) { return value; }

// 向列表追加值；同一选项内的重复值没有意义，忽略并记一条 note。
template <typename T>
void PushUnique(std::vector<T>* list, T value, const wchar_t* label,
                std::vector<Diagnostic>* notes) {
    if (std::find(list->begin(), list->end(), value) != list->end()) {
        notes->push_back(Diagnostic{codes::kDuplicateValue,
                                    Msgf(L"note.duplicate_value", std::wstring(label)), label,
                                    ToDisplay(value), L""});
        return;
    }
    list->push_back(std::move(value));
}

// 从输出文件名推断扩展名（小写，不含点）。
std::wstring ExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return ToLower(path.substr(dot + 1));
}

std::optional<ImageFormat> FormatFromExtension(const std::wstring& ext) {
    if (ext == L"png") return ImageFormat::kPng;
    if (ext == L"jpg" || ext == L"jpeg") return ImageFormat::kJpeg;
    if (ext == L"bmp") return ImageFormat::kBmp;
    if (ext == L"tif" || ext == L"tiff") return ImageFormat::kTiff;
    if (ext == L"gif") return ImageFormat::kGif;
    return std::nullopt;
}

bool ContainsPlaceholder(const std::wstring& path) {
    return path.find(L'%') != std::wstring::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// 解析主流程
// ---------------------------------------------------------------------------
ParseResult ParseCommandLine(int argc, wchar_t* const* argv) {
    ParseResult result;
    Options& opt = result.options;

    // 语言要在任何一条诊断产生之前就定下来
    SelectLanguageFromCommandLine(argc, argv);

    // 结构化诊断：code 是给机器读的稳定标识，message 是给人读的中文补充
    auto Err = [&](const wchar_t* code, std::wstring msg, std::wstring option = std::wstring(),
                   std::wstring value = std::wstring(), std::wstring hint = std::wstring()) {
        result.errors.push_back(
            Diagnostic{code, std::move(msg), std::move(option), std::move(value), std::move(hint)});
    };
    auto Note = [&](const wchar_t* code, std::wstring msg, std::wstring option = std::wstring(),
                    std::wstring value = std::wstring(), std::wstring hint = std::wstring()) {
        result.warnings.push_back(
            Diagnostic{code, std::move(msg), std::move(option), std::move(value), std::move(hint)});
    };
    auto& warnings = result.warnings;  // PushUnique 直接用它记 note

    std::vector<std::wstring> positional;
    bool helpFlag = false;
    bool versionFlag = false;
    bool formatExplicit = false;
    bool qualityExplicit = false;
    bool outExplicit = false;
    // 两条窗口查询是否**都**被写出过。opt.windowAction 只留最后一个写法（与其它取值选项的顺序
    // 语义一致），而互斥判定要看用户到底写了哪几条，所以各记一个给出标记。
    bool listGiven = false;
    bool inspectGiven = false;
    // 用过的选择策略写法：按**策略**去重，而不是按用户敲的那个名字 ——
    // --newest 与 --topmost-match 是同一条策略的两种写法，同时给出不是冲突。
    struct PickUsage {
        MultiMatch strategy;
        std::wstring display;   // 用户实际写的那一个（互斥报错时要能对上他打了什么）
    };
    std::vector<PickUsage> picks;
    MultiMatch multiFlag = MultiMatch::kAsk;
    bool newestAliasUsed = false;   // --newest / --oldest：旧名字，各留一条废弃 note
    bool oldestAliasUsed = false;

    // ---- 查询命令的互斥收集 ----
    // 两套只读查询各有各的允许清单，所以冲突也各收一份，扫完之后按「这一次到底是哪一类查询」
    // 取对应那一份报出来（与选择策略互斥同一做法：一次把用户写的所有冲突项列全，不修一个报一个）：
    //   * 环境查询（--capabilities / --diagnostics / --screens）只接受 --lang / -v / -q ——
    //     它们连目标都不问，问的都是"这台机器现在是什么样子"。
    //   * 窗口查询（--list / --inspect）问的正是「哪些窗口命中这批条件」，所以窗口条件、
    //     --monitor 与 --timeout-ms 都在允许之列；而截图那一级的选项（输出路径 / 格式 / 覆盖 /
    //     --capture / --dry-run / 确认框期限）一条都不成立：它不出图，也就没有「截哪一扇、
    //     截来放哪」这回事。--yes 同样不成立但**不算错**（见下面那一段）。
    //   * 选择策略那一组要分开看：--inspect 需要的正是「多匹配里定哪一扇」，判据与截图同一条线，
    //     所以 --index / --topmost-match / --bottommost-match（含旧别名）对它有效；
    //     --list 本来就是「全部命中的分页」，再问「取哪一扇」说不通，--all 与 --inspect 也说不通
    //     （一份快照只能对应一扇窗口）。这两条在扫完整条 argv、看清是哪个入口之后才判。
    // 位置参数（输出路径）在两类里都算冲突，单独在扫完之后补进清单；报错里写 --out
    // 而不回显用户那条路径本身 —— 查询这一路的规矩是不把路径带进输出。
    std::vector<std::wstring> envConflicts;
    std::vector<std::wstring> windowConflicts;
    // 用户写过的选择策略名字：窗口查询那一条要在扫完之后按实际入口分别处置，所以先记下来。
    std::vector<std::wstring> pickFlags;
    const auto AllowedWithEnvQuery = [](const std::wstring& name) {
        return name == L"lang" || name == L"verbose" || name == L"quiet" ||
               name == L"capabilities" || name == L"diagnostics" || name == L"screens";
    };
    const auto AllowedWithWindowQuery = [](const std::wstring& name) {
        // 条件求值那一条线上的东西（与截图用的是同一套匹配语义）
        if (name == L"hwnd" || name == L"pid" || name == L"process" || name == L"exe" ||
            name == L"title" || name == L"title-contains" || name == L"title-regex" ||
            name == L"class" || name == L"monitor" || name == L"timeout-ms") {
            return true;
        }
        // 查询本身与它的取舍开关（取舍写在 --list / --inspect 的取值里）
        if (name == L"list" || name == L"inspect" || name == L"offset" || name == L"limit") {
            return true;
        }
        // 选择策略那一组：对 --inspect 有效、对 --list 无效 —— 这里先放行，
        // 由扫完之后的那一段按实际入口处置（pickFlags 记的就是用户写过的那些名字）。
        if (name == L"index" || name == L"topmost-match" || name == L"bottommost-match" ||
            name == L"newest" || name == L"oldest") {
            return true;
        }
        // 文案与详略：窗口查询的 message / hint 也随 --lang 变，标题本身更是原样交付
        if (name == L"lang" || name == L"verbose" || name == L"quiet") return true;
        // --yes 是截图授权那一级的事（只免掉窗口内容路径的确认框），对一次不取像素的查询
        // 没有任何作用，所以写了它不算用法错 —— 但结果必须一模一样，包括"哪些字段读得到"。
        // 判这条的地方是 authorization.yesAffectsResult: false 与 tests\windows.ps1 第 1 节。
        return name == L"yes";
    };
    const auto NoteConflict = [&](std::vector<std::wstring>* into, const std::wstring& flag) {
        if (std::find(into->begin(), into->end(), flag) == into->end()) into->push_back(flag);
    };

    // 单个选项 -> 数据结构。取值型选项的 value 是用户给的原文；开关的 value 是布尔写法的规范化结果：
    // 裸开关 = 空串，--flag=true/1/yes/y/on = "1"，=false/0/no/n/off = "0"（普通开关写 =false 时压根不到这里）。
    // 目前只有负向开关 --no-overwrite 会看这个值。
    auto Apply = [&](const OptionSpec& spec, const std::wstring& value) {
        const std::wstring name = spec.name;

        // ---- 截图目标 ----
        if (name == L"monitor") {
            opt.monitor.given = true;
            opt.monitor.kind = MonitorSelector::Kind::kPrimary;
            opt.monitor.ordinal = 0;
            opt.monitor.id.clear();
            opt.monitor.written = Trim(value);
            if (value.empty()) return;                                    // 省略取值 = 主屏
            if (MonitorKeyword(value)) {
                if (ToLower(Trim(value)) == L"all") opt.monitor.kind = MonitorSelector::Kind::kAll;
                return;                                               // primary => 保持 kPrimary
            }
            // 两条标识写法：把 --screens 交回的标识原样收回来。这里只判**形状**
            // （前缀认得、取值非空），"这台机器上有没有这么一块屏"由选屏那一步判 ——
            // 在那儿判不了主屏兜底，因为兜底就等于替用户挑了一块他没点名的屏。
            std::wstring body;
            const MonitorIdKind idKind = SplitMonitorSelector(value, &body);
            if (idKind == MonitorIdKind::kDevice || idKind == MonitorIdKind::kPath) {
                if (body.empty()) {
                    Err(codes::kMonitorSelectorEmpty, Msg(L"cli.monitor_selector_empty"),
                        L"--monitor", value, Msg(L"cli.monitor_selector_empty_hint"));
                    opt.monitor.given = false;
                    return;
                }
                opt.monitor.kind = idKind == MonitorIdKind::kDevice ? MonitorSelector::Kind::kDevice
                                                                    : MonitorSelector::Kind::kPath;
                opt.monitor.id = idKind == MonitorIdKind::kDevice
                                     ? StripScreenDevicePrefix(body)
                                     : body;
                return;
            }
            // 内联写法里出现冒号而前缀不是那两条之一（`--monitor=foo:1`）：这多半是想写标识
            // 而写错了前缀。照实报一条说清楚有哪两种前缀，别退化成 cli.invalid_number ——
            // 那句"屏幕编号只认十进制"会把人引向"那我换个数字试试"，而他本来要的就是标识。
            if (value.find(L':') != std::wstring::npos) {
                Err(codes::kMonitorSelectorKind, Msg(L"cli.monitor_selector_kind"), L"--monitor",
                    value, Msg(L"cli.monitor_selector_kind_hint"));
                opt.monitor.given = false;
                return;
            }
            // 编号只认严格十进制，且从 1 起。ScanArgv 里"要不要吃下一个参数"用的就是
            // 这套语法（见 LooksLikeMonitorValue），所以像 1e3 这种写坏了的数字会走到这里
            // 报错，而不会被悄悄当成输出文件名。
            uint64_t n = 0;
            if (!ParseDecimal(value, 1, kMaxOrdinal, &n)) {
                Err(codes::kInvalidNumber, Msg(L"cli.monitor_value"), L"--monitor", value,
                    Msg(L"cli.monitor_value_hint"));
                opt.monitor.given = false;
                return;
            }
            opt.monitor.kind = MonitorSelector::Kind::kOrdinal;
            opt.monitor.ordinal = static_cast<uint32_t>(n);
            return;
        }

        // ---- 匹配条件 ----
        if (name == L"hwnd") {
            uint64_t hwnd = 0;
            if (!ParseHandleValue(value, &hwnd) || hwnd == 0) {
                Err(codes::kInvalidNumber, Msg(L"cli.hwnd_value"), L"--hwnd", value,
                    Msg(L"cli.hwnd_hint"));
                return;
            }
            PushUnique(&opt.match.hwnds, hwnd, L"--hwnd", &warnings);
            return;
        }
        if (name == L"pid") {
            uint64_t pid = 0;
            if (!ParseDecimal(value, 1, kMaxPid, &pid)) {
                Err(codes::kInvalidNumber, Msg(L"cli.pid_value"), L"--pid", value,
                    Msg(L"cli.decimal_hint"));
                return;
            }
            PushUnique(&opt.match.pids, static_cast<uint32_t>(pid), L"--pid", &warnings);
            return;
        }
        if (name == L"process") {
            const std::wstring v = Trim(value);
            if (v.empty()) { Err(codes::kInvalidValue, Msg(L"cli.process_empty"), L"--process", value); return; }
            if (HasPathSeparator(v)) {
                Err(codes::kInvalidValue, Msg(L"cli.process_path"), L"--process", v,
                    Msg(L"cli.process_path_hint"));
                return;
            }
            PushUnique(&opt.match.processes, v, L"--process", &warnings);
            if (v.find(L'.') == std::wstring::npos)
                Note(codes::kExtensionAppended, Msg(L"note.extension_appended"), L"--process",
                     v + L".exe");
            return;
        }
        if (name == L"exe") {
            const std::wstring v = Trim(value);
            if (v.empty()) { Err(codes::kInvalidValue, Msg(L"cli.exe_empty"), L"--exe", value); return; }
            if (!HasPathSeparator(v))
                Note(codes::kExeLooksLikeName, Msg(L"cli.exe_no_path"), L"--exe", v);
            PushUnique(&opt.match.exePaths, v, L"--exe", &warnings);
            return;
        }
        if (name == L"title") { PushUnique(&opt.match.titles, value, L"--title", &warnings); return; }
        if (name == L"title-contains") {
            PushUnique(&opt.match.titleContains, value, L"--title-contains", &warnings);
            return;
        }
        if (name == L"title-regex") {
            // 解析层只记原文与判重，**不构造正则**：旧实现在这里（父进程、任何 Deadline 建立
            // 之前）完整编译一遍来"验语法"，而那一步既没有期限也没有隔离——--timeout-ms 与
            // 内置隔离上限都管不到它。语法由受约束的匹配执行层在辅助进程里编译时判
            //（CompileConditions，见 WindowMatch.h）：语法不合照实回 cli.invalid_regex、
            // 退出码仍是 1，只是 stage 变成 match，且一个像素都不取、不弹框。
            PushUnique(&opt.match.titleRegexes, value, L"--title-regex", &warnings);
            return;
        }
        if (name == L"class") {
            const std::wstring v = Trim(value);
            if (v.empty()) { Err(codes::kInvalidValue, Msg(L"cli.class_empty"), L"--class", value); return; }
            PushUnique(&opt.match.classes, v, L"--class", &warnings);
            return;
        }

        // ---- 多窗口选择策略 ----
        // 记进 picks 的是"用户实际写的那个名字"，而互斥判定按**策略**去重：
        // --newest 与 --topmost-match 说的是同一件事，同时给出不算两个互斥策略。
        auto Pick = [&](MultiMatch strategy, const std::wstring& display) {
            multiFlag = strategy;
            picks.push_back(PickUsage{strategy, display});
        };
        if (name == L"index") {
            uint64_t n = 0;
            if (!ParseDecimal(value, 1, kMaxOrdinal, &n)) {
                Err(codes::kInvalidNumber, Msg(L"cli.index_value"), L"--index", value,
                    Msg(L"cli.decimal_hint"));
                return;
            }
            opt.index = static_cast<int>(n);
            Pick(MultiMatch::kIndex, L"--index");
            return;
        }
        if (name == L"topmost-match") { Pick(MultiMatch::kTopmost, L"--topmost-match"); return; }
        if (name == L"bottommost-match") { Pick(MultiMatch::kBottommost, L"--bottommost-match"); return; }
        if (name == L"newest") { newestAliasUsed = true; Pick(MultiMatch::kTopmost, L"--newest"); return; }
        if (name == L"oldest") { oldestAliasUsed = true; Pick(MultiMatch::kBottommost, L"--oldest"); return; }
        if (name == L"all") { Pick(MultiMatch::kAll, L"--all"); return; }

        // ---- 取图方式 ----
        if (name == L"capture") {
            const std::wstring v = ToLower(Trim(value));
            const wchar_t* const* allowed = kCaptureValues;
            bool known = false;
            for (; *allowed; ++allowed) {
                if (v == *allowed) { known = true; break; }
            }
            if (!known) {
                std::wstring list;
                for (const wchar_t* const* p = kCaptureValues; *p; ++p) {
                    if (p != kCaptureValues) list += L", ";
                    list += *p;
                }
                Err(codes::kUnknownCaptureMethod, Msg(L"cli.unknown_capture_method"), L"--capture", value, list);
                return;
            }
            if (v == L"wgc") opt.capture = CaptureMethod::kWgc;
            else if (v == L"dwm") opt.capture = CaptureMethod::kDwmThumbnail;
            else if (v == L"printwindow") opt.capture = CaptureMethod::kPrintWindow;
            else if (v == L"bitblt") opt.capture = CaptureMethod::kBitBlt;
            else if (v == L"duplication") opt.capture = CaptureMethod::kDuplication;
            else opt.capture = CaptureMethod::kAuto;  // 取值已在上面按 kCaptureValues 校验过
            opt.captureExplicit = true;
            return;
        }

        // ---- 光标 ----
        // 取值只认那三个词（忽略大小写与首尾空白，与 --capture / --format 同一套写法）。
        // 认不了就整条不认，不退化成 default：那等于把一次写错的"要不要光标"变成一个没人要的决定。
        // 重复给出时最后一个生效（与其它取值选项的顺序语义一致），而 written 留的是**最后一个有效**
        // 的那个原样写法，报错与 -v 回显都按它。
        if (name == L"cursor") {
            const std::wstring v = ToLower(Trim(value));
            CursorMode mode = CursorMode::kDefault;
            bool known = false;
            if (v == L"default") { mode = CursorMode::kDefault; known = true; }
            else if (v == L"include") { mode = CursorMode::kInclude; known = true; }
            else if (v == L"exclude") { mode = CursorMode::kExclude; known = true; }
            if (!known) {
                std::wstring list;
                for (const wchar_t* const* p = kCursorValues; *p; ++p) {
                    if (p != kCursorValues) list += L", ";
                    list += *p;
                }
                Err(codes::kInvalidValue, Msg(L"cli.cursor_value"), L"--cursor", value, list);
                return;
            }
            opt.cursor.mode = mode;
            opt.cursor.given = true;
            return;
        }

        // ---- HDR 色彩处理 ----
        // 取值只认那三个词（忽略大小写与首尾空白，与 --capture / --cursor 同一套写法）。
        // 认不了就整条不认，不退化成 auto：那等于把一次写错的 HDR 要求变成一个没人要的决定。
        // 重复给出时最后一个生效（与其它取值选项的顺序语义一致）。
        if (name == L"hdr") {
            const std::wstring v = ToLower(Trim(value));
            HdrPolicy policy = HdrPolicy::kAuto;
            bool known = false;
            if (v == L"auto") { policy = HdrPolicy::kAuto; known = true; }
            else if (v == L"tonemap") { policy = HdrPolicy::kToneMap; known = true; }
            else if (v == L"refuse") { policy = HdrPolicy::kRefuse; known = true; }
            if (!known) {
                std::wstring list;
                for (const wchar_t* const* p = kHdrValues; *p; ++p) {
                    if (p != kHdrValues) list += L", ";
                    list += *p;
                }
                Err(codes::kInvalidValue, Msg(L"cli.hdr_value"), L"--hdr", value, list);
                return;
            }
            opt.hdr.policy = policy;
            opt.hdr.given = true;
            return;
        }

        // ---- 窗口内部裁剪 ----
        // 两条互斥，但**先各自落到结构体上**再由扫完之后的那一段报冲突：报错要把用户实际写的
        // 那两个名字一起列出来，而 mode 只留最后一个写法（与其它取值选项的顺序语义一致）。
        if (name == L"roi") {
            CropRequest parsed;
            if (!ParseRoiValue(value, &parsed)) {
                Err(codes::kInvalidValue, Msg(L"cli.roi_value"), L"--roi", value,
                    Msg(L"cli.roi_value_hint"));
                return;
            }
            opt.crop.x = parsed.x;
            opt.crop.y = parsed.y;
            opt.crop.width = parsed.width;
            opt.crop.height = parsed.height;
            opt.crop.mode = CropMode::kRoi;
            opt.crop.roiGiven = true;
            opt.crop.written = value;
            return;
        }
        if (name == L"client-area") {
            opt.crop.mode = CropMode::kClientArea;
            opt.crop.clientAreaGiven = true;
            return;
        }

        // ---- 等比缩小 ----
        // 与裁剪那两条各说各的（一个说留哪一块、一个说交多大），所以直接合到同一个请求上：
        // 这一条写多次也只是把三条天花板各记各的，不构成一次"重复给值"。写坏时整条不生效。
        if (name == L"scale") {
            ScaleRequest parsed;
            if (!ParseScaleValue(value, &parsed)) {
                Err(codes::kInvalidValue, Msg(L"cli.scale_value"), L"--scale", value,
                    Msg(L"cli.scale_value_hint"));
                return;
            }
            opt.scale.given = true;
            if (parsed.hasMaxWidth) {
                opt.scale.hasMaxWidth = true;
                opt.scale.maxWidth = parsed.maxWidth;
            }
            if (parsed.hasMaxHeight) {
                opt.scale.hasMaxHeight = true;
                opt.scale.maxHeight = parsed.maxHeight;
            }
            if (parsed.hasMaxPixels) {
                opt.scale.hasMaxPixels = true;
                opt.scale.maxPixels = parsed.maxPixels;
            }
            opt.scale.written = value;
            return;
        }

        // ---- 期限 ----
        // 只认十进制毫秒数：0x 前缀、下划线这种"句柄写法"放到时长上只会让人算错。
        // 0 有含义（= 不设这项期限），所以它下界是 0；空值仍然不算 0。
        if (name == L"timeout-ms" || name == L"consent-timeout-ms") {
            const bool consentOnly = name == L"consent-timeout-ms";
            uint64_t ms = 0;
            if (!ParseDecimal(value, 0, kMaxTimeoutMs, &ms)) {
                Err(codes::kInvalidNumber, Msgf(L"cli.timeout_value", kMaxTimeoutMs),
                    L"--" + name, value, Msg(L"cli.decimal_hint"));
                return;
            }
            if (consentOnly) opt.consentTimeoutMs = ms;
            else opt.timeoutMs = ms;
            return;
        }

        // ---- 输出 ----
        if (name == L"out") {
            if (outExplicit)
                Err(codes::kDuplicateOutput, Msg(L"cli.duplicate_output"), L"--out", value);
            opt.output = value;
            outExplicit = true;
            return;
        }
        if (name == L"format") {
            const auto f = ParseFormat(value);
            if (!f) {
                std::wstring allowed;
                for (const wchar_t* const* p = kFormatValues; *p; ++p) {
                    if (p != kFormatValues) allowed += L", ";
                    allowed += *p;
                }
                Err(codes::kInvalidFormat, Msg(L"cli.format_value"), L"--format", value, allowed);
                return;
            }
            opt.format = *f;
            opt.formatExplicit = true;
            formatExplicit = true;
            return;
        }
        if (name == L"quality") {
            uint64_t q = 0;
            // 区间的数字本体在 cli_limits 里（--capabilities 的 limits 段读同一份）
            if (!ParseDecimal(value, cli_limits::kJpegQualityMin, cli_limits::kJpegQualityMax, &q)) {
                Err(codes::kInvalidNumber, Msg(L"cli.quality_value"), L"--quality", value,
                    Msg(L"cli.decimal_hint"));
                return;
            }
            opt.jpegQuality = static_cast<int>(q);
            qualityExplicit = true;
            return;
        }
        // 负向开关：裸写与 --no-overwrite=true/1/yes/on/y 都是禁止覆盖（value 为空或 "1"）；
        // =false/0/no/off/n 才是取消禁令。每次都是整字段赋值，所以重复给出时最后一个生效。
        if (name == L"no-overwrite") { opt.overwrite = (value == L"0"); return; }

        // ---- 行为 ----
        if (name == L"lang") {
            // 取值语义与预扫描共用 ResolveLangToken：auto 是"明确回到系统显示语言"，
            // 不是"保持上一条 --lang"；认得的标签覆盖前一条；认不了的整条作废并报码。
            Language resolved = Language::kEn;
            const LangToken token = ResolveLangToken(value, DetectSystemLanguage(), &resolved);
            if (token == LangToken::kInvalid) {
                std::wstring list;
                for (const wchar_t* const* p = kLangValues; *p; ++p) {
                    if (p != kLangValues) list += L", ";
                    list += *p;
                }
                Err(codes::kUnknownLanguage, Msg(L"cli.unknown_language"), L"--lang", value, list);
                return;
            }
            SetLanguage(resolved);
            return;
        }
        if (name == L"dry-run") { opt.dryRun = true; return; }
        // 正向布尔但认 =false：裸写与 =true/1/yes/y/on 都是"跳过窗口内容路径的确认"，
        // =false/0/no/n/off 取消它；每次整字段赋值，所以重复给出时最后一个生效。
        if (name == L"yes") { opt.yes = (value != L"0"); return; }
        if (name == L"json") { opt.json = true; return; }
        if (name == L"verbose") { opt.verbose = true; return; }
        if (name == L"quiet") { opt.quiet = true; return; }
        if (name == L"help") { helpFlag = true; return; }
        if (name == L"version") { versionFlag = true; return; }

        // ---- 只读查询 ----
        // 落到 Options 上的只是"这一次要出哪份文档"，环境本身那一堆事实由 EnvReport 现问，
        // 所以这里不预判任何能力（也不该预判：那正是查询的用途）。
        if (name == L"capabilities") { opt.capabilities = true; return; }
        if (name == L"diagnostics") { opt.diagnostics = true; return; }
        if (name == L"screens") { opt.screens = true; return; }
        // 窗口查询的两个入口。同时给出算冲突（一份是列表、一份是单窗口快照，两份文档说的
        // 不是同一件事），这里只落最后一个写法，冲突判定在扫完整条 argv 之后做。
        // 这两条查询的取舍写在取值里：--list=all 把最小化窗口也列进来，
        // --inspect=path 把归属映像的完整路径也写出来（默认只写文件名，路径常含用户名）。
        // 取值只认这两个词；其余一律按"没给取值"处理而把后面的参数留给位置参数
        //（判据与 ScanArgv 里那个 LooksLikeListValue 同源，两处不许各写一套）。
        if (name == L"list") {
            listGiven = true;
            opt.windowAction = WindowAction::kList;
            if (!value.empty()) {
                if (ListQueryValue(value, &opt.listIconic)) return;
                Err(codes::kInvalidValue, Msg(L"cli.list_value"), L"--list", value,
                    Msg(L"cli.list_value_hint"));
            }
            return;
        }
        if (name == L"inspect") {
            inspectGiven = true;
            opt.windowAction = WindowAction::kInspect;
            if (!value.empty()) {
                if (ListQueryValue(value, &opt.inspectPath)) return;
                Err(codes::kInvalidValue, Msg(L"cli.inspect_value"), L"--inspect", value,
                    Msg(L"cli.inspect_value_hint"));
            }
            return;
        }
        // 分页两个数只认严格十进制，与 --index / --monitor 的编号同一套规矩。
        // 上界取「一次求值本来就能拿到多少条」那一道线，超过它就不可能对真实命中数有意义。
        if (name == L"offset") {
            if (!ParseDecimal(value, 0, kMaxWindowListItems, &opt.offset)) {
                Err(codes::kInvalidNumber, Msgf(L"cli.list_count_value", kMaxWindowListItems),
                    L"--offset", value, Msg(L"cli.decimal_hint"));
            }
            return;
        }
        if (name == L"limit") {
            if (!ParseDecimal(value, 1, kMaxWindowListItems, &opt.limit)) {
                Err(codes::kInvalidNumber, Msgf(L"cli.list_count_value", kMaxWindowListItems),
                    L"--limit", value, Msg(L"cli.decimal_hint"));
            }
            return;
        }

        Err(codes::kInvalidValue, Msg(L"cli.unhandled_option"), L"--" + name, value);
    };

    // token 怎么消费由 ScanArgv 一处决定（与开头的语言预扫描同一个扫描器）；这里只管
    // "扫出来的条目落到数据结构里是什么"。
    ScanArgv(argc, argv, [&](const ScannedItem& it) {
        switch (it.kind) {
            case ScannedItem::Kind::kPositional:
                positional.push_back(it.token);
                return;
            case ScannedItem::Kind::kUnknownOption: {
                std::wstring hint;
                if (const auto candidate = ClosestOption(it.name)) hint = *candidate;
                Err(codes::kUnknownOption, Msg(L"cli.unknown_option"), it.token, L"", hint);
                return;
            }
            case ScannedItem::Kind::kOption: break;
        }

        const ScannedOption& scanned = it.option;
        const std::wstring flagName = L"--" + std::wstring(scanned.spec->name);
        // 两份允许清单比的是**规范名**（不带 -- 前缀），报错时交出去的才是用户写的那个样子。
        const std::wstring bareName = scanned.spec->name;
        // 只登记「与哪一类查询冲突」这件事，不改变 Apply 的行为：这一次到底算哪一路，
        // 要等整条 argv 扫完、看见 --capabilities / --diagnostics / --list / --inspect 出没出没才知道。
        if (!AllowedWithEnvQuery(bareName)) NoteConflict(&envConflicts, flagName);
        if (!AllowedWithWindowQuery(bareName)) NoteConflict(&windowConflicts, flagName);
        // 选择策略那一组要按窗口查询的实际入口再判一次，所以把用户写过的名字留一份
        //（--all 也在里面：它与 --inspect 说不通，但与 --list 语义同源，两边各按不同方向判）。
        if (std::find(pickFlags.begin(), pickFlags.end(), flagName) == pickFlags.end() &&
            (bareName == L"index" || bareName == L"topmost-match" ||
             bareName == L"bottommost-match" || bareName == L"newest" || bareName == L"oldest" ||
             bareName == L"all")) {
            pickFlags.push_back(flagName);
        }
        if (scanned.spec->takesValue) {
            if (scanned.missingValue) {
                Err(codes::kMissingValue, Msg(L"cli.missing_value"), flagName, L"");
                return;
            }
            Apply(*scanned.spec, scanned.value);
            return;
        }

        // 开关一般不吃取值；只有 =true / =false 这种写法把规范化结果（"1" / "0"）交给 Apply。
        std::wstring boolArg;
        if (scanned.hasValue) {
            bool b = false;
            const std::wstring flag = flagName;
            if (!ParseBool(scanned.value, &b)) {
                Err(codes::kSwitchTakesNoValue, Msg(L"cli.switch_no_value"), flag, scanned.value,
                    Msgf(L"cli.switch_no_value_hint", flag));
                return;
            }
            // 普通开关写 =false 等于没写；--no-overwrite 这类反向开关的 =false 才是取消禁令。
            // --yes 这类带 valueAlways 的正向开关也要认 =false（=false 就是"不许跳过确认"）。
            if (!b && !scanned.spec->inverted && !scanned.spec->valueAlways) return;
            boolArg = b ? L"1" : L"0";
        }
        Apply(*scanned.spec, boolArg);
    });

    // ---- 位置参数：第一个是输出路径，多出来的视为误写 ----
    if (!positional.empty() && !outExplicit) opt.output = positional[0];
    if (positional.size() > 1) {
        std::wstring extra;
        for (size_t k = 1; k < positional.size(); ++k) {
            if (k > 1) extra += L", ";
            extra += positional[k];
        }
        Err(codes::kUnexpectedPositional, Msg(L"cli.unexpected_positional"), L"", extra);
    }
    if (!positional.empty() && outExplicit)
        Err(codes::kDuplicateOutput, Msg(L"cli.output_duplicate_positional"), L"--out", positional[0]);

    // ---- 选择策略互斥检查 ----
    // 按策略去重（同一条策略的新旧两种写法算一条），每个策略报出来的是用户实际写的那个名字。
    std::vector<PickUsage> distinctPick;
    for (const PickUsage& p : picks) {
        const bool seen = std::any_of(distinctPick.begin(), distinctPick.end(),
                                      [&](const PickUsage& q) { return q.strategy == p.strategy; });
        if (!seen) distinctPick.push_back(p);
    }
    std::sort(distinctPick.begin(), distinctPick.end(),
              [](const PickUsage& a, const PickUsage& b) { return a.display < b.display; });
    if (distinctPick.size() > 1) {
        std::wstring joined;
        for (size_t k = 0; k < distinctPick.size(); ++k) {
            if (k) joined += L", ";
            joined += distinctPick[k].display;
        }
        Err(codes::kConflictingOptions, Msg(L"cli.conflicting_options"), L"", joined,
            Msg(L"cli.conflicting_hint"));
    }
    opt.multi = distinctPick.empty() ? MultiMatch::kAsk : multiFlag;

    // ---- 裁剪方式互斥 ----
    // --roi 与 --client-area 说的是同一件事的两种写法（这一次交回来的图留哪一块），
    // 同时给出就不知道该听哪一条 —— 与选择策略那一组同一条规矩：不替用户挑一种执行。
    // 但这条码与 cli.conflicting_options 分开：那句文案说的是"选择策略"（--index 那一组），
    // 拿它回答一次裁剪请求会把人引向"删掉 --index"这种与本次毫无关系的下一步。
    if (opt.crop.roiGiven && opt.crop.clientAreaGiven) {
        Err(codes::kCropConflict, Msg(L"cli.crop_conflict"), L"",
            L"--client-area, --roi", Msg(L"cli.crop_conflict_hint"));
    }

    // ---- 查询命令的互斥判定 ----
    // 判据是「这一次是哪一类查询」，不是「有没有某个选项本身写坏了」：写在查询后面的 --title 就算
    // 值法不对，这里也先报冲突（一次只报说不通的那一处，免得调用方修完一个又一个）。
    // 两类查询各自允许什么写在上面那两份判据里；一份文档里绝不替用户挑一个执行。
    const auto JoinFlags = [](const std::vector<std::wstring>& items) {
        std::wstring joined;
        for (size_t k = 0; k < items.size(); ++k) {
            if (k) joined += L", ";
            joined += items[k];
        }
        return joined;
    };
    if (opt.EnvQueryMode()) {
        // 三条环境查询（--capabilities / --diagnostics / --screens）同时给出：每一份都是
        // 一次独立的问答，同时出两份只会让调用方不知道读哪一份，所以这里是一条冲突，
        // 而不是把两份拼起来。报错把用户写过的**每一条**都列上（不只是前两条）。
        std::vector<std::wstring> given;
        if (opt.capabilities) given.push_back(L"--capabilities");
        if (opt.diagnostics) given.push_back(L"--diagnostics");
        if (opt.screens) given.push_back(L"--screens");
        if (given.size() > 1) {
            for (const std::wstring& f : given) NoteConflict(&envConflicts, f);
        }
        // 位置参数就是输出路径：它不吃 spec，所以扫完之后单独登记。报错里写 --out 而不回显
        // 用户那条路径本身——查询这一路的规矩是不把路径带进输出。
        if (!positional.empty()) NoteConflict(&envConflicts, L"--out");
        if (!envConflicts.empty()) {
            Err(codes::kQueryConflict, Msg(L"cli.query_conflict"), given.front(),
                JoinFlags(envConflicts), Msg(L"cli.query_conflict_hint"));
        }
    }
    if (opt.WindowQueryMode()) {
        if (listGiven && inspectGiven) {
            // --list 交回一份列表、--inspect 交回一份单窗口快照：同一次只出一份，
            // 报法与上面「两条环境查询同时给出」同源。
            NoteConflict(&windowConflicts, L"--list");
            NoteConflict(&windowConflicts, L"--inspect");
        }
        // 选择策略按入口分别处置（上面那份 pickFlags 就是用户写过的名字）：
        //   --list   本来就是「全部命中的分页」，再问「取哪一扇」自相矛盾
        //   --inspect 一份快照只能对应一扇窗口，--all 那个「每扇各一张」没有对象
        for (const std::wstring& flag : pickFlags) {
            if (opt.windowAction == WindowAction::kList && flag != L"--all") {
                NoteConflict(&windowConflicts, flag);
            }
            if (opt.windowAction == WindowAction::kInspect && flag == L"--all") {
                NoteConflict(&windowConflicts, flag);
            }
        }
        if (!positional.empty()) NoteConflict(&windowConflicts, L"--out");
        if (!windowConflicts.empty()) {
            const std::wstring wanted =
                opt.windowAction == WindowAction::kList ? L"--list" : L"--inspect";
            Err(codes::kWindowQueryConflict, Msg(L"cli.window_query_conflict"), wanted,
                JoinFlags(windowConflicts), Msg(L"cli.window_query_conflict_hint"));
        }
    }

    opt.showVersion = versionFlag;
    if (helpFlag) opt.showHelp = true;
    // 只有在没有参数错误、且没显式 --help/--version 时，才因为"零条件"返回帮助。
    // 给了 --monitor 就不算零条件：那是明确的屏幕目标。
    // 查询命令同理：它本身就是明确的意图，没有窗口条件正是它的正常用法（一次截图都不做）。
    if (result.errors.empty() && !opt.showHelp && !versionFlag && !opt.QueryMode() &&
        !opt.HasAnyCondition() && !opt.monitor.given) {
        opt.showHelp = true;
        opt.helpReason = codes::kNoCondition;
    }

    // ---- 输出与格式 ----
    // 查询这一路不进这一段：它没有输出路径要展开，也不该因为"没给 --out"而被记成
    // "隐式 stdout + note.output_defaulted_stdout"（那条 note 说的是图片要挤哪条流，
    // 而查询压根没有图片）。输出路径留空，结果 JSON 因此恒走 stdout。
    if (!opt.showHelp && !versionFlag && !opt.QueryMode()) {
        // 屏幕目标的两条硬规矩，都在解析期定下来，不留到运行期退化：
        // --monitor all 是"每块屏各一张"，与"按屏过滤窗口"没法同时成立；
        // dwm / printwindow 取的是窗口自己的画面，屏幕上没有这样一个窗口可取。
        if (opt.monitor.given) {
            if (opt.monitor.kind == MonitorSelector::Kind::kAll && opt.HasAnyCondition()) {
                Err(codes::kMonitorConflict, Msg(L"cli.monitor_conflict"), L"--monitor", L"all",
                    Msg(L"cli.monitor_conflict_hint"));
            }
            if (opt.ScreenMode() && (opt.capture == CaptureMethod::kDwmThumbnail ||
                                     opt.capture == CaptureMethod::kPrintWindow)) {
                Err(codes::kUnsupported, Msgf(L"cap.unsupported_for_screen", CaptureMethodName(opt.capture)),
                    L"--capture", CaptureMethodName(opt.capture),
                    Msg(L"cap.unsupported_for_screen_hint"));
            }
            // 整块屏幕的目标上没有"一扇窗口"可以让裁剪坐标相对它的左上角去算。这时候**绝不**
            // 把 --roi 当成桌面绝对坐标来用：那等于允许一个调用方用一个窗口之外的位置去要一块
            // 谁都没批准过的画面，也与"相对于目标窗口"这条承诺打脸。照实在解析期报错，
            // 一个像素都不取、一个文件都不写，也不弹框。
            if (opt.ScreenMode() && opt.crop.mode != CropMode::kNone) {
                const bool clientArea = opt.crop.mode == CropMode::kClientArea;
                Err(codes::kUnsupported, Msg(L"cap.crop_unsupported_for_screen"),
                    clientArea ? L"--client-area" : L"--roi", clientArea ? L"" : opt.crop.written,
                    Msg(L"cap.crop_unsupported_for_screen_hint"));
            }
        }

        // ---- 光标要求与这条通道能不能兑现 ----
        // 这件事由那条路径的**来源像素**决定，不是一件可以事后商量的装饰：printwindow 让窗口自绘
        // 到 DC、dwm 读重定向位图、bitblt 拷屏幕 DC，这三条的来源里根本没有光标；桌面复制读的是
        // 显示器的合成分，而按官方说明那一幅桌面图像里指针要么**已经画在上面**、要么由显卡单独
        // 叠加，这一问在本工具手里分不开（判据与理由见 src/CursorControl.h 的规矩 1、5、6）。
        // 所以两种"做不到"都在解析期就说清楚，退出码 1，一个像素都不取、不弹框，也不"那就换一条
        // 会读桌面的通道试试"（换过去既没把光标加回来，又多拍一份没人批准过的画面）：
        //   * include 配任何一条没有那个开关的通道 —— 本工具不画光标，也不拿这条要求去换后端；
        //   * exclude 配桌面复制 —— 不是"来源没有光标"，而是"这条路线保证不了"。
        // 两句文案不同，因为调用方的下一步不一样（前者换 wgc 或去掉要求；后者去掉要求之后
        // 交回的图在光标这件事上仍是未知，结果里写的是 unverified 而不是 exclude）。
        // --capture auto 不在这里判：做不到的那几条由闸门从链里摘掉并各留一条 note，
        // 摘到空了才报错（见 src/CursorControl.cpp）。
        if (opt.cursor.given && !CursorRequestPossible(opt.capture, opt.cursor.mode)) {
            const CaptureMethod kChannels[] = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                               CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt,
                                               CaptureMethod::kDuplication};
            std::wstring canDo;
            for (const CaptureMethod m : kChannels) {
                if (!CursorRequestPossible(m, opt.cursor.mode)) continue;
                // 屏幕目标上没有"某一扇窗口自己的画面"可取：那两条窗口专属的通道不该出现在
                // "换这条试试"的建议里（建议给一条根本用不了的通道等于没说）。
                if (opt.ScreenMode() && (m == CaptureMethod::kDwmThumbnail ||
                                         m == CaptureMethod::kPrintWindow))
                    continue;
                if (!canDo.empty()) canDo += L", ";
                canDo += CaptureMethodName(m);
            }
            // 同一条码、两句文案：拿"来源里根本没有光标"去说桌面复制那一条是假话 ——
            // 那一条的来源可能已经把指针画在画面里，而它没有可读回的开关。
            // 判据与登记表、筛链共用同一份（这里查的正是那张表在通道级的那两句判断）。
            const bool unprovable = ChannelPointerMayBeInImage(opt.capture);
            Err(codes::kCursorUnsupported,
                Msgf(unprovable ? L"cap.cursor_unsupported_unprovable" : L"cap.cursor_unsupported",
                     CursorModeName(opt.cursor.mode), CaptureMethodName(opt.capture), canDo),
                L"--cursor", CursorModeName(opt.cursor.mode),
                Msg(unprovable ? L"cap.cursor_unsupported_unprovable_hint"
                               : L"cap.cursor_unsupported_hint"));
        }

        // ---- HDR 处理要求与这条通道兑不兑现得了 ----
        // 与 --cursor 同一类：判据是那张按**内部路径**登记的表（唯一出处在 src/HdrColor.h）。
        // 两种做不到的原因分开说，因为它们给调用方的下一步不一样，而这一条错误必须说真话：
        //   * printwindow / dwm / bitblt —— 结构上只带得回 8 位 SDR，来源里根本没有 HDR 可映射；
        //   * duplication —— 来源在 Windows 那一侧**可能**跟显示模式走，但本构建没实现兑现显式
        //     策略所需的那几步（仍用 DuplicateOutput()，采集前不问那块屏的色彩空间，10 位那一条
        //     的 PQ/HLG 之分也没核实），所以不敢拿它兑现一个用户显式要过的要求。
        // 两种都在解析期报 capture.hdr_unsupported + 退出码 1，**不换后端**（换一条会读桌面像素
        // 的通道既没有更多 HDR 可映射，又多拍一份没人批准过的画面）。
        // --capture auto 不在这里判（落到哪条通道要到运行期才知道，判据是 FilterChainForHdr）。
        // 这一条排在授权规则之外：它不改变"会不会弹框"，只回答"这个组合根本没法兑现"。
        if (opt.hdr.given && !HdrRequestPossible(opt.capture, opt.hdr.policy)) {
            std::wstring canDo;
            const CaptureMethod kChannels[] = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                               CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt,
                                               CaptureMethod::kDuplication};
            for (const CaptureMethod m : kChannels) {
                if (!HdrRequestPossible(m, opt.hdr.policy)) continue;
                if (!canDo.empty()) canDo += L", ";
                canDo += CaptureMethodName(m);
            }
            // 这条路径"来源可能带广色域、而本构建没兑现"时换那一句文案：拿"结构上带不回"
            // 去说桌面复制那条是假话，而文案与判据同源这件事本身就是这份契约的内容。
            const bool notImplemented =
                ChannelCarriesWideColorFrame(opt.capture) && !ChannelHonorsHdrPolicy(opt.capture);
            Err(codes::kHdrUnsupported,
                Msgf(notImplemented ? L"cap.hdr_unsupported_not_implemented"
                                    : L"cap.hdr_unsupported",
                     HdrPolicyName(opt.hdr.policy), CaptureMethodName(opt.capture), canDo),
                L"--hdr", HdrPolicyName(opt.hdr.policy), Msg(L"cap.hdr_unsupported_hint"));
        }

        // 没给输出路径不再算错：按 "--out -" 处理，图片走 stdout，JSON 走 stderr
        if (opt.output.empty()) {
            opt.output = L"-";
            opt.outputImplicitStdout = true;
            if (!formatExplicit) opt.format = ImageFormat::kPng;
            Note(codes::kOutputDefaultedStdout, Msg(L"note.output_defaulted_stdout"), L"--out",
                 L"-", Msg(L"note.output_defaulted_stdout_hint"));
        }

        if (opt.output == L"-" && !opt.outputImplicitStdout) {
            if (!formatExplicit) {
                opt.format = ImageFormat::kPng;
                Note(codes::kPipeDefaultFormat, Msg(L"note.pipe_default_format"), L"--out", L"-");
            }
        } else if (opt.output != L"-") {
            const std::wstring ext = ExtensionOf(opt.output);
            const auto fromExt = FormatFromExtension(ext);
            if (formatExplicit) {
                if (fromExt && *fromExt != opt.format) {
                    Note(codes::kFormatExtensionMismatch, Msg(L"note.format_extension_mismatch"),
                         L"--format", std::wstring(FormatName(opt.format)) + L" vs ." + ext);
                }
            } else if (fromExt) {
                opt.format = *fromExt;
            } else {
                opt.format = ImageFormat::kPng;
                if (!ext.empty()) {
                    Note(codes::kFormatDefaultedPng, Msg(L"note.format_defaulted_png"), L"--out",
                         opt.output, Msg(L"note.format_defaulted_png_hint"));
                }
                // 扩展名为空时不在此处提示：真正补 .png 的动作在 Capture.cpp 里，
                // 那里会发 note.output_extension_appended
            }
        }

        if (qualityExplicit && opt.format != ImageFormat::kJpeg)
            Note(codes::kQualityIgnored, Msg(L"note.quality_ignored"), L"--quality",
                 std::wstring(FormatName(opt.format)));
        if (opt.multi == MultiMatch::kAll && opt.output != L"-" && !ContainsPlaceholder(opt.output))
            Note(codes::kAllWithoutPlaceholder, Msg(L"note.all_without_placeholder"), L"--all",
                 opt.output, Msg(L"note.all_without_placeholder_hint"));
        if (opt.json)
            Note(codes::kJsonFlagDeprecated, Msg(L"note.json_flag_deprecated"), L"--json");
        // 旧的选择策略名字照旧有效，但要说清楚它选的是当下的 Z 序、不是创建时间，
        // 并且给出语义准确的那个写法。note 只在真的用了旧名字时才发，且各发一条。
        if (newestAliasUsed)
            Note(codes::kDeprecatedOption,
                 Msgf(L"note.deprecated_option", L"--newest", L"--topmost-match"), L"--newest",
                 L"--topmost-match");
        if (oldestAliasUsed)
            Note(codes::kDeprecatedOption,
                 Msgf(L"note.deprecated_option", L"--oldest", L"--bottommost-match"), L"--oldest",
                 L"--bottommost-match");
        // -v 与 -q 同时给出时按 --verbose 处理（文案里承诺过的就是这一条）。这里直接把
        // quiet 归掉，而不是在渲染那一层再判一次"verbose 时不抑制"：两处各判一套优先级，
        // 迟早会有一处漏掉 —— 而漏掉的症状是"这条冲突提示自己被 --quiet 抑制了"，
        // 也就是调用方看到 notes 还在、却不知道为什么不在了。
        // errors 与 images[].source / path / scope 从来不进这条链路：它们任何时候都不被隐藏。
        if (opt.verbose && opt.quiet) {
            Note(codes::kFlagOverridesQuiet, Msg(L"note.flag_overrides_quiet"), L"--verbose");
            opt.quiet = false;
        }
    }

    result.ok = result.errors.empty();
    return result;
}

const std::vector<OptionInfo>& OptionCatalog() {
    static const std::vector<OptionInfo> catalog = BuildCatalog();
    return catalog;
}

}  // namespace ecapture
