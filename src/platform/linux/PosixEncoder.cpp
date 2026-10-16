// 【平台层 · Linux】编码实现：libpng（PNG）+ libjpeg（JPEG）+ 自写的 32 位 BMP。
//
// 依赖选择（记进 README 的《许可》一节与《图片编码》那一节）：
//   * libpng —— PNG 的参考实现，1.6.x 分支，许可证是自带那份 BSD 风格的《libpng License》；
//     zlib 由它自己依赖（zlib 许可证）。PNG 的 deflate 与 CRC 不自己写：那两处一旦写错，
//     症状是"某些查看器读不出"，而不是本机报错。
//   * libjpeg —— JPEG 基线编码，本机由 libjpeg-turbo 提供（IJG 许可证 + 那几条 BSD-3 与 MIT
//     的组件许可证）。质量参数就是 --quality 那一条，与 Windows 侧的 ImageQuality 同一口径。
//   * BMP —— 不引第三方：32 位 BI_RGB 的容器只有 14+40 字节头与逐行像素，判得住也测得起。
//   * TIFF 与 GIF —— 明确不带（core/CodecCapability.h 登记为 kNoEncoder）。没有 libtiff 就
//     写不出 TIFF；GIF 只能写 256 色调色板，把真彩截图硬压成调色板是一条会静默改变画面的降级。
//     两条都在能力表与实请求里同一句答复，不拿"能写出一个文件"冒充支持。
//
// 这一份只兑现 EncodeFrameNative()：帧形状、紧凑行、颜色空间、格式支持与开工预算
// 全由 src/Encoder.cpp 判完（一条判据只写一处）。这里额外守的是两条本层自己的线：
//   1. **预算**：三家库都没有"半途终止"的接口，所以中断点摆在它们交还给我们的那两处回调上
//      （libpng 的 sink、libjpeg 的 empty_output_buffer / term_destination、逐行喂数据的那一步，
//      以及 BMP 的逐行）。到点就从库里跳回本层，交回 capture.timeout —— 不是"先编完整张图
//      再看有没有超时"。库内部那一段不可中断的压缩（一次 deflate 块、一次熵编码段）照实写进
//      README，不假装能停。
//   2. **产出上限**：编码字节一路累加进调用方那份 vector，每一块都先核对
//      "已有 + 本次 ≤ kFrameMaxBytes"，超了就当场失败。不拿"分配失败抛异常"当检查，
//      也不让一次编码换来越过整帧预算的分配。中途到点或超限都不交出已经写出的那半段字节。
//
// 通道顺序与 alpha：帧里的字节是 B,G,R,A。
//   * PNG 写 8 位 RGBA（color type 6）：逐行把 B 与 R 交换，alpha 原样带上，不合成、不猜预乘。
//   * BMP 写 32 位 BI_RGB：内存顺序本来就是 B,G,R,A，按行倒序（bottom-up）搬，alpha 那一路
//     在 BI_RGB 里不参与合成，照原字节写出（不谎称这一格式表示透明）。
//   * JPEG 没有 alpha 通道：只喂 R,G,B 三个分量，alpha 不参与任何合成 —— 这一条与 Windows 侧
//     BitmapAlphaMode::Ignore 同一口径："这条格式不表示透明"，不等于"这一帧是全不透明的"。
//     颜色由库按 JFIF 的常规做 RGB→YCbCr（质量 ≥90 时不色度抽样），与任何标准解码器读回的同源。
//   三条都不做预乘/去预乘的猜测：取帧那层交回什么字节就写什么字节。
//
// 与显示服务器无关：本文件一个 X11 / Wayland / GTK / wx 的符号都不 include，也不初始化 GUI；
// 判据里有一条在无 DISPLAY 的会话真编一张图（tests/encode_state.cpp）。

#include "Encoder.h"

#include <csetjmp>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Lang.h"

#if defined(ECAPTURE_HAVE_PNG)
#include <png.h>
#endif
#if defined(ECAPTURE_HAVE_JPEG)
#include <jpeglib.h>
#endif

