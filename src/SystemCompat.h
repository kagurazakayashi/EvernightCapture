#pragma once
// EvernightCapture - 运行环境能力判据（这台机器的 Windows 版本能不能提供某条取图路线）
//
// 为什么需要这一层：这个 exe 静态导入 `api-ms-win-core-winrt-error-l1-1-1.dll`
// （`RoOriginateLanguageException`，微软那份文档写的最低客户端就是 Windows 8.1）与
// `api-ms-win-core-winrt-l1-1-0.dll` / `api-ms-win-core-job-l2-1-0.dll`（Windows 8 才有），
// 而 API Set 这套机制在 Windows 7 上根本不存在，UCRT 那份可再分发的 46 个契约里也没有这几个
// （本机实测），补不上去 —— 所以 Windows 7 / 8 上连进程都起不来，一条 JSON 都交不出。
// Windows 8.1 与 Windows 10 1507..1809 能装载、能启动，但前者没有 WinRT 编码器（一张图都编不
// 出来），后者拿不到默认那条 WGC 路线。旧实现在那些机器上只会交回一条 `capture.failed` 加一个
// 来自激活调用的 HRESULT，调用方分不清"这个窗口截不到"与"这台机器就不能截"。
// 这一层把后者提前判出来，而且判在枚举窗口、弹确认框、读像素之前。
//
// 三件事必须分开写，别混成一个"支持 Windows X 以上"的口号（详见 README《系统支持》一节）：
//   * API 历史下限 —— 每条路线各自那道 API 文档写明的最低版本，判据在下面的 os_floor 常量上
//     逐条注明出处；单个 BitBlt / DWM 调用在 Windows 7 上存在，并不等于这个 exe 在那上面能跑。
//   * 程序声明下限 —— 本工具对外承诺能跑的最低版本：10.0.18362（Windows 10 版本 1903）x64。
//     取的是"默认那条路线真的能截到窗口"所要求的最高的那一道，而不是各条路线里最老的一道。
//   * 已实测版本 —— 只有开发机那一台（判据见 tests\compat.ps1）。中间那些能装载、但本工具
//     没在任何一台机器上跑过的版本，一律照实记"未验证"，不拿文档推导冒充实测通过。
//
// 所以这里的判据是**版本下限**，不是设备能力预测：显卡驱动不给力、系统组件被裁掉、
// 会话里没有可交互桌面，这些都不是版本号能提前说出来的，本层不作任何断言，
// 那一步自己交回真实的 HRESULT / Win32 码（见「输出契约」里的 capture.* 那组码）。

#include <cstdint>
#include <vector>

#include "CliOptions.h"  // CaptureMethod、Diagnostic、codes::

namespace ecapture {

// 一次探测拿到的 Windows 版本号。`known=false` = 这一问没成功，此时**不按版本筛通道**
// （问不出来不等于支持，也不等于不支持；照旧让那一步自己去交回真实错误码）。
struct OsVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t build = 0;
    bool known = false;
};

// 探测本机版本。走 ntdll!RtlGetVersion：它是内核真正的那份版本号。
// 绝不用 GetVersionEx —— 那个函数受"应用清单兼容性声明"与版本伪装影响，没有清单时
// 在 Windows 10/11 上固定回 6.2，拿它判下限等于自己骗自己。结果在本进程内只问一次。
OsVersion ProbeOsVersion();

// 各条路线各自的最低 Windows 内部版本（build）。0 = 这一条没有版本门槛。
// 每一个数字都注明判据来源，改之前先回去核对，别凭印象动。
namespace os_floor {
// Windows.Graphics.Imaging.BitmapEncoder（WinRT，UWP API contract v1.0 = 10.0.10240）
// 是所有格式唯一的编码器，所以**任何一张图**都要求 Windows 10 起步。
inline constexpr uint32_t kEncoder = 10240;
// IDXGIOutput1::DuplicateOutput：Windows 8（DXGI 1.2）。
inline constexpr uint32_t kDuplication = 9200;
// PrintWindow 本身 Windows XP 就有，但 PW_RENDERFULLCONTENT 这个 flag 要 8.1：
// 本机 SDK 的 winuser.h 里它就写在 `#if(_WIN32_WINNT >= 0x0603)` 里面（0x0603 = Windows 8.1）。
// 不带这个 flag 时现代应用普遍画不出内容，所以这条路线的下限按 flag 算。
inline constexpr uint32_t kPrintWindow = 9600;
// dwm 这条路线是"DwmRegisterThumbnail（Win7）+ 屏幕外宿主窗口 + 对宿主窗口 PrintWindow
// （PW_RENDERFULLCONTENT）读回"，所以受同一道 8.1 门槛约束；它另需
// DWMWA_EXTENDED_FRAME_BOUNDS（Windows 8）。
inline constexpr uint32_t kDwmThumbnail = 9600;
// WGC 取窗口/屏幕这两条都靠 `IGraphicsCaptureItemInterop::CreateForWindow` / `CreateForMonitor`
// （Windows.Graphics.Capture.Interop.h）。注意 Windows.Graphics.Capture 这个命名空间本身
// （GraphicsCaptureSession 等）文档上是 Windows 10 版本 1803（10.0.17134），但**不经选择器
// 直接按 HWND / HMONITOR 建捕获项**是 1903（10.0.18362）才有的那条互操作接口 ——
// 本工具没有"让用户点一下选择器"这条路，所以按 18362 算。
inline constexpr uint32_t kWgc = 18362;
// WGC 那条会话上的光标开关 `IGraphicsCaptureSession2::IsCursorCaptureEnabled`（--cursor 唯一
// 真设得进去的地方）。判据两处对照过：本机 SDK 的 windows.graphics.capture.idl 里这条接口写在
// `[contract(Windows.Foundation.UniversalApiContract, 10.0)]`，而微软那份文档把它标在
// 10.0.19041.0（Windows 10 版本 2004）引入 —— 与上面那条 18362 是两道不同的门槛，
// 所以必须分开：18362 能建会话、能取帧，但那条会话问不出也设不进光标开关。
inline constexpr uint32_t kWgcCursor = 19041;

// 本工具**对外声明**的最低运行环境：64 位 Windows，内部版本 18362（Windows 10 版本 1903）。
// 取的是"默认那条路线真能截到窗口"所要求的最高的那一道门槛，而不是各条路线里最老的那一道 ——
// 单个 BitBlt / DwmRegisterThumbnail 在更旧的系统上存在，并不等于这个 exe 在那上面能跑。
// --version、--help 与四份 README 的那句声明都以这一个数为准，别在文档里另抄一份。
inline constexpr uint32_t kSupportedMinBuild = kWgc;
}  // namespace os_floor

