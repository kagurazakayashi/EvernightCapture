#include "Report.h"

#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Json.h"

namespace ecapture {
namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::wstring HwndHex(uint64_t hwnd) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%08llX", static_cast<unsigned long long>(hwnd));
    return buf;
}

std::wstring AbsoluteOf(const std::wstring& path) {
    if (path.empty() || path == L"-") return path;
    const DWORD cap = 8 * MAX_PATH;
    std::vector<wchar_t> buffer(cap);
    const DWORD n = GetFullPathNameW(path.c_str(), cap, buffer.data(), nullptr);
    if (n == 0 || n >= cap) return std::wstring();
    return std::wstring(buffer.data());
}

void StrArray(Json& j, const std::vector<std::wstring>& items) {
    j.Arr();
    for (const auto& s : items) j.Value(s);
    j.End();
}

void OptString(Json& j, const wchar_t* key, const std::wstring& value) {
    if (value.empty()) return;   // 精简契约：没内容就不出现这个键
    j.Key(key).Value(value);
}

// 空字段一律省略，调用方按存在与否取值
void DiagnosticArray(Json& j, const std::vector<Diagnostic>& items) {
    j.Arr();
    for (const auto& d : items) {
        j.Obj();
        j.Key(L"code").Value(d.code);
        OptString(j, L"message", d.message);
        OptString(j, L"option", d.option);
        OptString(j, L"value", d.value);
        OptString(j, L"hint", d.hint);
        j.End();
    }
    j.End();
}

// ---------------------------------------------------------------------------
// images：捕获结果。截图尚未实现，因此这里只有字段定义，实际总是空数组。
// 一旦实现，每条按下面的键写入（不存在的键省略）。
// ---------------------------------------------------------------------------
void WriteImages(Json& j) {
    j.Arr();
    j.End();
}

// ---------------------------------------------------------------------------
// input：--verbose 才输出的规范化输入，用于排查"程序到底理解了什么"
// ---------------------------------------------------------------------------
void WriteInputEcho(Json& j, const Options& opt) {
    const MatchOptions& m = opt.match;
    j.Obj();
    j.Key(L"hwnd").Arr();
    for (uint64_t h : m.hwnds) {
        j.Obj().Key(L"hex").Value(HwndHex(h)).Key(L"decimal").Value(h).End();
    }
    j.End();
    j.Key(L"pid").Arr();
    for (uint32_t p : m.pids) j.Value(static_cast<long long>(p));
    j.End();
    j.Key(L"process"); StrArray(j, m.processes);
    j.Key(L"exe");     StrArray(j, m.exePaths);
    j.Key(L"title");   StrArray(j, m.titles);
    j.Key(L"titleContains"); StrArray(j, m.titleContains);
    j.Key(L"titleRegex");    StrArray(j, m.titleRegexes);
    j.Key(L"class");         StrArray(j, m.classes);

    OptString(j, L"output", AbsoluteOf(opt.output));
    j.Key(L"toStdout").Value(opt.output == L"-");
    j.Key(L"format").Value(FormatName(opt.format));
    j.Key(L"policy").Value(MultiKey(opt.multi));
    if (opt.multi == MultiMatch::kIndex) j.Key(L"index").Value(opt.index);
    j.End();
}

const wchar_t* GroupTitle(const std::wstring& group) {
    if (group == L"match") return L"窗口匹配条件（同一选项多次出现取并集，不同选项必须同时命中）";
    if (group == L"pick") return L"匹配到多个窗口时（互斥）";
    if (group == L"output") return L"输出";
    return L"其它";
}

