#pragma once
// 跨平台核心：一次"会卡住的调用"停在哪儿的原因码。
//
// 放这一层有两个原因：
//   1. 条件求值（WindowMatch）与取帧（CaptureCommon / RenderOutcome）共用同一份取值，
//      而前者不该为了拿一个状态码去 include Windows 的取帧头。
//   2. 数值是辅助进程管道上的线上格式（WorkerProtocol.h），它属于**协议**，
//      协议的字汇表要能被任何一侧读到，不能长在某个平台的头文件里。
//
// 【过渡桥接】下面这些取值里有一批名字带 Windows 通道的痕迹（kPrintWindowFailed、
// kDibCreate、kRegisterThumb…）。那是既有事实，本步不重命名：改名等于同时动线上格式与
// 四种语言的判据表，收益只有整齐。Linux 后端接进来时按同一条规矩**追加**自己的取值并升
// kProtocolVersion，不另起第二套状态码。

#include <cstdint>
#include <string>

namespace ecapture {

// 会卡住的那几步（PrintWindow / DWM 缩略图 / 条件求值里的正则回溯）失败在哪一步的原因码。
// 辅助进程只报这个码 + 系统错误码，**不报文字**：文案由父进程按 --lang 现场取，
// 于是四种语言都不必跟着协议走。数值是管道里的线上格式，改顺序或改含义要同步
// 改 WorkerProtocol.h 的 kProtocolVersion。
enum class BlockedStatus : uint32_t {
    kOk = 0,
    kRectEmpty = 1,          // 目标窗口矩形已经量不出来（窗口没了 / 正被销毁）
    kDibCreate = 2,          // 建 DIB 失败
    kPrintWindowFailed = 3,  // PrintWindow 带 flag 与不带 flag 两次都返回 FALSE（窗口自己画到 DC）
    kHostClass = 4,          // 注册 DWM 宿主窗口类失败
    kHostCreate = 5,         // 建宿主窗口失败
    kRegisterThumb = 6,      // DwmRegisterThumbnail 失败
    kUpdateProps = 7,        // DwmUpdateThumbnailProperties 失败
    kHostRectEmpty = 8,      // 宿主窗口自己的矩形量不出来
    kRegexInvalid = 9,       // 本机正则库拒绝编译（语法就在这里判：解析层不预编译，见 CliOptions.cpp）
    kBadTask = 10,           // 交来的任务不合法（尺寸 / 条数超限）
    kInternal = 11,          // 那一步抛了异常（ASCII 细节在 detail）
    kHostPrintWindowFailed = 12,  // 对宿主窗口的 PrintWindow 两次都返回 FALSE（dwm 通道）
    kRegexTooComplex = 13,  // 回溯复杂度超限：模式语法没问题，但这台机器的正则库拒绝把它跑完
};

// 状态码之外那条系统错误码（GetLastError 的原值 / 一次 COM 调用的返回值）仍然只在
// 产生它的那一侧与线上格式里说话：字段名（win32 / hresult）负责说明来源，本层不另立
// 一套"来源枚举"，也不在这里造第二份 ASCII 词表 —— 状态到稳定诊断码与文案的映射
// 一直只有 CliOptions / Lang 那一份，再加一份就是两份会互相打脸的事实。

}  // namespace ecapture
