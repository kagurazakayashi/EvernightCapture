// 【平台层 · Linux】XDG Desktop Portal ScreenCast 的会话 D-Bus 驱动（授权与来源选择阶段）。
//
// 接口依据（当前官方文档）：
//   * org.freedesktop.portal.ScreenCast v6：CreateSession / SelectSources / Start 的
//     session 生命周期与 options / results；
//   * org.freedesktop.portal.Request：handle_token 预生成请求路径、Response(u,a{sv}) 竞争、
//     Close() 与 response=1 的用户取消；
//   * org.freedesktop.portal.Session：Close() 与 Closed 信号；
//   * Window Identifiers：没有可导出的父窗口时 parent_window 传空字符串是允许的。
//
// 授权成功后：auth-only 请求立即关闭会话；PipeWire 交付请求保留同一会话并允许调用方
// 通过 OpenPipeWireRemote() 取 fd。本文件读不到像素，也不枚举 PipeWire registry；错误原因
// 只写稳定 token，不记录对象路径、恢复令牌或用户输入。

#include "DBusPortalScreenCast.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

#include "PortalScreenCast.h"

#if defined(ECAPTURE_HAVE_DBUS)
#include <dbus/dbus.h>
#endif

namespace ecapture {
namespace portal {
namespace {

int64_t NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#if !defined(ECAPTURE_HAVE_DBUS)
class UnavailablePortalScreenCast final : public IPortalScreenCast {
public:
    AuthorizationResult AuthorizeAndSelect(const Request& request) override {
        AuthorizationResult result;
        result.outcome = Outcome::kUnavailable;
        result.reason = reason::kBusUnavailable;
        result.requestedSourceMask = SourceKindMask(request.source);
        return result;
    }
};
#else

constexpr const char* kPortalService = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCastInterface = "org.freedesktop.portal.ScreenCast";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";
constexpr const char* kSessionInterface = "org.freedesktop.portal.Session";

// 官方文档：source_type 从 ScreenCast v3 起才在 Start 的 stream 属性里可核实。
// 低于 3 时即使拿到会话也无法证明“单窗口请求只选中了窗口”，本步骤因此明确 unsupported。
constexpr uint32_t kMinVersionForVerifiedSource = 3;

std::string RandomToken() {
    std::random_device rd;
    std::string token = "ecapture_";
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        token.push_back(kHex[rd() & 0xfu]);
    }
    return token;
}

std::string SenderComponent(const char* uniqueName) {
    if (!uniqueName) return std::string();
    std::string sender(uniqueName);
    if (!sender.empty() && sender[0] == ':') sender.erase(sender.begin());
    std::replace(sender.begin(), sender.end(), '.', '_');
    return sender;
}

std::string RequestPath(const char* uniqueName, const std::string& token) {
    return std::string("/org/freedesktop/portal/desktop/request/") +
           SenderComponent(uniqueName) + "/" + token;
}

std::string SessionPath(const char* uniqueName, const std::string& token) {
    return std::string("/org/freedesktop/portal/desktop/session/") +
           SenderComponent(uniqueName) + "/" + token;
}


void AppendDictString(DBusMessageIter* dict, const char* key, const std::string& value) {
    const char* rawKey = key;
    const char* rawValue = value.c_str();
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &rawKey);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &rawValue);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void AppendDictUint32(DBusMessageIter* dict, const char* key, uint32_t value) {
    const char* rawKey = key;
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &rawKey);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void AppendDictBool(DBusMessageIter* dict, const char* key, bool value) {
    const char* rawKey = key;
    dbus_bool_t rawValue = value ? TRUE : FALSE;
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &rawKey);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &rawValue);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

struct PropertyAnswer {
    bool known = false;
    uint32_t value = 0;
};

PropertyAnswer ReadUint32Property(DBusConnection* conn, const char* interface,
                                  const char* property, int64_t deadline) {
    PropertyAnswer answer;
    if (!conn) return answer;
    DBusError error;
    dbus_error_init(&error);
    DBusMessage* request = dbus_message_new_method_call(
        kPortalService, kPortalPath, "org.freedesktop.DBus.Properties", "Get");
    if (!request) return answer;
    const char* iface = interface;
    const char* prop = property;
    dbus_message_append_args(request, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                             DBUS_TYPE_INVALID);

    int waitMs = 2500;
    int remain = 0;
    if (deadline > 0) {
        const int64_t left = deadline - NowMs();
        if (left <= 0) {
            dbus_message_unref(request);
            return answer;
        }
        waitMs = static_cast<int>(std::min<int64_t>(left, 2500));
    }
    (void)remain;
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, request, waitMs, &error);
    dbus_message_unref(request);
    if (!reply) {
        if (dbus_error_is_set(&error)) {
            const bool missing =
                std::strcmp(error.name, "org.freedesktop.DBus.Error.UnknownInterface") == 0 ||
                std::strcmp(error.name, "org.freedesktop.DBus.Error.UnknownProperty") == 0 ||
                std::strcmp(error.name, "org.freedesktop.DBus.Error.InvalidArgs") == 0;
            answer.known = missing;
        }
        dbus_error_free(&error);
        return answer;
    }
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char* name = dbus_message_get_error_name(reply);
        answer.known = name && (std::strcmp(name, "org.freedesktop.DBus.Error.UnknownInterface") == 0 ||
                                std::strcmp(name, "org.freedesktop.DBus.Error.UnknownProperty") == 0);
        dbus_message_unref(reply);
        return answer;
    }

    DBusMessageIter it;
    if (dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        DBusMessageIter variant;
        dbus_message_iter_recurse(&it, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
            dbus_message_iter_get_basic(&variant, &answer.value);
            answer.known = true;
        }
    }
    dbus_message_unref(reply);
    return answer;
}

