// 【平台层 · Linux】XDG Desktop Portal ScreenCast 授权流 -> PipeWire CPU 帧。
//
// 官方依据（当前文档）：
//   * org.freedesktop.portal.ScreenCast v6：OpenPipeWireRemote() 交回的 fd 只能用于该会话的
//     ScreenCast 节点；node id 可复用，v6 起用 pipewire-serial + PW_KEY_TARGET_OBJECT；
//   * PipeWire SPA buffer：spa_data.type 标识内存类型，spa_chunk.offset/size/stride 描述有效载荷，
//     MemPtr / MemFd 是简单 CPU 可读类型，DmaBuf 等不保证可映射；
//   * pw_stream process 事件：dequeue 后必须在同一生命周期内复制/消费，再 queue；
//   * pw_buffer.time 可作为捕获缓冲的周期时间（自 1.0.5）。
//
// 本文件不枚举 registry、不连接任意系统节点，也不把 Portal node id 当永久身份保存。

#include "PipeWireCaptureLinux.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#if defined(ECAPTURE_HAVE_PIPEWIRE)
#include <fcntl.h>
#include <sys/mman.h>

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#endif

namespace ecapture {
namespace pipewire {
namespace {

#if !defined(ECAPTURE_HAVE_PIPEWIRE)

class UnavailablePipeWireCapture final : public IPipeWireCapture {
public:
    CaptureResult Capture(int remoteFd, const CaptureRequest&) override {
        if (remoteFd >= 0) ::close(remoteFd);
        CaptureResult result;
        result.outcome = Outcome::kUnavailable;
        result.reason = reason::kLibraryNotCompiled;
        return result;
    }
};

#else

using Clock = std::chrono::steady_clock;

uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now().time_since_epoch())
                                     .count());
}

uint32_t SpaFormatToPixelFormat(enum spa_video_format format) {
    switch (format) {
        case SPA_VIDEO_FORMAT_BGRA: return static_cast<uint32_t>(PixelFormat::kBgra);
        case SPA_VIDEO_FORMAT_RGBA: return static_cast<uint32_t>(PixelFormat::kRgba);
        case SPA_VIDEO_FORMAT_BGRx: return static_cast<uint32_t>(PixelFormat::kBgrx);
        case SPA_VIDEO_FORMAT_RGBx: return static_cast<uint32_t>(PixelFormat::kRgbx);
        case SPA_VIDEO_FORMAT_xRGB: return static_cast<uint32_t>(PixelFormat::kXrgb);
        case SPA_VIDEO_FORMAT_xBGR: return static_cast<uint32_t>(PixelFormat::kXbgr);
        case SPA_VIDEO_FORMAT_ARGB: return static_cast<uint32_t>(PixelFormat::kArgb);
        case SPA_VIDEO_FORMAT_ABGR: return static_cast<uint32_t>(PixelFormat::kAbgr);
        default: return static_cast<uint32_t>(PixelFormat::kUnknown);
    }
}

enum spa_video_format PixelFormatToSpa(PixelFormat format) {
    switch (format) {
        case PixelFormat::kBgra: return SPA_VIDEO_FORMAT_BGRA;
        case PixelFormat::kRgba: return SPA_VIDEO_FORMAT_RGBA;
        case PixelFormat::kBgrx: return SPA_VIDEO_FORMAT_BGRx;
        case PixelFormat::kRgbx: return SPA_VIDEO_FORMAT_RGBx;
        case PixelFormat::kXrgb: return SPA_VIDEO_FORMAT_xRGB;
        case PixelFormat::kXbgr: return SPA_VIDEO_FORMAT_xBGR;
        case PixelFormat::kArgb: return SPA_VIDEO_FORMAT_ARGB;
        case PixelFormat::kAbgr: return SPA_VIDEO_FORMAT_ABGR;
        case PixelFormat::kUnknown: break;
    }
    return SPA_VIDEO_FORMAT_UNKNOWN;
}

MemoryType SpaMemoryType(uint32_t type) {
    switch (type) {
        case SPA_DATA_MemPtr: return MemoryType::kMemPtr;
        case SPA_DATA_MemFd: return MemoryType::kMemFd;
        case SPA_DATA_DmaBuf: return MemoryType::kDmaBuf;
        case SPA_DATA_MemId: return MemoryType::kMemId;
        case SPA_DATA_SyncObj: return MemoryType::kSyncObj;
        default: return MemoryType::kUnknown;
    }
}

