#include "CaptureDuplication.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "CaptureCommon.h"
#include "D3dDevice.h"
#include "DupGeometry.h"
#include "ImageOps.h"

using Microsoft::WRL::ComPtr;

namespace ecapture {
namespace {

constexpr const wchar_t* kChannel = L"duplication";
constexpr UINT kAcquireSliceMs = 100;

// 取到的桌面帧必须 ReleaseFrame，否则显示器的这份资源一直被占着
class AcquiredFrame {
public:
    AcquiredFrame(IDXGIOutputDuplication* dup, const DXGI_OUTDUPL_FRAME_INFO& info,
                  IDXGIResource* resource)
        : dup_(dup), info_(info), resource_(resource) {}
    AcquiredFrame(const AcquiredFrame&) = delete;
    AcquiredFrame& operator=(const AcquiredFrame&) = delete;
    ~AcquiredFrame() {
        if (dup_) dup_->ReleaseFrame();
    }
    const DXGI_OUTDUPL_FRAME_INFO& info() const { return info_; }
    IDXGIResource* resource() const { return resource_.Get(); }

private:
    ComPtr<IDXGIOutputDuplication> dup_;
    DXGI_OUTDUPL_FRAME_INFO info_{};
    ComPtr<IDXGIResource> resource_;
};

DupRotation ToDupRotation(DXGI_MODE_ROTATION r) {
    switch (r) {
        case DXGI_MODE_ROTATION_IDENTITY: return DupRotation::kIdentity;
        case DXGI_MODE_ROTATION_ROTATE90: return DupRotation::kRotate90;
        case DXGI_MODE_ROTATION_ROTATE180: return DupRotation::kRotate180;
        case DXGI_MODE_ROTATION_ROTATE270: return DupRotation::kRotate270;
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        default: return DupRotation::kUnspecified;
    }
}

// 枚举到的输出：纯判据用的描述 + 后面真要用那两个 COM 对象。三者必须放在一起，
// 因为"选中哪块输出"是由纯函数按序号给的，回来要用同一序号取活的输出与它的适配器。
struct LiveOutput {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output1;
    DupOutputInfo info;
};

// 把**所有**适配器上的**所有**输出摊平成一张表。
//
// 旧做法是先建默认适配器的设备、再只枚举那个适配器的输出：目标屏由第二块显卡（核显通常
// 才是接了屏的那块）驱动时，表里根本不会有它，于是"窗口不在任何屏幕上"这种假结论就出来了。
// 顺序必须反过来：先按拓扑定位目标，再到目标所属的适配器上建设与它匹配的设备。
bool BuildOutputTable(std::vector<LiveOutput>* out, Diagnostic* err) {
    ComPtr<IDXGIFactory1> factory;
    const HRESULT fhr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(fhr)) {
        CaptureError(err, kChannel, Msgf(L"cap.dup.factory", HResultText(fhr)),
                     Msg(L"cap.dup.no_outputs_hint"), codes::kCaptureFailed, 0, fhr);
        return false;
    }
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            ComPtr<IDXGIOutput1> as1;
            if (FAILED(output.As(&as1))) continue;   // Win8 之前的输出没有复制能力
            LiveOutput entry;
            entry.adapter = adapter;
            entry.output1 = as1;
            entry.info.adapterIndex = a;
            entry.info.outputIndex = o;
            entry.info.deviceName = desc.DeviceName;
            entry.info.desktopRect = desc.DesktopCoordinates;
            entry.info.attachedToDesktop = desc.AttachedToDesktop != FALSE;
            out->push_back(std::move(entry));
        }
    }
    return true;
}

const LiveOutput* FindLive(const std::vector<LiveOutput>& table, const DupPickResult& pick) {
    for (const LiveOutput& e : table) {
        if (e.info.adapterIndex == pick.adapterIndex && e.info.outputIndex == pick.outputIndex) {
            return &e;
        }
    }
    return nullptr;
}

