#pragma once
// 多语言文案：文本取自程序自带的嵌入资源（resources/strings-*.txt 四份按语言命名的 UTF-8
// 文案，构建期由 scripts/gen-lang-data.cmake 生成一份 C++ 源文件编进程序，见 src/LangData.h），
// 运行时解析成 key -> 文案 的表。这一层不碰任何平台资源 API，两个平台跑同一份判据。
//
// key 是稳定的字符串（"cli.pid_range" / "opt.hwnd" / "cap.dwm.flat"），取不到时返回 "?key"，
// 这样写错 key 会在输出里留下看得见的痕迹，而不是静默变成空白。
// 占位符：%1 .. %9 按位置代入，%% 出一个 %；其它 %x（如文件名里的 %i）原样保留。
// 所以文案里的百分号要输出字面量时写成 %%。

#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace ecapture {

// 已提供文案的语言。默认跟随系统显示语言，判不出来时用英语。
enum class Language {
    kZhCn,  // 简体中文
    kZhTw,  // 繁体中文（台湾用语）
    kEn,    // 英语
    kJa,    // 日语
};

// 系统显示语言 -> 支持的语言；不支持的一律归到 kEn。
// 由**平台服务**实现（Windows 读显示语言 LCID，Linux 读 LC_MESSAGES / LANG 那套环境变量）：
// 判据是"这台机器的界面语言是什么"，各系统自己答，共用层不猜。
Language DetectSystemLanguage();

// "--lang" 的取值：大小写不敏感，'_' 与 '-' 等价，zh-TW / zhHant / cht / tw 都认。
// 不认识时返回 nullopt（调用方据此报错，绝不静默换成别的语言）。
std::optional<Language> LanguageFromTag(const std::wstring& tag);

// 规范写法，用于 --lang 的取值列表、错误提示与 -v 的 input 回显
const wchar_t* LanguageTag(Language lang);

// 解析参数前就要调好：解析期的报错也得用调用方指定的语言。
void SetLanguage(Language lang);
Language CurrentLanguage();

std::wstring Msg(const wchar_t* key);

// 这一份表里**有没有**这个 key。它存在的理由是给"可选的那一句"用的：例如诊断的补充说明
// 按约定有 <code>_hint 就带上、没有就整个字段省略，而 Msg() 对不存在的 key 返回 "?key"
// （那是给写错的 key 留的痕迹，不该被当成一句真的文案送出去）。
bool HasMsg(const wchar_t* key);

namespace detail {
inline std::wstring ToArg(const std::wstring& v) { return v; }
inline std::wstring ToArg(const wchar_t* v) { return v ? std::wstring(v) : std::wstring(); }
inline std::wstring ToArg(wchar_t v) { return std::wstring(1, v); }
template <typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
std::wstring ToArg(T v) {
    return std::to_wstring(v);
}

std::wstring Substitute(std::wstring text, const std::vector<std::wstring>& args);
}  // namespace detail

// Msgf(L"cap.wgc.timeout", 2000) => "超时未收到帧（2000 ms）"
template <typename... A>
std::wstring Msgf(const wchar_t* key, A&&... args) {
    std::vector<std::wstring> packed{detail::ToArg(std::forward<A>(args))...};
    return detail::Substitute(Msg(key), packed);
}

}  // namespace ecapture
