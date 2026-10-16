// 跨平台核心边界的离线判据（Windows 由 build\ecapture-core-tests.exe 跑，
// Linux 由 scripts/core-portability-check.sh 用 g++ 直接编好再跑 —— 同一个源文件）。
//
// 为什么要单独一层：这一步交付的是"边界"本身，而边界最容易在下次改动里悄悄漏掉。
// 这个文件的存在就是那道门：**整个文件没有 include 任何 windows.h，也没有用任何
// HWND / RECT / HANDLE / HRESULT / WinRT 类型**。它 include 的是共用核心与那几份
// 共用数据模型头，所以：
//   * 在 Linux 上编得过 = 那一层的模型真的与平台无关（不是把 RECT 改个名字骗自己）；
//   * 在 Windows 上编得过 = 搬走原生类型之后，既有的判据与结构仍然自洽；
//   * 下面这些断言钉住的是"未知必须能表达、判据不许被搬动改松改紧、
//     功能矩阵不许把没实现的平台写成已实现"三条，都是这次划分要守的东西。
//
// 这一层不测截图、不弹框、不碰桌面，也不链任何 Windows 库：它判的就是模型与纯判据。
// 授权状态机本体在 tests\consent_state.cpp，裁剪几何在 tests\crop_state.cpp，
// 这里不重抄它们的用例。

#include <cstdio>
#include <cstdint>
#include <string>
#include <type_traits>

#include "../src/Capture.h"
#include "../src/CaptureScope.h"
#include "../src/CliOptions.h"
#include "../src/Consent.h"
#include "../src/CropGeometry.h"
#include "../src/CursorControl.h"
#include "../src/Deadline.h"
#include "../src/Delivery.h"
#include "../src/DupGeometry.h"
#include "../src/EnvReport.h"
#include "../src/FileSave.h"
#include "../src/HdrColor.h"
#include "../src/HistoryArchive.h"
#include "../src/ImageOps.h"
#include "../src/Json.h"
#include "../src/OutputPlan.h"
#include "../src/Report.h"
#include "../src/ScreenIdentity.h"
#include "../src/ScreenMatch.h"
#include "../src/SystemCompat.h"
#include "../src/WgcGeometry.h"
#include "../src/WindowIdentity.h"
#include "../src/WindowMatch.h"
#include "../src/WindowQuery.h"
#include "../src/core/Authorization.h"
#include "../src/core/Capability.h"
#include "../src/core/Error.h"
#include "../src/core/Frame.h"
#include "../src/core/Geometry.h"
#include "../src/core/Platform.h"
#include "../src/core/Status.h"
#include "../src/core/Target.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("[FAIL] %s\n", what);
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

using namespace ecapture;

PhysRect R(int32_t l, int32_t t, int32_t r, int32_t b) { return PhysRect{l, t, r, b}; }

// ---------------------------------------------------------------------------
// 1) 边界本身：模型里没有平台类型，宽度写死
// ---------------------------------------------------------------------------
void CheckModelShapes() {
    Section("1) 共用模型的形状（不许渗出平台类型）");

    static_assert(sizeof(PhysRect) == 4 * sizeof(int32_t),
                  "PhysRect 必须是四条 int32 边界：换成 LONG/HMONITOR 那一族就破边界了");
    static_assert(std::is_same<decltype(PhysRect::left), int32_t>::value, "PhysRect.left 是 int32");
    static_assert(std::is_same<NativeErrorCode, uint32_t>::value,
                  "原生错误码用 32 位无符号承载（DWORD / HRESULT / errno 都落得下）");
    static_assert(sizeof(CapturedFrame) > 0, "CapturedFrame 在 core/Frame.h 里，不在平台头里");
    static_assert(std::is_same<decltype(CapturedFrame::pixels), std::vector<uint8_t>>::value,
                  "像素缓冲是自有内存，不挂任何句柄");
    static_assert(std::is_same<decltype(CapturedImage::rect), PhysRect>::value,
                  "交付结果里的屏幕矩形是共用坐标模型");
    static_assert(std::is_same<decltype(ScreenInfo::bounds), PhysRect>::value,
                  "屏幕候选的矩形也是共用坐标模型");
    static_assert(std::is_same<decltype(GateTarget::area), PhysRect>::value,
                  "授权清单里的目标区域也是共用坐标模型");

    Check(kNativeErrorNone == 0u, "0 = 没有错误可报（与 Diagnostic.win32 的既有约定同一句）");
    Check(BuildPlatform() == PlatformKind::kWindows || BuildPlatform() == PlatformKind::kUnknown,
          "编译期平台身份要么是 Windows、要么如实未知（不猜 X11 / Wayland）");
#if defined(_WIN32)
    Check(BuildPlatform() == PlatformKind::kWindows, "Windows 构建认得出自己是 Windows");
#else
    Check(!PlatformIsKnown(BuildPlatform()),
          "本步还没有 Linux 后端：非 Windows 构建认不出平台，不许把 kLinuxX11 当默认值");
#endif
    Check(std::string(PlatformName(PlatformKind::kLinuxX11)) == "linux-x11", "平台名是 ASCII 机器名");
    Check(std::string(PlatformName(PlatformKind::kUnknown)) == "unknown", "认不出来就写 unknown");
}

