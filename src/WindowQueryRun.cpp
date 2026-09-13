#include "WindowQueryRun.h"

#include <cstdio>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"   // EnsureDpiAware：窗口矩形要的是物理像素
#include "Deadline.h"
#include "Lang.h"
#include "ScreenMatch.h"
#include "Worker.h"          // IsolatedMatch / BlockedToDiagnostic（条件求值的同一条路线）

namespace ecapture {

WindowQueryResult RunWindowQuery(const Options& opt) {
    // 窗口矩形必须是物理像素，否则同一扇窗口在 --list 里与在 images[].rect 里对不上号
    //（截图链路在 RunCapture 开头做同一件事，这里不是新增的副作用，只是同一条要求）。
    EnsureDpiAware();

    WindowQueryResult result;
    const Deadline dl = Deadline::FromTotalMs(opt.timeoutMs);

    // 还没跑到判据就先失败的几条路（屏幕编号越界、条件求值本身没跑完）也交同一份文档：
    // 取舍先落在结果里，报告不会出现"这一次允许的条数是 0"这种没问出来的形状。
    WindowQuerySpec spec = MakeWindowQuerySpec(opt);
    spec.monitorLabel = MonitorLabelOf(opt);
    result.spec = spec;
    result.limit = spec.limit != 0 ? spec.limit : cli_limits::kDefaultWindowListLimit;
    result.limitDefaulted = (spec.limit == 0);

    std::vector<RECT> onScreens;
    if (opt.monitor.given) {
        // --monitor 的编号越界在这里就报：它与 --index 越界同一类（写错了编号），退出码 1。
        onScreens = SelectedScreenRects(opt, &result.errors);
        if (!result.errors.empty()) {
            for (auto& d : result.errors) {
                if (d.stage.empty()) d.stage = stages::kMatch;
            }
            result.exitCode = WindowQueryExitCodeFor(result.errors.front().code);
            return result;
        }
    }

    // 条件求值与截图链路走**同一条**判据：用了 --title-regex 或设了预算就整步交给辅助进程，
    // 两者都没有时照旧在本进程枚举。这一步不弹框、不取帧，辅助进程本来也只有这两类活之一。
    MatchRequest request;
    request.match = opt.match;
    request.onScreens = onScreens;
    const bool isolate = !opt.match.titleRegexes.empty() || dl.Enabled();

    WindowQuerySnapshot snapshot;
    if (isolate) {
        Diagnostic err;
        if (!IsolatedMatch(request.match, request.onScreens, dl, &snapshot.hits,
                           &snapshot.iconic, &err)) {
            // 期限到了 / 正则不合本机上限 / 辅助进程坏了：照实报，不悄悄退回本进程再跑一遍
            //（那样等于把期限当成建议，慢的那一步下一次还会再慢一遍），也**不**在这里重新分类
            //（判据见 WindowQuery.h 的 RecordMatchFailure）。
            RecordMatchFailure(&result, std::move(err));
            return result;
        }
    } else {
        const MatchOutcome m = EnumerateMatches(request);
        if (m.status != BlockedStatus::kOk) {
            RecordMatchFailure(&result,
                               BlockedToDiagnostic(m.status, 0, S_OK, m.detail, L"match",
                                                  stages::kMatch));
            return result;
        }
        snapshot.hits = std::move(m.hits);
        snapshot.iconic = std::move(m.iconic);
    }

    result = BuildWindowQueryResult(snapshot, spec, opt.match, spec.monitorLabel);
    if (!result.errors.empty()) return result;

    // 每一次成功的窗口查询都配一条快照过期提示：这一份列表不是凭证，
    // 真去截图时仍要按《窗口选择与身份一致性》那一节复核。--quiet 可以抑制这一条，
    // 但它不能把这件事本身抑制掉 —— caveats 里同源的 token 恒在。
    Diagnostic stale;
    stale.code = codes::kWindowQueryStale;
    stale.message = Msg(L"note.window_query_stale");
    stale.hint = Msg(L"note.window_query_stale_hint");
    stale.stage = stages::kMatch;
    if (result.hasTarget) {
        wchar_t buf[24];
        swprintf(buf, 24, L"0x%08X", static_cast<unsigned>(result.targetHwnd));
        stale.target = buf;
    }
    result.notes.push_back(std::move(stale));
    return result;
}

}  // namespace ecapture
