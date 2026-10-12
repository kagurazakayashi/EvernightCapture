// 【平台层 · Windows】argv 的收取：UTF-16 参数表 → CliTokens。
//
// wmain 本体在 src/platform/windows/EntryWin.cpp（那一份只有主程序 ecapture 用得到，
// 因为它要分诊辅助进程模式）；这一份只管换码，所以离线判据可以直接链它，
// 不必把 Worker 那一整套管道实现也拖进测试程序。
//
// 收取这一层只干一件事，一件都不判参数含义：
//   1. 把 wchar_t** 原样收进 CliTokens（Windows 交来的已经是内部文字，不转换、不校验：
//      校验 UTF-16 码元合不合是 CRT 之前那一步的事，程序拿到的就是系统给的那一串）；
// 系统交来的 wchar_t** 与内部约定同一种文字，逐条原样搬，一次多余的转换都不做
// （"先转出去再转回来"正是把参数弄坏的起点）。
#include "CliArgs.h"

namespace ecapture {

CliTokens DecodeWindowsArgv(int argc, wchar_t* const* argv) {
    CliTokens tokens;
    if (argc < 1 || !argv) return tokens;
    for (int i = 0; i < argc; ++i) {
        // argv[i] 在正常启动下不会为空；真遇到空指针就当一条空参数收下来，
        // 让它走"空参数"那一条既有判据，而不是让入口先崩掉。
        tokens.PushBack(argv[i] ? std::wstring(argv[i]) : std::wstring());
    }
    return tokens;
}

}  // namespace ecapture
