// 屏幕身份的问答与判据。判据本体（SelectScreenCandidates / CompareScreenIdentity /
// MonitorSelectorLabel / ScreenIdEquals）是纯函数；只有 EnumScreenCandidates 与
// SelectScreenCandidatesOf 碰 Win32。这一整个文件不取一个像素、不弹确认框、不写文件，
// 也**绝不**改任何显示设置 —— 想"看清这块屏是不是转过的"就去调 SetDisplayConfig，
// 等于为了读答案把考卷改了。

#include "ScreenIdentity.h"

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"   // LastError() / HResultText()：失败点当场取码
#include "Lang.h"

namespace ecapture {
namespace {

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

// 一条选择器诊断：稳定的 code + 把人引向 --screens 的那句 hint + 候选一览。
// listed 给了就只列那几条并用它的条数，没给就列本机全部屏幕：多匹配那条要说的是
// "命中了这几块"，拿本机总数填进去会把它说成"本机有几块屏"。
Diagnostic SelectorError(const wchar_t* code, const wchar_t* messageKey, const wchar_t* hintKey,
                         const MonitorSelector& sel, const std::vector<ScreenCandidate>& all,
                         const std::vector<size_t>* listed) {
    Diagnostic d;
    d.code = code;
    d.message = Msg(messageKey);
    d.option = L"--monitor";
    d.value = sel.written.empty() ? MonitorSelectorLabel(sel) : sel.written;
    std::vector<ScreenInfo> infos;
    if (listed) {
        for (const size_t i : *listed) infos.push_back(all[i].screen);
    } else {
        infos.reserve(all.size());
        for (const ScreenCandidate& c : all) infos.push_back(c.screen);
    }
    d.hint = Msgf(hintKey, infos.size(), BriefScreenList(infos));
    d.stage = stages::kMatch;
    return d;
}

// 矩形/编号/主屏归属有没有变（判据与 ScreenMatch.h 的 CompareScreen 同源）。
bool SameGeometry(const ScreenInfo& a, const ScreenInfo& b) {
    return a.bounds.left == b.bounds.left && a.bounds.top == b.bounds.top &&
           a.bounds.right == b.bounds.right && a.bounds.bottom == b.bounds.bottom &&
           a.ordinal == b.ordinal && a.primary == b.primary;
}

bool PathReadable(const ScreenCandidate& c) {
    return c.facts.monitorPathQ.read == ReadState::kReadable && !c.facts.monitorPath.empty();
}

}  // namespace

bool ScreenIdEquals(const std::wstring& a, const std::wstring& b) {
    // 逐字符不区分大小写，与 Windows 自己看设备名的办法一致（区域敏感的比较规则会把
    // 土耳其语的 i 之类写成另一个名字，而"是不是同一个设备"不该由那种规则决定）。
    if (a.size() != b.size()) return false;
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool MonitorSelectorNeedsIdentity(const MonitorSelector& sel) {
    return sel.kind == MonitorSelector::Kind::kPath;
}

std::wstring MonitorSelectorLabel(const MonitorSelector& sel) {
    using Kind = MonitorSelector::Kind;
    switch (sel.kind) {
        case Kind::kAll: return L"all";
        case Kind::kOrdinal: return std::to_wstring(sel.ordinal);
        case Kind::kDevice: return L"device:" + sel.id;
        case Kind::kPath: return L"id:" + sel.id;
        case Kind::kPrimary: break;
    }
    return L"primary";
}

const wchar_t* MonitorSelectorKindName(const MonitorSelector& sel) {
    using Kind = MonitorSelector::Kind;
    switch (sel.kind) {
        case Kind::kAll: return L"all";
        case Kind::kOrdinal: return L"ordinal";
        case Kind::kDevice: return L"device";
        case Kind::kPath: return L"id";
        case Kind::kPrimary: break;
    }
    return L"primary";
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

ScreenSelectResult SelectScreenCandidates(const MonitorSelector& sel,
                                          const std::vector<ScreenCandidate>& all) {
    using Kind = MonitorSelector::Kind;
    ScreenSelectResult r;

    // 一块屏都没有：这与"编号写错了"同一类（调用方的下一步都是重新看一遍本机），
    // 也是旧行为 —— 不因这次加了标识写法就给它换一条码。
    if (all.empty()) {
        r.outcome = ScreenSelect::kOutOfRange;
        Diagnostic d;
        d.code = codes::kMonitorOutOfRange;
        d.message = Msg(L"match.monitor_out_of_range");
        d.option = L"--monitor";
        d.value = MonitorSelectorLabel(sel);
        d.hint = Msgf(L"match.monitor_out_of_range_hint", all.size(), std::wstring());
        d.stage = stages::kMatch;
        r.errors.push_back(std::move(d));
        return r;
    }

    switch (sel.kind) {
        case Kind::kAll:
            r.picked.resize(all.size());
            for (size_t i = 0; i < all.size(); ++i) r.picked[i] = i;
            r.outcome = ScreenSelect::kFound;
            return r;
        case Kind::kPrimary: {
            // 没有哪块被标成主屏（远程会话里见过）时照旧用第一块。这一条老行为不改：
            // 调用方说的是"主屏"，而本机对"哪块是主屏"这个问题的答复是"没有主屏可言"，
            // 在这台机器上"第一块"与"唯一那块"是同一件事（一块屏的会话里就是这么判的）。
            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].screen.primary) {
                    r.picked.push_back(i);
                    r.outcome = ScreenSelect::kFound;
                    return r;
                }
            }
            r.picked.push_back(0);
            r.outcome = ScreenSelect::kFound;
            return r;
        }
        case Kind::kOrdinal: {
            for (size_t i = 0; i < all.size(); ++i) {
                if (all[i].screen.ordinal == sel.ordinal) {
                    r.picked.push_back(i);
                    r.outcome = ScreenSelect::kFound;
                    return r;
                }
            }
            r.outcome = ScreenSelect::kOutOfRange;
            r.errors.push_back(SelectorError(codes::kMonitorOutOfRange,
                                             L"match.monitor_out_of_range",
                                             L"match.monitor_out_of_range_hint", sel, all, nullptr));
            return r;
        }
        case Kind::kDevice: {
            // 两边都先去前缀再比：解析层已经把用户写的那一条归一过一次，这里再判一次是幂等的
            // （同一个 inline 函数，形状只写一处），这样比对这一层收到的 id 带不带前缀都对得上,
            // 不必要求每一条调用路径都记得先剥。
            const std::wstring want = StripScreenDevicePrefix(sel.id);
            for (size_t i = 0; i < all.size(); ++i) {
                if (ScreenIdEquals(StripScreenDevicePrefix(all[i].screen.deviceName), want)) {
                    r.picked.push_back(i);
                }
            }
            break;
        }
        case Kind::kPath: {
            // 这一路只有显示配置答得上来。整张表里一条 devnode 都没有 = 这一问没答案，
            // 那不等于"没有这块屏"：报 unverifiable，让调用方去查显示拓扑，
            // 而不是拿"没找到"把人引向"那我改用编号试试"。
            bool anyAnswer = false;
            uint32_t why = 0;
            for (const ScreenCandidate& c : all) {
                if (PathReadable(c)) anyAnswer = true;
                if (why == 0 && c.facts.hasFacts) {
                    why = c.facts.config.win32 != 0 ? c.facts.config.win32
                                                    : c.facts.monitorPathQ.win32;
                }
            }
            if (!anyAnswer) {
                r.outcome = ScreenSelect::kUnverifiable;
                Diagnostic d = SelectorError(codes::kMonitorIdUnverifiable,
                                             L"match.monitor_id_unverifiable",
                                             L"match.monitor_id_unverifiable_hint", sel, all,
                                             nullptr);
                d.win32 = why;
                r.errors.push_back(std::move(d));
                return r;
            }
            for (size_t i = 0; i < all.size(); ++i) {
                if (PathReadable(all[i]) && ScreenIdEquals(all[i].facts.monitorPath, sel.id)) {
                    r.picked.push_back(i);
                }
            }
            break;
        }
    }

