#pragma once
// 跨平台模型：把一次已经通过 XDG Desktop Portal 授权的 ScreenCast 来源转换成
// 一张可交付的 CPU 帧。
//
// 本层刻意不认识 libpipewire / spa / D-Bus 的类型：平台层只把已经映射好的内存区块、
// 协商出来的像素格式与缓冲区事实交进来，纯判据在本文件对应的 .cpp 里。这样下面这些
// 容易出错的规则可以在没有 Wayland、没有 PipeWire 服务、也不弹系统授权窗口的离线判据里
// 逐条注入：
//   * 只接受**当前 Portal 会话**返回的那一个 stream；窗口请求绝不接受 monitor 来源；
//   * 只接受可安全映射的 MemPtr / MemFd；DmaBuf、MemId、SyncObj 等明确报不支持；
//   * 校验行距、chunk offset / size / maxsize、损坏 / 空 chunk 与 64 位溢出；
//   * 在重新入队或销毁源缓冲之前，把像素复制进 CapturedFrame 自己的 std::vector；
//   * 拒绝请求开始前产生的缓存帧，并把尺寸变化报出来，不把旧尺寸当最新帧；
//   * 把 PipeWire 的 VideoTransform 转成顺时针角度；翻转无法保证语义时明确拒绝。
//
// 真实 PipeWire 驱动在 src/platform/linux/PipeWireCaptureLinux.cpp。它只接收
// OpenPipeWireRemote() 交回的 fd，不枚举系统 registry，也不连接任意节点。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "PortalScreenCast.h"
#include "core/Frame.h"

namespace ecapture {
namespace pipewire {

// 一次取帧的稳定结局。只有 kOk 表示 CapturedFrame 里有完整可交付的 BGRA8 像素。
enum class Outcome : uint32_t {
    kOk = 0,
    kUnavailable = 1,       // 这个构建没有 PipeWire 客户端 / 无法建立连接
    kUnsupported = 2,       // 协商格式、内存类型或视频变换无法兑现
    kTimeout = 3,           // 等到期限仍没有有效帧
    kCancelled = 4,         // 程序取消
    kDisconnected = 5,      // PipeWire 流 / 远端连接断开
    kNoFrame = 6,           // 连接还在，但没有拿到有效帧（空 / 损坏 / 全部过期）
    kFrameInvalid = 7,      // 帧形状或 payload 边界不自洽
    kSourceMismatch = 8,    // Portal 会话返回的不是请求的那一个来源
    kProtocolError = 9,     // 目标流元数据 / 协议回复不可核实
    kHdrUnverified = 10,    // 像素布局或色彩空间无法证明是 8 位 SDR
};

inline const char* OutcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::kOk: return "ok";
        case Outcome::kUnavailable: return "unavailable";
        case Outcome::kUnsupported: return "unsupported";
        case Outcome::kTimeout: return "timeout";
        case Outcome::kCancelled: return "cancelled";
        case Outcome::kDisconnected: return "disconnected";
        case Outcome::kNoFrame: return "no_frame";
        case Outcome::kFrameInvalid: return "frame_invalid";
        case Outcome::kSourceMismatch: return "source_mismatch";
        case Outcome::kProtocolError: return "protocol_error";
        case Outcome::kHdrUnverified: return "hdr_unverified";
    }
    return "protocol_error";
}

// 稳定原因 token（只增不改名；不随 --lang 变）。
namespace reason {
inline constexpr const char* kOk = "pipewire_frame_delivered";
inline constexpr const char* kLibraryNotCompiled = "pipewire_client_not_compiled";
inline constexpr const char* kTargetMissing = "portal_stream_target_missing";
inline constexpr const char* kTargetNotUnique = "portal_stream_target_not_unique";
inline constexpr const char* kSourceUnverified = "portal_source_type_unverified";
inline constexpr const char* kSourceMismatch = "portal_source_type_mismatch";
inline constexpr const char* kCanvasMissing = "pipewire_remote_fd_missing";
inline constexpr const char* kContextFailed = "pipewire_context_connect_failed";
inline constexpr const char* kStreamFailed = "pipewire_stream_connect_failed";
inline constexpr const char* kNoFrame = "pipewire_no_valid_frame";
inline constexpr const char* kDisconnected = "pipewire_stream_disconnected";
inline constexpr const char* kFormatUnsupported = "pipewire_format_unsupported";
inline constexpr const char* kMemoryUnsupported = "pipewire_memory_type_unsupported";
inline constexpr const char* kFrameInvalid = "pipewire_frame_invalid";
inline constexpr const char* kHdrUnverified = "pipewire_color_space_unverified";
inline constexpr const char* kTransformUnsupported = "pipewire_video_transform_unsupported";
inline constexpr const char* kProtocolError = "pipewire_protocol_error";
inline constexpr const char* kTimeout = "pipewire_frame_timeout";
inline constexpr const char* kCancelled = "application_cancelled";
}  // namespace reason

