#include "CliOptions.h"

#include <algorithm>
#include <cwctype>
#include <iterator>
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

// ---------------------------------------------------------------------------
// 选项目录（CLI 契约的唯一来源：解析、--help 的 JSON 与文本都由它生成）
// ---------------------------------------------------------------------------

constexpr const wchar_t* kFormatValues[] = {
    L"auto", L"png", L"jpg", L"jpeg", L"bmp", L"tiff", L"gif", L"webp", L"ico", nullptr};

constexpr const wchar_t* kCaptureValues[] = {
    L"wgc", L"dwm", L"printwindow", L"bitblt", L"duplication", L"magnification", L"auto", nullptr};

struct OptionSpec {
    const wchar_t* name;        // 规范名（不含前导 -）
    const wchar_t* shortName;   // 单字母别名，可为空
    bool takesValue;            // false = 开关
    const wchar_t* group;       // match / pick / output / behavior
    const wchar_t* valueHint;   // 人读的取值占位符，开关为空
    const wchar_t* const* allowed;    // 枚举取值，nullptr 结尾；没有则 nullptr
    const wchar_t* description;
};

constexpr OptionSpec kOptions[] = {
    // ---- 窗口匹配条件（同类 OR，跨类 AND）----
    {L"hwnd", L"", true, L"match", L"<handle>", nullptr,
     L"窗口句柄。纯数字按十进制，0x 前缀或含 a-f 按十六进制；推荐写 0x"},
    {L"pid", L"", true, L"match", L"<pid>", nullptr,
     L"进程 ID，十进制且大于 0"},
    {L"process", L"p", true, L"match", L"<image-name>", nullptr,
     L"映像文件名（不含路径），忽略大小写；无扩展名时按 .exe 处理"},
    {L"exe", L"", true, L"match", L"<full-path>", nullptr,
     L"映像完整路径，忽略大小写"},
    {L"title", L"t", true, L"match", L"<exact-title>", nullptr,
     L"窗口标题精确匹配"},
    {L"title-contains", L"T", true, L"match", L"<text>", nullptr,
     L"窗口标题包含子串"},
    {L"title-regex", L"R", true, L"match", L"<regex>", nullptr,
     L"窗口标题正则匹配，ECMAScript 语法，解析期即校验"},
    {L"class", L"c", true, L"match", L"<class-name>", nullptr,
     L"窗口类名，忽略大小写，如 Notepad / CabinetWClass"},
    // ---- 匹配到多个窗口时的选择策略（互斥）----
    {L"index", L"i", true, L"pick", L"<n>", nullptr,
     L"取第 n 个窗口，从 1 开始，按可见性/叠放次序排序"},
    {L"newest", L"", false, L"pick", L"", nullptr, L"取最后创建的窗口"},
    {L"oldest", L"", false, L"pick", L"", nullptr, L"取最早创建的窗口"},
    {L"all", L"a", false, L"pick", L"", nullptr, L"每个匹配窗口各存一张"},
    // ---- 取图方式 ----
    {L"capture", L"C", true, L"capture", L"<method>", kCaptureValues,
     L"wgc(默认，被遮挡也能截) / dwm(DWM 缩略图) / printwindow(窗口自绘) / "
     L"bitblt(拷屏幕可见像素) / duplication(DXGI 桌面复制) / magnification(放大镜 API) / "
     L"auto(按 wgc-dwm-printwindow-bitblt 回退)"},
    // ---- 输出 ----
    {L"out", L"o", true, L"output", L"<path|->", nullptr,
     L"输出路径；特殊值 - 表示把图片字节写到标准输出。也可用位置参数"},
    {L"format", L"f", true, L"output", L"<name>", kFormatValues,
     L"强制编码格式，默认由输出文件扩展名推断"},
    {L"quality", L"", true, L"output", L"<1-100>", nullptr, L"JPEG 质量，默认 90"},
    {L"no-overwrite", L"", false, L"output", L"", nullptr, L"目标已存在时不覆盖，报错退出"},
    // ---- 行为 ----
    {L"dry-run", L"d", false, L"behavior", L"", nullptr,
     L"只解析并列出候选窗口，不截图不写文件"},
    {L"json", L"j", false, L"behavior", L"", nullptr,
     L"已废弃的兼容开关，无副作用：成功与错误本来就输出 JSON"},
    {L"verbose", L"v", false, L"behavior", L"", nullptr,
     L"JSON 中追加 input 段（规范化后的全部输入），并保留 notes"},
    {L"quiet", L"q", false, L"behavior", L"", nullptr,
     L"省略 notes；errors 无论如何都会返回"},
    {L"help", L"h", false, L"behavior", L"", nullptr, L"输出文本帮助（本段）"},
    {L"version", L"", false, L"behavior", L"", nullptr, L"输出版本与阶段"},
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
        info.description = spec.description;
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

const wchar_t* CaptureMethodName(CaptureMethod m) {
    switch (m) {
        case CaptureMethod::kWgc: return L"wgc";
        case CaptureMethod::kDwmThumbnail: return L"dwm";
        case CaptureMethod::kPrintWindow: return L"printwindow";
        case CaptureMethod::kBitBlt: return L"bitblt";
        case CaptureMethod::kDuplication: return L"duplication";
        case CaptureMethod::kMagnification: return L"magnification";
        case CaptureMethod::kAuto: return L"auto";
    }
    return L"?";
}

const wchar_t* CaptureMethodDescription(CaptureMethod m) {
    switch (m) {
        case CaptureMethod::kWgc:
            return L"Windows.Graphics.Capture：取 DWM 合成后的窗口面，被遮挡/在后台也能截";
        case CaptureMethod::kDwmThumbnail:
            return L"DwmRegisterThumbnail：DWM 缓存表面，可截被遮挡窗口，带合成效果";
        case CaptureMethod::kPrintWindow:
            return L"PrintWindow(PW_RENDERFULLCONTENT)：让窗口自绘到 DC，硬件加速内容常为黑";
        case CaptureMethod::kBitBlt:
            return L"BitBlt 屏幕 DC：拷屏幕上该窗口矩形，只能拿到当前可见部分";
        case CaptureMethod::kDuplication:
            return L"DXGI Desktop Duplication：抓显示器合成分后按窗口矩形裁，须可见";
        case CaptureMethod::kMagnification:
            return L"Magnification API：系统放大镜的取图通道，抓合成后画面";
        case CaptureMethod::kAuto:
            return L"按 wgc -> dwm -> printwindow -> bitblt 依次回退，取第一个成功的";
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
                                    std::wstring(L"同一选项内重复的取值已忽略") + label,
                                    label, ToDisplay(value), L""});
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

        // ---- 匹配条件 ----
        if (name == L"hwnd") {
            uint64_t hwnd = 0;
            if (!ParseNumber(value, &hwnd) || hwnd == 0) {
                Err(codes::kInvalidNumber,
                    L"--hwnd 需要有效的句柄值（十进制，或带 0x 前缀的十六进制）", L"--hwnd", value,
                    L"纯数字按十进制解析；十六进制请写成 0x……，或含 a-f 时自动按十六进制");
                return;
            }
            PushUnique(&opt.match.hwnds, hwnd, L"--hwnd", &warnings);
            return;
        }
        if (name == L"pid") {
            uint64_t pid = 0;
            if (!ParseNumber(value, &pid) || pid == 0 || pid > 0xFFFFFFFFull) {
                Err(codes::kInvalidNumber, L"--pid 需要有效的进程 ID（十进制，> 0）", L"--pid", value);
                return;
            }
            PushUnique(&opt.match.pids, static_cast<uint32_t>(pid), L"--pid", &warnings);
            return;
        }
        if (name == L"process") {
            const std::wstring v = Trim(value);
            if (v.empty()) { Err(codes::kInvalidValue, L"--process 不能为空", L"--process", value); return; }
            if (HasPathSeparator(v)) {
                Err(codes::kInvalidValue,
                    L"--process 只接受映像文件名（例如 notepad.exe），完整路径请用 --exe", L"--process", v,
                    L"--exe \"D:\\App\\notepad.exe\"");
                return;
            }
            PushUnique(&opt.match.processes, v, L"--process", &warnings);
            if (v.find(L'.') == std::wstring::npos)
                Note(codes::kExtensionAppended, L"--process 取值没有扩展名，匹配时按 .exe 处理",
                     L"--process", v + L".exe");
            return;
        }
        if (name == L"exe") {
            const std::wstring v = Trim(value);
            if (v.empty()) { Err(codes::kInvalidValue, L"--exe 不能为空", L"--exe", value); return; }
            if (!HasPathSeparator(v))
                Note(codes::kExeLooksLikeName,
                     L"--exe 取值不含路径分隔符，看起来只是文件名；若只要文件名请改用 --process", L"--exe", v);
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
                std::wstringstream ss;
                ss << L"--title-regex 正则表达式无效 (regex_error code=" << static_cast<int>(e.code())
                   << L"): " << detail;
                Err(codes::kInvalidRegex, ss.str(), L"--title-regex", value,
                    L"ECMAScript 语法；量词前要有可重复项，字符类里的 [ 需要配对");
                return;
            }
            PushUnique(&opt.match.titleRegexes, value, L"--title-regex", &warnings);
            return;
        }
        if (name == L"class") {
            const std::wstring v = Trim(value);
            if (v.empty()) { Err(codes::kInvalidValue, L"--class 不能为空", L"--class", value); return; }
            PushUnique(&opt.match.classes, v, L"--class", &warnings);
            return;
        }

        // ---- 多窗口选择策略 ----
        if (name == L"index") {
            uint64_t n = 0;
            if (!ParseNumber(value, &n) || n == 0 || n > 0xFFFF) {
                Err(codes::kInvalidNumber, L"--index 是从 1 开始的序号", L"--index", value);
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
                Err(codes::kUnknownCaptureMethod, L"--capture 取值不在允许列表内", L"--capture", value, list);
                return;
            }
            if (v == L"wgc") opt.capture = CaptureMethod::kWgc;
            else if (v == L"dwm") opt.capture = CaptureMethod::kDwmThumbnail;
            else if (v == L"printwindow") opt.capture = CaptureMethod::kPrintWindow;
            else if (v == L"bitblt") opt.capture = CaptureMethod::kBitBlt;
            else if (v == L"duplication") opt.capture = CaptureMethod::kDuplication;
            else if (v == L"magnification") opt.capture = CaptureMethod::kMagnification;
            else opt.capture = CaptureMethod::kAuto;
            opt.captureExplicit = true;
            return;
        }

        // ---- 输出 ----
        if (name == L"out") {
            if (outExplicit)
                Err(codes::kDuplicateOutput, L"重复的 --out，输出路径只能有一个", L"--out", value);
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
                Err(codes::kInvalidFormat, L"--format 取值不在允许列表内", L"--format", value, allowed);
                return;
            }
            opt.format = *f;
            formatExplicit = true;
            return;
        }
        if (name == L"quality") {
            uint64_t q = 0;
            if (!ParseNumber(value, &q) || q < 1 || q > 100) {
                Err(codes::kInvalidNumber, L"--quality 需要在 1..100 之间", L"--quality", value);
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

        Err(codes::kInvalidValue, L"内部错误：目录中存在但解析器未处理的选项", L"--" + name, value);
    };

    auto RequireValue = [&](const OptionSpec& spec, int& i, const std::optional<std::wstring>& inlineValue,
                            int argcTotal, wchar_t* const* argvTotal) -> std::optional<std::wstring> {
        if (inlineValue) return inlineValue;
        if (i + 1 >= argcTotal) {
            Err(codes::kMissingValue, L"该选项需要一个取值", L"--" + std::wstring(spec.name), L"");
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
            Err(codes::kUnknownOption, L"未知选项", arg, L"", hint);
            continue;
        }

        if (spec->takesValue) {
            if (auto value = RequireValue(*spec, i, inlineValue, argc, argv); value) Apply(*spec, *value);
        } else {
            if (inlineValue) {
                bool b = false;
                const std::wstring flag = L"--" + std::wstring(spec->name);
                if (!ParseBool(*inlineValue, &b)) {
                    Err(codes::kSwitchTakesNoValue, L"该开关不接受取值，直接写选项名即可", flag, *inlineValue,
                        flag + L"（开）或 " + flag + L"=false（关）");
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
        Err(codes::kUnexpectedPositional,
            L"多余的位置参数：只有第一个位置参数当作输出路径，其余条件请用选项给出", L"", extra);
    }
    if (!positional.empty() && outExplicit)
        Err(codes::kDuplicateOutput, L"输出路径重复：--out 与位置参数只能给一个", L"--out", positional[0]);

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
        Err(codes::kConflictingOptions, L"这些选择策略互斥，只能选一个", L"", joined,
            L"例如只保留 --index 2");
    }
    opt.multi = distinctPick.empty() ? MultiMatch::kAsk : multiFlag;

    opt.showVersion = versionFlag;
    if (helpFlag) opt.showHelp = true;
    // 只有在没有参数错误、且没显式 --help/--version 时，才因为"零条件"返回帮助
    if (result.errors.empty() && !opt.showHelp && !versionFlag && !opt.HasAnyCondition()) {
        opt.showHelp = true;
        opt.helpReason = codes::kNoCondition;
    }

    // ---- 输出与格式 ----
    if (!opt.showHelp && !versionFlag) {
        if (opt.output.empty()) {
            Err(codes::kMissingOutput,
                L"缺少输出路径：把它作为最后一个位置参数，或用 --out <路径>（`-` 表示写到标准输出）");
        } else if (opt.output != L"-") {
            const std::wstring ext = ExtensionOf(opt.output);
            const auto fromExt = FormatFromExtension(ext);
            if (formatExplicit && fromExt && *fromExt != opt.format) {
                Note(codes::kFormatExtensionMismatch,
                     std::wstring(L"--format 与文件扩展名不一致，按 --format 编码") +
                         L"（文件名保持不变）", L"--format",
                     std::wstring(FormatName(opt.format)) + L" vs ." + ext);
            } else if (!formatExplicit && fromExt) {
                opt.format = *fromExt;
            } else if (!formatExplicit && !fromExt) {
                Err(codes::kUnrecognizedExtension,
                    L"无法从输出文件名确定图片格式：加已知扩展名，或用 --format 指定", L"--out",
                    opt.output, L"png / jpg / bmp / tiff / gif / webp / ico");
            }
        } else if (!formatExplicit) {
            opt.format = ImageFormat::kPng;
            Note(codes::kPipeDefaultFormat, L"输出到标准输出且未指定 --format，按 png 编码", L"--out", L"-");
        }

        if (qualityExplicit && opt.format != ImageFormat::kJpeg)
            Note(codes::kQualityIgnored, L"--quality 只对 jpeg 生效，当前格式下会被忽略", L"--quality",
                 std::wstring(FormatName(opt.format)));
        if (opt.multi == MultiMatch::kAll && opt.output != L"-" && !ContainsPlaceholder(opt.output))
            Note(codes::kAllWithoutPlaceholder,
                 L"--all 且输出名没有占位符时，会在文件名后自动追加序号，可能不符合预期", L"--all",
                 opt.output, L"例如 shot_%i_%h.png");
        if (opt.json)
            Note(codes::kJsonFlagDeprecated, L"输出恒为 JSON，--json 已成为无副作用的兼容开关", L"--json");
        if (opt.verbose && opt.quiet)
            Note(codes::kFlagOverridesQuiet, L"--verbose 与 --quiet 同时给出时按 --verbose 处理", L"--verbose");
    }

    result.ok = result.errors.empty();
    return result;
}

const std::vector<OptionInfo>& OptionCatalog() {
    static const std::vector<OptionInfo> catalog = BuildCatalog();
    return catalog;
}

const wchar_t* MultiDescription(MultiMatch m) {
    switch (m) {
        case MultiMatch::kAsk: return L"匹配到多个窗口时报错并列出候选";
        case MultiMatch::kIndex: return L"取第 N 个";
        case MultiMatch::kNewest: return L"取最后创建的窗口";
        case MultiMatch::kOldest: return L"取最早创建的窗口";
        case MultiMatch::kAll: return L"每个窗口各存一张";
    }
    return L"?";
}

}  // namespace ecapture
