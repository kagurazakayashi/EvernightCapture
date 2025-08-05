#pragma once
// 条件求值的**内部**形状：把已解析的匹配条件编译一次，再对一条候选窗口逐类判定。
//
// 这一份本来只住在 src/WindowMatch.cpp 的匿名命名空间里。放出来的唯一理由是判据：
// "跨类 AND、同类 OR"这条语义最要紧的现场是**某一类自己不成立而别的类成立**，
// 那需要注入一条假候选窗口，真机上既凑不稳也没法安排阴性对照。所以生产枚举
// （EnumerateMatches）与离线判据共用这里这两个函数，测试不许另抄一套匹配算法。
// 它不是 ECAPTURE.EXE 对外的接口：只有 WindowMatch.cpp 与 tests 包含本头。

#include <regex>
#include <string>
#include <vector>

#include "WindowMatch.h"

namespace ecapture {

// 编译后的条件：与用户写法一一对应，但已经做完大小写折叠、--process 补 .exe、
// 以及"一条正则只编译一次"（正则构造很贵，不能对每扇窗口重复做）。
// 空的那一类不参与判定 —— 一个条件都没给时所有窗口都算命中。
struct CompiledConditions {
    std::vector<uint64_t> hwnds;
    std::vector<uint32_t> pids;
    std::vector<std::wstring> processes;      // 已小写
    std::vector<std::wstring> exePaths;       // 已小写
    std::vector<std::wstring> titles;
    std::vector<std::wstring> titleContains;
    std::vector<std::wstring> classes;        // 已小写
    std::vector<std::wregex> titleRegexes;
    std::vector<RECT> onScreens;              // --monitor 的屏幕限制；空 = 不限
};

// 求值中途的 regex_error：异常不许穿出 EnumWindows 那条回调边界，所以就地换成原因码。
// 由 MatchesWindow 在回溯复杂度超限（或本机正则库拒绝跑完）时填写。
struct RegexFault {
    bool hit = false;
    BlockedStatus status = BlockedStatus::kOk;
    std::string detail;
};

// 把条件编译成 CompiledConditions。返回 false 表示有一条正则本机标准库拒绝编译，
// 原因码与 ASCII 细节写进 status / detail（人话文案由调用方按 --lang 现取）。
bool CompileConditions(const MatchRequest& req, CompiledConditions* out,
                       BlockedStatus* status, std::string* detail);

// 对一条候选窗口判定：每一类独立成立才算命中，类与类之间不共享任何中间结果。
// 判的是标题、映像名、类名、句柄、PID 这些**条件**；可见性与 --monitor 限缩由调用方
// 另外判（OnAnyScreen 与枚举那一段），因为最小化那批只为提示收集。
bool MatchesWindow(const CompiledConditions& c, const WindowInfo& w, RegexFault* fault);

}  // namespace ecapture
