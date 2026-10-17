// 【平台层 · Windows】本机版本探测（RtlGetVersion）。
//
// 判据本体（版本门槛表、通道闸门、环境诊断）留在 src/SystemCompat.cpp：那一份是平台无关
// 的判据，注入一个假 OsVersion 就能离线逐条判。本文件只回答"这台机器的 Windows 内部版本
// 是多少"——那是只有 Windows SDK 问得出来的事实。
//
// 绝不用 GetVersionEx：那个函数受"应用清单兼容性声明"与版本伪装影响，没有清单时在
// Windows 10/11 上固定回 6.2，拿它判下限等于自己骗自己。

#include "SystemCompat.h"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {
namespace {

// RtlGetVersion 的函数原型。ntdll.dll 本来就在进程里（它是最初装载的那批之一），
// 所以这里只取函数地址，不 LoadLibrary；那个结构是纯 POD，不依赖任何 SDK 版本宏。
using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);

// 那个函数的成功返回值是 NTSTATUS 的 0。不引 STATUS_SUCCESS：它在 ntstatus.h 里，
// 而这里只需要这一个数。
constexpr LONG kNtSuccess = 0;

}  // namespace

OsVersion ProbeOsVersion() {
    // 只问一次：一次运行里版本号不会变，而通道闸门与 -v 的回显两处都要读它。
    static const OsVersion cached = [] {
        OsVersion v;
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return v;
        const RtlGetVersionFn proc = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (!proc) return v;
        OSVERSIONINFOEXW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (proc(&info) != kNtSuccess) return v;
        v.major = static_cast<uint32_t>(info.dwMajorVersion);
        v.minor = static_cast<uint32_t>(info.dwMinorVersion);
        v.build = static_cast<uint32_t>(info.dwBuildNumber);
        v.known = v.build != 0;
        return v;
    }();
    return cached;
}

}  // namespace ecapture
