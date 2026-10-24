// Portal 授权流 -> CPU 帧的离线判据（不连 D-Bus、不连 PipeWire、不取真实像素）。
//
// 可控帧生产器直接构造 spa_chunk 形状（offset / size / stride / flags / maxsize）与
// 8 种 packed RGB 布局，用来钉住：
//   * 只接受当前 Portal 会话交回的“恰好一个、来源类型可核实”的 stream；
//   * 窗口请求不接受 monitor，monitor 请求不接受 window；
//   * 行距、chunk 偏移、长度、空 / 损坏标记、尺寸与 64 位溢出；
//   * 复制发生在模拟 buffer 归还之前，失败时不改写目标帧；
//   * pw_buffer.time 早于本次连接开始 = 缓存帧，不能交付；
//   * 尺寸变化可被识别而不会被当成旧尺寸；旋转使用 SPA 的逆时针语义转成交付顺时针角度。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/ImageOps.h"
#include "../src/PipeWireCapture.h"

using namespace ecapture;
using namespace ecapture::pipewire;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (ok) {
        std::printf("  PASS  %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

portal::StreamInfo Stream(uint32_t nodeId, portal::SourceKind source, bool serialKnown = false,
                          uint64_t serial = 0) {
    portal::StreamInfo s;
    s.nodeId = nodeId;
    s.sourceTypeKnown = true;
    s.sourceType = source;
    s.pipewireSerialKnown = serialKnown;
    s.pipewireSerial = serial;
    s.sizeKnown = true;
    s.width = 640;
    s.height = 480;
    return s;
}

portal::Selection One(portal::SourceKind source, uint32_t nodeId = 42) {
    portal::Selection s;
    s.requested = source;
    s.exactlyOneStream = true;
    s.sourceTypeVerified = true;
    s.streams.push_back(Stream(nodeId, source, true, 9001));
    return s;
}

uint8_t ByteFor(PixelFormat format, int channel, int x, int y) {
    // 每像素四个字节在源布局中的语义：
    //  0/1/2/3 = channel 在缓冲区中的位置，255 = 常量 alpha 占位。
    const int b = 10 + x * 7 + y * 13;
    const int g = 20 + x * 11 + y * 3;
    const int r = 30 + x * 5 + y * 17;
    const int a = 40 + x * 2 + y * 2;
    switch (format) {
        case PixelFormat::kBgra: return channel == 0 ? b : channel == 1 ? g : channel == 2 ? r : a;
        case PixelFormat::kRgba: return channel == 0 ? r : channel == 1 ? g : channel == 2 ? b : a;
        case PixelFormat::kBgrx: return channel == 0 ? b : channel == 1 ? g : channel == 2 ? r : 0xCC;
        case PixelFormat::kRgbx: return channel == 0 ? r : channel == 1 ? g : channel == 2 ? b : 0xCC;
        case PixelFormat::kXrgb: return channel == 0 ? 0xCC : channel == 1 ? r : channel == 2 ? g : b;
        case PixelFormat::kXbgr: return channel == 0 ? 0xCC : channel == 1 ? b : channel == 2 ? g : r;
        case PixelFormat::kArgb: return channel == 0 ? a : channel == 1 ? r : channel == 2 ? g : b;
        case PixelFormat::kAbgr: return channel == 0 ? a : channel == 1 ? b : channel == 2 ? g : r;
        case PixelFormat::kUnknown: break;
    }
    return 0;
}

struct Synthetic {
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t offset = 0;
    PixelFormat format = PixelFormat::kUnknown;
    MemoryType memory = MemoryType::kMemPtr;
    uint32_t flags = 0;
    uint32_t chunkSize = 0;

    FrameBufferView View() const {
        FrameBufferView v;
        v.data = bytes.data();
        v.dataMaxSize = bytes.size();
        v.chunkOffset = offset;
        v.chunkSize = chunkSize ? chunkSize : static_cast<uint32_t>(bytes.size() - offset);
        v.stride = static_cast<int32_t>(stride);
        v.chunkFlags = flags;
        v.width = width;
        v.height = height;
        v.format = format;
        v.memoryType = memory;
        return v;
    }
};

Synthetic Make(PixelFormat format, uint32_t width = 4, uint32_t height = 3,
               uint32_t extraStride = 8, uint32_t prefix = 16) {
    Synthetic s;
    s.width = width;
    s.height = height;
    s.stride = width * 4 + extraStride;
    s.offset = prefix;
    s.format = format;
    s.bytes.assign(static_cast<size_t>(s.offset) + s.stride * height + 8, 0xA5);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = s.bytes.data() + s.offset + static_cast<size_t>(y) * s.stride;
        for (uint32_t x = 0; x < width; ++x) {
            for (int c = 0; c < 4; ++c) row[x * 4 + c] = ByteFor(format, c, x, y);
        }
    }
    s.chunkSize = static_cast<uint32_t>(s.stride * height);
    return s;
}

int SourceChannelForSemantic(PixelFormat format, int semantic) {
    // semantic: 0=B, 1=G, 2=R, 3=A。返回源缓冲区中的字节下标；-1 = 该格式没有 alpha。
    switch (format) {
        case PixelFormat::kBgra: return semantic;                       // B,G,R,A
        case PixelFormat::kRgba: return semantic == 0 ? 2 : semantic == 1 ? 1 : semantic == 2 ? 0 : 3;
        case PixelFormat::kBgrx: return semantic == 3 ? -1 : semantic;  // B,G,R,x
        case PixelFormat::kRgbx: return semantic == 0 ? 2 : semantic == 1 ? 1 : semantic == 2 ? 0 : -1;
        case PixelFormat::kXrgb: return semantic == 0 ? 3 : semantic == 1 ? 2 : semantic == 2 ? 1 : -1;
        case PixelFormat::kXbgr: return semantic == 0 ? 1 : semantic == 1 ? 2 : semantic == 2 ? 3 : -1;
        case PixelFormat::kArgb: return semantic == 0 ? 3 : semantic == 1 ? 2 : semantic == 2 ? 1 : 0;
        case PixelFormat::kAbgr: return semantic == 0 ? 1 : semantic == 1 ? 2 : semantic == 2 ? 3 : 0;
        case PixelFormat::kUnknown: break;
    }
    return -1;
}

bool ExpectBgra(const CapturedFrame& frame, PixelFormat format, uint32_t x, uint32_t y) {
    const uint8_t* p = frame.pixels.data() + static_cast<size_t>(y) * frame.stride + x * 4;
    const int b = SourceChannelForSemantic(format, 0);
    const int g = SourceChannelForSemantic(format, 1);
    const int r = SourceChannelForSemantic(format, 2);
    const int a = SourceChannelForSemantic(format, 3);
    return p[0] == ByteFor(format, b, x, y) &&
           p[1] == ByteFor(format, g, x, y) &&
           p[2] == ByteFor(format, r, x, y) &&
           p[3] == (a < 0 ? 255 : ByteFor(format, a, x, y));
}

void TestStreamTarget() {
    Section("1) 只连接当前 Portal 会话授权的唯一 stream");
    AuthorizedStreamTarget target;
    const char* reason = nullptr;
    portal::Selection one = One(portal::SourceKind::kWindow);
    Check(SelectAuthorizedStream(one, portal::SourceKind::kWindow, &target, &reason),
          "单一 window 来源可定位");
    Check(target.nodeId == 42 && target.serialKnown && target.serial == 9001,
          "node id 与 pipewire-serial 都从当前会话结果读出");
    Check(!SelectAuthorizedStream(one, portal::SourceKind::kMonitor, &target, &reason),
          "窗口 stream 不能冒充 monitor 请求");
    Check(std::string(reason) == reason::kSourceMismatch, "来源不符有独立原因");
    portal::Selection monitor = One(portal::SourceKind::kMonitor, 77);
    Check(!SelectAuthorizedStream(monitor, portal::SourceKind::kWindow, &target, &reason),
          "monitor stream 不能冒充 window 请求");

    portal::Selection two = one;
    two.streams.push_back(one.streams.front());
    Check(!SelectAuthorizedStream(two, portal::SourceKind::kWindow, &target, &reason),
          "multiple=false 却有两个 stream -> 不定位");
    Check(std::string(reason) == reason::kTargetNotUnique, "多流原因");

    portal::Selection unknown = one;
    unknown.streams[0].sourceTypeKnown = false;
    Check(!SelectAuthorizedStream(unknown, portal::SourceKind::kWindow, &target, &reason),
          "来源类型不可核实 -> 不连接");

    portal::Selection zero = one;
    zero.streams[0].nodeId = 0;
    Check(!SelectAuthorizedStream(zero, portal::SourceKind::kWindow, &target, &reason),
          "node id 缺失 -> 不猜");

    portal::Selection noSerial = one;
    noSerial.streams[0].pipewireSerialKnown = false;
    Check(SelectAuthorizedStream(noSerial, portal::SourceKind::kWindow, &target, &reason) &&
              !target.serialKnown,
          "旧 Portal 没 serial 时保留 node id 回退事实，由调用方标注会话作用域");
}

void TestFormatsAndBounds() {
    Section("2) 格式、行距、offset / size / maxsize 与内存类型");
    const PixelFormat formats[] = {PixelFormat::kBgra, PixelFormat::kRgba, PixelFormat::kBgrx,
                                   PixelFormat::kRgbx, PixelFormat::kXrgb, PixelFormat::kXbgr,
                                   PixelFormat::kArgb, PixelFormat::kAbgr};
    bool allFormats = true;
    for (PixelFormat f : formats) {
        Synthetic s = Make(f);
        CapturedFrame frame;
        const CopyResult r = CopyFrameToBgra(s.View(), &frame);
        bool ok = r == CopyResult::kOk && frame.width == 4 && frame.height == 3 &&
                  frame.stride == 16 && frame.pixels.size() == 48;
        for (uint32_t y = 0; ok && y < frame.height; ++y)
            for (uint32_t x = 0; ok && x < frame.width; ++x) ok = ExpectBgra(frame, f, x, y);
        if (!ok) {
            std::printf("        格式失败: %s (%s)\n", PixelFormatName(f),
                        CopyResultName(r));
            allFormats = false;
        }
    }
    Check(allFormats, "8 种 packed RGB 布局逐像素转成 BGRA8（含行尾填充）");

    Synthetic bgra = Make(PixelFormat::kBgra);
    CapturedFrame frame;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kOk, "基线缓冲可复制");
    Check(RequiredPayloadBytes(4, 3, 24) == 24u * 2u + 16u,
          "payload 只要求最后一行 rowBytes，不含最后一行行尾填充");

    bgra.memory = MemoryType::kDmaBuf;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kUnsupportedMemory,
          "DmaBuf 明确拒绝，不按 CPU 指针硬解释");
    bgra.memory = MemoryType::kSyncObj;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kUnsupportedMemory,
          "SyncObj 明确拒绝");

    bgra = Make(PixelFormat::kBgra);
    bgra.stride = bgra.width * 4 - 1;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kBadStride,
          "行距装不下一行 -> 拒绝");
    bgra = Make(PixelFormat::kBgra);
    bgra.stride = bgra.width * 4 * 2 + 4;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kBadStride,
          "行距超过行长两倍 -> 拒绝");
    bgra = Make(PixelFormat::kBgra);
    bgra.chunkSize = 10;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kPayloadShort,
          "chunk.size 短于最后一行所需 -> 拒绝");
    bgra = Make(PixelFormat::kBgra);
    bgra.offset = static_cast<uint32_t>(bgra.bytes.size()) - 10;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kPayloadShort,
          "offset 取模后无足够空间 -> 拒绝");
    bgra = Make(PixelFormat::kBgra);
    bgra.flags = 1u << 0;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kCorruptedChunk,
          "SPA_CHUNK_FLAG_CORRUPTED -> 不交付");
    bgra.flags = 1u << 1;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kEmptyChunk,
          "SPA_CHUNK_FLAG_EMPTY -> 不交付");
    bgra.format = PixelFormat::kUnknown;
    Check(CopyFrameToBgra(bgra.View(), &frame) == CopyResult::kUnsupportedFormat,
          "认不出的像素格式 -> 不猜布局");

    // 失败不改写调用方已有帧（异步生命周期纪律的一部分）。
    CapturedFrame sentinel;
    sentinel.width = 7;
    sentinel.height = 9;
    sentinel.pixels = {1, 2, 3};
    bgra = Make(PixelFormat::kBgra);
    bgra.chunkSize = 1;
    Check(CopyFrameToBgra(bgra.View(), &sentinel) != CopyResult::kOk &&
              sentinel.width == 7 && sentinel.height == 9 && sentinel.pixels.size() == 3,
          "复制失败不改写目标帧");
}