namespace ecapture {
namespace {

// 96 DPI 换算成 PNG 的 pHYs 与 BMP 的单位像素米数：3780 px/m ≈ 96 dpi。
// Windows 那一路交给 BitmapEncoder 的就是 96.0 × 96.0，这里对齐同一句"每英寸 96 像素"，
// 免得同一帧在两个平台上写出的逻辑尺寸不同。
inline constexpr uint32_t kPpmFor96Dpi = 3780u;

// 编码产物的天花板 = 整帧预算（core/Frame.h 的 kFrameMaxBytes）。一张图编码后的字节数
// 越过"这一帧像素本身占的字节数"就已经说明压缩什么都没做到，再往下只会是无界分配。
inline constexpr uint64_t kMaxEncodedBytes = kFrameMaxBytes;

// 把一次本层内部的失败换成诊断（稳定码 + enc.* 文案 + 恒 ASCII 细节）。
// 细节只作排障用，调用方分支靠的是 code。
bool Fail(Diagnostic* err, const CapturedFrame& frame, ImageFormat fmt, const wchar_t* key,
          const std::wstring& detail = std::wstring()) {
    return EncodeFailure(err, codes::kEncoderUnavailable, Msg(key), FormatName(fmt), detail,
                         std::wstring(), frame.source.c_str());
}

// 把"预算到点"换成一条 capture.timeout 诊断（与 Windows 那一路同一份算式：BudgetSpent）。
bool BudgetFail(const Deadline& dl, Diagnostic* err, const CapturedFrame& frame, ImageFormat fmt) {
    if (!dl.Spent()) return false;
    if (!err) return true;
    Diagnostic d = BudgetSpent(dl, codes::kCaptureTimeout, stages::kEncode, nullptr);
    d.option = L"--format";
    d.value = FormatName(fmt);
    *err = d;
    return true;
}

std::wstring AsciiText(const char* text) {
    std::wstring out;
    for (const char* p = text; p && *p; ++p) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    return out;
}

// 累加一块字节，先核对预算与上限。中止时只登记原因，不抛出、不写 stderr。
struct OutputSink {
    std::vector<uint8_t>* out = nullptr;
    const Deadline* dl = nullptr;
    uint64_t limit = kMaxEncodedBytes;
    bool aborted = false;
    enum class Why : uint32_t { kNone = 0, kBudget, kTooLarge, kNoMemory } why = Why::kNone;

    bool Append(const uint8_t* data, size_t len) {
        if (aborted) return false;
        if (dl && dl->Spent()) {
            aborted = true;
            why = Why::kBudget;
            return false;
        }
        const uint64_t have = out ? static_cast<uint64_t>(out->size()) : 0ull;
        if (have + len > limit) {
            aborted = true;
            why = Why::kTooLarge;
            return false;
        }
        if (out && len) {
            try {
                out->insert(out->end(), data, data + len);
            } catch (...) {
                aborted = true;
                why = Why::kNoMemory;
                return false;
            }
        }
        return true;
    }

    // 到点/超限那两种下场换成各自的诊断；两条不许混成一条"编码失败"。
    bool ReportAbort(Diagnostic* err, const CapturedFrame& frame, ImageFormat fmt) const {
        if (why == Why::kBudget) return BudgetFail(*dl, err, frame, fmt);
        if (why == Why::kTooLarge) {
            return Fail(err, frame, fmt, L"enc.too_large", std::to_wstring(limit));
        }
        if (why == Why::kNoMemory) {
            return Fail(err, frame, fmt, L"enc.exception", L"allocation failed while writing output");
        }
        return Fail(err, frame, fmt, L"enc.failed", L"the codec produced no bytes");
    }
};

// 逐行把紧凑的 B,G,R,A 搬成另一种字节序；调用方给每像素输出字节数（4 = 带 alpha，3 = 丢掉 alpha）。
// 只借一行的临时缓冲，不复制整帧（那会把内存预算乘二）。
void ConvertRows(const std::vector<uint8_t>& tight, uint32_t width, uint32_t height,
                 uint8_t* scratch, uint32_t outBytesPerPixel,
                 const std::function<void(const uint8_t* row, uint32_t y)>& emit) {
    const uint8_t* src = tight.data();
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * width * 4ull;
        if (outBytesPerPixel == 4) {
            std::memcpy(scratch, row, static_cast<size_t>(width) * 4ull);
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t* p = scratch + static_cast<size_t>(x) * 4;
                const uint8_t b = p[0];
                p[0] = p[2];   // R
                p[2] = b;      // B
            }
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                scratch[static_cast<size_t>(x) * 3 + 0] = row[static_cast<size_t>(x) * 4 + 2];
                scratch[static_cast<size_t>(x) * 3 + 1] = row[static_cast<size_t>(x) * 4 + 1];
                scratch[static_cast<size_t>(x) * 3 + 2] = row[static_cast<size_t>(x) * 4 + 0];
            }
        }
        emit(scratch, y);
    }
}

