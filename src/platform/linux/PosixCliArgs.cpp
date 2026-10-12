// 【平台层 · Linux】命令行参数的收取：argv 的字节按 UTF-8 严格解码。
//
// 这一份是本平台的"入口换码"，与 Windows 那一份（src/platform/windows/WinCliArgs.cpp 的
// UTF-16 原样收取）对应；两者交回的都是同一个 CliTokens，往后所有判据只认内部文字。
//
// 本步只交付到"把一串参数换成 CliTokens"这一层，没有 Linux 上的 main：ECAPTURE 的响应组装
// （src/Report.cpp）与取帧、窗口/屏幕枚举、辅助进程管道都还在 Windows 那一侧，
// ecapture 这个目标在 Linux 上还链不起来。所以这里编得过、也被离线判据逐条判过，
// 但不假装已经有一条能跑的 Linux 命令行入口。
#include "CliArgs.h"

#include <cstddef>
#include <string>

#include "core/TextCoding.h"

namespace ecapture {

CliTokens DecodePosixArgv(int argc, const char* const* argv) {
    CliTokens tokens;
    if (argc < 1 || !argv) return tokens;
    for (int i = 0; i < argc; ++i) {
        const char* raw = argv[i];
        if (!raw) {
            // 正常 execve 给不出空指针；真遇到就当一条空参数，让它走"空参数"那一条既有判据，
            // 而不是让入口先崩。
            tokens.PushBack(std::wstring());
            continue;
        }
        const std::string bytes(raw);
        const Utf8Decoded decoded = DecodeUtf8(bytes.data(), bytes.size());
        // 不合的地方不丢：文本里留下 U+FFFD，同时把第一处坏字节的位置记下来，
        // 由解析层逐条报 cli.invalid_utf8（detail 里给那个偏移）。
        tokens.PushBack(decoded.text,
                        decoded.WellFormed() ? std::wstring::npos : decoded.firstBadOffset);
    }
    return tokens;
}

}  // namespace ecapture
