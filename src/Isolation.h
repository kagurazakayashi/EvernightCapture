#pragma once
// 【平台接口】阻塞调用隔离：带期限的"整步交给辅助进程"入口与失败映射。
//
// 为什么提成单独一份：条件求值（std::regex 的编译与回溯、给每扇窗口取标题）没有可查的
// 中断点，只有把整步放进本工具自己的辅助进程、到点结束**自己的**那个进程，--timeout-ms
// 才真的管得到它。这条路线同时被截图流水线（src/Capture.cpp）与只读窗口查询
// （src/WindowQueryRun.cpp）使用，两处必须走同一份判据 —— 所以入口声明放在共用层，
// 实现在平台层（Windows：src/platform/windows/Worker.cpp 的 Transaction；Linux 后端的
// 辅助进程链同一份核心报头与期限算式，见 core/WorkerFraming.h、core/HelperTiming.h）。
//
// 三条边界（与 Worker.h 顶部同源）：
//   1. 辅助进程只做"读某个窗口自己的画面"与"把顶层窗口列一遍"，**永不读桌面像素**；
//      会拍到桌面 / 别的窗口的路径留在父进程，由 ConsentGate 签发凭证之后才动。
//   2. 界限由调用方给：dl 是"这一次运行还剩多少"的同一份预算，本层不重新领一份总额。
//   3. 失败照实交回稳定码，不把"期限到点"与"辅助进程没起来"混成一句 capture.failed。

#include <string>
#include <vector>

#include "CliOptions.h"     // Diagnostic / MatchOptions / codes:: / stages::
#include "Deadline.h"
#include "core/Error.h"     // NativeErrorCode
#include "core/Geometry.h"  // PhysRect（按屏过滤的矩形，线上格式已改成中性类型）
#include "core/Status.h"    // BlockedStatus
#include "WindowMatch.h"    // WindowInfo

namespace ecapture {

// 把条件求值整步交给辅助进程：编一遍正则、枚举顶层窗口、逐个取值并求值。
// 成功返回 true；失败（期限到点 / 正则被本机上限挡下 / 辅助进程坏了）填 *err 返回 false。
// 失败时 hits / iconic 不动（绝不交回半套命中列表冒充"就这些窗口"）。
bool IsolatedMatch(const MatchOptions& match, const std::vector<PhysRect>& onScreens,
                   const Deadline& dl, std::vector<WindowInfo>* hits,
                   std::vector<WindowInfo>* iconic, Diagnostic* err);

// 一步"会被卡住的调用"失败 -> 结构化诊断。本进程内跑的退路与辅助进程报回来的失败共用
// 这一份映射，所以同一种失败在两条路上传出去的文字与码完全一致。
// win32 / hresult 是那两个原生码的**数值承载**（core/Error.h），0 = 该来源没有失败过。
Diagnostic BlockedToDiagnostic(BlockedStatus status, NativeErrorCode win32,
                               NativeErrorCode hresult, const std::string& detail,
                               const wchar_t* backend, const wchar_t* stage);

}  // namespace ecapture