// 一条能力对某个版本的判定结果。
enum class Support {
    kOk,       // 版本够，这条路线在本机可用（设备层面的问题另说，见本文件开头）
    kBelow,    // 版本不够：这就是"整个运行环境不支持"，换目标、重试都没用
    kUnknown,  // 版本没问出来，不作判断
};

struct Capability {
    Support support = Support::kUnknown;
    uint32_t minBuild = 0;  // 这条要求的内部版本，0 = 没有版本门槛
};

// 整工具下限：编码器。判 `kBelow` 时这一次运行一张图都编不出来。
Capability AssessRuntime(const OsVersion& os);

// 单条通道（屏幕模式与窗口模式分开判：同一通道在两种模式下走的是不同的 API）。
// 显式指定的通道与 auto 链里的每一条都用它，所以"哪一条能试"这个决定只有一份判据。
Capability AssessChannel(CaptureMethod method, bool screenMode, const OsVersion& os);

// WGC 会话上那个**光标开关**（IGraphicsCaptureSession2::IsCursorCaptureEnabled）在本机问不问得到。
// 与 AssessChannel 分开：那条判的是"这条通道能不能取帧"，这一条判的是"取到帧之后光标这件事
// 说不说得准"。18362 的机器上通道本身能用（--cursor default 照旧出图），但 include / exclude
// 这两种明确要求都兑现不了 —— 见 src/CursorControl.h。
Capability AssessWgcCursorControl(const OsVersion& os);

// 一次调用的通道闸门结果。
struct ChannelGate {
    // 这一次真正可以试的通道，按尝试顺序。显式指定一条时它就只有那一条；
    // auto 时被版本挡掉的那几条已经不在里面了。整工具那一道下限（EnvironmentError）过了
    // 之后它恒不为空，因为 bitblt 那条没有版本门槛。
    std::vector<CaptureMethod> chain;
    // 非空 = 这次请求在这台机器上根本不该开工（一条像素都不取、不弹框）。
    Diagnostic error;
    // 提示信息：auto 链里被跳过的那几条、以及版本问不出来这件事。
    std::vector<Diagnostic> notes;
};

// 把 `--capture` 的取值展开成本机可用通道链。规矩：
//   * 显式指定的那条被版本挡掉 -> 一条 env.channel_unsupported，chain 为空。
//     **绝不替用户换成别的通道**（与"取值非法不退化成默认通道"同源）。
//   * auto 链里被挡掉的那几条：跳过并留一条 note.channel_unavailable，剩下的照旧回退。
//   * 版本问不出来 -> 整条链原样交给调用方，只补一条 note.os_unverifiable。
ChannelGate GateChannels(CaptureMethod requested, bool screenMode, const OsVersion& os);

// 整工具那一道下限（编码器）的**成因为哪一条诊断**：code 为空 = 这次运行可以开工。
// 与 GateChannels 分开，是因为这条与用户选了哪条通道无关：编码那一步六条通道共用，
// 所以它是"这台机器不行"，而通道那一条是"这条路线在这台机器上不行、别的选择还可能行"。
Diagnostic EnvironmentError(const OsVersion& os);

// 窗口模式与屏幕模式下 auto 各自的回退链（那份顺序本来在 Capture.cpp，挪到这里只写一份，
// 好让通道闸门与 -v 的回显不再各抄一遍顺序而互相打脸）。
const std::vector<CaptureMethod>& AutoChain(bool screenMode);

}  // namespace ecapture
