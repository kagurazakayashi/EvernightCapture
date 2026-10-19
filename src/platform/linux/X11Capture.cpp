#include "platform/linux/X11Capture.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "CaptureDiag.h"
#include "Deadline.h"
#include "Lang.h"
#include "core/TextCoding.h"

namespace ecapture {
namespace x11 {
namespace {

thread_local int g_trapError = 0;

int TrapHandler(Display*, XErrorEvent* event) {
    // 只留第一个错误：一次调用里后续错误多半是第一个的连锁反应，拿最后一个会把原因说反。
    if (g_trapError == 0) g_trapError = event->error_code;
    return 0;
}

std::wstring W(const char* text) {
    if (!text) return std::wstring();
    return DecodeUtf8(text, std::strlen(text)).text;
}

// X11 的错误码 -> 稳定英文 token（进 detail；不本地化，四种语言共用）。
const char* XErrorToken(int code) {
    switch (code) {
        case Success: return "success";
        case BadRequest: return "BadRequest";
        case BadValue: return "BadValue";
        case BadWindow: return "BadWindow";
        case BadPixmap: return "BadPixmap";
        case BadAtom: return "BadAtom";
        case BadCursor: return "BadCursor";
        case BadFont: return "BadFont";
        case BadMatch: return "BadMatch";
        case BadDrawable: return "BadDrawable";
        case BadAccess: return "BadAccess";
        case BadAlloc: return "BadAlloc";
        case BadColor: return "BadColor";
        case BadGC: return "BadGC";
        case BadIDChoice: return "BadIDChoice";
        case BadName: return "BadName";
        case BadLength: return "BadLength";
        case BadImplementation: return "BadImplementation";
        default: return "unknown";
    }
}

void SetFailure(Diagnostic* err, const wchar_t* code, const wchar_t* messageKey,
                const wchar_t* hintKey, const std::wstring& value,
                const std::string& detail = std::string()) {
    if (!err) return;
    err->code = code;
    err->option = L"--capture";
    err->message = Msg(messageKey);
    if (hintKey) err->hint = Msg(hintKey);
    err->value = value;
    err->backend = value;
    err->stage = stages::kCapture;
    if (!detail.empty()) err->detail = W(detail.c_str());
}

// 一条属性的原始字节。type/format 交回给调用方判类型；失败时 data 为空。
struct Property {
    bool read = false;
    int xError = 0;
    Atom type = 0;
    int format = 0;
    unsigned long items = 0;
    std::vector<unsigned char> bytes;
};

Property GetProperty(Display* display, Window window, const char* name) {
    Property prop;
    const Atom atom = XInternAtom(display, name, False);
    if (atom == None) return prop;

    // 256 KiB 足够装下一扇窗口的标题 / 类名 / 状态列表；超出的部分按截断处理，
    // 不为了一个异常长的标题把整份属性反复重取（那会让一次枚举变成多次同步往返）。
    constexpr long kLongLength = 256 * 1024 / 4;
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long count = 0;
    unsigned long after = 0;
    unsigned char* data = nullptr;

    ErrorTrap trap(display);
    const int status = XGetWindowProperty(display, window, atom, 0, kLongLength, False, AnyPropertyType,
                                          &actualType, &actualFormat, &count, &after, &data);
    prop.xError = trap.Sync();
    if (status != Success || prop.xError != 0) {
        if (data) XFree(data);
        return prop;
    }
    // XGetWindowProperty 对"属性不存在"也返回 Success：actualType == None、format == 0、
    // count == 0。把这种回答当成"读到了"会让 IsClientWindow 把每一层装饰框都认成客户端，
    // 枚举就停在最外层、真正的窗口反而列不出来。
    prop.read = actualType != None;
    prop.type = actualType;
    prop.format = actualFormat;
    prop.items = count;
    if (data && actualFormat > 0) {
        const std::size_t bytes = static_cast<std::size_t>(count) * static_cast<std::size_t>(actualFormat / 8);
        prop.bytes.assign(data, data + bytes);
    }
    if (data) XFree(data);
    return prop;
}

bool ReadCardinal(Display* display, Window window, const char* name, uint32_t* out,
                  ReadState* state, uint32_t* errorCode) {
    const Property prop = GetProperty(display, window, name);
    if (!prop.read) {
        *state = ReadState::kFailed;
        *errorCode = static_cast<uint32_t>(prop.xError);
        return false;
    }
    if (prop.format != 32 || prop.items == 0 || prop.bytes.size() < 4) {
        // 属性存在但形状不是 CARDINAL：这是"问到了，但读不懂"，仍按失败处理。
        *state = ReadState::kFailed;
        *errorCode = 0;
        return false;
    }
    uint32_t value = 0;
    std::memcpy(&value, prop.bytes.data(), sizeof(value));
    *out = value;
    *state = ReadState::kReadable;
    *errorCode = 0;
    return true;
}

bool ReadWindowList(Display* display, Window window, const char* name, std::vector<uint32_t>* out) {
    const Property prop = GetProperty(display, window, name);
    if (!prop.read || prop.format != 32) return false;
    out->clear();
    out->reserve(prop.items);
    for (unsigned long i = 0; i < prop.items; ++i) {
        uint32_t value = 0;
        std::memcpy(&value, prop.bytes.data() + i * 4, sizeof(value));
        out->push_back(value);
    }
    return true;
}

std::wstring ReadWindowTitle(Display* display, Window window, ReadState* state) {
    // EWMH 的 _NET_WM_NAME 是 UTF-8；没有它才退回 ICCCM 的 WM_NAME（按 UTF-8 解，
    // 解不动就留替换字符）。不调用 setlocale，也不让标题是否可读取决于进程 locale。
    const Property net = GetProperty(display, window, "_NET_WM_NAME");
    if (net.read && net.format == 8 && !net.bytes.empty()) {
        *state = ReadState::kReadable;
        return DecodeUtf8(reinterpret_cast<const char*>(net.bytes.data()), net.bytes.size()).text;
    }
    const Property legacy = GetProperty(display, window, "WM_NAME");
    if (legacy.read && legacy.format == 8) {
        *state = ReadState::kReadable;
        if (legacy.bytes.empty()) return std::wstring();
        return DecodeUtf8(reinterpret_cast<const char*>(legacy.bytes.data()), legacy.bytes.size()).text;
    }
    *state = ReadState::kFailed;
    return std::wstring();
}

std::wstring ReadWindowClass(Display* display, Window window, ReadState* state) {
    XClassHint hint{};
    ErrorTrap trap(display);
    const int ok = XGetClassHint(display, window, &hint);
    const int error = trap.Sync();
    if (ok == 0 || error != 0) {
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        *state = ReadState::kFailed;
        return std::wstring();
    }
    std::wstring result;
    if (hint.res_class && *hint.res_class) result = W(hint.res_class);
    else if (hint.res_name && *hint.res_name) result = W(hint.res_name);
    if (hint.res_name) XFree(hint.res_name);
    if (hint.res_class) XFree(hint.res_class);
    *state = result.empty() ? ReadState::kFailed : ReadState::kReadable;
    return result;
}

// /proc/<pid>/stat 的 starttime（自开机起的时钟滴答数）-> 100ns ticks。
// 用同一个换算给枚举与复核两条路，所以两个值可以直接比。
bool ReadProcessStart(uint32_t pid, uint64_t* ticks, uint32_t* errorCode) {
    if (pid == 0) {
        *errorCode = 0;
        return false;
    }
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        *errorCode = static_cast<uint32_t>(errno);
        return false;
    }
    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
        if (text.size() > 8192) break;
    }
    std::fclose(file);

