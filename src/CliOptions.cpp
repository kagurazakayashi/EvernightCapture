#include "CliOptions.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <regex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

// 取值可省略的选项要靠这个判断"下一个参数是不是我的取值"，否则会把输出路径吃掉。
bool LooksLikeMonitorValue(const std::wstring& raw) {
    const std::wstring v = ToLower(Trim(raw));
    if (v.empty()) return false;
    if (v == L"primary" || v == L"all") return true;
    return std::all_of(v.begin(), v.end(), [](wchar_t c) { return std::iswdigit(c) != 0; });
}

bool LooksLikeOptionalValue(const std::wstring& name, const std::wstring& raw) {
    if (name == L"monitor") return LooksLikeMonitorValue(raw);
    return false;
}

// --lang 必须早于其余选项定下来：解析期的错误文案本身就要用调用方指定的语言。
// 这里只找 --lang / -l / /lang 三种写法，取值非法不在这里报错——正式解析会按
// 当前已生效的语言报 cli.unknown_language。
void SelectLanguageFromCommandLine(int argc, wchar_t* const* argv) {
    SetLanguage(DetectSystemLanguage());
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        std::wstring body;
        if (StartsWith(arg, L"--")) body = arg.substr(2);
        else if (!arg.empty() && (arg[0] == L'-' || arg[0] == L'/')) body = arg.substr(1);
        else continue;

        std::wstring name = body;
        std::optional<std::wstring> inlineValue;
        const size_t eq = body.find(L'=');
        if (eq != std::wstring::npos) {
            name = body.substr(0, eq);
            inlineValue = body.substr(eq + 1);
        }
        const bool isLong = EqualsInsensitive(name, L"lang");
        const bool isShort = !isLong && name.size() == 1 && name[0] == L'l';
        if (!isLong && !isShort) continue;

        std::wstring value = inlineValue ? *inlineValue
                                         : (i + 1 < argc ? std::wstring(argv[i + 1]) : std::wstring());
        value = Trim(value);
        // 不 break：写了多个 --lang 时以最后一个为准（正式解析也是后者覆盖前者）
        if (value.empty() || EqualsInsensitive(value, L"auto")) continue;  // 沿用系统语言
        if (const auto lang = LanguageFromTag(value)) SetLanguage(*lang);
    }
}

// ---------------------------------------------------------------------------
// 选项目录（CLI 契约的唯一来源：解析、--help 的 JSON 与文本都由它生成）
// ---------------------------------------------------------------------------

constexpr const wchar_t* kFormatValues[] = {
    L"png", L"jpg", L"jpeg", L"bmp", L"tiff", L"gif", nullptr};

constexpr const wchar_t* kCaptureValues[] = {
    L"wgc", L"dwm", L"printwindow", L"bitblt", L"duplication", L"auto", nullptr};

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
};

