// 【平台层 · Windows】显示器来源：EnumDisplayMonitors 枚举、显示配置（QueryDisplayConfig）
// 问答、DPI 与朝向问答，以及按选择器取屏/取屏矩形的那两个运行时入口。
//
// 从 src/ScreenMatch.cpp（EnumScreens / SelectScreens / SelectedScreenRects）与
// src/ScreenIdentity.cpp（EnumScreenCandidates 与它依赖的几条 Win32 问答）搬出来：
// 判据本体（SelectScreenCandidates / CompareScreenIdentity / RecheckScreenSampling /
// MonitorSelectorLabel）留在共用层，本文件只把"这台机器现在有哪些屏、它们各自是什么"
// 问出来。声明在 src/ScreenMatch.h 与 src/ScreenIdentity.h。
//
// 本文件不取一个像素、不弹确认框、不写文件，也**绝不**改任何显示设置。

#include "ScreenIdentity.h"
#include "ScreenMatch.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"   // EnsureDpiAware / LastError / HResultText
#include "ErrorText.h"
#include "platform/windows/WinBridge.h"

namespace ecapture {
namespace {

BOOL CALLBACK CollectMonitor(HMONITOR hmon, HDC /*dc*/, LPRECT /*rect*/, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<ScreenInfo>*>(lParam);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hmon, &info)) return TRUE;

    ScreenInfo s;
    s.monitor = reinterpret_cast<uint64_t>(hmon);
    s.ordinal = static_cast<uint32_t>(list->size() + 1);
    s.deviceName = info.szDevice;
    s.bounds = FromNativeRect(info.rcMonitor);
    s.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    list->push_back(std::move(s));
    return TRUE;
}

// 显示配置那一路的问答结果，先摊平成这个文件内部的一张表，再按设备名并到候选上。
struct DisplayPathFacts {
    std::wstring sourceName;         // viewGdiDeviceName（带前缀那一套）
    ScreenQuestion monitorPathQ;
    std::wstring monitorPath;
    std::wstring friendlyName;
    std::wstring adapterLuid;
    ScreenQuestion adapterPathQ;
    std::wstring adapterPath;
    uint32_t targetId = 0;
    uint32_t outputTechnology = 0;
    bool targetAvailable = false;
    uint32_t edidManufactureId = 0;
    uint32_t edidProductCode = 0;
    bool edidIdsValid = false;
    uint32_t panelRotation = 0;
};

// user32 / GDI 那几条返回 Win32 错误码。ERROR_ACCESS_DENIED 与特权不足算 denied，
// 其它算 failed；两种都**不是**"这块屏不存在"。
ReadState StateOfError(DWORD code) {
    if (code == ERROR_SUCCESS) return ReadState::kReadable;
    if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD) return ReadState::kDenied;
    return ReadState::kFailed;
}

// shcore 那一条返回 HRESULT：完整性级别不够时它报的是 0x80070005，那与 win32 的 5 同一件事，
// 但码要照实交回，所以这里按 HRESULT 记、并只在 ACCESS_DENIED 那一种情况下写 denied。
ReadState StateOfHresult(HRESULT hr) {
    if (SUCCEEDED(hr)) return ReadState::kReadable;
    if (hr == E_ACCESSDENIED) return ReadState::kDenied;
    return ReadState::kFailed;
}

std::wstring LuidHex(const LUID& l) {
    wchar_t buf[32];
    swprintf(buf, 32, L"0x%08X%08X", static_cast<unsigned>(static_cast<DWORD>(l.HighPart)),
             static_cast<unsigned>(l.LowPart));
    return buf;
}

std::wstring OutputTechnologyName(uint32_t t) {
    switch (t) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return L"hd15";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO: return L"svideo";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO: return L"composite_video";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO: return L"component_video";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return L"dvi";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"hdmi";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS: return L"lvds";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_D_JPN: return L"d_jpn";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI: return L"sdi";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"displayport_external";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"displayport_embedded";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL: return L"udi_external";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED: return L"udi_embedded";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDTVDONGLE: return L"sdtvdongle";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return L"miracast";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return L"indirect_wired";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL: return L"indirect_virtual";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL:
            return L"displayport_usb_tunnel";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"internal";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER: return L"other";
        default: return L"unknown";   // 取值不在登记表中：不作断言，也不硬套一个相邻的取值
    }
}

