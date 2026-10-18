// 【平台层 · Linux】会话探测：把"这台机器现在是什么会话、哪条服务真的连得上"收成事实。
//
// 三条硬边界（与 AGENTS.md《Wayland 提示与系统权限》《输出、数据与兼容性》同源）：
//   1. **环境变量只是线索，不是证据。** DISPLAY / WAYLAND_DISPLAY / XDG_SESSION_TYPE /
//      桌面名称都可能存在而服务根本连不上；本文件只在实际 connect 成功后才写 connected。
//      原值一个都不进报告，只报"设没设"与归一化后的会话类型。
//   2. **只读。** 不选 Portal 来源、不创建 ScreenCast 会话、不读像素、不弹框、不启动
//      dbus-daemon / xdg-desktop-portal / pipewire、不安装依赖、不提权。D-Bus 用
//      dbus_connection_open_private（绝不走 dbus_bus_get 的自动拉起），因此没有会话总线时
//      只会得到"服务不存在"，不会顺手造一个。
//   3. **问不出来就是 unknown。** 缺库（本构建没链 xcb / wayland-client / dbus-1 /
//      libpipewire）时 compiled=false、reachable=unknown；连接失败带 errno 原值，但不把
//      原生错误码当成跨平台分支依据。
//
// 这里没有任何截图实现：X11 扩展、Wayland 全局接口、Portal 版本与来源类型、PipeWire
// 套接字都只是"接入前提"，不是"可以截图"的证明。

#include "SessionProbe.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#if defined(ECAPTURE_HAVE_XCB)
#include <xcb/xcb.h>
#endif

#if defined(ECAPTURE_HAVE_WAYLAND)
#include <wayland-client.h>
#endif

#if defined(ECAPTURE_HAVE_DBUS)
#include <dbus/dbus.h>
#endif