void AppendOptionsStart(DBusMessage* message, DBusMessageIter* root, DBusMessageIter* dict) {
    dbus_message_iter_init_append(message, root);
    dbus_message_iter_open_container(root, DBUS_TYPE_ARRAY, "{sv}", dict);
}

void FinishOptions(DBusMessageIter* root, DBusMessageIter* dict) {
    dbus_message_iter_close_container(root, dict);
}

bool ParseUint32Argument(DBusMessage* message, uint32_t* out) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(message, &it)) return false;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
    dbus_message_iter_get_basic(&it, out);
    return true;
}

bool ParseObjectPathArgument(DBusMessage* message, std::string* out) {
    if (!message || !out) return false;
    DBusMessageIter it;
    if (!dbus_message_iter_init(message, &it)) return false;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH) return false;
    const char* value = nullptr;
    dbus_message_iter_get_basic(&it, &value);
    if (!value) return false;
    *out = value;
    return true;
}

bool DictFindVariant(DBusMessageIter* dict, const char* wanted, DBusMessageIter* variantOut) {
    if (!dict || !wanted) return false;
    DBusMessageIter it = *dict;
    while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&it, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
            const char* key = nullptr;
            dbus_message_iter_get_basic(&entry, &key);
            if (key && std::strcmp(key, wanted) == 0) {
                if (!dbus_message_iter_next(&entry) ||
                    dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
                    return false;
                }
                dbus_message_iter_recurse(&entry, variantOut);
                return true;
            }
        }
        if (!dbus_message_iter_next(&it)) break;
    }
    return false;
}

bool VariantString(DBusMessageIter* variant, std::string* out) {
    if (!variant || !out) return false;
    const int type = dbus_message_iter_get_arg_type(variant);
    if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH) return false;
    const char* value = nullptr;
    dbus_message_iter_get_basic(variant, &value);
    if (!value) return false;
    *out = value;
    return true;
}

struct ResponseEnvelope {
    DBusMessage* message = nullptr;   // owned
    uint32_t response = 2;
};

struct WaitContext {
    RequestTracker* tracker = nullptr;
    std::map<std::string, ResponseEnvelope> responses;
    std::string sessionHandle;
    bool serviceLost = false;
    bool disconnected = false;
    bool unsupported = false;
    bool sessionClosedByPortal = false;

    ~WaitContext() {
        for (auto& item : responses) {
            if (item.second.message) dbus_message_unref(item.second.message);
        }
    }
};