std::wstring FlagColumn(const OptionInfo& o) {
    std::wstring s = L"--" + o.name;
    if (!o.shortName.empty()) s += L", -" + o.shortName;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯文本帮助：由选项目录生成，所以不会和实现脱节
// ---------------------------------------------------------------------------
std::wstring HelpText() {
    std::wstring t;
    t += L"EvernightCapture (ECAPTURE.EXE) —— 按条件窗口截图，基于 Windows.Graphics.Capture\r\n";
    t += L"\r\n";
    t += L"用法: ECAPTURE.EXE [条件...] <输出路径>        不给任何条件 => 显示本帮助\r\n";
    t += L"      ECAPTURE.EXE [条件...] --out <路径>      路径写 - 表示把图片字节输出到标准输出\r\n";
    t += L"\r\n";

    const auto& catalog = OptionCatalog();
    const wchar_t* groups[] = {L"match", L"pick", L"output", L"behavior"};
    size_t width = 0;
    for (const auto& o : catalog) {
        std::wstring col = FlagColumn(o);
        if (!o.valueHint.empty()) col += L" " + o.valueHint;
        width = std::max(width, col.size());
    }
    for (const wchar_t* g : groups) {
        t += GroupTitle(g);
        t += L"\r\n";
        for (const auto& o : catalog) {
            if (o.group != g) continue;
            std::wstring col = FlagColumn(o);
            if (!o.valueHint.empty()) col += L" " + o.valueHint;
            col = L"  " + col;
            col.append(width + 3 - col.size(), L' ');
            t += col + o.description + L"\r\n";
        }
        t += L"\r\n";
    }

    t += L"写法: --opt=value / -opt / /opt 都接受；取值本身以 - 开头时写成 --title=-x，或用 -- 结束选项解析\r\n";
    t += L"输出: 成功与错误都是 JSON，只含 captured / images（另有 errors / notes，--verbose 才有 input）\r\n";
    t += L"      --help / --version 以及不给条件时是文本\r\n";
    t += L"退出码: 0 成功 / 1 参数错 / 2 未给条件 / 3 --help / 4 无匹配窗口 / 5 匹配多个窗口 /\r\n";
    t += L"        6 目标受保护 / 7 截图失败 / 8 写文件失败 / 9 内部异常\r\n";
    t += L"当前构建: 只完成参数解析，所以 captured 恒为 0、images 恒为空数组，不会写出任何图片\r\n";
    t += L"\r\n";
    t += L"示例:\r\n";
    t += L"  ECAPTURE.EXE --process notepad.exe D:\\shots\\epad.png\r\n";
    t += L"  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\\shots\\game.png\r\n";
    t += L"  ECAPTURE.EXE --pid 12345 --title-contains 报告 --all D:\\shots\\rpt_%i.png\r\n";
    t += L"  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png\r\n";
    t += L"  ECAPTURE.EXE --process notepad.exe --out - > snap.png\r\n";
    return t;
}

std::wstring VersionText() {
    return std::wstring(L"EvernightCapture ") + kVersion + L"  (ECAPTURE.EXE / x64)  stage=" + kStage +
           L"\r\n";
}

// ---------------------------------------------------------------------------
// 响应组装
// ---------------------------------------------------------------------------
int BuildResponse(const ParseResult& parse, int argc, wchar_t* const* argv, Response* out) {
    const Options& opt = parse.options;
    out->body.clear();
    out->toStderr = false;

    if (opt.showVersion) {
        out->body = VersionText();
        out->exitCode = EX_OK;
        return EX_OK;
    }
    if (opt.showHelp) {
        if (!opt.helpReason.empty())
            out->body += L"未指定任何匹配条件，因此显示帮助。\r\n\r\n";
        out->body += HelpText();
        const int code = opt.helpReason.empty() ? EX_HELP : EX_NO_CONDITION;
        out->exitCode = code;
        return code;
    }

    Json j;
    std::vector<Diagnostic> errors = parse.errors;
    std::vector<Diagnostic> notes = parse.warnings;

    j.Obj();
    j.Key(L"captured").Value(0);          // 截图未实现，恒为 0
    j.Key(L"images");
    WriteImages(j);
    // errors 不受 --quiet 影响：调用方失败时必须能看到原因
    if (!errors.empty()) {
        j.Key(L"errors");
        DiagnosticArray(j, errors);
    }
    if (!notes.empty() && !opt.quiet) {
        j.Key(L"notes");
        DiagnosticArray(j, notes);
    }
    if (opt.verbose) {
        j.Key(L"input");
        WriteInputEcho(j, opt);
    }
    j.End();
    out->body = j.Str();

    const int code = parse.ok ? EX_OK : EX_USAGE;
    out->exitCode = code;
    // 图片要占用 stdout 时，JSON 改走 stderr，两个通道永不混流
    out->toStderr = (opt.output == L"-");
    (void)argc;
    (void)argv;
    return code;
}

// ---------------------------------------------------------------------------
// 输出层：UTF-8 直写，绕开 CRT 文本模式；行尾统一 CRLF，避免老式控制台阶梯错位
// ---------------------------------------------------------------------------
namespace {

std::wstring NormalizeNewlines(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'\r' && i + 1 < in.size() && in[i + 1] == L'\n') continue;  // 已有 CRLF
        if (in[i] == L'\n') out += L"\r\n";
        else out.push_back(in[i]);
    }
    return out;
}

bool WriteHandle(DWORD which, const std::wstring& text) {
    HANDLE handle = GetStdHandle(which);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    const std::wstring body = NormalizeNewlines(text);
    const int need = WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    std::vector<char> buf(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()), buf.data(), need,
                        nullptr, nullptr);
    DWORD written = 0;
    return WriteFile(handle, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == static_cast<DWORD>(buf.size());
}

}  // namespace

bool EmitStdout(const std::wstring& text) { return WriteHandle(STD_OUTPUT_HANDLE, text); }

bool EmitStderrRaw(const std::wstring& text) { return WriteHandle(STD_ERROR_HANDLE, text); }

}  // namespace ecapture