    // comm 里可以有空格与右括号，所以从最后一个 ')' 之后开始数。字段 3 是 state，
    // starttime 是字段 22，也就是 comm 之后的第 20 个 token（下标 19）。
    const std::size_t close = text.rfind(')');
    if (close == std::string::npos) {
        *errorCode = 0;
        return false;
    }
    std::size_t pos = close + 1;
    for (int field = 3; field <= 22; ++field) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
        if (pos >= text.size()) {
            *errorCode = 0;
            return false;
        }
        const std::size_t begin = pos;
        while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n') ++pos;
        if (field == 22) {
            unsigned long long value = 0;
            for (std::size_t i = begin; i < pos; ++i) {
                if (text[i] < '0' || text[i] > '9') {
                    *errorCode = 0;
                    return false;
                }
                value = value * 10 + static_cast<unsigned long long>(text[i] - '0');
            }
            const long hz = sysconf(_SC_CLK_TCK);
            if (hz <= 0) {
                *errorCode = 0;
                return false;
            }
            *ticks = value * 10000000ull / static_cast<uint64_t>(hz);
            *errorCode = 0;
            return true;
        }
    }
    *errorCode = 0;
    return false;
}

void ReadProcessPath(uint32_t pid, std::wstring* path, std::wstring* name, ReadState* state,
                     uint32_t* errorCode) {
    if (pid == 0) {
        *state = ReadState::kFailed;
        *errorCode = 0;
        return;
    }
    const std::string link = "/proc/" + std::to_string(pid) + "/exe";
    char buffer[4096];
    const ssize_t n = readlink(link.c_str(), buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        *state = ReadState::kFailed;
        *errorCode = static_cast<uint32_t>(errno);
        return;
    }
    buffer[n] = '\0';
    *path = W(buffer);
    const std::size_t slash = path->find_last_of(L'/');
    *name = slash == std::wstring::npos ? *path : path->substr(slash + 1);
    *state = ReadState::kReadable;
    *errorCode = 0;
}