// 显示配置那条 rotation 说的是"相对面板自己原生朝向要转多少"，与"用户看到的朝向"
// （DEVMODE 那一条）不是同一件事，所以两份各写各的，不互相换算也不互相冒充。
std::wstring PanelRotationName(uint32_t r) {
    switch (r) {
        case DISPLAYCONFIG_ROTATION_IDENTITY: return L"identity";
        case DISPLAYCONFIG_ROTATION_ROTATE90: return L"rotate90";
        case DISPLAYCONFIG_ROTATION_ROTATE180: return L"rotate180";
        case DISPLAYCONFIG_ROTATION_ROTATE270: return L"rotate270";
        default: return L"unknown";
    }
}

// 本次桌面上每条活动路径的显示配置事实。整次问答的下场由 *config 交回，因为
// "一条路径都没有"与"这一问根本没跑成"是两件事：前者是 no，后者是 unknown。
std::vector<DisplayPathFacts> ProbeDisplayPaths(ScreenQuestion* config) {
    std::vector<DisplayPathFacts> out;
    config->read = ReadState::kFailed;
    config->win32 = 0;
    config->hresult.clear();

    UINT32 pathCount = 0, modeCount = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (rc != ERROR_SUCCESS) {
        config->read = StateOfError(static_cast<DWORD>(rc));
        config->win32 = static_cast<DWORD>(rc);
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                            modes.data(), nullptr);
    if (rc != ERROR_SUCCESS) {
        config->read = StateOfError(static_cast<DWORD>(rc));
        config->win32 = static_cast<DWORD>(rc);
        return out;
    }
    config->read = ReadState::kReadable;
    paths.resize(pathCount);

    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DisplayPathFacts f;
        f.targetId = p.targetInfo.id;
        f.targetAvailable = p.targetInfo.targetAvailable != FALSE;
        f.outputTechnology = static_cast<uint32_t>(p.targetInfo.outputTechnology);
        f.panelRotation = static_cast<uint32_t>(p.targetInfo.rotation);
        f.adapterLuid = LuidHex(p.targetInfo.adapterId);

        // 这条路径接在哪个视图设备上（`\\.\DISPLAY1` 那一套）—— 与 EnumDisplayMonitors 对号的键
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        rc = DisplayConfigGetDeviceInfo(&src.header);
        if (rc != ERROR_SUCCESS) continue;   // 对不上号的这条不并进来，也不当成"这块屏没了"
        f.sourceName = src.viewGdiDeviceName;

        // 监视器自己的身份：devnode 路径 + EDID 报出来的那两个数 + 友好名
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgt{};
        tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgt.header.size = sizeof(tgt);
        tgt.header.adapterId = p.targetInfo.adapterId;
        tgt.header.id = p.targetInfo.id;
        rc = DisplayConfigGetDeviceInfo(&tgt.header);
        if (rc == ERROR_SUCCESS) {
            f.monitorPath = tgt.monitorDevicePath;
            f.monitorPathQ.read = ReadState::kReadable;
            f.friendlyName = tgt.monitorFriendlyDeviceName;
            f.edidManufactureId = tgt.edidManufactureId;
            f.edidProductCode = tgt.edidProductCodeId;
            f.edidIdsValid = tgt.flags.edidIdsValid != 0;
        } else {
            f.monitorPathQ.read = StateOfError(static_cast<DWORD>(rc));
            f.monitorPathQ.win32 = static_cast<DWORD>(rc);
        }

        // 适配器（显卡）自己的设备接口路径：跨会话的那一条。LUID 只在这次会话里唯一，
        // 所以这条问得到就写这条，问不到就只剩会话内的那一条（写清楚，不硬凑）。
        // 文档规定这一问的 header.id 写 0、header.adapterId 写**源**那条 LUID。
        DISPLAYCONFIG_ADAPTER_NAME adp{};
        adp.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADAPTER_NAME;
        adp.header.size = sizeof(adp);
        adp.header.adapterId = p.sourceInfo.adapterId;
        adp.header.id = 0;
        rc = DisplayConfigGetDeviceInfo(&adp.header);
        if (rc == ERROR_SUCCESS) {
            f.adapterPath = adp.adapterDevicePath;
            f.adapterPathQ.read = ReadState::kReadable;
        } else {
            // 失败原因（比如 ERROR_FILE_NOT_FOUND）照实记，不当成"这条路径不存在"。
            f.adapterPathQ.read = StateOfError(static_cast<DWORD>(rc));
            f.adapterPathQ.win32 = static_cast<DWORD>(rc);
        }
        out.push_back(std::move(f));
    }
    return out;
}

