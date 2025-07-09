#include "OutputPlan.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {
namespace {

// %n 的长度上限：按 UTF-16 码元算，沿用旧实现的 80（改这个数字会让老调用方的文件名变样）
constexpr size_t kMaxNameChars = 80;

std::wstring ToLowerLocal(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(ch));
    return s;
}

bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
bool IsLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

bool IllegalNameChar(wchar_t c) {
    return c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
           c == L'|' || c == L'?' || c == L'*';
}

// Windows 在普通路径里把这些名字当设备而不是文件（连 CON.png 一起拦），
// 所以 %n 正好落成其中之一时必须改掉，否则后面无论哪条通道都会撞开不出文件。
bool IsReservedDeviceName(const std::wstring& name) {
    static const wchar_t* const kNames[] = {
        L"con", L"prn", L"aux", L"nul",
        L"com1", L"com2", L"com3", L"com4", L"com5", L"com6", L"com7", L"com8", L"com9",
        L"lpt1", L"lpt2", L"lpt3", L"lpt4", L"lpt5", L"lpt6", L"lpt7", L"lpt8", L"lpt9",
        nullptr};
    const std::wstring lower = ToLowerLocal(name);
    const size_t stemEnd = lower.find(L'.');  // 扩展名前那一段才是设备名
    const std::wstring stem = lower.substr(0, stemEnd);
    for (const wchar_t* const* p = kNames; *p; ++p) {
        if (stem == *p) return true;
    }
    return false;
}

void TrimTrailingDotsAndSpaces(std::wstring* s) {
    while (!s->empty() && (s->back() == L' ' || s->back() == L'.')) s->pop_back();
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

// 一个批次的日期与时间：整批只取一次。分开取的话，跨午夜的批次会把同一次截图劈成两天的名字，
// 也让 %d-%t 这种"看时间戳找同一批"的用法失效。
struct BatchClock {
    std::wstring date;  // YYYYMMDD
    std::wstring time;  // HHMMSS
};

BatchClock MakeBatchClock() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    BatchClock clock;
    clock.date = FourDigits(st.wYear) + TwoDigits(st.wMonth) + TwoDigits(st.wDay);
    clock.time = TwoDigits(st.wHour) + TwoDigits(st.wMinute) + TwoDigits(st.wSecond);
    return clock;
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

std::wstring AbsoluteOfOrRaw(const std::wstring& path) {
    const DWORD cap = 8 * MAX_PATH;
    std::vector<wchar_t> buffer(cap);
    const DWORD n = GetFullPathNameW(path.c_str(), cap, buffer.data(), nullptr);
    if (n == 0 || n >= cap) return path;  // 路径过长时原样留着，交给开文件那一步报错
    return std::wstring(buffer.data());
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

// 占位符展开。窗口目标给句柄 / 进程 / 标题；屏幕目标没有前两者（取到 0），
// %n 用去掉 "\\.\\" 前缀的设备名，这样 shot_%n.png 每块屏一个文件。
// 未知的 %x 原样留两个字符（现有约定），但留下的照样是两个目标共用的名字，
// 所以碰撞检测一视同仁。
std::wstring Expand(const std::wstring& pattern, const OutputTarget& target, size_t ordinal,
                    const BatchClock& clock) {
    std::wstring out;
    wchar_t hwndBuf[24];
    swprintf(hwndBuf, 24, L"0x%08X", static_cast<unsigned>(target.hwnd));
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != L'%' || i + 1 >= pattern.size()) {
            out.push_back(pattern[i]);
            continue;
        }
        const wchar_t token = pattern[++i];
        switch (token) {
            case L'd': out += clock.date; break;
            case L't': out += clock.time; break;
            case L'h': out += hwndBuf; break;
            case L'p': out += std::to_wstring(target.pid); break;
            case L'i': out += std::to_wstring(ordinal); break;
            case L'n': out += SafeFileNamePart(target.name); break;
            case L'%': out.push_back(L'%'); break;
            default:
                out.push_back(L'%');
                out.push_back(token);
                break;
        }
    }
    return out;
}

// 路径比较：不区分大小写（Windows 文件系统默认如此），并且刻意不走区域敏感规则 ——
// culture 比较会把某些看似不同的名字判成相等，Ordinal 只按码位简单折叠大小写。
bool SameWindowsPath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

std::wstring SafeFileNamePart(const std::wstring& raw) {
    std::wstring s;
    s.reserve(raw.size());
    for (wchar_t c : raw) s.push_back(IllegalNameChar(c) ? L'_' : c);
    TrimTrailingDotsAndSpaces(&s);

    if (s.size() > kMaxNameChars) {
        size_t cut = kMaxNameChars;
        // cut 是截断后的长度：切点上的低代理属于前一个高代理时，退一格，别把一个字劈成两半
        if (cut > 0 && IsLowSurrogate(s[cut]) && IsHighSurrogate(s[cut - 1])) --cut;
        s.resize(cut);
        while (!s.empty() && IsHighSurrogate(s.back())) s.pop_back();  // 落单的高代理同样非法
        TrimTrailingDotsAndSpaces(&s);  // 截断之后可能又露出尾部的点或空格
    }

    if (s.empty()) return L"_";
    if (IsReservedDeviceName(s)) s.insert(s.begin(), L'_');
    return s;
}

bool PlanOutputPaths(const Options& opt, const std::vector<OutputTarget>& targets,
                     std::vector<std::wstring>* paths, std::vector<Diagnostic>* notes,
                     Diagnostic* err) {
    paths->clear();
    if (targets.empty()) return true;

    // 标准输出不是一个可以展开的名字：整批都往同一条管道里写，占位符与扩展名一概不碰，
    // 也就没有"两个目标撞同一个路径"可言。
    if (opt.output == L"-") {
        paths->assign(targets.size(), L"-");
        return true;
    }

    const BatchClock clock = MakeBatchClock();
    const bool templated = opt.output.find(L'%') != std::wstring::npos;
    bool extensionNoted = false;

    for (size_t i = 0; i < targets.size(); ++i) {
        // 多于一个目标且模板里没有任何占位符时，才按原约定追加 _序号；
        // 有占位符就照模板，撞不撞由下面的检测负责。
        std::wstring name = (templated || targets.size() < 2)
                                ? Expand(opt.output, targets[i], i + 1, clock)
                                : AppendOrdinal(opt.output, i + 1);
        const bool neededExtension = !HasExtension(name);
        if (neededExtension) name += ExtensionFor(opt.format);
        const std::wstring absolute = AbsoluteOfOrRaw(name);

        for (size_t k = 0; k < paths->size(); ++k) {
            if (!SameWindowsPath(paths->at(k), absolute)) continue;
            // 第 k+1 张与第 i+1 张要写到同一个名字。改名字等于替调用方做决定（他给的模板就是这样），
            // 所以这里只报错：整批还没取帧，一张也不会落地。
            if (err) {
                *err = Diagnostic{codes::kOutputCollision, Msg(L"io.output_collision"), L"--out",
                                  opt.output, Msgf(L"io.output_collision_hint", k + 1, i + 1, absolute)};
            }
            paths->clear();
            return false;
        }
        paths->push_back(absolute);

        // 文件名被改了要说一声，否则调用方按自己给的名字去找会找不到。整批只发一条。
        if (neededExtension && !extensionNoted && notes) {
            notes->push_back(Diagnostic{codes::kOutputExtensionAppended,
                                        Msg(L"note.output_extension_appended"), L"--out", opt.output,
                                        Msgf(L"note.output_extension_hint", absolute)});
            extensionNoted = true;
        }
    }
    return true;
}

}  // namespace ecapture
