// Portal ScreenCast 请求 / 响应关联与系统授权映射的离线判据。
//
// 这里不连会话总线、不打开系统窗口、不取像素；用可注入的 IPortalScreenCast 假实现
// 逐条钉住状态机。真实 D-Bus 驱动由生产代码提供，真实 GNOME/KDE 授权仍必须由用户手动验证。

#include <cstdio>
#include <string>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/PortalScreenCast.h"
#include "../src/PortalSystemAuth.h"
#include "../src/core/ConsentState.h"

using namespace ecapture;
using namespace ecapture::portal;

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        ++g_passed;
        std::printf("  ok   %s\n", what);
    } else {
        ++g_failed;
        std::printf("[FAIL] %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

bool CodeIs(const SystemAuthReply& reply, const wchar_t* code) {
    return reply.code == code;
}

class FakePortal final : public IPortalScreenCast {
public:
    AuthorizationResult result;
    Request seen;
    int calls = 0;

    AuthorizationResult AuthorizeAndSelect(const Request& request) override {
        ++calls;
        seen = request;
        return result;
    }
};

AuthorizationResult Granted(SourceKind source) {
    AuthorizationResult r;
    r.outcome = Outcome::kGranted;
    r.reason = reason::kGranted;
    r.versionKnown = true;
    r.portalVersion = 6;
    r.sourceTypesKnown = true;
    r.availableSourceTypes = SourceKindMask(SourceKind::kWindow) |
                             SourceKindMask(SourceKind::kMonitor);
    r.requestedSourceMask = SourceKindMask(source);
    r.sessionCreated = true;
    r.requestCompleted = true;
    r.sessionClosed = true;
    r.selection.requested = source;
    r.selection.exactlyOneStream = true;
    r.selection.sourceTypeVerified = true;
    StreamInfo stream;
    stream.nodeId = 42;
    stream.sourceTypeKnown = true;
    stream.sourceType = source;
    r.selection.streams.push_back(stream);
    return r;
}

}  // namespace

int main() {
    Section("1) RequestTracker：Response 早于方法回复的竞争");
    {
        RequestTracker t;
        const std::string expected =
            "/org/freedesktop/portal/desktop/request/1_42/ecapture_abc";
        t.Expect(expected);
        t.OnResponse(expected, 0);   // 官方允许的竞争：先到
        Check(t.pending(), "方法回复前不抢跑：先到 Response 只缓存，仍是 pending");
        Check(t.ResponseArrivedBeforeMethodReply(), "记录 Response 先到");
        t.OnMethodReply(expected);
        Check(t.state() == RequestTracker::State::kSuccess, "方法回复后按路径配对成 success");
        Check(t.MatchedResponsePath() == expected, "成功响应路径与请求路径一致");
        Check(!t.LateResponseIgnored(), "这不是迟到响应");
    }
    {
        RequestTracker t;
        const std::string expected = "/request/expected";
        const std::string actual = "/request/old-style-actual";
        t.Expect(expected);
        // 旧版早期 Response 可能带不可预测路径；驱动器按实际路径缓存，方法回复后再配对。
        t.OnResponse(actual, 0);
        Check(t.pending(), "不可预测路径的早期 Response 先缓存，不抢跑");
        t.OnMethodReply(actual);
        Check(t.state() == RequestTracker::State::kSuccess,
              "方法回复给出实际路径后配对成功");
    }
    {
        RequestTracker t;
        const std::string expected = "/request/expected";
        const std::string actual = "/request/actual";
        t.Expect(expected);
        t.OnMethodReply(actual);
        Check(t.pending(), "方法回复本身不是授权完成");
        t.OnResponse(actual, 0);
        Check(t.state() == RequestTracker::State::kSuccess, "按方法实际返回路径配对");
        Check(t.HandleChanged(), "记录旧版实际路径与预期不同");
    }
    {
        RequestTracker t;
        t.Expect("/request/expected");
        t.OnMethodReply("/request/actual");
        t.OnResponse("/request/expected", 0);
        Check(t.pending(), "非实际路径上的 Response 不冒充本请求成功");
        Check(t.AcceptsPath("/request/expected"), "预期路径仍可用于旧式宽匹配过滤");
    }

    Section("2) RequestTracker：取消、超时与迟到成功");
    {
        RequestTracker t;
        t.Expect("/request/a");
        t.MarkCancelled();
        t.OnResponse("/request/a", 0);
        Check(t.state() == RequestTracker::State::kCancelled, "程序取消后状态不变");
        Check(t.LateResponseIgnored(), "迟到成功被记为忽略");
    }
    {
        RequestTracker t;
        t.Expect("/request/b");
        t.OnMethodReply("/request/b");
        t.MarkTimedOut();
        t.OnResponse("/request/b", 0);
        Check(t.state() == RequestTracker::State::kTimedOut, "超时后迟到成功不能改判");
        Check(t.LateResponseIgnored(), "超时后的成功响应被忽略");
    }
    {
        RequestTracker t;
        t.Expect("/request/c");
        t.OnMethodReply("/request/c");
        t.OnResponse("/request/c", 1);
        Check(t.state() == RequestTracker::State::kCancelledOrDenied,
              "Portal response=1 是合并的取消/拒绝状态");
    }
    {
        RequestTracker t;
        t.Expect("/request/d");
        t.OnMethodReply("/request/d");
        t.OnResponse("/request/d", 2);
        Check(t.state() == RequestTracker::State::kRequestFailed,
              "Portal response=2 是其它结束，不编造用户拒绝");
    }

    Section("3) 系统授权映射：单窗口 / 单显示器与两层绑定");
    {
        FakePortal fake;
        fake.result = Granted(SourceKind::kWindow);
        PortalSystemAuthProvider provider(fake, SourceKind::kWindow, 3210, 4560);
        SystemAuthRequest request;
        request.scope = PixelScope::kWindowContent;
        request.path = L"portal.window";
        request.targetKey = L"portal.window";
        const SystemAuthReply reply = provider.Authorize(request);
        Check(reply.state == SystemAuthState::kGranted, "正确窗口来源 -> granted");
        Check(reply.targetKey == L"portal.window", "授权绑定本次目标");
        Check(fake.calls == 1 && fake.seen.source == SourceKind::kWindow,
              "请求只申请 WINDOW 来源");
        Check(fake.seen.parentWindow.empty(), "无父窗口时使用接口允许的空字符串");
        Check(fake.seen.automaticTimeoutMs == 3210 && fake.seen.interactionTimeoutMs == 4560,
              "自动预算与人工等待分别传递");
        Check(fake.seen.closeSessionOnSuccess, "本步骤成功后关闭会话，不遗留可用会话");
    }
    {
        FakePortal fake;
        fake.result = Granted(SourceKind::kMonitor);
        PortalSystemAuthProvider provider(fake, SourceKind::kMonitor, 1000, 2000);
        SystemAuthRequest request;
        request.scope = PixelScope::kDesktop;
        request.path = L"portal.monitor";
        request.targetKey = L"portal.monitor";
        const SystemAuthReply reply = provider.Authorize(request);
        Check(reply.state == SystemAuthState::kGranted, "正确显示器来源 -> granted");
        Check(fake.seen.source == SourceKind::kMonitor, "显示器请求只申请 MONITOR 来源");
        Check(SourceKindMask(fake.seen.source) == 1u, "来源位掩码只有 MONITOR");
    }
    {
        FakePortal fake;
        fake.result = Granted(SourceKind::kWindow);
        fake.result.selection.streams[0].sourceType = SourceKind::kMonitor;
        PortalSystemAuthProvider provider(fake, SourceKind::kWindow, 1000, 2000);
        SystemAuthRequest request;
        request.scope = PixelScope::kWindowContent;
        request.path = L"portal.window";
        request.targetKey = L"portal.window";
        const SystemAuthReply reply = provider.Authorize(request);
        Check(reply.state == SystemAuthState::kError, "返回 monitor 冒充 window -> 失败关闭");
        Check(CodeIs(reply, codes::kPortalSourceMismatch), "来源类型不符有独立稳定码");
    }
    {
        FakePortal fake;
        fake.result = Granted(SourceKind::kWindow);
        fake.result.selection.streams.push_back(fake.result.selection.streams.front());
        PortalSystemAuthProvider provider(fake, SourceKind::kWindow, 1000, 2000);
        SystemAuthRequest request;
        request.scope = PixelScope::kWindowContent;
        request.path = L"portal.window";
        request.targetKey = L"portal.window";
        const SystemAuthReply reply = provider.Authorize(request);
        Check(reply.state == SystemAuthState::kError, "multiple=false 却返回两个流 -> 失败");
        Check(CodeIs(reply, codes::kPortalMultipleStreams), "多流有独立稳定码");
    }

    Section("4) 系统授权失败分类：拒绝/取消、超时、不可用、不支持、服务消失、协议错误");
    {
        struct Case {
            Outcome outcome;
            SystemAuthState state;
            const wchar_t* code;
            const char* what;
        } cases[] = {
            {Outcome::kCancelledOrDenied, SystemAuthState::kCancelledOrDenied,
             codes::kPortalCancelledOrDenied, "response=1 -> 合并拒绝/取消"},
            {Outcome::kTimedOut, SystemAuthState::kTimedOut, codes::kPortalTimeout,
             "超时 -> timed_out"},
            {Outcome::kCancelled, SystemAuthState::kCancelled, codes::kPortalCancelled,
             "程序取消 -> cancelled"},
            {Outcome::kUnavailable, SystemAuthState::kUnavailable, codes::kPortalUnavailable,
             "总线/服务不可用 -> unavailable"},
            {Outcome::kUnsupported, SystemAuthState::kUnsupported, codes::kPortalUnsupported,
             "接口/版本/来源不支持 -> unsupported"},
            {Outcome::kServiceLost, SystemAuthState::kServiceLost, codes::kPortalServiceLost,
             "服务中途消失 -> service_lost"},
            {Outcome::kProtocolError, SystemAuthState::kError, codes::kPortalProtocolError,
             "协议错误 -> error"},
        };
        for (const Case& c : cases) {
            FakePortal fake;
            fake.result.outcome = c.outcome;
            fake.result.reason = reason::kProtocolError;
            PortalSystemAuthProvider provider(fake, SourceKind::kWindow, 1000, 2000);
            SystemAuthRequest request;
            request.scope = PixelScope::kWindowContent;
            request.path = L"portal.window";
            request.targetKey = L"portal.window";
            const SystemAuthReply reply = provider.Authorize(request);
            Check(reply.state == c.state, c.what);
            Check(CodeIs(reply, c.code), "失败分类码稳定");
            Check(reply.detail == L"portal_protocol_error",
                  "detail 是稳定 ASCII token，不含句柄或令牌");
        }
    }

    Section("5) 两层授权合并判据");
    {
        Check(!ConsentLayersSatisfy(kGrantProgramConfirm | kGrantSystemAuthorization,
                                    ConsentState::kGranted,
                                    SystemAuthState::kCancelledOrDenied),
              "系统合并拒绝/取消时两层不满足");
        Check(ConsentLayersSatisfy(kGrantProgramConfirm | kGrantSystemAuthorization,
                                   ConsentState::kGranted, SystemAuthState::kGranted),
              "两层都取得才满足");
        Check(!ConsentLayersSatisfy(kGrantProgramConfirm | kGrantSystemAuthorization,
                                    ConsentState::kWaiting, SystemAuthState::kGranted),
              "程序确认还在等待时仍不满足");
        Check(SystemAuthBlocks(SystemAuthState::kCancelledOrDenied) &&
                  SystemAuthBlocks(SystemAuthState::kUnsupported) &&
                  SystemAuthBlocks(SystemAuthState::kServiceLost),
              "系统失败各档都会停批");
    }

    std::printf("\nportal: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
