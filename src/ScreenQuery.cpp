// 只读屏幕枚举（--screens）的判据与渲染。这里没有一条 Win32 问答：
// 候选表由调用方交进来（真机是 ScreenIdentity.h 的 EnumScreenCandidates，离线判据注入假表），
// 所以"一块屏都没有""devnode 问不到""同一视图设备挂着两条路径""副屏在负坐标"
// 这些现场都能逐条断言，不必真去插显示器。

#include "ScreenQuery.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Json.h"
#include "Lang.h"
#include "Version.h"

namespace ecapture {
namespace {

// 空的那一个整个键不出现（与诊断项那条规矩一致：不输出 null 占位）。
void OptStr(Json& j, const wchar_t* key, const std::wstring& value) {
    if (value.empty()) return;
    j.Key(key).Value(value);
}

void WriteRect(Json& j, const wchar_t* key, const RECT& r) {
    j.Key(key).Obj();
    j.Key(L"x").Value(static_cast<long long>(r.left));
    j.Key(L"y").Value(static_cast<long long>(r.top));
    j.Key(L"width").Value(static_cast<long long>(r.right - r.left));
    j.Key(L"height").Value(static_cast<long long>(r.bottom - r.top));
    j.End();
}

// 一问的下场写成一个对象：state 恒在（readable / denied / failed），
// 错误码只在真的失败过之后才出现。调用方由此能把「读不到」与「值是 0」分开判。
void WriteQuestion(Json& j, const wchar_t* key, const ScreenQuestion& q) {
    j.Key(key).Obj();
    j.Key(L"state").Value(ReadStateName(q.read));
    if (q.win32 != 0) j.Key(L"win32").Value(static_cast<long long>(q.win32));
    OptStr(j, L"hresult", q.hresult);
    j.End();
}

// 一种身份自述一句：它是什么、稳到哪一层、有没有选择器写法。
// 这一段是本功能最容易被读错的地方，所以写成字段而不是正文：
// "设备名"与"devnode 路径"看着都是字符串，可一个会被系统重新发出去、一个不会。
void WriteIdentityKind(Json& j, const wchar_t* key, const wchar_t* kind, const wchar_t* stable,
                       bool usableAsSelector, const wchar_t* selectorForm) {
    j.Key(key).Obj();
    j.Key(L"kind").Value(kind);
    j.Key(L"stableAcross").Value(stable);
    j.Key(L"usableAsSelector").Value(usableAsSelector);
    // 没有选择器写法的那两条传的是空指针（不是空串）：这里判掉，别让 std::wstring 去接 nullptr。
    if (selectorForm) j.Key(L"selectorForm").Value(selectorForm);
    j.End();
}

void DiagArray(Json& j, const wchar_t* key, const std::vector<Diagnostic>& items) {
    j.Key(key).Arr();
    for (const auto& d : items) {
        j.Obj();
        j.Key(L"code").Value(d.code);
        OptStr(j, L"message", d.message);
        OptStr(j, L"option", d.option);
        OptStr(j, L"value", d.value);
        OptStr(j, L"hint", d.hint);
        OptStr(j, L"target", d.target);
        OptStr(j, L"backend", d.backend);
        OptStr(j, L"stage", d.stage);
        OptStr(j, L"hresult", d.hresult);
        if (d.win32 != 0) j.Key(L"win32").Value(static_cast<long long>(d.win32));
        j.End();
    }
    j.End();
}

void StrArray(Json& j, const wchar_t* key, const std::vector<std::wstring>& items) {
    j.Key(key).Arr();
    for (const auto& s : items) j.Value(s);
    j.End();
}

// 一块屏的一条记录。字段的取舍规则只有一条：读得到的才写值，读不到的只写那一问的下场。
// 所以"这个键不见了"永远有两种可能，而 readability 那一段说得出是哪一种。
void WriteScreen(Json& j, const ScreenCandidate& c) {
    const ScreenFacts& f = c.facts;
    j.Obj();
    j.Key(L"ordinal").Value(static_cast<long long>(c.screen.ordinal));
    j.Key(L"deviceName").Value(c.screen.deviceName);
    j.Key(L"displayName").Value(ScreenDisplayName(c.screen));
    j.Key(L"primary").Value(c.screen.primary);
    WriteRect(j, L"rect", c.screen.bounds);

    // 能直接抄回 --monitor 的那两条。id 那一条只在问得到 devnode 时出现 ——
    // 问不到就交回一条"没有这条标识"，而不是交回一条空的 id: 让调用方去试。
    j.Key(L"selectors").Obj();
    j.Key(L"ordinal").Value(std::to_wstring(c.screen.ordinal));
    j.Key(L"device").Value(L"device:" + ScreenDisplayName(c.screen));
    if (f.monitorPathQ.read == ReadState::kReadable && !f.monitorPath.empty()) {
        j.Key(L"id").Value(L"id:" + f.monitorPath);
    }
    j.End();

    // 这两问与显示配置无关，各自独立：整块屏的 DPI 与用户看到的朝向
    j.Key(L"dpi").Obj();
    if (f.dpi.read == ReadState::kReadable) {
        j.Key(L"effectiveX").Value(static_cast<long long>(f.dpiEffectiveX));
        j.Key(L"effectiveY").Value(static_cast<long long>(f.dpiEffectiveY));
    }
    if (f.dpiRaw.read == ReadState::kReadable) {
        j.Key(L"rawX").Value(static_cast<long long>(f.dpiRawX));
        j.Key(L"rawY").Value(static_cast<long long>(f.dpiRawY));
    }
    j.End();
    j.Key(L"rotation").Obj();
    if (f.rotation.read == ReadState::kReadable && f.rotationDegrees >= 0) {
        // 人在屏幕上看到的朝向
        j.Key(L"degrees").Value(static_cast<long long>(f.rotationDegrees));
    }
    // 显示配置那条说的是"相对面板自己原生朝向要转多少"，与上面那条不是同一件事
    OptStr(j, L"panel", f.panelRotation);
    j.End();

    if (f.hasFacts) {
        // 这一块屏在显示配置里对上了几条活动路径。0 条 = 这一问没答案；
        // 多于 1 条 = 复制模式下几块面板共享同一个桌面，那还是一个桌面里的一块屏。
        j.Key(L"displayConfigPaths").Value(static_cast<long long>(f.pathsMatched));
        j.Key(L"adapter").Obj();
        OptStr(j, L"luid", f.adapterLuid);
        if (f.hasPath) {
            j.Key(L"targetId").Value(static_cast<long long>(f.targetId));
            j.Key(L"targetAvailable").Value(f.targetAvailable);
        }
        OptStr(j, L"outputTechnology", f.outputTechnology);
        // 跨会话的那一条：适配器自己的设备接口路径。LUID 只在这次会话里唯一，
        // 所以这两条写在同一层而各说各的话。
        OptStr(j, L"devicePath", f.adapterPath);
        j.End();
        if (f.hasPath) {
            j.Key(L"monitor").Obj();
            OptStr(j, L"friendlyName", f.friendlyName);
            if (f.edidIdsValid) {
                wchar_t buf[16];
                swprintf(buf, 16, L"0x%04X", f.edidManufactureId);
                j.Key(L"edid").Obj();
                j.Key(L"manufactureId").Value(std::wstring(buf));
                j.Key(L"productCode").Value(static_cast<long long>(f.edidProductCode));
                j.End();
            }
            j.End();
        }
    }

    // 字段级可读性：上面少掉的那些键在这里说得出为什么少。
    j.Key(L"readability").Obj();
    if (f.dpi.read != ReadState::kReadable) WriteQuestion(j, L"dpi", f.dpi);
    if (f.dpiRaw.read != ReadState::kReadable) WriteQuestion(j, L"dpiRaw", f.dpiRaw);
    if (f.rotation.read != ReadState::kReadable) WriteQuestion(j, L"rotation", f.rotation);
    if (f.hasFacts) {
        WriteQuestion(j, L"monitorDevicePath", f.monitorPathQ);
        WriteQuestion(j, L"adapterDevicePath", f.adapterPathQ);
    }
    j.End();
    j.End();
}

}  // namespace

ScreenQueryResult BuildScreenQuery(const std::vector<ScreenCandidate>& all) {
    ScreenQueryResult r;
    r.screens = all;
    if (!all.empty()) {
        r.config = all.front().facts.config;
    } else {
        // 一块屏都没有：这一问本身还是跑了的（EnumDisplayMonitors 成功返回空表），
        // 所以这里是 no 而不是 unknown —— 差别写在 caveats 里。
        r.config.read = ReadState::kReadable;
    }

    // 虚拟屏幕的并集。副屏可以在主屏左边或上边，那一段坐标是负的，
    // 所以这里全程在带符号 64 位里算，宽度用无符号差值（右边界一定不小于左边界）。
    bool first = true;
    int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (const ScreenCandidate& c : all) {
        const RECT& b = c.screen.bounds;
        if (b.right <= b.left || b.bottom <= b.top) continue;   // 零尺寸的条目不参与并集
        if (first) {
            minX = b.left;
            minY = b.top;
            maxX = b.right;
            maxY = b.bottom;
            first = false;
            continue;
        }
        minX = std::min<int64_t>(minX, b.left);
        minY = std::min<int64_t>(minY, b.top);
        maxX = std::max<int64_t>(maxX, b.right);
        maxY = std::max<int64_t>(maxY, b.bottom);
    }
    if (!first) {
        r.virtualX = static_cast<int32_t>(minX);
        r.virtualY = static_cast<int32_t>(minY);
        r.virtualWidth = static_cast<uint32_t>(maxX - minX);
        r.virtualHeight = static_cast<uint32_t>(maxY - minY);
        r.virtualKnown = true;
    }

    for (const ScreenCandidate& c : all) {
        if (c.facts.hasFacts && !c.facts.hasPath) r.unmatchedPaths++;
        if (c.facts.pathsMatched > 1) r.clonedScreens++;
    }

    // 每一次成功的屏幕枚举都配一条快照过期提示：这一份列表不是凭证，
    // 真去截图时那一路仍要在取帧之前按身份再核一次（ScreenIdentity.h）。
    // --quiet 能抑制这一条，但 caveats 里同源的 token 恒在，抑制不掉的才是判据。
    Diagnostic stale;
    stale.code = codes::kScreenQueryStale;
    stale.message = Msg(L"note.screen_query_stale");
    stale.hint = Msgf(L"note.screen_query_stale_hint", all.size());
    stale.stage = stages::kMatch;
    r.notes.push_back(std::move(stale));
    return r;
}

ScreenQueryResult RunScreenQuery() {
    // 这份文档要回答的就是"每块屏的哪几种身份问得到"，所以显示配置那一路必须问。
    return BuildScreenQuery(EnumScreenCandidates(/*needFacts=*/true));
}

std::wstring RenderScreenQuery(const ScreenQueryResult& r, bool verbose, bool quiet) {
    Json j;
    j.Obj();
    // 与环境查询 / 窗口查询同级的**另一份契约**：带 contract 与 contractVersion，
    // 也**不**反推截图那份精简 JSON 去加顶层元信息。
    j.Key(L"contract").Value(kScreenQueryContractName);
    j.Key(L"contractVersion").Value(static_cast<long long>(kScreenQueryContractVersion));
    j.Key(L"query").Value(L"screens");
    j.Key(L"program").Obj().Key(L"version").Value(kVersion).End();

    j.Key(L"authorization").Obj();
    j.Key(L"readOnly").Value(true);
    j.Key(L"pixelsRead").Value(0);
    j.Key(L"consentDialogShown").Value(false);
    j.Key(L"filesWritten").Value(false);
    // 为了"看清这块屏转了多少度"去调 SetDisplayConfig 等于把考卷改了再答题，所以一条都不调。
    j.Key(L"displaySettingsChanged").Value(false);
    // --yes 在这条路上没有任何作用，也不改变"哪些字段读得到"。
    j.Key(L"yesAffectsResult").Value(false);
    // 这份列表不是免确认的凭证：真去截整屏仍然一定弹框，而且 --yes 对桌面像素不生效。
    j.Key(L"identifiersAreNotConsent").Value(true);
    j.Key(L"screenCaptureConsent").Obj();
    j.Key(L"desktopPixelsAlwaysAsk").Value(true);
    j.Key(L"yesSkipsThisLevel").Value(false);
    j.End();
    j.End();

    // 四种身份各稳在哪一层，写成字段而不是正文。选择器只有两条：设备名与 devnode 路径。
    // 编号与 LUID 没有选择器写法是有意的 —— 前者的作用域是"本次枚举"，
    // 后者是"本次会话"，都能对上号却都不能拿去点名一块屏（点名用它们 = 赌）。
    j.Key(L"identity").Obj();
    WriteIdentityKind(j, L"ordinal", L"enum_display_monitors_position",
                      screen_stability::kThisInvocation, true, L"--monitor <n>");
    WriteIdentityKind(j, L"deviceName", L"gdi_view_device_name", screen_stability::kThisAttach,
                      true, L"device:");
    WriteIdentityKind(j, L"monitorDevicePath", L"monitor_devnode_path",
                      screen_stability::kCrossSession, true, L"id:");
    WriteIdentityKind(j, L"adapterLuid", L"adapter_locally_unique_id",
                      screen_stability::kThisSession, false, nullptr);
    WriteIdentityKind(j, L"adapterDevicePath", L"adapter_devnode_path",
                      screen_stability::kCrossSession, false, nullptr);
    // 跨会话那两条是"设备节点决定的"这一层道理，不是本项目实测过的结论。
    j.Key(L"crossSessionClaimTested").Value(false);
    j.End();

    j.Key(L"topology").Obj();
    j.Key(L"monitors").Value(static_cast<long long>(r.screens.size()));
    WriteQuestion(j, L"displayConfig", r.config);
    if (r.virtualKnown) {
        j.Key(L"virtualScreen").Obj();
        j.Key(L"x").Value(static_cast<long long>(r.virtualX));
        j.Key(L"y").Value(static_cast<long long>(r.virtualY));
        j.Key(L"width").Value(static_cast<long long>(r.virtualWidth));
        j.Key(L"height").Value(static_cast<long long>(r.virtualHeight));
        j.End();
    } else {
        j.Key(L"virtualScreenKnown").Value(false);
    }
    if (r.unmatchedPaths != 0) {
        // 对不上活动路径的有几块。这一条不作"那几块屏不存在"的断言，
        // 也不作"它们截不到"的断言 —— 它只说这一问没给出对应的路径。
        j.Key(L"screensWithoutDisplayPath").Value(static_cast<long long>(r.unmatchedPaths));
    }
    if (r.clonedScreens != 0) {
        j.Key(L"clonedScreens").Value(static_cast<long long>(r.clonedScreens));
    }
    j.End();

    j.Key(L"screens").Arr();
    for (const ScreenCandidate& c : r.screens) WriteScreen(j, c);
    j.End();

    if (!r.notes.empty() && !quiet) DiagArray(j, L"notes", r.notes);

    std::vector<std::wstring> caveats;
    caveats.push_back(screen_caveat::kNoCapture);
    caveats.push_back(screen_caveat::kNoDialog);
    caveats.push_back(screen_caveat::kNoFiles);
    caveats.push_back(screen_caveat::kNoSettingsChange);
    caveats.push_back(screen_caveat::kSnapshotExpires);
    caveats.push_back(screen_caveat::kNotAToken);
    caveats.push_back(screen_caveat::kDeviceNamesReassigned);
    caveats.push_back(screen_caveat::kCrossSessionUntested);
    caveats.push_back(screen_caveat::kScreenShotAlwaysAsks);
    caveats.push_back(screen_caveat::kNamesHardware);
    if (r.config.read != ReadState::kReadable) {
        // 显示配置那一路整体没能回答：这一份里的身份字段因此全是 unknown，
        // 而"问不出来"绝不折叠成"这台机器上没有这样的屏"。
        caveats.push_back(screen_caveat::kConfigUnverifiable);
    }
    StrArray(j, L"caveats", caveats);

    // 这份文档带着设备接口路径（那是硬件身份），但不带任何文件系统路径，也不带用户名。
    // 与 --capabilities 那份 privacy 的区别只有这一条，所以写清楚，别让调用方去猜。
    j.Key(L"privacy").Obj();
    j.Key(L"capturesScreen").Value(false);
    j.Key(L"showsDialog").Value(false);
    j.Key(L"writesFiles").Value(false);
    j.Key(L"uploads").Value(false);
    j.Key(L"changesDisplaySettings").Value(false);
    j.Key(L"includesUsernames").Value(false);
    j.Key(L"includesFileSystemPaths").Value(false);
    j.Key(L"includesDevicePaths").Value(true);
    j.End();

    j.Key(L"limits").Obj();
    // --monitor 的编号上限与解析层、与 --capabilities 那份 limits 同源（数字只写一次）。
    j.Key(L"maxOrdinal").Value(static_cast<long long>(cli_limits::kMaxOrdinal));
    j.End();

    if (verbose) {
        j.Key(L"input").Obj();
        j.Key(L"lang").Value(LanguageTag(CurrentLanguage()));
        j.Key(L"query").Value(L"screens");
        j.Key(L"screens").Value(static_cast<long long>(r.screens.size()));
        j.Key(L"displayConfig").Value(ReadStateName(r.config.read));
        j.End();
    }
    j.End();
    return j.Str();
}

}  // namespace ecapture
