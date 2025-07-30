#pragma once
// 窗口内部裁剪（--roi / --client-area）的判据本体。
//
// 这一层是**纯算术**：不碰窗口、不读像素、不问 Win32、不弹框，所以离线判据能把负值、溢出、
// 越界、空矩形、以及"图像原点核实不出来"这几种形状逐条注入（tests\crop_state.cpp）。
// 测量那一步在 Capture.cpp 与 CaptureCommon.cpp，它只负责把三件事量出来交给这里：
// 交付图像的尺寸、这块图像在虚拟屏幕坐标里对应的那一块（能不能核实）、以及客户区那块矩形
// （问不问得到）。判据本身一条都不留在那里。
//
// 坐标系（这一条是整个功能的根，改之前先对齐 README《窗口内部裁剪》那一节）：
//   * 裁剪矩形说的是**这张交付图像自己的像素坐标**：(0,0) = 图像左上角那个像素，右下边不含。
//     绝不把它当成桌面（虚拟屏幕）绝对坐标 —— 那样等于允许调用方用一个窗口之外的位置
//     去要一块没人批准过的画面，也与"相对于目标窗口"这件事自相矛盾。
//   * 单位是**物理像素**。本进程声明了 per-monitor DPI v2，窗口矩形、帧尺寸与像素缓冲本来就
//     都在物理像素这一套系里，所以这里没有任何按 DPI 缩放的步骤：一条 `--roi 0,0,100,50`
//     在 100% 与 200% 的屏上都是"取图像左上角那 100×50 个像素"。要按逻辑像素（DIP）指定
//     尺寸的调用方自己乘那道缩放，本工具不猜窗口在哪块屏上、也不猜该用哪块的 DPI。
//   * 边框：交付的整窗图像是**用户看到的那一圈边框**之内（DWMWA_EXTENDED_FRAME_BOUNDS，
//     也就是 GetWindowRect 那圈透明resize边框**之外**的可见矩形；各条通道交回的尺寸本来就按
//     它对齐）。`--client-area` 就是再往里去掉头与三边边框，只留客户区。
//
// 规矩：
//   1. 越界与空矩形一律**拒绝**，不裁到边上为止、不放大、不挪位置。静默改成"那就截能截到的
//      那一块"会让调用方拿到一张它没要求过的图。
//   2. 裁剪发生在取帧**之后**，所以授权判断一条都不因为它而变窄：确认框上列出的是整个目标，
//      会读桌面像素的那几条照样一定问人，`--yes` 不会因为"最后只留一小块"而开始生效。
//   3. "问不出来"与"放不下"是两件事，各有一种状态：客户区量不出来、或图像原点核实不出来，
//      都不能被当成"客户区是空的"或"那块矩形在图外"。

#include <cstdint>
#include <windows.h>

#include "CliOptions.h"

namespace ecapture {

// 图像像素坐标里的一个矩形（原点 = 图像左上角，四值都非负，宽高至少 1）
struct ImageRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// 交付的那张整窗图像，以及它在虚拟屏幕坐标里对应的那一块。
// hasScreenRect 为假 = 这一块核实不出来（窗口在这中间改了尺寸、或这条通道交付的根本不是
// 当初量到的那一块），此时**不**交回一个"大概是一样的"screenRect。
struct DeliveredImage {
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasScreenRect = false;
    RECT screenRect{};
};

// 客户区那一块，虚拟屏幕坐标（左上含、右下不含）。readable 为假 = 这一问没有答案，
// 与"客户区是零尺寸"是两件事。
struct ClientAreaProbe {
    bool readable = false;
    RECT screenRect{};
};

enum class CropStatus {
    kNoCrop,        // 这一次没有裁剪请求（--roi 与 --client-area 都没给）
    kCropped,       // 裁得下来，crop 已填
    kOutOfRange,    // 放不下：越界、空矩形、或客户区有一部分不在这块图像之内
    kUnmeasurable,  // 定位这块矩形所需要的那一问没有答案
};

// 失败的具体是哪一种（诊断的 message 走 --lang，这个只在测试判据与 ASCII 细节里出现）
enum class CropFail : uint32_t {
    kNone = 0,
    kRoiBeyondImage,       // --roi 的矩形没有落在交付图像之内
    kEmptyImage,           // 交付的图像本身零尺寸（形状那一关另有判据，这里只是不放行裁剪）
    kClientUnmeasurable,   // 客户区问不出来
    kImageUnmeasurable,    // 图像原点核实不出来，于是客户区在这张图里落在哪儿无法确定
    kClientBeyondImage,    // 客户区落在交付图像之外（含负偏移：那块根本没被交付）
    kEmptyClientArea,      // 客户区是空的（零宽或零高）
};

struct CropResolution {
    CropStatus status = CropStatus::kNoCrop;
    CropFail fail = CropFail::kNone;
    ImageRect crop{};              // status == kCropped 时有效：图像坐标里要留下的那一块
    bool hasCropScreen = false;    // 这块裁剪矩形能不能同时报成屏幕坐标
    RECT cropScreen{};             // hasCropScreen 时有效：虚拟屏幕坐标，与 rect 同一套系
    uint32_t imageWidth = 0;       // 以下四个数只为诊断服务：图像多大、请求的矩形右下边到哪
    uint32_t imageHeight = 0;
    uint64_t requestRight = 0;     // 64 位里算出来的右下边，所以越界时也报得出真实数字
    uint64_t requestBottom = 0;
};

// 取帧之前那一道：这条 --roi 放不放得进"当初准备交付的那块窗口矩形"。
// 目的不是替代取帧之后的那次判据，而是**别去打扰人** —— 一条注定放不进的裁剪矩形不该
// 先弹出确认框、等人答完"是"之后再失败。rectWidth/rectHeight 为 0（矩形量不出来、窗口已经
// 没了）时任何裁剪矩形都放不下，照实拒绝。相加在 64 位里判，不靠回绕。
bool RoiFitsTarget(const CropRequest& request, uint32_t rectWidth, uint32_t rectHeight);

// 交付图像的屏幕位置核实得出来吗：
//   * 桌面裁切那几条本来就把"实际截到的那一块"报在 sampledScreenRect 里，那块就是这块图像的
//     屏幕位置（sampledRectOk 为真才用）。
//   * 窗口内容那几条只能拿此刻量到的可见矩形（measuredWindowRect，measuredOk 为真才用）对照，
//     而且**尺寸必须与交付的帧完全相同**才承认：尺寸不同说明窗口在这中间改了大小、或者这条
//     通道交付的并不是那一块（printwindow 的"边框裁不下来就整张交出"就是这一种），两种都不许猜。
// 两个都不成立时 hasScreenRect 为假，调用方仍然能裁 --roi（那块矩形只需要图像尺寸），
// 但 --client-area 与 cropScreenRect 那一行就交不出来了。
DeliveredImage DescribeDeliveredImage(uint32_t frameWidth, uint32_t frameHeight,
                                      const RECT& measuredWindowRect, bool measuredOk,
                                      const RECT& sampledScreenRect, bool sampledRectOk);

// 把一次裁剪请求落到交付的那张整窗图像上。这是裁剪判据唯一的一份实现：取帧之前的
// RoiFitsTarget 只看"放不放进得下当初那块矩形"，真正的裁剪矩形以这里算出来的为准。
CropResolution ResolveWindowCrop(const CropRequest& request, const DeliveredImage& image,
                                 const ClientAreaProbe& client);

// 失败原因的 ASCII 名（测试判据与 diagnostics 的 ASCII 细节用，不随 --lang 变）
const char* CropFailName(CropFail fail);

}  // namespace ecapture
