// 【平台层 · Linux】ECAPTURE 的 Linux 入口。
//
// 入口只做两件事：把 argv 按 UTF-8 严格解码成 CliTokens（唯一一次换码），以及把 tokens
// 交给共用正文 src/Main.cpp 的 CliMain。解析、查询、截图流水线、交付与错误归类都在共用层 ——
// 这一份里**没有**第二套命令分派，也没有"某个后端还没接上"的占位分支：本构建接的是
// X11 单窗口通道（xcomposite），走不走得通由那一次真实采集自己交回错误码。

#include "CliArgs.h"
#include "Main.h"

int main(int argc, char** argv) {
    const ecapture::CliTokens tokens = ecapture::DecodePosixArgv(argc, argv);
    // 共用正文：解析、截图流水线、交付与错误归类都在 src/Main.cpp。
    return ecapture::CliMain(tokens);
}
