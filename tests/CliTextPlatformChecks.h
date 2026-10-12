#pragma once
// 入口那一层"把本平台的 argv 换成 CliTokens"的判据，按平台各一份实现：
//   * tests/platform/cli_text_win_checks.cpp   —— DecodeWindowsArgv：UTF-16 逐条原样搬
//   * tests/platform/cli_text_posix_checks.cpp —— DecodePosixArgv：字节按 UTF-8 严格解码
// 由 tests/cli_text_state.cpp 调用（CMakeLists.txt 里按平台选一份一起编）。
//
// check 是测试自己的判据出口（PASS/FAIL 计数在 cli_text_state.cpp 里），这里不另立一套。
// 之所以用函数指针而不是把这些用例直接写进主文件：主文件必须两个平台都编得过，
// 而这两个入口函数各自只在自家平台上被实现。

namespace ecapture {
class CliTokens;
}

void RunNativeArgvChecks(void (*check)(bool, const char*));
