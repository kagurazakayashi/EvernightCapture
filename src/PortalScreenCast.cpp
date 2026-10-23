#include "PortalScreenCast.h"

#include <utility>

namespace ecapture {
namespace portal {

void RequestTracker::Expect(std::string expectedPath) {
    expectedPath_ = std::move(expectedPath);
    actualPath_.clear();
    matchedResponsePath_.clear();
    methodReplied_ = false;
    responseBeforeReply_ = false;
    handleChanged_ = false;
    lateResponseIgnored_ = false;
    responsesSeen_ = 0;
    state_ = State::kPending;
    buffered_.clear();
}

bool RequestTracker::AcceptsPath(const std::string& path) const {
    if (path.empty()) return false;
    if (!expectedPath_.empty() && path == expectedPath_) return true;
    return methodReplied_ && !actualPath_.empty() && path == actualPath_;
}

void RequestTracker::OnMethodReply(std::string actualPath) {
    if (state_ != State::kPending) return;
    if (actualPath.empty()) {
        state_ = State::kProtocolError;
        return;
    }
    methodReplied_ = true;
    actualPath_ = std::move(actualPath);
    if (!expectedPath_.empty()) handleChanged_ = actualPath_ != expectedPath_;
    // 当前 Portal 会先让调用方拿到方法回复，再发 Response；但官方文档明确允许 Response
    // 竞争到前面。这里按信号自己带的 object path 配对，而不是假定"方法回复一定先到"。
    for (const auto& item : buffered_) {
        if (item.first != actualPath_) continue;
        responseBeforeReply_ = true;
        ApplyResponse(item.second.code, item.first);
        return;
    }
}

void RequestTracker::OnResponse(std::string path, uint32_t response) {
    ++responsesSeen_;
    if (state_ != State::kPending) {
        // 取消 / 超时 / 已经完成的请求不会因为迟到成功而复活。
        if (response == 0) lateResponseIgnored_ = true;
        return;
    }
    if (!methodReplied_) {
        // 调用方只转交它认为可能属于本请求的路径；先按信号路径缓存，等方法回复后再选。
        buffered_.push_back({std::move(path), BufferedResponse{response}});
        responseBeforeReply_ = true;
        return;
    }
    if (path != actualPath_) {
        if (response == 0) lateResponseIgnored_ = true;
        return;
    }
    ApplyResponse(response, path);
}

void RequestTracker::ApplyResponse(uint32_t response, const std::string& path) {
    if (state_ != State::kPending) return;
    matchedResponsePath_ = path;
    switch (response) {
        case 0: state_ = State::kSuccess; break;
        // Portal 只报"用户取消了交互"，没有可核实的拒绝/取消细分。
        case 1: state_ = State::kCancelledOrDenied; break;
        default: state_ = State::kRequestFailed; break;
    }
}

void RequestTracker::MarkTimedOut() {
    if (state_ != State::kPending) return;
    state_ = State::kTimedOut;
}

void RequestTracker::MarkCancelled() {
    if (state_ != State::kPending) return;
    state_ = State::kCancelled;
}

void RequestTracker::MarkCancelledOrDenied() {
    if (state_ != State::kPending) return;
    state_ = State::kCancelledOrDenied;
}

void RequestTracker::MarkProtocolError() {
    if (state_ != State::kPending) return;
    state_ = State::kProtocolError;
}

}  // namespace portal
}  // namespace ecapture