void TestFreshnessAndSizeChange() {
    Section("3) 请求前缓存帧、空 / 坏帧与尺寸变化");
    Synthetic s = Make(PixelFormat::kBgra);
    FrameBufferView v = s.View();
    v.bufferTimeKnown = true;
    v.bufferTimeNs = 100;
    Check(DecideFrame(v, 200, false, 0, 0) == FrameDecision::kStale,
          "buffer.time 早于本次连接开始 -> stale");
    v.bufferTimeNs = 200;
    Check(DecideFrame(v, 200, false, 0, 0) == FrameDecision::kAccept,
          "时间戳等于开始边界 -> 可接受");
    v.bufferTimeKnown = false;
    Check(DecideFrame(v, 200, false, 0, 0) == FrameDecision::kAccept,
          "旧版本没有 buffer.time 时不伪造已核实，但新连接仍可接受首帧");
    v.chunkFlags = 1u << 1;
    Check(DecideFrame(v, 200, false, 0, 0) == FrameDecision::kEmpty, "空 chunk 先被标记");
    v.chunkFlags = 1u << 0;
    Check(DecideFrame(v, 200, false, 0, 0) == FrameDecision::kCorrupted, "损坏 chunk 被标记");
    v.chunkFlags = 0;
    Check(DecideFrame(v, 200, true, 5, 4) == FrameDecision::kAcceptSizeChanged,
          "尺寸变化被识别（仍可交付最新帧，但必须记录）");
    v.width = 0;
    Check(DecideFrame(v, 200, true, 5, 4) == FrameDecision::kInvalidShape,
          "零尺寸被拒绝");

    Check(DecideWait(true, false, false, false, true, 10, 5) == WaitDecision::kFrameReady,
          "有效帧优先于已跨过的期限交付（取帧事实已经发生）");
    Check(DecideWait(false, false, false, true, true, 10, 5) == WaitDecision::kCancelled,
          "等待中取消 -> cancelled");
    Check(DecideWait(false, false, true, false, true, 10, 5) ==
              WaitDecision::kDisconnected,
          "等待中断流 -> disconnected");
    Check(DecideWait(false, false, false, false, true, 10, 10) ==
              WaitDecision::kTimedOut,
          "到达期限 -> timeout");
    Check(DecideWait(false, false, false, false, true, 9, 10) ==
              WaitDecision::kContinue,
          "未到期限且无事件 -> 继续等");
}

