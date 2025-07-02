#include "Report.h"

#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Json.h"
#include "Capture.h"
#include "Lang.h"

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
// images：每条 = 一次成功捕获。空字段省略。
// ---------------------------------------------------------------------------
void WriteImages(Json& j, const std::vector<CapturedImage>& images) {
    j.Arr();
    for (const auto& img : images) {
        j.Obj();
        OptString(j, L"file", img.file);
        j.Key(L"bytes").Value(static_cast<long long>(img.bytes));
        j.Key(L"width").Value(static_cast<long long>(img.width));
        j.Key(L"height").Value(static_cast<long long>(img.height));
        OptString(j, L"format", img.format);
        if (img.screen) {
            // 屏幕目标没有窗口可归属：给屏幕信息，窗口那几个键整个不出现
            j.Key(L"monitor").Value(static_cast<long long>(img.monitorOrdinal));
            OptString(j, L"device", img.deviceName);
            j.Key(L"primary").Value(img.primary);
        } else {
            OptString(j, L"hwnd", img.hwndHex);
            j.Key(L"pid").Value(static_cast<long long>(img.pid));
            OptString(j, L"title", img.title);
            OptString(j, L"class", img.windowClass);
            OptString(j, L"image", img.imageName);
        }
        j.Key(L"elapsedMs").Value(static_cast<long long>(img.elapsedMs));
        j.End();
    }
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
    if (opt.monitor.given) {
        if (opt.monitor.all) j.Key(L"monitor").Value(L"all");
        else if (opt.monitor.ordinal == 0) j.Key(L"monitor").Value(L"primary");
        else j.Key(L"monitor").Value(opt.monitor.ordinal);
        j.Key(L"target").Value(opt.ScreenMode() ? L"screen" : L"window");
    }
    j.Key(L"format").Value(FormatName(opt.format));
    j.Key(L"formatGiven").Value(opt.formatExplicit);
    j.Key(L"capture").Value(CaptureMethodName(opt.capture));
    j.Key(L"captureGiven").Value(opt.captureExplicit);
    j.Key(L"lang").Value(LanguageTag(CurrentLanguage()));
    j.Key(L"policy").Value(MultiKey(opt.multi));
    if (opt.multi == MultiMatch::kIndex) j.Key(L"index").Value(opt.index);
    j.End();
}

const wchar_t* GroupTitle(const std::wstring& group) {
    if (group == L"target") return L"grp.target";
    if (group == L"match") return L"grp.match";
    if (group == L"pick") return L"grp.pick";
    if (group == L"capture") return L"grp.capture";
    if (group == L"output") return L"grp.output";
    return L"grp.behavior";
}

std::wstring FlagColumn(const OptionInfo& o) {
    std::wstring s = L"--" + o.name;
    if (!o.shortName.empty()) s += L", -" + o.shortName;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯文本帮助：骨架由选项目录生成，所以不会和实现脱节；每一句文案都取自资源（见 Lang.h），
// 换行与缩进写在文案里，好让每种语言自己安排对齐。
// ---------------------------------------------------------------------------
std::wstring HelpText() {
    std::wstring t;
    t += Msg(L"help.header") + L"\r\n";
    t += L"\r\n";
    t += Msg(L"help.usage1") + L"\r\n";
    t += Msg(L"help.usage2") + L"\r\n";
    t += Msg(L"help.usage3") + L"\r\n";
    t += Msg(L"help.usage4") + L"\r\n";
    t += L"\r\n";

    const auto& catalog = OptionCatalog();
    const wchar_t* groups[] = {L"target", L"match", L"pick", L"capture", L"output", L"behavior"};
    size_t width = 0;
    for (const auto& o : catalog) {
        std::wstring col = FlagColumn(o);
        if (!o.valueHint.empty()) col += L" " + o.valueHint;
        width = std::max(width, col.size());
    }
    for (const wchar_t* g : groups) {
        t += Msg(GroupTitle(g));
        t += L"\r\n";
        for (const auto& o : catalog) {
            if (o.group != g) continue;
            std::wstring col = FlagColumn(o);
            if (!o.valueHint.empty()) col += L" " + o.valueHint;
            col = L"  " + col;
            col.append(width + 3 - col.size(), L' ');
            t += col + Msg(o.messageKey.c_str()) + L"\r\n";
        }
        t += L"\r\n";
    }

    t += Msg(L"help.syntax") + L"\r\n";
    t += Msg(L"help.output1") + L"\r\n";
    t += Msg(L"help.output2") + L"\r\n";
    t += Msg(L"help.exit1") + L"\r\n";
    t += Msg(L"help.exit2") + L"\r\n";
    t += Msg(L"help.status") + L"\r\n";
    t += L"\r\n";
    t += Msg(L"help.examples") + L"\r\n";
    for (const wchar_t* key : {L"help.example1", L"help.example2", L"help.example3", L"help.example4",
                               L"help.example5", L"help.example6"}) {
        t += L"  " + Msg(key) + L"\r\n";
    }
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
            out->body += Msg(L"help.no_condition") + L"\r\n\r\n";
        out->body += HelpText();
        const int code = opt.helpReason.empty() ? EX_HELP : EX_NO_CONDITION;
        out->exitCode = code;
        return code;
    }

    Json j;
    std::vector<Diagnostic> errors = parse.errors;
    std::vector<Diagnostic> notes = parse.warnings;
    std::vector<CapturedImage> images;
    int code = parse.ok ? EX_OK : EX_USAGE;

    if (parse.ok) {
        // --capture 的取值在解析期就已限定为已实现的通道，这里不再做能力判断
        CaptureOutcome outcome = RunCapture(opt);
        images = std::move(outcome.images);
        for (auto& e : outcome.errors) errors.push_back(std::move(e));
        for (auto& n : outcome.notes) notes.push_back(std::move(n));
        code = outcome.exitCode;
    }

    // 没给输出路径时图片字节已经占了 stdout，这条"偷懒路径"失败就只回一条
    // cli.missing_output：让调用方补 --out 比堆一串原因更有用（用户明确要求）。
    if (opt.outputImplicitStdout && code != EX_OK) {
        images.clear();
        notes.clear();
        errors = {Diagnostic{codes::kMissingOutput, Msg(L"cli.missing_output"), L"--out", L"-",
                             Msg(L"cli.missing_output_hint")}};
        code = EX_USAGE;
    }

    j.Obj();
    j.Key(L"captured").Value(static_cast<long long>(images.size()));
    j.Key(L"images");
    WriteImages(j, images);
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

bool EmitStdoutBytes(const std::vector<uint8_t>& bytes) {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk)
            return false;
        offset += written;
    }
    return true;
}

}  // namespace ecapture