// 没找到目标输出时那条诊断：屏幕目标与窗口目标的下一步不同（前者要重新选屏，后者要把窗口
// 挪回屏内），所以 hint 分开写，并且都把本机实际枚举到的输出列出来 —— 只有"哪块卡上有什么"
// 摆在眼前，调用方才分得清"这块屏真的不在桌面里"和"本工具没看第二块卡"。
void ReportNoOutput(Diagnostic* err, const std::vector<DupOutputInfo>& table, bool forScreen,
                    const std::wstring& targetName) {
    const std::wstring list = BriefDupOutputs(table);
    if (forScreen) {
        CaptureError(err, kChannel, Msgf(L"cap.dup.no_output_for_screen", targetName),
                     Msgf(L"cap.dup.no_output_for_screen_hint", list));
        return;
    }
    CaptureError(err, kChannel, Msg(L"cap.dup.no_output"),
                 Msgf(L"cap.dup.no_output_hint", list));
}

std::wstring DuplicationHint(HRESULT hr) {
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        return Msg(L"cap.dup.hint_unsupported");
    }
    if (hr == E_ACCESSDENIED) {
        return Msg(L"cap.dup.hint_denied");
    }
    if (hr == DXGI_ERROR_INVALID_CALL) {
        return Msg(L"cap.dup.hint_invalid_call");
    }
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return Msg(L"cap.dup.hint_timeout");
    }
    // 显示器被拔掉 / 驱动重置 / 复制被别的进程抢占：这一帧的来路已经不在了。
    // 说清楚"不会静默换一块屏"，因为换屏截到的画面虽然尺寸一样，内容却是别人的桌面。
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_NOT_FOUND) {
        return Msg(L"cap.dup.hint_lost");
    }
    return std::wstring();
}

// 该输出的桌面矩形（用户看到的朝向，与虚拟屏幕坐标同一系）
uint32_t DesktopWidthOf(const RECT& r) {
    return r.right > r.left ? static_cast<uint32_t>(static_cast<int64_t>(r.right) - r.left) : 0u;
}
uint32_t DesktopHeightOf(const RECT& r) {
    return r.bottom > r.top ? static_cast<uint32_t>(static_cast<int64_t>(r.bottom) - r.top) : 0u;
}

// 把 GPU 上的桌面帧拷进 CPU 可读的 staging 纹理，再按行搬进帧。
// 拷贝那一段与 wgc 通道共用（CaptureCommon.h 的 CopyTextureToFrame）：形状、行距与上限都在
// 分配之前判完，Map 之后抛异常也要 Unmap。桌面纹理跟随该输出的显示模式：HDR 时它可能不是
// B8G8R8A8；hdr 决定这一步认不认广色域来源、以及认回来之后是映射（tonemap）还是拒绝（refuse），
// dl 守映射那趟扫描的预算。没写 --hdr 时照旧只认 B8G8R8A8（非它就报 cap.frame_format）。
bool CopyDesktopToCpu(ID3D11Device* device, IDXGIResource* resource, const HdrRequest& hdr,
                      const Deadline& dl, CapturedFrame* out, Diagnostic* err) {
    // 桌面复制给的资源本身就是 D3D11 纹理，直接 QueryInterface 到 ID3D11Texture2D 即可
    ComPtr<ID3D11Texture2D> desktop;
    HRESULT hr = resource->QueryInterface(IID_PPV_ARGS(&desktop));
    if (FAILED(hr)) {
        CaptureError(err, kChannel, Msg(L"cap.dup.to_texture"), Msgf(L"cap.hresult", HResultText(hr)),
                     codes::kCaptureFailed, 0, hr);
        return false;
    }
    return CopyTextureToFrame(device, desktop.Get(), kChannel, hdr, dl, out, err);
}

// 一次整幅桌面帧的采集结果：纹理朝向的那份像素 + 该输出的桌面矩形 + 驱动报回的旋转。
struct DesktopGrab {
    CapturedFrame frame;
    RECT desktop{};
    DupRotation rotation = DupRotation::kUnspecified;
};