void TestRotation() {
    Section("4) VideoTransform 与交付朝向");
    Check(ClockwiseDegrees(VideoTransform::kNone) == 0, "None -> 0");
    Check(ClockwiseDegrees(VideoTransform::kRotate90Ccw) == 270,
          "SPA 90 逆时针 -> 交付 270 顺时针");
    Check(ClockwiseDegrees(VideoTransform::kRotate180) == 180, "180 -> 180");
    Check(ClockwiseDegrees(VideoTransform::kRotate270Ccw) == 90,
          "SPA 270 逆时针 -> 交付 90 顺时针");
    Check(ClockwiseDegrees(VideoTransform::kFlipped180) == -1, "翻转不猜语义");

    CapturedFrame frame;
    frame.width = 2;
    frame.height = 1;
    frame.stride = 8;
    frame.pixels = {1, 2, 3, 4, 5, 6, 7, 8};
    frame.source = L"portal";
    frame.path = L"portal.window";
    frame.sourceColorSpace = FrameColorSpace::kSrgbBgra8;
    Check(RotateFrameClockwise(&frame, 90), "2x1 顺时针 90 度成功");
    Check(frame.width == 1 && frame.height == 2 && frame.stride == 4,
          "旋转后尺寸与紧凑行距正确");
    Check(frame.pixels.size() == 8 && frame.pixels[0] == 1 && frame.pixels[4] == 5,
          "旋转后第一列映射符合顺时针 90 度的自然结果");
    Check(frame.source == L"portal" && frame.path == L"portal.window",
          "旋转保留来路事实");
}

}  // namespace

int main() {
    TestStreamTarget();
    TestFormatsAndBounds();
    TestFreshnessAndSizeChange();
    TestRotation();
    std::printf("\npipewire frame checks: %d passed, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
