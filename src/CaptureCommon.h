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
};

// 让屏幕坐标与物理像素一致：GDI 通道按屏幕矩形取图，被 DPI 虚拟化时
// GetWindowRect 给的是缩放后坐标，截出来就是错位或只有一半。
void EnsureDpiAware();

std::wstring Win32ErrorText();
std::wstring HResultText(HRESULT hr);

// 统一的取帧失败诊断：code = capture.failed，option = --capture，value = 通道名
void CaptureError(Diagnostic* err, const wchar_t* channel, const std::wstring& message,
                  const std::wstring& hint);

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

// 从屏幕 DC 取一块矩形（超出虚拟屏幕的部分被丢掉）
bool GrabScreenRect(const RECT& rect, const wchar_t* channel, CapturedFrame* out, Diagnostic* err);

// 没有消息循环时，泵消息等 ms 毫秒：DWM 要几帧才把缩略图合成出来
void PumpMessagesFor(uint32_t ms);

// 该矩形是不是真的归这个窗口：临时窗口可能被别的置顶窗口压住，那样从屏幕拷回来的
// 就是别人的画面，必须报错而不是交一张错图。取四个点问 WindowFromPoint。
bool WindowIsOnTopAt(HWND hwnd, const RECT& rect);

}  // namespace ecapture