// ---------------------------------------------------------------------------
// BMP：32 位 BI_RGB，bottom-up，无调色板
// ---------------------------------------------------------------------------

void Put32(std::vector<uint8_t>& out, uint64_t v) {
    uint8_t buf[4];
    buf[0] = static_cast<uint8_t>(v & 0xFF);
    buf[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    out.insert(out.end(), buf, buf + 4);
}

bool EncodeBmp(const CapturedFrame& frame, const std::vector<uint8_t>& tight, const Deadline& dl,
               std::vector<uint8_t>* bytes, Diagnostic* err) {
    const uint64_t width = frame.width;
    const uint64_t height = frame.height;
    const uint64_t rowBytes = width * 4ull;
    const uint64_t pixelBytes = rowBytes * height;
    const uint64_t total = 14ull + 40ull + pixelBytes;
    if (pixelBytes > kMaxEncodedBytes || total > kMaxEncodedBytes) {
        // 上面的形状判据已经把整帧限制在 1 GiB 之内，这一条是"加上头就可能越线"那一种：
        // 先把话说清楚，不把长度截断成一个小值写进头里。
        return Fail(err, frame, ImageFormat::kBmp, L"enc.too_large", std::to_wstring(total));
    }
    if (tight.size() < pixelBytes) {
        // 共用层交来的紧凑行比声明的尺寸小：形状那一关的事，不在这里搬越界。
        return Fail(err, frame, ImageFormat::kBmp, L"enc.failed",
                    L"tight buffer shorter than the declared frame");
    }

    std::vector<uint8_t> header;
    header.reserve(54);
    header.push_back('B');
    header.push_back('M');
    Put32(header, total);
    header.insert(header.end(), 4, 0);   // reserved
    Put32(header, 54);                   // 像素数据偏移
    Put32(header, 40);                   // BITMAPINFOHEADER 大小
    Put32(header, width);                // 正高 = bottom-up
    Put32(header, height);
    header.insert(header.end(), {1, 0, 32, 0});   // planes=1, bitcount=32
    Put32(header, 0);                    // BI_RGB：无压缩，也没有可以撒谎的 alpha 处理
    Put32(header, pixelBytes);
    Put32(header, kPpmFor96Dpi);         // x pixels per meter
    Put32(header, kPpmFor96Dpi);         // y pixels per meter
    Put32(header, 0);                    // clrUsed
    Put32(header, 0);                    // clrImportant

    bytes->clear();
    OutputSink sink;
    sink.out = bytes;
    sink.dl = &dl;
    if (!sink.Append(header.data(), header.size())) {
        return sink.ReportAbort(err, frame, ImageFormat::kBmp);
    }
    for (uint64_t row = 0; row < height; ++row) {
        if (BudgetFail(dl, err, frame, ImageFormat::kBmp)) return false;
        const uint8_t* srcRow = tight.data() + (height - 1ull - row) * rowBytes;   // bottom-up
        if (!sink.Append(srcRow, static_cast<size_t>(rowBytes))) {
            return sink.ReportAbort(err, frame, ImageFormat::kBmp);
        }
    }
    return !bytes->empty();
}

#if defined(ECAPTURE_HAVE_PNG)
// ---------------------------------------------------------------------------
// PNG：libpng，逐行 B,G,R,A -> R,G,B,A，自定义 sink 里守预算与上限
// ---------------------------------------------------------------------------
namespace {

struct PngCtx {
    jmp_buf jump;
    bool jumped = false;
    const char* message = "";
};

void PngError(png_structp png_ptr, png_const_charp text) {
    // 库的默认错误处理会 fprintf 到 stderr 再 exit() —— 那一条绝不能留在这里：本工具的一句
    // 抱怨都不许替调用方结束进程，更不许往标准流里写库的碎语。
    PngCtx* ctx = static_cast<PngCtx*>(png_get_error_ptr(png_ptr));
    if (ctx) {
        ctx->jumped = true;
        ctx->message = text ? text : "";
        longjmp(ctx->jump, 1);
    }
}

void PngWarning(png_structp, png_const_charp) {
    // 警告同样不写 stderr：那两路流留给图片二进制与 JSON。
}

void PngSink(png_structp png_ptr, png_bytep data, png_size_t length) {
    OutputSink* sink = static_cast<OutputSink*>(png_get_io_ptr(png_ptr));
    if (sink == nullptr || !sink->Append(data, length)) {
        PngError(png_ptr, sink && sink->why == OutputSink::Why::kBudget
                               ? "encode deadline spent"
                               : "encoded output over limit");
    }
}

void PngFlush(png_structp) {
    // 内存目标没有可 flush 的东西；库结束时会再问一次，空实现即可。
}

bool EncodePng(const CapturedFrame& frame, const std::vector<uint8_t>& tight, const Deadline& dl,
               std::vector<uint8_t>* bytes, Diagnostic* err) {
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const uint64_t rowBytes = static_cast<uint64_t>(width) * 4ull;
    if (tight.size() < rowBytes * height) {
        return Fail(err, frame, ImageFormat::kPng, L"enc.failed",
                    L"tight buffer shorter than the declared frame");
    }

    // 自己的上下文通过 png_create_write_struct 的 error_ptr 参数带进去（libpng 没有公开的
    // png_set_error_ptr；error_ptr 只能在创建时给），回调里再用 png_get_error_ptr 取回来。
    PngCtx ctx;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &ctx, PngError, PngWarning);
    if (!png) return Fail(err, frame, ImageFormat::kPng, L"enc.exception", L"png_create_write_struct");
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        return Fail(err, frame, ImageFormat::kPng, L"enc.exception", L"png_create_info_struct");
    }

    OutputSink sink;
    sink.out = bytes;
    sink.dl = &dl;
    bytes->clear();

    bool wrote = false;
    if (setjmp(ctx.jump) == 0) {
        png_set_write_fn(png, &sink, PngSink, PngFlush);
        // 8 位 RGBA、非交错、无 tRNS：alpha 那一路原样带着，不合成也不猜预乘。
        png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
        png_set_pHYs(png, info, kPpmFor96Dpi, kPpmFor96Dpi, PNG_RESOLUTION_METER);
        png_write_info(png, info);
        std::vector<uint8_t> scratch(static_cast<size_t>(rowBytes));
        uint8_t* scratchPtr = scratch.data();
        bool rowOk = true;
        ConvertRows(tight, width, height, scratchPtr, 4u, [&](const uint8_t* row, uint32_t y) {
            if (!rowOk) return;
            if (dl.Spent()) {
                sink.aborted = true;
                sink.why = OutputSink::Why::kBudget;
                rowOk = false;
                return;
            }
            (void)y;
            png_write_row(png, row);   // sink 里到点会通过 PngError 跳回下面那条 setjmp 分支
        });
        if (rowOk && !sink.aborted) {
            png_write_end(png, nullptr);
            wrote = !sink.aborted;
        }
    }

    png_destroy_write_struct(&png, &info);
    if (wrote) return !bytes->empty();
    bytes->clear();   // 半途到点不交出那半张图
    if (sink.aborted) return sink.ReportAbort(err, frame, ImageFormat::kPng);
    if (ctx.jumped) return Fail(err, frame, ImageFormat::kPng, L"enc.failed", AsciiText(ctx.message));
    return Fail(err, frame, ImageFormat::kPng, L"enc.failed", L"libpng wrote nothing");
}

}  // namespace
#endif  // ECAPTURE_HAVE_PNG

