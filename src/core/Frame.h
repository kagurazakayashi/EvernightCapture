#pragma once
// 跨平台核心：一帧像素的内存形状与随帧交回的事实。
//
// 这里放的是"任何一条取帧通道都要交回的那一份东西"，与像素是怎么弄到手的无关：
// Windows 的 WGC / DWM / PrintWindow / BitBlt / Desktop Duplication 与将来的 X11、
// Wayland Portal + PipeWire 都往这一个结构里填，下游（裁剪、缩放、编码、交付、历史副本）
// 因此一条都不需要知道后端是谁。
//
// 三条边界：
//   1. **不与任何句柄绑定。** 像素是 std::vector<uint8_t>，帧交出去之后通道可以立刻销毁它的
//      HDC / ID3D11Texture2D / PipeWire 缓冲。谁持有句柄、什么时候释放，是各平台层内部的事
//      （Windows 那一侧的 RAII 在 CaptureCommon.h 的 Dib）。共用结构里出现句柄就等于要求
//      每个平台都得有一个"长得像 HDC 的东西"，那是把一家平台的模型强加给另外两家。
//   2. **像素布局写死**：BGRA8、自上而下、行距 = stride 字节。HDR 帧在落地之前已经映射回
//      8 位（src/HdrColor.h），所以下游永远只见这一种布局；映射前的那一帧来源色彩空间作为
//      一份**事实**记在 sourceColorSpace 里，而不是让下游去猜布局。
//   3. **量不出来的就留 false / kUnknown**，不填一个看起来合理的默认值。下面那些
//      reportsCrop / cursorStateKnown / displayHdrState 三条各自都有"没答案"这一档，
//      它们的合成判据只在 MakeCursorReport / MakeHdrReport 一处，不在这里再判一遍。

#include <cstdint>
#include <string>
#include <vector>

#include "HdrColor.h"        // FrameColorSpace / DisplayHdrState：来源色彩空间那一份事实
#include "core/Error.h"      // NativeErrorCode：原生错误码的数值承载
#include "core/Geometry.h"   // PhysRect：虚拟屏幕坐标里的矩形
#include "core/Status.h"     // BlockedStatus：那一步停在哪儿

namespace ecapture {

// 帧的内存不变量与资源上限。这两条数字都是能说明白的，不是随手挑的：
//   单边 kFrameMaxSide = 16384 —— D3D11 纹理边长的上限（Feature Level 11_0），也是 GDI 那边
//     Dib::Create 与辅助进程管道协议（WorkerProtocol.h）一直沿用的同一条线。8K 显示是 7680，
//     在这条线之内还有近一倍的余量，所以正常截图撞不到它；撞到的就不是"一张截图"了。
//   整帧 kFrameMaxBytes = 1 GiB —— 上面那条的自然推论（16384 × 16384 × 4），任何一步要分配的
//     像素缓冲都在这个数之内。检查全部用 64 位乘法在**分配之前**做完，超限直接报错，
//     不靠"分配失败抛异常"当检查（那条路在内存真耗尽时是不可恢复的）。
// 行距另外要求落在 [width*4, 2*width*4]：下界是"必须装得下一行像素"，上界是给 GPU 与 GDI
// 的对齐填充留一行余量 —— 比行长两倍还宽的行距不可能是填充，只能是形状本身已经坏了。
// 这条上限对 BGRA8（每像素 4 字节）而言；会带回 8 字节/像素中间帧的那一步在分配之前按
// 自己的字节数走同一条判据（ImageOps.h 的 FrameShapeInfo::bytesPerPixel）。
// 判据本体在 ImageOps.h（CheckFrameShape），这里只放数字与形状本体。
inline constexpr uint32_t kFrameMaxSide = 16384u;
inline constexpr uint64_t kFrameMaxBytes = 1024ull * 1024ull * 1024ull;

// 一帧 CPU 可读的像素：BGRA8，行长 = stride。
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
    PhysRect requestedRect{};  // reportsCrop 为真时有效：这条通道本来要截的那一块（虚拟屏幕坐标）
    PhysRect capturedRect{};   // reportsCrop 为真时有效：实际截到的那一块，同样虚拟屏幕坐标
    bool clipped = false;      // requestedRect 是否没有被完整截到
    uint32_t rotation = 0;     // reportsCrop 为真时有效：交付前对桌面帧顺时针转的度数（0/90/180/270）

