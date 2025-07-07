// 版本号的唯一来源。
//
// 三处都从这里取，避免互相漂移：
//   * C++：src/CliOptions.h 的 kVersion，也就是 --version 打出来的那个数字
//   * 文件属性：resources/ecapture.rc 里的 VERSIONINFO（资源管理器"属性 → 详细信息"）
//   * CMakeLists.txt 的 project(VERSION ...)，由脚本从这里解析
//
// 本文件同时被 cl 和 rc.exe 预处理，所以只有 #define、不带 #pragma once
// （rc.exe 认不认得别处的编译指示不值得赌），也不 include 任何东西。
// 字面量一律不带 L 前缀：rc.exe 的 VERSIONINFO 只吃窄字面量，而那份字符串存进
// 二进制时本来就是 UTF-16；C++ 侧要宽字符就用 ECAPTURE_TEXT()。

#ifndef ECAPTURE_VERSION_H
#define ECAPTURE_VERSION_H

#define ECAPTURE_VERSION_MAJOR 0
#define ECAPTURE_VERSION_MINOR 4
#define ECAPTURE_VERSION_PATCH 0

// 点号形式：--version 与文件属性里的"文件版本 / 产品版本"都用它。
// 改版本号必须同时改上面三个数字（FILEVERSION / PRODUCTVERSION 靠它们做数值比较）。
#define ECAPTURE_VERSION_STRING "0.4.0"

// "0.4.0" -> L"0.4.0"。## 之前不展开宏，所以要先套一层。
#define ECAPTURE_WIDE_(s) L##s
#define ECAPTURE_WIDE(s) ECAPTURE_WIDE_(s)
#define ECAPTURE_TEXT(s) ECAPTURE_WIDE(s)

#endif  // ECAPTURE_VERSION_H
