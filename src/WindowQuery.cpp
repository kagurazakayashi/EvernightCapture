#include "WindowQuery.h"

#include <algorithm>
#include <cstdio>
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

// 与截图那一份 images[].hwnd / errors[].target 同形的句柄写法（同一个格式，不许两处各写一套）。
std::wstring HwndHexOf(uint64_t hwnd) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%08X", static_cast<unsigned>(hwnd));
    return buf;
}

// 只在这一次真的取到了值时才写这一行：诊断项的形状与截图那一份一致（空字段整个键省略）。
void OptStr(Json& j, const wchar_t* key, const std::wstring& value) {
    if (value.empty()) return;
    j.Key(key).Value(value);
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

// 一条问句的下场写成一个对象：state 恒在（readable / denied / failed），
// win32 只在真的失败过之后才出现。调用方由此能把「读不到」与「值是空的」分开判。
void WriteFieldState(Json& j, const wchar_t* key, const std::wstring& state, uint32_t win32) {
    j.Key(key).Obj();
    j.Key(L"state").Value(state);
    if (win32 != 0) j.Key(L"win32").Value(static_cast<long long>(win32));
    j.End();
}

// 身份约束字段那一小段：--list 与 --inspect 共用同一个形状，不各写一份。
void WriteIdentity(Json& j, const WindowIdentity& id) {
    std::wstring hwndHex;
    std::wstring cls;
    uint32_t pid = 0;
    uint64_t start = 0;
    bool needsRecheck = false;
    WindowIdentityOf(id, &hwndHex, &cls, &pid, &start, &needsRecheck);
    j.Obj();
    j.Key(L"hwnd").Value(hwndHex);
    j.Key(L"pid").Value(static_cast<long long>(pid));
    OptStr(j, L"class", cls);
    // 问不出来的那一条写 unknown：截图时的复核会整个跳过基线没有的判据，
    // 但那是一次**没做出来**的判定，不是「它等于 0」。
    if (start != 0) j.Key(L"processStartTicks").Value(static_cast<long long>(start));
    else j.Key(L"processStartTicks").Value(L"unknown");
    j.Key(L"selectionNeedsRecheck").Value(needsRecheck);
    // 这三句是这份文档最容易被读错的地方，所以写在字段旁边而不是正文里：
    // 复核照旧要做、这不是许可、也不保证竞态窗口为零。
    j.Key(L"verificationRequired").Value(true);
    j.Key(L"isAuthorizationToken").Value(false);
    j.Key(L"raceWindowReducedNotEliminated").Value(true);
    j.End();
}

void WriteWindow(Json& j, const WindowRecord& r) {
    j.Obj();
    j.Key(L"hwnd").Value(r.hwndHex);
    j.Key(L"pid").Value(static_cast<long long>(r.pid));
    // 标题与类名原样交付：调用方（含 AI）按字段读，不该再解析一段拼出来的描述字符串。
    j.Key(L"title").Value(r.title);          // 空标题就是空串，那是真值而不是「没读到」
    OptStr(j, L"class", r.windowClass);
    OptStr(j, L"image", r.imageName);
    if (r.exePathIncluded) j.Key(L"exePath").Value(r.imagePath);
    j.Key(L"rect").Obj();
    j.Key(L"x").Value(static_cast<long long>(r.x));
    j.Key(L"y").Value(static_cast<long long>(r.y));
    j.Key(L"width").Value(static_cast<long long>(r.width));
    j.Key(L"height").Value(static_cast<long long>(r.height));
    j.End();
    j.Key(L"visible").Value(r.visible);
    j.Key(L"minimized").Value(r.iconic);
    j.Key(L"zOrder").Value(static_cast<long long>(r.zOrder));
    // 字段级可读性：读不到的那几项在上面写的是哨兵值，靠这一段说明「那不是值，是没问出来」。
    j.Key(L"readability").Obj();
    j.Key(L"process").Obj().Key(L"state").Value(r.processRead).End();
    WriteFieldState(j, L"imagePath", r.imagePathRead, r.imagePathWin32);
    WriteFieldState(j, L"processStart", r.startRead, r.startWin32);
    WriteFieldState(j, L"rect", r.rectRead, r.rectWin32);
    j.End();
    if (r.exePathRequested) {
        // 想写完整路径而这一问没答案：两个字段一起说清，而不是安静地少一个键。
        j.Key(L"exePathRequested").Value(true);
        j.Key(L"exePathReadable").Value(r.exePathReadable);
    }
    j.Key(L"identity");
    WriteIdentity(j, r.identity);
    j.End();
}

}  // namespace