bool IsSrgbFullRgb(const struct spa_video_info_raw& info) {
    // 只交付能说清楚是 8 位 sRGB full-range RGB 的 packed 帧。属性缺失（0）与
    // 任何 PQ/HLG/BT.2020 组合都拒绝，绝不猜色调映射。
    const bool rangeOk = info.color_range == SPA_VIDEO_COLOR_RANGE_0_255 ||
                         info.color_range == SPA_VIDEO_COLOR_RANGE_UNKNOWN;
    const bool matrixOk = info.color_matrix == SPA_VIDEO_COLOR_MATRIX_RGB ||
                          info.color_matrix == SPA_VIDEO_COLOR_MATRIX_UNKNOWN;
    const bool transferOk = info.transfer_function == SPA_VIDEO_TRANSFER_SRGB ||
                            info.transfer_function == SPA_VIDEO_TRANSFER_UNKNOWN;
    const bool primariesOk = info.color_primaries == SPA_VIDEO_COLOR_PRIMARIES_BT709 ||
                              info.color_primaries == SPA_VIDEO_COLOR_PRIMARIES_UNKNOWN;
    return rangeOk && matrixOk && transferOk && primariesOk;
}

struct StreamingState {
    pw_main_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::string streamName;
    bool formatKnown = false;
    bool havePreviousFormatSize = false;
    bool colorVerified = false;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t stride = 0;
    PixelFormat pixelFormat = PixelFormat::kUnknown;
    MemoryType memoryType = MemoryType::kUnknown;
    uint32_t transformRaw = 0;
    bool transformKnown = false;
    uint64_t startedGraphNs = 0;
    bool gotFrame = false;
    bool fatal = false;
    bool disconnected = false;
    std::string error;
    uint32_t emptyChunks = 0;
    uint32_t corruptedChunks = 0;
    uint32_t staleFrames = 0;
    uint32_t invalidFrames = 0;
    uint32_t sizeChanges = 0;
    bool sizeChangedWhileWaiting = false;
    uint32_t previousWidth = 0;
    uint32_t previousHeight = 0;
    bool havePreviousSize = false;
    bool timestampKnown = false;
    bool timestampVerified = false;
    CapturedFrame frame;

    void Fail(const char* token) {
        if (!fatal) {
            fatal = true;
            error = token ? token : reason::kFrameInvalid;
        }
    }
};

