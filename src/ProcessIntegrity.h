#pragma once
// EvernightCapture - 本进程自己的完整性级别（不是目标窗口的，也不是操作系统的版本）
//
// 为什么要有这一层：把 ECAPTURE.EXE 放进一个被显式降级过的目录（`icacls <dir>
// /setintegritylevel Low`，或者继承自父目录的那条强制完整性标签）之后，进程就以低完整性启动，
// 于是同一份二进制会在一台本来完全支持的机器上出现三类彼此无关的失败：
//   * `wgc` 的 `GraphicsCaptureItem.CreateForWindow` 回 `E_ACCESSDENIED`（0x80070005）；
//   * `printwindow` 那条要往目标窗口发绘制请求，而 UIPI 不让低完整性进程向高完整性窗口发消息，
//     于是 PrintWindow 直接失败并留下错误码 5；
//   * 连**像素已经拿到手**之后那一步也可能失败：低完整性进程不能在中完整性的目录里建文件，
//     于是写盘报 `io.write_failed` + 5（图已经在内存里了）。
// 这三条原本只会各自交回一句"采集失败 / 写失败"，调用方据此去换后端、加期限、重装编码器，
// 全是白做功。这一层把"本进程跑在哪个完整性级别"这一问单独答出来，判据只写这一份：
// --capabilities / --diagnostics 报出这一事实，截图失败时也由同一个判据决定要不要补那句可操作的提示。
//
// 两条规矩，与 src/EnvReport.h 里"只报这个事实，不断言截不到"同源：
//   1. **这一问没答案时不折成任何一边。** 令牌问不出来就是 kUnknown，既不按"够用"处理，
//      也不凭空扣一顶"被降级了"的帽子。
//   2. **只报级别，不把某条通道判成 unavailable。** 本机实测：低完整性下 `dwm.thumbnail`、
//      `bitblt.screen`、`duplication.frame` 都照常出图，被挡的是 `wgc` 与 `printwindow` 那两条
//      以及"往中完整性目录落盘"这一件事。这是三次现场观察，不是一条可以外推的 API 契约，
//      所以 status 那一段照旧由版本下限与屏幕拓扑判，完整性只作为一条 caveat 与失败时的提示出现。
// 完整性级别不能替用户提升，也不构成"请以管理员身份运行"的建议：低完整性是那个目录的标签
// 决定的，要改的是标签或者文件所在的位置，不是给这个 exe 加权限。

#include <cstddef>
#include <vector>

#include "CliOptions.h"  // Diagnostic、codes::、stages::

namespace ecapture {

// 进程令牌的强制完整性级别。名字对应 Windows 的那几个标准标签 RID
// （GetTokenInformation(TokenIntegrityLevel) 交回的那条 SID 的最后一个子授权）；
// kUnknown = 这一问没答案（问不成、回空指针、或回一个本工具没登记过的取值）。
// 令牌上没有完整性标签那种形状不在这里出现：那道问答不出结果，不会回一个"无标签"的值，
// 所以这一层不发明自己拿不到的取值。
enum class Integrity {
    kUnknown,
    kUntrusted,
    kLow,
    kMedium,
    kHigh,
    kSystem,
    kProtectedProcess,
};

// JSON / 提示里用的稳定 ASCII token（不随 --lang 变）。
const wchar_t* IntegrityName(Integrity value);

// 令牌里那条标签 SID 的最后一个子授权 -> 级别。数字一律取 SDK 的 SECURITY_MANDATORY_*_RID
// 符号（winnt.h），不在这里自己写一遍 0x2000 那种十六进制：本工具踩过的坑正是"照印象写表"，
// 一档之差会把 Medium 报成 Low、把 Low 报成 Untrusted，而两边看起来都像"有个答案"。
Integrity IntegrityFromLabelRid(uint32_t rid);

// 本进程问一遍（GetTokenInformation(TokenIntegrityLevel)）。不读用户名、不读 SID 归属、
// 不开文件、不枚举目录，所以这条问答可以直接放进那份"隐私自述"里说得通的只读查询。
// `probeError` 可以为空；非空时带走这一问为什么没能给出答案：`OpenProcessToken` 或
// `GetTokenInformation` 失败时的 GetLastError 原值，0 = 那两步都成功（成功却回了一个本工具
// 没登记过的标签取值时同样是 0，级别仍是 unknown）。与窗口查询里 readability 那条规矩同源：
// 问不出来要带着成因，调用方才分得开"这一问没跑成"与"这一问回了个读不懂的值"。
Integrity ProbeProcessIntegrity(uint32_t* probeError);

// 低于中完整性（也就是会被 UIPI 与强制完整性规则挡下来的那一档）。
// kUnknown 与 kMedium 以上都是 false：问不出来不等于被挡。
bool IntegrityBelowMedium(Integrity value);

// 这一条诊断是不是"系统说不让"那一类：HRESULT 0x80070005，或者 Win32 错误码 5。
// 只认这两个原值，不看 message（message 随 --lang 变，而且"受保护内容"那类失败另有成因）。
bool IsAccessDenied(const Diagnostic& diag);

// 纯判据 + 就地补写：本进程低于中完整性时，给这一轮里每一条 access-denied 类的
// 采集 / 写盘失败补上可操作的 hint，并最多追加一条 note.low_integrity 说明整件事。
// 返回补了 hint 的条数（0 = 这一档不成立，或这一轮里没有一条对得上）。
// 不改动 code、stage、退出码，也不删任何已经记下的交付事实：低完整性解释的是"为什么被拒"，
// 不是"这一次什么都没发生"。
size_t AnnotateIntegrityDenials(Integrity integrity, std::vector<Diagnostic>* errors,
                                std::vector<Diagnostic>* notes);

}  // namespace ecapture
