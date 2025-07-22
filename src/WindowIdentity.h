#pragma once
// 目标身份复核：确认「手上这个 HWND 现在还是当初选中的那一扇窗口」。
//
// 为什么需要它：条件求值（枚举窗口）与真正取帧之间隔着输出名规划、人工确认框（人可能想几秒
// 才点，点完之后还有约 1 秒关闭动画）、以及 auto 回退链的每一次尝试。这段时间里目标可以被销毁、
// 进程可以退出而 HWND 值被另一扇窗口复用 —— 单靠一个 64 位整数区分不了「还是它」与
// 「一个长得像它的新对象」。而用户批准的是前者。
//
// 判据分两档，按"这一问会不会把本进程拖住"划分，而不是按重要性：
//
//   kCheap —— 每一条通道的每一次尝试之前都做（授权之后、读像素之前还再做一次）：
//     * IsWindow(hwnd)                 句柄还有效吗
//     * GetWindowThreadProcessId       还属于当初那个进程吗
//     * 那个 PID 的进程创建时间          PID 会被复用，光比 PID 不够（GetProcessTimes）
//     * GetClassNameW                  类名对得上吗（同一进程重建了另一类窗口也是换对象）
//   这四问的答案都在 user32 / kernel32 自己那份结构里，**不往目标线程发消息**，所以问得起、
//   也卡不住 —— 这一点是硬要求：本工具对期限的承诺是"能停下来的都要停下来"。
//
//   kFull —— 每个目标开工之前做一次，以及任何要升级到桌面像素之前（dwm 的屏幕退路）：
//     上面四条 + **拿当初那份选择条件重新求值一次，这个句柄还在不在命中列表里**。
//     标题这类易变属性就是这么处理的：不逐字比较快照（应用刷新标题是正常现象，那不该被当成
//     换了目标），而是问"当初挑中它的理由现在还成立吗"。求值复用条件求值那同一套 machinery
//     （WindowMatch.h / Worker.h），所以 --title-regex 那种没有中断点的匹配照旧要么在原处
//     本进程跑、要么进辅助进程，**不会因为要复核就多出一种跨进程发消息的新等待**。
//
// 三条边界，别说过头：
//   1. 这条复核**降低**竞态窗口，不声称完全消除。判据与取帧之间不是原子的 —— 真要原子，
//      得让内核替我们持有这个对象，而 HWND 既不是可等待对象，也没有"锁住一个窗口"的公开 API。
//   2. 查不到就是查不到：任何一问没给出答案就按 capture.target_unverifiable 拒绝取帧，
//      绝不把"没发现不同"当成"相同"。基线当时就没有的那一条（例如进程创建时间读不到）整个跳过，
//      那是一次**没做出来**的判定，不是失败的判定 —— 否则别人以管理员身份开的窗口就再也截不了。
//   3. 身份变了**不去找替代目标**：不重新消歧、不改取别的窗口、也不放宽条件再挑一次。
//      用户针对旧对象给出的许可不转移给新对象（Consent.h 第 6 条同一件事）。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CliOptions.h"

