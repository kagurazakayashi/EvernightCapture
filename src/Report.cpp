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
#include "SystemCompat.h"

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
        // 定位字段：只在这一步真的拿到了值时才出现，不去凑一个"看起来有"的占位
        OptString(j, L"target", d.target);
        OptString(j, L"backend", d.backend);
        OptString(j, L"stage", d.stage);
        OptString(j, L"hresult", d.hresult);
        if (d.win32 != 0) j.Key(L"win32").Value(static_cast<long long>(d.win32));
        j.End();
    }
    j.End();
}

// 一个屏幕矩形：负坐标照写（副屏可以在主屏左边/上边），零宽高整个键省略由调用方判。
void WriteRect(Json& j, const wchar_t* key, const RECT& r) {
    j.Key(key).Obj()
        .Key(L"x").Value(static_cast<long long>(r.left))
        .Key(L"y").Value(static_cast<long long>(r.top))
        .Key(L"width").Value(static_cast<long long>(r.right - r.left))
        .Key(L"height").Value(static_cast<long long>(r.bottom - r.top))
        .End();
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
        OptString(j, L"source", img.source);   // 实际出图的通道，auto 时与请求值不同
        // 来路这三项是隐私判据：这一帧是"窗口自己的画面"还是"屏幕上那块区域"，以及那块区域
        // 在哪。调用方（包括 AI）必须能看到它，--quiet 也不抑制（images 段从来不被抑制）。
        OptString(j, L"path", img.path);
        OptString(j, L"scope", img.scope);
        if (img.rect.right > img.rect.left && img.rect.bottom > img.rect.top) {
            WriteRect(j, L"rect", img.rect);
        }
        // 从整幅桌面帧里裁出目标的通道（duplication、拷屏幕的 bitblt）另外报告定位过程：
        // requestedRect 是本来要截的那一块、capturedRect 是实际截到的那一块、clipped 是两者
        // 不等价、rotation 是交付前把桌面帧顺时针转了多少度。窗口内容路径不写这几个键 ——
        // 它们截的就是整个目标，没有"丢区域"这回事，写一堆 false/0 反而让调用方以为有第二套判断。
        if (img.reportsCrop) {
            WriteRect(j, L"requestedRect", img.requestedRect);
            WriteRect(j, L"capturedRect", img.capturedRect);
            if (img.clipped) j.Key(L"clipped").Value(true);
            if (img.rotation != 0) j.Key(L"rotation").Value(static_cast<long long>(img.rotation));
        }
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
    // 覆盖策略也回显出来：--no-overwrite 的布尔写法（裸写 / =true / =false）只有这样才能
    // 在不截图的情况下被断言（布尔别名一共有十种写法）。
    j.Key(L"overwrite").Value(opt.overwrite);
    // --yes 同理：它是"跳过窗口内容路径的确认"这个决定的最终结果，重复给出时最后一个生效，
    // 断言它不必真的去截一张图（也不必打扰人）。
    j.Key(L"yes").Value(opt.yes);
    // 两条期限也回显：0 = 不设这项期限。断言"参数最终落到什么值"不必真的去等一个超时。
    j.Key(L"timeoutMs").Value(static_cast<long long>(opt.timeoutMs));
    j.Key(L"consentTimeoutMs").Value(static_cast<long long>(opt.consentTimeoutMs));
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
    // 运行环境这两项是"这台机器能走哪几条通道"的答案，与 --capture 请求了什么无关：
    //   osBuild       本机 Windows 内部版本；整个键不出现 = 这一问没成功（问不出来不等于支持，
    //                 也不等于不支持，所以下面那条链这时没有被版本筛过）
    //   captureChain  这一次真正可以试的通道，按尝试顺序（auto 时被版本挡掉的那几条不在里面；
    //                 显式指定且被挡掉时是空数组，而不是偷偷换成别的那一条）
    // 有了这两项，调用方（含 AI）在不必先截图、也不必打扰人的情况下就能判出"这次失败该换后端"
    // 还是"这台机器不行"。
    const OsVersion os = ProbeOsVersion();
    if (os.known) j.Key(L"osBuild").Value(static_cast<long long>(os.build));
    j.Key(L"captureChain").Arr();
    for (const CaptureMethod usable : GateChannels(opt.capture, opt.ScreenMode(), os).chain) {
        j.Value(CaptureMethodName(usable));
    }
    j.End();
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
    if (group == L"consent") return L"grp.consent";
    if (group == L"timeout") return L"grp.timeout";
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
    const wchar_t* groups[] = {L"target", L"match", L"pick", L"capture", L"consent", L"timeout",
                               L"output", L"behavior"};
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
    t += Msg(L"help.system") + L"\r\n";
    // 授权这件事写在选项目录里（grp.consent 那一行 + --yes 的说明），不另立一段正文：
    // 免得同一个规矩在两处各写一遍，改了一处忘了另一处。
    t += L"\r\n";
    t += Msg(L"help.examples") + L"\r\n";
    for (const wchar_t* key : {L"help.example1", L"help.example2", L"help.example3", L"help.example4",
                               L"help.example5", L"help.example6", L"help.example7",
                               L"help.example8"}) {
        t += L"  " + Msg(key) + L"\r\n";
    }
    return t;
}