// spa_data.type 中本后端明确支持的两种 CPU 可读内存；其余取值只用于报告。
enum class MemoryType : uint32_t {
    kUnknown = 0,
    kMemPtr = 1,
    kMemFd = 2,
    kDmaBuf = 3,
    kMemId = 4,
    kSyncObj = 5,
};

inline const char* MemoryTypeName(MemoryType type) {
    switch (type) {
        case MemoryType::kMemPtr: return "mem_ptr";
        case MemoryType::kMemFd: return "mem_fd";
        case MemoryType::kDmaBuf: return "dma_buf";
        case MemoryType::kMemId: return "mem_id";
        case MemoryType::kSyncObj: return "sync_obj";
        case MemoryType::kUnknown: return "unknown";
    }
    return "unknown";
}

inline bool IsSupportedMemoryType(MemoryType type) {
    return type == MemoryType::kMemPtr || type == MemoryType::kMemFd;
}

// 只接受 4 字节/像素、能无损转成 BGRA8 的 packed RGB 布局。
// 10 位打包、FP16、YUV、平面格式都在枚举之外/明确拒绝，绝不按 4 字节硬解释。
enum class PixelFormat : uint32_t {
    kUnknown = 0,
    kBgra = 1,
    kRgba = 2,
    kBgrx = 3,
    kRgbx = 4,
    kXrgb = 5,
    kXbgr = 6,
    kArgb = 7,
    kAbgr = 8,
};

inline const char* PixelFormatName(PixelFormat format) {
    switch (format) {
        case PixelFormat::kBgra: return "BGRA";
        case PixelFormat::kRgba: return "RGBA";
        case PixelFormat::kBgrx: return "BGRx";
        case PixelFormat::kRgbx: return "RGBx";
        case PixelFormat::kXrgb: return "xRGB";
        case PixelFormat::kXbgr: return "xBGR";
        case PixelFormat::kArgb: return "ARGB";
        case PixelFormat::kAbgr: return "ABGR";
        case PixelFormat::kUnknown: return "unknown";
    }
    return "unknown";
}

inline bool IsSupportedPixelFormat(PixelFormat format) {
    return format >= PixelFormat::kBgra && format <= PixelFormat::kAbgr;
}

// PipeWire spa_meta_videotransform_value 的语义子集。数值保持与 SPA 枚举一致，
// 平台层只做一次静态转换，纯判据不 include spa 头。
enum class VideoTransform : uint32_t {
    kNone = 0,
    kRotate90Ccw = 1,
    kRotate180 = 2,
    kRotate270Ccw = 3,
    kFlipped = 4,
    kFlipped90 = 5,
    kFlipped180 = 6,
    kFlipped270 = 7,
};

// -1 = 这个变换不能由本后端无歧义地交付（含所有翻转）；否则是顺时针角度 0/90/180/270。
int ClockwiseDegrees(VideoTransform transform);

// 从 Portal Start 结果里挑出**唯一**授权流。要求 exactlyOneStream、source_type 可核实且与请求
// 一致、nodeId 非零；pipewire-serial 可用时交给调用方优先使用，绝不当 permanente 身份。
struct AuthorizedStreamTarget {
    uint32_t nodeId = 0;
    bool serialKnown = false;
    uint64_t serial = 0;
    portal::SourceKind sourceType = portal::SourceKind::kWindow;
};

bool SelectAuthorizedStream(const portal::Selection& selection,
                            portal::SourceKind expectedSource,
                            AuthorizedStreamTarget* out,
                            const char** reasonOut);

// 平台层交给纯判据的一段已映射内存事实。data 只在本次调用期间有效。
struct FrameBufferView {
    const uint8_t* data = nullptr;
    uint64_t dataMaxSize = 0;      // spa_data.maxsize
    uint32_t chunkOffset = 0;       // spa_chunk.offset
    uint32_t chunkSize = 0;         // spa_chunk.size
    int32_t stride = 0;             // spa_chunk.stride
    uint32_t chunkFlags = 0;        // SPA_CHUNK_FLAG_*
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::kUnknown;
    MemoryType memoryType = MemoryType::kUnknown;
    uint64_t bufferTimeNs = 0;      // pw_buffer.time（0 = 该版本没有这个字段）
    bool bufferTimeKnown = false;
};