// ---------------------------------------------------------------------------
// 2) 坐标：判据搬家之后必须与搬之前逐字一致
// ---------------------------------------------------------------------------
void CheckGeometry() {
    Section("2) 坐标判据（虚拟屏幕坐标可为负，空矩形不特殊对待）");

    Check(RectIsEmpty(R(0, 0, 0, 0)), "右下不过左上 = 空矩形");
    Check(RectIsEmpty(R(-100, -100, -100, 50)), "零宽也是空");
    Check(!RectIsEmpty(R(-1920, 0, -100, 800)), "负坐标的副屏矩形是正常矩形，不是空");

    // 包含：零容差，且**不**因为"哪一侧是空矩形"而改变结论 —— DesktopPermit::Covers
    // 自己先用 RectIsEmpty 明说拒绝，所以这里的语义必须保持纯四条边比较。
    Check(RectContains(R(0, 0, 100, 100), R(0, 0, 100, 100)), "自身包含自己（边界含）");
    Check(RectContains(R(0, 0, 100, 100), R(10, 10, 90, 90)), "内缩一块当然在里");
    Check(!RectContains(R(0, 0, 100, 100), R(0, 0, 101, 100)), "向外多一格就不算批准过");
    Check(!RectContains(R(0, 0, 100, 100), R(-1, 0, 50, 50)), "向左多一格也不放行");
    Check(!RectContains(R(0, 0, 0, 0), R(0, 0, 10, 10)), "空 outer 装不下任何东西");

    // 重叠：贴边不算重叠（右下不含）；零宽的窗口矩形落在屏内**算**重叠 —— 那是
    // WindowMatch 搬动之前的既有下注，改了它同一条命令行会忽然找不到窗口。
    Check(RectOverlaps(R(0, 0, 100, 100), R(50, 50, 200, 200)), "真重叠");
    Check(!RectOverlaps(R(0, 0, 100, 100), R(100, 0, 200, 100)), "只贴边不算重叠");
    Check(RectOverlaps(R(10, 10, 10, 100), R(0, 0, 100, 100)), "零宽窗口矩形落在屏内仍算命中（保持既有行为）");

    Check(RectWidth(R(0, 0, 1920, 1080)) == 1920, "宽 = right - left");
    Check(RectHeight(R(-1920, -100, -1000, 400)) == 500, "负坐标区的尺寸照样算得对");
    Check(RectWidth(R(10, 0, 5, 100)) == 0, "右下在左上之前 = 尺寸 0，不返回负数");
    // 极端边界：坐标相减在 64 位里算完再收回来，不许绕回成一个小正数
    Check(RectWidth(R(INT32_MIN, 0, INT32_MAX, 1)) == INT32_MAX,
          "整条虚拟屏幕宽度贴到 int32 上限时不绕回");
    Check(RectHeight(R(0, INT32_MIN, 1, INT32_MAX)) == INT32_MAX, "高的同一条判据");

    MaybePhysRect unknown = MaybePhysRect::Unknown();
    Check(!unknown.known, "问不出来 = known false");
    Check(!unknown.IsEmpty(), "问不出来**不**等于\"这块矩形是空的\"：那是没有答案");
    Check(MaybePhysRect::Of(R(0, 0, 0, 0)).IsEmpty(), "零尺寸是一块量出来的事实");
}

