#pragma once
// 【平台层 · Windows】取帧通道的公共件：GDI 位图、窗口矩形问答、屏幕取图、消息泵。
// 每个通道自己一个文件（CaptureWgc / CaptureDwm / CapturePrintWindow / CaptureBitBlt /
// CaptureDuplication），像素级处理在 ImageOps，D3D 设备在 D3dDevice。
//
// 分层归属（划分跨平台边界这一步定下来的）：本头文件里的每一样都直接使 Win32 / GDI / D3D 资源
//（HDC、HBITMAP、HWND、RECT、ID3D11Texture2D），所以它是 Windows 后端自己的件，不进共用核心。
// 跨平台共用的那一半（一帧像素的形状与上限、BlockedStatus、RenderOutcome）已经搬到
// src/core/Frame.h 与 src/core/Status.h；本文件 include 它们，所以既有调用点一行都不用改。
// Linux 的 X11 / Wayland 后端将来在自己的目录里 provide 同职责的一组函数，不共用下面任何一条。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CliOptions.h"
#include "Consent.h"
#include "HdrColor.h"   // FrameColorSpace：一帧的来源色彩空间（GPU 交回来、转换之前那一份事实）
#include "core/Frame.h"  // CapturedFrame 本体与帧的内存上限（跨平台共用；本文件只放 Windows 取帧件）

// CopyTextureToFrame 的两个 D3D 参数类型。本头文件不 include d3d11.h（各通道本来就 include 了），
// 前向声明必须写在**全局作用域**：SDK 里它们就在那儿，声明进 ecapture 里会得到另一个同名类型，
// 通道传进来的真指针在这边就成了未定义类型。
struct ID3D11Device;
struct ID3D11Texture2D;

namespace ecapture {

// PrintWindow 的第 2 个 flag（Win8.1+）：要求连硬件加速 / DirectComposition 的内容
// 一起渲染进 DC。老系统不认它，所以调用方失败后要退回 flags=0 再试。
inline constexpr UINT kPwRenderFullContent = 0x00000002u;

// CapturedFrame（一帧 CPU 可读的 BGRA8 像素）、帧的内存不变量与资源上限
// （kFrameMaxSide / kFrameMaxBytes）都在 core/Frame.h：那是三条平台共用的形状定义，
// 放在这里会让"共用核心不暴露 HWND / HDC / PipeWire 对象"这条边界从中间断开。
// 本文件从此只放 **Windows 取帧件**：GDI 的 Dib、D3D 纹理拷回 CPU、窗口矩形问答、屏幕取图、
// 消息泵。BlockedStatus 与 RenderOutcome 在 core/Status.h 与 core/Frame.h（父进程与辅助进程
// 共用同一个结果形状，而条件求值也要读那个状态码，所以不能让它们长在平台头里）。

// 让屏幕坐标与物理像素一致：GDI 通道按屏幕矩形取图，被 DPI 虚拟化时
// GetWindowRect 给的是缩放后坐标，截出来就是错位或只有一半。
void EnsureDpiAware();

// 失败点当场取错误码：文案要用 Msg / Msgf 去读资源，那一路 API 会把上一次的 GetLastError
// 覆盖掉，所以"取码"必须排在拼文案之前，由调用方显式做一次。
inline DWORD LastError() { return GetLastError(); }

// 把当前异常换成一段**只含 ASCII** 的细节（异常 what() 本来就是窄字符）。
// 隔离执行时这段文字要穿过管道交给父进程，所以这里不放本地化文案：
// 辅助进程只说"崩在哪"，说什么话由父进程按 --lang 决定。
void DetailFromCurrentException(std::string* detail);

// 只负责把已经取到的码写成给人看的文字（不再自己去问 GetLastError）
std::wstring Win32ErrorText(DWORD gle);
std::wstring HResultText(HRESULT hr);

// 统一的取帧诊断：option = --capture，value 与 backend 都是这条通道的名字。
// code 默认 capture.failed；帧超时与窗口消失各有自己的稳定码（调用方处理方式不同：
// 前者等一会儿还能重试，后者要重新枚举窗口）。stage 在这里定为 capture，
// target 由流水线补（只有它知道当前处理的是哪个目标）。
void CaptureError(Diagnostic* err, const wchar_t* channel, const std::wstring& message,
                  const std::wstring& hint, const wchar_t* code = codes::kCaptureFailed,
                  DWORD gle = 0, HRESULT hr = S_OK);

// 32 位自上而下 DIB 段：内存布局即 BGRA8，行距恒等于 width * 4
class Dib {
public:
    Dib() = default;
    ~Dib();
    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;

