#pragma once
// 隔离执行：把"会卡在别的进程里、又没有被设计成可中断"的那几个调用，交给本工具自己的
// 辅助进程去做，父进程只等一段有期限的时间；到点拿不到结果就结束**自己的**辅助进程。
//
// 为什么需要它：PrintWindow 是让目标窗口自己画到给它的 DC，它把消息发给那个窗口的线程并
// **同步等它画完**。那个线程要是正卡在自己的消息处理里（死循环、等一个永远不会来的事件），
// PrintWindow 就跟着卡死，而调用方给的超时参数在这条路上根本没有中断点 ——
// 旧实现就是这样把 --capture printwindow 的等待上限当成装饰。
// 同理，std::regex 对失控的模式没有中断点（--title-regex "(a+)+b" 配上几十个长标题就能把
// 一次"匹配"拖成几分钟），而长度限制不是执行期限，所以匹配这一步也整半交给辅助进程。
//
// 三条边界，改动前先对齐：
//   1. 辅助进程只做"读某个窗口自己的画面"和"把顶层窗口列一遍"这两类事，
//      **永不读桌面像素**、不写文件、不弹确认框。所以它不构成任何隐私旁路：
//      会拍到别的项目窗口的那几条路径（bitblt / duplication / 整屏 / dwm 的屏幕退路）
//      照旧留在父进程里，由 ConsentGate 签发凭证之后才动。--yes 的分级判断也一律在父进程做。
//   2. 辅助模式不是公共入口。它不在选项目录里、不出现在 --help 里，且必须验证"是谁起的我"：
//      起我的进程必须是同一个 ECAPTURE.EXE（映像路径一致）、是我的直接父进程、在同一会话里、
//      我活在一个"父进程一关作业我就一起结束"的作业里、消息带着本次的 nonce。
//      任一条不成立就拒绝执行，且不往任何标准流写一个字节。测试的注入也不在公共选项里
//      （协议编解码的离线判据在 tests\worker_protocol.cpp，走的是另一个测试可执行文件）。
//   3. 只结束自己起的那个进程：绝不用 TerminateThread，绝不结束目标应用，
//      绝不按映像名批量收尾。目标窗口只是"收到一次没人等的绘制请求"，它自己不受影响。
//
// 保证到什么程度（写明白，别说过头）：辅助进程被终止时它占的 GDI 对象随进程由系统回收；
// 父进程被结束时作业句柄关闭，内核把没退完的辅助进程一并带走，所以"长期遗留"不靠我们自己记得收尾。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"
#include "CliOptions.h"
#include "Deadline.h"
#include "WindowMatch.h"

namespace ecapture {

// ---------------------------------------------------------------------------
// 父进程侧：带期限的隔离调用。失败（含超时）时填 *err 并返回 false；
// err 里带真实的 backend（通道名）、stage，超时那条还带预算与实际耗时。
// ---------------------------------------------------------------------------
bool IsolatedPrintWindow(uint64_t hwnd, const Deadline& dl, CapturedFrame* out, Diagnostic* err);
bool IsolatedDwmThumbnail(uint64_t hwnd, uint32_t waitMs, const Deadline& dl, CapturedFrame* out,
                          Diagnostic* err);
bool IsolatedMatch(const MatchOptions& match, const std::vector<RECT>& onScreens,
                   const Deadline& dl, std::vector<WindowInfo>* hits,
                   std::vector<WindowInfo>* iconic, Diagnostic* err);

// 一步"会被卡住的调用"失败 -> 结构化诊断。本进程内跑的退路与辅助进程报回来的失败共用这一份
// 映射，所以同一种失败在两条路上传出去的文字与码完全一致。
Diagnostic BlockedToDiagnostic(BlockedStatus status, DWORD gle, HRESULT hr,
                               const std::string& detail, const wchar_t* backend,
                               const wchar_t* stage);

// 把"总预算"换算成一次隔离调用实际能等多久：给了 --timeout-ms 就用剩余预算，没给就用内置上限。
Deadline IsolatedWaitFor(const Deadline& dl);

// ---------------------------------------------------------------------------
// 辅助进程侧（同一个 exe 的工作模式）
// ---------------------------------------------------------------------------

// argv[1] 是不是工作模式标记。wmain 在解析选项**之前**问这一次，
// 因为工作模式不是选项，走正常解析只会被报成"未知选项"。
bool LooksLikeWorkerInvocation(const wchar_t* firstArg);

// 工作模式的入口：验证绑定 -> 收一条任务 -> 执行 -> 交回一条应答 -> 退出。
// 返回值是进程退出码（这个模式不进 JSON 契约：0 = 已交回结果，6 = 绑定不成立拒绝执行，
// 7 = 任务失败但已把失败原因交回，9 = 管道/协议不成立）。
int RunWorkerMode(int argc, wchar_t* const* argv);

}  // namespace ecapture