void OnProcess(void* userdata) {
    auto* state = static_cast<StreamingState*>(userdata);
    if (!state || !state->stream || state->gotFrame || state->fatal) {
        // 仍然把可能已 dequeue 的缓冲归还，但不在已完成后继续交付。
        return;
    }
    struct pw_buffer* b = nullptr;
    while ((b = pw_stream_dequeue_buffer(state->stream)) != nullptr) {
        bool keep = false;
        struct spa_buffer* buf = b->buffer;
        if (!buf || buf->n_datas != 1) {
            // packed RGB 必须是一个数据块；平面 / 多块布局不在首版支持范围内。
            ++state->invalidFrames;
            state->Fail(reason::kFormatUnsupported);
            pw_stream_queue_buffer(state->stream, b);
            return;
        }

        struct spa_data& data = buf->datas[0];
        struct spa_chunk* chunk = data.chunk;
        if (struct spa_meta_videotransform* transform =
                static_cast<struct spa_meta_videotransform*>(spa_buffer_find_meta_data(
                    buf, SPA_META_VideoTransform, sizeof(struct spa_meta_videotransform)))) {
            state->transformKnown = true;
            state->transformRaw = transform->transform;
        }
        if (!chunk) {
            ++state->invalidFrames;
            pw_stream_queue_buffer(state->stream, b);
            continue;
        }

        FrameBufferView view;
        view.data = static_cast<const uint8_t*>(data.data);
        view.dataMaxSize = data.maxsize;
        view.chunkOffset = chunk->offset;
        view.chunkSize = chunk->size;
        view.stride = chunk->stride;
        view.chunkFlags = static_cast<uint32_t>(chunk->flags);
        view.width = state->width;
        view.height = state->height;
        view.format = state->pixelFormat;
        view.memoryType = SpaMemoryType(data.type);
        view.bufferTimeNs = b->time;
        view.bufferTimeKnown = b->time != 0;
        state->timestampKnown = view.bufferTimeKnown;

        // MemFd 在启用 MAP_BUFFERS 后通常已有 data.data；没有就临时 mmap，并在
        // queue_buffer 前解除映射。DmaBuf 永远不在这里硬当成普通内存。
        void* mapped = nullptr;
        bool temporaryMap = false;
        int mapFd = -1;
        if (view.memoryType == MemoryType::kDmaBuf) {
            ++state->invalidFrames;
            state->Fail(reason::kMemoryUnsupported);
            pw_stream_queue_buffer(state->stream, b);
            return;
        }
        if (view.memoryType == MemoryType::kMemFd && !view.data) {
            mapFd = static_cast<int>(data.fd);
            if (mapFd < 0 || data.maxsize == 0) {
                ++state->invalidFrames;
                pw_stream_queue_buffer(state->stream, b);
                continue;
            }
            const long page = sysconf(_SC_PAGESIZE);
            if (page <= 0 || (data.mapoffset % static_cast<uint32_t>(page)) != 0) {
                ++state->invalidFrames;
                state->Fail(reason::kFrameInvalid);
                pw_stream_queue_buffer(state->stream, b);
                return;
            }
            temporaryMap = true;
            mapped = mmap(nullptr, data.maxsize, PROT_READ, MAP_SHARED, mapFd,
                          static_cast<off_t>(data.mapoffset));
            if (mapped == MAP_FAILED) {
                mapped = nullptr;
                ++state->invalidFrames;
                pw_stream_queue_buffer(state->stream, b);
                continue;
            }
            view.data = static_cast<const uint8_t*>(mapped);
        }

        const FrameDecision decision =
            DecideFrame(view, state->startedGraphNs, state->havePreviousSize,
                        state->previousWidth, state->previousHeight);
        if (decision == FrameDecision::kEmpty) {
            ++state->emptyChunks;
            keep = false;
        } else if (decision == FrameDecision::kCorrupted) {
            ++state->corruptedChunks;
            keep = false;
        } else if (decision == FrameDecision::kStale) {
            ++state->staleFrames;
            keep = false;
        } else if (decision == FrameDecision::kUnsupportedFormat) {
            state->Fail(reason::kFormatUnsupported);
            keep = false;
        } else if (decision == FrameDecision::kUnsupportedMemory) {
            state->Fail(reason::kMemoryUnsupported);
            keep = false;
        } else if (decision == FrameDecision::kInvalidShape) {
            ++state->invalidFrames;
            keep = false;
        } else {
            if (view.bufferTimeKnown && state->startedGraphNs != 0 &&
                view.bufferTimeNs < state->startedGraphNs) {
                ++state->staleFrames;
                keep = false;
            } else {
                CapturedFrame copied;
                const CopyResult copy = CopyFrameToBgra(view, &copied);
                if (copy == CopyResult::kOk) {
                    state->memoryType = view.memoryType;
                    state->pixelFormat = view.format;
                    if (decision == FrameDecision::kAcceptSizeChanged) {
                        state->sizeChangedWhileWaiting = true;
                        ++state->sizeChanges;
                    }
                    state->timestampVerified = view.bufferTimeKnown;
                    if (state->transformKnown) {
                        const int cw = ClockwiseDegrees(
                            static_cast<VideoTransform>(state->transformRaw));
                        if (cw < 0 || !RotateFrameClockwise(&copied, static_cast<uint32_t>(cw))) {
                            state->Fail(reason::kTransformUnsupported);
                        }
                    }
                    if (!state->fatal) {
                        state->frame = std::move(copied);
                        state->gotFrame = true;
                    }
                    state->havePreviousSize = true;
                    state->previousWidth = view.width;
                    state->previousHeight = view.height;
                } else if (copy == CopyResult::kUnsupportedFormat) {
                    state->Fail(reason::kFormatUnsupported);
                } else if (copy == CopyResult::kUnsupportedMemory) {
                    state->Fail(reason::kMemoryUnsupported);
                } else if (copy == CopyResult::kEmptyChunk) {
                    ++state->emptyChunks;
                } else if (copy == CopyResult::kCorruptedChunk) {
                    ++state->corruptedChunks;
                } else {
                    ++state->invalidFrames;
                }
            }
        }

        if (temporaryMap && mapped) munmap(mapped, data.maxsize);
        // 复制/映射都已结束，才允许把系统缓冲重新入队。
        pw_stream_queue_buffer(state->stream, b);
        if (state->gotFrame || state->fatal) return;
        (void)keep;
    }
}

