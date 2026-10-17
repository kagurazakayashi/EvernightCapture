// 【平台层 · Windows】"一次取帧调用抛出来的东西"的分类。
//
// 为什么要平台层：是不是致命（换一条后端也不会有区别）取决于原生错误码的取值空间 ——
// E_OUTOFMEMORY 与 DXGI_ERROR_DEVICE_* 是 Windows 这一侧的判断；异常类型本身
// （winrt::hresult_error 是 C++/WinRT 的）也只有这一层认识。共用流水线只看结果。
//
// 调用约定：DescribeCaptureException() 必须在 catch(...) 里面调用。

#include "CaptureFault.h"

#include <exception>
#include <string>

#include <dxgi.h>        // DXGI_ERROR_DEVICE_*：显卡设备没了 = 换后端也不会有区别
#include <winrt/base.h>  // 后端可能直接抛出 hresult_error，而不是返回错误码

#include "CaptureCommon.h"   // HResultText（0x 八位十六进制，与既有诊断逐字相同）
#include "Lang.h"
#include "core/Error.h"

namespace ecapture {
namespace {

// 异常 what() 是窄字符，逐字节宽化：本项目所有对外文字都走宽字符，而这一段细节要进
// message 的 %1 —— 与 src/Capture.cpp 改造前那一份逐字相同。
std::wstring WidenAscii(const char* text) {
    std::wstring out;
    for (const char* p = text; p && *p; ++p) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    return out;
}

}  // namespace

CaptureFault DescribeCaptureException() {
    CaptureFault fault;
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const winrt::hresult_error& e) {
        const HRESULT hr = e.code();
        fault.hresult = static_cast<NativeErrorCode>(hr);
        fault.detail = HResultText(fault.hresult);
        const bool deviceLost = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                                hr == DXGI_ERROR_DEVICE_HUNG;
        fault.fatal = hr == E_OUTOFMEMORY || deviceLost;
    } catch (const std::bad_alloc&) {
        fault.detail = L"std::bad_alloc";
        fault.fatal = true;
    } catch (const std::length_error&) {
        fault.detail = L"std::length_error";
        fault.fatal = true;
    } catch (const std::exception& e) {
        fault.detail = WidenAscii(e.what());
        if (fault.detail.empty()) fault.detail = L"std::exception";
    } catch (...) {
        fault.detail = L"unknown";
    }
    return fault;
}

}  // namespace ecapture