    if (r.picked.size() == 1) {
        r.outcome = ScreenSelect::kFound;
        return r;
    }
    if (r.picked.empty()) {
        r.outcome = ScreenSelect::kNoMatch;
        r.errors.push_back(SelectorError(codes::kMonitorUnknownId, L"match.monitor_unknown_id",
                                         L"match.monitor_unknown_id_hint", sel, all, nullptr));
        return r;
    }
    // 同一个标识命中多块屏：绝不替调用方挑一块（挑哪一块都没依据），把命中的那几块列全。
    r.outcome = ScreenSelect::kAmbiguous;
    r.errors.push_back(SelectorError(codes::kMonitorAmbiguousId, L"match.monitor_ambiguous_id",
                                     L"match.monitor_ambiguous_id_hint", sel, all, &r.picked));
    return r;
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

ScreenIdentityCheck CompareScreenIdentity(const ScreenCandidate& wanted,
                                          const std::vector<ScreenCandidate>& current,
                                          ScreenCandidate* fresh) {
    if (!PathReadable(wanted)) {
        // 当初就没问出跨会话标识：照旧按设备名核 —— 这一档不比旧实现更严，也不比它更松。
        std::vector<ScreenInfo> infos;
        infos.reserve(current.size());
        for (const ScreenCandidate& c : current) infos.push_back(c.screen);
        ScreenInfo freshInfo{};
        const ScreenCheck old = CompareScreen(wanted.screen, infos, &freshInfo);
        if (fresh && old != ScreenCheck::kGone) fresh->screen = freshInfo;
        return old == ScreenCheck::kSame   ? ScreenIdentityCheck::kSame
             : old == ScreenCheck::kMoved ? ScreenIdentityCheck::kMoved
                                          : ScreenIdentityCheck::kGone;
    }

    bool anyAnswer = false;
    for (const ScreenCandidate& c : current) {
        if (!PathReadable(c)) continue;
        anyAnswer = true;
        if (!ScreenIdEquals(c.facts.monitorPath, wanted.facts.monitorPath)) continue;
        if (fresh) *fresh = c;
        return SameGeometry(wanted.screen, c.screen) ? ScreenIdentityCheck::kSame
                                                    : ScreenIdentityCheck::kMoved;
    }
    // 这一次一个 devnode 都没问出来 —— 那不是"那块屏没了"，是"这一问没答案"。
    // 这时**不**退回按名字核：那个名字可能已经发给了另一块面板，照名字截就是截一块
    // 没人批准过的屏（与「身份变了不去找替代目标」那一条同源）。
    if (!anyAnswer) return ScreenIdentityCheck::kUnverifiable;
    return ScreenIdentityCheck::kGone;
}

SamplingRecheck RecheckScreenSampling(const ScreenCandidate& approvedTarget,
                                      const RECT& approvedArea,
                                      const std::vector<ScreenCandidate>& current,
                                      ScreenCandidate* fresh) {
    ScreenCandidate now{};
    switch (CompareScreenIdentity(approvedTarget, current, &now)) {
        case ScreenIdentityCheck::kUnverifiable: return SamplingRecheck::kUnverifiable;
        case ScreenIdentityCheck::kGone: return SamplingRecheck::kGone;
        // "还是那块屏，但形状/编号/主屏归属变了"落在采样这一问里就是确认之后的漂移：
        // 人在框上批准的是旧样子。不采、也不把旧授权追认到新布局上 —— 重新确认由
        // 调用方带着新的事实重来一遍，而不是在这里悄悄换一块矩形继续。
        case ScreenIdentityCheck::kMoved: return SamplingRecheck::kStale;
        case ScreenIdentityCheck::kSame: break;
    }
    // 身份说形状没变，还要与"刚批准的那一片"再对一次：批准区域是人看过的那份事实，
    // 采样矩形越出它以外就不是被批准的画面。零容差 —— 与 DesktopPermit::Covers 同一判据。
    const RECT& b = now.screen.bounds;
    const RECT& a = approvedArea;
    if (b.left < a.left || b.top < a.top || b.right > a.right || b.bottom > a.bottom) {
        return SamplingRecheck::kStale;
    }
    if (fresh) *fresh = now;
    return SamplingRecheck::kOk;
}

}  // namespace ecapture
