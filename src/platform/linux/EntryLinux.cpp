// 【平台层 · Linux】ECAPTURE 的 Linux 入口（只读查询 + 明确的未实现拒绝）。
//
// Linux 采集后端还没有接上，但"在截图之前先问清这台机器能做什么"不能等到后端齐了才有。
// 这个入口因此只交付与桌面像素无关的三类命令：
//   * --help / --version / --show-config
//   * --capabilities / --diagnostics（会话识别 + 运行能力查询）
// 其余截图 / 窗口查询 / 屏幕枚举一律交回 capture.unsupported + 7，并在 hint 里说明
// 这是后端未实现，不是"目标不对"或"换一条通道再试"。它绝不返回空列表冒充成功，
// 也不因为 X11 连得上就回答"可以截图"。
//
// 入口只做两件事：把 argv 按 UTF-8 严格解码成 CliTokens（唯一一次换码），以及把响应送到
// 约定的流。其余判据都在共用层。

#include <exception>
#include <string>
#include <vector>

#include "CliArgs.h"
#include "CliOptions.h"
#include "EnvReport.h"
#include "Json.h"
#include "Lang.h"
#include "OutputStream.h"
#include "Report.h"   // Response 结构（平台无关）
#include "core/TextCoding.h"

namespace ecapture {
namespace {

bool GoesToStderr(const Options& opt) { return opt.output == L"-" || StdoutClaimed(); }

void WriteDiagnostics(Json& j, const std::vector<Diagnostic>& items) {
    j.Arr();
    for (const auto& d : items) {
        j.Obj();
        j.Key(L"code").Value(d.code);
        OptString(j, L"message", d.message);
        OptString(j, L"option", d.option);
        OptString(j, L"value", d.value);
        OptString(j, L"hint", d.hint);
        OptString(j, L"target", d.target);
        OptString(j, L"backend", d.backend);
        OptString(j, L"stage", d.stage);
        if (d.line != 0) j.Key(L"line").Value(static_cast<long long>(d.line));
        OptString(j, L"detail", d.detail);
        j.End();
    }
    j.End();
}

std::wstring ErrorDocument(const std::vector<Diagnostic>& errors,
                           const std::vector<Diagnostic>& notes, bool quiet) {
    Json j;
    j.Obj();
    j.Key(L"captured").Value(0);
    j.Key(L"images").Arr().End();
    if (!errors.empty()) {
        j.Key(L"errors");
        WriteDiagnostics(j, errors);
    }
    if (!notes.empty() && !quiet) {
        j.Key(L"notes");
        WriteDiagnostics(j, notes);
    }
    j.End();
    return j.Str();
}

// 解析失败 / 后端未实现共用的失败文档。stage 由调用点给：解析错误是 parse，
// 后端未实现是 capture / match。退出码由调用点决定。
int EmitFailure(const ParseResult& parse, const wchar_t* stage, int code,
                const wchar_t* messageKey, Response* out) {
    std::vector<Diagnostic> errors = parse.errors;
    if (errors.empty()) {
        Diagnostic d;
        d.code = codes::kUnsupported;
        d.message = Msg(messageKey);
        d.hint = Msg(L"cap.linux_backend_not_implemented_hint");
        d.backend = L"none";
        d.stage = stage;
        errors.push_back(std::move(d));
    } else {
        for (auto& d : errors) {
            if (d.stage.empty()) d.stage = stages::kParse;
        }
    }
    out->body = ErrorDocument(errors, parse.warnings, parse.options.quiet) + L"\n";
    out->exitCode = code;
    out->toStderr = GoesToStderr(parse.options);
    return code;
}

std::wstring EmergencyDoc(const char* what, const wchar_t* stage) {
    const std::string raw = what ? what : std::string();
    const std::wstring detail = DecodeUtf8(raw.data(), raw.size()).text;
    std::wstring message = Msg(L"cli.internal_error");
    if (!detail.empty()) message += L": " + detail;
    Json j;
    j.Obj().Key(L"captured").Value(0).Key(L"images").Arr().End().Key(L"errors").Arr().Obj()
        .Key(L"code").Value(codes::kInternalError)
        .Key(L"message").Value(message)
        .Key(L"stage").Value(stage)
        .End()
        .End()
        .End();
    return j.Str();
}

int LinuxCliMain(const CliTokens& tokens) {
    Response resp;
    try {
        const ParseResult parse = ParseCommandLine(tokens);
        const Options& opt = parse.options;

        if (!parse.ok) {
            // --help / --version 的既有语义是"其余参数一条都不执行"：先把人要的帮助交出去。
            // 其余解析错误保持与 Windows 同形（captured=0 / images=[] / errors=[...]）。
            EmitFailure(parse, stages::kParse, EX_USAGE, L"cap.unsupported", &resp);
        } else if (opt.ConfigViewMode()) {
            resp.body = RenderConfigView(opt);
            if (!resp.body.empty() && resp.body.back() != L'\n') resp.body += L'\n';
            resp.exitCode = EX_OK;
            resp.toStderr = false;
        } else if (opt.EnvQueryMode()) {
            if (opt.screens) {
                // 屏幕枚举需要真实屏幕后端；本构建没有，明确报未实现，不交空列表。
                EmitFailure(parse, stages::kMatch, EX_CAPTURE_FAILED, L"cap.unsupported", &resp);
            } else {
                const EnvQueryKind kind =
                    opt.diagnostics ? EnvQueryKind::kDiagnostics : EnvQueryKind::kCapabilities;
                const EnvReport report = BuildEnvReport(ProbeEnvFacts(), kind);
                resp.body = RenderEnvJson(report, opt.verbose, opt.quiet);
                if (!resp.body.empty() && resp.body.back() != L'\n') resp.body += L'\n';
                resp.exitCode = EX_OK;
                resp.toStderr = false;   // 查询不产图，stdout 上不会有图片字节
            }
        } else if (opt.WindowQueryMode()) {
            // --list / --inspect 问的是窗口系统；Linux 窗口枚举后端未实现。
            EmitFailure(parse, stages::kMatch, EX_CAPTURE_FAILED, L"cap.unsupported", &resp);
        } else if (opt.showVersion) {
            resp.body = VersionText();
            resp.exitCode = EX_OK;
            resp.toStderr = false;
        } else if (opt.showHelp) {
            if (!opt.helpReason.empty()) resp.body += Msg(L"help.no_condition") + L"\r\n\r\n";
            resp.body += HelpText();
            resp.exitCode = opt.helpReason.empty() ? EX_HELP : EX_NO_CONDITION;
            resp.toStderr = false;
        } else {
            // 正常截图意图：本构建没有 Linux 采集后端。绝不返回"可以截图"。
            EmitFailure(parse, stages::kCapture, EX_CAPTURE_FAILED, L"cap.unsupported", &resp);
        }
    } catch (const std::exception& e) {
        resp.body = EmergencyDoc(e.what(), StdoutClaimed() ? stages::kReport : stages::kParse);
        resp.exitCode = EX_INTERNAL;
        resp.toStderr = true;
    } catch (...) {
        resp.body = EmergencyDoc("unknown exception",
                                 StdoutClaimed() ? stages::kReport : stages::kParse);
        resp.exitCode = EX_INTERNAL;
        resp.toStderr = true;
    }

    if (!resp.body.empty() && resp.body.back() != L'\n') resp.body += L'\n';
    const bool written = resp.toStderr ? EmitStderrRaw(resp.body) : EmitStdout(resp.body);
    if (written) return resp.exitCode;
    if (!resp.toStderr) EmitStderrRaw(resp.body);
    return EX_IO_FAILED;
}

}  // namespace
}  // namespace ecapture

int main(int argc, char** argv) {
    const ecapture::CliTokens tokens = ecapture::DecodePosixArgv(argc, argv);
    return ecapture::LinuxCliMain(tokens);
}