void OnParamChanged(void* userdata, uint32_t id, const struct spa_pod* param) {
    auto* state = static_cast<StreamingState*>(userdata);
    if (!state || !state->stream) return;
    if (id != SPA_PARAM_Format) return;
    if (!param) {
        state->formatKnown = false;
        return;
    }

    struct spa_video_info info;
    std::memset(&info, 0, sizeof(info));
    if (spa_format_video_parse(param, &info) < 0 ||
        info.media_type != SPA_MEDIA_TYPE_video ||
        info.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        state->Fail(reason::kFormatUnsupported);
        return;
    }
    const struct spa_video_info_raw& raw = info.info.raw;
    if (!IsSrgbFullRgb(raw)) {
        state->Fail(reason::kHdrUnverified);
        return;
    }

    const PixelFormat format = static_cast<PixelFormat>(SpaFormatToPixelFormat(raw.format));
    if (!IsSupportedPixelFormat(format) || raw.size.width == 0 || raw.size.height == 0 ||
        raw.size.width > kFrameMaxSide || raw.size.height > kFrameMaxSide) {
        state->Fail(reason::kFormatUnsupported);
        return;
    }

    if (state->havePreviousFormatSize &&
        (state->width != raw.size.width || state->height != raw.size.height)) {
        state->sizeChangedWhileWaiting = true;
        ++state->sizeChanges;
    }
    state->width = raw.size.width;
    state->height = raw.size.height;
    state->pixelFormat = format;
    state->colorVerified =
        raw.color_range == SPA_VIDEO_COLOR_RANGE_0_255 &&
        raw.color_matrix == SPA_VIDEO_COLOR_MATRIX_RGB &&
        raw.transfer_function == SPA_VIDEO_TRANSFER_SRGB &&
        raw.color_primaries == SPA_VIDEO_COLOR_PRIMARIES_BT709;
    state->formatKnown = true;
    state->havePreviousFormatSize = true;
    state->transformKnown = false;
    state->transformRaw = 0;

    // 只请求 CPU 可读的 MemPtr / MemFd；如果对端只给 DmaBuf，process 回调会明确拒绝。
    uint8_t paramsBuffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(paramsBuffer, sizeof(paramsBuffer));
    const struct spa_pod* params[1];
    params[0] = static_cast<const struct spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 8),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd))));
    pw_stream_update_params(state->stream, params, 1);
}

void OnStateChanged(void* userdata, enum pw_stream_state old, enum pw_stream_state state,
                    const char* error) {
    (void)old;
    auto* streamState = static_cast<StreamingState*>(userdata);
    if (!streamState) return;
    if (streamState->stream && streamState->startedGraphNs == 0 &&
        state == PW_STREAM_STATE_STREAMING) {
        streamState->startedGraphNs = pw_stream_get_nsec(streamState->stream);
    }
    if (state == PW_STREAM_STATE_ERROR) {
        streamState->disconnected = true;
        streamState->Fail(reason::kDisconnected);
        if (error && *error && streamState->error.empty()) streamState->error = error;
    } else if (state == PW_STREAM_STATE_UNCONNECTED) {
        streamState->disconnected = true;
    }
}

void OnAddBuffer(void* userdata, struct pw_buffer* buffer) {
    (void)userdata;
    (void)buffer;
}

void OnRemoveBuffer(void* userdata, struct pw_buffer* buffer) {
    (void)userdata;
    (void)buffer;
}

const struct pw_stream_events kStreamEvents = {
    PW_VERSION_STREAM_EVENTS,
    nullptr,            // destroy
    OnStateChanged,
    nullptr,            // control_info
    nullptr,            // io_changed
    OnParamChanged,
    OnAddBuffer,
    OnRemoveBuffer,
    OnProcess,
};

