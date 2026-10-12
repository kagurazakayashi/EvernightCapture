#pragma once
// 平台服务：把一个小文件整份读进内存。
//
// 声明共用（src/Config.cpp 读 ECAPTURE.ini 走这一条），实现分平台
// （src/platform/windows/WinHostFile.cpp 与 src/platform/linux/PosixHostFile.cpp）。
//
// 三条共同的规矩，两套实现都必须满足：
//   1. **上限在分配之前判**：一份超限的文件不该先被整个搬进内存再报错（tooLarge + 不填字节）。
//   2. **撞上同名目录要当场说清**：目录不是"读不出来的文件"。Windows 上这正是不用
//      GetPrivateProfileStringW 的原因之一（它会一直阻塞），Linux 上 open 一个目录能成功而
//      read 会失败，所以先判 S_ISDIR 再读。
//   3. **原生错误码只做核对，不做分支**：Windows 交回 GetLastError，Linux 交回 errno，
//      两个数空间互不相干（同一个 5 在两边是两件事），所以调用方一律按 ReadOutcome 分支，
//      原生码只进诊断的 detail 供人核对（core/Error.h 的 NativeErrorCode）。
//
// 路径在这里换码：共用层拿到的路径是 wstring，落到系统调用要变回本平台的原生字节。
// Linux 那一份按 UTF-8 编码（src/core/TextCoding.h），Windows 那一份本来就是 UTF-16 原地。

#include <cstdint>
#include <string>
#include <vector>

#include "core/Error.h"

namespace ecapture {

// 一个文件的字节读不出来是**哪一种**读不出来，三种下场的下一步不同，所以各给一个 detail token：
//   not-found     路径上这一级根本没有这个文件
//   directory     这个名字被一个目录占着
//   other         其余（权限、独占、读到一半失败……），带原生码原值
enum class ReadOutcome { kOk, kNotFound, kDirectory, kOther };

struct HostFileRead {
    ReadOutcome outcome = ReadOutcome::kOther;
    std::vector<uint8_t> bytes;
    NativeErrorCode native = kNativeErrorNone;  // Windows=GetLastError，Linux=errno
    bool tooLarge = false;
};

// 整份读取，最多 maxBytes。超限或读不出时 bytes 不可用（kOk 才代表拿到了）。
HostFileRead ReadWholeFileBounded(const std::wstring& path, uint64_t maxBytes);

// 诊断 detail 里那个原生码的前缀名：Windows 写 win32=，Linux 写 errno=。
// 判"这是哪一种故障"用的是 ReadOutcome，这一格只是把系统给的原值如实带出来。
const wchar_t* NativeErrorCodeLabel();

}  // namespace ecapture
