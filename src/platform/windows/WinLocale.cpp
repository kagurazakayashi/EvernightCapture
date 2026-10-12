// 【平台层 · Windows】系统显示语言的探测。
//
// 这是 DetectSystemLanguage() 的 Windows 答案，共用层（src/Lang.cpp）只认它交回来的语言枚举。
#include "Lang.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {

Language DetectSystemLanguage() {
    // 「系统语言」取 Windows 显示语言（设置里那一项，LCID 如 0x0804）。
    // 不用 GetUserPreferredUILanguages 的首选语言列表：那是给应用做 MUI 回退用的，
    // 和用户在显示器上看到的界面语言可以不一样；.NET 的 CurrentUICulture 更不可靠
    // （实测一台显示语言为 zh-CN 的机器上它是 en-US）。
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (LCIDToLocaleName(GetUserDefaultUILanguage(), name, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        if (const auto lang = LanguageFromTag(name)) return *lang;
    }
    return Language::kEn;
}

}  // namespace ecapture