namespace ecapture {

struct WindowInfo;      // WindowMatch.h：枚举出来的一条候选
struct MatchOptions;    // CliOptions.h：用户给的那些条件

// ---------------------------------------------------------------------------
// 选定那一刻的身份快照
// ---------------------------------------------------------------------------
struct WindowIdentity {
    uint64_t hwnd = 0;
    uint32_t pid = 0;
    // 归属进程的创建时间（100 纳秒 ticks，FILETIME 原值）。0 = 选定当时读不到 -> 这一条整个跳过。
    uint64_t processStartTicks = 0;
    std::wstring className;
    std::wstring title;       // 只为诊断与人看的那一行留着，复核不逐字比它
    // 当初使它成为目标的判据里有没有看标题的那一条（--title / --title-contains / --title-regex，
    // 也包括 --monitor 那种"按屏过滤"）。为真时 kFull 要重跑一次条件求值；为假时不用 ——
    // 因为剩下的那些条件要么是句柄本身、要么是 PID / 映像 / 类名，全都已经被 kCheap 那四问覆盖。
    bool selectionNeedsRecheck = false;
};

// 从选定结果与原始条件做出快照。进程创建时间取自枚举那一步（WindowInfo），**不是**在这里
// 才去问 —— 事后补问等于自己跟自己对答案：中间那次销毁重建会被当成基线记下来。
WindowIdentity MakeWindowIdentity(const WindowInfo& selected, const MatchOptions& match,
                                  bool monitorGiven);

// ---------------------------------------------------------------------------
// 查询层：复核要问的那几件事。真机用 SystemWindowQueryLayer() 再补上重跑条件那一条；
// 测试注入假答案，才能把「句柄被另一个进程复用」「PID 相同但那是另一个进程」这类
// 在真机上安排不出来的现场逐条判完。
// 某条函数为空 / 返回 false = 这一问答不出来 -> capture.target_unverifiable，不是跳过。
// ---------------------------------------------------------------------------
struct WindowQueryLayer {
    std::function<bool(uint64_t hwnd)> alive;
    std::function<bool(uint64_t hwnd, uint32_t* pid)> windowPid;
    std::function<bool(uint64_t hwnd, std::wstring* out)> windowClass;
    std::function<bool(uint32_t pid, uint64_t* outTicks)> processStart;
    // kFull 那一问：拿当初那份条件重新求值，这个句柄还在不在命中列表里。
    // 返回值 = 问出来没有；*matched = 它是否仍然命中。问不出来时 *matched 无意义。
    std::function<bool(uint64_t hwnd, bool* matched)> selectionStillMatches;
};

// 真机的 kCheap 那四问。selectionStillMatches 留空 —— 它要拿条件与期限才能跑，
// 那两个东西只有流水线知道，所以由 Capture.cpp 把这一问补上（见 MakeTargetQueryLayer）。
WindowQueryLayer SystemWindowQueryLayer();

// 复核结果。四种都是终局判据，调用方据此决定"取不取这一帧"。
enum class IdentityVerdict {
    kSame,          // 所有可用判据都指向同一个目标 —— 可以取帧
    kGone,          // 句柄已经失效：目标在本次请求中被销毁
    kChanged,       // 还是这个句柄值，但它已经是另一个对象（归属进程 / 类名 / 选择条件）
    kUnverifiable,  // 有一条判据问不出来，无法确认还是同一个目标
};

// 复核做到哪一档，见本文件顶部。
enum class IdentityScope { kFull, kCheap };

struct IdentityFault {
    IdentityVerdict verdict = IdentityVerdict::kSame;
    // ASCII 的机器细节（"pid 1234 -> 5678" 这种），进 message 的 %1。
    // 保持 ASCII 是有意的：四种语言共用同一条 message 模板，细节不必翻译。
    std::string detail;
};

// 对着快照逐条问。顺序按"便宜且严"排在前面，任何一条不成立就立刻交出去，不再问后面那些
//（后面的答案不会改变结论）。每条"问不出来"都按 kUnverifiable 交出去，而不是当成"没发现不同"。
IdentityVerdict CheckWindowIdentity(const WindowIdentity& id, const WindowQueryLayer& query,
                                    IdentityScope scope, IdentityFault* fault);

// 复核结果 -> 稳定诊断（capture.target_gone / capture.target_changed /
// capture.target_unverifiable，stage=capture，target=0x…。退出码沿用 7）。
Diagnostic IdentityDiagnostic(IdentityVerdict verdict, const std::string& detail, uint64_t hwnd);

// 流水线用的那一句：通过返回 true；否则填好 *err 并返回 false（调用方一张都不该截）。
bool VerifyWindowIdentity(const WindowIdentity& id, const WindowQueryLayer& query,
                          IdentityScope scope, Diagnostic* err);

// 快照 + 查询层绑在一起，交给每一条通道。理由与 DesktopPermit 同一条思路：
// "复核"这件事只有带着查询层才做得出来，所以通道想跳过判断就没有可跳过的东西 ——
// 传 uint64_t 句柄进去时"记得检查一下"是规矩，传这个进去时是编译期。
struct WindowTarget {
    WindowIdentity id;
    WindowQueryLayer query;

    // 复核一次。scope 见本文件顶部：每一次实际取帧之前用 kCheap，目标开工与升级到桌面之前用 kFull。
    bool Recheck(IdentityScope scope, Diagnostic* err) const {
        return VerifyWindowIdentity(id, query, scope, err);
    }
    uint64_t hwnd() const { return id.hwnd; }
};

// 给测试用：把判据结果换成机器名（"same" / "gone" / "changed" / "unverifiable"）。
const char* IdentityVerdictName(IdentityVerdict verdict);

}  // namespace ecapture
