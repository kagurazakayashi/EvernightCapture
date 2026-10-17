// 【平台层 · Windows】原生错误码 -> 文本。
//
// 判据（哪条失败该用哪一句文案）在共用层 src/Isolation.cpp 的 BlockedToDiagnostic 与各
// 平台的诊断构造处；本文件只回答"这个数字怎么写成人话"。Windows 这一份沿用改造前
// CaptureCommon.cpp 的两条实现：系统错误码走 Msgf(err.win32_code)（与 Lang 的四语资源同源），
// COM / WinRT 的返回值写成 0x 加八位十六进制（长度恒 10）。

#include "ErrorText.h"

#include <cstdio>
#include <string>

#include "CliOptions.h"   // Lang 的 Msgf 走 CliOptions 的资源接口（与改造前同一份调用面）
#include "Lang.h"

namespace ecapture {

std::wstring NativeErrorText(NativeErrorCode code) { return Msgf(L"err.win32_code", code); }

std::wstring HResultCodeText(NativeErrorCode hr) {
    wchar_t buf[40];
    swprintf(buf, 40, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace ecapture