// 单帧复制/校验的细分结局；Outcome 只取其中一族写到对外结果。
enum class CopyResult : uint32_t {
    kOk = 0,
    kZeroSize,
    kBadStride,
    kPayloadShort,
    kEmptyChunk,
    kCorruptedChunk,
    kUnsupportedFormat,
    kUnsupportedMemory,
    kTooLarge,
    kOverflow,
};

const char* CopyResultName(CopyResult result);

// 计算 packed BGRA 帧实际需要的 payload 字节数（最后一行只需要 rowBytes，不含行尾填充）。
// 参数不自洽时返回 0；调用方用 CopyResult 的细分原因给出诊断。
uint64_t RequiredPayloadBytes(uint32_t width, uint32_t height, int32_t stride);

// 把一段已映射的 PipeWire 缓冲区复制成紧凑 BGRA8（stride = width*4）。
// 必须在调用方归还/重新入队 PipeWire buffer 之前完成。失败时不改写 *out。
CopyResult CopyFrameToBgra(const FrameBufferView& view, CapturedFrame* out);

// 判断一帧能不能交给调用方。Fresh 表示时间戳（若有）不早于 minGraphNs；
// kAcceptSizeChanged 仍然可交付，但调用方必须记录并丢弃“旧尺寸”的结论。
enum class FrameDecision : uint32_t {
    kAccept = 0,
    kAcceptSizeChanged = 1,
    kStale = 2,
    kEmpty = 3,
    kCorrupted = 4,
    kUnsupportedFormat = 5,
    kUnsupportedMemory = 6,
    kInvalidShape = 7,
    kTimestampUnverified = 8,
};

FrameDecision DecideFrame(const FrameBufferView& view, uint64_t minGraphNs,
                          bool havePreviousSize, uint32_t previousWidth,
                          uint32_t previousHeight);

// 取帧等待循环的纯状态判据。生产后端每一步只问这一处，离线判据能确定地摆出
// “先取消 / 先断流 / 先超时 / 已拿到帧”这几种竞争，而不是只能靠真实服务碰运气。
enum class WaitDecision : uint32_t {
    kContinue = 0,
    kFrameReady = 1,
    kCancelled = 2,
    kDisconnected = 3,
    kFatal = 4,
    kTimedOut = 5,
};

WaitDecision DecideWait(bool gotFrame, bool fatal, bool disconnected, bool cancelRequested,
                        bool deadlineEnabled, uint64_t nowNs, uint64_t deadlineNs);

// 把已经复制的帧按 PipeWire VideoTransform 变成交付朝向。只接受 0/90/180/270 顺时针；
// 翻转返回 false 且不改 *frame。成功后保留 source / path / color / cursor 这些非像素事实。
bool RotateFrameClockwise(CapturedFrame* frame, uint32_t clockwiseDegrees);

struct CaptureRequest {
    portal::SourceKind expectedSource = portal::SourceKind::kWindow;
    portal::Selection selection;
    uint32_t timeoutMs = 2000;
    portal::CancelToken* cancelToken = nullptr;
};

struct CaptureResult {
    Outcome outcome = Outcome::kUnavailable;
    std::string reason = reason::kLibraryNotCompiled;
    CapturedFrame frame;

    MemoryType memoryType = MemoryType::kUnknown;
    PixelFormat pixelFormat = PixelFormat::kUnknown;
    uint32_t sourceWidth = 0;        // 旋转前的缓冲区像素尺寸
    uint32_t sourceHeight = 0;
    uint32_t rotationCw = 0;
    bool sizeChangedWhileWaiting = false;
    bool frameTimestampKnown = false;
    bool frameTimestampVerified = false;
    bool colorSpaceVerified = false;
    uint32_t emptyChunks = 0;
    uint32_t corruptedChunks = 0;
    uint32_t staleFrames = 0;
    uint32_t invalidFrames = 0;
    uint32_t sizeChanges = 0;

    bool serialKnown = false;
    uint64_t serial = 0;
    uint32_t nodeId = 0;
};

// PipeWire 只从 OpenPipeWireRemote() 交回的 fd 建立连接；fd 的所有权在调用成功后转给实现。
class IPipeWireCapture {
public:
    virtual ~IPipeWireCapture() = default;
    virtual CaptureResult Capture(int remoteFd, const CaptureRequest& request) = 0;
};

}  // namespace pipewire
}  // namespace ecapture
