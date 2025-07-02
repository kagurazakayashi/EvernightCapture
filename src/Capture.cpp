#include "Capture.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureWgc.h"
#include "CaptureBitBlt.h"
#include "CaptureCommon.h"
#include "CaptureDwm.h"
#include "CaptureDuplication.h"
#include "CapturePrintWindow.h"
#include "Encoder.h"
#include "Report.h"
#include "ScreenMatch.h"
#include "WindowMatch.h"

namespace ecapture {
namespace {

constexpr uint32_t kFrameTimeoutMs = 2000;

std::wstring AbsoluteOfOrRaw(const std::wstring& path) {
    const DWORD cap = 8 * MAX_PATH;
    std::vector<wchar_t> buffer(cap);
    const DWORD n = GetFullPathNameW(path.c_str(), cap, buffer.data(), nullptr);
    if (n == 0 || n >= cap) return path;
    return std::wstring(buffer.data());
}

std::wstring FourDigits(int v) {
    std::wstring s = std::to_wstring(v);
    while (s.size() < 4) s.insert(s.begin(), L'0');
    return s;
}

std::wstring TwoDigits(int v) {
    std::wstring s = std::to_wstring(v);
    if (s.size() < 2) s.insert(s.begin(), L'0');
    return s;
}

std::wstring DateToken() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return FourDigits(st.wYear) + TwoDigits(st.wMonth) + TwoDigits(st.wDay);
}

std::wstring TimeToken() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return TwoDigits(st.wHour) + TwoDigits(st.wMinute) + TwoDigits(st.wSecond);
}

bool IllegalNameChar(wchar_t c) {
    return c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
           c == L'|' || c == L'?' || c == L'*';
}

std::wstring SanitizeForFileName(std::wstring s) {
    for (auto& c : s) if (IllegalNameChar(c)) c = L'_';
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    if (s.size() > 80) s.resize(80);
    return s;
}

std::wstring ExtensionFor(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::kJpeg: return L".jpg";
        case ImageFormat::kBmp: return L".bmp";
        case ImageFormat::kTiff: return L".tif";
        case ImageFormat::kGif: return L".gif";
        default: return L".png";
    }
}

bool HasExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
}

// 占位符展开。窗口目标给句柄 / 进程 / 标题；屏幕目标没有前两者（取到 0），
// %n 用去掉 "\\.\\" 前缀的设备名，这样 shot_%n.png 每块屏一个文件。
std::wstring Expand(const std::wstring& pattern, uint64_t hwnd, uint32_t pid,
                    const std::wstring& name, size_t ordinal) {
    std::wstring out;
    wchar_t hwndBuf[24];
    swprintf(hwndBuf, 24, L"0x%08X", static_cast<unsigned>(hwnd));
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != L'%' || i + 1 >= pattern.size()) {
            out.push_back(pattern[i]);
            continue;
        }
        const wchar_t token = pattern[++i];
        switch (token) {
            case L'd': out += DateToken(); break;
            case L't': out += TimeToken(); break;
            case L'h': out += hwndBuf; break;
            case L'p': out += std::to_wstring(pid); break;
            case L'i': out += std::to_wstring(ordinal); break;
            case L'n': out += SanitizeForFileName(name); break;
            case L'%': out.push_back(L'%'); break;
            default:
                out.push_back(L'%');
                out.push_back(token);
                break;
        }
    }
    return out;
}

// 没有占位符时，把序号插在扩展名前面
std::wstring AppendOrdinal(std::wstring path, size_t ordinal) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    const std::wstring suffix =
        (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
            ? path.substr(dot) : std::wstring();
    if (!suffix.empty()) path = path.substr(0, dot);
    path += L"_" + std::to_wstring(ordinal);
    path += suffix;
    return path;
}

bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
              Diagnostic* err) {
    if (!overwrite) {
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES) {
            if (err) *err = Diagnostic{codes::kFileExists, Msg(L"io.file_exists"),
                                       L"--no-overwrite", path, Msg(L"io.file_exists_hint")};
            return false;
        }
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD gle = GetLastError();
        const std::wstring hint = gle == ERROR_PATH_NOT_FOUND
                                      ? Msg(L"io.dir_missing")
                                      : Msgf(L"io.open_failed_hint", Msgf(L"err.win32_code", gle));
        if (err) *err = Diagnostic{codes::kWriteFailed, Msg(L"io.open_failed"), L"--out", path, hint};
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            CloseHandle(handle);
            if (err) *err = Diagnostic{codes::kWriteFailed, Msg(L"io.write_interrupted"),
                                       L"--out", path, Msgf(L"err.win32_code", GetLastError())};
            DeleteFileW(path.c_str());
            return false;
        }
        offset += written;
    }
    CloseHandle(handle);
    return true;
}

std::wstring HwndHexOf(uint64_t hwnd) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%08X", static_cast<unsigned>(hwnd));
    return buf;
}

