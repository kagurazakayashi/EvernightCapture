// 入口判据（Linux）：main 拿到的 argv 字节 → CliTokens。
//
// 与生产代码走的确实是同一条 DecodePosixArgv（src/platform/linux/PosixCliArgs.cpp），
// 这里不另写一份"看着一样的解码"。
#include "../CliTextPlatformChecks.h"

#include <string>

#include "../../src/CliArgs.h"
#include "../../src/core/CliTokens.h"
#include "../../src/core/TextCoding.h"

using namespace ecapture;

void RunNativeArgvChecks(void (*check)(bool, const char*)) {
    {
        const char* argv[] = {"ecapture", "--title", "\xe5\xa4\x9c \xe7\xa9\xba",
                              "\xe4\xb8\xad\xe6\x96\x87 \xe7\x9b\xae\xe5\xbd\x95/out.png"};
        const CliTokens tokens = DecodePosixArgv(4, argv);
        check(tokens.Count() == 4, "Linux：四条参数收齐（含程序名那一条）");
        check(tokens.At(2) == L"\u591c \u7a7a", "Linux：中文与空格的取值逐字成为内部文本");
        check(tokens.At(3).find(L'/') != std::wstring::npos, "Linux：路径里的分隔符不被吞掉");
        check(!tokens.HasInvalidUtf8(), "Linux：都合的参数不报编码问题");
    }
    {
        const std::string bad = std::string("a") + static_cast<char>(0xFF) + "b";
        const char* raw = bad.c_str();
        const char* argv[] = {"ecapture", raw};
        const CliTokens tokens = DecodePosixArgv(2, argv);
        check(tokens.HasInvalidUtf8(), "Linux：非法字节那一条被标出来");
        check(tokens.InvalidOffset(1) == 1, "Linux：交回第一处不可用字节的偏移");
        check(tokens.At(1).find(L'\uFFFD') != std::wstring::npos,
              "Linux：坏段留下 U+FFFD，其后的正常字节不丢");
        check(tokens.InvalidOffset(0) == std::wstring::npos, "Linux：程序名那一条没被牵连");
    }
    {
        const char* argv[] = {nullptr};
        check(DecodePosixArgv(0, argv).Count() == 0, "Linux：argc=0 交回空表而不是崩");
        check(DecodePosixArgv(2, nullptr).Count() == 0, "Linux：argv 为空指针时交回空表");
    }
    {
        // 出口那一头：内部文本编回 UTF-8 应当与进来的字节一致（同一条约定两头对得上）
        const char* argv[] = {"ecapture", "\xe6\x8a\xa5\xe5\x91\x8a"};
        const CliTokens tokens = DecodePosixArgv(2, argv);
        check(EncodeUtf8(tokens.At(1)) == "\xe6\x8a\xa5\xe5\x91\x8a",
              "Linux：解进来再编出去是同一串字节");
    }
}
