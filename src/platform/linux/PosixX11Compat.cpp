// 【平台层 · Linux】共用层需要的几个"平台事实"在 X11 构建上的实现。
//
// 这些入口在 Windows 上各有自己的文件（WinSystemCompat / WinErrorText /
// WinProcessIntegrity / WinCaptureFault / WxConsentDriver）。Linux 本构建：
//   * 没有 Windows 版本门槛：ProbeOsVersion 交回 known=false（判据表在共用层，
//     Linux 的 AutoChain 只有 xcomposite，见 SystemCompat.cpp）。
//   * 没有完整性标签：ProbeProcessIntegrity 交回 kUnknown，不凭空扣"被降级"的帽子。
//   * 没有 wxWidgets 许可对话框：DialogConsentPrompt 如实交回 kInitFailed（没有 --yes
//     时不会静默放行）。
//   * 原生错误码是 errno / X 错误码：只把数字写成稳定文本，不做本地化。
//
// 这一份不取任何像素，也不改任何窗口。

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "CaptureFault.h"
#include "Consent.h"
#include "ErrorText.h"
#include "ProcessIntegrity.h"
#include "SystemCompat.h"

namespace ecapture {

OsVersion ProbeOsVersion() {
    // X11 构建没有"Windows 版本"这一问。known=false 表示这一问没答案；通道闸门在
    // SystemCompat.cpp 里按平台分支，不会因为版本未知而放宽任何东西。
    return OsVersion{};
}

std::wstring NativeErrorText(NativeErrorCode code) {
    // errno 那一路：数字本身就是稳定信息，附上 strerror 的英文说明。
    const int value = static_cast<int>(code);
    std::string text;
    if (value != 0) {
        char buffer[256] = {0};
        // GNU strerror_r 的返回类型与 POSIX 不同；只用 XSI 版本的第三种写法会踩编译差异，
        // 这里改用 strerror（进程级静态缓冲，单线程调用）来避免那处分叉。
        const char* message = std::strerror(value);
        if (message) {
            text = message;
        } else {
            text = "unknown";
        }
    }
    std::wstring out = L"errno " + std::to_wstring(code);
    if (!text.empty()) out += L": " + std::wstring(text.begin(), text.end());
    return out;
}

std::wstring HResultCodeText(NativeErrorCode hr) {
    wchar_t buffer[16] = {0};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"0x%08X",
                  static_cast<unsigned>(hr));
    return buffer;
}

CaptureFault DescribeCaptureException() {
    CaptureFault fault;
    try {
        throw;
    } catch (const std::bad_alloc&) {
        fault.detail = L"bad_alloc";
        fault.fatal = true;
    } catch (const std::exception& error) {
        const char* what = error.what() ? error.what() : "unknown";
        for (const char* p = what; *p; ++p) {
            fault.detail.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        }
    } catch (...) {
        fault.detail = L"unknown";
    }
    return fault;
}

Integrity ProbeProcessIntegrity(uint32_t* probeError) {
    // Linux 没有 Windows 的强制完整性标签。kUnknown = 这一问没有答案；不按"够用"处理，
    // 也不凭空说"被降级了"。
    if (probeError) *probeError = 0;
    return Integrity::kUnknown;
}

bool IntegrityBelowMedium(Integrity value) {
    (void)value;
    return false;
}

bool IsAccessDenied(const Diagnostic& diag) {
    (void)diag;
    // 0x80070005 / Win32 5 是 Windows 的"系统说不让"。Linux 的 EACCES 走 io.write_failed
    // 的既有路径，不在这里冒充同一件事。
    return false;
}

size_t AnnotateIntegrityDenials(Integrity integrity, std::vector<Diagnostic>* errors,
                                std::vector<Diagnostic>* warnings) {
    (void)integrity;
    (void)errors;
    (void)warnings;
    return 0;
}

ConsentReply DialogConsentPrompt::Ask(const ConsentQuestion& question) {
    (void)question;
    // 本构建没有图形许可对话框。没有 --yes 时，授权状态机会把这一步当成"弹不出来"并
    // 拒绝本次采集（capture.consent_unavailable），绝不静默放行。
    ConsentReply reply;
    reply.answer = ConsentAnswer::kInitFailed;
    return reply;
}

}  // namespace ecapture
