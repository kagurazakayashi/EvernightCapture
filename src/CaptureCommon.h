#pragma once
// 取帧通道的公共件：帧的内存形状、GDI 位图、窗口矩形、屏幕取图、消息泵。
// 每个通道自己一个文件（CaptureWgc / CaptureDwm / CapturePrintWindow / CaptureBitBlt /
// CaptureDuplication），像素级处理在 ImageOps，D3D 设备在 D3dDevice。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CliOptions.h"
#include "Consent.h"

// CopyTextureToFrame 的两个 D3D 参数类型。本头文件不 include d3d11.h（各通道本来就 include 了），
// 前向声明必须写在**全局作用域**：SDK 里它们就在那儿，声明进 ecapture 里会得到另一个同名类型，
// 通道传进来的真指针在这边就成了未定义类型。
struct ID3D11Device;
struct ID3D11Texture2D;

namespace ecapture {

// PrintWindow 的第 2 个 flag（Win8.1+）：要求连硬件加速 / DirectComposition 的内容
// 一起渲染进 DC。老系统不认它，所以调用方失败后要退回 flags=0 再试。
inline constexpr UINT kPwRenderFullContent = 0x00000002u;

// 一帧 CPU 可读的像素：BGRA8，行长 = stride
struct CapturedFrame {
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    std::wstring source;  // 产出这帧的通道名，如 "wgc"
    std::wstring path;    // 实际走的那条内部路径名（"dwm.thumbnail" / "dwm.screen" / ...）
                          // scope 由它算出来，所以图里写的来源与当初授权的是同一件事

    // 以下只有"从整幅桌面帧里裁出一块"的那些通道（duplication、以及拷屏幕的 bitblt）会填：
    // 那几条截到的可能只是目标的一部分（窗口跨屏、部分在屏幕外），调用方必须能看见这件事，
    // 而不是拿到一张比目标小的图还以为截到了完整窗口。窗口内容路径（wgc / dwm 缩略图 /
    // printwindow）拍的就是整个目标，reportsCrop 留 false = "没有丢区域"这层意思。
    bool reportsCrop = false;
    RECT requestedRect{};  // reportsCrop 为真时有效：这条通道本来要截的那一块（虚拟屏幕坐标）
    RECT capturedRect{};   // reportsCrop 为真时有效：实际截到的那一块，同样虚拟屏幕坐标
    bool clipped = false;  // requestedRect 是否没有被完整截到
    uint32_t rotation = 0; // reportsCrop 为真时有效：交付前对桌面帧顺时针转的度数（0/90/180/270）
};

// 帧的内存不变量与资源上限。这两条数字都是能说明白的，不是随手挑的：
//   单边 kFrameMaxSide = 16384 —— D3D11 纹理边长的上限（Feature Level 11_0），也是 GDI 那边
//     Dib::Create 与辅助进程管道协议（WorkerProtocol.h）一直沿用的同一条线。8K 显示是 7680，
//     在这条线之内还有近一倍的余量，所以正常截图撞不到它；撞到的就不是"一张截图"了。
//   整帧 kFrameMaxBytes = 1 GiB —— 上面那条的自然推论（16384 × 16384 × 4），任何一步要分配的
//     像素缓冲都在这个数之内。检查全部用 64 位乘法在**分配之前**做完，超限直接报错，
//     不靠"分配失败抛异常"当检查（那条路在内存真耗尽时是不可恢复的）。
// 行距另外要求落在 [width*4, 2*width*4]：下界是"必须装得下一行像素"，上界是给 GPU 与 GDI
// 的对齐填充留一行余量 —— 比行长两倍还宽的行距不可能是填充，只能是形状本身已经坏了。
inline constexpr uint32_t kFrameMaxSide = 16384u;
inline constexpr uint64_t kFrameMaxBytes = 1024ull * 1024ull * 1024ull;

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
    kRegexInvalid = 9,       // 正则编不出来（解析期已挡过一遍，正常走不到这里）
    kBadTask = 10,           // 交来的任务不合法（尺寸 / 条数超限）
    kInternal = 11,          // 那一步抛了异常（ASCII 细节在 detail）
    kHostPrintWindowFailed = 12,  // 对宿主窗口的 PrintWindow 两次都返回 FALSE（dwm 通道）
    kRegexTooComplex = 13,  // 回溯复杂度超限：模式语法没问题，但这台机器的正则库拒绝把它跑完
};

// 一次"只读某个窗口自己的画面"的调用结果。放在这里是因为它既能在本进程里产生，
// 也能在辅助进程里产生（见 Worker.h），两边共用同一个形状。
struct RenderOutcome {
    BlockedStatus status = BlockedStatus::kOk;
    DWORD win32 = 0;
    HRESULT hresult = S_OK;
    std::string detail;   // 只放 ASCII 细节（异常 what()），不放大自然语言
    CapturedFrame frame;
};


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

// 窗口完整矩形（含 DWM 那圈透明边框）；取不到时返回全零
RECT WindowFullRect(HWND hwnd);
// 用户实际看到的矩形：优先 DWMWA_EXTENDED_FRAME_BOUNDS，否则退回完整矩形
RECT WindowScreenRect(HWND hwnd);

// 从屏幕 DC 取一块矩形（超出虚拟屏幕的部分被丢掉）。
// permit 是必需参数：这张凭证只能由授权判定器发出，所以任何"从屏幕上拿像素"的代码都绕不过
// 那次人工确认 —— 包括通道自己在内部临时改走屏幕取图的那条退路。
bool GrabScreenRect(const RECT& rect, const wchar_t* channel, const wchar_t* path,
                    const DesktopPermit& permit, CapturedFrame* out, Diagnostic* err);

// 取样之前核对凭证：批准的是那一块，现在要取的必须是它里面的一块。
// 不覆盖 = 目标在确认之后挪了位置或变了大小，这次不截（宁可少截一张也不拍别人的画面）。
bool PermitCovers(const DesktopPermit& permit, const RECT& area, const wchar_t* channel,
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

// 建 staging 纹理 -> CopyResource -> Map -> 按行搬 width*4 字节。设备由调用方给
//（各通道的设备创建策略不同，拷回 CPU 这一段没有区别）。
// 像素按 BGRA8 解释；格式不是 B8G8R8A8_UNORM、单边或整帧超上限、行距装不下一行像素，
// 都在这里按 capture.frame_invalid 报出来，不带着坏形状往下走。
// CopyResource 返回 void，它自己失败只能由 GetDeviceRemovedReason 这条 **API 层面**的问法
// 发现（设备没了 / 被移除就报 capture.failed 带真码），不靠画面颜色反证。
bool CopyTextureToFrame(::ID3D11Device* device, ::ID3D11Texture2D* src, const wchar_t* channel,
                        CapturedFrame* out, Diagnostic* err);

// 只把纹理里 (x,y) 起 width×height 那一块拷进帧，纹理剩下的边缘一个字节都不读。
// 用在"纹理比有效内容大"的那条路上：窗口缩小之后 WGC 交回的帧，其纹理仍是帧池当初那份较大
// 的尺寸，而 frame.ContentSize 说的那块左上角才是有效像素 —— 整张复制等于把没定义的边缘
// 当成画面交出去。这一条走 CopySubresourceRegion（带源矩形），形状与上限仍在分配之前判完。
// x/width 与纹理边界对不上（相加绕回、越界、0 边、超单边上限）一律 capture.frame_invalid。
bool CopyTextureRectToFrame(::ID3D11Device* device, ::ID3D11Texture2D* src, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height, const wchar_t* channel,
                            CapturedFrame* out, Diagnostic* err);

}  // namespace ecapture