int WindowQueryExitCodeFor(const std::wstring& code) {
    if (code == codes::kAmbiguousWindow) return EX_AMBIGUOUS;
    if (code == codes::kIndexOutOfRange || code == codes::kMonitorOutOfRange ||
        code == codes::kInvalidRegex || code == codes::kInvalidNumber) {
        return EX_USAGE;
    }
    if (code == codes::kMatchTimeout || code == codes::kCaptureTimeout ||
        code == codes::kWorkerFailed || code == codes::kCaptureFailed) {
        return EX_CAPTURE_FAILED;
    }
    return EX_NO_MATCH;   // match.no_window（以及任何没登记过的码，按「没对上目标」处理）
}

const wchar_t* ReadStateName(ReadState state) {
    switch (state) {
        case ReadState::kReadable: return L"readable";
        case ReadState::kDenied: return L"denied";
        case ReadState::kFailed: return L"failed";
    }
    return L"failed";   // 没登记过的取值不当成「能读」：宁可说问不出来
}

void WindowIdentityOf(const WindowIdentity& id, std::wstring* hwndHex, std::wstring* className,
                      uint32_t* pid, uint64_t* processStartTicks, bool* selectionNeedsRecheck) {
    *hwndHex = HwndHexOf(id.hwnd);
    *className = id.className;
    *pid = id.pid;
    *processStartTicks = id.processStartTicks;
    *selectionNeedsRecheck = id.selectionNeedsRecheck;
}

WindowQuerySpec MakeWindowQuerySpec(const Options& opt) {
    WindowQuerySpec spec;
    spec.offset = opt.offset;
    spec.limit = opt.limit;
    spec.includeIconic = opt.listIconic;
    spec.exePath = opt.inspectPath;
    spec.action = opt.windowAction;
    spec.inspectPolicy = opt.multi;
    spec.inspectIndex = opt.index;
    spec.timeoutMs = opt.timeoutMs;
    return spec;
}

WindowRecord MakeWindowRecord(const WindowInfo& w, const WindowQuerySpec& spec) {
    WindowRecord r;
    r.hwndHex = HwndHexOf(w.hwnd);
    r.title = w.title;
    r.windowClass = w.className;
    r.pid = w.pid;
    r.x = w.x;
    r.y = w.y;
    r.width = w.width;
    r.height = w.height;
    r.iconic = w.iconic;
    r.visible = !w.iconic;
    r.zOrder = w.zOrder;
    r.imageName = w.imageName;
    // 归属进程这一层的综合下场，取两条问句里更糟的那一个：被挡下优先于「问过而失败」，
    // 因为前者告诉调用方「换个权限的调用方读得到」，后者不告诉任何人这件事。
    if (w.pathRead == ReadState::kDenied || w.startRead == ReadState::kDenied) {
        r.processRead = ReadStateName(ReadState::kDenied);
    } else if (w.pathRead != ReadState::kReadable || w.startRead != ReadState::kReadable) {
        r.processRead = ReadStateName(ReadState::kFailed);
    } else {
        r.processRead = ReadStateName(ReadState::kReadable);
    }
    r.imagePathRead = ReadStateName(w.pathRead);
    r.startRead = ReadStateName(w.startRead);
    r.rectRead = ReadStateName(w.rectRead);
    r.imagePathWin32 = w.pathRead == ReadState::kReadable ? 0 : w.pathWin32;
    r.startWin32 = w.startRead == ReadState::kReadable ? 0 : w.startWin32;
    r.rectWin32 = w.rectRead == ReadState::kReadable ? 0 : w.rectWin32;
    // 路径的「能不能读」与「写不写」是两件事：--inspect=path 给了而这一问没答案时，报告里要看得见
    // 「想写而没写成」（exePathRequested 为真而 exePathIncluded 为假），而不是安静地少一个键。
    r.exePathRequested = spec.exePath;
    r.exePathReadable = w.pathRead == ReadState::kReadable && !w.imagePath.empty();
    r.exePathIncluded = spec.exePath && r.exePathReadable;
    if (r.exePathIncluded) r.imagePath = w.imagePath;
    return r;
}

