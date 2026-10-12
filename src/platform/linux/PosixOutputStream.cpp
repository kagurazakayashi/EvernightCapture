// 【平台层 · Linux】标准流写出：UTF-8 字节直写 fd 1 / fd 2。
//
// 与 Windows 那一份同一套通道规矩（图片占用 stdout 后文字一律改道、原生码只作核对、
// 实际发出的字节数照实交回），动作换成本平台的：
//   * 内部文字在出口处编一次 UTF-8（src/core/TextCoding.h），不经 CRT 的文本模式与 locale
//     转换；行尾按内部写法原样送出（内部只写 \n，本平台不做 CRLF 归一）。
//   * 直接写文件描述符而不是 stdio：本程序可能在同一份 stdout 上先发图再判失败，
//     stdio 的缓冲会把"到底排出去几个字节"这件事说不清。
//   * EPIPE（读的一边先关）与 ENOSPC（磁盘/管道满）是两种故障，原生码分开交回，不并成一句话。
//   * 短写要继续写：一次 write() 少于请求字节数是正常情况，不当成失败。
#include "OutputStream.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <vector>

#include "core/TextCoding.h"

namespace ecapture {
namespace {

// 图片写 stdout 的那个时点记在这里：响应组装与入口的兜底路径都按同一个事实决定结果送去
// 哪条流，而不是各猜一次给出互相矛盾的答复。
bool g_stdoutClaimed = false;

int FdOf(FILE* stream) {
    const int fd = fileno(stream);
    return fd < 0 ? -1 : fd;
}

// 写完整一份字节；返回 false 时 \*native 是最后一个失败调用的 errno，\*written 是实际
// 排出去的字节数（可能是 0，也可能停在中间）。
bool WriteAll(int fd, const void* data, size_t size, NativeErrorCode* native, size_t* written) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const ssize_t n = ::write(fd, bytes + offset, size - offset);
        if (n > 0) {
            offset += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;  // 被信号打断不算失败
        if (native) *native = static_cast<NativeErrorCode>(errno);
        if (written) *written = offset;
        return false;
    }
    if (written) *written = offset;
    return true;
}

bool EmitText(FILE* stream, const std::wstring& text) {
    const int fd = FdOf(stream);
    if (fd < 0) return false;
    const std::string body = EncodeUtf8(text);  // 内部文本只写 \n，本平台不加转换层
    NativeErrorCode native = kNativeErrorNone;
    return WriteAll(fd, body.data(), body.size(), &native, nullptr);
}

}  // namespace

bool EmitStdout(const std::wstring& text) {
    // 图片已占用 stdout 时这里就是硬边界：文字一律改道，绝不让 JSON 混进 PNG 里。
    // 返回 false 表示"没送到约定通道"，由调用方决定退出码（见 src/Main.cpp）。
    if (StdoutClaimed()) return false;
    return EmitText(stdout, text);
}

bool EmitStderrRaw(const std::wstring& text) { return EmitText(stderr, text); }

void ClaimStdout() { g_stdoutClaimed = true; }

bool StdoutClaimed() { return g_stdoutClaimed; }

bool EmitStdoutBytes(const std::vector<uint8_t>& bytes, NativeErrorCode* ioError,
                     uint64_t* emittedBytes) {
    if (emittedBytes) *emittedBytes = 0;
    if (ioError) *ioError = kNativeErrorNone;
    const int fd = FdOf(stdout);
    if (fd < 0) {
        if (ioError) *ioError = static_cast<NativeErrorCode>(EBADF);
        return false;
    }
    // 写第一块字节之前就声明归属：哪怕这次只发出去半张图，stdout 也不能再给文字用。
    ClaimStdout();
    size_t written = 0;
    NativeErrorCode native = kNativeErrorNone;
    const bool ok = WriteAll(fd, bytes.data(), bytes.size(), &native, &written);
    if (ioError) *ioError = native;
    if (emittedBytes) *emittedBytes = written;
    return ok;
}

}  // namespace ecapture