#if defined(ECAPTURE_HAVE_JPEG)
// ---------------------------------------------------------------------------
// JPEG：libjpeg 基线，自定义 destination manager，质量走 --quality
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kJpegBufferBytes = 4096;

struct JpegCtx {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
    bool jumped = false;
    const char* message = "";
};

// jpeg_destination_mgr 必须是本结构的第一成员：库交回来的只有那个公共指针，
// 取回外层结构靠的是 reinterpret_cast（C 里惯用的 container_of 形状，static_cast 在 C++ 不跨类型）。
struct JpegDestMgr {
    struct jpeg_destination_mgr mgr;
    OutputSink* sink = nullptr;
    std::vector<uint8_t> buffer;
};

void JpegErrorExit(j_common_ptr cinfo) {
    // 库的默认 error_exit 会 exit(EXIT_FAILURE)：本工具绝不让一次编码替调用方结束进程。
    // jpeg_error_mgr 是下面那个结构的第一成员（C 里惯用的 container_of 形状），从公共成员
    // 指针取回外层结构只能用 reinterpret_cast —— static_cast 在 C++ 里不跨类型。
    JpegCtx* ctx = reinterpret_cast<JpegCtx*>(cinfo->err);
    ctx->jumped = true;
    ctx->message = "libjpeg reported an error";
    longjmp(ctx->jump, 1);
}