    // 光标（--cursor）：这一条只有"真的有一个可设开关"的那条通道会填 —— 填的是**设完再读回来**
    // 的那两个答案（cursorStateKnown = 那一问有没有答案，cursorInFrame = 这条会话现在画不画光标）。
    // 其余通道一律留 false/false：那两条 false 在那儿说的是"没有可依据的读数"，不是一次 API 问答，
    // 所以不该在这里冒充"我读过开关"。它们交回的帧在光标这件事上算什么，由 src/CursorControl.h
    // 那张按路径登记的表说：printwindow / dwm / bitblt 的来源像素里本来就没有光标，而桌面复制那两条
    // 登记成 pointer_state_unverified —— 官方说明允许指针**已经画在那幅桌面图像上**，这一问因此
    // 没有答案，那两个 false 对它们不等于"读过并且确认没有"。
    // 两个值与那张登记表怎么合成结果里的 requested/effective/basis 三个键，只有一份判据
    //（MakeCursorReport）。
    bool cursorStateKnown = false;
    bool cursorInFrame = false;

    // HDR 色彩（--hdr）：下面这三件是**三个独立的事实**，各自由唯一的那一步写，合成结果里那组键
    // 时谁也不替谁作保（判据与合成只有一份，src/HdrColor.h 的 MakeHdrReport）：
    //   sourceColorSpace 取帧那一步从 GPU 纹理描述里读回的内存布局（编码之前的原样）。最常见的
    //     B8G8R8A8 那条恒为 kSrgbBgra8、toneMapped 为 false —— 与这条选项存在之前逐字节相同。
    //     只有会带回广色域帧的两条通道（wgc / 桌面复制）在某次真带回 FP16 / 10 位帧时才写成别的值，
    //     而那是被 --hdr tonemap 就地映射成 8 位 BGRA 之后仍然保留"映射前来源"那一份，供结果报告。
    //   toneMapped 只在确实过了一遍浮点 tone mapping 时为 true（SDR 透传不置它）——"已映射"那一句
    //     唯一的来源就是这里，不是 sourceColorSpace 写着广色域。
    //   displayHdrState 显式 --hdr 策略下，这条路径在**开始采集之前**对"这块屏此刻是不是 HDR 模式"
    //     那一次只读问答的答复。没问过（没写 --hdr、写成 auto、或这条路径根本不问）与问不出来一律
    //     kUnknown，绝不折成 kSdr —— 一张 8 位 BGRA 帧本身不证明原始内容是 SDR（合成器可能把 HDR
    //     画面压成 8 位再交给一个 B8G8R8A8 的帧池），而那一句"来源是 SDR"只能由这次问答或这条路径
    //     结构上带不回广色域帧来支撑。拿去决定留哪一条提示的判据在 JudgeHdrPassiveNote。
    FrameColorSpace sourceColorSpace = FrameColorSpace::kSrgbBgra8;
    bool toneMapped = false;
    DisplayHdrState displayHdrState = DisplayHdrState::kUnknown;
};

// 一次"只读某个窗口自己的画面"的调用结果。
//
// 它既能在本进程里产生，也能在辅助进程里产生（见 Worker.h），两边共用同一个形状，所以放在核心。
// 两个错误码字段用 NativeErrorCode 承载：win32 那一格是 GetLastError 的原值，
// hresult 那一格是一次 COM/WinRT 调用自己的返回值（0 = 成功，与原先写 S_OK 逐字节同义）。
// 字段名继续承担"这一格是哪一家来源"的说明，本步不改线上格式与既有的判据表。
struct RenderOutcome {
    BlockedStatus status = BlockedStatus::kOk;
    NativeErrorCode win32 = kNativeErrorNone;
    NativeErrorCode hresult = kNativeErrorNone;
    std::string detail;   // 只放 ASCII 细节（异常 what()），不放大自然语言
    CapturedFrame frame;
};

}  // namespace ecapture
