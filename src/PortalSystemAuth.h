#pragma once
// 把平台 Portal ScreenCast 驱动接到 ConsentGate 的系统授权层接缝。
//
// 这里不包含任何 D-Bus 头。测试可以给 IPortalScreenCast 注入假实现，逐条验证：
//   * 单窗口只请求 WINDOW、单显示器只请求 MONITOR；
//   * Start 成功但 stream 数量 / source_type 无法核实时不冒充成功；
//   * 系统只报 response=1 时状态是合并的 cancelled_or_denied；
//   * 超时、服务消失、协议错误与用户取消分开。

#include <cstdint>
#include <string>

#include "Consent.h"
#include "PortalScreenCast.h"

namespace ecapture {

class PortalSystemAuthProvider final : public ISystemAuthorizationProvider {
public:
    PortalSystemAuthProvider(portal::IPortalScreenCast& portal, portal::SourceKind source,
                             uint64_t automaticTimeoutMs, uint64_t interactionTimeoutMs,
                             portal::CancelToken* cancelToken = nullptr,
                             bool closeSessionOnSuccess = true,
                             portal::CursorMode cursorMode = portal::CursorMode::kDefault);

    SystemAuthReply Authorize(const SystemAuthRequest& request) override;

    const portal::AuthorizationResult& LastResult() const { return last_; }
    portal::SourceKind Source() const { return source_; }

private:
    portal::IPortalScreenCast& portal_;
    portal::SourceKind source_;
    uint64_t automaticTimeoutMs_ = 5000;
    uint64_t interactionTimeoutMs_ = 0;
    portal::CancelToken* cancelToken_ = nullptr;
    bool closeSessionOnSuccess_ = true;
    portal::CursorMode cursorMode_ = portal::CursorMode::kDefault;
    portal::AuthorizationResult last_{};
};

}  // namespace ecapture