// ---------------------------------------------------------------------------
// 3) 图像帧：上限与默认值都是"没答案就说没答案"
// ---------------------------------------------------------------------------
void CheckFrameModel() {
    Section("3) 帧模型（上限同源，未填的字段不冒充读数）");

    Check(kFrameMaxSide == 16384u, "单边上限仍是 D3D11 那条线（ImageOps 与 GDI 共用同一个数）");
    Check(kFrameMaxBytes == 1024ull * 1024ull * 1024ull, "整帧上限仍是 1 GiB");

    CapturedFrame f;
    Check(f.width == 0 && f.height == 0 && f.stride == 0, "默认是空帧，不是假装有一帧");
    Check(!f.reportsCrop && !f.clipped, "没裁过就不报告裁剪事实");
    Check(f.rotation == 0, "没转过就是 0 度");
    Check(!f.cursorStateKnown && !f.cursorInFrame,
          "光标那两格默认是\"没有读数\"，不是\"读过并且确认没有\"");
    Check(f.displayHdrState == DisplayHdrState::kUnknown,
          "HDR 显示模式没问过就是 kUnknown，绝不折成 kSdr");
    Check(f.sourceColorSpace == FrameColorSpace::kSrgbBgra8, "常见的 8 位 BGRA 是默认那一档");

    RenderOutcome o;
    Check(o.status == BlockedStatus::kOk, "默认状态是没失败");
    Check(o.win32 == kNativeErrorNone && o.hresult == kNativeErrorNone,
          "两个错误码都是 32 位承载值，默认 0 = 不适用");

    // 帧的坐标也走共用模型：填进去、读出来，四条边原样。
    f.reportsCrop = true;
    f.requestedRect = R(-1920, 0, -100, 800);
    f.capturedRect = R(-1920, 0, -200, 800);
    f.clipped = true;
    Check(f.requestedRect.left == -1920 && f.capturedRect.right == -200, "负坐标原样保留");
}

// ---------------------------------------------------------------------------
// 4) 目标描述：跨平台、跨会话不许比出"相同"，比不了就是比不了
// ---------------------------------------------------------------------------
void CheckTargetRef() {
    Section("4) 目标描述（平台 + 会话 + 那一家自己的标识）");

    TargetRef win{};
    win.domain = TargetDomain::kWindow;
    win.platform = PlatformKind::kWindows;
    win.session = SessionRef{true, 7};
    win.form = NativeIdForm::kNumber;
    win.number = 0x1234;

    Check(win.HasNativeId(), "有整数标识时认得出\"有标识\"");
    TargetRef noId = win;
    noId.number = kUnknownNativeId;
    Check(!noId.HasNativeId(), "0 永远是\"没拿到标识\"，不是一号窗口");

    TargetRef text{};
    text.domain = TargetDomain::kWindow;
    text.platform = PlatformKind::kLinuxWayland;
    text.session = SessionRef{true, 1};
    text.form = NativeIdForm::kText;
    text.text = "wayland-toplevel-1";
    Check(text.HasNativeId(), "Wayland 的不透明句柄用字符串形状表达，不压成一个假整数");

    Check(SameTargetAs(win, win) == Tri::kYes, "同平台同会话同标识 = 同一个目标");
    TargetRef other = win;
    other.number = 0x9999;
    Check(SameTargetAs(win, other) == Tri::kNo, "同一次会话里标识不同 = 敢断言换对象了");
    TargetRef otherSession = win;
    otherSession.session.id = 8;
    Check(SameTargetAs(win, otherSession) == Tri::kUnknown,
          "跨会话比不了：不写 kYes，也不写 kNo");
    TargetRef unknownSession = win;
    unknownSession.session.known = false;
    Check(SameTargetAs(win, unknownSession) == Tri::kUnknown, "会话问不出来 = 没有答案");
    TargetRef otherPlatform = win;
    otherPlatform.platform = PlatformKind::kLinuxX11;
    Check(SameTargetAs(win, otherPlatform) == Tri::kUnknown,
          "跨平台的数值相等不代表同一目标（HWND 与 XID 不在同一个数空间里）");
    Check(SameTargetAs(TargetRef{}, TargetRef{}) == Tri::kUnknown,
          "两个空引用比不出\"相同\"：域与平台都不明");

    Check(std::string(TargetDomainName(TargetDomain::kScreen)) == "screen", "目标域是 ASCII 机器名");
    Check(std::string(NativeIdFormName(NativeIdForm::kUnknown)) == "unknown", "没登记的标识形状");
}

