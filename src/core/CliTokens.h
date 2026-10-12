#pragma once
// 跨平台核心：命令行参数的交接物 —— 入口与解析之间只有这一个说法。
//
// 平台入口负责把系统的 argv 变成它（这是全仓唯一一次"从系统的文字表示换成本项目的文字表示"）：
//   * Windows（wmain）：wchar_t** 交给进程的已经是 UTF-16 码元序列，与内部约定同一种，
//     逐条原样搬过来，不做任何再编码（"转一次再转回来"正是把参数弄坏的起点）。
//   * Linux（main）：argv 是字节，按 UTF-8 严格解码（src/core/TextCoding.h）。不合的那几条
//     **照样带着**，但记下这条里第一处不可用字节的偏移：解析层据此逐条报
//     cli.invalid_utf8 并失败退出，绝不把 U+FFFD 当成一次合法的标题、路径或正则去匹配 ——
//     那等于把用户的原话悄悄换成另一句话。
//
// items_[0] 是程序名那一条（Windows 由 CRT 给，Linux 是 argv[0]），解析从下标 1 起读，
// 与 argc/argv 的既有习惯一致；调用方不再需要 argc。
//
// 这一层不收 wchar_t*，也不收 char*：中间一旦允许两种表示并存，"哪一边负责换码"就又变成
// 各调用点自己决定了。

#include <cstddef>
#include <string>
#include <vector>

namespace ecapture {

class CliTokens {
public:
    CliTokens() = default;

    // badOffset = npos 表示这一条整份合；否则它是这条原文里第一处不可用字节的偏移。
    void PushBack(std::wstring token, std::size_t badOffset = std::wstring::npos) {
        items_.push_back(Token{std::move(token), badOffset});
    }

    std::size_t Count() const { return items_.size(); }
    // 下标越界时交回空串：解析循环都按 Count() 走，这里不越界读，也不为此多写一份分支。
    const std::wstring& At(std::size_t i) const {
        static const std::wstring kEmpty;
        return i < items_.size() ? items_[i].text : kEmpty;
    }

    bool HasInvalidUtf8() const {
        for (const Token& t : items_) {
            if (t.invalidOffset != std::wstring::npos) return true;
        }
        return false;
    }
    // 这一条的坏字节偏移；这一条合时返回 npos。
    std::size_t InvalidOffset(std::size_t i) const {
        return i < items_.size() ? items_[i].invalidOffset : std::wstring::npos;
    }

private:
    struct Token {
        std::wstring text;
        std::size_t invalidOffset = std::wstring::npos;
    };

    std::vector<Token> items_;
};

}  // namespace ecapture
