#include "CliOptions.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {
namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

constexpr const wchar_t* kProgramName = L"ECAPTURE.EXE";

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

// 数值解析：允许十进制，允许 0x/0X 前缀的十六进制；带字母的按十六进制解释。
bool ParseNumber(const std::wstring& raw, uint64_t* out) {
    const std::wstring text = Trim(raw);
    if (text.empty()) return false;
    int base = 10;
    std::wstring body = text;
    if (StartsWith(body, L"0x") || StartsWith(body, L"0X")) {
        base = 16;
        body = body.substr(2);
    } else if (std::any_of(body.begin(), body.end(),
                           [](wchar_t c) { return std::iswxdigit(c) && std::iswalpha(c); })) {
        base = 16;  // 形如 001A0B4C，按 Spy++ 风格十六进制处理
    }
    if (body.empty()) return false;
    // 允许下划线分隔的可读写法：0x001A_0B4C
    std::wstring cleaned;
    cleaned.reserve(body.size());
    for (wchar_t c : body) if (c != L'_') cleaned.push_back(c);

    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long value = std::wcstoull(cleaned.c_str(), &end, base);
    if (errno == ERANGE || end == cleaned.c_str()) return false;
    while (end && *end && std::iswspace(*end)) ++end;
    if (end && *end) return false;
    *out = value;
    return true;
}

bool ParseBool(const std::wstring& raw, bool* out) {
    const std::wstring v = ToLower(Trim(raw));
    if (v == L"1" || v == L"true" || v == L"yes" || v == L"y" || v == L"on") { *out = true; return true; }
    if (v == L"0" || v == L"false" || v == L"no" || v == L"n" || v == L"off") { *out = false; return true; }
    return false;
}

std::wstring Quote(const std::wstring& s) {
    std::wstringstream ss;
    ss << L'"' << s << L'"';
    return ss.str();
}

std::wstring FormatHwnd(uint64_t hwnd) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%016llX", static_cast<unsigned long long>(hwnd));
    return buf;
}

// ---------------------------------------------------------------------------
// 选项表
// ---------------------------------------------------------------------------

struct OptionSpec {
    const wchar_t* name;        // 规范名（不含前导 -）
    const wchar_t* shortName;   // 单字母别名，可为空
    bool takesValue;            // false = 开关
    const wchar_t* group;       // 分组，用于 --help 与错误提示
};

constexpr OptionSpec kOptions[] = {
    // 匹配条件
    {L"hwnd",            L"",  true,  L"match"},
    {L"pid",             L"",  true,  L"match"},
    {L"process",         L"p",  true,  L"match"},
    {L"exe",             L"",  true,  L"match"},
    {L"title",           L"t",  true,  L"match"},
    {L"title-contains",  L"T",  true,  L"match"},
    {L"title-regex",     L"R",  true,  L"match"},
    {L"class",           L"c",  true,  L"match"},
    // 选择策略（匹配到多个窗口时）
    {L"index",           L"i",  true,  L"pick"},
    {L"newest",          L"",  false, L"pick"},
    {L"oldest",          L"",  false, L"pick"},
    {L"all",             L"a",  false, L"pick"},
    // 输出
    {L"out",             L"o",  true,  L"output"},
    {L"format",          L"f",  true,  L"output"},
    {L"quality",         L"",  true,  L"output"},
    {L"no-overwrite",    L"",  false, L"output"},
    // 行为
    {L"dry-run",         L"d",  false, L"behavior"},
    {L"json",            L"j",  false, L"behavior"},
    {L"verbose",         L"v",  false, L"behavior"},
    {L"quiet",           L"q",  false, L"behavior"},
    {L"help",            L"h",  false, L"behavior"},
    {L"version",         L"",  false, L"behavior"},
};

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
    if (v == L"auto") return ImageFormat::kAuto;
    if (v == L"png") return ImageFormat::kPng;
    if (v == L"jpg" || v == L"jpeg") return ImageFormat::kJpeg;
    if (v == L"bmp") return ImageFormat::kBmp;
    if (v == L"tif" || v == L"tiff") return ImageFormat::kTiff;
    if (v == L"gif") return ImageFormat::kGif;
    if (v == L"webp") return ImageFormat::kWebp;
    if (v == L"ico") return ImageFormat::kIco;
    return std::nullopt;
}

}  // namespace