// ---------------------------------------------------------------------------
// 5) 功能矩阵：每一格都得有明确说法，没实现的平台不许冒充
// ---------------------------------------------------------------------------
void CheckMatrix() {
    Section("5) 跨平台功能矩阵");

    const PlatformKind platforms[] = {PlatformKind::kWindows, PlatformKind::kLinuxX11,
                                      PlatformKind::kLinuxWayland};
    const MatrixFeature features[] = {MatrixFeature::kTargetListing, MatrixFeature::kWindowCapture,
                                      MatrixFeature::kScreenCapture, MatrixFeature::kInteractiveGrant,
                                      MatrixFeature::kMinimizedWindow, MatrixFeature::kCursorControl,
                                      MatrixFeature::kHdr, MatrixFeature::kHeadlessSession};

    uint32_t cells = 0;
    const MatrixCell* table = PlatformMatrixCells(&cells);
    const uint32_t expected = static_cast<uint32_t>(
        (sizeof(platforms) / sizeof(platforms[0])) * (sizeof(features) / sizeof(features[0])));
    Check(cells == expected, "矩阵是满的：三个平台族 × 八项功能，一格都不缺");

    for (PlatformKind p : platforms) {
        for (MatrixFeature f : features) {
            const MatrixStatus s = MatrixStatusOf(p, f);
            Check(s != MatrixStatus::kUnknown, "每格都必须明确登记，kUnknown 不许当兜底值");
            if (p != PlatformKind::kWindows) {
                Check(!MatrixIsCompiled(s),
                      "非 Windows 那一列不许声称已实现（本构建一行 Linux 取帧代码都没有）");
            }
        }
    }
    // 没登记过的平台族（kUnknown）不该被误读成"支持"
    Check(MatrixStatusOf(PlatformKind::kUnknown, MatrixFeature::kWindowCapture) ==
              MatrixStatus::kUnknown,
          "没登记的平台族 = 没登记，不套任何一列的答案");

    Check(MatrixIsCompiled(MatrixStatusOf(PlatformKind::kWindows, MatrixFeature::kWindowCapture)),
          "Windows 的窗口截图是这个构建里的事实");
    Check(MatrixStatusOf(PlatformKind::kWindows, MatrixFeature::kHdr) == MatrixStatus::kUnverified,
          "Windows 的 HDR 记\"实现了但没在 HDR 显示器上实测过\"，不写成已验证");
    Check(MatrixStatusOf(PlatformKind::kWindows, MatrixFeature::kMinimizedWindow) ==
              MatrixStatus::kNotSupported,
          "最小化窗口只能列出来、截不到（既有行为就是这条判据）");
    Check(MatrixStatusOf(PlatformKind::kLinuxWayland, MatrixFeature::kTargetListing) ==
              MatrixStatus::kNotSupported,
          "Wayland 没有全局窗口列表：登记成\"不做\"，不去虚构一个");
    Check(std::string(MatrixFeatureName(MatrixFeature::kCount)) == "unknown",
          "功能轴加了新值却忘了登记名字时，读出来是 unknown 而不是崩溃");

    // 每一格都得带一句为什么（note 非空），否则 README 与表会各说一套
    for (uint32_t i = 0; i < cells; ++i) {
        Check(table[i].note != nullptr && table[i].note[0] != '\0', "矩阵每一格都要写明依据");
    }
}