void JpegEmitMessage(j_common_ptr, int) {
    // 库的抱怨不许进标准流（与 PNG 那一条同一规矩）。
}

void JpegInitDest(j_compress_ptr cinfo) {
    JpegDestMgr* dest = reinterpret_cast<JpegDestMgr*>(cinfo->dest);  // 第一成员，见结构体处说明
    dest->buffer.assign(kJpegBufferBytes, 0);
    dest->mgr.next_output_byte = dest->buffer.data();
    dest->mgr.free_in_buffer = dest->buffer.size();
}

boolean JpegEmptyDest(j_compress_ptr cinfo) {
    JpegDestMgr* dest = reinterpret_cast<JpegDestMgr*>(cinfo->dest);  // 第一成员，见结构体处说明
    if (!dest->sink ||
        !dest->sink->Append(dest->buffer.data(), dest->buffer.size())) {
        // 缓冲满了而对面的写不下。libjpeg 没有"部分写出"这条路，也不从回调里返回失败：
        // 只能通过 error_exit 跳回本层，那一段未完成的流由本层清掉，不交给调用方。
        // （ERREXIT 那个宏在 JPEG_INTERNALS 里，应用程序写的 manager 直接问那颗函数指针。）
        dest->mgr.free_in_buffer = 0;
        (*cinfo->err->error_exit)(reinterpret_cast<j_common_ptr>(cinfo));
        return FALSE;
    }
    dest->buffer.assign(kJpegBufferBytes, 0);
    dest->mgr.next_output_byte = dest->buffer.data();
    dest->mgr.free_in_buffer = dest->buffer.size();
    return TRUE;
}

void JpegTermDest(j_compress_ptr cinfo) {
    JpegDestMgr* dest = reinterpret_cast<JpegDestMgr*>(cinfo->dest);  // 第一成员，见结构体处说明
    const size_t used = dest->buffer.size() - dest->mgr.free_in_buffer;
    if (!dest->sink || !dest->sink->Append(dest->buffer.data(), used)) {
        (*cinfo->err->error_exit)(reinterpret_cast<j_common_ptr>(cinfo));
    }
}

