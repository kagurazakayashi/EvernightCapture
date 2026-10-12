#pragma once
// 平台服务：把本平台的 argv 换成本项目内部的文字（CliTokens）。
//
// 这是"命令行文本"唯一的一次换码处，也是入口边界本体：
//   * 一个构建里只用得到下面的一条，用哪条由本次构建的平台族决定（src/core/Platform.h），
//     不在共用代码里 #ifdef：两条各写在自己的平台源文件里，共用层只认 CliTokens。
//   * Windows 的 wmain 拿到的 wchar_t** 本来就是 UTF-16 码元 —— 与内部约定同一种，逐条原样搬，
//     一次多余的转换都不做（"先转出去再转回来"正是把参数弄坏的起点）。
//   * Linux 的 main 拿到的是字节：按 UTF-8 严格解码。读不通的那几条**照样带下去**，但记下这条
//     原文里第一处不可用字节的偏移，由解析层逐条报 cli.invalid_utf8 并让本次请求失败 ——
//     不把 U+FFFD 当成一次合法的标题 / 路径 / 正则去用（那是把用户的原话换成另一句话）。
//   * 两个函数收的 argc 都含程序名那一条（argv[0]），CliTokens 里它占第 0 格。

#include "core/CliTokens.h"

namespace ecapture {

// 【Windows 入口】src/platform/windows/WinCliArgs.cpp
CliTokens DecodeWindowsArgv(int argc, wchar_t* const* argv);

// 【Linux 入口】src/platform/linux/PosixCliArgs.cpp
CliTokens DecodePosixArgv(int argc, const char* const* argv);

}  // namespace ecapture