bool QueryRect(Display* display, Window window, int32_t* x, int32_t* y, uint32_t* width,
               uint32_t* height, uint32_t* errorCode) {
    XWindowAttributes attrs{};
    ErrorTrap trap(display);
    const int ok = XGetWindowAttributes(display, window, &attrs);
    int error = trap.Sync();
    if (ok == 0 || error != 0) {
        *errorCode = static_cast<uint32_t>(error);
        return false;
    }
    int rootX = 0;
    int rootY = 0;
    Window child = None;
    ErrorTrap translateTrap(display);
    const int translated = XTranslateCoordinates(display, window, attrs.root, 0, 0, &rootX, &rootY,
                                                  &child);
    error = translateTrap.Sync();
    if (translated == 0 || error != 0) {
        *errorCode = static_cast<uint32_t>(error);
        return false;
    }
    *x = rootX;
    *y = rootY;
    *width = static_cast<uint32_t>(std::max(0, attrs.width));
    *height = static_cast<uint32_t>(std::max(0, attrs.height));
    *errorCode = 0;
    return true;
}

bool ReadFacts(Display* display, Window window, ClientWindowFacts* out) {
    XWindowAttributes attrs{};
    {
        ErrorTrap trap(display);
        const int ok = XGetWindowAttributes(display, window, &attrs);
        const int error = trap.Sync();
        if (ok == 0 || error != 0) {
            out->rectRead = ReadState::kFailed;
            out->rectErrno = static_cast<uint32_t>(error);
            return false;
        }
    }
    out->window = window;
    out->viewable = attrs.map_state == IsViewable;

    ReadState state = ReadState::kFailed;
    uint32_t errorCode = 0;
    ReadCardinal(display, window, "_NET_WM_PID", &out->pid, &out->pidRead, &errorCode);
    if (out->pidRead == ReadState::kReadable) {
        out->startRead = ReadProcessStart(out->pid, &out->processStartTicks, &out->startErrno)
                             ? ReadState::kReadable
                             : ReadState::kFailed;
        ReadProcessPath(out->pid, &out->imagePath, &out->imageName, &state, &errorCode);
        out->pathRead = state;
        out->pathErrno = errorCode;
    }

    out->title = ReadWindowTitle(display, window, &out->titleRead);
    out->className = ReadWindowClass(display, window, &out->classRead);

    // 最小化：EWMH 的 _NET_WM_STATE_HIDDEN 或 ICCCM 的 WM_STATE == IconicState。
    out->iconic = false;
    std::vector<uint32_t> states;
    if (ReadWindowList(display, window, "_NET_WM_STATE", &states)) {
        const Atom hidden = XInternAtom(display, "_NET_WM_STATE_HIDDEN", True);
        for (uint32_t value : states) {
            if (hidden != None && static_cast<Atom>(value) == hidden) {
                out->iconic = true;
                break;
            }
        }
    }
    if (!out->iconic) {
        const Property wmState = GetProperty(display, window, "WM_STATE");
        if (wmState.read && wmState.format == 32 && wmState.items > 0) {
            uint32_t value = 0;
            std::memcpy(&value, wmState.bytes.data(), sizeof(value));
            out->iconic = value == IconicState;
        }
    }

    uint32_t rectError = 0;
    out->rectRead = QueryRect(display, window, &out->x, &out->y, &out->width, &out->height,
                              &rectError)
                        ? ReadState::kReadable
                        : ReadState::kFailed;
    out->rectErrno = rectError;
    return true;
}

bool IsClientWindow(Display* display, Window window) {
    // 有 WM_STATE / WM_CLASS 的就是客户端窗口；窗口管理器的装饰框两者都没有。
    const Property wmState = GetProperty(display, window, "WM_STATE");
    if (wmState.read) return true;
    const Property wmClass = GetProperty(display, window, "WM_CLASS");
    return wmClass.read;
}

void CollectTopLevel(Display* display, Window root, std::vector<ClientWindowFacts>* out,
                     int* zOrder, int depth) {
    if (depth > 8) return;  // 装饰框最多几层；更深的递归只可能是坏了或恶意构造的树
    Window parent = None;
    Window* children = nullptr;
    unsigned int count = 0;
    ErrorTrap trap(display);
    const int ok = XQueryTree(display, root, &root, &parent, &children, &count);
    const int error = trap.Sync();
    if (ok == 0 || error != 0) {
        if (children) XFree(children);
        return;
    }
    // XQueryTree 给的是自下而上；反过来才是"0 = 最前"。
    for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
        const Window child = children[i];
        XWindowAttributes attrs{};
        ErrorTrap childTrap(display);
        const int okAttr = XGetWindowAttributes(display, child, &attrs);
        const int childError = childTrap.Sync();
        if (okAttr == 0 || childError != 0) continue;
        // Xlib 在 C++ 下把字段名写成 c_class（class 是关键字）。
        if (attrs.c_class == InputOnly) continue;

        if (IsClientWindow(display, child)) {
            ClientWindowFacts facts;
            if (ReadFacts(display, child, &facts)) {
                facts.zOrder = (*zOrder)++;
                out->push_back(std::move(facts));
            }
            continue;
        }
        // 不是客户端：可能是窗口管理器的装饰框，继续往下找它的客户端。
        CollectTopLevel(display, child, out, zOrder, depth + 1);
    }
    if (children) XFree(children);
}