std::wstring VersionText() {
    // minWindowsBuild 是本工具**对外声明**的最低 Windows 内部版本，与运行时那道能力检查取的是
    // 同一个数（src/SystemCompat.h 的 kSupportedMinBuild），所以脚本不必先读文档就能问出
    // "这个 exe 打算在哪种系统上工作"。它是声明，不是实测范围：实测只有 README
    // 《系统支持》那一节写的那一个版本。
    return std::wstring(L"EvernightCapture ") + kVersion + L"  (ECAPTURE.EXE / x64)  stage=" + kStage +
           L"  minWindowsBuild=" + std::to_wstring(os_floor::kSupportedMinBuild) + L"\r\n";
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

    // 省略 --out 与显式 --out - 是同一件事：真实诊断、退出码与已经交出去的图原样送出，
    // 不再把这一条路上的失败统一换成 cli.missing_output（旧行为会把"人拒绝了""没命中窗口"
    // "写坏了文件"都说成"缺少输出路径"，调用方补上 --out 之后又试一次，白打扰人一遍）。
    // "本次没给输出路径"只在它确实是下一步可执行的那一条上以 hint 出现（见 Capture.cpp 里
    // io.stdout_failed 那处），不作为 code 出现。

    j.Obj();
    j.Key(L"captured").Value(static_cast<long long>(images.size()));
    j.Key(L"images");
    WriteImages(j, images);
    // errors 不受 --quiet 影响：调用方失败时必须能看到原因
    if (!errors.empty()) {
        j.Key(L"errors");
        DiagnosticArray(j, errors);
    }
    // --quiet 只影响 notes：opt.quiet 是解析层定过优先级的最终值（-v 与 -q 同时给出时
    // 已经是 false），所以这里不再判一次"verbose 要不要赢"——两处各判迟早不一致。
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
    // 图片要占用 stdout 时，JSON 改走 stderr，两个通道永不混流。取值与 Main 的兜底路径同源：
    // 半途异常时（见 Main.cpp）也是同一个判断，不会一处说"在 stderr"另一处写到 stdout。
    out->toStderr = ResultGoesToStderr(opt);
    (void)argc;
    (void)argv;
    return code;
}

// ---------------------------------------------------------------------------
// 输出层：UTF-8 直写，绕开 CRT 文本模式；行尾统一 CRLF，避免老式控制台阶梯错位
// ---------------------------------------------------------------------------
namespace {

// 图片写 stdout 的那个时点记在这里：BuildResponse 与 Main 的兜底路径都按同一个事实决定
// 结果送去哪条流，而不是各猜一次给出互相矛盾的答复。
bool g_stdoutClaimed = false;

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

bool EmitStdout(const std::wstring& text) {
    // 图片已占用 stdout 时这里就是硬边界：文字一律改道，绝不让 JSON 混进 PNG 里。
    // 返回 false 表示"没送到约定通道"，由调用方决定退出码（见 Main.cpp）。
    if (StdoutClaimed()) return false;
    return WriteHandle(STD_OUTPUT_HANDLE, text);
}

bool EmitStderrRaw(const std::wstring& text) { return WriteHandle(STD_ERROR_HANDLE, text); }

bool ResultGoesToStderr(const Options& opt) { return opt.output == L"-" || StdoutClaimed(); }

void ClaimStdout() { g_stdoutClaimed = true; }

bool StdoutClaimed() { return g_stdoutClaimed; }

bool EmitStdoutBytes(const std::vector<uint8_t>& bytes, DWORD* ioError) {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        // 句柄本身就取不到：这里 GetLastError 早就不是失败原因了，照实报"句柄无效"
        if (ioError) *ioError = ERROR_INVALID_HANDLE;
        return false;
    }
    // 写第一块字节之前就声明归属：哪怕这次只发出去半张图，stdout 也不能再给文字用。
    ClaimStdout();
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        const BOOL ok = WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr);
        if (!ok || written != chunk) {
            // 立刻取错误码：断管 / 磁盘满 / 句柄失效在这一步是三种不同的故障，
            // 调用方要靠它区分，晚一步就被后续 API 覆盖了。
            if (ioError) *ioError = GetLastError();
            return false;
        }
        offset += written;
    }
    return true;
}

}  // namespace ecapture