DBusHandlerResult PortalFilter(DBusConnection* connection, DBusMessage* message, void* data) {
    (void)connection;
    auto* ctx = static_cast<WaitContext*>(data);
    if (!ctx) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_signal(message, kRequestInterface, "Response")) {
        const char* path = dbus_message_get_path(message);
        if (!path || !ctx->tracker) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        std::string requestPath(path);
        // 方法回复之前实际路径还不知道。官方当前版本可用 handle_token 预测，但旧版可能用
        // 不可预测的路径；所以这里先把所有 Response 按自己的 object path 缓存，等方法回复
        // 给出实际 handle 后再配对。方法回复之后只接收预期/实际路径，避免把别的客户端信号
        // 当成自己的。
        if (ctx->tracker->MethodReplied() && !ctx->tracker->AcceptsPath(requestPath)) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        uint32_t response = 2;
        if (!ParseUint32Argument(message, &response)) {
            ctx->tracker->MarkProtocolError();
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        auto existing = ctx->responses.find(requestPath);
        if (existing != ctx->responses.end()) {
            if (existing->second.message) dbus_message_unref(existing->second.message);
            ctx->responses.erase(existing);
        }
        ResponseEnvelope envelope;
        envelope.message = dbus_message_ref(message);
        envelope.response = response;
        ctx->responses.emplace(requestPath, envelope);
        ctx->tracker->OnResponse(requestPath, response);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, kSessionInterface, "Closed")) {
        const char* path = dbus_message_get_path(message);
        if (path && !ctx->sessionHandle.empty() && ctx->sessionHandle == path) {
            ctx->sessionClosedByPortal = true;
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, "org.freedesktop.DBus", "NameOwnerChanged")) {
        DBusMessageIter it;
        if (!dbus_message_iter_init(message, &it)) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        const char* name = nullptr;
        dbus_message_iter_get_basic(&it, &name);
        if (!name || std::strcmp(name, kPortalService) != 0)
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        ctx->serviceLost = true;
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

class DBusPortalScreenCast final : public IPortalScreenCast {
public:
    ~DBusPortalScreenCast() override { CloseSession(); }

    AuthorizationResult AuthorizeAndSelect(const Request& request) override {
        AuthorizationResult result;
        if (heldConnection_) {
            // 一个驱动实例只持有一次授权会话；复用会让来源与目标绑定变得不可核实。
            result.outcome = Outcome::kProtocolError;
            result.reason = reason::kProtocolError;
            return result;
        }
        result.requestedSourceMask = SourceKindMask(request.source);
        result.selection.requested = request.source;

        DBusError error;
        dbus_error_init(&error);
        const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        std::string derived;
        if (!address || !*address) {
            const char* runtime = std::getenv("XDG_RUNTIME_DIR");
            if (runtime && *runtime) {
                derived = "unix:path=";
                derived += runtime;
                derived += "/bus";
                address = derived.c_str();
            }
        }
        if (!address || !*address || std::strncmp(address, "autolaunch:", 11) == 0) {
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kBusUnavailable;
            return result;
        }

        const int64_t automaticDeadline =
            request.automaticTimeoutMs == 0 ? 0
                                            : NowMs() + static_cast<int64_t>(request.automaticTimeoutMs);
        DBusConnection* connection =
            dbus_connection_open_private(address, &error);
        if (!connection) {
            dbus_error_free(&error);
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kBusUnavailable;
            return result;
        }
        dbus_connection_set_exit_on_disconnect(connection, FALSE);
        if (!dbus_bus_register(connection, &error)) {
            dbus_error_free(&error);
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kBusUnavailable;
            return result;
        }

        // 必须在任何 Portal 方法调用之前安装宽匹配与过滤器：Response 可能早于方法回复到达。
        if (!dbus_bus_name_has_owner(connection, kPortalService, &error)) {
            dbus_error_free(&error);
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kServiceUnavailable;
            return result;
        }
        dbus_error_free(&error);

        const PropertyAnswer version =
            ReadUint32Property(connection, kScreenCastInterface, "version", automaticDeadline);
        if (!version.known) {
            CleanupConnection(connection, nullptr, nullptr, false);
            result.outcome = Outcome::kUnsupported;
            result.reason = reason::kVersionUnverified;
            return result;
        }
        result.versionKnown = true;
        result.portalVersion = version.value;
        if (version.value < kMinVersionForVerifiedSource) {
            CleanupConnection(connection, nullptr, nullptr, false);
            result.outcome = Outcome::kUnsupported;
            result.reason = reason::kVersionTooLow;
            return result;
        }

        const PropertyAnswer sources =
            ReadUint32Property(connection, kScreenCastInterface, "AvailableSourceTypes",
                               automaticDeadline);
        if (!sources.known) {
            CleanupConnection(connection, nullptr, nullptr, false);
            result.outcome = Outcome::kUnsupported;
            result.reason = reason::kSourceTypesUnavailable;
            return result;
        }
        result.sourceTypesKnown = true;
        result.availableSourceTypes = sources.value;
        if ((sources.value & result.requestedSourceMask) == 0) {
            CleanupConnection(connection, nullptr, nullptr, false);
            result.outcome = Outcome::kUnsupported;
            result.reason = reason::kSourceTypeNotOffered;
            return result;
        }

        const PropertyAnswer cursorModes =
            ReadUint32Property(connection, kScreenCastInterface, "AvailableCursorModes",
                               automaticDeadline);
        result.cursorModesKnown = cursorModes.known;
        result.availableCursorModes = cursorModes.known ? cursorModes.value : 0;
        result.requestedCursorMode = request.cursorMode;
        result.cursorModeRequested = request.cursorMode != CursorMode::kDefault;
        if (result.cursorModeRequested) {
            const uint32_t wanted = static_cast<uint32_t>(request.cursorMode);
            if (!cursorModes.known || (cursorModes.value & wanted) == 0) {
                CleanupConnection(connection, nullptr, nullptr, false);
                result.outcome = Outcome::kUnsupported;
                result.reason = reason::kCursorModeUnavailable;
                return result;
            }
        }

        WaitContext ctx;
        dbus_connection_add_filter(connection, PortalFilter, &ctx, nullptr);
        const char* requestMatch =
            "type='signal',sender='org.freedesktop.portal.Desktop',"
            "interface='org.freedesktop.portal.Request',member='Response'";
        const char* sessionMatch =
            "type='signal',sender='org.freedesktop.portal.Desktop',"
            "interface='org.freedesktop.portal.Session',member='Closed'";
        const char* ownerMatch =
            "type='signal',sender='org.freedesktop.DBus',"
            "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='org.freedesktop.portal.Desktop'";
        dbus_bus_add_match(connection, requestMatch, &error);
        dbus_error_free(&error);
        dbus_bus_add_match(connection, sessionMatch, &error);
        dbus_error_free(&error);
        dbus_bus_add_match(connection, ownerMatch, &error);
        dbus_error_free(&error);

        const char* unique = dbus_bus_get_unique_name(connection);

        // 1) CreateSession。handle_token 与 session_handle_token 都预先生成；请求路径也先算好。
        const std::string createToken = RandomToken();
        const std::string sessionToken = RandomToken();
        const std::string expectedCreate = RequestPath(unique, createToken);
        const std::string expectedSession = SessionPath(unique, sessionToken);
        RequestTracker createTracker;
        createTracker.Expect(expectedCreate);
        ctx.tracker = &createTracker;

        DBusMessage* createCall =
            dbus_message_new_method_call(kPortalService, kPortalPath, kScreenCastInterface,
                                         "CreateSession");
        if (!createCall) {
            CleanupConnection(connection, &createTracker, &ctx, false);
            result.outcome = Outcome::kProtocolError;
            result.reason = reason::kProtocolError;
            return result;
        }
        DBusMessageIter root;
        DBusMessageIter dict;
        AppendOptionsStart(createCall, &root, &dict);
        AppendDictString(&dict, "handle_token", createToken);
        AppendDictString(&dict, "session_handle_token", sessionToken);
        FinishOptions(&root, &dict);

        bool createMethodOk = false;
        if (!InvokeRequest(connection, createCall, &ctx, &createTracker, automaticDeadline, false,
                           request.cancelToken, &createMethodOk)) {
            dbus_message_unref(createCall);
            CloseRequest(connection, createTracker.ActualPath().empty()
                                         ? createTracker.ExpectedPath()
                                         : createTracker.ActualPath());
            result.requestClosed = true;
            result.outcome = ClassifyFailure(&ctx, &createTracker, result.reason,
                                             request.cancelToken != nullptr &&
                                                 request.cancelToken->Cancelled());
            result.lateResponseIgnored = createTracker.LateResponseIgnored();
            CleanupConnection(connection, &createTracker, &ctx, false);
            return result;
        }
        dbus_message_unref(createCall);
        if (!createMethodOk ||
            createTracker.state() != RequestTracker::State::kSuccess) {
            CloseRequest(connection, createTracker.ActualPath().empty()
                                         ? createTracker.ExpectedPath()
                                         : createTracker.ActualPath());
            result.requestClosed = true;
            result.outcome = ClassifyFailure(&ctx, &createTracker, result.reason,
                                             request.cancelToken != nullptr &&
                                                 request.cancelToken->Cancelled());
            result.lateResponseIgnored = createTracker.LateResponseIgnored();
            CleanupConnection(connection, &createTracker, &ctx, false);
            return result;
        }

        const ResponseEnvelope* createResponse = FindResponse(&ctx, createTracker);
        std::string sessionHandle;
        if (!createResponse || !ResponseString(createResponse->message, "session_handle",
                                               &sessionHandle) ||
            sessionHandle.empty()) {
            result.outcome = Outcome::kProtocolError;
            result.reason = reason::kSessionMissing;
            CloseRequest(connection, createTracker.ActualPath());
            result.requestClosed = true;
            CleanupConnection(connection, &createTracker, &ctx, false);
            return result;
        }
        result.sessionCreated = true;
        result.requestCompleted = true;
        ctx.sessionHandle = sessionHandle;

        // 2) SelectSources：只请求单一来源类型，multiple=false，不带 restore_token / persist_mode。
        const std::string selectToken = RandomToken();
        const std::string expectedSelect = RequestPath(unique, selectToken);
        RequestTracker selectTracker;
        selectTracker.Expect(expectedSelect);
        ctx.tracker = &selectTracker;
        DBusMessage* selectCall =
            dbus_message_new_method_call(kPortalService, kPortalPath, kScreenCastInterface,
                                         "SelectSources");
        if (!selectCall) {
            CloseSession(connection, sessionHandle);
            result.sessionClosed = true;
            CleanupConnection(connection, &selectTracker, &ctx, false);
            result.outcome = Outcome::kProtocolError;
            result.reason = reason::kProtocolError;
            return result;
        }
        {
            const char* objectPath = sessionHandle.c_str();
            dbus_message_iter_init_append(selectCall, &root);
            dbus_message_iter_append_basic(&root, DBUS_TYPE_OBJECT_PATH, &objectPath);
            dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
            AppendDictString(&dict, "handle_token", selectToken);
            AppendDictUint32(&dict, "types", result.requestedSourceMask);
            AppendDictBool(&dict, "multiple", FALSE);
            if (result.cursorModeRequested) {
                AppendDictUint32(&dict, "cursor_mode",
                                 static_cast<uint32_t>(request.cursorMode));
            }
            FinishOptions(&root, &dict);
        }
        bool selectMethodOk = false;
        if (!InvokeRequest(connection, selectCall, &ctx, &selectTracker, automaticDeadline, false,
                           request.cancelToken, &selectMethodOk)) {
            dbus_message_unref(selectCall);
            CloseRequest(connection, selectTracker.ActualPath().empty()
                                         ? selectTracker.ExpectedPath()
                                         : selectTracker.ActualPath());
            CloseSession(connection, sessionHandle);
            result.requestClosed = true;
            result.sessionClosed = true;
            result.outcome = ClassifyFailure(&ctx, &selectTracker, result.reason,
                                             request.cancelToken != nullptr &&
                                                 request.cancelToken->Cancelled());
            result.lateResponseIgnored = selectTracker.LateResponseIgnored();
            CleanupConnection(connection, &selectTracker, &ctx, false);
            return result;
        }
        dbus_message_unref(selectCall);
        if (!selectMethodOk || selectTracker.state() != RequestTracker::State::kSuccess) {
            CloseRequest(connection, selectTracker.ActualPath().empty()
                                         ? selectTracker.ExpectedPath()
                                         : selectTracker.ActualPath());
            CloseSession(connection, sessionHandle);
            result.requestClosed = true;
            result.sessionClosed = true;
            result.outcome = ClassifyFailure(&ctx, &selectTracker, result.reason,
                                             request.cancelToken != nullptr &&
                                                 request.cancelToken->Cancelled());
            result.lateResponseIgnored = selectTracker.LateResponseIgnored();
            CleanupConnection(connection, &selectTracker, &ctx, false);
            return result;
        }
        result.requestCompleted = true;

        // 3) Start：这才进入系统授权 / 来源选择的人工等待。自动回复与人工交互分开计时。
        const std::string startToken = RandomToken();
        const std::string expectedStart = RequestPath(unique, startToken);
        RequestTracker startTracker;
        startTracker.Expect(expectedStart);
        ctx.tracker = &startTracker;
        DBusMessage* startCall =
            dbus_message_new_method_call(kPortalService, kPortalPath, kScreenCastInterface,
                                         "Start");
        if (!startCall) {
            CloseSession(connection, sessionHandle);
            result.sessionClosed = true;
            CleanupConnection(connection, &startTracker, &ctx, false);
            result.outcome = Outcome::kProtocolError;
            result.reason = reason::kProtocolError;
            return result;
        }
        {
            const char* objectPath = sessionHandle.c_str();
            const char* parent = request.parentWindow.c_str();
            dbus_message_iter_init_append(startCall, &root);
            dbus_message_iter_append_basic(&root, DBUS_TYPE_OBJECT_PATH, &objectPath);
            dbus_message_iter_append_basic(&root, DBUS_TYPE_STRING, &parent);
            dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
            AppendDictString(&dict, "handle_token", startToken);
            FinishOptions(&root, &dict);
        }
        const int64_t interactionDeadline =
            request.interactionTimeoutMs == 0
                ? 0
                : NowMs() + static_cast<int64_t>(request.interactionTimeoutMs);
        bool startMethodOk = false;
        const bool startWait = InvokeRequest(
            connection, startCall, &ctx, &startTracker, automaticDeadline, true,
            request.cancelToken, &startMethodOk, interactionDeadline);
        dbus_message_unref(startCall);
        if (!startWait || !startMethodOk ||
            startTracker.state() != RequestTracker::State::kSuccess) {
            const std::string requestPath = startTracker.ActualPath().empty()
                                                ? startTracker.ExpectedPath()
                                                : startTracker.ActualPath();
            // 取消 / 超时 / 未拿到可信成功响应：请求和已经创建的会话都要关闭。
            // 即使方法回复已经给出实际句柄，只要 Response 没有可核实成功，就主动结束交互；
            // Close() 本身不会再发 Response，因此不会产生一个可被后续误解的晚到成功。
            CloseRequest(connection, requestPath);
            CloseSession(connection, sessionHandle);
            result.requestClosed = true;
            result.sessionClosed = true;
            result.outcome = ClassifyFailure(
                &ctx, &startTracker, result.reason,
                request.cancelToken != nullptr && request.cancelToken->Cancelled(),
                startTracker.MethodReplied() ? reason::kInteractionTimeout
                                             : reason::kAutomaticTimeout);
            result.lateResponseIgnored = startTracker.LateResponseIgnored();
            CleanupConnection(connection, &startTracker, &ctx, false);
            return result;
        }

        const ResponseEnvelope* startResponse = FindResponse(&ctx, startTracker);
        if (!startResponse || !ParseStreams(startResponse->message, request.source,
                                            &result.selection, result.reason)) {
            CloseSession(connection, sessionHandle);
            result.sessionClosed = true;
            result.outcome = Outcome::kProtocolError;
            if (result.reason.empty() || result.reason == reason::kGranted)
                result.reason = reason::kProtocolError;
            CleanupConnection(connection, &startTracker, &ctx, false);
            return result;
        }
        result.requestCompleted = true;

        // 授权与来源选择成功。auth-only 动作按请求立即关闭；PipeWire 交付动作保留同一个
        // 会话，供 OpenPipeWireRemote() 使用。两种路径都不会留下“拿到了但没人负责”的连接。
        result.sessionClosedByPortal = ctx.sessionClosedByPortal;
        result.outcome = Outcome::kGranted;
        result.reason = reason::kGranted;
        result.pixelsRead = false;
        result.imageDelivered = false;
        result.restoreTokenPersisted = false;
        if (request.closeSessionOnSuccess) {
            CloseSession(connection, sessionHandle);
            result.sessionClosed = true;
            RemoveFilterAndMatches(connection, &ctx);
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
        } else {
            // 先把临时过滤器的回调引用彻底摘掉，再把连接移交给 heldConnection_。
            // 此后迟到信号不会再访问栈上的 ctx / tracker。
            RemoveFilterAndMatches(connection, &ctx);
            heldConnection_ = connection;
            heldSessionHandle_ = sessionHandle;
        }
        return result;
    }

    bool OpenPipeWireRemote(int* fd, uint64_t timeoutMs, std::string* reasonOut) override {
        if (fd) *fd = -1;
        if (!heldConnection_ || heldSessionHandle_.empty()) {
            if (reasonOut) *reasonOut = reason::kPipeWireRemoteUnavailable;
            return false;
        }
        DBusMessage* call = dbus_message_new_method_call(
            kPortalService, kPortalPath, kScreenCastInterface, "OpenPipeWireRemote");
        if (!call) {
            if (reasonOut) *reasonOut = reason::kProtocolError;
            return false;
        }
        DBusMessageIter root;
        DBusMessageIter dict;
        dbus_message_iter_init_append(call, &root);
        const char* session = heldSessionHandle_.c_str();
        dbus_message_iter_append_basic(&root, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dbus_message_iter_close_container(&root, &dict);

        DBusError error;
        dbus_error_init(&error);
        int waitMs = 5000;
        if (timeoutMs != 0) waitMs = static_cast<int>(std::min<uint64_t>(timeoutMs, 60000));
        DBusMessage* reply =
            dbus_connection_send_with_reply_and_block(heldConnection_, call, waitMs, &error);
        dbus_message_unref(call);
        if (!reply) {
            if (reasonOut) {
                *reasonOut = error.name && std::strstr(error.name, "ServiceUnknown")
                                 ? reason::kServiceLost
                                 : reason::kPipeWireRemoteUnavailable;
            }
            dbus_error_free(&error);
            return false;
        }
        if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
            if (reasonOut) *reasonOut = reason::kPipeWireRemoteUnavailable;
            dbus_message_unref(reply);
            return false;
        }
        DBusMessageIter it;
        if (!dbus_message_iter_init(reply, &it) ||
            dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UNIX_FD) {
            if (reasonOut) *reasonOut = reason::kProtocolError;
            dbus_message_unref(reply);
            return false;
        }
        int remote = -1;
        // libdbus 对 DBUS_TYPE_UNIX_FD 的 get_basic 会 dup 一份，所有权归调用方。
        dbus_message_iter_get_basic(&it, &remote);
        dbus_message_unref(reply);
        if (remote < 0) {
            if (reasonOut) *reasonOut = reason::kProtocolError;
            return false;
        }
        if (fd) *fd = remote;
        else close(remote);
        return true;
    }

    void CloseSession() override {
        if (!heldConnection_) return;
        if (!heldSessionHandle_.empty()) {
            CloseSession(heldConnection_, heldSessionHandle_);
        }
        dbus_connection_close(heldConnection_);
        dbus_connection_unref(heldConnection_);
        heldConnection_ = nullptr;
        heldSessionHandle_.clear();
    }

private:
    static bool InvokeRequest(DBusConnection* connection, DBusMessage* call, WaitContext* ctx,
                              RequestTracker* tracker, int64_t automaticDeadline,
                              bool hasInteractionPhase, CancelToken* cancelToken,
                              bool* methodOk, int64_t interactionDeadline = 0) {
        *methodOk = false;
        DBusPendingCall* pending = nullptr;
        if (!dbus_connection_send_with_reply(connection, call, &pending, -1) || !pending) {
            tracker->MarkProtocolError();
            return false;
        }
        dbus_connection_flush(connection);

        bool methodReplied = false;
        bool methodReplyOk = false;
        while (tracker->pending()) {
            if (cancelToken && cancelToken->Cancelled()) {
                tracker->MarkCancelled();
                break;
            }
            if (ctx->serviceLost) break;
            if (ctx->disconnected) break;
            if (ctx->sessionClosedByPortal) {
                tracker->MarkProtocolError();
                break;
            }
            const int64_t now = NowMs();
            const int64_t deadline = methodReplied && hasInteractionPhase
                                         ? interactionDeadline
                                         : automaticDeadline;
            if (deadline > 0 && now >= deadline) {
                tracker->MarkTimedOut();
                break;
            }
            int waitMs = 25;
            if (deadline > 0) {
                waitMs = static_cast<int>(std::min<int64_t>(deadline - now, 25));
                if (waitMs <= 0) {
                    tracker->MarkTimedOut();
                    break;
                }
            }
            if (!dbus_connection_read_write_dispatch(connection, waitMs)) {
                ctx->disconnected = true;
                break;
            }
            if (!methodReplied && dbus_pending_call_get_completed(pending)) {
                DBusMessage* reply = dbus_pending_call_steal_reply(pending);
                methodReplied = true;
                if (!reply) {
                    tracker->MarkProtocolError();
                    break;
                }
                if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
                    const char* errorName = dbus_message_get_error_name(reply);
                    const std::string name = errorName ? errorName : "";
                    dbus_message_unref(reply);
                    if (name.find("Cancelled") != std::string::npos) {
                        tracker->MarkCancelledOrDenied();
                    } else if (name.find("ServiceUnknown") != std::string::npos ||
                               name.find("NameHasNoOwner") != std::string::npos ||
                               name.find("Disconnected") != std::string::npos) {
                        ctx->serviceLost = true;
                    } else if (name.find("UnknownMethod") != std::string::npos ||
                               name.find("UnknownInterface") != std::string::npos ||
                               name.find("NotSupported") != std::string::npos) {
                        ctx->unsupported = true;
                        tracker->MarkProtocolError();
                    } else {
                        tracker->MarkProtocolError();
                    }
                    break;
                }
                std::string requestPath;
                if (!ParseObjectPathArgument(reply, &requestPath) || requestPath.empty()) {
                    dbus_message_unref(reply);
                    tracker->MarkProtocolError();
                    break;
                }
                dbus_message_unref(reply);
                tracker->OnMethodReply(requestPath);
                methodReplyOk = true;
                *methodOk = true;
                // 方法回复已到，但 Response 可能先到（tracker 已按实际路径配对）或还没到。
                // 后一种情况继续循环；Start 的 deadline 已切到人工交互预算。
            }
        }

        if (!methodReplied && pending) {
            dbus_pending_call_cancel(pending);
        }
        if (pending) {
            dbus_pending_call_unref(pending);
        }
        if (ctx->serviceLost) return false;
        if (ctx->disconnected) return false;
        if (!methodReplyOk) return false;
        return tracker->done();
    }

    static const ResponseEnvelope* FindResponse(WaitContext* ctx, const RequestTracker& tracker) {
        const std::string& path = tracker.MatchedResponsePath();
        if (path.empty()) return nullptr;
        auto it = ctx->responses.find(path);
        if (it == ctx->responses.end() || !it->second.message) return nullptr;
        return &it->second;
    }

    static bool ResponseString(DBusMessage* response, const char* key, std::string* out) {
        if (!response || !key || !out) return false;
        DBusMessageIter it;
        if (!dbus_message_iter_init(response, &it)) return false;
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
        if (!dbus_message_iter_next(&it)) return false;
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
        DBusMessageIter dict;
        dbus_message_iter_recurse(&it, &dict);
        DBusMessageIter variant;
        if (!DictFindVariant(&dict, key, &variant)) return false;
        return VariantString(&variant, out);
    }

    static bool ParseStreams(DBusMessage* response, SourceKind requested, Selection* selection,
                             std::string& reasonOut) {
        if (!response || !selection) {
            reasonOut = reason::kProtocolError;
            return false;
        }
        DBusMessageIter it;
        if (!dbus_message_iter_init(response, &it)) {
            reasonOut = reason::kProtocolError;
            return false;
        }
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
            reasonOut = reason::kProtocolError;
            return false;
        }
        if (!dbus_message_iter_next(&it) ||
            dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
            reasonOut = reason::kProtocolError;
            return false;
        }
        DBusMessageIter dict;
        dbus_message_iter_recurse(&it, &dict);
        DBusMessageIter streamsVariant;
        if (!DictFindVariant(&dict, "streams", &streamsVariant) ||
            dbus_message_iter_get_arg_type(&streamsVariant) != DBUS_TYPE_ARRAY) {
            reasonOut = reason::kProtocolError;
            return false;
        }

        DBusMessageIter streams;
        dbus_message_iter_recurse(&streamsVariant, &streams);
        selection->requested = requested;
        selection->streams.clear();
        while (dbus_message_iter_get_arg_type(&streams) == DBUS_TYPE_STRUCT) {
            DBusMessageIter item;
            dbus_message_iter_recurse(&streams, &item);
            StreamInfo stream;
            if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_UINT32) {
                dbus_message_iter_get_basic(&item, &stream.nodeId);
            }
            if (!dbus_message_iter_next(&item) ||
                dbus_message_iter_get_arg_type(&item) != DBUS_TYPE_ARRAY) {
                reasonOut = reason::kStreamSourceUnverifiable;
                return false;
            }
            DBusMessageIter props;
            dbus_message_iter_recurse(&item, &props);
            DBusMessageIter sourceTypeVariant;
            if (DictFindVariant(&props, "source_type", &sourceTypeVariant) &&
                dbus_message_iter_get_arg_type(&sourceTypeVariant) == DBUS_TYPE_UINT32) {
                uint32_t sourceType = 0;
                dbus_message_iter_get_basic(&sourceTypeVariant, &sourceType);
                if (sourceType == SourceKindMask(SourceKind::kWindow)) {
                    stream.sourceType = SourceKind::kWindow;
                    stream.sourceTypeKnown = true;
                } else if (sourceType == SourceKindMask(SourceKind::kMonitor)) {
                    stream.sourceType = SourceKind::kMonitor;
                    stream.sourceTypeKnown = true;
                } else if (sourceType == SourceKindMask(SourceKind::kVirtual)) {
                    stream.sourceType = SourceKind::kVirtual;
                    stream.sourceTypeKnown = true;
                }
            }
            DBusMessageIter positionVariant;
            if (DictFindVariant(&props, "position", &positionVariant) &&
                dbus_message_iter_get_arg_type(&positionVariant) == DBUS_TYPE_STRUCT) {
                DBusMessageIter pos;
                dbus_message_iter_recurse(&positionVariant, &pos);
                if (dbus_message_iter_get_arg_type(&pos) == DBUS_TYPE_INT32) {
                    dbus_message_iter_get_basic(&pos, &stream.x);
                    if (dbus_message_iter_next(&pos) &&
                        dbus_message_iter_get_arg_type(&pos) == DBUS_TYPE_INT32) {
                        dbus_message_iter_get_basic(&pos, &stream.y);
                        stream.positionKnown = true;
                    }
                }
            }
            DBusMessageIter sizeVariant;
            if (DictFindVariant(&props, "size", &sizeVariant) &&
                dbus_message_iter_get_arg_type(&sizeVariant) == DBUS_TYPE_STRUCT) {
                DBusMessageIter size;
                dbus_message_iter_recurse(&sizeVariant, &size);
                if (dbus_message_iter_get_arg_type(&size) == DBUS_TYPE_INT32) {
                    dbus_message_iter_get_basic(&size, &stream.width);
                    if (dbus_message_iter_next(&size) &&
                        dbus_message_iter_get_arg_type(&size) == DBUS_TYPE_INT32) {
                        dbus_message_iter_get_basic(&size, &stream.height);
                        stream.sizeKnown = true;
                    }
                }
            }
            DBusMessageIter serialVariant;
            if (DictFindVariant(&props, "pipewire-serial", &serialVariant) &&
                dbus_message_iter_get_arg_type(&serialVariant) == DBUS_TYPE_UINT64) {
                dbus_message_iter_get_basic(&serialVariant, &stream.pipewireSerial);
                stream.pipewireSerialKnown = true;
            }
            selection->streams.push_back(stream);
            if (!dbus_message_iter_next(&streams)) break;
        }

        if (selection->streams.size() != 1) {
            reasonOut = reason::kMultipleStreams;
            return false;
        }
        const StreamInfo& stream = selection->streams.front();
        if (!stream.sourceTypeKnown) {
            reasonOut = reason::kStreamSourceUnverifiable;
            return false;
        }
        if (stream.sourceType != requested) {
            reasonOut = reason::kSourceMismatch;
            return false;
        }
        selection->exactlyOneStream = true;
        selection->sourceTypeVerified = true;
        return true;
    }

    static Outcome ClassifyFailure(WaitContext* ctx, RequestTracker* tracker,
                                   std::string& reasonOut, bool appCancelled,
                                   const char* timeoutReason = reason::kAutomaticTimeout) {
        if (ctx->serviceLost) {
            reasonOut = reason::kServiceLost;
            return Outcome::kServiceLost;
        }
        if (ctx->disconnected) {
            reasonOut = reason::kServiceLost;
            return Outcome::kServiceLost;
        }
        if (ctx->unsupported) {
            reasonOut = reason::kInterfaceMissing;
            return Outcome::kUnsupported;
        }
        if (appCancelled) {
            reasonOut = reason::kAppCancelled;
            return Outcome::kCancelled;
        }
        switch (tracker->state()) {
            case RequestTracker::State::kCancelledOrDenied:
                reasonOut = reason::kCancelledOrDenied;
                return Outcome::kCancelledOrDenied;
            case RequestTracker::State::kTimedOut:
                // 调用点按失败阶段交来准确 token：方法回复前的自动预算，还是 Start 已进入
                // 人工选择后的 --portal-timeout-ms。两者都映射到 kTimedOut，但原因分开写。
                reasonOut = timeoutReason ? timeoutReason : reason::kAutomaticTimeout;
                return Outcome::kTimedOut;
            case RequestTracker::State::kCancelled:
                reasonOut = reason::kAppCancelled;
                return Outcome::kCancelled;
            case RequestTracker::State::kRequestFailed:
                reasonOut = reason::kRequestFailed;
                return Outcome::kRequestFailed;
            case RequestTracker::State::kProtocolError:
                reasonOut = reason::kProtocolError;
                return Outcome::kProtocolError;
            case RequestTracker::State::kSuccess:
            case RequestTracker::State::kPending:
                break;
        }
        reasonOut = reason::kProtocolError;
        return Outcome::kProtocolError;
    }

    static void CloseRequest(DBusConnection* connection, const std::string& path) {
        if (!connection || path.empty()) return;
        DBusMessage* close = dbus_message_new_method_call(
            kPortalService, path.c_str(), kRequestInterface, "Close");
        if (!close) return;
        dbus_connection_send(connection, close, nullptr);
        dbus_message_unref(close);
        dbus_connection_flush(connection);
    }

    static void CloseSession(DBusConnection* connection, const std::string& path) {
        if (!connection || path.empty()) return;
        DBusMessage* close = dbus_message_new_method_call(
            kPortalService, path.c_str(), kSessionInterface, "Close");
        if (!close) return;
        dbus_connection_send(connection, close, nullptr);
        dbus_message_unref(close);
        dbus_connection_flush(connection);
    }

    static void RemoveFilterAndMatches(DBusConnection* connection, WaitContext* ctx) {
        if (!connection) return;
        dbus_connection_remove_filter(connection, PortalFilter, ctx);
        DBusError error;
        dbus_error_init(&error);
        dbus_bus_remove_match(
            connection,
            "type='signal',sender='org.freedesktop.portal.Desktop',"
            "interface='org.freedesktop.portal.Request',member='Response'",
            &error);
        dbus_error_free(&error);
        dbus_bus_remove_match(
            connection,
            "type='signal',sender='org.freedesktop.portal.Desktop',"
            "interface='org.freedesktop.portal.Session',member='Closed'",
            &error);
        dbus_error_free(&error);
        dbus_bus_remove_match(
            connection,
            "type='signal',sender='org.freedesktop.DBus',"
            "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
            "arg0='org.freedesktop.portal.Desktop'",
            &error);
        dbus_error_free(&error);
    }

    static void CleanupConnection(DBusConnection* connection, RequestTracker* tracker,
                                  WaitContext* ctx, bool completedSuccessfully) {
        (void)tracker;
        (void)completedSuccessfully;
        if (!connection) return;
        RemoveFilterAndMatches(connection, ctx);
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
    }

    DBusConnection* heldConnection_ = nullptr;
    std::string heldSessionHandle_;
};

#endif  // ECAPTURE_HAVE_DBUS

}  // namespace

std::unique_ptr<IPortalScreenCast> CreateDBusPortalScreenCast() {
#if defined(ECAPTURE_HAVE_DBUS)
    return std::make_unique<DBusPortalScreenCast>();
#else
    return std::make_unique<UnavailablePortalScreenCast>();
#endif
}

}  // namespace portal
}  // namespace ecapture
