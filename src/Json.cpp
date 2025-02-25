#include "Json.h"

#include <string>

namespace ecapture {

std::wstring Json::Escape(const std::wstring& in) {
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(in.size() + 8);
    for (wchar_t c : in) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\b': out += L"\\b"; break;
            case L'\f': out += L"\\f"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20) {
                    out += L"\\u00";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(c);   // 含中文：原样保留，输出层负责写 UTF-8 字节
                }
        }
    }
    return out;
}

void Json::NewlineIndent() {
    out_.push_back(L'\n');
    out_.append(stack_.size() * 2u, L' ');
}

// 元素之间补逗号并换行缩进（根元素没有容器，直接跳过）
void Json::BeforeElement() {
    if (stack_.empty()) return;
    Scope& top = stack_.back();
    if (top.hasItem) out_ += L',';
    top.hasItem = true;
    NewlineIndent();
}

// 紧跟 Key() 之后的值只需要 ": "，其余情况按新元素处理
void Json::Prelude() {
    if (pendingKey_) {
        out_ += L": ";
        pendingKey_ = false;
        return;
    }
    BeforeElement();
}

void Json::Open(bool isObject, wchar_t bracket) {
    Prelude();
    out_.push_back(bracket);
    stack_.push_back(Scope{isObject, false});
}

Json& Json::Obj() {
    Open(true, L'{');
    return *this;
}

Json& Json::Arr() {
    Open(false, L'[');
    return *this;
}

Json& Json::End() {
    if (stack_.empty()) return *this;
    const Scope closed = stack_.back();
    stack_.pop_back();
    if (closed.hasItem) NewlineIndent();
    out_.push_back(closed.isObject ? L'}' : L']');
    return *this;
}

Json& Json::Key(std::wstring name) {
    BeforeElement();
    out_.push_back(L'"');
    out_ += Escape(name);
    out_.push_back(L'"');
    pendingKey_ = true;
    return *this;
}

Json& Json::Value(std::wstring text) {
    Prelude();
    out_.push_back(L'"');
    out_ += Escape(text);
    out_.push_back(L'"');
    return *this;
}

Json& Json::Value(bool b) {
    Prelude();
    out_ += b ? L"true" : L"false";
    return *this;
}

Json& Json::Value(long long n) {
    Prelude();
    out_ += std::to_wstring(n);
    return *this;
}

Json& Json::Value(unsigned long long n) {
    Prelude();
    out_ += std::to_wstring(n);
    return *this;
}

Json& Json::Raw(std::wstring literal) {
    Prelude();
    out_ += literal;
    return *this;
}

Json& Json::Null() {
    Prelude();
    out_ += L"null";
    return *this;
}

}  // namespace ecapture