WindowQueryResult BuildWindowQueryResult(const WindowQuerySnapshot& snapshot,
                                         const WindowQuerySpec& spec, const MatchOptions& match,
                                         const std::wstring& monitorLabel) {
    WindowQueryResult out;
    // 生效的取舍随结果一起交出去（渲染层的 input 段读的就是这一份，不再从别处现推）。
    out.spec = spec;
    out.spec.monitorLabel = monitorLabel;
    // -v 的 input 段回显的是**这一次实际参与求值的那一份条件**：在算出结果的原地填，
    // 不在渲染处从 Options 再抄一遍，否则「回显的条件」与「命中的条件」会有两份来源。
    for (uint64_t h : match.hwnds) out.spec.echoHwnds.push_back(HwndHexOf(h));
    for (uint32_t p : match.pids) out.spec.echoPids.push_back(std::to_wstring(p));
    out.spec.echoProcesses = match.processes;
    out.spec.echoExePaths = match.exePaths;
    out.spec.echoTitles = match.titles;
    out.spec.echoTitleContains = match.titleContains;
    out.spec.echoTitleRegexes = match.titleRegexes;
    out.spec.echoClasses = match.classes;

    // ---- --inspect：唯一目标，判据完全复用截图那一条选择策略 ----
    // 不在这里另写一份「取第一个」的规则：SelectFromHits 就是那次截图会用的同一份，
    // 所以 inspect 说歧义与截图说歧义不可能打脸。命中 0 / 越界 / 歧义三条诊断原样交回。
    if (spec.action == WindowAction::kInspect) {
        Options pick;
        pick.match = match;
        pick.monitor.given = !monitorLabel.empty();
        pick.multi = spec.inspectPolicy;
        pick.index = spec.inspectIndex;
        std::vector<Diagnostic> errors;
        const std::vector<WindowInfo> chosen =
            SelectFromHits(pick, snapshot.hits, snapshot.iconic, monitorLabel, &errors);
        out.matched = snapshot.hits.size();
        out.iconicExcluded = snapshot.iconic.size();
        // 交回一份快照就是一个名额；即使这一扇没定下来，也不让报告里出现"允许 0 个"。
        out.limit = 1;
        out.errors = std::move(errors);
        if (!out.errors.empty()) {
            // 这三条诊断由选择策略给出，它只填 code / option / value / hint；stage 由这里补
            //（同一次截图流水线里也是这一层才知道错在哪一步）。target 整个不出现：
            // 无匹配与歧义都归不到某一个具体窗口上，凑一个「第一个候选」反而是假定位。
            for (auto& d : out.errors) {
                if (d.stage.empty()) d.stage = stages::kMatch;
            }
            out.exitCode = WindowQueryExitCodeFor(out.errors.front().code);
            return out;
        }
        // --all 在解析期就算与 --inspect 互斥，所以这里正常只会剩一条。真的多于一条时不猜：
        // 报歧义，绝不随便选一个交给调用方（他据这一份快照去截图，截到的可能是另一扇）。
        if (chosen.size() != 1) {
            Diagnostic d;
            d.code = codes::kAmbiguousWindow;
            d.value = std::to_wstring(chosen.size());
            d.stage = stages::kMatch;
            d.message = Msg(L"match.ambiguous");
            d.hint = Msg(L"match.inspect_ambiguous_hint");
            out.errors.push_back(std::move(d));
            out.exitCode = EX_AMBIGUOUS;
            return out;
        }
        out.offset = 0;
        out.limit = 1;
        out.returned = 1;
        out.truncated = false;
        out.hasTarget = true;
        out.targetHwnd = chosen.front().hwnd;
        WindowRecord r = MakeWindowRecord(chosen.front(), spec);
        // 身份快照与截图那一次用的是同一份构造函数：条件里含标题或按屏过滤时，
        // 复核要靠「重跑当初那份条件」，这一件事也一并写进结果，调用方看得见。
        r.identity = MakeWindowIdentity(chosen.front(), match, !monitorLabel.empty());
        out.windows.push_back(std::move(r));
        return out;
    }

    // ---- --list：多匹配不是截图歧义，而是一次正常的列表 ----
    // 默认只交可见窗口（与截图链路的枚举同一条策略）；--list=all 时把最小化那些
    // 按当下 Z 序并进同一份列表。被挡掉的条数照样报告（iconicExcluded），
    // 免得「列表里没有」被读成「没有这样的窗口」。
    std::vector<const WindowInfo*> ordered;
    ordered.reserve(snapshot.hits.size() + snapshot.iconic.size());
    for (const auto& w : snapshot.hits) ordered.push_back(&w);
    if (spec.includeIconic) {
        for (const auto& w : snapshot.iconic) ordered.push_back(&w);
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const WindowInfo* a, const WindowInfo* b) {
                             return a->zOrder < b->zOrder;
                         });
    }
    out.matched = ordered.size();
    out.iconicExcluded = spec.includeIconic ? 0 : snapshot.iconic.size();

    out.offset = spec.offset;
    out.limitDefaulted = (spec.limit == 0);
    out.limit = out.limitDefaulted ? cli_limits::kDefaultWindowListLimit : spec.limit;

    const size_t from = std::min<size_t>(static_cast<size_t>(out.offset), out.matched);
    const size_t to = std::min<size_t>(from + static_cast<size_t>(out.limit), out.matched);
    out.returned = to - from;
    // truncated 判的是「窗口那边还有没有下一条」，不是「本批是否恰好装满」：
    // 停在末尾时哪怕本批不满也不算截断，调用方据此知道不用再翻页。
    out.truncated = to < out.matched;
    for (size_t i = from; i < to; ++i) {
        WindowRecord r = MakeWindowRecord(*ordered[i], spec);
        r.identity = MakeWindowIdentity(*ordered[i], match, !monitorLabel.empty());
        out.windows.push_back(std::move(r));
    }
    return out;
}

