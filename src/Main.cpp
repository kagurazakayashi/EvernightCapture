// EvernightCapture —— CLI 截图工具
//
// 本阶段只做一件事：解析并校验命令行"条件"，然后把解析结果回显出来。
// 窗口枚举与 Windows.Graphics.Capture 截图尚未接入。

#include "CliOptions.h"

#include <sstream>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {
namespace {

using ecapture::Options;

std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring JoinText(const std::vector<std::wstring>& items) {
    std::wstring out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += L", ";
        out += Q(items[i]);
    }
    return out;
}

std::wstring JoinHwnd(const std::vector<uint64_t>& items) {
    std::wstring out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += L", ";
        wchar_t buf[24];
        swprintf(buf, 24, L"0x%llX", static_cast<unsigned long long>(items[i]));
        out += buf;
    }
    return out;
}

std::wstring JoinPid(const std::vector<uint32_t>& items) {
    std::wstring out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out += L", ";
        out += std::to_wstring(items[i]);
    }
    return out;
}

std::wstring AbsoluteOf(const std::wstring& path) {
    if (path.empty() || path == L"-") return path;
    const DWORD cap = 8 * MAX_PATH;
    std::vector<wchar_t> buffer(cap);
    const DWORD n = GetFullPathNameW(path.c_str(), cap, buffer.data(), nullptr);
    if (n == 0 || n >= cap) return L"(无法解析)";
    return std::wstring(buffer.data());
}

const wchar_t* MultiName(MultiMatch m) {
    switch (m) {
        case MultiMatch::kAsk:    return L"报错并要求消歧 (exit 5)";
        case MultiMatch::kIndex:  return L"取第 N 个";
        case MultiMatch::kNewest: return L"取最新窗口";
        case MultiMatch::kOldest: return L"取最早窗口";
        case MultiMatch::kAll:    return L"全部各存一张";
    }
    return L"?";
}

void AddRow(std::vector<std::wstring>* out, const wchar_t* label, const std::wstring& value) {
    if (value.empty()) return;
    std::wstring line = L"  ";
    line += label;
    const size_t width = 22;
    if (line.size() < width) line.append(width - line.size(), L' ');
    line += L": ";
    line += value;
    out->push_back(std::move(line));
}

void PrintWarnings(const ParseResult& r) {
    if (r.options.quiet && !r.options.verbose) return;
    for (const auto& w : r.warnings) PrintLine(L"[提示] " + w, true);
}

void PrintPlain(const Options& opt) {
    std::vector<std::wstring> lines;
    lines.push_back(L"== EvernightCapture 参数解析结果 ==");
    lines.push_back(L"（截图功能尚未实现，本版本只校验并回显条件）");
    lines.push_back(L"");

    const MatchOptions& m = opt.match;
    std::wstringstream head;
    head << L"匹配条件 (" << m.CountGroups() << L" 类 / " << m.CountValues()
         << L" 个值：同类 OR、跨类 AND)";
    lines.push_back(head.str());
    AddRow(&lines, L"--hwnd", JoinHwnd(m.hwnds));
    AddRow(&lines, L"--pid", JoinPid(m.pids));
    AddRow(&lines, L"--process", JoinText(m.processes));
    AddRow(&lines, L"--exe", JoinText(m.exePaths));
    AddRow(&lines, L"--title", JoinText(m.titles));
    AddRow(&lines, L"--title-contains", JoinText(m.titleContains));
    AddRow(&lines, L"--title-regex", JoinText(m.titleRegexes));
    AddRow(&lines, L"--class", JoinText(m.classes));
    lines.push_back(L"");

    std::wstringstream out;
    out << L"输出 : " << (opt.output.empty() ? L"(未指定)" : Q(opt.output)) << L"   ->  "
        << AbsoluteOf(opt.output);
    lines.push_back(out.str());
    lines.push_back(std::wstring(L"  格式 : ") + FormatName(opt.format) +
                    (opt.output == L"-" ? L"   目标 : 标准输出" : L""));
    if (opt.format == ImageFormat::kJpeg)
        lines.push_back(L"  JPEG 质量 : " + std::to_wstring(opt.jpegQuality));
    lines.push_back(std::wstring(L"  覆盖已有文件 : ") +
                    (opt.output == L"-" ? L"不适用（标准输出）"
                                        : (opt.overwrite ? L"是" : L"否 (--no-overwrite)")));
    std::wstringstream multi;
    multi << L"  多窗口策略 : " << MultiName(opt.multi);
    if (opt.multi == MultiMatch::kIndex) multi << L"  (index=" << opt.index << L")";
    lines.push_back(multi.str());
    std::wstringstream flags;
    flags << L"  开关 : dry-run=" << (opt.dryRun ? L"on" : L"off")
          << L"  json=" << (opt.json ? L"on" : L"off")
          << L"  verbose=" << (opt.verbose ? L"on" : L"off")
          << L"  quiet=" << (opt.quiet ? L"on" : L"off");
    lines.push_back(flags.str());

    for (const auto& line : lines) PrintLine(line, false);
}