class LinuxPipeWireCapture final : public IPipeWireCapture {
public:
    CaptureResult Capture(int remoteFd, const CaptureRequest& request) override {
        CaptureResult result;
        if (remoteFd < 0) {
            result.outcome = Outcome::kProtocolError;
            result.reason = reason::kCanvasMissing;
            return result;
        }

        AuthorizedStreamTarget target;
        const char* targetReason = nullptr;
        if (!SelectAuthorizedStream(request.selection, request.expectedSource, &target,
                                    &targetReason)) {
            result.outcome = Outcome::kSourceMismatch;
            result.reason = targetReason ? targetReason : reason::kTargetMissing;
            ::close(remoteFd);
            return result;
        }
        result.nodeId = target.nodeId;
        result.serialKnown = target.serialKnown;
        result.serial = target.serial;

        pw_init(nullptr, nullptr);
        StreamingState state;
        state.streamName = "ecapture-portal-capture";

        state.loop = pw_main_loop_new(nullptr);
        if (!state.loop) {
            pw_deinit();
            ::close(remoteFd);
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kContextFailed;
            return result;
        }

        struct pw_context* context = pw_context_new(pw_main_loop_get_loop(state.loop), nullptr, 0);
        if (!context) {
            pw_main_loop_destroy(state.loop);
            pw_deinit();
            ::close(remoteFd);
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kContextFailed;
            return result;
        }

        // fd 的所有权从这一刻交给 pw_context；成功与失败路径都由 PipeWire 关闭。
        struct pw_core* core = pw_context_connect_fd(context, remoteFd, nullptr, 0);
        if (!core) {
            pw_context_destroy(context);
            pw_main_loop_destroy(state.loop);
            pw_deinit();
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kContextFailed;
            return result;
        }

        struct pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            PW_KEY_NODE_NAME, state.streamName.c_str(),
            PW_KEY_APP_NAME, "EvernightCapture",
            NULL);
        if (!props) {
            pw_core_disconnect(core);
            pw_context_destroy(context);
            pw_main_loop_destroy(state.loop);
            pw_deinit();
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kStreamFailed;
            return result;
        }

