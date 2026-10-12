// 【平台层 · Linux】系统显示语言的探测。
//
// 这是 DetectSystemLanguage() 的 Linux 答案。没有 Windows 那个"界面语言 LCID"可问，
// 桌面会话把语言放在环境变量里，所以按 locale(7) 的优先级读：LC_ALL > LC_MESSAGES > LANG。
// 取不到、或是 "C" / "POSIX" 这种"没有语言只有字节"的取值时，如实回到英语，
// 不拿 DISPLAY、也不拿桌面环境的 NAME 猜语言。
#include "Lang.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

const char* EnvNonEmpty(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// 环境变量里语言在第一位：zh_CN.UTF-8、en_US.UTF-8、ja_JP@japanese...
// 标签本体交给 LanguageFromTag（它认 '_'、忽略大小写、把 @ 之后的部分当成尾巴），
// 这里只负责判"这条说的是语言，还是只是 C 运行库的字节环境"。
bool IsPlainByteLocale(const char* v) {
    const std::string s(v);
    return s == "C" || s == "POSIX";
}

}  // namespace

Language DetectSystemLanguage() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = EnvNonEmpty(name);
        if (!value || IsPlainByteLocale(value)) continue;
        // LanguageFromTag 只在开头打分支，所以 "zh_CN.UTF-8" 与 "zh_CN" 得到同一个答案。
        if (const auto lang = LanguageFromTag(DecodeUtf8(value, std::strlen(value)).text)) {
            return *lang;
        }
    }
    return Language::kEn;
}

}  // namespace ecapture