// ---------------------------------------------------------------------------
// 像素转换
// ---------------------------------------------------------------------------

unsigned CountTrailingZeros(uint64_t value) {
    unsigned shift = 0;
    while (shift < 64 && ((value >> shift) & 1u) == 0) ++shift;
    return shift;
}

uint8_t ScaleTo8(uint64_t value, uint64_t mask) {
    if (mask == 0) return 0;
    const unsigned shift = CountTrailingZeros(mask);
    const uint64_t max = mask >> shift;
    if (max == 0) return 0;
    const uint64_t component = (value & mask) >> shift;
    return static_cast<uint8_t>((component * 255u + max / 2u) / max);
}

uint64_t ReadPixel(const uint8_t* row, int x, int bitsPerPixel, int byteOrder) {
    const uint8_t* p = row + static_cast<std::size_t>(x) * (bitsPerPixel / 8);
    switch (bitsPerPixel) {
        case 32: {
            uint32_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            if (byteOrder == MSBFirst) {
                value = ((value & 0x000000ffu) << 24) | ((value & 0x0000ff00u) << 8) |
                        ((value & 0x00ff0000u) >> 8) | ((value & 0xff000000u) >> 24);
            }
            return value;
        }
        case 24:
            if (byteOrder == LSBFirst) {
                return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
                       (static_cast<uint64_t>(p[2]) << 16);
            }
            return static_cast<uint64_t>(p[2]) | (static_cast<uint64_t>(p[1]) << 8) |
                   (static_cast<uint64_t>(p[0]) << 16);
        case 16: {
            uint16_t value = 0;
            std::memcpy(&value, p, sizeof(value));
            if (byteOrder == MSBFirst) value = static_cast<uint16_t>((value >> 8) | (value << 8));
            return value;
        }
        case 8:
            return p[0];
        default:
            return 0;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// DisplayConnection / ErrorTrap
// ---------------------------------------------------------------------------

DisplayConnection::DisplayConnection() {
    display_ = XOpenDisplay(nullptr);
    error_ = display_ ? 0 : errno;
}

DisplayConnection::~DisplayConnection() {
    if (display_) XCloseDisplay(display_);
}

ErrorTrap::ErrorTrap(Display* display) : display_(display) {
    g_trapError = 0;
    previous_ = XSetErrorHandler(TrapHandler);
}

ErrorTrap::~ErrorTrap() {
    if (display_) XSync(display_, False);
    XSetErrorHandler(previous_);
}

int ErrorTrap::Sync() {
    if (!display_) return 0;
    XSync(display_, False);
    return g_trapError;
}

// ---------------------------------------------------------------------------
// 窗口事实
// ---------------------------------------------------------------------------

std::vector<ClientWindowFacts> EnumerateClientWindows(Display* display) {
    std::vector<ClientWindowFacts> result;
    if (!display) return result;
    const Window root = DefaultRootWindow(display);
    int zOrder = 0;
    CollectTopLevel(display, root, &result, &zOrder, 0);
    return result;
}

bool QueryClientWindow(Display* display, Window window, ClientWindowFacts* out) {
    if (!display || !out) return false;
    return ReadFacts(display, window, out);
}

bool ReadProcessStartTicksForPid(uint32_t pid, uint64_t* ticks, uint32_t* errorCode) {
    if (!ticks) return false;
    uint32_t error = 0;
    const bool ok = ReadProcessStart(pid, ticks, &error);
    if (errorCode) *errorCode = error;
    return ok;
}

// ---------------------------------------------------------------------------
// XImage -> BGRA8
// ---------------------------------------------------------------------------

bool ConvertXImageToBgra8(const XImage& image, std::vector<uint8_t>* out, uint32_t* stride,
                          std::string* detail) {
    if (!out || !stride) {
        if (detail) *detail = "null output";
        return false;
    }
    if (image.width <= 0 || image.height <= 0) {
        if (detail) *detail = "empty image";
        return false;
    }
    if (image.bits_per_pixel != 8 && image.bits_per_pixel != 16 && image.bits_per_pixel != 24 &&
        image.bits_per_pixel != 32) {
        if (detail) *detail = "unsupported bits_per_pixel " + std::to_string(image.bits_per_pixel);
        return false;
    }
    const uint64_t width = static_cast<uint64_t>(image.width);
    const uint64_t height = static_cast<uint64_t>(image.height);
    if (width > kFrameMaxSide || height > kFrameMaxSide) {
        if (detail) *detail = "image exceeds per-side limit";
        return false;
    }
    const uint64_t rowBytes = width * 4;
    if (rowBytes * height > kFrameMaxBytes) {
        if (detail) *detail = "image exceeds byte limit";
        return false;
    }
    if (image.bytes_per_line < 0 ||
        static_cast<uint64_t>(image.bytes_per_line) < width * (image.bits_per_pixel / 8)) {
        if (detail) *detail = "stride shorter than one row";
        return false;
    }

    out->assign(static_cast<std::size_t>(rowBytes * height), 0);
    *stride = static_cast<uint32_t>(rowBytes);
    // XGetImage 对服务端建的 ARGB32 Pixmap（XRender 的合成目标）可能把三个颜色掩码都
    // 报成 0。那不是灰度图，而是"这个 XImage 没有 visual 可问"；此时按 XRender 规定的
    // 0xAARRGGBB 布局解释。24 位同理，只有 8 位且无掩码才是真正的灰度视觉。
    uint64_t redMask = image.red_mask;
    uint64_t greenMask = image.green_mask;
    uint64_t blueMask = image.blue_mask;
    if (redMask == 0 && greenMask == 0 && blueMask == 0 &&
        (image.bits_per_pixel == 24 || image.bits_per_pixel == 32)) {
        redMask = 0x00ff0000u;
        greenMask = 0x0000ff00u;
        blueMask = 0x000000ffu;
    }
    const bool gray = redMask == 0 && greenMask == 0 && blueMask == 0;
    // XImage 没有 alpha 掩码这一格：32 位深度（ARGB32）的窗口位图把 alpha 放在高 8 位，
    // 24 位及以下没有 alpha，按不透明处理。XComposite 的透明窗口由 XRender 合成时已经
    // 按这条通道解释过，这里不另造一份"看起来像 alpha"的掩码。
    const bool hasAlpha = image.depth == 32;

    for (int y = 0; y < image.height; ++y) {
        const uint8_t* row = reinterpret_cast<const uint8_t*>(image.data) +
                             static_cast<std::size_t>(y) * static_cast<std::size_t>(image.bytes_per_line);
        uint8_t* dst = out->data() + static_cast<std::size_t>(y) * rowBytes;
        for (int x = 0; x < image.width; ++x) {
            const uint64_t pixel = ReadPixel(row, x, image.bits_per_pixel, image.byte_order);
            uint8_t b = 0;
            uint8_t g = 0;
            uint8_t r = 0;
            if (gray) {
                // 8 位灰度视觉：像素值本身就是亮度；0 通道掩码不是"黑色"。
                b = g = r = static_cast<uint8_t>(pixel & 0xffu);
            } else {
                r = ScaleTo8(pixel, redMask);
                g = ScaleTo8(pixel, greenMask);
                b = ScaleTo8(pixel, blueMask);
            }
            const uint8_t a = hasAlpha ? static_cast<uint8_t>((pixel >> 24) & 0xffu) : 255;
            // XRender 的 ARGB32 目标是预乘的；XComposite 直接读回的 32 位窗口位图不是。
            // 调用方按来源告诉我们，这里只在预乘时还原直通 alpha。
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = a;
            dst += 4;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 取帧
// ---------------------------------------------------------------------------

namespace {

struct PixmapRef {
    Pixmap pixmap = None;
    Picture picture = None;
    Window window = None;
    bool redirected = false;
};

void ReleaseRef(Display* display, PixmapRef* ref) {
    if (!display || !ref) return;
    if (ref->picture != None) XRenderFreePicture(display, ref->picture);
    if (ref->pixmap != None) XFreePixmap(display, ref->pixmap);
    if (ref->redirected && ref->window != None) {
        ErrorTrap trap(display);
        XCompositeUnredirectWindow(display, ref->window, CompositeRedirectAutomatic);
        trap.Sync();
    }
    ref->pixmap = None;
    ref->picture = None;
    ref->window = None;
    ref->redirected = false;
}

// 取一扇窗口自己的存储。返回 false 时 *errorCode 是 X 错误（0 = 形状不支持）。
bool NamePixmap(Display* display, Window window, Pixmap* pixmap, bool* redirected,
                 int* errorCode, bool* missing) {
    *missing = false;
    *redirected = false;
    *pixmap = None;

    XWindowAttributes attrs{};
    ErrorTrap attrTrap(display);
    const int okAttr = XGetWindowAttributes(display, window, &attrs);
    int error = attrTrap.Sync();
    if (okAttr == 0 || error != 0) {
        *missing = true;
        *errorCode = error;
        return false;
    }
    if (attrs.c_class == InputOnly) {
        *errorCode = 0;
        return false;
    }
    if (attrs.map_state != IsViewable) {
        // 未映射 / 最小化：XComposite 不提供窗口位图。绝不 MapWindow 去"弄出来"。
        *missing = true;
        *errorCode = 0;
        return false;
    }

    {
        ErrorTrap trap(display);
        XCompositeRedirectWindow(display, window, CompositeRedirectAutomatic);
        error = trap.Sync();
        if (error == 0) {
            *redirected = true;
        } else if (error != BadAccess) {
            // BadAccess = 已有合成器（或别的客户端）手动重定向了它：用它的位图即可，
            // 绝不取消别人的重定向。其它错误则如实失败。
            *errorCode = error;
            return false;
        }
    }

    ErrorTrap trap(display);
    const Pixmap named = XCompositeNameWindowPixmap(display, window);
    error = trap.Sync();
    if (named == None || error != 0) {
        *errorCode = error;
        if (*redirected) {
            XCompositeUnredirectWindow(display, window, CompositeRedirectAutomatic);
            *redirected = false;
        }
        return false;
    }
    *pixmap = named;
    *errorCode = 0;
    return true;
}

struct CompositeContext {
    Display* display = nullptr;
    Picture destination = None;
    int32_t originX = 0;
    int32_t originY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    const Deadline* deadline = nullptr;
    bool childrenIncomplete = false;
    bool timedOut = false;
    bool targetFailure = false;
    bool targetMissing = false;
    int targetError = 0;
};

bool CompositeSubtree(CompositeContext* ctx, Window window, int32_t offsetX, int32_t offsetY,
                      int depth, bool isTarget) {
    if (depth > 8) return true;
    if (ctx->deadline && ctx->deadline->Spent()) {
        ctx->timedOut = true;
        return false;
    }

    PixmapRef ref;
    ref.window = window;
    int errorCode = 0;
    bool missing = false;
    if (!NamePixmap(ctx->display, window, &ref.pixmap, &ref.redirected, &errorCode, &missing)) {
        if (isTarget) {
            // 目标自己取不到是终局错误：绝不交一张只有背景的图。
            ctx->targetFailure = true;
            ctx->targetMissing = missing;
            ctx->targetError = errorCode;
            ReleaseRef(ctx->display, &ref);
            return false;
        }
        // 子窗口取不到只说明少了一块（通常是它正在销毁），目标本身仍然有效。
        ctx->childrenIncomplete = true;
        ReleaseRef(ctx->display, &ref);
        return true;
    }

    XWindowAttributes attrs{};
    {
        ErrorTrap trap(ctx->display);
        const int ok = XGetWindowAttributes(ctx->display, window, &attrs);
        const int error = trap.Sync();
        if (ok == 0 || error != 0) {
            if (isTarget) {
                ctx->targetFailure = true;
                ctx->targetMissing = true;
                ctx->targetError = error;
            }
            ctx->childrenIncomplete = true;
            ReleaseRef(ctx->display, &ref);
            return !isTarget;
        }
    }

    XRenderPictFormat* format = XRenderFindVisualFormat(ctx->display, attrs.visual);
    if (!format) {
        if (isTarget) {
            ctx->targetFailure = true;
            ctx->targetError = 0;
        }
        ctx->childrenIncomplete = true;
        ReleaseRef(ctx->display, &ref);
        return !isTarget;
    }
    ref.picture = XRenderCreatePicture(ctx->display, ref.pixmap, format, 0, nullptr);
    if (ref.picture == None) {
        if (isTarget) {
            ctx->targetFailure = true;
            ctx->targetError = 0;
        }
        ctx->childrenIncomplete = true;
        ReleaseRef(ctx->display, &ref);
        return !isTarget;
    }

    // 裁剪到目标窗口：服务端会按目标 picture 的边界裁掉超出的部分。子窗口坐标是相对
    // 父窗口的，累加之后就是相对目标窗口的偏移。
    XRenderComposite(ctx->display, PictOpOver, ref.picture, None, ctx->destination, 0, 0, 0, 0,
                     offsetX, offsetY, static_cast<unsigned>(attrs.width),
                     static_cast<unsigned>(attrs.height));

    // 继续合成这扇窗口的子窗口（自下而上）。目标窗口自己的位图不含子窗口内容，
    // 所以这一层递归决定了"子窗口包不包含在捕获范围里"——包含。
    Window root = None;
    Window parent = None;
    Window* children = nullptr;
    unsigned int count = 0;
    {
        ErrorTrap trap(ctx->display);
        const int ok = XQueryTree(ctx->display, window, &root, &parent, &children, &count);
        const int error = trap.Sync();
        if (ok == 0 || error != 0) {
            if (children) XFree(children);
            ReleaseRef(ctx->display, &ref);
            return true;
        }
    }
    for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
        const Window child = children[i];
        XWindowAttributes childAttrs{};
        ErrorTrap childTrap(ctx->display);
        const int ok = XGetWindowAttributes(ctx->display, child, &childAttrs);
        const int error = childTrap.Sync();
        if (ok == 0 || error != 0) continue;
        if (childAttrs.c_class == InputOnly || childAttrs.map_state != IsViewable) continue;
        int cx = 0;
        int cy = 0;
        Window ignored = None;
        ErrorTrap translateTrap(ctx->display);
        const int translated = XTranslateCoordinates(ctx->display, child, window, 0, 0, &cx, &cy,
                                                     &ignored);
        const int translateError = translateTrap.Sync();
        if (translated == 0 || translateError != 0) continue;
        if (!CompositeSubtree(ctx, child, offsetX + cx, offsetY + cy, depth + 1, false)) {
            XFree(children);
            ReleaseRef(ctx->display, &ref);
            return false;
        }
    }
    if (children) XFree(children);
    ReleaseRef(ctx->display, &ref);
    return true;
}

}  // namespace

bool CaptureWindowStorage(Display* display, Window window, uint32_t timeoutMs,
                          const Deadline& deadline, CapturedFrame* out, Diagnostic* err,
                          bool* childrenIncomplete) {
    (void)timeoutMs;
    if (childrenIncomplete) *childrenIncomplete = false;
    if (!out) return false;
    if (!display) {
        SetFailure(err, codes::kCaptureFailed, L"cap.x11.display_unavailable",
                   L"cap.x11.display_unavailable_hint", L"xcomposite",
                   "XOpenDisplay returned null");
        return false;
    }
    if (deadline.Spent()) {
        SetFailure(err, codes::kFrameTimeout, L"cap.no_detail", nullptr, L"xcomposite",
                   "deadline spent before capture");
        return false;
    }

    int compositeEvent = 0;
    int compositeError = 0;
    if (XCompositeQueryExtension(display, &compositeEvent, &compositeError) == 0) {
        SetFailure(err, codes::kUnsupported, L"cap.x11.composite_unavailable",
                   L"cap.x11.composite_unavailable_hint", L"xcomposite",
                   "XComposite extension not present");
        return false;
    }

    XWindowAttributes attrs{};
    {
        ErrorTrap trap(display);
        const int ok = XGetWindowAttributes(display, window, &attrs);
        const int error = trap.Sync();
        if (ok == 0 || error != 0) {
            SetFailure(err, codes::kTargetGone, L"cap.window_gone", nullptr, L"xcomposite",
                       std::string("XGetWindowAttributes: ") + XErrorToken(error));
            return false;
        }
    }
    if (attrs.map_state != IsViewable) {
        // 最小化 / 未映射：目标自己的存储现在没有可读内容。不 MapWindow、不激活、不移动。
        SetFailure(err, codes::kWindowGone, L"cap.x11.not_viewable", L"cap.x11.not_viewable_hint",
                   L"xcomposite", "window is not viewable");
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(std::max(0, attrs.width));
    const uint32_t height = static_cast<uint32_t>(std::max(0, attrs.height));
    if (width == 0 || height == 0) {
        SetFailure(err, codes::kWindowGone, L"cap.rect_empty_draw", L"cap.window_gone",
                   L"xcomposite", "window has zero client area");
        return false;
    }
    if (width > kFrameMaxSide || height > kFrameMaxSide ||
        static_cast<uint64_t>(width) * height * 4 > kFrameMaxBytes) {
        SetFailure(err, codes::kFrameInvalid, L"cap.x11.frame_too_large", nullptr, L"xcomposite",
                   "window exceeds frame limits");
        return false;
    }

    // 目标 picture：ARGB32 的离屏位图，先用透明黑铺底，再把目标与子窗口各自的重定向存储
    // 叠上去。这样透明窗口的 alpha 也是"应用自己画出来的那一个"，不是屏幕背景。
    Pixmap destinationPixmap = None;
    Picture destination = None;
    {
        ErrorTrap trap(display);
        destinationPixmap = XCreatePixmap(display, attrs.root, width, height, 32);
        const int pixmapError = trap.Sync();
        if (destinationPixmap == None || pixmapError != 0) {
            SetFailure(err, codes::kCaptureFailed, L"cap.x11.pixmap_failed", nullptr, L"xcomposite",
                       std::string("XCreatePixmap: ") + XErrorToken(pixmapError));
            return false;
        }
        XRenderPictFormat* format = XRenderFindStandardFormat(display, PictStandardARGB32);
        if (!format) {
            XFreePixmap(display, destinationPixmap);
            SetFailure(err, codes::kUnsupported, L"cap.x11.render_unavailable",
                       L"cap.x11.render_unavailable_hint", L"xcomposite",
                       "XRender ARGB32 format unavailable");
            return false;
        }
        destination = XRenderCreatePicture(display, destinationPixmap, format, 0, nullptr);
        const int pictureError = trap.Sync();
        if (destination == None || pictureError != 0) {
            XFreePixmap(display, destinationPixmap);
            SetFailure(err, codes::kCaptureFailed, L"cap.x11.pixmap_failed", nullptr, L"xcomposite",
                       std::string("XRenderCreatePicture: ") + XErrorToken(pictureError));
            return false;
        }
        XRenderColor transparent{0, 0, 0, 0};
        XRenderFillRectangle(display, PictOpSrc, destination, &transparent, 0, 0, width, height);
        const int fillError = trap.Sync();
        if (fillError != 0) {
            XRenderFreePicture(display, destination);
            XFreePixmap(display, destinationPixmap);
            SetFailure(err, codes::kCaptureFailed, L"cap.x11.pixmap_failed", nullptr, L"xcomposite",
                       std::string("XRenderFillRectangle: ") + XErrorToken(fillError));
            return false;
        }
    }

    CompositeContext context;
    context.display = display;
    context.destination = destination;
    context.width = width;
    context.height = height;
    context.deadline = &deadline;

    const bool composed = CompositeSubtree(&context, window, 0, 0, 0, true);

    if (context.targetFailure) {
        XRenderFreePicture(display, destination);
        XFreePixmap(display, destinationPixmap);
        if (context.targetMissing) {
            SetFailure(err, codes::kWindowGone, L"cap.x11.not_viewable",
                       L"cap.x11.not_viewable_hint", L"xcomposite",
                       std::string("target window became unviewable: ") +
                           XErrorToken(context.targetError));
        } else {
            SetFailure(err, codes::kCaptureFailed, L"cap.x11.name_pixmap_failed", nullptr,
                       L"xcomposite",
                       std::string("XCompositeNameWindowPixmap: ") +
                           XErrorToken(context.targetError));
        }
        return false;
    }

    // 读回之前再核一次目标：销毁 / 改尺寸都说明这一帧不对应当下的窗口。绝不交旧图。
    XWindowAttributes after{};
    bool alive = false;
    {
        ErrorTrap trap(display);
        const int ok = XGetWindowAttributes(display, window, &after);
        const int error = trap.Sync();
        alive = ok != 0 && error == 0;
    }
    if (!alive) {
        XRenderFreePicture(display, destination);
        XFreePixmap(display, destinationPixmap);
        SetFailure(err, codes::kTargetGone, L"cap.window_gone", nullptr, L"xcomposite",
                   "window disappeared during capture");
        return false;
    }
    if (static_cast<uint32_t>(std::max(0, after.width)) != width ||
        static_cast<uint32_t>(std::max(0, after.height)) != height) {
        XRenderFreePicture(display, destination);
        XFreePixmap(display, destinationPixmap);
        SetFailure(err, codes::kTargetChanged, L"cap.target_changed", nullptr, L"xcomposite",
                   "window resized during capture");
        return false;
    }
    if (context.timedOut || deadline.Spent()) {
        XRenderFreePicture(display, destination);
        XFreePixmap(display, destinationPixmap);
        SetFailure(err, codes::kFrameTimeout, L"cap.no_detail", nullptr, L"xcomposite",
                   "deadline spent during compositing");
        return false;
    }

    XImage* image = nullptr;
    {
        ErrorTrap trap(display);
        image = XGetImage(display, destinationPixmap, 0, 0, width, height, AllPlanes, ZPixmap);
        const int error = trap.Sync();
        if (!image || error != 0) {
            if (image) XDestroyImage(image);
            XRenderFreePicture(display, destination);
            XFreePixmap(display, destinationPixmap);
            SetFailure(err, codes::kCaptureFailed, L"cap.x11.read_pixels_failed", nullptr,
                       L"xcomposite", std::string("XGetImage: ") + XErrorToken(error));
            return false;
        }
    }

    // XRender 的 ARGB32 是预乘的；转换时按预乘还原直通 alpha。XComposite 直接读回的
    // 32 位窗口位图不是预乘的，但那条路只在我们没有 XRender 目标时才会用到，而本函数
    // 在没有 XRender 时已经失败（见上），所以这里恒为预乘。
    std::vector<uint8_t> pixels;
    uint32_t stride = 0;
    std::string detail;
    const bool converted = ConvertXImageToBgra8(*image, &pixels, &stride, &detail);
    XDestroyImage(image);
    XRenderFreePicture(display, destination);
    XFreePixmap(display, destinationPixmap);
    if (!converted) {
        SetFailure(err, codes::kFrameInvalid, L"cap.x11.convert_failed", nullptr, L"xcomposite",
                   detail);
        return false;
    }

    // 逐像素转换也要算进期限：这一步在大窗口上耗时不短，期限已到就不交这一帧，
    // 而不是把一张已经过期的图当成功交出去。
    if (context.timedOut || deadline.Spent()) {
        SetFailure(err, codes::kFrameTimeout, L"cap.no_detail", nullptr, L"xcomposite",
                   "deadline spent during pixel conversion");
        return false;
    }

    // 预乘还原（XRender 合成结果）。透明像素保持 (0,0,0,0)。
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const uint8_t a = pixels[i + 3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; ++c) {
            const unsigned value = pixels[i + c];
            pixels[i + c] = static_cast<uint8_t>(std::min<unsigned>(255, (value * 255u + a / 2u) / a));
        }
    }

    if (!composed) {
        // 合成过程中期限到了：这一帧不完整，按超时失败，不交半张图。
        SetFailure(err, codes::kFrameTimeout, L"cap.no_detail", nullptr, L"xcomposite",
                   "deadline spent during compositing");
        return false;
    }

    out->pixels = std::move(pixels);
    out->width = width;
    out->height = height;
    out->stride = stride;
    out->source = L"xcomposite";
    out->path = L"xcomposite";
    out->reportsCrop = false;
    out->cursorStateKnown = false;
    out->cursorInFrame = false;
    out->sourceColorSpace = FrameColorSpace::kSrgbBgra8;
    out->toneMapped = false;
    out->displayHdrState = DisplayHdrState::kUnknown;
    if (childrenIncomplete) *childrenIncomplete = context.childrenIncomplete;
    return true;
}

}  // namespace x11
}  // namespace ecapture