// ---------------------------------------------------------------------------
// 6) 隐私边界：--yes 的适用范围、以及 Wayland 少一层授权就不放行
// ---------------------------------------------------------------------------
void CheckPrivacy() {
    Section("6) 隐私与授权层");

    Check(YesGivenExempts(PixelScope::kWindowContent, true),
          "只有真正绑定窗口自己的画面那一档，才可能因 --yes 免掉程序确认");
    Check(!YesGivenExempts(PixelScope::kWindowContent, false), "没给 --yes 就要问");
    Check(!YesGivenExempts(PixelScope::kDesktop, true),
          "桌面像素 + --yes 仍然要问：先取全屏再裁成窗口大小也还是桌面截图");
    Check(!YesGivenExempts(PixelScope::kDesktop, false), "同上，没给 --yes 更要问");

    // 登记表那一半（漏登记 = 更严格而不是更松）
    Check(!NeedsHumanConsent(paths::kWgc, true), "wgc 是窗口内容路径");
    Check(NeedsHumanConsent(paths::kBitBltScreen, true), "拷屏幕那条永远要人确认");
    Check(NeedsHumanConsent(paths::kDwmScreen, true), "通道内部的屏幕退路同样要问");
    Check(NeedsHumanConsent(paths::kScreenWgc, true), "整块屏的 WGC 也算桌面像素");
    Check(NeedsHumanConsent(L"never.registered", true), "没登记过的路径名按桌面处理");
    Check(ScopeOf(L"") == PixelScope::kDesktop, "说不出来源 = 按最危险的处理");
    Check(RegisteredCapturePaths().size() >= 10, "登记表整份对外，能力查询不再抄第二份");

    // 两层授权：程序确认 + 系统授权。Wayland 两层都要，缺系统那一层就是没授权。
    const uint32_t win = RequiredGrantLayers(PlatformKind::kWindows);
    Check((win & kGrantProgramConfirm) != 0, "Windows 至少要程序自己问一次");
    Check((win & kGrantSystemAuthorization) == 0,
          "Windows 现在没有每次调用的系统截屏授权这一层：如实登记\"没有\"，不提前编一个");
    const uint32_t wayland = RequiredGrantLayers(PlatformKind::kLinuxWayland);
    Check(HasAllGrantLayers(wayland, kGrantProgramConfirm | kGrantSystemAuthorization),
          "Wayland 两层都齐 = 放行");
    Check(!HasAllGrantLayers(wayland, kGrantProgramConfirm),
          "只有程序确认框不够：Portal 那一次授权不能被本程序的确认替代");
    Check(!HasAllGrantLayers(wayland, kGrantSystemAuthorization),
          "只有系统授权也不够：桌面像素仍然要本程序问一次人");

    // 授权结果默认值：关框、弹不出来、没人能答，都不许被读成同意
    ConsentReply reply;
    Check(reply.answer == ConsentAnswer::kInitFailed, "默认是\"没弹成\"，不是\"同意\"");
    Check(reply.nativeError == kNativeErrorNone, "没失败过就没有系统码");
    Check(ConsentAnswer::kAccepted != ConsentAnswer::kDeclined &&
              ConsentAnswer::kDeclined != ConsentAnswer::kClosed &&
              ConsentAnswer::kClosed != ConsentAnswer::kTimedOut &&
              ConsentAnswer::kTimedOut != ConsentAnswer::kCancelled &&
              ConsentAnswer::kCancelled != ConsentAnswer::kInitFailed,
          "同意 / 答否 / 关框 / 超时 / 取消 / 弹不出框是六种分开的结局");
}

// ---------------------------------------------------------------------------
// 7) 能力查询那三件事仍然分开：编译支持 / 本机可用 / 实测过
// ---------------------------------------------------------------------------
void CheckCapabilityModel() {
    Section("7) 能力模型分层");

    Check(Tri::kYes != Tri::kNo, "三值判据的三种下场互不相同");
    static_assert(static_cast<int>(Tri::kUnknown) == 2, "unknown 还是那一个值（协议里的数不动）");
    Check(CapStatus::kAvailable != CapStatus::kUnavailable &&
              CapStatus::kUnavailable != CapStatus::kUnverified,
          "可用 / 被挡掉 / 没能核实是三种不同结论，不许合并");
    Check(MatrixIsCompiled(MatrixStatus::kRuntimeProbe),
          "runtime_probe 说的是\"实现了，能不能兑现要问运行时\"，所以它算有实现");
    Check(!MatrixIsCompiled(MatrixStatus::kPlanned) && !MatrixIsCompiled(MatrixStatus::kNotSupported),
          "计划支持与明确不做都不是\"这个构建做得到\"");
}

}  // namespace

int main() {
    CheckModelShapes();
    CheckGeometry();
    CheckFrameModel();
    CheckTargetRef();
    CheckMatrix();
    CheckPrivacy();
    CheckCapabilityModel();

    std::printf("\ncore-boundary: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    if (g_failures != 0) return 1;
    std::printf("这一份文件没有 include windows.h，也不含任何平台类型：边界成立。\n");
    return 0;
}