// shcore!GetDpiForMonitor 的取值（与 ShellScalingApi.h 里 MONITOR_DPI_TYPE 同源）。
// 这里不引那个头：它要 initguid 那一套，而本工具只要这一个函数与这两个数。
constexpr uint32_t kMonitorDpiEffective = 0;   // MDT_EFFECTIVE_DPI
constexpr uint32_t kMonitorDpiRaw = 2;         // MDT_RAW_DPI
using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, uint32_t, UINT*, UINT*);

// 动态取：那条 API 是 Win8.1 才有的，而本工具在更低版本上也要能出一份诚实的 unknown
// （与 src/SystemCompat.h「构建期能过的线不冒充运行期能力」同一条思路）。
GetDpiForMonitorFn ResolveGetDpiForMonitor(DWORD* why) {
    *why = 0;
    static const HMODULE module = [] {
        return LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    if (!module) {
        *why = LastError();
        return nullptr;
    }
    FARPROC fn = GetProcAddress(module, "GetDpiForMonitor");
    if (!fn) *why = LastError();
    return reinterpret_cast<GetDpiForMonitorFn>(fn);
}

void ProbeDpi(HMONITOR hmon, ScreenFacts* facts) {
    DWORD why = 0;
    const GetDpiForMonitorFn fn = ResolveGetDpiForMonitor(&why);
    if (!fn) {
        facts->dpi.read = ReadState::kFailed;
        facts->dpi.win32 = why;
        facts->dpiRaw = facts->dpi;
        return;
    }
    UINT x = 0, y = 0;
    const HRESULT hr = fn(hmon, kMonitorDpiEffective, &x, &y);
    facts->dpi.read = StateOfHresult(hr);
    if (SUCCEEDED(hr)) {
        facts->dpiEffectiveX = x;
        facts->dpiEffectiveY = y;
    } else {
        facts->dpi.hresult = HResultText(hr);
    }
    UINT rx = 0, ry = 0;
    const HRESULT rhr = fn(hmon, kMonitorDpiRaw, &rx, &ry);
    facts->dpiRaw.read = StateOfHresult(rhr);
    if (SUCCEEDED(rhr)) {
        facts->dpiRawX = rx;
        facts->dpiRawY = ry;
    } else {
        facts->dpiRaw.hresult = HResultText(rhr);
    }
}

// 用户看到的朝向。EnumDisplaySettingsW 要的就是 EnumDisplayMonitors 那套带前缀的设备名。
// 这一问只读当前模式（ChangeDisplaySettings 一条都不调）。
void ProbeRotation(const std::wstring& deviceName, ScreenFacts* facts) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm) == FALSE) {
        const DWORD gle = LastError();   // 必须在任何其它 API 之前取走
        facts->rotation.read = StateOfError(gle);
        facts->rotation.win32 = gle;
        return;
    }
    // dmDisplayOrientation：DMDO_DEFAULT / 90 / 180 / 270 = 0..3
    switch (dm.dmDisplayOrientation) {
        case DMDO_DEFAULT: facts->rotationDegrees = 0; break;
        case DMDO_90: facts->rotationDegrees = 90; break;
        case DMDO_180: facts->rotationDegrees = 180; break;
        case DMDO_270: facts->rotationDegrees = 270; break;
        default:
            // 取值不合登记表：不作任何断言（宁缺勿错），也不写成 0 度。
            facts->rotationDegrees = -1;
            facts->rotation.read = ReadState::kFailed;
            return;
    }
    facts->rotation.read = ReadState::kReadable;
}

}  // namespace

