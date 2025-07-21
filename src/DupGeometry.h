#pragma once
// Desktop Duplication 的几何判据：一块显示的"桌面坐标"与"驱动交回的那张纹理"之间到底
// 是什么关系，以及要截的目标矩形该从纹理的哪一块读、读出来要不要转、还剩多少没截到。
//
// 为什么这一段必须搬进纯算术（判据本体在 tests\dup_state.cpp，逐条注入）：
//   * 显示器可以处于 0/90/180/270 度旋转。旋转状态下 `DesktopCoordinates`（虚拟屏幕坐标，
//     也就是窗口矩形与人工确认矩形所在的坐标系）是**转好之后**的尺寸，而桌面复制按文档
//     交回的纹理是**显示面板原生朝向**的那张图（`DXGI_OUTDUPL_DESC.Rotation` 说明要转多少度
//     才是用户看到的画面）。直接把桌面矩形套在这张纹理上，竖屏时截到的是横躺的画面，
//     而尺寸还对得上，光看退出码与字节数发现不了。
//   * 但不同驱动/系统版本在这件事上不完全一致（有的已经把画面转好才交回来）。所以判据不写死
//     "一定没转"，而是拿**纹理的实际尺寸**与桌面尺寸对照：能判定就照判定结果转，两种尺寸都对不上
//     就是这张帧自相矛盾，判不合法而不是将就用。
//   * 多块显卡时"哪个适配器上有哪块输出"也不是默认适配器那一份列表：目标屏可能在第二块卡上。
//     定位这件事是纯查表，能用假枚举器逐条判（含热拔出、无输出、并列重叠面积）。
//   * 跨屏窗口只能截到与某一块输出重叠的那部分，旧实现把丢掉的部分静默吞了 —— 剩下的区域
//     算不算"完整的目标"必须由调用方判，所以这里把 requested / captured / 每边丢了几个像素都算出来。
//
// 这里全是纯计算：不碰 DXGI、不碰窗口、不弹框，也不产出本地化文字（只给原因码，
// 文案由调用方按 --lang 现取）。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {

// 显示器旋转。取值与 DXGI_MODE_ROTATION 一一对应（数值相同），但本头文件不 include dxgi.h：
// 判据层不需要 DXGI，测试也就不必链图形库。CaptureDuplication.cpp 负责把 DXGI 的值换过来。
enum class DupRotation : uint32_t {
    kUnspecified = 0,
    kIdentity = 1,
    kRotate90 = 2,
    kRotate180 = 3,
    kRotate270 = 4,
};

// ---------------------------------------------------------------------------
// 交回的那张纹理该转多少度，才与用户看到的朝向（= DesktopCoordinates 的朝向）一致
// ---------------------------------------------------------------------------
enum class DupTransformReason : uint32_t {
    kNone = 0,
    kDesktopRectEmpty,  // 该输出的桌面矩形自己就是空的（拓扑正在变 / 枚举给了坏值）
    kFrameMismatch,     // 纹理既不是桌面那么大、也不是交换宽高后那么大：这张帧说不通
};

struct DupTransformDecision {
    bool ok = false;
    uint32_t angle = 0;  // 要顺时针转的度数；ok 为假时无意义
    DupTransformReason reason = DupTransformReason::kDesktopRectEmpty;
    // 下面几个是判据实际比过的数字，失败时交给 hint（机器看的坐标，不随 --lang 变）
    uint32_t desktopWidth = 0;
    uint32_t desktopHeight = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
};

// desktopWidth/Height 是该输出在虚拟屏幕上的宽高（用户看到的朝向），texture 是帧的宽高。
// 规矩：
//   * 报"不转"（identity）=> 纹理必须正好和桌面一样大，转 0 度。尺寸对不上就是坏帧。
//   * 报 90/270 => 纹理若是"交换过宽高"的那份，说明驱动交的是面板朝向，要转；
//     若已经和桌面一样大，说明驱动早就转好了，不再转（重复旋转会把画面转回去）。
//   * 报 180 => 交换与不交换是同一个尺寸，尺寸分不出来，只能照文档按 180 度处理；
//     这一步的可判性写在诊断里（images[].rotation），真机换到 180 度屏上一眼能核对。
DupTransformDecision DecideDupTransform(DupRotation rotation, uint32_t desktopWidth,
                                        uint32_t desktopHeight, uint32_t textureWidth,
                                        uint32_t textureHeight);

