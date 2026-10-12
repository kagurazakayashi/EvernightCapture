// 【跨平台核心】多语言文案：表解析、占位符代入、语言标签与当前语言。
//
// 文案字节来自构建期生成的嵌入物（见 src/LangData.h），不再来自 Windows 的 RCDATA；
// 解码走 core/TextCoding 那一份严格 UTF-8。唯一还留在平台层的是"系统显示语言是哪一种"
// （src/platform/windows/WinLocale.cpp 与 src/platform/linux/PosixLocale.cpp）。
#include "Lang.h"

#include <map>

#include "LangData.h"
#include "core/TextCoding.h"

namespace ecapture {
namespace {

// 语言 -> 嵌入名。这张表只在这里写一次，生成脚本与判据都按同一组名字对账。
const char* BlobName(Language lang) {
    switch (lang) {
        case Language::kZhCn: return "ECP_TEXT_ZH_CN";
        case Language::kZhTw: return "ECP_TEXT_ZH_TW";
        case Language::kJa:   return "ECP_TEXT_JA";
        case Language::kEn:   return "ECP_TEXT_EN";
    }
    return "ECP_TEXT_EN";
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
    const LangBlob* blob = FindLangBlob(BlobName(lang));
    if (!blob && lang != Language::kEn) {
        // 该语言整份缺失时退回英语，至少输出的是能读的句子
        blob = FindLangBlob(BlobName(Language::kEn));
    }
    if (!blob || !blob->bytes || blob->size == 0) return {};
    // 严格解码：源文件坏了也只会看见 U+FFFD，不会丢句子。合法性由离线判据
    // （ecapture-cli-text-tests）逐份查，那里把"嵌入物里有非法 UTF-8"当成构建失败。
    return ParseTable(DecodeUtf8(blob->bytes, blob->size).text);
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

// 与 Msg 同一份表、同一次惰性加载，只是**不**把"没有这个 key"做成一句可见的文字。
// 给可选的那一句用（例如诊断的 hint：有 <code>_hint 就带，没有就整个字段省略）。
bool HasMsg(const wchar_t* key) {
    if (!key || !*key) return false;
    Cache& cache = Active();
    if (!cache.loaded) {
        cache.table = LoadTable(cache.lang);
        cache.loaded = true;
    }
    return cache.table.find(key) != cache.table.end();
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
    // 标签本体恒为 ASCII（BCP 47 的字母与 - _ 分隔符），所以折叠只看 ASCII：
    // 不跟系统语言、也不跟 C 运行期 locale 走（那是 towlower 会连带改掉非 ASCII 字符的来源）。
    std::wstring t;
    t.reserve(tag.size());
    for (wchar_t ch : tag) {
        if (ch == L'_' || ch == L' ') ch = L'-';
        t.push_back(AsciiLowerChar(ch));
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

}  // namespace ecapture
