// 模拟会话 D-Bus 的 Portal ScreenCast 服务，直接跑生产 DBusPortalScreenCast 驱动。
//
// 目的不是替代真机 GNOME/KDE 授权，而是把协议层可离线验证的几件事钉住：
//   * 先安装匹配/过滤器再发方法调用；
//   * Response 早于方法回复（本测试故意先发 Response）；
//   * CreateSession / SelectSources / Start 的请求路径按 handle_token 关联；
//   * 单窗口只申请 WINDOW、单显示器只申请 MONITOR；
//   * Start 超时后发送 Request.Close 与 Session.Close，拒绝迟到的成功；
//   * Portal 服务消失与来源类型不符分别返回稳定结局。
//
// 必须在 dbus-run-session 里运行，并由测试脚本检查退出码。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <dbus/dbus.h>

#include "../src/PortalScreenCast.h"
#include "../src/platform/linux/DBusPortalScreenCast.h"

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

constexpr const char* kService = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";

std::string RequestPathFor(const char* sender, const std::string& token) {
    std::string unique(sender ? sender : "");
    if (!unique.empty() && unique[0] == ':') unique.erase(unique.begin());
    for (char& c : unique) if (c == '.') c = '_';
    return "/org/freedesktop/portal/desktop/request/" + unique + "/" + token;
}

std::string SessionPathFor(const char* sender, const std::string& token) {
    std::string unique(sender ? sender : "");
    if (!unique.empty() && unique[0] == ':') unique.erase(unique.begin());
    for (char& c : unique) if (c == '.') c = '_';
    return "/org/freedesktop/portal/desktop/session/" + unique + "/" + token;
}

bool DictString(DBusMessageIter* dict, const char* key, std::string* out) {
    DBusMessageIter it = *dict;
    while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&it, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            const char* k = nullptr;
            dbus_message_iter_get_basic(&entry, &k);
            if (k && std::strcmp(k, key) == 0 && dbus_message_iter_next(&entry) &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &variant);
                if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                    const char* v = nullptr;
                    dbus_message_iter_get_basic(&variant, &v);
                    if (v) *out = v;
                    return v != nullptr;
                }
            }
        }
        if (!dbus_message_iter_next(&it)) break;
    }
    return false;
}

bool DictUint32(DBusMessageIter* dict, const char* key, uint32_t* out) {
    DBusMessageIter it = *dict;
    while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, variant;
        dbus_message_iter_recurse(&it, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            const char* k = nullptr;
            dbus_message_iter_get_basic(&entry, &k);
            if (k && std::strcmp(k, key) == 0 && dbus_message_iter_next(&entry) &&
                dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&entry, &variant);
                if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
                    dbus_message_iter_get_basic(&variant, out);
                    return true;
                }
            }
        }
        if (!dbus_message_iter_next(&it)) break;
    }
    return false;
}

void SendReply(DBusConnection* conn, DBusMessage* call) {
    DBusMessage* reply = dbus_message_new_method_return(call);
    if (reply) {
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
    }
    dbus_connection_flush(conn);
}

void AppendVariantString(DBusMessageIter* dict, const char* key, const std::string& value) {
    DBusMessageIter entry, variant;
    const char* k = key;
    const char* v = value.c_str();
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void EmitResponse(DBusConnection* conn, const std::string& requestPath, uint32_t response,
                  DBusMessage* resultMessage, bool createSessionResult,
                  const std::string& sessionHandle, uint32_t streamSourceType) {
    (void)resultMessage;
    DBusMessage* signal = dbus_message_new_signal(requestPath.c_str(),
                                                 "org.freedesktop.portal.Request", "Response");
    if (!signal) return;
    DBusMessageIter root, dict;
    dbus_message_iter_init_append(signal, &root);
    dbus_message_iter_append_basic(&root, DBUS_TYPE_UINT32, &response);
    dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
    if (response == 0 && createSessionResult) {
        AppendVariantString(&dict, "session_handle", sessionHandle);
    } else if (response == 0 && !createSessionResult && streamSourceType != 0) {
        // results: streams -> a(ua{sv})
        DBusMessageIter entry, variant, array, object;
        const char* key = "streams";
        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(ua{sv})", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(ua{sv})", &array);
        dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, nullptr, &object);
        uint32_t nodeId = 77;
        dbus_message_iter_append_basic(&object, DBUS_TYPE_UINT32, &nodeId);
        DBusMessageIter props, propEntry, propVariant;
        dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY, "{sv}", &props);
        const char* sourceKey = "source_type";
        dbus_message_iter_open_container(&props, DBUS_TYPE_DICT_ENTRY, nullptr, &propEntry);
        dbus_message_iter_append_basic(&propEntry, DBUS_TYPE_STRING, &sourceKey);
        dbus_message_iter_open_container(&propEntry, DBUS_TYPE_VARIANT, "u", &propVariant);
        dbus_message_iter_append_basic(&propVariant, DBUS_TYPE_UINT32, &streamSourceType);
        dbus_message_iter_close_container(&propEntry, &propVariant);
        dbus_message_iter_close_container(&props, &propEntry);
        dbus_message_iter_close_container(&object, &props);
        dbus_message_iter_close_container(&array, &object);
        dbus_message_iter_close_container(&variant, &array);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&dict, &entry);
    }
    dbus_message_iter_close_container(&root, &dict);
    dbus_connection_send(conn, signal, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(signal);
}