std::vector<ScreenInfo> EnumScreens() {
    std::vector<ScreenInfo> screens;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&screens));
    return screens;
}

std::vector<ScreenCandidate> EnumScreenCandidates(bool needFacts) {
    // 矩形与 DPI 问的都是物理像素，所以这一问之前必须已经声明过 per-monitor DPI v2，
    // 否则 GetMonitorInfo 交回来的是虚拟化过的坐标，与 images[].rect 那一份对不上号。
    // 放在这里而不是各调用点"记得检查一下"：这一层是屏幕问答唯一的入口。
    EnsureDpiAware();
    std::vector<ScreenCandidate> out;
    for (const ScreenInfo& s : EnumScreens()) {
        ScreenCandidate c;
        c.screen = s;
        out.push_back(std::move(c));
    }
    if (out.empty()) return out;

    for (ScreenCandidate& c : out) {
        // 这两问各自独立，也不依赖显示配置：整块屏的 DPI 与用户看到的朝向
        ProbeDpi(reinterpret_cast<HMONITOR>(c.screen.monitor), &c.facts);
        ProbeRotation(c.screen.deviceName, &c.facts);
    }
    if (!needFacts) return out;

    ScreenQuestion config;
    const std::vector<DisplayPathFacts> paths = ProbeDisplayPaths(&config);
    for (ScreenCandidate& c : out) {
        ScreenFacts& f = c.facts;
        f.hasFacts = true;
        f.config = config;
        // 同一个视图设备可能挂着多条路径（复制模式下两块面板共享一个桌面），
        // 那不是一个桌面里多了两块屏，所以这里计数而不重复并入。
        for (const DisplayPathFacts& p : paths) {
            if (!ScreenIdEquals(StripScreenDevicePrefix(p.sourceName),
                                StripScreenDevicePrefix(c.screen.deviceName))) {
                continue;
            }
            f.pathsMatched++;
            // 优先取"现在可用"的那一条；两条都在时取先枚举到的（同机同屏的结果不抖）
            const bool take = !f.hasPath || (p.targetAvailable && !f.targetAvailable);
            if (!take) continue;
            f.hasPath = true;
            f.monitorPath = p.monitorPath;
            f.monitorPathQ = p.monitorPathQ;
            f.friendlyName = p.friendlyName;
            f.adapterLuid = p.adapterLuid;
            f.adapterPath = p.adapterPath;
            f.adapterPathQ = p.adapterPathQ;
            f.targetId = p.targetId;
            f.outputTechnology = OutputTechnologyName(p.outputTechnology);
            f.targetAvailable = p.targetAvailable;
            f.edidManufactureId = p.edidManufactureId;
            f.edidProductCode = p.edidProductCode;
            f.edidIdsValid = p.edidIdsValid;
            f.panelRotation = PanelRotationName(p.panelRotation);
        }
    }
    return out;
}

std::vector<ScreenCandidate> SelectScreenCandidatesOf(const MonitorSelector& sel,
                                                      std::vector<Diagnostic>* errors) {
    std::vector<ScreenCandidate> out;
    const std::vector<ScreenCandidate> all = EnumScreenCandidates(MonitorSelectorNeedsIdentity(sel));
    const ScreenSelectResult r = SelectScreenCandidates(sel, all);
    if (r.outcome != ScreenSelect::kFound) {
        for (const Diagnostic& d : r.errors) errors->push_back(d);
        return out;
    }
    for (const size_t i : r.picked) out.push_back(all[i]);
    return out;
}

std::vector<ScreenInfo> SelectScreens(const Options& opt, std::vector<Diagnostic>* errors) {
    std::vector<ScreenInfo> out;
    for (const ScreenCandidate& c : SelectScreenCandidatesOf(opt.monitor, errors)) {
        out.push_back(c.screen);
    }
    return out;
}

std::vector<PhysRect> SelectedScreenRects(const Options& opt, std::vector<Diagnostic>* errors) {
    std::vector<PhysRect> rects;
    for (const auto& s : SelectScreens(opt, errors)) {
        if (!RectIsEmpty(s.bounds)) rects.push_back(s.bounds);
    }
    return rects;
}

}  // namespace ecapture
