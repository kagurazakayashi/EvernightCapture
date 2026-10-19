#pragma once
// 【平台层 · Linux】X11 连接、窗口事实与 XComposite 取帧的共用骨架。
//
// 为什么单独一份：窗口枚举（--list / 条件求值）、身份复核（WindowIdentity）与真正的取帧
// （XComposite）三处都要问同一组 X11 问题（窗口属性、WM_CLASS、_NET_WM_PID、坐标、映射状态）。
// 把它们各写一遍迟早出现"枚举看到的还是这个窗口、取帧时问到的却是另一个答案"的两套事实。
// 这一份只放**只读问答**与资源 RAII，取帧的合成顺序在 PosixWindowCapture.cpp。
//
// 四条边界（改代码前先对齐）：
//   1. **一个像素都不从屏幕上取。** 这一层只问窗口/属性，取帧走 NameWindowPixmap（窗口
//      自己的重定向存储），所以它落在 CaptureScope 的窗口内容路径上，--yes 对它有效。
//   2. **不替用户改窗口。** 不 MapWindow、不 SetInputFocus、不 ConfigureWindow、不切工作区、
//      不取消别人的重定向。只有"这次我们自己重定向的"才在结束时还原（见 PosixWindowCapture.cpp）。
//   3. **问不出来 ≠ 读到空值。** 每一条属性都带 ReadState；失败时把 errno / X 错误码留在
//      对应字段里，不用 0 / 空串冒充答案。
//   4. **目标销毁 / 重建 / 改尺寸不靠"看起来一样"糊过去。** 取帧前后各问一次窗口属性与
//      尺寸，对不上就交回 capture.target_gone / capture.target_changed，绝不交一张旧图。

#include <X11/Xlib.h>

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"    // Diagnostic
#include "core/Frame.h"    // CapturedFrame
#include "WindowMatch.h"   // ReadState / WindowInfo 的字段语义

namespace ecapture {
namespace x11 {

// 一条 X11 连接的 RAII。构造失败时 ok() == false，error() 是 XOpenDisplay 留下的 errno。
// 不在全局缓存连接：每次取帧自己开、自己关，目标与会话变化不会被一条长命连接悄悄带过去。
class DisplayConnection {
public:
    DisplayConnection();
    ~DisplayConnection();

    DisplayConnection(const DisplayConnection&) = delete;
    DisplayConnection& operator=(const DisplayConnection&) = delete;

    Display* get() const { return display_; }
    bool ok() const { return display_ != nullptr; }
    int error() const { return error_; }

private:
    Display* display_ = nullptr;
    int error_ = 0;
};

// Xlib 的错误处理器是进程级的，而默认处理器会直接终止进程。所有可能因为目标销毁 / 竞态
// 返回 BadWindow / BadMatch 的调用都包在这个作用域里：构造时接管、Sync() 后读第一个错误码、
// 析构时还原。单线程使用（本工具没有在多个线程里同时取帧）。
class ErrorTrap {
public:
    explicit ErrorTrap(Display* display);
    ~ErrorTrap();

    ErrorTrap(const ErrorTrap&) = delete;
    ErrorTrap& operator=(const ErrorTrap&) = delete;

    // 把已排队的请求送出去，再交回这段时间里的第一个 X 错误码（0 = 没有错误）。
    int Sync();

private:
    Display* display_ = nullptr;
    XErrorHandler previous_ = nullptr;
};

// 一扇顶层客户端窗口在枚举那一刻的事实。字段语义与 WindowInfo 一一对应（这里不复制
// WindowInfo，是因为平台层不该依赖共用层怎么排字段；转换在 PosixWindowSource.cpp）。
struct ClientWindowFacts {
    Window window = 0;
    bool viewable = false;
    bool iconic = false;
    int32_t zOrder = 0;

    uint32_t pid = 0;
    ReadState pidRead = ReadState::kFailed;
    uint64_t processStartTicks = 0;
    ReadState startRead = ReadState::kFailed;
    uint32_t startErrno = 0;

    std::wstring title;
    ReadState titleRead = ReadState::kFailed;
    std::wstring className;
    ReadState classRead = ReadState::kFailed;

    std::wstring imagePath;
    std::wstring imageName;
    ReadState pathRead = ReadState::kFailed;
    uint32_t pathErrno = 0;

    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    ReadState rectRead = ReadState::kFailed;
    uint32_t rectErrno = 0;
};

// 按当前叠放次序枚举全部顶层客户端窗口（0 = 最前）。返回的是**客户端**窗口，不是窗口管理器
// 的装饰框：装饰框没有 WM_CLASS / WM_STATE，递归下去找到真正的客户端。override-redirect
// 的窗口（工具自己的测试图案窗口、菜单）也是顶层客户端，照样枚举。
std::vector<ClientWindowFacts> EnumerateClientWindows(Display* display);

// 只问一扇窗口的事实（不枚举列表）。失败时 *out 的对应 ReadState 是 kFailed，
// 返回 false 仅表示"这扇窗口现在问不到"（多半是已经销毁）。
bool QueryClientWindow(Display* display, Window window, ClientWindowFacts* out);

// 进程创建时间（100ns ticks，与 ClientWindowFacts::processStartTicks 同一换算）。
// 身份复核只要"同一个 PID 还是不是同一个进程"，不需要窗口；单独暴露是为了让那条问答
// 与枚举走同一份 /proc 解析与同一套单位。
bool ReadProcessStartTicksForPid(uint32_t pid, uint64_t* ticks, uint32_t* errorCode);

// X11 的"窗口自己的存储"：XCompositeNameWindowPixmap 取回的位图 + 子窗口各自的重定向存储，
// 用 XRender 合成成一整扇窗口。成功时 out->pixels 是 BGRA8、自上而下、stride = width*4。
//
// 失败时 *err 是稳定诊断：
//   capture.unsupported           这个 X 服务器没有 XComposite（或 XRender，见下）
//   capture.window_gone           窗口已经销毁 / 未映射 / 最小化（XComposite 不给位图）
//   capture.target_changed        读取期间窗口改了尺寸（这一帧不对应当下的窗口）
//   capture.frame_timeout         期限已到（XGetImage 本身不可中断，转换阶段会分段检查）
//   capture.failed                其它 X11 失败（细节在 message 里）
//
// 没有任何"回退到屏幕读取"的分支：这条路走不通就如实失败，绝不拿桌面像素补齐窗口外观。
// *childrenIncomplete 为 true 表示有映射中的子窗口没能取到（通常是它正在销毁），
// 结果图可能少一块 —— 调用方据此留一条 note，而不是把"少了一块"当成完整窗口。
bool CaptureWindowStorage(Display* display, Window window, uint32_t timeoutMs,
                          const Deadline& deadline, CapturedFrame* out, Diagnostic* err,
                          bool* childrenIncomplete);

// 把一张 XImage 的像素按它自己的通道掩码 / 位深 / 字节序 / 行距转成 BGRA8 自上而下。
// 单独暴露是为了离线判据能用合成的 XImage 逐种布局核对（15/16/24/32 位、LSB/MSB、
// 带或不带 alpha、行距带填充）。失败时返回 false 并把原因写进 *detail（ASCII）。
bool ConvertXImageToBgra8(const XImage& image, std::vector<uint8_t>* out, uint32_t* stride,
                          std::string* detail);

}  // namespace x11
}  // namespace ecapture