// 以下为 ecapture 命名空间的公开辅助函数（供 main.cpp 复用）
const wchar_t* FormatName(ImageFormat f) {
    switch (f) {
        case ImageFormat::kAuto: return L"auto";
        case ImageFormat::kPng: return L"png";
        case ImageFormat::kJpeg: return L"jpeg";
        case ImageFormat::kBmp: return L"bmp";
        case ImageFormat::kTiff: return L"tiff";
        case ImageFormat::kGif: return L"gif";
        case ImageFormat::kWebp: return L"webp";
        case ImageFormat::kIco: return L"ico";
    }
    return L"?";
}

const wchar_t* MultiKey(MultiMatch m) {
    switch (m) {
        case MultiMatch::kAsk: return L"ask";
        case MultiMatch::kIndex: return L"index";
        case MultiMatch::kNewest: return L"newest";
        case MultiMatch::kOldest: return L"oldest";
        case MultiMatch::kAll: return L"all";
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

// 向列表追加值；重复值给出提示（同一选项内的重复值没有意义）。
template <typename T>
void PushUnique(std::vector<T>* list, T value, const wchar_t* label,
                std::vector<std::wstring>* warnings) {
    if (std::find(list->begin(), list->end(), value) != list->end()) {
        warnings->push_back(std::wstring(L"重复的 ") + label + L" " + std::to_wstring(value) + L" 已忽略");
        return;
    }
    list->push_back(std::move(value));
}

template <>
void PushUnique<std::wstring>(std::vector<std::wstring>* list, std::wstring value,
                              const wchar_t* label, std::vector<std::wstring>* warnings) {
    if (std::find(list->begin(), list->end(), value) != list->end()) {
        warnings->push_back(std::wstring(L"重复的 ") + label + L" " + Quote(value) + L" 已忽略");
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
    if (ext == L"webp") return ImageFormat::kWebp;
    if (ext == L"ico") return ImageFormat::kIco;
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
    auto& errors = result.errors;
    auto& warnings = result.warnings;
    auto& suggestions = result.suggestions;

    std::vector<std::wstring> positional;
    bool helpFlag = false;
    bool versionFlag = false;
    bool formatExplicit = false;
    bool qualityExplicit = false;
    bool outExplicit = false;
    std::vector<std::wstring> pickFlags;   // --index/--newest/--oldest/--all，互斥
    MultiMatch multiFlag = MultiMatch::kAsk;

    // 单个选项 -> 数据结构
    auto Apply = [&](const OptionSpec& spec, const std::wstring& value) {
        const std::wstring name = spec.name;

        // ---- 匹配条件 ----
        if (name == L"hwnd") {
            uint64_t hwnd = 0;
            if (!ParseNumber(value, &hwnd) || hwnd == 0) {
                errors.push_back(L"--hwnd 需要有效的句柄值（十进制，或带 0x 前缀的十六进制）：" + value);
                return;
            }
            PushUnique(&opt.match.hwnds, hwnd, L"--hwnd", &warnings);
            return;
        }
        if (name == L"pid") {
            uint64_t pid = 0;
            if (!ParseNumber(value, &pid) || pid == 0 || pid > 0xFFFFFFFFull) {
                errors.push_back(L"--pid 需要有效的进程 ID（十进制，> 0）：" + value);
                return;
            }
            PushUnique(&opt.match.pids, static_cast<uint32_t>(pid), L"--pid", &warnings);
            return;
        }
        if (name == L"process") {
            const std::wstring v = Trim(value);
            if (v.empty()) { errors.push_back(L"--process 不能为空"); return; }
            if (HasPathSeparator(v)) {
                errors.push_back(L"--process 只接受映像文件名（例如 notepad.exe）；完整路径请使用 --exe：" + v);
                return;
            }
            PushUnique(&opt.match.processes, v, L"--process", &warnings);
            if (v.find(L'.') == std::wstring::npos)
                warnings.push_back(L"--process " + Quote(v) + L" 没有扩展名，匹配时按 " + Quote(v + L".exe") + L" 处理");
            return;
        }
        if (name == L"exe") {
            const std::wstring v = Trim(value);
            if (v.empty()) { errors.push_back(L"--exe 不能为空"); return; }
            if (!HasPathSeparator(v))
                warnings.push_back(L"--exe " + Quote(v) + L" 看起来不是完整路径；若只要文件名请改用 --process");
            PushUnique(&opt.match.exePaths, v, L"--exe", &warnings);
            return;
        }
        if (name == L"title") { PushUnique(&opt.match.titles, value, L"--title", &warnings); return; }
        if (name == L"title-contains") {
            PushUnique(&opt.match.titleContains, value, L"--title-contains", &warnings);
            return;
        }
        if (name == L"title-regex") {
            try {
                std::wregex probe(value, std::regex_constants::ECMAScript);
                (void)probe;
            } catch (const std::regex_error& e) {
                std::wstringstream ss;
                ss << L"--title-regex 正则表达式无效: " << value << L"  (code="
                   << static_cast<int>(e.code()) << L", ";
                for (const char* p = e.what(); p && *p; ++p)
                    ss << static_cast<wchar_t>(static_cast<unsigned char>(*p));
                ss << L")";
                errors.push_back(ss.str());
                return;
            }
            PushUnique(&opt.match.titleRegexes, value, L"--title-regex", &warnings);
            return;
        }
        if (name == L"class") {
            const std::wstring v = Trim(value);
            if (v.empty()) { errors.push_back(L"--class 不能为空"); return; }
            PushUnique(&opt.match.classes, v, L"--class", &warnings);
            return;
        }

        // ---- 多窗口选择策略 ----
        if (name == L"index") {
            uint64_t n = 0;
            if (!ParseNumber(value, &n) || n == 0 || n > 0xFFFF) {
                errors.push_back(L"--index 是从 1 开始的序号：" + value);
                return;
            }
            opt.index = static_cast<int>(n);
            multiFlag = MultiMatch::kIndex;
            pickFlags.push_back(L"--index");
            return;
        }
        if (name == L"newest") { multiFlag = MultiMatch::kNewest; pickFlags.push_back(L"--newest"); return; }
        if (name == L"oldest") { multiFlag = MultiMatch::kOldest; pickFlags.push_back(L"--oldest"); return; }
        if (name == L"all")    { multiFlag = MultiMatch::kAll;    pickFlags.push_back(L"--all"); return; }

        // ---- 输出 ----
        if (name == L"out") {
            if (outExplicit) errors.push_back(L"重复的 --out，输出路径只能有一个");
            opt.output = value;
            outExplicit = true;
            return;
        }
        if (name == L"format") {
            const auto f = ParseFormat(value);
            if (!f) {
                errors.push_back(L"--format 取值无效：" + value +
                                 L"（可用：auto png jpg bmp tiff gif webp ico）");
                return;
            }
            opt.format = *f;
            formatExplicit = true;
            return;
        }
        if (name == L"quality") {
            uint64_t q = 0;
            if (!ParseNumber(value, &q) || q < 1 || q > 100) {
                errors.push_back(L"--quality 需要在 1..100 之间：" + value);
                return;
            }
            opt.jpegQuality = static_cast<int>(q);
            qualityExplicit = true;
            return;
        }
        if (name == L"no-overwrite") { opt.overwrite = false; return; }

        // ---- 行为 ----
        if (name == L"dry-run") { opt.dryRun = true; return; }
        if (name == L"json") { opt.json = true; return; }
        if (name == L"verbose") { opt.verbose = true; return; }
        if (name == L"quiet") { opt.quiet = true; return; }
        if (name == L"help") { helpFlag = true; return; }
        if (name == L"version") { versionFlag = true; return; }

        errors.push_back(L"内部错误：未处理的选项 --" + name);
    };

    auto RequireValue = [&](const OptionSpec& spec, int& i, const std::optional<std::wstring>& inlineValue,
                            int argcTotal, wchar_t* const* argvTotal) -> std::optional<std::wstring> {
        if (inlineValue) return inlineValue;
        if (i + 1 >= argcTotal) {
            errors.push_back(L"选项 --" + std::wstring(spec.name) + L" 缺少取值");
            return std::nullopt;
        }
        ++i;
        return std::wstring(argvTotal[i]);
    };

    bool stopParsing = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];

        if (stopParsing || arg.empty()) { positional.push_back(arg); continue; }
        if (arg == L"--") { stopParsing = true; continue; }

        std::wstring body;
        bool longForm = false;
        if (StartsWith(arg, L"--")) { body = arg.substr(2); longForm = true; }
        else if (arg[0] == L'-' || arg[0] == L'/') { body = arg.substr(1); }
        else { positional.push_back(arg); continue; }
        if (body.empty()) { positional.push_back(arg); continue; }

        // 拆 name=value
        std::optional<std::wstring> inlineValue;
        const size_t eq = body.find(L'=');
        std::wstring name = body;
        if (eq != std::wstring::npos) {
            name = body.substr(0, eq);
            inlineValue = body.substr(eq + 1);
        }

        const OptionSpec* spec = FindOption(name);
        if (!spec && !longForm) spec = FindShortOption(name);

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
                for (const OptionSpec* s : cluster) Apply(*s, L"");
                continue;
            }
        }

        if (!spec) {
            if (longForm || HasPathSeparator(arg) == false) {
                std::wstringstream ss;
                ss << L"未知选项: " << arg;
                errors.push_back(ss.str());
            } else {
                positional.push_back(arg);
                continue;
            }
            if (const auto hint = ClosestOption(name)) {
                suggestions.push_back(L"是否想输入 " + *hint + L" ?");
            }
            continue;
        }

        if (spec->takesValue) {
            if (auto value = RequireValue(*spec, i, inlineValue, argc, argv); value) Apply(*spec, *value);
        } else {
            if (inlineValue) {
                bool b = false;
                if (!ParseBool(*inlineValue, &b)) {
                    errors.push_back(L"开关 --" + std::wstring(spec->name) +
                                     L" 不接受取值（写 --" + std::wstring(spec->name) +
                                     L" 即可，或 --" + std::wstring(spec->name) + L"=false）");
                    continue;
                }
                // 允许 --no-overwrite=false 这种反写
                if (std::wstring(spec->name) == L"no-overwrite") { opt.overwrite = b; continue; }
                if (!b) continue;
            }
            Apply(*spec, L"");
        }
    }

    // ---- 位置参数：第一个是输出路径，多出来的视为误写 ----
    if (!positional.empty() && !outExplicit) opt.output = positional[0];
    if (positional.size() > 1) {
        std::wstringstream ss;
        ss << L"意外的位置参数（输出路径只能有一个，其余条件请用选项给出）: ";
        for (size_t k = 1; k < positional.size(); ++k) {
            if (k > 1) ss << L"  ";
            ss << Quote(positional[k]);
        }
        errors.push_back(ss.str());
    }
    if (!positional.empty() && outExplicit)
        errors.push_back(L"输出路径重复：--out 与位置参数只能给一个");

    // ---- 选择策略互斥检查 ----
    std::vector<std::wstring> distinctPick = pickFlags;
    std::sort(distinctPick.begin(), distinctPick.end());
    distinctPick.erase(std::unique(distinctPick.begin(), distinctPick.end()), distinctPick.end());
    if (distinctPick.size() > 1) {
        std::wstringstream ss;
        ss << L"这些选项互相冲突，只能选一个: ";
        for (size_t k = 0; k < distinctPick.size(); ++k) {
            if (k) ss << L"  ";
            ss << distinctPick[k];
        }
        errors.push_back(ss.str());
    }
    opt.multi = distinctPick.empty() ? MultiMatch::kAsk : multiFlag;

    opt.showVersion = versionFlag;
    if (helpFlag) opt.showHelp = true;
    if (!opt.showHelp && !versionFlag && !opt.HasAnyCondition()) {
        opt.showHelp = true;
        opt.helpReason = L"未指定任何匹配条件";
    }

    // ---- 输出与格式 ----
    if (!opt.showHelp && !versionFlag) {
        if (opt.output.empty()) {
            errors.push_back(L"缺少输出路径文件名（或用 --out <路径>，`-` 表示写到标准输出）");
        } else if (opt.output != L"-") {
            const std::wstring ext = ExtensionOf(opt.output);
            const auto fromExt = FormatFromExtension(ext);
            if (formatExplicit && fromExt && *fromExt != opt.format) {
                warnings.push_back(std::wstring(L"--format ") + FormatName(opt.format) +
                                   L" 与扩展名 ." + ext + L" 不一致，按 --format 编码（文件名保持不变）");
            } else if (!formatExplicit && fromExt) {
                opt.format = *fromExt;
            } else if (!formatExplicit && !fromExt) {
                errors.push_back(L"无法从输出文件名确定图片格式，请加已知扩展名或用 --format 指定: " + opt.output);
            }
        } else if (!formatExplicit) {
            opt.format = ImageFormat::kPng;
            warnings.push_back(L"输出到标准输出且未指定 --format，默认使用 png");
        }

        if (qualityExplicit && opt.format != ImageFormat::kJpeg)
            warnings.push_back(L"--quality 只对 jpeg 有效，当前格式为 " + std::wstring(FormatName(opt.format)));
        if (opt.multi == MultiMatch::kAll && opt.output != L"-" && !ContainsPlaceholder(opt.output))
            warnings.push_back(L"--all 建议输出名包含占位符（如 %t_%h_%i.png），否则会在文件名后自动追加序号");
        if (opt.json && opt.quiet) warnings.push_back(L"--json 与 --quiet 同时给出时，以 --json 为准");
        if (opt.verbose && opt.quiet) warnings.push_back(L"--verbose 与 --quiet 同时给出时，以 --verbose 为准");
    }

    result.ok = errors.empty();
    return result;
}

// ---------------------------------------------------------------------------
// 控制台输出
// ---------------------------------------------------------------------------
void PrintLine(const std::wstring& text, bool toStdErr) {
    HANDLE handle = GetStdHandle(toStdErr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    const std::wstring line = text + L"\r\n";
    if (handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) == FILE_TYPE_CHAR) {
        DWORD written = 0;
        WriteConsoleW(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        return;
    }
    // 重定向到文件/管道：输出 UTF-8 字节
    const int need = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::vector<char> buf(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), buf.data(), need,
                        nullptr, nullptr);
    FILE* stream = toStdErr ? stderr : stdout;
    fwrite(buf.data(), 1, static_cast<size_t>(need), stream);
    fflush(stream);
}

const std::wstring& VersionText() {
    static const std::wstring text =
        L"EvernightCapture 0.1.0  (ECAPTURE.EXE / x64)\n"
        L"当前版本只实现命令行参数解析，截图功能尚未接入。";
    return text;
}

}  // namespace ecapture