// 单个通道的取帧入口
bool CaptureOneChannel(uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                       Diagnostic* err) {
    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureWindowWgc(hwnd, timeoutMs, out, err);
        case CaptureMethod::kDwmThumbnail:
            return CaptureWindowDwmThumbnail(hwnd, timeoutMs, out, err);
        case CaptureMethod::kPrintWindow:
            return CaptureWindowPrintWindow(hwnd, timeoutMs, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureWindowBitBlt(hwnd, timeoutMs, out, err);
        case CaptureMethod::kDuplication:
            return CaptureWindowDuplication(hwnd, timeoutMs, out, err);
        case CaptureMethod::kAuto:
            break;  // auto 由 CaptureWithMethod 展开成回退链
    }
    if (err) *err = Diagnostic{codes::kUnsupported, Msg(L"cap.unsupported"), L"--capture",
                               CaptureMethodName(method), std::wstring()};
    return false;
}

// 屏幕目标的单通道取帧。dwm / printwindow 取的是"某个窗口的画面"，屏幕上并没有
// 这么一个窗口可让它们画，所以这两种通道在解析期就已经被挡在屏幕目标之外。
bool CaptureScreenOneChannel(const ScreenInfo& screen, CaptureMethod method, uint32_t timeoutMs,
                             CapturedFrame* out, Diagnostic* err) {
    switch (method) {
        case CaptureMethod::kWgc:
            return CaptureScreenWgc(screen, timeoutMs, out, err);
        case CaptureMethod::kBitBlt:
            return CaptureScreenBitBlt(screen, timeoutMs, out, err);
        case CaptureMethod::kDuplication:
            return CaptureScreenDuplication(screen, timeoutMs, out, err);
        case CaptureMethod::kAuto:
            break;  // auto 由 CaptureScreenWithMethod 展开成回退链
        default:
            break;
    }
    if (err) {
        *err = Diagnostic{codes::kUnsupported, Msgf(L"cap.unsupported_for_screen", CaptureMethodName(method)),
                          L"--capture", CaptureMethodName(method),
                          Msg(L"cap.unsupported_for_screen_hint")};
    }
    return false;
}

// auto 的回退链：按顺序试到第一个成功的通道。实际用的不是链首时留一条 note，
// 让调用方知道画面来路不同。
template <typename Try>
bool FallbackChain(const std::vector<CaptureMethod>& chain, CapturedFrame* out, Diagnostic* err,
                   std::vector<Diagnostic>* notes, Try tryOne) {
    std::wstring tried;
    Diagnostic last{};
    for (const CaptureMethod m : chain) {
        CapturedFrame attempt;
        Diagnostic attemptErr{};
        if (tryOne(m, &attempt, &attemptErr)) {
            *out = std::move(attempt);
            if (m != chain.front() && notes) {
                notes->push_back(Diagnostic{
                    codes::kCaptureChannel,
                    Msgf(L"note.capture_channel", CaptureMethodName(chain.front()), CaptureMethodName(m)),
                    L"--capture", L"auto", std::wstring()});
            }
            return true;
        }
        if (!tried.empty()) tried += L", ";
        tried += CaptureMethodName(m);
        last = std::move(attemptErr);
    }
    if (err) {
        *err = Diagnostic{codes::kCaptureFailed, Msgf(L"cap.auto_failed", tried), L"--capture",
                          L"auto", last.message};
    }
    return false;
}

// --capture 分派。auto 对窗口按 wgc -> dwm -> printwindow -> bitblt，对屏幕按
// wgc -> duplication -> bitblt。显式指定的通道绝不回退：用户要哪个就要哪个。
bool CaptureWithMethod(uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                       Diagnostic* err, std::vector<Diagnostic>* notes) {
    if (method != CaptureMethod::kAuto) return CaptureOneChannel(hwnd, method, timeoutMs, out, err);
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                              CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt};
    return FallbackChain(chain, out, err, notes, [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
        return CaptureOneChannel(hwnd, m, timeoutMs, frame, e);
    });
}

bool CaptureScreenWithMethod(const ScreenInfo& screen, CaptureMethod method, uint32_t timeoutMs,
                             CapturedFrame* out, Diagnostic* err, std::vector<Diagnostic>* notes) {
    if (method != CaptureMethod::kAuto)
        return CaptureScreenOneChannel(screen, method, timeoutMs, out, err);
    const std::vector<CaptureMethod> chain = {CaptureMethod::kWgc, CaptureMethod::kDuplication,
                                             CaptureMethod::kBitBlt};
    return FallbackChain(chain, out, err, notes, [&](CaptureMethod m, CapturedFrame* frame, Diagnostic* e) {
        return CaptureScreenOneChannel(screen, m, timeoutMs, frame, e);
    });
}

// 一次截图的目标：一个窗口，或一整块屏幕。
struct Target {
    bool isScreen = false;
    WindowInfo window;
    ScreenInfo screen;
};

std::wstring TargetName(const Target& t) {
    return t.isScreen ? ScreenDisplayName(t.screen) : t.window.title;
}

}  // namespace