class MockPortalService {
public:
    enum class Mode { kSuccess, kSourceMismatch, kStartNoResponse, kDropOnStart };

    explicit MockPortalService(Mode mode) : mode_(mode) {}
    ~MockPortalService() { Stop(); }

    bool Start() {
        const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        DBusError error;
        dbus_error_init(&error);
        conn_ = dbus_connection_open_private(address, &error);
        if (!conn_) {
            dbus_error_free(&error);
            return false;
        }
        dbus_connection_set_exit_on_disconnect(conn_, FALSE);
        if (!dbus_bus_register(conn_, &error)) {
            dbus_error_free(&error);
            return false;
        }
        const int rc = dbus_bus_request_name(conn_, kService, DBUS_NAME_FLAG_DO_NOT_QUEUE,
                                             &error);
        dbus_error_free(&error);
        if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) return false;
        dbus_connection_add_filter(conn_, &MockPortalService::Filter, this, nullptr);
        running_.store(true);
        thread_ = std::thread([this] { Loop(); });
        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
        if (conn_) {
            dbus_bus_release_name(conn_, kService, nullptr);
            dbus_connection_remove_filter(conn_, &MockPortalService::Filter, this);
            dbus_connection_close(conn_);
            dbus_connection_unref(conn_);
            conn_ = nullptr;
        }
    }

    std::atomic<int> requestCloses{0};
    std::atomic<int> sessionCloses{0};
    std::atomic<int> createCalls{0};
    std::atomic<int> selectCalls{0};
    std::atomic<int> startCalls{0};
    std::atomic<int> openRemoteCalls{0};
    std::atomic<uint32_t> requestedMask{0};
    std::atomic<bool> serviceLost{false};

