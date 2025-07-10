// EvernightCapture —— CLI 截图工具（ECAPTURE.EXE）
//
// 输出：--help / --version / 不给条件时是纯文本；其余（成功与错误）是精简 JSON，
// 只含 captured / images，外加按需出现的 errors / notes / input。详见 src/Report.h。
// 进程退出码始终有效，与 body 属于两套独立信号。
//
// 标准流规矩：一旦图片要用 stdout（显式 -o - 或根本没给输出路径），结果 JSON 就整份走
// stderr，本文件的兜底路径也不例外 —— 已发给 stdout 的图片字节收不回来。

#include <exception>
#include <string>

#include "CliOptions.h"
#include "Json.h"
#include "Report.h"

namespace {

// 连解析/渲染本身都抛异常时的兜底：保持同一精简形状，让调用方仍能按 errors 处理。
// stage 写明异常出在哪个阶段（parse = 还没开始出图，report = 图片可能已经发出去了）。
std::wstring EmergencyDoc(const char* what, const wchar_t* stage) {
    std::wstring detail;
    for (const char* p = what; p && *p; ++p) detail.push_back(static_cast<wchar_t>(*p));
    std::wstring message = ecapture::Msg(L"cli.internal_error");
    if (!detail.empty()) message += L": " + detail;

    ecapture::Json j;
    j.Obj().Key(L"captured").Value(0).Key(L"images").Arr().End().Key(L"errors").Arr().Obj()
        .Key(L"code").Value(ecapture::codes::kInternalError)
        .Key(L"message").Value(message)
        .Key(L"stage").Value(stage)
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
        // 异常也可能出自取帧 / 编码 / 写文件那一段，那时 resp.toStderr 还没被 BuildResponse 赋值。
        // 这里不再"另写一份 argv 解析"去猜该走哪条流（猜错就会把文字混进图片里），
        // 而是按最保守的约定一律走 stderr：正常路径下 ResultGoesToStderr 给的也是同一条。
        resp.body = EmergencyDoc(e.what(), ecapture::StdoutClaimed() ? ecapture::stages::kReport
                                                                     : ecapture::stages::kParse);
        resp.exitCode = ecapture::EX_INTERNAL;
        resp.toStderr = true;
    } catch (...) {
        resp.body = EmergencyDoc("unknown exception",
                                 ecapture::StdoutClaimed() ? ecapture::stages::kReport
                                                           : ecapture::stages::kParse);
        resp.exitCode = ecapture::EX_INTERNAL;
        resp.toStderr = true;
    }

    if (!resp.body.empty() && resp.body.back() != L'\n') resp.body += L'\n';
    const bool written = resp.toStderr ? ecapture::EmitStderrRaw(resp.body)
                                       : ecapture::EmitStdout(resp.body);
    if (written) return resp.exitCode;
    // 约定通道写不出去就是 I/O 失败（8）：调用方按约定流读不到结果，就不能因为
    // "另一条流上补发成功"而把退出码留成原来的值。图片占用 stdout 时不反向往 stdout 补发。
    if (!resp.toStderr) ecapture::EmitStderrRaw(resp.body);
    return ecapture::EX_IO_FAILED;
}