constexpr OptionSpec kOptions[] = {
    // ---- 截图目标 ----
    // valueHint 用方括号表示"取值可省略"：省略时下一个参数不会被吞掉，
    // 所以 `ECAPTURE --monitor out.png` 里的 out.png 仍是输出路径。
    {L"monitor", L"m", true, L"target", L"[<n|primary|all>]", nullptr, L"opt.monitor", true},
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
    {L"index", L"i", true, L"pick", L"<n>", nullptr, L"opt.index"},
    {L"newest", L"", false, L"pick", L"", nullptr, L"opt.newest"},
    {L"oldest", L"", false, L"pick", L"", nullptr, L"opt.oldest"},
    {L"all", L"a", false, L"pick", L"", nullptr, L"opt.all"},
    // ---- 取图方式 ----
    {L"capture", L"C", true, L"capture", L"<method>", kCaptureValues, L"opt.capture"},
    // ---- 输出 ----
    {L"out", L"o", true, L"output", L"<path|->", nullptr, L"opt.out"},
    {L"format", L"f", true, L"output", L"<name>", kFormatValues, L"opt.format"},
    {L"quality", L"", true, L"output", L"<1-100>", nullptr, L"opt.quality"},
    {L"no-overwrite", L"", false, L"output", L"", nullptr, L"opt.no-overwrite"},
    // ---- 行为 ----
    {L"dry-run", L"d", false, L"behavior", L"", nullptr, L"opt.dry-run"},
    {L"json", L"j", false, L"behavior", L"", nullptr, L"opt.json"},
    {L"verbose", L"v", false, L"behavior", L"", nullptr, L"opt.verbose"},
    {L"quiet", L"q", false, L"behavior", L"", nullptr, L"opt.quiet"},
    {L"lang", L"l", true, L"behavior", L"<language>", kLangValues, L"opt.lang"},
    {L"help", L"h", false, L"behavior", L"", nullptr, L"opt.help"},
    {L"version", L"", false, L"behavior", L"", nullptr, L"opt.version"},
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
        case MultiMatch::kNewest: return L"newest";
        case MultiMatch::kOldest: return L"oldest";
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
    std::vector<std::wstring> pickFlags;   // --index/--newest/--oldest/--all，互斥
    MultiMatch multiFlag = MultiMatch::kAsk;

    // 单个选项 -> 数据结构
    auto Apply = [&](const OptionSpec& spec, const std::wstring& value) {
        const std::wstring name = spec.name;

        // ---- 截图目标 ----
        if (name == L"monitor") {
            const std::wstring v = ToLower(Trim(value));
            opt.monitor.given = true;
            opt.monitor.all = false;
            opt.monitor.ordinal = 0;
            if (v.empty()) return;                                    // 省略取值 = 主屏
            if (v == L"all") { opt.monitor.all = true; return; }
            if (v == L"primary") return;
            uint64_t n = 0;
            if (!ParseNumber(v, &n) || n == 0 || n > 0xFFFF) {
                Err(codes::kInvalidNumber, Msg(L"cli.monitor_value"), L"--monitor", value,
                    Msg(L"cli.monitor_value_hint"));
                opt.monitor.given = false;
                return;
            }
            opt.monitor.ordinal = static_cast<int>(n);
            return;
        }

        // ---- 匹配条件 ----
        if (name == L"hwnd") {
            uint64_t hwnd = 0;
            if (!ParseNumber(value, &hwnd) || hwnd == 0) {
                Err(codes::kInvalidNumber, Msg(L"cli.hwnd_value"), L"--hwnd", value,
                    Msg(L"cli.hwnd_hint"));
                return;
            }
            PushUnique(&opt.match.hwnds, hwnd, L"--hwnd", &warnings);
            return;
        }
        if (name == L"pid") {
            uint64_t pid = 0;
            if (!ParseNumber(value, &pid) || pid == 0 || pid > 0xFFFFFFFFull) {
                Err(codes::kInvalidNumber, Msg(L"cli.pid_value"), L"--pid", value);
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
            try {
                std::wregex probe(value, std::regex_constants::ECMAScript);
                (void)probe;
            } catch (const std::regex_error& e) {
                std::wstring detail;
                for (const char* p = e.what(); p && *p; ++p)
                    detail.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
                Err(codes::kInvalidRegex,
                    Msgf(L"cli.regex_invalid", static_cast<int>(e.code()), detail), L"--title-regex",
                    value, Msg(L"cli.regex_hint"));
                return;
            }
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
        if (name == L"index") {
            uint64_t n = 0;
            if (!ParseNumber(value, &n) || n == 0 || n > 0xFFFF) {
                Err(codes::kInvalidNumber, Msg(L"cli.index_value"), L"--index", value);
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
            if (!ParseNumber(value, &q) || q < 1 || q > 100) {
                Err(codes::kInvalidNumber, Msg(L"cli.quality_value"), L"--quality", value);
                return;
            }
            opt.jpegQuality = static_cast<int>(q);
            qualityExplicit = true;
            return;
        }
        if (name == L"no-overwrite") { opt.overwrite = false; return; }

        // ---- 行为 ----
        if (name == L"lang") {
            const std::wstring v = Trim(value);
            if (!EqualsInsensitive(v, L"auto")) {
                const auto lang = LanguageFromTag(v);
                if (!lang) {
                    std::wstring list;
                    for (const wchar_t* const* p = kLangValues; *p; ++p) {
                        if (p != kLangValues) list += L", ";
                        list += *p;
                    }
                    Err(codes::kUnknownLanguage, Msg(L"cli.unknown_language"), L"--lang", value, list);
                    return;
                }
                SetLanguage(*lang);
            }
            return;
        }
        if (name == L"dry-run") { opt.dryRun = true; return; }
        if (name == L"json") { opt.json = true; return; }
        if (name == L"verbose") { opt.verbose = true; return; }
        if (name == L"quiet") { opt.quiet = true; return; }
        if (name == L"help") { helpFlag = true; return; }
        if (name == L"version") { versionFlag = true; return; }

        Err(codes::kInvalidValue, Msg(L"cli.unhandled_option"), L"--" + name, value);
    };

    auto RequireValue = [&](const OptionSpec& spec, int& i, const std::optional<std::wstring>& inlineValue,
                            int argcTotal, wchar_t* const* argvTotal) -> std::optional<std::wstring> {
        if (inlineValue) return inlineValue;
        if (i + 1 >= argcTotal) {
            Err(codes::kMissingValue, Msg(L"cli.missing_value"), L"--" + std::wstring(spec.name), L"");
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
            if (!longForm && HasPathSeparator(arg)) {
                positional.push_back(arg);   // 像路径（含 \ 或 /）就当位置参数，其余按误写的选项处理
                continue;
            }
            std::wstring hint;
            if (const auto candidate = ClosestOption(name)) hint = *candidate;
            Err(codes::kUnknownOption, Msg(L"cli.unknown_option"), arg, L"", hint);
            continue;
        }

        if (spec->takesValue) {
            if (spec->optionalValue && !inlineValue) {
                // 取值可省略：只在下一个参数明显就是本选项的取值时才吃掉它，
                // 否则当开关用（--monitor = 主屏），剩下的照常按位置参数处理。
                std::wstring next;
                if (i + 1 < argc) next = Trim(argv[i + 1]);
                if (LooksLikeOptionalValue(std::wstring(spec->name), next)) {
                    Apply(*spec, argv[++i]);
                } else {
                    Apply(*spec, L"");
                }
            } else if (auto value = RequireValue(*spec, i, inlineValue, argc, argv); value) {
                Apply(*spec, *value);
            }
        } else {
            if (inlineValue) {
                bool b = false;
                const std::wstring flag = L"--" + std::wstring(spec->name);
                if (!ParseBool(*inlineValue, &b)) {
                    Err(codes::kSwitchTakesNoValue, Msg(L"cli.switch_no_value"), flag, *inlineValue,
                        Msgf(L"cli.switch_no_value_hint", flag));
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
    std::vector<std::wstring> distinctPick = pickFlags;
    std::sort(distinctPick.begin(), distinctPick.end());
    distinctPick.erase(std::unique(distinctPick.begin(), distinctPick.end()), distinctPick.end());
    if (distinctPick.size() > 1) {
        std::wstring joined;
        for (size_t k = 0; k < distinctPick.size(); ++k) {
            if (k) joined += L", ";
            joined += distinctPick[k];
        }
        Err(codes::kConflictingOptions, Msg(L"cli.conflicting_options"), L"", joined,
            Msg(L"cli.conflicting_hint"));
    }
    opt.multi = distinctPick.empty() ? MultiMatch::kAsk : multiFlag;

    opt.showVersion = versionFlag;
    if (helpFlag) opt.showHelp = true;
    // 只有在没有参数错误、且没显式 --help/--version 时，才因为"零条件"返回帮助。
    // 给了 --monitor 就不算零条件：那是明确的屏幕目标。
    if (result.errors.empty() && !opt.showHelp && !versionFlag && !opt.HasAnyCondition() &&
        !opt.monitor.given) {
        opt.showHelp = true;
        opt.helpReason = codes::kNoCondition;
    }

    // ---- 输出与格式 ----
    if (!opt.showHelp && !versionFlag) {
        // 屏幕目标的两条硬规矩，都在解析期定下来，不留到运行期退化：
        // --monitor all 是"每块屏各一张"，与"按屏过滤窗口"没法同时成立；
        // dwm / printwindow 取的是窗口自己的画面，屏幕上没有这样一个窗口可取。
        if (opt.monitor.given) {
            if (opt.monitor.all && opt.HasAnyCondition()) {
                Err(codes::kMonitorConflict, Msg(L"cli.monitor_conflict"), L"--monitor", L"all",
                    Msg(L"cli.monitor_conflict_hint"));
            }
            if (opt.ScreenMode() && (opt.capture == CaptureMethod::kDwmThumbnail ||
                                     opt.capture == CaptureMethod::kPrintWindow)) {
                Err(codes::kUnsupported, Msgf(L"cap.unsupported_for_screen", CaptureMethodName(opt.capture)),
                    L"--capture", CaptureMethodName(opt.capture),
                    Msg(L"cap.unsupported_for_screen_hint"));
            }
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
        if (opt.verbose && opt.quiet)
            Note(codes::kFlagOverridesQuiet, Msg(L"note.flag_overrides_quiet"), L"--verbose");
    }

    result.ok = result.errors.empty();
    return result;
}

const std::vector<OptionInfo>& OptionCatalog() {
    static const std::vector<OptionInfo> catalog = BuildCatalog();
    return catalog;
}

}  // namespace ecapture