bool EncodeJpeg(const CapturedFrame& frame, const std::vector<uint8_t>& tight, int jpegQuality,
                const Deadline& dl, std::vector<uint8_t>* bytes, Diagnostic* err) {
    const uint32_t width = frame.width;
    const uint32_t height = frame.height;
    const uint64_t rowBytes = static_cast<uint64_t>(width) * 4ull;
    if (tight.size() < rowBytes * height) {
        return Fail(err, frame, ImageFormat::kJpeg, L"enc.failed",
                    L"tight buffer shorter than the declared frame");
    }

    struct jpeg_compress_struct cinfo;
    JpegCtx ctx;
    cinfo.err = jpeg_std_error(&ctx.pub);
    ctx.pub.error_exit = JpegErrorExit;
    ctx.pub.emit_message = JpegEmitMessage;
    ctx.pub.num_warnings = 0;
    JpegDestMgr dest;
    OutputSink sink;
    sink.out = bytes;
    sink.dl = &dl;
    dest.sink = &sink;
    bytes->clear();

    bool wrote = false;
    if (setjmp(ctx.jump) == 0) {
        jpeg_create_compress(&cinfo);
        cinfo.dest = &dest.mgr;
        dest.mgr.init_destination = JpegInitDest;
        dest.mgr.empty_output_buffer = JpegEmptyDest;
        dest.mgr.term_destination = JpegTermDest;

        cinfo.image_width = width;
        cinfo.image_height = height;
        cinfo.input_components = 3;          // 只喂 R,G,B：这一格式没有 alpha 通道
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);           // 默认的 jpeg_color_space 是 YCbCr：库自己做常规转换
        jpeg_set_quality(&cinfo, jpegQuality, TRUE);
        cinfo.dct_method = JDCT_ISLOW;       // 指定慢 DCT：跨平台可复现，不按速度挑近似
        jpeg_start_compress(&cinfo, TRUE);   // TRUE = 写 JFIF 头（密度按 1x1，与像素比无涉）
        std::vector<uint8_t> scratch(static_cast<size_t>(width) * 3ull);
        uint8_t* scratchPtr = scratch.data();
        bool rowOk = true;
        ConvertRows(tight, width, height, scratchPtr, 3u, [&](const uint8_t* row, uint32_t y) {
            if (!rowOk) return;
            if (dl.Spent()) {
                sink.aborted = true;
                sink.why = OutputSink::Why::kBudget;
                rowOk = false;
                return;
            }
            JSAMPROW rowptr = const_cast<uint8_t*>(row);
            jpeg_write_scanlines(&cinfo, &rowptr, 1);
            (void)y;
        });
        if (rowOk && !sink.aborted) {
            jpeg_finish_compress(&cinfo);
            wrote = !sink.aborted && !ctx.jumped;
        }
        jpeg_destroy_compress(&cinfo);
    } else {
        jpeg_destroy_compress(&cinfo);
    }

    if (wrote) return !bytes->empty();
    bytes->clear();   // 半途到点不交出那半张图
    if (sink.aborted) return sink.ReportAbort(err, frame, ImageFormat::kJpeg);
    return Fail(err, frame, ImageFormat::kJpeg, L"enc.failed",
                ctx.jumped ? AsciiText(ctx.message) : L"libjpeg wrote nothing");
}

}  // namespace
#endif  // ECAPTURE_HAVE_JPEG

}  // namespace

bool CodecLibraryLinked(ImageFormat format) {
    switch (format) {
        case ImageFormat::kBmp:
            return true;   // 自写的那一条，没有外部库
        case ImageFormat::kPng:
#if defined(ECAPTURE_HAVE_PNG)
            return true;
#else
            return false;
#endif
        case ImageFormat::kJpeg:
#if defined(ECAPTURE_HAVE_JPEG)
            return true;
#else
            return false;
#endif
        case ImageFormat::kTiff:
        case ImageFormat::kGif:
            return false;  // 登记表那两格本来就是 kNoEncoder
    }
    return false;
}

bool EncodeFrameNative(const CapturedFrame& frame, const std::vector<uint8_t>& tight,
                       ImageFormat fmt, int jpegQuality, const Deadline& dl,
                       std::vector<uint8_t>* bytes, Diagnostic* err) {
    if (!bytes) return false;
    switch (fmt) {
        case ImageFormat::kPng:
#if defined(ECAPTURE_HAVE_PNG)
            return EncodePng(frame, tight, dl, bytes, err);
#else
            return Fail(err, frame, fmt, L"enc.format_unsupported", L"this build did not link libpng");
#endif
        case ImageFormat::kJpeg:
#if defined(ECAPTURE_HAVE_JPEG)
            return EncodeJpeg(frame, tight, jpegQuality, dl, bytes, err);
#else
            return Fail(err, frame, fmt, L"enc.format_unsupported",
                        L"this build did not link libjpeg");
#endif
        case ImageFormat::kBmp:
            return EncodeBmp(frame, tight, dl, bytes, err);
        case ImageFormat::kTiff:
        case ImageFormat::kGif:
            // 共用层的格式判据 normally 已经把这两条拦下；留这一格是因为它的下场是"这条路径
            // 没有这一格式的编码器"而不是"没链上库"：真走到这里说明调用方绕过了
            // CheckEncodeInput，那就照实失败，不静默换成别的格式。
            return Fail(err, frame, fmt, L"enc.format_unsupported",
                        L"no encoder in this codec path");
    }
    return Fail(err, frame, fmt, L"enc.failed", L"unrecognized format");
}

}  // namespace ecapture
