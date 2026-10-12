// EvernightCapture —— CLI 截图工具（ECAPTURE.EXE）的共用正文
//
// 输出：--help / --version / 不给条件时是纯文本；其余（成功与错误）是精简 JSON，
// 只含 captured / images，外加按需出现的 errors / notes / input。详见 src/Report.h。
// 进程退出码始终有效，与 body 属于两套独立信号。
//
// 标准流规矩：一旦图片要用 stdout（显式 -o - 或根本没给输出路径），结果 JSON 就整份走
// stderr，本文件的兜底路径也不例外 —— 已发给 stdout 的图片字节收不回来。
//
// 参数从 CliTokens 进来（已经换成本项目内部的文字，见 src/core/CliTokens.h）：入口那一层
// 负责 Windows 的 UTF-16 原样收取与 Linux 的 UTF-8 严格解码，这里不再碰系统的那一份 argv。
#include <exception>
#include <string>

#include "CliOptions.h"
#include "Json.h"
#include "Main.h"
#include "Report.h"
#include "core/TextCoding.h"

namespace {

// 连解析/渲染本身都抛异常时的兜底：保持同一精简形状，让调用方仍能按 errors 处理。
// stage 写明异常出在哪个阶段（parse = 还没开始出图，report = 图片可能已经发出去了）。
// 异常带回来的 what() 是窄字节：按 UTF-8 解一遍而不是逐字节扩成 wchar_t —— 后者会把一段
// 合法的 UTF-8 拆成一堆乱码字符，正文里出现非 ASCII 时（例如某些运行库的消息）就是那样坏掉的。
std::wstring EmergencyDoc(const char* what, const wchar_t* stage) {
    const std::string raw = what ? what : std::string();
    const std::wstring detail = ecapture::DecodeUtf8(raw.data(), raw.size()).text;
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

namespace ecapture {

int CliMain(const CliTokens& tokens) {
    Response resp;

    try {
        const ParseResult parse = ParseCommandLine(tokens);
        BuildResponse(parse, &resp);
    } catch (const std::exception& e) {
        // 异常也可能出自取帧 / 编码 / 写文件那一段，那时 resp.toStderr 还没被 BuildResponse 赋值。
        // 这里不再"另写一遍参数解析"去猜该走哪条流（猜错就会把文字混进图片里），
        // 而是按最保守的约定一律走 stderr：正常路径下 ResultGoesToStderr 给的也是同一条。
        resp.body = EmergencyDoc(e.what(), StdoutClaimed() ? stages::kReport
                                                           : stages::kParse);
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
    // 约定通道写不出去就是 I/O 失败（8）：调用方按约定流读不到结果，就不能因为
    // "另一条流上补发成功"而把退出码留成原来的值。图片占用 stdout 时不反向往 stdout 补发。
    if (!resp.toStderr) EmitStderrRaw(resp.body);
    return EX_IO_FAILED;
}

}  // namespace ecapture
