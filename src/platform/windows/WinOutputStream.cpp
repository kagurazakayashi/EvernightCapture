// 【平台层 · Windows】标准流写出：UTF-8 直写句柄，绕开 CRT 文本模式。
//
// 行为与 ECAPTURE 一直在 Windows 上的写法逐字相同：
//   * GetStdHandle + WriteFile，不经 stdout/printf（那会按控制台代码页转换，中文先在那一步坏）；
//   * 行尾统一成 CRLF，避免老式控制台阶梯错位；
//   * 图片按 stdout 交付时块状写出，失败当场取 GetLastError（晚一步就被别的 API 覆盖了），
//     并把**实际排出去**的字节数交回调用方。
#include "OutputStream.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace ecapture {
namespace {

// 图片写 stdout 的那个时点记在这里：BuildResponse 与 Main 的兜底路径都按同一个事实决定
// 结果送去哪条流，而不是各猜一次给出互相矛盾的答复。
bool g_stdoutClaimed = false;

std::wstring NormalizeNewlines(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'\r' && i + 1 < in.size() && in[i + 1] == L'\n') continue;  // 已有 CRLF
        if (in[i] == L'\n') out += L"\r\n";
        else out.push_back(in[i]);
    }
    return out;
}

bool WriteHandle(DWORD which, const std::wstring& text) {
    HANDLE handle = GetStdHandle(which);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    const std::wstring body = NormalizeNewlines(text);
    const int need = WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    std::vector<char> buf(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()), buf.data(), need,
                        nullptr, nullptr);
    DWORD written = 0;
    return WriteFile(handle, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == static_cast<DWORD>(buf.size());
}

}  // namespace

bool EmitStdout(const std::wstring& text) {
    // 图片已占用 stdout 时这里就是硬边界：文字一律改道，绝不让 JSON 混进 PNG 里。
    // 返回 false 表示"没送到约定通道"，由调用方决定退出码（见 src/Main.cpp）。
    if (StdoutClaimed()) return false;
    return WriteHandle(STD_OUTPUT_HANDLE, text);
}

bool EmitStderrRaw(const std::wstring& text) { return WriteHandle(STD_ERROR_HANDLE, text); }

void ClaimStdout() { g_stdoutClaimed = true; }

bool StdoutClaimed() { return g_stdoutClaimed; }

bool EmitStdoutBytes(const std::vector<uint8_t>& bytes, NativeErrorCode* ioError,
                     uint64_t* emittedBytes) {
    if (emittedBytes) *emittedBytes = 0;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        // 句柄本身就取不到：这里 GetLastError 早就不是失败原因了，照实报"句柄无效"
        if (ioError) *ioError = ERROR_INVALID_HANDLE;
        return false;
    }
    // 写第一块字节之前就声明归属：哪怕这次只发出去半张图，stdout 也不能再给文字用。
    ClaimStdout();
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        const BOOL ok = WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr);
        if (!ok || written != chunk) {
            // 立刻取错误码：断管 / 磁盘满 / 句柄失效在这一步是三种不同的故障，
            // 调用方要靠它区分，晚一步就被后续 API 覆盖了。
            if (ioError) *ioError = GetLastError();
            // 已经排出去的那一段收不回来：把它照实交回去，别让调用方以为管道还是干净的。
            if (emittedBytes) *emittedBytes = ok ? offset + written : offset;
            return false;
        }
        offset += written;
    }
    if (emittedBytes) *emittedBytes = offset;
    return true;
}

}  // namespace ecapture