namespace ecapture {
namespace {

const char* EnvRaw(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? value : nullptr;
}

bool EnvSet(const char* name) { return EnvRaw(name) != nullptr; }

std::wstring NormalizeSessionType(const char* raw) {
    if (!raw) return L"unknown";
    std::string lower;
    lower.reserve(std::strlen(raw));
    for (const char* p = raw; *p; ++p) {
        const char c = *p;
        lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (lower == "wayland") return L"wayland";
    if (lower == "x11") return L"x11";
    if (lower == "tty") return L"tty";
    return L"unknown";
}

SessionEnvironmentFacts ProbeEnvironment() {
    SessionEnvironmentFacts env;
    const char* sessionType = EnvRaw("XDG_SESSION_TYPE");
    env.xdgSessionTypeSet = sessionType != nullptr;
    env.xdgSessionType = NormalizeSessionType(sessionType);
    env.displaySet = EnvSet("DISPLAY");
    env.waylandDisplaySet = EnvSet("WAYLAND_DISPLAY");
    env.sshConnectionSet = EnvSet("SSH_CONNECTION");
    env.sshTtySet = EnvSet("SSH_TTY");
    env.runtimeDirSet = EnvSet("XDG_RUNTIME_DIR");
    env.dbusAddressSet = EnvSet("DBUS_SESSION_BUS_ADDRESS");
    // 桌面名称读到了也不当证据：这里只留一个"读到过"，报告里另有 caveat 钉住它。
    env.desktopNameSet = EnvSet("XDG_CURRENT_DESKTOP") || EnvSet("DESKTOP_SESSION");
    return env;
}

#if defined(ECAPTURE_HAVE_XCB)
X11Facts ProbeX11(const SessionEnvironmentFacts& env) {
    X11Facts facts;
    facts.compiled = true;
    facts.displayConfigured = env.displaySet;
    if (!env.displaySet) return facts;

    facts.attempted = true;
    int screen = 0;
    xcb_connection_t* conn = xcb_connect(nullptr, &screen);
    if (!conn) {
        return facts;
    }
    const int error = xcb_connection_has_error(conn);
    if (error != 0) {
        facts.connectError = error;
        xcb_disconnect(conn);
        return facts;
    }

    facts.connected = true;
    const xcb_setup_t* setup = xcb_get_setup(conn);
    if (setup) {
        facts.protocolMajor = setup->protocol_major_version;
        facts.protocolMinor = setup->protocol_minor_version;
        facts.screenCount = xcb_setup_roots_length(setup);
    }

    // 服务端声明的扩展名：只读一次列表，不调用任何扩展。XWayland 用它自己的 "XWAYLAND"
    // 扩展把自己与独立 Xorg 分开；没有这个扩展时不硬说"一定是 Xorg"。
    xcb_list_extensions_cookie_t cookie = xcb_list_extensions(conn);
    xcb_list_extensions_reply_t* reply = xcb_list_extensions_reply(conn, cookie, nullptr);
    if (reply) {
        const int count = xcb_list_extensions_names_length(reply);
        xcb_str_iterator_t it = xcb_list_extensions_names_iterator(reply);
        for (int i = 0; i < count && it.rem; ++i, xcb_str_next(&it)) {
            const int len = xcb_str_name_length(it.data);
            const char* name = xcb_str_name(it.data);
            if (len > 0) {
                facts.extensions.emplace_back(name, name + static_cast<size_t>(len));
                if (len == 8 && std::memcmp(name, "XWAYLAND", 8) == 0) {
                    facts.xwaylandExtension = true;
                }
            }
        }
        std::free(reply);
    }

    xcb_disconnect(conn);
    return facts;
}
#else
X11Facts ProbeX11(const SessionEnvironmentFacts&) {
    X11Facts facts;
    facts.compiled = false;
    return facts;
}
#endif

#if defined(ECAPTURE_HAVE_WAYLAND)
struct RegistryScan {
    WaylandFacts* facts = nullptr;
};

void RegistryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface,
                    uint32_t version) {
    (void)registry;
    (void)name;
    (void)version;
    auto* scan = static_cast<RegistryScan*>(data);
    if (!scan || !scan->facts || !interface) return;
    const std::string iface(interface);
    scan->facts->interfaces.emplace_back(iface.begin(), iface.end());
    if (iface == "wl_compositor") scan->facts->hasCompositor = true;
    else if (iface == "wl_shm") scan->facts->hasShm = true;
    else if (iface == "wl_output") ++scan->facts->outputCount;
    // wlr-screencopy 与 ext-image-copy-capture 都只是"合成器露出过这个接口"；
    // 它们不是 Portal 授权，也不是本构建已经实现了采集。
    else if (iface.find("screencopy") != std::string::npos) scan->facts->hasScreencopy = true;
    else if (iface.find("image_copy_capture") != std::string::npos)
        scan->facts->hasImageCopyCapture = true;
}

void RegistryGlobalRemove(void* data, wl_registry* registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

const wl_registry_listener kRegistryListener = {RegistryGlobal, RegistryGlobalRemove};

WaylandFacts ProbeWayland(const SessionEnvironmentFacts& env) {
    WaylandFacts facts;
    facts.compiled = true;
    facts.displayConfigured = env.waylandDisplaySet;
    if (!env.waylandDisplaySet) return facts;

    facts.attempted = true;
    // wl_display_connect(nullptr) 走 WAYLAND_DISPLAY（相对名解析到 XDG_RUNTIME_DIR），
    // 不会启动合成器；连不上就返回空指针。
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        facts.connectError = errno;
        return facts;
    }

    facts.connected = true;
    wl_registry* registry = wl_display_get_registry(display);
    if (registry) {
        RegistryScan scan{&facts};
        wl_registry_add_listener(registry, &kRegistryListener, &scan);
        const int roundTrip = wl_display_roundtrip(display);
        facts.registryRoundTripOk = roundTrip >= 0;
        if (roundTrip < 0) {
            facts.connectError = errno;
        }
    }
    wl_display_disconnect(display);
    return facts;
}
#else
WaylandFacts ProbeWayland(const SessionEnvironmentFacts&) {
    WaylandFacts facts;
    facts.compiled = false;
    return facts;
}
#endif

#if defined(ECAPTURE_HAVE_DBUS)
// 只读一个 uint32 属性。三种下场分开：属性存在并读到（true + known=true）；
// 接口/属性不存在（false + known=true）；这一问失败（false + known=false）。
// 绝不在这里 CreateSession / SelectSources / OpenPipeWireRemote。
bool ReadUint32Property(DBusConnection* conn, const char* interface, const char* property,
                        bool* known, uint32_t* value) {
    *known = false;
    *value = 0;
    DBusError error;
    dbus_error_init(&error);
    DBusMessage* request = dbus_message_new_method_call(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.DBus.Properties", "Get");
    if (!request) return false;
    const char* iface = interface;
    const char* prop = property;
    dbus_message_append_args(request, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
                             DBUS_TYPE_INVALID);
    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(conn, request, 1500, &error);
    dbus_message_unref(request);
    if (!reply) {
        if (dbus_error_is_set(&error)) {
            const bool knownMissing =
                std::strcmp(error.name, "org.freedesktop.DBus.Error.UnknownInterface") == 0 ||
                std::strcmp(error.name, "org.freedesktop.DBus.Error.UnknownProperty") == 0 ||
                std::strcmp(error.name, "org.freedesktop.DBus.Error.InvalidArgs") == 0;
            *known = knownMissing;
        }
        dbus_error_free(&error);
        return false;
    }
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char* name = dbus_message_get_error_name(reply);
        *known = name && (std::strcmp(name, "org.freedesktop.DBus.Error.UnknownInterface") == 0 ||
                          std::strcmp(name, "org.freedesktop.DBus.Error.UnknownProperty") == 0);
        dbus_message_unref(reply);
        return false;
    }

    DBusMessageIter it;
    if (!dbus_message_iter_init(reply, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply);
        return false;
    }
    DBusMessageIter variant;
    dbus_message_iter_recurse(&it, &variant);
    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32) {
        dbus_message_iter_get_basic(&variant, value);
        *known = true;
        dbus_message_unref(reply);
        return true;
    }
    dbus_message_unref(reply);
    return false;
}

PortalFacts ProbePortal(const SessionEnvironmentFacts& env) {
    PortalFacts facts;
    facts.compiled = true;
    const char* address = EnvRaw("DBUS_SESSION_BUS_ADDRESS");
    std::string derived;
    if (!address) {
        const char* runtimeDir = EnvRaw("XDG_RUNTIME_DIR");
        if (runtimeDir) {
            derived = "unix:path=";
            derived += runtimeDir;
            derived += "/bus";
            address = derived.c_str();
            facts.addressConfigured = true;
        }
    } else {
        facts.addressConfigured = true;
        // 自动拉起地址（autolaunch:）意味着"没有现成的会话总线"：本查询不启动服务。
        if (std::strncmp(address, "autolaunch:", 11) == 0) {
            facts.addressConfigured = false;
            address = nullptr;
        }
    }
    if (!address) return facts;

    facts.attempted = true;
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* conn = dbus_connection_open_private(address, &error);
    if (!conn) {
        facts.connectError = 0;   // DBusError 不是 errno；不伪造一个数值
        dbus_error_free(&error);
        return facts;
    }
    // 注册（发 Hello）只是拿一个唯一名，不启动任何服务；失败说明这条地址不是一个活总线。
    if (!dbus_bus_register(conn, &error)) {
        facts.connectError = 0;
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return facts;
    }
    facts.busConnected = true;

    const dbus_bool_t owner =
        dbus_bus_name_has_owner(conn, "org.freedesktop.portal.Desktop", &error);
    if (!dbus_error_is_set(&error)) {
        facts.serviceOwnerKnown = true;
        facts.serviceOwner = owner != FALSE;
    }
    dbus_error_free(&error);

    bool present = false;
    uint32_t version = 0;
    bool known = false;
    present = ReadUint32Property(conn, "org.freedesktop.portal.ScreenCast", "version", &known,
                                 &version);
    facts.screenCastInterfaceKnown = known;
    facts.screenCastInterface = present;
    facts.screenCastVersionKnown = present;
    facts.screenCastVersion = present ? version : 0;

    present = ReadUint32Property(conn, "org.freedesktop.portal.Screenshot", "version", &known,
                                 &version);
    facts.screenshotInterfaceKnown = known;
    facts.screenshotInterface = present;
    facts.screenshotVersionKnown = present;
    facts.screenshotVersion = present ? version : 0;

    uint32_t mask = 0;
    facts.sourceTypesKnown =
        ReadUint32Property(conn, "org.freedesktop.portal.ScreenCast", "AvailableSourceTypes",
                           &known, &mask) &&
        known;
    facts.sourceTypesMask = facts.sourceTypesKnown ? mask : 0;

    uint32_t cursors = 0;
    facts.cursorModesKnown =
        ReadUint32Property(conn, "org.freedesktop.portal.ScreenCast", "AvailableCursorModes",
                           &known, &cursors) &&
        known;
    facts.cursorModesMask = facts.cursorModesKnown ? cursors : 0;

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return facts;
}
#else
PortalFacts ProbePortal(const SessionEnvironmentFacts&) {
    PortalFacts facts;
    facts.compiled = false;
    return facts;
}
#endif

PipeWireFacts ProbePipeWire(const SessionEnvironmentFacts& env) {
    PipeWireFacts facts;
#if defined(ECAPTURE_HAVE_PIPEWIRE)
    facts.compiled = true;
#else
    facts.compiled = false;
#endif
    facts.runtimeDirConfigured = env.runtimeDirSet;
    if (env.runtimeDirSet) {
        const char* runtimeDir = EnvRaw("XDG_RUNTIME_DIR");
        // 只 stat 一个固定名字的套接字，不遍历目录、不连协议、不启动 pipewire。
        std::string socketPath;
        if (runtimeDir) {
            socketPath = runtimeDir;
            if (!socketPath.empty() && socketPath.back() != '/') socketPath.push_back('/');
            socketPath += "pipewire-0";
            struct stat st {};
            facts.socketPresentKnown = true;
            facts.socketPresent = ::stat(socketPath.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
        }
    }
    if (facts.compiled && facts.socketPresentKnown && !facts.socketPresent) {
    } else if (facts.compiled && !facts.runtimeDirConfigured) {
    }
    return facts;
}

}  // namespace

SessionFacts ProbeSessionFacts() {
    SessionFacts facts;
    facts.environment = ProbeEnvironment();
    facts.x11 = ProbeX11(facts.environment);
    facts.wayland = ProbeWayland(facts.environment);
    facts.portal = ProbePortal(facts.environment);
    facts.pipewire = ProbePipeWire(facts.environment);
    // 这三条是探测本身的保证：只读查询不选来源、不建会话、不读像素。
    facts.sourceSelectionRequested = false;
    facts.captureSessionCreated = false;
    facts.screenPixelsRead = false;
    return facts;
}

}  // namespace ecapture