std::wstring RenderWindowQuery(const WindowQueryResult& r, const std::wstring& contract,
                               bool verbose, bool quiet) {
    const WindowQuerySpec& spec = r.spec;
    Json j;
    j.Obj();
    // 与 --capabilities / --diagnostics 同级的另一份契约：这两个键**不**加到截图那份精简 JSON 上，
    // 反过来截图那份的 captured / images / notes 规则也与这里无关。
    j.Key(L"contract").Value(contract);
    j.Key(L"contractVersion").Value(static_cast<long long>(kWindowQueryContractVersion));
    j.Key(L"query").Value(spec.action == WindowAction::kInspect ? L"inspect" : L"list");

    // 程序版本：这份文档要与「哪个版本的实现在这台机器上问出来的」对上号，才值得存档比对。
    j.Key(L"program").Obj().Key(L"version").Value(kVersion).End();

    // 只读自述：这几条是真的这么实现的，写出来让调用方不必读源码就能核对。
    j.Key(L"authorization").Obj();
    j.Key(L"readOnly").Value(true);
    j.Key(L"pixelsRead").Value(0);
    j.Key(L"consentDialogShown").Value(false);
    j.Key(L"filesWritten").Value(false);
    // --yes 在这条路上没有任何作用，也不改变「哪些字段读得到」（那由这一问的下场决定）。
    j.Key(L"yesAffectsResult").Value(false);
    // 拿这份快照当免确认的凭证：不行。截图那一级照旧按像素来源判，桌面像素一定问人。
    j.Key(L"identityFieldsAreNotConsent").Value(true);
    j.End();

    // 可见性策略：说清「列表里没有」是什么意思，免得被读成「这台机器上没有这样的窗口」。
    j.Key(L"policy").Obj();
    j.Key(L"invisibleExcluded").Value(true);
    j.Key(L"zeroSizedExcluded").Value(true);
    j.Key(L"minimizedIncluded").Value(spec.includeIconic);
    if (r.iconicExcluded != 0) {
        j.Key(L"minimizedExcluded").Value(static_cast<long long>(r.iconicExcluded));
    }
    // 系统窗口没有可判的身份（没有公开 API 说「我是系统窗口」），所以这里不作任何断言。
    j.Key(L"systemWindowAssertion").Value(false);
    j.Key(L"order").Value(L"zOrder");
    j.End();

    j.Key(L"pagination").Obj();
    j.Key(L"offset").Value(static_cast<long long>(r.offset));
    j.Key(L"limit").Value(static_cast<long long>(r.limit));
    j.Key(L"limitDefaulted").Value(r.limitDefaulted);
    j.Key(L"defaultLimit").Value(static_cast<long long>(cli_limits::kDefaultWindowListLimit));
    j.Key(L"maxLimit").Value(static_cast<long long>(cli_limits::kMaxWindowListItems));
    j.Key(L"matched").Value(static_cast<long long>(r.matched));
    j.Key(L"returned").Value(static_cast<long long>(r.returned));
    j.Key(L"truncated").Value(r.truncated);
    // 下一次翻页的偏移；没有剩下的东西时整个键不出现（写 0 会被读成「从头再来」）。
    if (r.truncated) {
        j.Key(L"nextOffset").Value(static_cast<long long>(r.offset + r.returned));
    }
    j.End();

    // 结果本体：--list 恒为数组（空数组 = 这一次没有任何窗口满足全部条件，那是正常答复）；
    // --inspect 成功时是单个对象。
    if (spec.action == WindowAction::kInspect && r.hasTarget) {
        j.Key(L"window");
        WriteWindow(j, r.windows.front());
    } else {
        j.Key(L"windows").Arr();
        for (const auto& w : r.windows) WriteWindow(j, w);
        j.End();
    }

    if (!r.errors.empty()) DiagArray(j, L"errors", r.errors);
    if (!r.notes.empty() && !quiet) DiagArray(j, L"notes", r.notes);

    // caveats 不受 --quiet 影响：它们与上面那两段自述一样是判据，不是礼貌性提示。
    std::vector<std::wstring> caveats;
    caveats.push_back(window_caveat::kNoCapture);
    caveats.push_back(window_caveat::kNoDialog);
    caveats.push_back(window_caveat::kNoWindowTouched);
    caveats.push_back(window_caveat::kSnapshotExpires);
    caveats.push_back(window_caveat::kIdentityNotToken);
    caveats.push_back(window_caveat::kVisibilityPolicy);
    caveats.push_back(window_caveat::kDeniedNotGuarantee);
    if (r.truncated || r.iconicExcluded > 0) {
        // 「这份列表不是全集」单独写一条：只数 windows[] 会以为整机就这几个窗口。
        caveats.push_back(window_caveat::kListMayBePartial);
    }
    StrArray(j, L"caveats", caveats);

    if (verbose) {
        // 规范化后的这一次查询。别把参数回显塞进默认输出（与截图那一份同一条规矩）。
        j.Key(L"input").Obj();
        j.Key(L"lang").Value(LanguageTag(CurrentLanguage()));
        j.Key(L"action").Value(spec.action == WindowAction::kInspect ? L"inspect" : L"list");
        j.Key(L"offset").Value(static_cast<long long>(r.offset));
        j.Key(L"limit").Value(static_cast<long long>(r.limit));
        j.Key(L"limitGiven").Value(spec.limit != 0);
        j.Key(L"includeIconic").Value(spec.includeIconic);
        j.Key(L"exePath").Value(spec.exePath);
        j.Key(L"timeoutMs").Value(static_cast<long long>(spec.timeoutMs));
        if (!spec.monitorLabel.empty()) {
            j.Key(L"monitor").Value(spec.monitorLabel);
            j.Key(L"target").Value(L"window");
        }
        StrArray(j, L"hwnd", spec.echoHwnds);
        j.Key(L"pid").Arr();
        for (const std::wstring& p : spec.echoPids) j.Value(p);
        j.End();
        StrArray(j, L"process", spec.echoProcesses);
        StrArray(j, L"exe", spec.echoExePaths);
        StrArray(j, L"title", spec.echoTitles);
        StrArray(j, L"titleContains", spec.echoTitleContains);
        StrArray(j, L"titleRegex", spec.echoTitleRegexes);
        StrArray(j, L"class", spec.echoClasses);
        // 选择策略只对 --inspect 有意义（--list 里多匹配是正常答复），所以这里回显的是
        // inspect 会用的那一条策略，与截图那一份的 input.policy 同一个词表。
        j.Key(L"policy").Value(MultiKey(spec.inspectPolicy));
        if (spec.inspectPolicy == MultiMatch::kIndex) {
            j.Key(L"index").Value(spec.inspectIndex);
        }
        j.End();
    }
    j.End();
    return j.Str();
}

// ---------------------------------------------------------------------------
// 本文件到此为止全是纯函数：不碰取帧、不弹框、不开新句柄问进程、不读写文件，
// 所以「字段读不到」「分页边界」「inspect 的多匹配报歧义」这些现场都能由
// tests\windows_state.cpp 注入一条假候选逐条判（离线判据可执行文件也只链这一份 +
// WindowIdentity / WindowMatch 那些与图形库无关的源文件）。
// 真机问答那一步在 WindowQueryRun.cpp —— 它要用条件求值的辅助进程路线（Worker.h），
// 那条路线连着取帧通道，不该被拖进离线判据里。
// ---------------------------------------------------------------------------

}  // namespace ecapture
