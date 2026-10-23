#include "PortalSystemAuth.h"

#include "CliOptions.h"   // 稳定诊断码
#include "core/ConsentState.h"

namespace ecapture {
namespace {

SystemAuthReply Reply(SystemAuthState state, const wchar_t* code, const std::wstring& detail,
                      const std::wstring& targetKey) {
    SystemAuthReply reply;
    reply.state = state;
    if (code) reply.code = code;
    reply.detail = detail;
    reply.targetKey = targetKey;
    return reply;
}

std::wstring W(const char* text) {
    std::wstring out;
    if (!text) return out;
    while (*text) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text++)));
    return out;
}

}  // namespace

PortalSystemAuthProvider::PortalSystemAuthProvider(portal::IPortalScreenCast& portal,
                                                   portal::SourceKind source,
                                                   uint64_t automaticTimeoutMs,
                                                   uint64_t interactionTimeoutMs,
                                                   portal::CancelToken* cancelToken)
    : portal_(portal),
      source_(source),
      automaticTimeoutMs_(automaticTimeoutMs),
      interactionTimeoutMs_(interactionTimeoutMs),
      cancelToken_(cancelToken) {}

SystemAuthReply PortalSystemAuthProvider::Authorize(const SystemAuthRequest& request) {
    portal::Request portalRequest;
    portalRequest.source = source_;
    // 本项目是 CLI，没有可安全导出的父窗口句柄。窗口标识规范明确允许空字符串；
    // 不伪造 x11:/wayland: 标识，也不要求调用方为了授权去创建假窗口。
    portalRequest.parentWindow.clear();
    portalRequest.automaticTimeoutMs =
        automaticTimeoutMs_ == 0 ? 5000 : automaticTimeoutMs_;
    portalRequest.interactionTimeoutMs = interactionTimeoutMs_;
    portalRequest.cancelToken = cancelToken_;
    // 本步骤只验证授权 / 选择链：会话必须关闭，绝不留下一个可继续读取的会话。
    portalRequest.closeSessionOnSuccess = true;

    last_ = portal_.AuthorizeAndSelect(portalRequest);
    const std::wstring detail = W(last_.reason.c_str());
    const std::wstring targetKey = request.targetKey;

    switch (last_.outcome) {
        case portal::Outcome::kGranted: {
            const uint32_t requestedMask = portal::SourceKindMask(source_);
            const bool maskOk = last_.requestedSourceMask == requestedMask;
            const bool oneStream = last_.selection.exactlyOneStream &&
                                   last_.selection.streams.size() == 1;
            const bool sourceOk = last_.selection.sourceTypeVerified &&
                                  oneStream &&
                                  last_.selection.streams[0].sourceTypeKnown &&
                                  last_.selection.streams[0].sourceType == source_;
            if (!maskOk || !sourceOk) {
                // 门户返回了成功，但返回的来源不是这次请求的那一种 / 不是恰好一个：
                // 这不能证明"单窗口只授权了窗口来源"，按协议错误 fail-closed。
                if (!oneStream) {
                    return Reply(SystemAuthState::kError, codes::kPortalMultipleStreams,
                                 L"stream_count_or_source_type_not_exactly_one", targetKey);
                }
                return Reply(SystemAuthState::kError, codes::kPortalSourceMismatch,
                             L"selected_source_type_mismatch", targetKey);
            }
            return Reply(SystemAuthState::kGranted, nullptr, detail, targetKey);
        }
        case portal::Outcome::kCancelledOrDenied:
            return Reply(SystemAuthState::kCancelledOrDenied,
                         codes::kPortalCancelledOrDenied, detail, targetKey);
        case portal::Outcome::kTimedOut:
            return Reply(SystemAuthState::kTimedOut, codes::kPortalTimeout, detail, targetKey);
        case portal::Outcome::kCancelled:
            return Reply(SystemAuthState::kCancelled, codes::kPortalCancelled, detail, targetKey);
        case portal::Outcome::kUnavailable:
            return Reply(SystemAuthState::kUnavailable, codes::kPortalUnavailable, detail,
                         targetKey);
        case portal::Outcome::kUnsupported:
            return Reply(SystemAuthState::kUnsupported, codes::kPortalUnsupported, detail,
                         targetKey);
        case portal::Outcome::kServiceLost:
            return Reply(SystemAuthState::kServiceLost, codes::kPortalServiceLost, detail,
                         targetKey);
        case portal::Outcome::kProtocolError:
            return Reply(SystemAuthState::kError, codes::kPortalProtocolError, detail, targetKey);
        case portal::Outcome::kRequestFailed:
            return Reply(SystemAuthState::kError, codes::kPortalProtocolError, detail, targetKey);
    }
    return Reply(SystemAuthState::kError, codes::kPortalProtocolError, detail, targetKey);
}

}  // namespace ecapture