// 取该输出的整幅桌面帧：等一次真实 present、拷进 CPU、把"这一帧根本没有画面"判掉。
// 单色只在调用方那侧留一条质量提示（note.frame_uniform），不在这儿拒绝图片。
bool GrabOutputFrame(ID3D11Device* device, const LiveOutput& picked, uint32_t timeoutMs,
                     const HdrRequest& hdr, const Deadline& dl, DesktopGrab* out, Diagnostic* err) {
    ComPtr<IDXGIOutputDuplication> dup;
    const HRESULT hr = picked.output1->DuplicateOutput(device, &dup);
    if (FAILED(hr)) {
        CaptureError(err, kChannel, Msgf(L"cap.dup.duplicate", HResultText(hr)),
                     DuplicationHint(hr), codes::kCaptureFailed, 0, hr);
        return false;
    }

    // 旋转与显示模式以这次复制自己的描述为准（它就是这张帧的出处），拿不到时按"没报"处理
    DXGI_OUTDUPL_DESC dupDesc{};
    dup->GetDesc(&dupDesc);
    out->rotation = ToDupRotation(dupDesc.Rotation);
    out->desktop = picked.info.desktopRect;

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    const DWORD deadline = GetTickCount() + (timeoutMs ? timeoutMs : 2000u);
    // 刚建好复制时拿到的往往是"只有时间信息、没有新画面"的空帧，纹理里可能还是空内容。
    // 先给真实 present 一点宽限期，超期就用当前帧（静态桌面上它仍是有效的当前画面）。
    constexpr UINT kRealFrameGraceMs = 300;
    const DWORD graceEnd = GetTickCount() + kRealFrameGraceMs;
    bool have = false;
    while (!have) {
        const int32_t remaining = static_cast<int32_t>(deadline - GetTickCount());
        if (remaining <= 0) {
            CaptureError(err, kChannel, Msgf(L"cap.dup.timeout", timeoutMs),
                         DuplicationHint(DXGI_ERROR_WAIT_TIMEOUT), codes::kFrameTimeout);
            return false;
        }
        const HRESULT acquire = dup->AcquireNextFrame(
            std::min(kAcquireSliceMs, static_cast<UINT>(remaining)), &info, &resource);
        if (acquire == DXGI_ERROR_WAIT_TIMEOUT || acquire == S_FALSE) continue;
        if (FAILED(acquire)) {
            CaptureError(err, kChannel, Msgf(L"cap.dup.acquire", HResultText(acquire)),
                         DuplicationHint(acquire),
                         // 复制被抢占/设备没了：这一帧的来路已经不在了，与"目标不受支持"不同，
                         // 单给一条码让调用方知道该重新枚举拓扑而不是换通道再试。
                         (acquire == DXGI_ERROR_ACCESS_LOST || acquire == DXGI_ERROR_NOT_FOUND)
                             ? codes::kMonitorChanged
                             : codes::kCaptureFailed,
                         0, acquire);
            return false;
        }
        const bool presented = info.LastPresentTime.QuadPart != 0 || info.AccumulatedFrames > 0;
        if (presented || static_cast<int32_t>(GetTickCount() - graceEnd) >= 0) {
            have = true;
        } else {
            dup->ReleaseFrame();
            resource.Reset();
        }
    }

    bool copied = false;
    {
        AcquiredFrame guard(dup.Get(), info, resource.Get());
        copied = CopyDesktopToCpu(device, guard.resource(), hdr, dl, &out->frame, err);
    }  // 这里 ReleaseFrame，之后才能安全地只用 CPU 副本
    if (!copied) return false;

    // 单色本身不是失败理由：桌面上那一刻真的可能就是一整幅同一个颜色。
    // 判"根本没拿到画面"要两条**API 层面**的证据凑齐：这一帧既没有任何 present 记录
    //（LastPresentTime 与 AccumulatedFrames 都是 0 —— 纹理还没被写过），整幅又只有一个颜色。
    // 只要驱动真 present 过，这一帧就是当前画面，颜色单也照样交出去
    //（调用方另外会收到一条 note.frame_uniform 质量提示，那是提示不是错误）。
    // 整幅（裁剪与旋转之前）判，避免"窗口区域本来就单色"被误判成空帧。
    const bool presented = info.LastPresentTime.QuadPart != 0 || info.AccumulatedFrames > 0;
    FrameColor uniform{};
    if (!presented && FrameIsUniform(out->frame, &uniform)) {
        CaptureError(err, kChannel,
                     Msgf(L"cap.dup.flat", info.LastPresentTime.QuadPart, info.AccumulatedFrames,
                          info.ProtectedContentMaskedOut ? 1 : 0),
                     Msg(L"cap.dup.flat_hint"));
        out->frame.pixels.clear();
        out->frame.width = out->frame.height = out->frame.stride = 0;
        return false;
    }
    out->frame.source = kChannel;
    return true;
}

