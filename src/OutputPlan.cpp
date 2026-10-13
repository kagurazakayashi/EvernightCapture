#include "OutputPlan.h"

#include <algorithm>
#include <string>
#include <vector>

#include "HostPath.h"
#include "Lang.h"

namespace ecapture {
namespace {

std::wstring TwoDigits(int v) {
    std::wstring s = std::to_wstring(v);
    if (s.size() < 2) s.insert(s.begin(), L'0');
    return s;
}

std::wstring FourDigits(int v) {
    std::wstring s = std::to_wstring(v);
    while (s.size() < 4) s.insert(s.begin(), L'0');
    return s;
}

// 一个批次的日期与时间：整批只取一次。分开取的话，跨午夜的批次会把同一次截图劈成两天的名字，
// 也让 %d-%t 这种"看时间戳找同一批"的用法失效。本地时间由平台服务给（与历史归档同一把尺）。
struct BatchClock {
    std::wstring date;  // YYYYMMDD
    std::wstring time;  // HHMMSS
};

BatchClock MakeBatchClock() {
    LocalDateTime local{};
    if (!LocalDateTimeNow(&local)) return BatchClock{};
    BatchClock clock;
    clock.date = FourDigits(local.year) + TwoDigits(local.month) + TwoDigits(local.day);
    clock.time = TwoDigits(local.hour) + TwoDigits(local.minute) + TwoDigits(local.second);
    return clock;
}

bool HasExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(PathSeparatorSet());
    const size_t dot = path.find_last_of(L'.');
    return dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash);
}

std::wstring AbsoluteOfOrRaw(const std::wstring& path) {
    std::wstring absolute;
    if (!NormalizeAbsolutePath(path, &absolute)) return path;   // 说不通就原样交回，由开文件那一步报错
    return absolute;
}

// 没有占位符时，把序号插在扩展名前面
std::wstring AppendOrdinal(std::wstring path, size_t ordinal) {
    const size_t slash = path.find_last_of(PathSeparatorSet());
    const size_t dot = path.find_last_of(L'.');
    const std::wstring suffix =
        (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
            ? path.substr(dot) : std::wstring();
    if (!suffix.empty()) path = path.substr(0, dot);
    path += L"_" + std::to_wstring(ordinal);
    path += suffix;
    return path;
}

// 句柄写成定宽十六进制。用平台无关的数字拼接，不为了一个 %08X 把这一层绑到某一家 C 运行期。
std::wstring HandleText(uint64_t hwnd) {
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring out(10, L'0');
    out[0] = L'0';
    out[1] = L'x';
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(9 - i)] =
            kHex[static_cast<size_t>((hwnd >> (4 * i)) & 0xF)];
    }
    return out;
}

// 占位符展开。窗口目标给句柄 / 进程 / 标题；屏幕目标没有前两者（取到 0），
// %n 用去掉 "\\.\" 前缀的设备名，这样 shot_%n.png 每块屏一个文件。
// 未知的 %x 原样留两个字符（现有约定），但留下的照样是两个目标共用的名字，
// 所以碰撞检测一视同仁。
std::wstring Expand(const std::wstring& pattern, const OutputTarget& target, size_t ordinal,
                    const BatchClock& clock) {
    std::wstring out;
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != L'%' || i + 1 >= pattern.size()) {
            out.push_back(pattern[i]);
            continue;
        }
        const wchar_t token = pattern[++i];
        switch (token) {
            case L'd': out += clock.date; break;
            case L't': out += clock.time; break;
            case L'h': out += HandleText(target.hwnd); break;
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

}  // namespace

std::wstring ExtensionFor(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::kJpeg: return L".jpg";
        case ImageFormat::kBmp: return L".bmp";
        case ImageFormat::kTiff: return L".tif";
        case ImageFormat::kGif: return L".gif";
        default: return L".png";
    }
}

std::wstring SafeFileNamePart(const std::wstring& raw) {
    return SanitizeFileNamePart(raw);
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
            // 平台自己的"同一个文件"判据：Windows 不区分大小写，POSIX 区分。
            if (!SamePath(paths->at(k), absolute)) continue;
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
