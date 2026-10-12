// 入口判据（Windows）：wmain 拿到的 wchar_t** → CliTokens。
//
// 与生产代码同一条 DecodeWindowsArgv（src/platform/windows/WinCliArgs.cpp）。
// Windows 交来的这一串本来就是 UTF-16 码元，与内部约定同一种，所以这里判的是
// "确实一次都没有重新编码"：内容逐字相等、且任何一条都不会被标成编码问题。
#include "../CliTextPlatformChecks.h"

#include <string>

#include "../../src/CliArgs.h"
#include "../../src/core/CliTokens.h"
#include "../../src/core/TextCoding.h"

using namespace ecapture;

void RunNativeArgvChecks(void (*check)(bool, const char*)) {
    {
        wchar_t argTitle[] = {L'-', L'-', L't', L'i', L't', L'l', L'e', 0};
        wchar_t argValue[] = {L'\u591c', L' ', L'\u7a7a', 0};
        wchar_t argPath[] = {L'D', L':', L'\\', L'\u622a', L'\u56fe', 0};
        wchar_t argEmpty[] = {0};
        wchar_t storage0[] = {L'e', L'c', L'a', L'p', L't', L'u', L'r', L'e', 0};
        wchar_t* argv[] = {storage0, argTitle, argValue, argPath, argEmpty};
        const CliTokens tokens = DecodeWindowsArgv(5, argv);
        check(tokens.Count() == 5, "Windows：五条参数收齐（含程序名那一条）");
        check(tokens.At(2) == std::wstring(argValue), "Windows：中文与空格逐码元原样搬");
        check(tokens.At(3).find(L'\\') != std::wstring::npos, "Windows：反斜杠路径保持原样");
        check(tokens.At(4).empty(), "Windows：空参数照样收进来，由解析层按空参数那一条判");
        check(!tokens.HasInvalidUtf8(), "Windows：入口不做再编码，所以没有编码问题可报");
    }
    {
        // 落单的高代理：Windows 的 argv 里真可能出现（调用方传进来什么就是什么）。
        // 入口把它原样带着，不"顺手修好"；到出口编 UTF-8 时才换成 U+FFFD（TextCoding 那条），
        // 两头各自的动作都只在自己那一步发生。
        wchar_t lone[] = {L'a', static_cast<wchar_t>(0xD800), L'b', 0};
        wchar_t storage0[] = {L'e', 0};
        wchar_t* argv[] = {storage0, lone};
        const CliTokens tokens = DecodeWindowsArgv(2, argv);
        check(tokens.At(1) == std::wstring(lone), "Windows：落单的代理码元照样原样保留");
        check(!tokens.HasInvalidUtf8(), "Windows：这一条不算编码问题（不臆造失败）");
        const std::string out = EncodeUtf8(tokens.At(1));
        check(out == "a\xef\xbf\xbdb", "Windows：到出口才换成 U+FFFD，不写半个序列");
    }
    {
        wchar_t storage0[] = {L'e', 0};
        wchar_t* argv[] = {nullptr};
        check(DecodeWindowsArgv(0, argv).Count() == 0, "Windows：argc=0 交回空表而不是崩");
        check(DecodeWindowsArgv(2, nullptr).Count() == 0, "Windows：argv 为空指针时交回空表");
        wchar_t* withNull[] = {storage0, nullptr};
        check(DecodeWindowsArgv(2, withNull).Count() == 2 &&
                  DecodeWindowsArgv(2, withNull).At(1).empty(),
              "Windows：真遇到空指针当空参数收，不让入口先崩");
    }
}