// 旋转判据 + 裁剪计划：从纹理朝向的整幅帧里，按目标矩形取出该交付的那一块。
// 交付的坐标空间因此恒等于虚拟屏幕坐标（也就是人工确认时给人看的那些矩形所在的系）。
bool ExtractTargetRect(const DesktopGrab& grab, const RECT& target, CapturedFrame* out,
                       Diagnostic* err) {
    const DupTransformDecision transform =
        DecideDupTransform(grab.rotation, DesktopWidthOf(grab.desktop),
                           DesktopHeightOf(grab.desktop), grab.frame.width, grab.frame.height);
    if (!transform.ok) {
        // 帧的尺寸与这块输出宣称的桌面尺寸对不上：这张帧不是当前桌面，不能拿它去裁。
        CaptureError(err, kChannel, Msgf(L"cap.dup.frame_shape", static_cast<uint32_t>(transform.reason)),
                     Msgf(L"cap.dup.frame_shape_hint", transform.desktopWidth, transform.desktopHeight,
                          transform.textureWidth, transform.textureHeight,
                          static_cast<uint32_t>(grab.rotation)),
                     codes::kFrameInvalid);
        return false;
    }

    const DupCropPlan plan = PlanDupCrop(grab.desktop, target, grab.frame.width,
                                         grab.frame.height, transform.angle);
    if (!plan.ok) {
        switch (plan.reason) {
            case DupCropReason::kTargetEmpty:
                CaptureError(err, kChannel, Msg(L"cap.rect_empty"), Msg(L"cap.window_gone"));
                return false;
            case DupCropReason::kNotOnOutput:
                CaptureError(err, kChannel, Msg(L"cap.dup.not_in_frame"),
                             Msg(L"cap.dup.not_in_frame_hint"));
                return false;
            case DupCropReason::kOutsideTexture:
            case DupCropReason::kBadAngle:
            default:
                CaptureError(err, kChannel, Msgf(L"cap.dup.crop_outside",
                                                 static_cast<uint32_t>(plan.reason)),
                             Msgf(L"cap.dup.crop_outside_hint", plan.src.left, plan.src.top,
                                  plan.src.right, plan.src.bottom, grab.frame.width,
                                  grab.frame.height),
                             codes::kFrameInvalid);
                return false;
        }
    }

    CapturedFrame cropped;
    if (!RotateCropFrame(grab.frame, plan.src, transform.angle, &cropped)) {
        CaptureError(err, kChannel, Msg(L"cap.crop_failed"),
                     Msgf(L"cap.crop_failed_hint", static_cast<uint64_t>(plan.src.left),
                          static_cast<uint64_t>(plan.src.top),
                          static_cast<uint64_t>(plan.src.right - plan.src.left),
                          static_cast<uint64_t>(plan.src.bottom - plan.src.top)),
                     codes::kFrameInvalid);
        return false;
    }
    cropped.source = kChannel;
    // 交付的每一层坐标都留痕：调用方要能核对"本来要截哪一块、实际截到哪一块、转了多少度"，
    // 而不是只看见"取到一张图"。
    cropped.reportsCrop = true;
    cropped.requestedRect = target;
    cropped.capturedRect = plan.captured;
    cropped.clipped = plan.clipped;
    cropped.rotation = transform.angle;
    *out = std::move(cropped);
    return true;
}

