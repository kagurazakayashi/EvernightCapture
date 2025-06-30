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

std::wstring Expand(const std::wstring& pattern, const WindowInfo& w, size_t ordinal) {
    std::wstring out;
    wchar_t hwndBuf[24];
    swprintf(hwndBuf, 24, L"0x%08X", static_cast<unsigned>(w.hwnd));
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
            case L'p': out += std::to_wstring(w.pid); break;
            case L'i': out += std::to_wstring(ordinal); break;
            case L'n': out += SanitizeForFileName(w.title); break;
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
            if (err) *err = Diagnostic{codes::kFileExists, L"目标文件已存在（--no-overwrite）",
                                       L"--no-overwrite", path, L"去掉该选项以覆盖，或换输出文件名"};
            return false;
        }
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD gle = GetLastError();
        std::wstring hint = L"父目录是否存在？路径是否合法（错误码 " + std::to_wstring(gle) + L"）";
        if (gle == ERROR_PATH_NOT_FOUND) hint = L"输出目录不存在，请先建好目录";
        if (err) *err = Diagnostic{codes::kWriteFailed, L"无法打开输出文件", L"--out", path, hint};
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            CloseHandle(handle);
            if (err) *err = Diagnostic{codes::kWriteFailed, L"写文件中断", L"--out", path,
                                       L"错误码 " + std::to_wstring(GetLastError())};
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
    if (err) *err = Diagnostic{codes::kUnsupported, L"该取图方式尚未实现", L"--capture",
                               CaptureMethodName(method), std::wstring()};
    return false;
}

// --capture 分派。auto 按 wgc -> dwm -> printwindow -> bitblt 依次试，取第一个成功的；
// 实际用的通道不是 wgc 时留一条 note，让调用方知道画面来路不同。
// 显式指定的通道绝不回退：用户要哪个就要哪个。
bool CaptureWithMethod(uint64_t hwnd, CaptureMethod method, uint32_t timeoutMs, CapturedFrame* out,
                       Diagnostic* err, std::vector<Diagnostic>* notes) {
    if (method != CaptureMethod::kAuto) return CaptureOneChannel(hwnd, method, timeoutMs, out, err);

    static const CaptureMethod kChain[] = {CaptureMethod::kWgc, CaptureMethod::kDwmThumbnail,
                                           CaptureMethod::kPrintWindow, CaptureMethod::kBitBlt};
    std::wstring tried;
    Diagnostic last{};
    for (const CaptureMethod m : kChain) {
        CapturedFrame attempt;
        Diagnostic attemptErr{};
        if (CaptureOneChannel(hwnd, m, timeoutMs, &attempt, &attemptErr)) {
            *out = std::move(attempt);
            if (m != CaptureMethod::kWgc && notes) {
                notes->push_back(Diagnostic{codes::kCaptureChannel,
                                            L"auto：wgc 不可用，改用 " +
                                                std::wstring(CaptureMethodName(m)),
                                            L"--capture", L"auto", std::wstring()});
            }
            return true;
        }
        if (!tried.empty()) tried += L", ";
        tried += CaptureMethodName(m);
        last = std::move(attemptErr);
    }
    if (err) {
        *err = Diagnostic{codes::kCaptureFailed, L"auto 的回退通道全部失败（" + tried + L"）",
                          L"--capture", L"auto", last.message};
    }
    return false;
}

}  // namespace

CaptureOutcome RunCapture(const Options& opt) {
    CaptureOutcome outcome;

    // 屏幕矩形必须与物理像素一致，否则 GDI 通道会截偏
    EnsureDpiAware();

    std::vector<WindowInfo> targets = SelectWindows(opt, &outcome.errors, &outcome.notes);
    if (!outcome.errors.empty()) {
        const std::wstring& code = outcome.errors.front().code;
        outcome.exitCode = code == codes::kAmbiguousWindow ? EX_AMBIGUOUS
                         : code == codes::kIndexOutOfRange ? EX_USAGE
                                                           : EX_NO_MATCH;
        return outcome;
    }

    if (opt.dryRun) {
        std::wstring list;
        for (const auto& w : targets) {
            if (!list.empty()) list += L" | ";
            list += DescribeWindow(w);
        }
        outcome.notes.push_back(Diagnostic{codes::kDryRun,
                                           L"--dry-run：已选出 " + std::to_wstring(targets.size()) +
                                               L" 个窗口，未截图也未写文件",
                                           L"--dry-run", list, std::wstring()});
        return outcome;
    }

    for (size_t i = 0; i < targets.size(); ++i) {
        const WindowInfo& w = targets[i];
        const ULONGLONG started = GetTickCount64();

        CapturedImage img;
        img.hwndHex = HwndHexOf(w.hwnd);
        img.pid = w.pid;
        img.title = w.title;
        img.windowClass = w.className;
        img.imageName = w.imageName;
        img.format = FormatName(opt.format);

        if (targets.size() > 1) {
            img.file = opt.output.find(L'%') != std::wstring::npos
                           ? Expand(opt.output, w, i + 1)
                           : AppendOrdinal(opt.output, i + 1);
        } else {
            img.file = Expand(opt.output, w, 1);
        }
        if (img.file != L"-" && !HasExtension(img.file)) {
            img.file += ExtensionFor(opt.format);
            // 文件名被改了要说一声，否则调用方按自己给的名字去找会找不到
            if (i == 0) {
                outcome.notes.push_back(Diagnostic{codes::kOutputExtensionAppended,
                                                   L"输出名没有扩展名，已按所选格式补上", L"--out",
                                                   opt.output, L"实际写成 " + img.file});
            }
        }

        CapturedFrame frame;
        Diagnostic capErr;
        if (!CaptureWithMethod(w.hwnd, opt.capture, kFrameTimeoutMs, &frame, &capErr,
                               &outcome.notes)) {
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
                outcome.errors.push_back(Diagnostic{codes::kWriteFailed, L"写标准输出失败", L"--out",
                                                    L"-", std::wstring()});
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
        // 部分成功：图片已写出，但有的窗口失败 -> 用截图失败码提示调用方看 errors
        outcome.exitCode = outcome.errors.empty() ? EX_OK : EX_CAPTURE_FAILED;
    }
    return outcome;
}

}  // namespace ecapture