    bool Create(uint32_t width, uint32_t height, Diagnostic* err, const wchar_t* channel);
    void Destroy();
    bool valid() const { return dc_ != nullptr; }
    HDC dc() const { return dc_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    void ToFrame(const wchar_t* channel, CapturedFrame* out) const;

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ saved_ = nullptr;
    void* bits_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

// 下面这三条问答返回/交出的是**原生 RECT**：它们量的是 Win32 那一刻的事实，调用方把它们
// 搬进共用结构（CapturedImage.rect / GateTarget.area / ClientAreaProbe.screenRect）时，
// 必须经 platform/windows/WinBridge.h 的 FromNativeRect 那一次看得见的转换 —— 转换点就是边界点。
// 窗口完整矩形（含 DWM 那圈透明边框）；取不到时返回全零
RECT WindowFullRect(HWND hwnd);
// 用户实际看到的矩形：优先 DWMWA_EXTENDED_FRAME_BOUNDS，否则退回完整矩形
RECT WindowScreenRect(HWND hwnd);

// 客户区在**虚拟屏幕坐标**里的那一块（GetClientRect + ClientToScreen，左上含、右下不含）。
// --client-area 那条裁剪要它，所以这里把"问没问出来"与"问出来是一块空的客户区"分开交回来：
// 返回 false = 这一问没有答案（句柄已经无效 / 两个 API 有一个失败 / 加出来的坐标不在 LONG
// 里），调用方按无法测量处理，**不**写成零尺寸那块矩形 —— 后者是一扇本来就还没有客户区的窗口，
// 是一条事实而不是一次失败（WindowIdentity 那一条"问不出来 ≠ 相同"同源）。
bool ClientScreenRect(HWND hwnd, RECT* out);

// 从屏幕 DC 取一块矩形（超出虚拟屏幕的部分被丢掉）。
// permit 是必需参数：这张凭证只能由授权判定器发出，所以任何"从屏幕上拿像素"的代码都绕不过
// 那次人工确认 —— 包括通道自己在内部临时改走屏幕取图的那条退路。
bool GrabScreenRect(const PhysRect& rect, const wchar_t* channel, const wchar_t* path,
                    const DesktopPermit& permit, CapturedFrame* out, Diagnostic* err);

// 取样之前核对凭证：批准的是那一块，现在要取的必须是它里面的一块。
// 不覆盖 = 目标在确认之后挪了位置或变了大小，这次不截（宁可少截一张也不拍别人的画面）。
bool PermitCovers(const DesktopPermit& permit, const PhysRect& area, const wchar_t* channel,
                  Diagnostic* err);

// 没有消息循环时，泵消息等 ms 毫秒：DWM 要几帧才把缩略图合成出来
void PumpMessagesFor(uint32_t ms);

// 该矩形是不是真的归这个窗口：临时窗口可能被别的置顶窗口压住，那样从屏幕拷回来的
// 就是别人的画面，必须报错而不是交一张错图。取四个点问 WindowFromPoint。
bool WindowIsOnTopAt(HWND hwnd, const RECT& rect);

// ---------------------------------------------------------------------------
// GPU 纹理 -> CPU 帧。两条会用到 D3D 的通道（wgc 的帧、duplication 的桌面帧）走同一段实现，
// 于是"形状与上限在分配之前判完"和"Map 之后异常也要 Unmap"这两件事只写一遍。
// ---------------------------------------------------------------------------

// 建 staging 纹理 -> CopyResource -> Map -> 按行搬 width*bpp 字节。设备由调用方给
//（各通道的设备创建策略不同，拷回 CPU 这一段没有区别）。
// 默认（hdr.given=false 或 --hdr auto）只按 BGRA8 解释像素：格式不是 B8G8R8A8_UNORM、单边或
// 整帧超上限、行距装不下一行像素，都在这里按 capture.frame_invalid / cap.frame_format 报出来，
// 与这条选项存在之前的语义逐字节一致。CopyResource 返回 void，它自己失败只能由
// GetDeviceRemovedReason 这条 **API 层面** 的问法发现（设备没了 / 被移除就报 capture.failed 带真码）。
// 显式要过 HDR 处理时才认得广色域来源，而且保证交出去的永远是 8 位 BGRA（下游一处都不用改）：
//   * --hdr tonemap —— 把 FP16 scRGB / 10 位 PQ|HLG 就地映射成 8 位 BGRA sRGB 再交出；
//   * --hdr refuse  —— 核实来源确是 HDR 时一个像素都不落地（capture.hdr_refused），
//                      带回一个认不出的广色域格式时 capture.hdr_unverifiable（不硬按 BGRA8 解释）。
// dl 守 tone mapping 那趟线性扫描的预算（花光就 capture.timeout 而不动 frame）。
bool CopyTextureToFrame(::ID3D11Device* device, ::ID3D11Texture2D* src, const wchar_t* channel,
                        const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
                        Diagnostic* err);

// 只把纹理里 (x,y) 起 width×height 那一块拷进帧，纹理剩下的边缘一个字节都不读。
// 用在"纹理比有效内容大"的那条路上：窗口缩小之后 WGC 交回的帧，其纹理仍是帧池当初那份较大
// 的尺寸，而 frame.ContentSize 说的那块左上角才是有效像素 —— 整张复制等于把没定义的边缘
// 当成画面交出去。这一条走 CopySubresourceRegion（带源矩形），形状与上限仍在分配之前判完。
// x/width 与纹理边界对不上（相加绕回、越界、0 边、超单边上限）一律 capture.frame_invalid。
bool CopyTextureRectToFrame(::ID3D11Device* device, ::ID3D11Texture2D* src, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height, const wchar_t* channel,
                            const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
                            Diagnostic* err);

}  // namespace ecapture