// 屏幕目标必须交出整块屏：裁不全 = 拓扑在确认之后变了，不能交一张"尺寸比该屏小"的图，
// 也不能悄悄换成另一块输出（那块屏的画面从来没人批准过）。
bool ExtractWholeScreen(const DesktopGrab& grab, const RECT& bounds, CapturedFrame* out,
                        Diagnostic* err) {
    CapturedFrame frame;
    if (!ExtractTargetRect(grab, bounds, &frame, err)) return false;
    if (frame.clipped || frame.width != DesktopWidthOf(bounds) ||
        frame.height != DesktopHeightOf(bounds)) {
        const uint32_t wantW = DesktopWidthOf(bounds);
        const uint32_t wantH = DesktopHeightOf(bounds);
        CaptureError(err, kChannel, Msg(L"cap.dup.screen_changed"),
                     Msgf(L"cap.dup.screen_changed_hint", wantW, wantH, frame.width, frame.height,
                          DesktopWidthOf(grab.desktop), DesktopHeightOf(grab.desktop)),
                     codes::kMonitorChanged);
        return false;
    }
    *out = std::move(frame);
    return true;
}

// 公共路线：定位输出 -> 在该输出的适配器上建设备 -> 取整幅桌面帧 -> 按旋转换算 -> 取出目标矩形
bool CaptureRectDuplication(const RECT& rect, const ScreenInfo* screen, const wchar_t* path,
                            uint32_t timeoutMs, const HdrRequest& hdr, const Deadline& dl,
                            CapturedFrame* out, Diagnostic* err) {
    std::vector<LiveOutput> table;
    if (!BuildOutputTable(&table, err)) return false;

    std::vector<DupOutputInfo> infos;
    infos.reserve(table.size());
    for (const LiveOutput& e : table) infos.push_back(e.info);

    DupPickResult pick;
    if (screen) {
        pick = PickDupOutputForScreen(infos, screen->deviceName, screen->bounds);
    } else {
        pick = PickDupOutputForRect(infos, rect);
    }
    if (pick.status != DupPickStatus::kFound) {
        if (pick.status == DupPickStatus::kNoOutputs) {
            CaptureError(err, kChannel, Msg(L"cap.dup.no_outputs"),
                         Msgf(L"cap.dup.no_outputs_hint", BriefDupOutputs(infos)));
        } else {
            ReportNoOutput(err, infos, screen != nullptr,
                           screen ? ScreenDisplayName(*screen) : std::wstring());
        }
        return false;
    }
    const LiveOutput* live = FindLive(table, pick);
    if (!live) {
        // 表是刚由纯函数按序号给的，序号却找不到对应对象 = 上面的对应关系写错了
        CaptureError(err, kChannel, Msgf(L"cap.dup.output_lost", static_cast<uint32_t>(pick.adapterIndex),
                                         static_cast<uint32_t>(pick.outputIndex)),
                     Msg(L"cap.dup.output_lost_hint"), codes::kMonitorChanged);
        return false;
    }

    HRESULT deviceHr = S_OK;
    ComPtr<ID3D11Device> device = CreateDeviceOnAdapter(live->adapter.Get(), &deviceHr);
    if (!device) {
        // 明说是在哪块适配器上建的、为什么不退 WARP：那条路给不出物理输出的像素
        CaptureError(err, kChannel, Msgf(L"cap.dup.adapter_device", static_cast<uint32_t>(pick.adapterIndex),
                                         HResultText(deviceHr)),
                     Msgf(L"cap.dup.adapter_device_hint", live->info.deviceName),
                     codes::kCaptureFailed, 0, deviceHr);
        return false;
    }

    DesktopGrab grab;
    if (!GrabOutputFrame(device.Get(), *live, timeoutMs, hdr, dl, &grab, err)) return false;
    if (screen) {
        if (!ExtractWholeScreen(grab, screen->bounds, out, err)) return false;
    } else {
        if (!ExtractTargetRect(grab, rect, out, err)) return false;
    }
    // 裁成窗口大小不改变来路：这一帧取自显示器合成分，仍然是桌面像素。
    out->path = path;
    return true;
}