// ---------------------------------------------------------------------------
// 把"要截的桌面矩形"换算成"从纹理的哪一块读、读出来转成多大"
// ---------------------------------------------------------------------------
enum class DupCropReason : uint32_t {
    kNone = 0,
    kTargetEmpty,    // 目标矩形自己就是空的
    kNotOnOutput,    // 目标与这块输出完全不相交
    kOutsideTexture, // 算出的读取矩形落在这张纹理之外（帧比宣称的桌面小 / 坐标不一致）
    kBadAngle,       // 传进来的旋转度数不是 0/90/180/270：不敢猜该按哪一种处理
};

struct DupCropPlan {
    bool ok = false;
    DupCropReason reason = DupCropReason::kNone;
    RECT src{};              // 纹理坐标（旋转之前），left/top 含、right/bottom 不含
    uint32_t outWidth = 0;   // 旋转之后交付的图像尺寸 = 实际截到的那块桌面区域
    uint32_t outHeight = 0;
    RECT captured{};         // 实际截到的那块，虚拟屏幕坐标（给 JSON 的 capturedRect）
    bool clipped = false;    // 目标矩形有没有被这块输出丢掉一部分
    int32_t lostLeft = 0;
    int32_t lostTop = 0;
    int32_t lostRight = 0;
    int32_t lostBottom = 0;
    // 目标矩形自己：原样带回，调用方直接写进 requestedRect 的对照里
    RECT requested{};
};

// angle 是 DecideDupTransform 出来的那个度数（0/90/180/270）。
// 桌面坐标可以整块在负值区（副屏在主屏左边/上边），换算全部在带符号 64 位里判，
// 判过才转回无符号的像素偏移 —— 旧实现直接用 int 减完再 max(0,·)，
// 越界与相加绕回都拦不住。
DupCropPlan PlanDupCrop(const RECT& desktop, const RECT& target, uint32_t textureWidth,
                        uint32_t textureHeight, uint32_t angle);

// ---------------------------------------------------------------------------
// 适配器 / 输出定位（纯查表：调用方把枚举到的输出摊平成 vector 交进来）
// ---------------------------------------------------------------------------
struct DupOutputInfo {
    uint32_t adapterIndex = 0;  // 属于哪块适配器（D3D11CreateDevice 要按它建设备）
    uint32_t outputIndex = 0;   // 该适配器的第几个输出
    std::wstring deviceName;    // "\\.\DISPLAY1"，与 EnumDisplayMonitors 的 szDevice 同形
    RECT desktopRect{};         // 该输出在虚拟屏幕上的矩形（已含旋转的朝向）
    bool attachedToDesktop = false;  // FALSE = 这块输出当前没接进桌面（禁用/热拔出中）
};

enum class DupPickStatus : uint32_t {
    kFound,
    kNoOutputs,   // 一台适配器都没枚举到输出（远程会话、基本显示驱动）
    kNoMatch,     // 有输出，但没有一块能对上目标
};

struct DupPickResult {
    DupPickStatus status = DupPickStatus::kNoOutputs;
    uint32_t adapterIndex = 0;
    uint32_t outputIndex = 0;
    // 命中那块输出的资料；没命中时是默认值
    DupOutputInfo output{};
    // 对屏幕目标：命中的那块输出是否与请求的屏同名（同名 = 用设备名中的；
    // 只按矩形对上 = 虚拟显卡偶有设备名两边不一致，此时调用方要留一条说明）
    bool matchedByDeviceName = false;
};

// 屏幕目标：先按设备名找，名字对不上再按矩形完全相同找（跳过没接进桌面的输出）。
// 绝不"随便挑一块还在的" —— 换一块屏截就是截了没人批准过的画面。
DupPickResult PickDupOutputForScreen(const std::vector<DupOutputInfo>& outputs,
                                     const std::wstring& deviceName, const RECT& bounds);

// 窗口目标：取与窗口重叠面积最大的那块输出。面积并列时按 (adapterIndex, outputIndex)
// 最小者，保证同一台机器上同一个窗口每次截到同一块屏（不随驱动枚举顺序抖）。
// 跨屏的窗口只会截到最大重叠那一块，剩下的部分由 DupCropPlan 报成 clipped。
DupPickResult PickDupOutputForRect(const std::vector<DupOutputInfo>& outputs, const RECT& window);

// 给人看的输出一览（设备名 + 矩形 + 适配器序号），写进"找不到输出"那类诊断的 hint。
// 纯 ASCII + 数字，不随 --lang 变，调用方能直接拿去对号入座。
std::wstring BriefDupOutputs(const std::vector<DupOutputInfo>& outputs);

}  // namespace ecapture