std::wstring JsonString(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'"') out += L"\\\"";
        else if (c == L'\\') out += L"\\\\";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else if (c == L'\t') out += L"\\t";
        else out += c;
    }
    return L"\"" + out + L"\"";
}

void PrintJson(const Options& opt) {
    std::vector<std::wstring> body;
    auto addList = [&](const std::vector<std::wstring>& v, const wchar_t* key) {
        if (v.empty()) return;
        std::wstring s = L"    \"";
        s += key;
        s += L"\": [";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += L", ";
            s += JsonString(v[i]);
        }
        s += L"]";
        body.push_back(std::move(s));
    };
    const MatchOptions& m = opt.match;
    if (!m.hwnds.empty()) {
        std::wstring s = L"    \"hwnd\": [";
        for (size_t i = 0; i < m.hwnds.size(); ++i) {
            if (i) s += L", ";
            s += std::to_wstring(m.hwnds[i]);
        }
        s += L"]";
        body.push_back(s);
    }
    if (!m.pids.empty()) {
        std::wstring s = L"    \"pid\": [";
        for (size_t i = 0; i < m.pids.size(); ++i) {
            if (i) s += L", ";
            s += std::to_wstring(m.pids[i]);
        }
        s += L"]";
        body.push_back(s);
    }
    addList(m.processes, L"process");
    addList(m.exePaths, L"exe");
    addList(m.titles, L"title");
    addList(m.titleContains, L"titleContains");
    addList(m.titleRegexes, L"titleRegex");
    addList(m.classes, L"class");

    std::vector<std::wstring> out;
    out.push_back(L"{");
    out.push_back(L"  \"tool\": \"EvernightCapture\",");
    out.push_back(L"  \"stage\": \"cli-parsing-only\",");
    out.push_back(L"  \"output\": " + JsonString(AbsoluteOf(opt.output)) + L",");
    out.push_back(L"  \"format\": " + std::wstring(JsonString(FormatName(opt.format))) + L",");
    out.push_back(L"  \"overwrite\": " + std::wstring(opt.overwrite ? L"true" : L"false") + L",");
    out.push_back(L"  \"multiMatch\": " + std::wstring(JsonString(MultiKey(opt.multi))) +
                  (opt.multi == MultiMatch::kIndex ? L", \"index\": " + std::to_wstring(opt.index) + L"," : L","));
    out.push_back(L"  \"match\": {");
    for (size_t i = 0; i < body.size(); ++i)
        out.push_back(body[i] + (i + 1 < body.size() ? L"," : L""));
    out.push_back(L"  }");
    out.push_back(L"}");
    for (const auto& line : out) PrintLine(line, false);
}

void PrintHelp(const std::wstring& help) {
    size_t start = 0;
    while (start <= help.size()) {
        const size_t nl = help.find(L'\n', start);
        std::wstring line = help.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        PrintLine(line, false);
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
}

}  // namespace
}  // namespace ecapture

int wmain(int argc, wchar_t** argv) {
    using namespace ecapture;

    ParseResult r = ParseCommandLine(argc, argv);

    if (!r.ok) {
        for (const auto& e : r.errors) PrintLine(L"错误: " + e, true);
        for (const auto& s : r.suggestions) PrintLine(L"提示: " + s, true);
        PrintWarnings(r);
        PrintLine(L"", true);
        PrintLine(L"用法: ECAPTURE.EXE [条件...] <输出路径>      （ECAPTURE.EXE --help 查看完整帮助）", true);
        return EX_USAGE;
    }

    PrintWarnings(r);
    const Options& opt = r.options;

    if (opt.showVersion) {
        PrintHelp(VersionText());
        return EX_OK;
    }

    if (opt.showHelp) {
        if (!opt.helpReason.empty()) PrintLine(opt.helpReason + L"，因此显示帮助。", true);
        PrintHelp(HelpText());
        return opt.helpReason.empty() ? EX_HELP : EX_NO_CONDITION;
    }

    if (opt.json) PrintJson(opt);
    else PrintPlain(opt);
    return EX_OK;
}
