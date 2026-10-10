#pragma once
// 跨平台核心：错误码承载与三值问答结果。
//
// 两件事写在这一层，因为它们都是"一条问句的下场"，与各平台怎么问出来无关。
//
// NativeErrorCode：平台原生错误码的**数值承载**。
//   * Windows 上这一格放的是 GetLastError 的 DWORD 或一次调用自己的 HRESULT；两者都是
//     32 位无符号，0x80070005 这类值原样落得下，所以从原生 API 交回来只要一次显式转换。
//   * 将来的 Linux 后端在同一格里放 errno 或 X11 的 error_code。
//   * 字段**名字**负责说明它是哪一种来源（win32 / hresult / monitorWin32……），这一层不另造
//     一个"来源枚举"：既有结构本来就按来源命名，加一份平行说法只会变成第二份要同步的表。
//   * 0 = 不适用 / 没失败过。这与 Diagnostic.win32 一直以来的约定相同（那份 JSON 模型本来就用
//     uint32_t，本次只是把函数签名上的 DWORD 收到同一个说法上）。
//   * 原值只做**核对**用，不做分支：调用方按稳定的 codes:: 那一路分支，不读这里的数字。
//     所以平台换了、码的取值空间变了，都不影响对外契约；跨平台时也不许拿 Windows 的数
//     去比 Linux 的数（同一个 5 在两边是完全不同的两条错误）。
//
// Tri：一条问句的三值下场（yes / no / unknown）。原先写在 EnvReport.h 里，搬到这一层是因为
//   它不只服务于 --capabilities：身份复核、HDR 显示模式、光标开关、屏幕拓扑、编码器登记，
//   凡"问过但没答案"的地方都要它能写 unknown，绝不折成 yes 或 no 蒙混
//   （AGENTS.md：字段不可获取时报告未知或不支持，不编造默认值）。
//   取值词表（"yes"/"no"/"unknown"）仍然只在 EnvReport 那一路写一次（TriName），这里只放枚举本体。

#include <cstdint>

namespace ecapture {

using NativeErrorCode = uint32_t;

inline constexpr NativeErrorCode kNativeErrorNone = 0;

// 一次问句的三值结果。
enum class Tri { kYes, kNo, kUnknown };

}  // namespace ecapture