CaptureOutcome RunCapture(const Options& opt) {
    CaptureOutcome outcome;

    // 屏幕矩形必须与物理像素一致，否则 GDI 通道会截偏
    EnsureDpiAware();

    std::vector<Target> targets;
    if (opt.ScreenMode()) {
        for (const auto& s : SelectScreens(opt, &outcome.errors)) {
            Target t;
            t.isScreen = true;
            t.screen = s;
            targets.push_back(std::move(t));
        }
    } else {
        for (const auto& w : SelectWindows(opt, &outcome.errors, &outcome.notes)) {
            Target t;
            t.window = w;
            targets.push_back(std::move(t));
        }
    }
    if (!outcome.errors.empty()) {
        const std::wstring& code = outcome.errors.front().code;
        outcome.exitCode = code == codes::kAmbiguousWindow ? EX_AMBIGUOUS
                         : code == codes::kIndexOutOfRange || code == codes::kMonitorOutOfRange
                             ? EX_USAGE
                             : EX_NO_MATCH;
        return outcome;
    }

    if (opt.dryRun) {
        std::wstring list;
        for (const Target& t : targets) {
            if (!list.empty()) list += L" | ";
            list += t.isScreen ? DescribeScreen(t.screen) : DescribeWindow(t.window);
        }
        const wchar_t* key = targets.front().isScreen ? L"note.dry_run_monitor" : L"note.dry_run";
        outcome.notes.push_back(Diagnostic{codes::kDryRun, Msgf(key, targets.size()), L"--dry-run",
                                           list, std::wstring()});
        return outcome;
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        const Target& t = targets[i];
        const ULONGLONG started = GetTickCount64();

        CapturedImage img;
        img.format = FormatName(opt.format);
        if (t.isScreen) {
            img.screen = true;
            img.monitorOrdinal = t.screen.ordinal;
            img.deviceName = t.screen.deviceName;
            img.primary = t.screen.primary;
        } else {
            img.hwndHex = HwndHexOf(t.window.hwnd);
            img.pid = t.window.pid;
            img.title = t.window.title;
            img.windowClass = t.window.className;
            img.imageName = t.window.imageName;
        }

        // %h / %p 只对窗口有意义，屏幕目标给 0；%n 是窗口标题或设备名
        const uint64_t hwnd = t.isScreen ? 0 : t.window.hwnd;
        const uint32_t pid = t.isScreen ? 0 : t.window.pid;
        const std::wstring name = TargetName(t);

        if (targets.size() > 1) {
            img.file = opt.output.find(L'%') != std::wstring::npos
                           ? Expand(opt.output, hwnd, pid, name, i + 1)
                           : AppendOrdinal(opt.output, i + 1);
        } else {
            img.file = Expand(opt.output, hwnd, pid, name, 1);
        }
        if (img.file != L"-" && !HasExtension(img.file)) {
            img.file += ExtensionFor(opt.format);
            // 文件名被改了要说一声，否则调用方按自己给的名字去找会找不到
            if (i == 0) {
                outcome.notes.push_back(Diagnostic{codes::kOutputExtensionAppended,
                                                   Msg(L"note.output_extension_appended"), L"--out",
                                                   opt.output, Msgf(L"note.output_extension_hint", img.file)});
            }
        }

        CapturedFrame frame;
        Diagnostic capErr;
        const bool got = t.isScreen
                             ? CaptureScreenWithMethod(t.screen, opt.capture, kFrameTimeoutMs, &frame,
                                                       &capErr, &outcome.notes)
                             : CaptureWithMethod(t.window.hwnd, opt.capture, kFrameTimeoutMs, &frame,
                                                 &capErr, &outcome.notes);
        if (!got) {
            outcome.errors.push_back(std::move(capErr));
            continue;
        }
        img.width = frame.width;
        img.height = frame.height;

        std::vector<uint8_t> bytes;
        Diagnostic encErr;
        if (!EncodeFrame(frame, opt.format, opt.jpegQuality, &bytes, &encErr)) {
            outcome.errors.push_back(std::move(encErr));
            continue;
        }

        if (img.file == L"-") {
            if (!EmitStdoutBytes(bytes)) {
                outcome.errors.push_back(Diagnostic{codes::kWriteFailed, Msg(L"io.stdout_failed"),
                                                    L"--out", L"-", std::wstring()});
                continue;
            }
        } else {
            Diagnostic writeErr;
            const std::wstring absolute = AbsoluteOfOrRaw(img.file);
            if (!WriteAll(absolute, bytes, opt.overwrite, &writeErr)) {
                outcome.errors.push_back(std::move(writeErr));
                continue;
            }
            img.file = absolute;
        }
        img.bytes = bytes.size();
        img.elapsedMs = static_cast<uint32_t>(GetTickCount64() - started);
        outcome.images.push_back(std::move(img));
    }

    if (outcome.images.empty()) {
        const std::wstring& code =
            outcome.errors.empty() ? std::wstring() : outcome.errors.front().code;
        outcome.exitCode = code == codes::kWriteFailed || code == codes::kFileExists
                               ? EX_IO_FAILED
                               : EX_CAPTURE_FAILED;
    } else {
        // 部分成功：图片已写出，但有目标失败 -> 用截图失败码提示调用方看 errors
        outcome.exitCode = outcome.errors.empty() ? EX_OK : EX_CAPTURE_FAILED;
    }
    return outcome;
}

}  // namespace ecapture
