#include "Lang.h"

#include <cwctype>
#include <map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {
namespace {

// resources/ecapture.rc 里的资源名：一份语言一个名字（rc.exe 的 LANGUAGE 子句不接受
// 文件名形式的数据，CMake 的 RC 规则又会吞掉 rc 的 /l 开关，所以靠名字区分）
const wchar_t* ResourceName(Language lang) {
    switch (lang) {
        case Language::kZhCn: return L"ECP_TEXT_ZH_CN";
        case Language::kZhTw: return L"ECP_TEXT_ZH_TW";
        case Language::kJa:   return L"ECP_TEXT_JA";
        case Language::kEn:   return L"ECP_TEXT_EN";
    }
    return L"ECP_TEXT_EN";
}

std::wstring FromUtf8(const char* data, size_t size) {
    if (size == 0) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), out.data(), need);
    return out;
}

// 一行一条：key<TAB>文案；# 开头是注释，空行忽略。文案里不允许出现 TAB/换行，
// 需要分行就拆成多条 key（帮助文本本来就是一行一个 key）。
std::map<std::wstring, std::wstring> ParseTable(const std::wstring& blob) {
    std::map<std::wstring, std::wstring> table;
    size_t pos = blob.starts_with(L"\uFEFF") ? 1 : 0;  // 编辑器存的 BOM 会让第一条 key 失效
    while (pos < blob.size()) {
        size_t end = blob.find(L'\n', pos);
        if (end == std::wstring::npos) end = blob.size();
        std::wstring line = blob.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        const size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos || tab == 0) continue;
        const std::wstring key = line.substr(0, tab);
        std::wstring text = line.substr(tab + 1);
        if (text.empty()) continue;
        table.emplace(key, std::move(text));
    }
    return table;
}

std::map<std::wstring, std::wstring> LoadTable(Language lang) {
    const HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return {};

    HRSRC found = FindResourceW(module, ResourceName(lang), RT_RCDATA);
    if (!found && lang != Language::kEn) {
        // 该语言整份缺失时退回英语，至少输出的是能读的句子
        found = FindResourceW(module, ResourceName(Language::kEn), RT_RCDATA);
    }
    if (!found) return {};

    const HGLOBAL loaded = LoadResource(module, found);
    const DWORD size = SizeofResource(module, found);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size == 0) return {};
    return ParseTable(FromUtf8(static_cast<const char*>(data), size));
}

struct Cache {
    Language lang = Language::kEn;
    std::map<std::wstring, std::wstring> table;
    bool loaded = false;
};

Cache& Active() {
    static Cache cache;
    return cache;
}

}  // namespace

namespace detail {

std::wstring Substitute(std::wstring text, const std::vector<std::wstring>& args) {
    std::wstring out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'%' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const wchar_t next = text[i + 1];
        if (next == L'%') {
            out.push_back(L'%');
            ++i;
            continue;
        }
        if (next >= L'1' && next <= L'9') {
            const size_t idx = static_cast<size_t>(next - L'1');
            if (idx < args.size()) {
                out += args[idx];
                ++i;
                continue;
            }
        }
        out.push_back(L'%');  // 不是占位符（文件名模板里的 %i / %h 等），原样留着
    }
    return out;
}

}  // namespace detail

void SetLanguage(Language lang) {
    Cache& cache = Active();
    if (cache.loaded && cache.lang == lang) return;
    cache.lang = lang;
    cache.table.clear();
    cache.loaded = false;
}

Language CurrentLanguage() { return Active().lang; }

std::wstring Msg(const wchar_t* key) {
    if (!key || !*key) return std::wstring();
    Cache& cache = Active();
    if (!cache.loaded) {
        cache.table = LoadTable(cache.lang);
        cache.loaded = true;
    }
    const auto it = cache.table.find(key);
    if (it != cache.table.end()) return it->second;
    return std::wstring(L"?") + key;  // 让写错的 key 在输出里显眼，而不是静默空白
}

const wchar_t* LanguageTag(Language lang) {
    switch (lang) {
        case Language::kZhCn: return L"zh-CN";
        case Language::kZhTw: return L"zh-TW";
        case Language::kJa:   return L"ja";
        case Language::kEn:   return L"en";
    }
    return L"en";
}

std::optional<Language> LanguageFromTag(const std::wstring& tag) {
    std::wstring t;
    t.reserve(tag.size());
    for (wchar_t ch : tag) {
        if (ch == L'_' || ch == L' ') ch = L'-';
        t.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    while (!t.empty() && t.back() == L'-') t.pop_back();
    if (t.empty()) return std::nullopt;

    const auto starts = [&](const wchar_t* p) {
        const std::wstring s(p);
        return t.size() >= s.size() && t.compare(0, s.size(), s) == 0;
    };
    const auto contains = [&](const wchar_t* p) { return t.find(p) != std::wstring::npos; };

    if (starts(L"ja") || t == L"jp") return Language::kJa;
    if (starts(L"en")) return Language::kEn;
    if (t == L"cht" || t == L"tw" || t == L"hk") return Language::kZhTw;
    if (t == L"chs" || t == L"cn") return Language::kZhCn;
    if (starts(L"zh")) {
        const bool traditional = contains(L"hant") || contains(L"-tw") || contains(L"-hk") ||
                                 contains(L"-mo");
        return traditional ? Language::kZhTw : Language::kZhCn;
    }
    return std::nullopt;
}

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
