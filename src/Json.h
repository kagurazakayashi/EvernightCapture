#pragma once
// 极简 JSON 生成器：本项目只输出 JSON，不解析 JSON。
//
// 链式写法，Obj()/Arr() 打开容器，End() 闭合，Key() 只能出现在对象里：
//   Json j;
//   j.Obj().Key("ok").Value(true).Key("hwnd").Arr().Value(L"0x1").End().End();
//   j.Str();  // 完整文档
//
// 输出规则固定：2 空格缩进、键值之间一个冒号加空格、非 ASCII 原样输出为 UTF-8、
// 控制字符转 \u00XX。同一份输入永远得到逐字节相同的输出，便于测试比对。

#include <string>
#include <vector>

namespace ecapture {

class Json {
public:
    Json() = default;

    Json& Obj();                     // 打开对象
    Json& Arr();                     // 打开数组
    Json& End();                     // 闭合最近的容器

    Json& Key(std::wstring name);    // 对象的键
    Json& Value(std::wstring text);
    Json& Value(const wchar_t* text) { return Value(std::wstring(text)); }
    Json& Value(bool b);
    Json& Value(long long n);
    Json& Value(unsigned long long n);
    Json& Value(int n) { return Value(static_cast<long long>(n)); }
    // 这两个宽度在两个平台上不是同一回事：Windows(LLP64) 的 long 是 32 位，
    // Linux(LP64) 的 long 是 64 位 —— 于是 uint64_t / int64_t 在 Linux 上就是 unsigned long /
    // long，只给 long long 那一对重载会撞上"两个候选都要转换"的二义性。
    // 各补一条并窄化到上面那两条，两个平台对同一个数值都只有一条出路。
    Json& Value(long n) { return Value(static_cast<long long>(n)); }
    Json& Value(unsigned long n) { return Value(static_cast<unsigned long long>(n)); }
    Json& Null();
    Json& Raw(std::wstring literal);  // 预先格式化好的数字等，调用方保证合法

    std::wstring Str() const { return out_; }

    static std::wstring Escape(const std::wstring& text);

private:
    struct Scope {
        bool isObject;
        bool hasItem;
    };

    void NewlineIndent();
    void BeforeElement();
    void Prelude();       // 值/容器开始前的分隔处理
    void Open(bool isObject, wchar_t bracket);

    std::wstring out_;
    std::vector<Scope> stack_;
    bool pendingKey_ = false;
};

// 值非空才写出这一对键值 —— 精简契约里"没内容就整个键不出现"是 everywhere 的同一条规矩
// （--show-config、input 回显、诊断都靠它）。写一次在 JSON 这一层，比在十几个调用点各判
// 一遍空更不容易漏，也保证了同一份数据在两条渲染路径上得到同一种省略方式。
inline Json& OptString(Json& j, const wchar_t* key, const std::wstring& value) {
    if (value.empty()) return j;
    return j.Key(key).Value(value);
}

}  // namespace ecapture
