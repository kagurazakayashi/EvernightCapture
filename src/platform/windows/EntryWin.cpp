// 【平台层 · Windows】程序入口：wmain。
//
// CRT 按外部符号找 wmain，所以它必须在文件作用域（放进匿名命名空间就变成内部链接，
// 链接阶段直接报找不到入口）。这里只有两步：
//   1. 分诊"辅助工作进程"这一种调用形态 —— 它不是一个选项，走正常解析只会被报成
//      cli.unknown_option；而它自己既不产生 JSON 也不写标准输出，唯一的出口是那条绑定了
//      本次父进程的管道（见 src/Worker.h，这套隔离执行目前只有 Windows 一侧实现）。
//   2. 其余交给共用正文 CliMain（src/Main.cpp），参数先经 DecodeWindowsArgv 收进 CliTokens。
#include "Main.h"

#include "CliArgs.h"
#include "Worker.h"

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && ecapture::LooksLikeWorkerInvocation(argv[1])) {
        return ecapture::RunWorkerMode(argc, argv);
    }
    return ecapture::CliMain(ecapture::DecodeWindowsArgv(argc, argv));
}