private:
    void Loop() {
        while (running_.load()) {
            dbus_connection_read_write_dispatch(conn_, 20);
        }
    }

    static DBusHandlerResult Filter(DBusConnection* conn, DBusMessage* msg, void* data) {
        auto* self = static_cast<MockPortalService*>(data);
        if (!self || dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        return self->Handle(conn, msg);
    }

    DBusHandlerResult Handle(DBusConnection* conn, DBusMessage* msg) {
        const char* iface = dbus_message_get_interface(msg);
        const char* member = dbus_message_get_member(msg);
        const char* path = dbus_message_get_path(msg);
        if (!iface || !member || !path) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        const std::string ifaceStr(iface);
        const std::string memberStr(member);
        const std::string pathStr(path);

        if (ifaceStr == "org.freedesktop.DBus.Properties" && memberStr == "Get") {
            DBusMessageIter it;
            const char* prop = nullptr;
            if (dbus_message_iter_init(msg, &it)) {
                const char* ifaceName = nullptr;
                dbus_message_iter_get_basic(&it, &ifaceName);
                if (dbus_message_iter_next(&it)) dbus_message_iter_get_basic(&it, &prop);
            }
            uint32_t value = 0;
            if (prop && std::strcmp(prop, "version") == 0) value = 6;
            else if (prop && std::strcmp(prop, "AvailableSourceTypes") == 0) value = 3;
            else if (prop && std::strcmp(prop, "AvailableCursorModes") == 0) value = 7;
            else return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
            DBusMessage* reply = dbus_message_new_method_return(msg);
            DBusMessageIter root, variant;
            dbus_message_iter_init_append(reply, &root);
            dbus_message_iter_open_container(&root, DBUS_TYPE_VARIANT, "u", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
            dbus_message_iter_close_container(&root, &variant);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
            dbus_connection_flush(conn);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (ifaceStr == "org.freedesktop.portal.Request" && memberStr == "Close") {
            ++requestCloses;
            SendReply(conn, msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (ifaceStr == "org.freedesktop.portal.Session" && memberStr == "Close") {
            ++sessionCloses;
            SendReply(conn, msg);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (ifaceStr != "org.freedesktop.portal.ScreenCast")
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it)) return DBUS_HANDLER_RESULT_HANDLED;
        const char* sender = dbus_message_get_sender(msg);

        if (memberStr == "CreateSession") {
            ++createCalls;
            std::string handleToken;
            std::string sessionToken;
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                DBusMessageIter dict;
                dbus_message_iter_recurse(&it, &dict);
                DictString(&dict, "handle_token", &handleToken);
                DictString(&dict, "session_handle_token", &sessionToken);
            }
            const std::string req = RequestPathFor(sender, handleToken);
            const std::string session = SessionPathFor(sender, sessionToken);
            sessionHandle_ = session;
            // 竞争测试：先发 Response，再回方法句柄。
            EmitResponse(conn, req, 0, nullptr, true, session, 0);
            DBusMessage* reply = dbus_message_new_method_return(msg);
            const char* objectPath = req.c_str();
            dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &objectPath, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
            dbus_connection_flush(conn);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (memberStr == "SelectSources") {
            ++selectCalls;
            const char* sessionObject = nullptr;
            dbus_message_iter_get_basic(&it, &sessionObject);
            dbus_message_iter_next(&it);
            std::string handleToken;
            uint32_t mask = 0;
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                DBusMessageIter dict;
                dbus_message_iter_recurse(&it, &dict);
                DictString(&dict, "handle_token", &handleToken);
                DictUint32(&dict, "types", &mask);
            }
            requestedMask = mask;
            const std::string req = RequestPathFor(sender, handleToken);
            EmitResponse(conn, req, 0, nullptr, false, std::string(), 0);
            DBusMessage* reply = dbus_message_new_method_return(msg);
            const char* objectPath = req.c_str();
            dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &objectPath, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
            dbus_connection_flush(conn);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (memberStr == "OpenPipeWireRemote") {
            ++openRemoteCalls;
            // 只回一个可关闭的普通 pipe fd；本判据不读像素，只验证 fd 协议、所有权与
            // “保留当前会话 -> OpenRemote -> 显式 CloseSession”的生命周期。
            int fds[2] = {-1, -1};
            if (pipe(fds) != 0) return DBUS_HANDLER_RESULT_HANDLED;
            DBusMessage* reply = dbus_message_new_method_return(msg);
            DBusMessageIter root;
            dbus_message_iter_init_append(reply, &root);
            dbus_message_iter_append_basic(&root, DBUS_TYPE_UNIX_FD, &fds[0]);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
            dbus_connection_flush(conn);
            close(fds[0]);
            close(fds[1]);
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        if (memberStr == "Start") {
            ++startCalls;
            const char* sessionObject = nullptr;
            dbus_message_iter_get_basic(&it, &sessionObject);
            dbus_message_iter_next(&it);   // parent_window
            dbus_message_iter_next(&it);
            std::string handleToken;
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                DBusMessageIter dict;
                dbus_message_iter_recurse(&it, &dict);
                DictString(&dict, "handle_token", &handleToken);
            }
            const std::string req = RequestPathFor(sender, handleToken);
            if (mode_ == Mode::kDropOnStart) {
                serviceLost = true;
                dbus_bus_release_name(conn, kService, nullptr);
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            DBusMessage* reply = dbus_message_new_method_return(msg);
            const char* objectPath = req.c_str();
            dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &objectPath, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, reply, nullptr);
            dbus_message_unref(reply);
            dbus_connection_flush(conn);
            if (mode_ == Mode::kStartNoResponse) {
                // 故意不发 Response：驱动应超时、关闭请求与会话，并忽略迟到的成功。
                return DBUS_HANDLER_RESULT_HANDLED;
            }
            if (mode_ == Mode::kSourceMismatch) {
                EmitResponse(conn, req, 0, nullptr, false, std::string(), 1u);
            } else {
                // 与 CreateSession / SelectSources 同样的竞争：先 Response 再正常结束。
                EmitResponse(conn, req, 0, nullptr, false, std::string(),
                             requestedMask.load() == 2u ? 2u : 1u);
            }
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    Mode mode_;
    DBusConnection* conn_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::string sessionHandle_;
};

struct CaseResult {
    AuthorizationResult result;
    int requestCloses = 0;
    int sessionCloses = 0;
};

CaseResult RunCase(MockPortalService::Mode mode, SourceKind source,
                   uint64_t interactionTimeoutMs) {
    MockPortalService service(mode);
    if (!service.Start()) {
        std::printf("[FAIL] mock portal service could not start\n");
        return CaseResult{};
    }
    auto driver = ecapture::portal::CreateDBusPortalScreenCast();
    Request request;
    request.source = source;
    request.automaticTimeoutMs = 2500;
    request.interactionTimeoutMs = interactionTimeoutMs;
    request.closeSessionOnSuccess = true;
    AuthorizationResult result = driver->AuthorizeAndSelect(request);
    // 给 mock 一点时间收尾 Close 方法；超时路径的 Close 是 fire-and-forget。
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        if (mode == MockPortalService::Mode::kStartNoResponse) {
            if (service.requestCloses.load() > 0 && service.sessionCloses.load() > 0) break;
        } else {
            if (service.sessionCloses.load() > 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CaseResult out;
    out.result = result;
    out.requestCloses = service.requestCloses.load();
    out.sessionCloses = service.sessionCloses.load();
    return out;
}

}  // namespace

int main() {
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        std::printf("SKIP: run this test under dbus-run-session\n");
        return 0;
    }

    Section("1) 窗口来源：Response 先于方法回复，最终授权并关闭会话");
    {
        CaseResult c = RunCase(MockPortalService::Mode::kSuccess, SourceKind::kWindow,
                                        3000);
        Check(c.result.outcome == Outcome::kGranted, "成功取得 Portal 授权与来源选择");
        Check(c.result.requestedSourceMask == 2u, "只向 SelectSources 申请 WINDOW 位");
        Check(c.result.selection.sourceTypeVerified && c.result.selection.exactlyOneStream,
              "stream source_type 已核实且恰好一个");
        Check(c.result.sessionCreated && c.result.sessionClosed, "会话创建后在本步骤关闭");
        Check(!c.result.pixelsRead && !c.result.imageDelivered, "没有读像素、没有交付图像");
        Check(!c.result.restoreTokenPersisted, "没有持久化恢复令牌");
        Check(c.result.requestCompleted, "请求已完成");
    }
    Section("2) 显示器来源：只申请 MONITOR");
    {
        CaseResult c = RunCase(MockPortalService::Mode::kSuccess, SourceKind::kMonitor,
                                        3000);
        Check(c.result.outcome == Outcome::kGranted, "显示器来源授权成功");
        Check(c.result.requestedSourceMask == 1u, "只向 SelectSources 申请 MONITOR 位");
        Check(c.result.selection.streams.size() == 1 &&
                  c.result.selection.streams[0].sourceType == SourceKind::kMonitor,
              "返回的 stream 是 MONITOR");
    }
    Section("3) 来源类型不符：明确停止而不是声称匹配");
    {
        CaseResult c = RunCase(MockPortalService::Mode::kSourceMismatch,
                                        SourceKind::kWindow, 3000);
        Check(c.result.outcome == Outcome::kProtocolError, "窗口请求返回 monitor -> protocol_error");
        Check(c.result.sessionClosed, "失败后关闭会话");
    }
    Section("4) Start 超时：关闭请求与会话，不把迟到成功当授权");
    {
        CaseResult c = RunCase(MockPortalService::Mode::kStartNoResponse,
                                        SourceKind::kWindow, 250);
        Check(c.result.outcome == Outcome::kTimedOut, "人工等待超时");
        Check(c.result.requestClosed && c.result.sessionClosed,
              "超时后结果标记请求与会话已关闭");
        Check(c.requestCloses > 0 && c.sessionCloses > 0,
              "模拟服务确实收到 Request.Close 与 Session.Close");
        Check(!c.result.imageDelivered, "没有图像");
    }
    Section("5) 服务中途消失：服务消失状态，不冒充用户拒绝");
    {
        CaseResult c = RunCase(MockPortalService::Mode::kDropOnStart,
                                        SourceKind::kWindow, 1000);
        Check(c.result.outcome == Outcome::kServiceLost, "服务消失 -> service_lost");
        Check(!c.result.imageDelivered, "没有图像");
    }
    Section("6) 保留当前会话：OpenPipeWireRemote fd 与显式 CloseSession 生命周期");
    {
        MockPortalService service(MockPortalService::Mode::kSuccess);
        Check(service.Start(), "模拟 Portal 服务启动");
        auto driver = ecapture::portal::CreateDBusPortalScreenCast();
        Request request;
        request.source = SourceKind::kWindow;
        request.automaticTimeoutMs = 2500;
        request.interactionTimeoutMs = 3000;
        request.closeSessionOnSuccess = false;
        const AuthorizationResult result = driver->AuthorizeAndSelect(request);
        Check(result.outcome == Outcome::kGranted && result.sessionCreated,
              "授权成功并保留当前会话");
        Check(!result.sessionClosed, "closeSessionOnSuccess=false 时不提前关闭");
        int fd = -1;
        std::string reason;
        Check(driver->OpenPipeWireRemote(&fd, 1000, &reason),
              "只从当前会话 OpenPipeWireRemote() 取得 fd");
        Check(fd >= 0, "fd 非负且所有权交给调用方");
        if (fd >= 0) close(fd);
        Check(service.openRemoteCalls.load() == 1, "模拟服务收到一次 OpenPipeWireRemote");
        driver->CloseSession();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (std::chrono::steady_clock::now() < until &&
               service.sessionCloses.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(service.sessionCloses.load() > 0, "显式清理时发送 Session.Close");
        Check(!driver->OpenPipeWireRemote(&fd, 1000, &reason),
              "会话关闭后不能再次打开 remote");
    }

    std::printf("\nportal-dbus: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
