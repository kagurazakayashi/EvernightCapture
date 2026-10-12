#pragma once
// 跨平台核心：嵌入文案的取回接口。
//
// 四份 resources/strings-*.txt 在**构建期**由 scripts/gen-lang-data.cmake 生成一份 C++ 源文件
// （原始字符串字面量里的 UTF-8 字节），两个平台编译同一份生成物。以前这四份是 Windows 的
// RCDATA，共用语言层要读它们就得 #include <windows.h> —— 那是只有 Windows 才有的资源系统，
// Linux 上根本没有这一份载荷。现在嵌入跟着构建走，改文案由自定义命令的 DEPENDS 触发重生成、
// 重编译，不再需要"记得 clean 重编"。
//
// 生成物由构建系统提供，不在仓库里；任何目标要用文案，必须同时编译那份生成的 .cpp
// （CMakeLists.txt 里的 ECAPTURE_LANG_SOURCES 一处登记，别处不要各抄一份路径）。

#include <cstddef>

namespace ecapture {

struct LangBlob {
    const char* bytes;  // UTF-8，可能带源文件自己写的 BOM（由表解析那一层剥掉）
    std::size_t size;   // 字节数，bytes 里**不**含结尾 NUL
};

// 按嵌入名取那一份（ECP_TEXT_ZH_CN / ECP_TEXT_ZH_TW / ECP_TEXT_EN / ECP_TEXT_JA）。
// 名字与语言枚举的对应表只写在 src/Lang.cpp 一处。这个名字没有嵌入时返回 nullptr，
// 不返回一份空表蒙人 —— 让调用方看得见"这份语言压根没进来"。
const LangBlob* FindLangBlob(const char* name);

}  // namespace ecapture