        // 当前协议有 pipewire-serial 时用 PW_KEY_TARGET_OBJECT；没有时只回退到
        // 当前 Portal 会话交回的 node id（target_id 参数，旧版兼容路径）。
        uint32_t targetId = PW_ID_ANY;
        if (target.serialKnown) {
            const std::string serial = std::to_string(target.serial);
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, serial.c_str());
        } else {
            targetId = target.nodeId;
        }

        state.stream = pw_stream_new_simple(pw_main_loop_get_loop(state.loop),
                                            state.streamName.c_str(), props, &kStreamEvents,
                                            &state);
        if (!state.stream) {
            pw_core_disconnect(core);
            pw_context_destroy(context);
            pw_main_loop_destroy(state.loop);
            pw_deinit();
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kStreamFailed;
            return result;
        }

        uint8_t paramsBuffer[4096];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(paramsBuffer, sizeof(paramsBuffer));
        const struct spa_pod* params[8];
        uint32_t nParams = 0;
        const PixelFormat kFormats[] = {
            PixelFormat::kBgra, PixelFormat::kRgba, PixelFormat::kBgrx,
            PixelFormat::kRgbx, PixelFormat::kXrgb, PixelFormat::kXbgr,
            PixelFormat::kArgb, PixelFormat::kAbgr,
        };
        for (PixelFormat format : kFormats) {
            struct spa_video_info_raw info;
            std::memset(&info, 0, sizeof(info));
            info.format = PixelFormatToSpa(format);
            info.color_range = SPA_VIDEO_COLOR_RANGE_0_255;
            info.color_matrix = SPA_VIDEO_COLOR_MATRIX_RGB;
            info.transfer_function = SPA_VIDEO_TRANSFER_SRGB;
            info.color_primaries = SPA_VIDEO_COLOR_PRIMARIES_BT709;
            params[nParams++] =
                spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
        }

        const int connectRes = pw_stream_connect(
            state.stream, PW_DIRECTION_INPUT, targetId,
            static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                              PW_STREAM_FLAG_MAP_BUFFERS |
                                              PW_STREAM_FLAG_DONT_RECONNECT),
            params, nParams);
        if (connectRes < 0) {
            pw_stream_destroy(state.stream);
            state.stream = nullptr;
            pw_core_disconnect(core);
            pw_context_destroy(context);
            pw_main_loop_destroy(state.loop);
            pw_deinit();
            result.outcome = Outcome::kUnavailable;
            result.reason = reason::kStreamFailed;
            return result;
        }

        const uint64_t startMono = NowNs();
        const uint64_t budgetNs =
            request.timeoutMs == 0 ? 0 : static_cast<uint64_t>(request.timeoutMs) * 1000000ull;
        const uint64_t deadline = budgetNs == 0 ? 0 : startMono + budgetNs;
        struct pw_loop* loop = pw_main_loop_get_loop(state.loop);
        pw_loop_enter(loop);
        for (;;) {
            const uint64_t now = NowNs();
            const WaitDecision decision =
                DecideWait(state.gotFrame, state.fatal, state.disconnected,
                           request.cancelToken != nullptr && request.cancelToken->Cancelled(),
                           deadline != 0, now, deadline);
            if (decision == WaitDecision::kFrameReady) break;
            if (decision == WaitDecision::kCancelled) {
                state.Fail(reason::kCancelled);
                break;
            }
            if (decision == WaitDecision::kDisconnected) {
                state.Fail(reason::kDisconnected);
                break;
            }
            if (decision == WaitDecision::kFatal) break;
            if (decision == WaitDecision::kTimedOut) {
                state.Fail(reason::kTimeout);
                break;
            }
            int waitMs = 25;
            if (deadline != 0) {
                const uint64_t leftNs = deadline - now;
                waitMs = static_cast<int>(std::min<uint64_t>(25, leftNs / 1000000ull + 1));
            }
            const int iterate = pw_loop_iterate(loop, waitMs);
            if (iterate < 0 && iterate != -EINTR) {
                state.disconnected = true;
                state.Fail(reason::kDisconnected);
                break;
            }
        }
        pw_loop_leave(loop);

        // 先销毁 stream（它拥有进程回调引用），再销毁 core/context/loop；此后不再有迟到回调
        // 能访问 StreamingState。所有映射都在 process 回调内、queue 之前解除。
        if (state.stream) {
            pw_stream_destroy(state.stream);
            state.stream = nullptr;
        }
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(state.loop);

        result.memoryType = state.memoryType;
        result.pixelFormat = state.pixelFormat;
        result.sourceWidth = state.width;
        result.sourceHeight = state.height;
        result.rotationCw = state.transformKnown
                                ? static_cast<uint32_t>(std::max(0, ClockwiseDegrees(
                                      static_cast<VideoTransform>(state.transformRaw))))
                                : 0;
        result.sizeChangedWhileWaiting = state.sizeChangedWhileWaiting;
        result.frameTimestampKnown = state.timestampKnown;
        result.frameTimestampVerified = state.timestampVerified;
        result.colorSpaceVerified = state.colorVerified;
        result.emptyChunks = state.emptyChunks;
        result.corruptedChunks = state.corruptedChunks;
        result.staleFrames = state.staleFrames;
        result.invalidFrames = state.invalidFrames;
        result.sizeChanges = state.sizeChanges;

        if (state.gotFrame) {
            result.outcome = Outcome::kOk;
            result.reason = reason::kOk;
            result.frame = std::move(state.frame);
        } else if (state.error == reason::kCancelled) {
            result.outcome = Outcome::kCancelled;
            result.reason = reason::kCancelled;
        } else if (state.error == reason::kTimeout) {
            result.outcome = Outcome::kTimeout;
            result.reason = reason::kTimeout;
        } else if (state.error == reason::kDisconnected || state.disconnected) {
            result.outcome = Outcome::kDisconnected;
            result.reason = reason::kDisconnected;
        } else if (state.error == reason::kMemoryUnsupported) {
            result.outcome = Outcome::kUnsupported;
            result.reason = reason::kMemoryUnsupported;
        } else if (state.error == reason::kFormatUnsupported ||
                   state.error == reason::kHdrUnverified) {
            result.outcome = state.error == reason::kHdrUnverified ? Outcome::kHdrUnverified
                                                                   : Outcome::kUnsupported;
            result.reason = state.error;
        } else if (state.error == reason::kTransformUnsupported) {
            result.outcome = Outcome::kUnsupported;
            result.reason = reason::kTransformUnsupported;
        } else if (state.error == reason::kFrameInvalid) {
            result.outcome = Outcome::kFrameInvalid;
            result.reason = reason::kFrameInvalid;
        } else {
            result.outcome = Outcome::kNoFrame;
            result.reason = reason::kNoFrame;
        }
        pw_deinit();
        return result;
    }
};

#endif  // ECAPTURE_HAVE_PIPEWIRE

}  // namespace

std::unique_ptr<IPipeWireCapture> CreatePipeWireCapture() {
#if defined(ECAPTURE_HAVE_PIPEWIRE)
    return std::make_unique<LinuxPipeWireCapture>();
#else
    return std::make_unique<UnavailablePipeWireCapture>();
#endif
}

}  // namespace pipewire
}  // namespace ecapture