// 画面被这块输出丢掉一部分时，帧里带着 capturedRect / clipped 交给流水线，由它发那条
// 可机器处理的提示（它才知道当前目标是哪一个）—— 判据不在通道与流水线两处各写一遍。

// 入口先把上一帧的形状与几何判据清零：失败路径上调用方拿到的必须是一张"空的"帧，
// 而不是一半旧像素 + 一半新几何那种自相矛盾的东西。
void ResetFrameGeometry(CapturedFrame* out) {
    out->pixels.clear();
    out->width = out->height = out->stride = 0;
    out->reportsCrop = false;
    out->requestedRect = RECT{};
    out->capturedRect = RECT{};
    out->clipped = false;
    out->rotation = 0;
    out->sourceColorSpace = FrameColorSpace::kSrgbBgra8;
    out->toneMapped = false;
    // 这一条路径采集之前**没有**去问过那块屏此刻的色彩空间（它仍用 DuplicateOutput()，也不选
    // 广色域格式），所以这里的 kUnknown 是"没问过"这条事实本身，不是没清干净。显式 HDR 策略下
    // 这条通道早就被 HDR 闸门摘掉了（src/HdrColor.h 的 kWideGamutUnverified），走不到这一步；
    // 留这一句是为了 --hdr auto 那一路也别想在帧上留下一个来历不明的值。
    out->displayHdrState = DisplayHdrState::kUnknown;
}

}  // namespace

bool CaptureWindowDuplication(uint64_t hwndValue, uint32_t timeoutMs, const DesktopPermit& permit,
                              const HdrRequest& hdr, const Deadline& dl, CapturedFrame* out,
                              Diagnostic* err) {
    const HWND hwnd = reinterpret_cast<HWND>(hwndValue);
    ResetFrameGeometry(out);

    const RECT ext = WindowScreenRect(hwnd);
    if (ext.right <= ext.left || ext.bottom <= ext.top) {
        CaptureError(err, kChannel, Msg(L"cap.rect_empty"), Msg(L"cap.window_gone"));
        return false;
    }
    if (!PermitCovers(permit, ext, kChannel, err)) return false;
    return CaptureRectDuplication(ext, nullptr, paths::kDuplicationFrame, timeoutMs, hdr, dl, out,
                                  err);
}

bool CaptureScreenDuplication(const ScreenInfo& screen, uint32_t timeoutMs,
                              const DesktopPermit& permit, const HdrRequest& hdr, const Deadline& dl,
                              CapturedFrame* out, Diagnostic* err) {
    ResetFrameGeometry(out);

    if (screen.bounds.right <= screen.bounds.left || screen.bounds.bottom <= screen.bounds.top) {
        CaptureError(err, kChannel, Msg(L"cap.rect_empty_screen"), Msg(L"cap.screen_rect_broken"));
        return false;
    }
    if (!PermitCovers(permit, screen.bounds, kChannel, err)) return false;
    // 屏幕目标取的是整块屏，本来就该与授权矩形一模一样；任何裁剪都在上面判成"拓扑变了"
    return CaptureRectDuplication(screen.bounds, &screen, paths::kScreenDuplication, timeoutMs, hdr,
                                  dl, out, err);
}

}  // namespace ecapture
