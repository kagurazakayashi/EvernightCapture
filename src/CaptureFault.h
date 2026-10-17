#pragma once
// 【平台接口】"一次取帧调用抛出来的东西" -> 一份可记入诊断的事实。
//
// 为什么要这一层：后端可能抛异常（WinRT 的 hresult_error、内存耗尽、正则库的异常……），
// 而"这个异常是不是致命"（换一条后端也不会有区别）与"它带着哪个原生错误码"都是平台事实：
// Windows 上要认 DXGI_ERROR_DEVICE_* 与 E_OUTOFMEMORY，Linux 后端将来认自己那套。
// 共用流水线（src/Capture.cpp）只想知道三件事：细节文字、原生码、以及要不要立刻终止整批。
//
// 只有一条调用约定：DescribeCaptureException() 必须在 catch(...) 里面调用 —— 它替调用方
// 重新抛出当前异常并分类；在 catch 之外调用得不到有意义的结果。

#include <string>

#include "core/Error.h"   // NativeErrorCode

namespace ecapture {

struct CaptureFault {
    // 细节文字（只放 ASCII：异常 what() 本来就是窄字符）。进 message 的 %1，不本地化。
    std::wstring detail;
    // 一次 COM / WinRT 调用自己的返回值；没有这一格来源时为 0。诊断里写成 0x........ 文本。
    NativeErrorCode hresult = kNativeErrorNone;
    // 换一条后端也不会有区别（内存耗尽、显卡设备没了）：调用方终止整批而不是继续回退。
    bool fatal = false;
};

// 描述当前正在传播的异常。必须在 catch(...) 内调用。
CaptureFault DescribeCaptureException();

}  // namespace ecapture
