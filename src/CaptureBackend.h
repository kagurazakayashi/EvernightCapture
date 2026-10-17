#pragma once
// 【平台接口】取帧后端的统一登记与调用口。
//
// 背景：Windows 现在有五条真实通道（WGC / DWM 缩略图 / PrintWindow / BitBlt / Desktop
// Duplication），每条通道有自己的能力边界与失败原因。此前流水线直接 include 五个通道头、
// 自己写 switch 分派 —— 那是"共用编排认识每一种平台实现"的形状，Linux 后端接进来时
// 只能再抄一份 switch。这一份把"通道是什么"与"怎么调它"收成一张登记表：
//
//   1. **登记表在平台层**（src/platform/windows/WinCaptureBackends.cpp）。共用层只拿
//      CaptureMethod 去问 FindCaptureBackend，拿到一条名字、窗口/屏幕两种目标支持与否、
//      以及两条内部路径名（像素来源仍由 CaptureScope 按路径判，不按通道名猜）。
//   2. **授权与期限是请求的一部分。** 窗口内容路径带目标身份快照与复核查询层（WindowTarget），
//      桌面路径必须带 ConsentGate 签发的 DesktopPermit；deadline 一律随请求传下去。
//      通道自己在内部临时改走屏幕取图的那条退路（dwm.screen）也要回来重新授权 ——
//      所以请求里同时带着 gate 与 targetKey，而不是让通道拿一个裸句柄就能读像素。
//   3. **失败原因照旧由通道自己给**：这里只负责分派，不把五种失败折成一句
//      capture.failed；Diagnostic 的 code / backend / win32 / hresult / detail 原样回传。
//
// 【平台接口】的边界：本头文件不 include 任何 Windows 头，也不出现 HWND / HMONITOR /
// HDC / ID3D11。目标标识是 WindowTarget（WindowIdentity.h）与 ScreenInfo（ScreenMatch.h），
// 两者都是平台无关形状，平台实现内部自己做一次看得见的转换。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"      // CaptureMethod / CursorRequest / HdrRequest / Diagnostic
#include "Consent.h"         // ConsentGate / DesktopPermit
#include "Deadline.h"
#include "core/Frame.h"      // CapturedFrame（共用帧形状，后端只往这里填像素）
#include "ScreenMatch.h"     // ScreenInfo
#include "WindowIdentity.h"  // WindowTarget

namespace ecapture {

// 一次窗口目标取帧的请求。哪些字段有意义由通道自己决定（例如 printwindow 不用光标、不用
// 桌面凭证；bitblt 必须带桌面凭证），但**身份复核与授权判定在流水线里就已经做完**：
// 通道拿到这份请求时，"这是不是当初选中的那扇窗口"与"人有没有批准这条路径"都已经判过，
// 这里再带着这些原料是为了 dwm 那条需要升级到桌面路径的内部退路能重新走一次授权。
struct CaptureWindowRequest {
    uint64_t hwnd = 0;                    // 目标句柄的数值（WindowIdentity.h 的中性承载）
    uint32_t timeoutMs = 0;               // 这条通道自己愿意等多久（等帧 / 泵消息）
    const WindowTarget* window = nullptr; // 身份快照 + 复核查询层（dwm 升级到桌面路径前要 kFull）
    ConsentGate* gate = nullptr;          // 授权判定器（只有需要重新授权的通道才用）
    const std::wstring* targetKey = nullptr;  // 诊断与授权里那个目标标识（"0x…"）
    const DesktopPermit* permit = nullptr;    // 桌面路径必需；没有它就只能走窗口内容路径
    const CursorRequest* cursor = nullptr;    // --cursor（只有 wgc 有真开关）
    const HdrRequest* hdr = nullptr;          // --hdr（wgc / duplication 的广色域来源）
    const Deadline* dl = nullptr;             // 这一次运行还剩的预算（守 tone mapping 等扫描）
};

// 一次屏幕目标取帧的请求。dwm / printwindow 只拍窗口，屏幕目标上不存在这两种通道。
struct CaptureScreenRequest {
    const ScreenInfo* screen = nullptr;   // 采样对象 = 授权后复核出来的**当下**那块屏
    uint32_t timeoutMs = 0;
    const DesktopPermit* permit = nullptr;  // 屏幕路径恒为桌面像素，凭证必需
    const CursorRequest* cursor = nullptr;
    const HdrRequest* hdr = nullptr;
    const Deadline* dl = nullptr;
};

// 一条已登记通道的静态事实。名字与内部路径名是稳定 ASCII（进 JSON / 诊断），
// 是否桌面路径由 CaptureScope 的登记表按**路径**判（不由通道名推断）。
struct CaptureBackend {
    CaptureMethod method = CaptureMethod::kAuto;
    const wchar_t* name = L"";               // 通道名（与 CaptureMethodName 同源）
    const wchar_t* windowPath = L"";         // 窗口目标实际走的内部路径名（WindowPathOf）
    const wchar_t* screenPath = L"";         // 屏幕目标实际走的内部路径名（ScreenPathOf）
    bool supportsWindow = false;
    bool supportsScreen = false;
};

// 本构建登记的全部通道（按 CaptureMethod 的枚举顺序，不含 kAuto）。
const std::vector<CaptureBackend>& RegisteredCaptureBackends();

// 按方法找一条已登记通道；kAuto / 未登记返回 nullptr（流水线据此报 unsupported）。
const CaptureBackend* FindCaptureBackend(CaptureMethod method);

// 分派到具体通道。成功 = 已经把一帧 CPU 可读的 BGRA8 交给 out（像素来源与 path 由通道填写）；
// 失败时 out 不被信任，*err 是这条通道自己的原因。调用方必须已经完成授权与身份复核 ——
// 这一层不再重做（重做会让"两次复核之间"的窗口竞态看起来像已解决，实际没有）。
bool AcquireWindowFrame(const CaptureBackend& backend, const CaptureWindowRequest& request,
                        CapturedFrame* out, Diagnostic* err);

bool AcquireScreenFrame(const CaptureBackend& backend, const CaptureScreenRequest& request,
                        CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
