// EvernightCapture —— CLI 截图工具（ECAPTURE.EXE）
//
// 输出：--help / --version / 不给条件时是纯文本；其余（成功与错误）是精简 JSON，
// 只含 captured / images，外加按需出现的 errors / notes / input。详见 src/Report.h。
// 进程退出码始终有效，与 body 属于两套独立信号。
//
// 当前构建只完成命令行解析与校验；窗口枚举与 Windows.Graphics.Capture 截图未实现。

#include <exception>
#include <string>

#include "CliOptions.h"
#include "Json.h"
#include "Report.h"

namespace {

// 连解析/渲染本身都抛异常时的兜底：保持同一精简形状，让调用方仍能按 errors 处理。
std::wstring EmergencyDoc(const char* what) {
    std::wstring detail;
    for (const char* p = what; p && *p; ++p) detail.push_back(static_cast<wchar_t>(*p));
    ecapture::Json j;
    j.Obj().Key(L"captured").Value(0).Key(L"images").Arr().End().Key(L"errors").Arr().Obj()
        .Key(L"code").Value(ecapture::codes::kInternalError)
        .Key(L"message").Value(detail.empty() ? ecapture::Msg(L"cli.internal_error") : detail)
        .End()
        .End()
        .End();
    return j.Str();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ecapture::Response resp;

    try {
        const ecapture::ParseResult parse = ecapture::ParseCommandLine(argc, argv);
        ecapture::BuildResponse(parse, argc, argv, &resp);
    } catch (const std::exception& e) {
        resp.body = EmergencyDoc(e.what());
        resp.exitCode = ecapture::EX_INTERNAL;
        resp.toStderr = false;
    } catch (...) {
        resp.body = EmergencyDoc("unknown exception");
        resp.exitCode = ecapture::EX_INTERNAL;
        resp.toStderr = false;
    }

    if (!resp.body.empty() && resp.body.back() != L'\n') resp.body += L'\n';
    const bool written = resp.toStderr ? ecapture::EmitStderrRaw(resp.body)
                                       : ecapture::EmitStdout(resp.body);
    if (written) return resp.exitCode;
    // stdout 写失败时退回 stderr；两处都失败才把退出码改成 8（文档可能已丢失）
    if (ecapture::EmitStderrRaw(resp.body)) return resp.exitCode;
    return ecapture::EX_IO_FAILED;
}
