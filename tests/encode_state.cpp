// 跨平台图像处理与编码的离线判据。
//
// Windows 由 tests\encode.ps1 运行 build\ecapture-encode-tests.exe，Linux 由 ./test-all.sh 运行
// build-linux/<编译器>-<配置>/ecapture-encode-tests。两边链的是**自己那一份**编码实现
//（Windows = WinRT BitmapEncoder 本体，Linux = libpng + libjpeg + 自写 BMP），
// 判据本体则是共用的 src/Encoder.cpp 与 src/ImageOps.cpp、src/CropGeometry.cpp、src/HdrColor.cpp。
// 这里不链任何取帧通道、不碰窗口、不弹框、不连显示服务器 —— 全部像素都是本文件手工摆出来的
// 合成图形（棋盘、边界色块、透明像素、带行末填充的行距），因此每一条现场都是确定的数。
//
// 为什么"编码解码"这一条不写成自证：PNG 与 BMP 的**容器结构**由本文件自己按格式规范解析
// （块边界、CRC32 用本文件自己的一份表算、IHDR/DIB 头逐字段核对），不借编码器那家库；
// 像素回读在 Linux 上用 libpng / libjpeg（它们验的是「喂进去的 BGRA 与读回来的 RGBA/RGB 对不对得上」
// 这一层通道顺序，不是「库自己承认自己写对了」），Windows 上则只做结构核对并把这一点如实打印出来，
// 不拿结构核对冒充像素核对。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#include "../src/Capture.h"
#include "../src/CropGeometry.h"
#include "../src/Deadline.h"
#include "../src/Delivery.h"
#include "../src/Encoder.h"
#include "../src/HdrColor.h"
#include "../src/HistoryArchive.h"
#include "../src/ImageOps.h"
#include "../src/Lang.h"
#include "../src/core/CodecCapability.h"
#include "../src/core/Frame.h"

#if !defined(_WIN32)
#include <unistd.h>

#include <png.h>
#include <jpeglib.h>
#include <csetjmp>
#endif

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}

// 只在本平台真的判了才计数；平台外的那一段如实打印，不记成通过，也不记成失败。
void Note(const char* what) { std::printf("  NOTE  %s\n", what); }

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

// ---------------------------------------------------------------------------
// 合成帧
// ---------------------------------------------------------------------------

// 棋盘 + 每个像素的 B/G/R 都随坐标变化 + 一条 alpha 带 + 可选的行末填充。
// 为什么用它当主要输入：通道顺序换错位、行距算错、行末填充漏进图片、alpha 被合成掉、
// 缩放把某一列跳过 —— 这五种错都会在这张帧上留下不同的痕迹，而纯色帧一种都看不出来。
CapturedFrame MakeTestFrame(uint32_t width, uint32_t height, uint32_t padBytes = 0) {
    CapturedFrame f;
    f.width = width;
    f.height = height;
    f.stride = width * 4u + padBytes;
    f.pixels.assign(static_cast<size_t>(f.stride) * height, 0);
    // 行末填充写成非零，用来检查它有没有漏进图片
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t p = 0; p < padBytes; ++p) {
            f.pixels[static_cast<size_t>(y) * f.stride + static_cast<size_t>(width) * 4u + p] = 0xEE;
        }
    }
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = f.pixels.data() + static_cast<size_t>(y) * f.stride;
        for (uint32_t x = 0; x < width; ++x) {
            const bool black = ((x + y) & 1u) == 0u;
            uint8_t* px = row + static_cast<size_t>(x) * 4u;
            px[0] = black ? static_cast<uint8_t>(x & 0xFF) : static_cast<uint8_t>(200 + (y & 0x37));
            px[1] = black ? static_cast<uint8_t>(y & 0xFF) : static_cast<uint8_t>(17 + (x & 0x5F));
            px[2] = black ? 0 : static_cast<uint8_t>((x * 3u + y) & 0xFF);
            // 顶部 1/4 的那几行给一条 alpha 带：0、半透明、不透明三种都要在场
            px[3] = y < (height + 3u) / 4u
                        ? static_cast<uint8_t>((x % 3u == 0u) ? 0 : ((x % 3u == 1u) ? 128 : 255))
                        : static_cast<uint8_t>(255);
        }
    }
    f.source = L"synthetic";
    f.path = L"synthetic.checker";
    return f;
}

CapturedFrame FrameOf(uint32_t width, uint32_t height, uint32_t padBytes) {
    CapturedFrame f;
    f.width = width;
    f.height = height;
    f.stride = width * 4u + padBytes;
    f.pixels.assign(static_cast<size_t>(f.stride) * height, 0);
    f.source = L"synthetic";
    f.path = L"synthetic.block";
    return f;
}

void PutPixel(CapturedFrame* f, uint32_t x, uint32_t y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    uint8_t* px = f->pixels.data() + static_cast<size_t>(y) * f->stride + static_cast<size_t>(x) * 4u;
    px[0] = b; px[1] = g; px[2] = r; px[3] = a;
}

void GetPixelBgra(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y,
                  uint8_t out[4]) {
    const uint8_t* p = rgba.data() + static_cast<size_t>(y) * width * 4u + static_cast<size_t>(x) * 4u;
    out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];   // RGBA -> BGRA 口径
}

// 期望值：与 MakeTestFrame 同一条算式（在测试里独立写一遍，就是为了拦下编码路上的通道错位）
void ExpectedBgra(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t out[4]) {
    const bool black = ((x + y) & 1u) == 0u;
    out[0] = black ? static_cast<uint8_t>(x & 0xFF) : static_cast<uint8_t>(200 + (y & 0x37));
    out[1] = black ? static_cast<uint8_t>(y & 0xFF) : static_cast<uint8_t>(17 + (x & 0x5F));
    out[2] = black ? 0 : static_cast<uint8_t>((x * 3u + y) & 0xFF);
    out[3] = y < (height + 3u) / 4u
                 ? static_cast<uint8_t>((x % 3u == 0u) ? 0 : ((x % 3u == 1u) ? 128 : 255))
                 : static_cast<uint8_t>(255);
}

const Deadline& NoBudget() {
    static const Deadline dl;
    return dl;
}

bool Encode(const CapturedFrame& frame, ImageFormat fmt, std::vector<uint8_t>* bytes,
            Diagnostic* err, int quality = 95) {
    return EncodeFrame(frame, fmt, quality, NoBudget(), bytes, err);
}

// 判据自己打印用的窄字符（不进产物，不参与判据）
std::string ToAscii(const wchar_t* text) {
    std::string out;
    for (const wchar_t* p = text; p && *p; ++p) out += static_cast<char>(*p);
    return out;
}

const wchar_t* const kAllFormats[] = {L"png", L"jpeg", L"bmp", L"tiff", L"gif"};

ImageFormat FormatOf(const wchar_t* name) {
    if (!std::wcscmp(name, L"png")) return ImageFormat::kPng;
    if (!std::wcscmp(name, L"jpeg")) return ImageFormat::kJpeg;
    if (!std::wcscmp(name, L"bmp")) return ImageFormat::kBmp;
    if (!std::wcscmp(name, L"tiff")) return ImageFormat::kTiff;
    return ImageFormat::kGif;
}

// ---------------------------------------------------------------------------
// 容器结构核对（不借任何编码库）
// ---------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t Le16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

struct PngInfo {
    bool ok = false;
    uint32_t width = 0, height = 0;
    uint8_t bitDepth = 0, colorType = 0, interlace = 0;
    uint32_t physX = 0, physY = 0, physUnit = 0;
    bool hasPhys = false, hasIdat = false, crcsOk = false, orderOk = false;
    size_t chunkCount = 0;
};

PngInfo ParsePng(const std::vector<uint8_t>& b) {
    PngInfo r;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (b.size() < 8 + 12 + 12 || std::memcmp(b.data(), sig, 8) != 0) return r;
    size_t p = 8;
    bool firstIsIhdr = false, lastIsIend = false;
    bool crcs = true;
    while (p + 12 <= b.size()) {
        const uint32_t len = Be32(b.data() + p);
        if (len > 0x7FFFFFFFu || p + 12 + len > b.size()) return r;
        const uint8_t* type = b.data() + p + 4;
        const uint8_t* data = b.data() + p + 8;
        const uint32_t want = Be32(b.data() + p + 8 + len);
        std::vector<uint8_t> crcInput;
        crcInput.insert(crcInput.end(), type, type + 4);
        crcInput.insert(crcInput.end(), data, data + len);
        if (Crc32(crcInput.data(), crcInput.size()) != want) crcs = false;
        ++r.chunkCount;
        lastIsIend = false;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (r.chunkCount == 1) firstIsIhdr = true;
            if (len != 13) return r;
            r.width = Be32(data);
            r.height = Be32(data + 4);
            r.bitDepth = data[8];
            r.colorType = data[9];
            r.interlace = data[12];
            if (data[10] != 0 || data[11] != 0) return r;   // 压缩与滤波方法恒 0
        } else if (!std::memcmp(type, "pHYs", 4)) {
            if (len != 9) return r;
            r.hasPhys = true;
            r.physX = Be32(data);
            r.physY = Be32(data + 4);
            r.physUnit = data[8];
        } else if (!std::memcmp(type, "IDAT", 4)) {
            r.hasIdat = true;
        } else if (!std::memcmp(type, "tRNS", 4)) {
            return r;   // 这一路不写 tRNS（带 alpha 就写 color type 6）
        } else if (!std::memcmp(type, "IEND", 4)) {
            lastIsIend = len == 0;
            break;
        }
        p += 12 + len;
    }
    r.crcsOk = crcs;
    r.orderOk = firstIsIhdr && lastIsIend;
    r.ok = firstIsIhdr && lastIsIend && r.hasIdat;
    return r;
}

struct BmpInfo {
    bool ok = false;
    uint32_t width = 0, height = 0;   // height 取绝对值；bottomUp 记录符号
    bool bottomUp = false;
    uint16_t planes = 0, bpp = 0;
    uint32_t compression = 0, offset = 0, fileSize = 0, image_size = 0;
};

BmpInfo ParseBmp(const std::vector<uint8_t>& b) {
    BmpInfo r;
    if (b.size() < 54) return r;
    if (b[0] != 'B' || b[1] != 'M') return r;
    r.fileSize = Le32(b.data() + 2);
    r.offset = Le32(b.data() + 10);
    const uint32_t dibSize = Le32(b.data() + 14);
    if (dibSize < 40) return r;
    r.width = Le32(b.data() + 18);
    const int32_t h = static_cast<int32_t>(Le32(b.data() + 22));
    r.bottomUp = h > 0;
    r.height = h < 0 ? static_cast<uint32_t>(-h) : static_cast<uint32_t>(h);
    r.planes = Le16(b.data() + 26);
    r.bpp = Le16(b.data() + 28);
    r.compression = Le32(b.data() + 30);
    r.image_size = Le32(b.data() + 34);
    r.ok = r.planes == 1 && r.bpp == 32 && r.compression == 0 && r.offset == 54 &&
           r.fileSize == b.size();
    return r;
}

struct JpegInfo {
    bool ok = false;
    bool hasJfif = false;
    uint32_t width = 0, height = 0;
    int qualityGuess = 0;      // 从量化表 DC 系数反推的近似值，只用于核对质量参数真起作用了
    int components = 0;
    bool baseline = false;
};

JpegInfo ParseJpeg(const std::vector<uint8_t>& b) {
    JpegInfo r;
    if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return r;
    if (b[b.size() - 2] != 0xFF || b[b.size() - 1] != 0xD9) return r;
    size_t p = 2;
    while (p + 4 <= b.size()) {
        if (b[p] != 0xFF) return r;
        uint8_t marker = b[p + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            p += 2;
            continue;
        }
        const uint32_t segLen = (static_cast<uint32_t>(b[p + 2]) << 8) | b[p + 3];
        if (segLen < 2 || p + 2 + segLen > b.size()) return r;
        const uint8_t* seg = b.data() + p + 2;
        if (marker == 0xE0 && segLen >= 16 && !std::memcmp(seg + 2, "JFIF", 4)) r.hasJfif = true;
        if (marker == 0xC0 || marker == 0xC2) {
            r.baseline = marker == 0xC0;
            r.height = (static_cast<uint32_t>(seg[3]) << 8) | seg[4];
            r.width = (static_cast<uint32_t>(seg[5]) << 8) | seg[6];
            r.components = seg[7];
        }
        if (marker == 0xDB && segLen >= 67) {
            // 量化表第一项（亮度 DC）随质量线性变化：拿它反推一次，核对 --quality 真传到了库里。
            const int q = seg[5];
            r.qualityGuess = q <= 0 ? 0 : (q < 50 ? (5000 / q + 1) / 2 : (200 - 2 * q + 1) / 2);
        }
        if (marker == 0xC4 || marker == 0xDB || marker == 0xDD || (marker >= 0xC0 && marker <= 0xCF)) {
            // 这些段之后仍可能继续有段，正常走
        }
        if (marker == 0xDA) {   // 熵数据开始：之后再没有可读的段头
            r.ok = r.width && r.height && r.hasJfif;
            return r;
        }
        p += 2 + segLen;
    }
    r.ok = false;
    return r;
}

#if !defined(_WIN32)
// 用 libpng 把 PNG 解回 RGBA（紧凑行）。这一步只用于**回读**：库在这里的角色是标准解码器，
// 判的是"我们喂进去的 BGRA 与解出来的 RGBA 对不对得上"，容器本身已经由上面那份独立解析核过。
bool DecodePngRgba(const std::vector<uint8_t>& b, std::vector<uint8_t>* rgba, uint32_t* w,
                   uint32_t* h) {
    png_image img;
    std::memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, b.data(), b.size())) {
        png_image_free(&img);
        return false;
    }
    *w = img.width;
    *h = img.height;
    img.format = PNG_FORMAT_RGBA;
    rgba->assign(PNG_IMAGE_SIZE(img), 0);
    const bool ok = png_image_finish_read(&img, nullptr, rgba->data(), 0, nullptr) != 0;
    png_image_free(&img);
    return ok;
}

struct JpegDecErr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

void JpegDecExit(j_common_ptr cinfo) {
    // jpeg_error_mgr 是那个结构的第一成员（C 语言里惯用的 container_of 形状），
    // 从公共成员指针取回外层结构只能用 reinterpret_cast，static_cast 在 C++ 里不跨类型。
    JpegDecErr* e = reinterpret_cast<JpegDecErr*>(cinfo->err);
    longjmp(e->jump, 1);
}

bool DecodeJpegRgb(const std::vector<uint8_t>& b, std::vector<uint8_t>* rgb, uint32_t* w,
                   uint32_t* h) {
    struct jpeg_decompress_struct cinfo;
    JpegDecErr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegDecExit;
    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, b.data(), static_cast<unsigned long>(b.size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    *w = cinfo.output_width;
    *h = cinfo.output_height;
    const size_t rowBytes = static_cast<size_t>(cinfo.output_width) * 3;
    rgb->assign(rowBytes * cinfo.output_height, 0);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = rgb->data() + static_cast<size_t>(cinfo.output_scanline) * rowBytes;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// 1) 登记表完整性
// ---------------------------------------------------------------------------
void TestCodecTable() {
    Section("编码格式登记表：每一格都登记过");

    const CodecPath paths[] = {CodecPath::kWinRtBitmapEncoder, CodecPath::kPosixLibCodecs};
    const ImageFormat formats[] = {ImageFormat::kPng, ImageFormat::kJpeg, ImageFormat::kBmp,
                                   ImageFormat::kTiff, ImageFormat::kGif};
    uint32_t n = 0;
    CodecFormatCells(&n);
    Check(n == 10, "登记表正好是 2 条路径 × 5 种格式");
    for (const CodecPath path : paths) {
        for (const ImageFormat fmt : formats) {
            const CodecSupport s = CodecSupportFor(path, fmt);
            Check(s != CodecSupport::kUnregistered,
                  (std::string("每格都有登记：") + CodecPathName(path) + "/" +
                   ToAscii(FormatName(fmt))).c_str());
        }
    }
    // 两条路径的差别必须是**说得出名字**的差别
    Check(CodecSupportFor(CodecPath::kWinRtBitmapEncoder, ImageFormat::kTiff) ==
              CodecSupport::kSupported,
          "winrt 那一路支持 tiff");
    Check(CodecSupportFor(CodecPath::kPosixLibCodecs, ImageFormat::kTiff) ==
              CodecSupport::kNoEncoder,
          "posix 那一路没有 tiff 编码器（不是「暂时没探测」）");
    Check(CodecSupportFor(CodecPath::kPosixLibCodecs, ImageFormat::kGif) ==
              CodecSupport::kNoEncoder,
          "posix 那一路没有 gif 编码器（256 色调色板不是「能写出文件就算支持」）");
    Check(CodecSupportFor(CodecPath::kPosixLibCodecs, ImageFormat::kPng) == CodecSupport::kSupported &&
              CodecSupportFor(CodecPath::kPosixLibCodecs, ImageFormat::kJpeg) ==
                  CodecSupport::kSupported &&
              CodecSupportFor(CodecPath::kPosixLibCodecs, ImageFormat::kBmp) ==
                  CodecSupport::kSupported,
          "posix 那一路 png / jpeg / bmp 三种都登记为支持");
    Check(std::string(CodecSupportReason(CodecPath::kPosixLibCodecs, ImageFormat::kGif))
              .find("indexed") != std::string::npos,
          "不支持的那一格带着一个可核对的 ASCII 理由");
    Check(CodecSupportFor(CodecPath::kWinRtBitmapEncoder, ImageFormat::kPng) ==
              CodecSupport::kSupported,
          "winrt 那一路 png 支持");
    Check(CodecSupportFor(CodecPath::kUnregistered, ImageFormat::kPng) ==
              CodecSupport::kUnregistered,
          "没有那条路径就没有那一格，不拿默认值兜底");
}

// ---------------------------------------------------------------------------
// 2) 帧规范：形状、行距、方向与所有权
// ---------------------------------------------------------------------------
void TestFrameSpec() {
    Section("帧规范：形状、行末填充、方向字段与所有权");

    CapturedFrame f = MakeTestFrame(40, 24, 13);
    Check(InspectFrameShape(f) == FrameShape::kOk, "带 13 字节行末填充的形状仍然说得通");
    std::vector<uint8_t> bytes;
    Diagnostic err;
    const bool supported = EncodeFormatSupported(ImageFormat::kPng, nullptr);
    bool ok = Encode(f, ImageFormat::kPng, &bytes, &err);
    Check(supported ? (ok && !bytes.empty()) : (!ok && bytes.empty()),
          "同一份带填充的帧按本平台的路径要么编出来、要么如实失败");

    // 行末填充不许进图片：填充写的是 0xEE，任何一种产物都不该有它
    if (ok) {
        bool leaks = false;
        for (size_t i = 0; i + 4 <= bytes.size(); ++i) {
            if (bytes[i] == 0xEE && bytes[i + 1] == 0xEE && bytes[i + 2] == 0xEE &&
                bytes[i + 3] == 0xEE) {
                leaks = true;
                break;
            }
        }
        Check(!leaks, "PNG 产物里找不到那 4 个连着的填充字节");
    }

    // 方向：旋转在取帧那一步就做完了，编码拿到的字段只是事实陈述，不该改变字节
    CapturedFrame same = MakeTestFrame(40, 24, 13);
    same.reportsCrop = true;
    same.requestedRect = PhysRect{-10, -20, 30, 4};
    same.capturedRect = PhysRect{0, 0, 40, 24};
    same.clipped = true;
    same.rotation = 90;
    std::vector<uint8_t> a, b;
    Diagnostic e2;
    if (ok && Encode(same, ImageFormat::kPng, &a, &e2) &&
        Encode(same, ImageFormat::kPng, &b, &e2)) {
        Check(a == b, "同一帧编两次字节相同（确定性，不看那些事实字段）");
    } else {
        Note("本平台没有 png 编码器，重复编码这一条判不了");
    }

    // 所有权：编码不借帧的缓冲之外的东西，帧在编码后仍可正常改动、产物不受影响
    std::vector<uint8_t> held;
    if (Encode(f, ImageFormat::kBmp, &held, &err)) {
        const size_t before = held.size();
        PutPixel(&f, 0, 0, 0, 0, 0, 255);
        Check(held.size() == before, "改动源帧不动已经编出的字节（产物自己拥有那份缓冲）");
    } else {
        Note("bmp 编码没成功，所有权这一条改由结构核对");
    }
}

// ---------------------------------------------------------------------------
// 3) 先裁剪后缩放、不放大、越界不自动裁
// ---------------------------------------------------------------------------
void TestCropScaleRules() {
    Section("裁剪与缩放规则：先裁再缩、不放大、越界拒绝");

    CapturedFrame f = MakeTestFrame(64, 48, 8);
    const uint32_t w0 = f.width, h0 = f.height;

    // 越界一条边就整条拒绝，且不改动帧（绝不"那就裁到边上为止"）
    const uint32_t before = static_cast<uint32_t>(f.pixels.size());
    Check(!CropFrame(&f, w0 - 1u, 0u, 2u, 1u), "右边越界一个像素就拒绝");
    Check(f.width == w0 && f.height == h0 &&
          f.pixels.size() == before,
          "被拒的裁剪一个字节都没改动帧");
    Check(!CropFrame(&f, 0u, 0u, 0u, 10u), "空矩形拒绝");
    Check(!CropFrame(&f, 0u, 0u, 1u, kFrameMaxSide + 1u), "超过单边上限拒绝");

    // 窗口内部裁剪走共用的判据本体：越界的请求换不来一次自动裁短
    CropRequest req;
    req.mode = CropMode::kRoi;
    req.roiGiven = true;
    req.x = 40u;
    req.y = 30u;
    req.width = 30u;
    req.height = 30u;
    DeliveredImage img = DescribeDeliveredImage(w0, h0, PhysRect{0, 0, static_cast<int32_t>(w0), static_cast<int32_t>(h0)}, true,
                                                PhysRect{0, 0, static_cast<int32_t>(w0),
                                                        static_cast<int32_t>(h0)}, true);
    ClientAreaProbe client;
    const CropResolution res = ResolveWindowCrop(req, img, client);
    Check(res.status == CropStatus::kOutOfRange && res.fail == CropFail::kRoiBeyondImage,
          "--roi 落在交付图像之外 = 拒绝，不裁到边");
    Check(res.requestRight == 70ull && res.requestBottom == 60ull,
          "越界那条边报的是请求本身算出来的数（64×48 的图请求到 70,60）");

    // 合法的 --roi 裁得下来，且裁完再缩：交付尺寸来自"裁之后"那张图
    req.width = 20u;
    req.height = 16u;
    const CropResolution ok = ResolveWindowCrop(req, img, client);
    Check(ok.status == CropStatus::kCropped && ok.crop.x == 40u && ok.crop.width == 20u,
          "落在图内的 --roi 按请求裁");
    CapturedFrame g = MakeTestFrame(64, 48, 8);
    Check(CropFrame(&g, ok.crop.x, ok.crop.y, ok.crop.width, ok.crop.height), "共用裁剪本体跟上");
    Check(g.width == 20u && g.height == 16u && g.stride == 80u,
          "裁剪后行距等于新行长（填充不参与）");
    ScaleRequest sc;
    sc.given = true;
    sc.hasMaxWidth = true;
    sc.maxWidth = 10u;
    const ScaleResolution sr = ResolveScale(sc, g.width, g.height);
    Check(sr.status == ScaleStatus::kScaled && sr.width == 10u && sr.height == 8u,
          "缩放天花板按**裁之后**的尺寸算（20×16 → 10×8，等比）");
    Check(ScaleFrame(&g, sr.width, sr.height), "缩放本体执行");
    std::vector<uint8_t> bytes;
    Diagnostic err;
    if (EncodeFormatSupported(ImageFormat::kPng, nullptr)) {
        Check(Encode(g, ImageFormat::kPng, &bytes, &err), "裁剪+缩放之后的帧照样编得出去");
        const PngInfo p = ParsePng(bytes);
        Check(p.width == 10u && p.height == 8u,
              "容器里写的就是先裁后缩的那对尺寸（不是 64×48、也不是 20×16）");
    } else {
        Note("本平台没有 png 编码器，容器尺寸这一条不判");
    }

    // 不放大
    const ScaleResolution up = ResolveScale(sc, 4u, 4u);
    Check(up.status == ScaleStatus::kUnchanged && up.width == 4u && up.height == 4u,
          "天花板比图还大时原样交付（默认不放大）");
    CapturedFrame t = MakeTestFrame(8, 8, 0);
    Check(!ScaleFrame(&t, 16u, 16u), "ScaleFrame 拒绝把图放大");
    Check(t.width == 8u && t.height == 8u, "被拒的放大没改动帧");
}

// ---------------------------------------------------------------------------
// 4) 通道顺序、alpha 与不猜预乘
// ---------------------------------------------------------------------------
void TestColorAndAlpha() {
    Section("通道顺序、透明度与预乘：不猜、不合成、不换错");

    // 每个通道都取"只有它自己那个值"的边界色块：换位的错在这里一定露出来
    CapturedFrame f = MakeTestFrame(32, 16, 4);
    PutPixel(&f, 0, 0, 255, 0, 0, 255);      // 纯蓝
    PutPixel(&f, 1, 0, 0, 255, 0, 255);      // 纯绿
    PutPixel(&f, 2, 0, 0, 0, 255, 255);      // 纯红
    PutPixel(&f, 3, 0, 0, 0, 0, 255);        // 黑
    PutPixel(&f, 4, 0, 255, 255, 255, 255);  // 白
    PutPixel(&f, 5, 0, 0, 0, 0, 0);          // alpha=0，颜色也是黑
    PutPixel(&f, 6, 0, 200, 100, 50, 0);     // alpha=0，颜色非零：透明不等于"颜色没内容"
    PutPixel(&f, 7, 0, 3, 251, 128, 1);      // alpha=1：最小非零 alpha

    std::vector<uint8_t> pngBytes, bmpBytes;
    Diagnostic err;
    const bool pngOk = Encode(f, ImageFormat::kPng, &pngBytes, &err);
    const bool bmpOk = Encode(f, ImageFormat::kBmp, &bmpBytes, &err);
    Check(pngOk || !EncodeFormatSupported(ImageFormat::kPng, nullptr), "png 编码按本平台的路径答复");
    Check(bmpOk, "bmp 编码成功（两条路径都自写/内建，不依赖外部库）");

#if !defined(_WIN32)
    if (pngOk) {
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        Check(DecodePngRgba(pngBytes, &rgba, &w, &h) && w == 32u && h == 16u,
              "libpng 把产物读回来，尺寸对得上");
        static const uint8_t want[8][4] = {
            {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
            {0, 0, 0, 255},   {255, 255, 255, 255}, {0, 0, 0, 0},
            {200, 100, 50, 0}, {3, 251, 128, 1}};
        bool channelsOk = true, alphaOk = true;
        for (uint32_t i = 0; i < 8; ++i) {
            uint8_t got[4];
            GetPixelBgra(rgba, w, i, 0, got);
            if (got[0] != want[i][0] || got[1] != want[i][1] || got[2] != want[i][2]) {
                channelsOk = false;
            }
            if (got[3] != want[i][3]) alphaOk = false;
        }
        Check(channelsOk, "边界色块逐通道读回与喂进去的 B/G/R 一致（没有换错位）");
        Check(alphaOk, "alpha 原样读回：0、1、128、255 都没被合成、也没被猜成预乘");
        // 非零颜色的透明像素不许被"去预乘"改暗或改亮
        uint8_t transparent[4];
        GetPixelBgra(rgba, w, 6, 0, transparent);
        Check(transparent[0] == 200 && transparent[1] == 100 && transparent[2] == 50,
              "alpha=0 的像素颜色分量保持原值（不做 unpremultiply 的猜测）");
        // 棋盘逐像素核对（整幅）
        bool allMatch = true;
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                if (x < 8u && y == 0u) continue;   // 上面那几个边界色块覆盖过的位置
                uint8_t got[4], want[4];
                GetPixelBgra(rgba, w, x, y, got);
                ExpectedBgra(x, y, w, h, want);
                if (std::memcmp(got, want, 4) != 0) {
                    allMatch = false;
                    break;
                }
            }
        }
        Check(allMatch, "整幅棋盘逐像素与喂进去的合成算式一致");
    }
    if (bmpOk) {
        std::vector<uint8_t> rgba;
        // BMP 的行序是 bottom-up：判据自己按头里的偏移与行长读，不借库
        const BmpInfo b = ParseBmp(bmpBytes);
        Check(b.ok && b.bpp == 32 && b.bottomUp, "bmp 头：32 位 BI_RGB、bottom-up、偏移 54");
        if (b.ok) {
            const uint8_t* pixels = bmpBytes.data() + b.offset;
            auto readBmp = [&](uint32_t x, uint32_t y, uint8_t out[4]) {
                const uint8_t* p = pixels + static_cast<size_t>(b.height - 1 - y) * b.width * 4ull +
                                   static_cast<size_t>(x) * 4;
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
            };
            uint8_t got[4];
            readBmp(0, 0, got);
            Check(got[0] == 255 && got[1] == 0 && got[2] == 0 && got[3] == 255,
                  "bmp 的第一行第一像素（bottom-up 之后）是纯蓝");
            readBmp(2, 0, got);
            Check(got[0] == 0 && got[1] == 0 && got[2] == 255, "bmp 里蓝红没有互换");
            readBmp(6, 0, got);
            Check(got[3] == 0 && got[0] == 200,
                  "bmp 里 alpha 那一路照原字节写出（BI_RGB 不表示透明，也不合成掉颜色）");
            bool match = true;
            for (uint32_t y = 0; y < b.height && match; ++y) {
                for (uint32_t x = 8u; x < b.width; ++x) {
                    uint8_t g2[4], want[4];
                    readBmp(x, y, g2);
                    ExpectedBgra(x, y, b.width, b.height, want);
                    if (std::memcmp(g2, want, 4) != 0) {
                        match = false;
                        break;
                    }
                }
            }
            Check(match, "bmp 的行序与行距都对：整幅棋盘逐像素对得上");
        }
    }
#else
    Note("Windows 侧本轮不链 libpng/libjpeg 的解码器：像素回读只在 Linux 实测，"
         "这里只做上面的容器结构核对");
#endif

    // JPEG 不表示 alpha：只写三个颜色分量，且 alpha 不参与任何合成。
    // 这里另造一张**大色块**帧（每个色块 16×16）：JPEG 的色度可以按 2×2 平均，
    // 拿一张相邻像素剧烈变化的棋盘去要求"逐像素读回"是把有损格式当无损判 ——
    // 色块够大才问得出"这一路写的是它自己的颜色"，而不是"平均出来的中间值"。
    CapturedFrame blocks = FrameOf(32, 32, 0);
    struct Blk { int b, g, r, a; };
    const Blk blks[4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {200, 100, 50, 0}};
    for (int i = 0; i < 4; ++i) {
        const uint32_t bx = static_cast<uint32_t>(i % 2) * 16u;
        const uint32_t by = static_cast<uint32_t>(i / 2) * 16u;
        for (uint32_t y = by; y < by + 16u; ++y) {
            for (uint32_t x = bx; x < bx + 16u; ++x) {
                PutPixel(&blocks, x, y, static_cast<uint8_t>(blks[i].b),
                         static_cast<uint8_t>(blks[i].g), static_cast<uint8_t>(blks[i].r),
                         static_cast<uint8_t>(blks[i].a));
            }
        }
    }
    std::vector<uint8_t> jpgBytes;
    if (EncodeFormatSupported(ImageFormat::kJpeg, nullptr) &&
        Encode(blocks, ImageFormat::kJpeg, &jpgBytes, &err, 95)) {
        const JpegInfo j = ParseJpeg(jpgBytes);
        Check(j.ok && j.baseline && j.width == 32u && j.height == 32u && j.components == 3,
              "jpeg 容器：SOI/EOI 齐、JFIF 段在、SOF 里就是这次交付的尺寸");
#if !defined(_WIN32)
        std::vector<uint8_t> rgb;
        uint32_t w = 0, h = 0;
        if (DecodeJpegRgb(jpgBytes, &rgb, &w, &h)) {
            auto center = [&](int i, int out[3]) {
                const uint32_t x = static_cast<uint32_t>(i % 2) * 16u + 8u;
                const uint32_t y = static_cast<uint32_t>(i / 2) * 16u + 8u;
                const uint8_t* p = rgb.data() + static_cast<size_t>(y) * w * 3 +
                                   static_cast<size_t>(x) * 3;
                out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
            };
            int got[3];
            center(0, got);
            Check(got[0] < 24 && got[1] < 24 && got[2] > 231,
                  "jpeg 色块中心读回纯蓝（有损，容差 ±24；alpha=255 不影响颜色）");
            center(1, got);
            Check(got[0] < 24 && got[1] > 231 && got[2] < 24, "jpeg 色块中心读回纯绿（通道没换位）");
            center(2, got);
            Check(got[0] > 231 && got[1] < 24 && got[2] < 24, "jpeg 色块中心读回纯红（通道没换位）");
            center(3, got);
            // 那个色块 alpha=0 而颜色是 (B=200,G=100,R=50)：不许被合成成黑，也不许 unpremultiply
            Check(got[0] < 70 && got[1] > 76 && got[1] < 124 && got[2] > 166,
                  "alpha=0 的色块在 jpeg 里写的仍是它自己的颜色（没有按 alpha 合成）");
        } else {
            Check(false, "libjpeg 把 jpeg 产物读回来");
        }
#endif
        // 质量参数真的到了编码器：同一张图质量 30 与质量 95 的字节数必须不同
        std::vector<uint8_t> low;
        Diagnostic e2;
        if (Encode(blocks, ImageFormat::kJpeg, &low, &e2, 30)) {
            Check(low.size() < jpgBytes.size(),
                  "--quality 30 的产物比 95 小（质量参数传到了编码器）");
        }
    }
}

// ---------------------------------------------------------------------------
// 5) HDR 与 SDR 分开：说不出正确映射的帧不许当普通 SDR 写出去
// ---------------------------------------------------------------------------
void TestHdrBoundary() {
    Section("HDR 边界：确凿是 HDR 而没过映射 = 拒绝");

    struct Case {
        FrameColorSpace cs;
        bool toneMapped;
        bool mustRefuse;
        const char* what;
    } cases[] = {
        {FrameColorSpace::kSrgbBgra8, false, false, "普通 8 位 sRGB 帧照常编"},
        {FrameColorSpace::kScRgbFloat16, false, true, "FP16 scRGB 没映射：拒"},
        {FrameColorSpace::kScRgbFloat16, true, false, "FP16 scRGB 已映射：编"},
        {FrameColorSpace::kPqBt2020, false, true, "PQ BT.2020 没映射：拒"},
        {FrameColorSpace::kHlgBt2020, false, true, "HLG BT.2020 没映射：拒"},
        {FrameColorSpace::kPqBt2020, true, false, "PQ 已映射：编"},
        {FrameColorSpace::kRgb10A2Unverified, false, true,
         "10 位打包而色彩空间说不出：拒（像素布局不是 8 位 BGRA）"},
        {FrameColorSpace::kUnknown, false, true, "连布局都认不出：拒"},
    };
    for (const Case& c : cases) {
        CapturedFrame f = MakeTestFrame(16, 16, 0);
        f.sourceColorSpace = c.cs;
        f.toneMapped = c.toneMapped;
        std::vector<uint8_t> bytes;
        Diagnostic err;
        const bool ok = EncodeFrame(f, ImageFormat::kPng, 95, NoBudget(), &bytes, &err);
        if (c.mustRefuse) {
            const bool rightCode = err.code == codes::kHdrUnverifiable;
            Check(!ok && bytes.empty() && rightCode, c.what);
        } else if (EncodeFormatSupported(ImageFormat::kPng, nullptr)) {
            Check(ok && !bytes.empty(), c.what);
        }
    }

    // tone mapping 本体在共用层（Linux 也编得过、也真跑得起来）：FP16 scRGB 的已知色块
    // 映射后必须是 8 位 BGRA，且 toneMapped 这一句自己置上。
    CapturedFrame wide;
    wide.width = 4;
    wide.height = 2;
    wide.stride = 4 * 8;   // RGBA16 浮点：每像素 8 字节
    wide.pixels.assign(wide.stride * 2, 0);
    // scRGB 那一帧的内存顺序是 R,G,B,A（DXGI_FORMAT_R16G16B16A16_FLOAT），与交付的 8 位帧
    // 那一套 B,G,R,A **相反**：这正是映射函数自己按通道名读写的东西，判据把它写死在这里，
    // 谁改了顺序立刻露出来。
    auto putHalf = [&](uint32_t x, uint32_t y, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
        uint16_t* p = reinterpret_cast<uint16_t*>(wide.pixels.data() +
                                                  static_cast<size_t>(y) * wide.stride +
                                                  static_cast<size_t>(x) * 8);
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    };
    putHalf(0, 0, 0x3C00, 0x3C00, 0x3C00, 0x3C00);  // 1.0,1.0,1.0 = SDR 参考白
    putHalf(1, 0, 0x3C00, 0, 0, 0x3C00);            // 线性红 1.0
    putHalf(2, 0, 0, 0x3C00, 0, 0x3C00);            // 线性绿 1.0
    putHalf(3, 0, 0, 0, 0x3C00, 0x3C00);            // 线性蓝 1.0
    putHalf(0, 1, 0x3800, 0x3800, 0x3800, 0x3C00);  // 0.5 灰
    putHalf(1, 1, 0, 0, 0, 0x3C00);                 // 黑
    putHalf(2, 1, 0x4000, 0x4000, 0x4000, 0x3C00);  // 2.0：超亮，要被曲线压回 [0,1]
    putHalf(3, 1, 0, 0, 0, 0);                       // 全透明
    wide.sourceColorSpace = FrameColorSpace::kScRgbFloat16;
    wide.source = L"synthetic";
    wide.path = L"synthetic.wide";

    Diagnostic err;
    const Deadline& noDl = NoBudget();
    if (ConvertWideFrameToSdrBgra8(&wide, &noDl, &err)) {
        Check(wide.toneMapped, "映射过的帧自己把 toneMapped 置真（sourceColorSpace 留给报告）");
        Check(wide.stride == wide.width * 4u && wide.pixels.size() == 4u * 4u * 2u,
              "映射后交出的永远是 8 位 BGRA 紧凑帧");
        // 期望值是那条固定曲线自己算的（相对白 4.0 的扩展 Reinhard + sRGB 正 OETF）：
        //   1.0 的 SDR 白 → 0.531 相对线性 → 每通道 193；2.0 → 0.75 → 225（更亮但仍在 [0,1] 之内）。
        // 不写成"应当接近 255"是因为那等于要求这条曲线是恒等的 —— 它偏亮部压缩是设计选择。
        uint8_t* white = wide.pixels.data();
        Check(white[0] >= 185 && white[0] <= 200 && white[0] == white[1] && white[1] == white[2],
              "scRGB 的 1.0 白映射到中性灰阶（三通道相同，落在曲线给的那一档）");
        uint8_t* red = wide.pixels.data() + 4;
        Check(red[2] >= 225 && red[1] == 0 && red[0] == 0, "线性红 1.0 仍是红（通道没有换位）");
        uint8_t* green = wide.pixels.data() + 8;
        Check(green[1] >= 190 && green[0] == 0 && green[2] == 0, "线性绿 1.0 仍是绿");
        uint8_t* blue = wide.pixels.data() + 12;
        Check(blue[0] >= 240 && blue[1] == 0 && blue[2] == 0, "线性蓝 1.0 仍是蓝");
        uint8_t* over = wide.pixels.data() + static_cast<size_t>(wide.stride) + 2 * 4;
        Check(over[0] > white[0],
              "2.0 的超亮比 1.0 更亮但被曲线压回 8 位之内（单调；能落进 uint8 就是没留 >1 的说法）");
        uint8_t* grey = wide.pixels.data() + static_cast<size_t>(wide.stride);
        Check(grey[0] < white[0] && grey[0] > 100, "0.5 的中灰比 1.0 暗（曲线保序）");
        uint8_t* transparent = wide.pixels.data() + static_cast<size_t>(wide.stride) + 3 * 4;
        Check(transparent[3] == 0, "alpha 不参与映射，透明仍是透明");
        std::vector<uint8_t> bytes;
        const bool canPng = EncodeFormatSupported(ImageFormat::kPng, nullptr);
        const bool encoded = EncodeFrame(wide, ImageFormat::kPng, 95, noDl, &bytes, &err);
        Check(encoded == canPng && (!canPng || !bytes.empty()),
              "映射过的帧可以进编码器（拒与编的答复与能力表一致）");
    } else {
        Note("本平台没链上映射用的 8 位 BGRA 前置件，wide 帧这一节只判到这里");
    }

    // 坏形状 + HDR 的组合：形状那一关在前（说不通形状的帧连来源都问不出来）
    CapturedFrame broken = MakeTestFrame(8, 8, 0);
    broken.pixels.resize(broken.pixels.size() - 1);
    broken.sourceColorSpace = FrameColorSpace::kScRgbFloat16;
    std::vector<uint8_t> bytes;
    Diagnostic err2;
    Check(!CheckEncodeInput(broken, ImageFormat::kPng, NoBudget(), &err2) &&
              err2.code == codes::kFrameInvalid,
          "坏形状排在 HDR 判定之前：先说形状说不通");
}

// ---------------------------------------------------------------------------
// 6) 错误路径：不交半张图、稳定码对得上
// ---------------------------------------------------------------------------
void TestErrorPaths() {
    Section("错误路径：形状、尺寸、格式与空产物");

    auto expectFail = [&](const CapturedFrame& f, ImageFormat fmt, const wchar_t* wantCode,
                          const char* what) {
        std::vector<uint8_t> bytes(7, 0xAB);   // 预先放脏字节，验证失败时不交出半张图
        Diagnostic err;
        const bool ok = EncodeFrame(f, fmt, 95, NoBudget(), &bytes, &err);
        Check(!ok && bytes.empty() && err.code == wantCode && err.stage == stages::kEncode &&
                  err.option == L"--format",
              what);
    };

    CapturedFrame zero = MakeTestFrame(1, 1, 0);
    zero.width = 0;
    zero.height = 0;
    zero.stride = 0;
    zero.pixels.clear();
    expectFail(zero, ImageFormat::kPng, codes::kCaptureFailed, "零尺寸：enc.size_zero（capture.failed）");

    CapturedFrame thin = MakeTestFrame(8, 8, 0);
    thin.pixels.resize(thin.pixels.size() - 1);
    expectFail(thin, ImageFormat::kPng, codes::kFrameInvalid, "缓冲区短一个字节：capture.frame_invalid");

    CapturedFrame pitch = MakeTestFrame(8, 8, 0);
    pitch.stride = 8 * 4u - 4u;
    expectFail(pitch, ImageFormat::kPng, codes::kFrameInvalid, "行距装不下一行：capture.frame_invalid");

    CapturedFrame fat = MakeTestFrame(8, 8, 0);
    fat.stride = 8 * 4u * 3u;
    fat.pixels.assign(static_cast<size_t>(fat.stride) * fat.height, 0);
    expectFail(fat, ImageFormat::kPng, codes::kFrameInvalid, "行距超过行长两倍：capture.frame_invalid");

    CapturedFrame huge = MakeTestFrame(1, 1, 0);
    huge.width = kFrameMaxSide + 1u;
    huge.stride = huge.width * 4u;
    expectFail(huge, ImageFormat::kPng, codes::kFrameInvalid, "单边超上限：capture.frame_invalid");

    // 外部声明的尺寸换不来一次无界分配：形状那一关在 CheckEncodeInput 里，
    // 那时缓冲区还是空的，之后一次分配都没有发生。
    CapturedFrame declare;
    declare.width = kFrameMaxSide;
    declare.height = kFrameMaxSide;
    declare.stride = kFrameMaxSide * 4u;
    declare.pixels.assign(64, 0);   // 只给 64 字节，声明却是 1 GiB
    Diagnostic err;
    const size_t capacityBefore = declare.pixels.capacity();
    Check(!CheckEncodeInput(declare, ImageFormat::kBmp, NoBudget(), &err) &&
              err.code == codes::kFrameInvalid && declare.pixels.size() == 64 &&
              declare.pixels.capacity() == capacityBefore,
          "声明 1 GiB 而缓冲只有 64 字节 = 当场拒绝，不分配、不搬");

    // 这条路径没有编码器的格式：失败 + 一句"现在编得出哪几种"
    for (const wchar_t* name : kAllFormats) {
        const ImageFormat fmt = FormatOf(name);
        const char* reason = nullptr;
        const bool declared = EncodeFormatSupported(fmt, &reason);
        CapturedFrame f = MakeTestFrame(8, 8, 0);
        std::vector<uint8_t> bytes;
        Diagnostic e;
        const bool encoded = EncodeFrame(f, fmt, 95, NoBudget(), &bytes, &e);
        Check(declared == encoded && (declared || bytes.empty()),
              (std::string("能力与实请求一致：") + ToAscii(name)).c_str());
        if (!declared) {
            Check(e.code == codes::kEncoderUnavailable &&
                      e.hint.find(L"bmp") != std::wstring::npos,
                  (std::string("不支持的格式带着当前可用清单：") + ToAscii(name)).c_str());
            Check(reason && *reason, "不支持的理由不是空话");
        }
    }

    // 空产物 = 失败：这条只在平台编码器一声不响交出 0 字节时才判得了，
    // 这里用"编码器被绕过"那条文路的等价现场：直接问共用层的空产物核对。
    Note("空产物那条路径由 EncodeFrame 收尾处的字节数核对兜住；本轮没有可控的假编码器注入点，"
         "未构造该现场");

    // 错误诊断里的文案 key 四语都要真取得到
    Section("文案：新增的 key 在四种语言里都读得到");
    const wchar_t* keys[] = {L"enc.format_unsupported", L"enc.available", L"enc.detail",
                             L"enc.size_zero", L"enc.failed", L"enc.empty", L"enc.too_large",
                             L"enc.exception", L"cap.hdr_unverifiable"};
    for (const wchar_t* k : keys) {
        const std::wstring m = Msg(k);
        Check(!m.empty() && m.front() != L'?', ("文案 key 取得到：" + ToAscii(k)).c_str());
    }
}

// ---------------------------------------------------------------------------
// 7) 预算与资源
// ---------------------------------------------------------------------------
void TestBudget() {
    Section("预算：开工前拒一次、中途到点不交半张图");

    CapturedFrame f = MakeTestFrame(64, 48, 0);

    // 开工之前预算已经用尽
    int64_t tick = 0;
    Deadline spent = Deadline::FromTotalMs(10, [&tick]() { return ++tick * 1000; });
    std::vector<uint8_t> bytes(5, 0x5A);
    Diagnostic err;
    Check(!EncodeFrame(f, ImageFormat::kPng, 95, spent, &bytes, &err) && bytes.empty() &&
              err.code == codes::kCaptureTimeout && err.stage == stages::kEncode,
          "预算已尽：一个字节都不产，报 capture.timeout");

    // 中途到点：假时钟每问一次就走 100ms，预算 250ms —— 开工前那一次问还来得及，
    // 逐行写数据那几次的某一问答不过去，于是从库里跳回来。
    int64_t running = 0;
    Deadline mid = Deadline::FromTotalMs(250, [&running]() { return ++running * 100; });
    std::vector<uint8_t> partial(5, 0x5A);
    Diagnostic err2;
    const bool ok = EncodeFrame(f, ImageFormat::kPng, 95, mid, &partial, &err2);
    Check(!ok && partial.empty(), "中途到点：交回空字节，不交出那半张图");
    Check(err2.code == codes::kCaptureTimeout || err2.code == codes::kEncoderUnavailable,
          "中途到点说的是期限或编码失败，不是一条空错误");

    // 不限预算时同一张帧一定能编出去（上面那条不是"永远失败"）
    std::vector<uint8_t> good;
    Diagnostic err3;
    if (EncodeFormatSupported(ImageFormat::kPng, nullptr)) {
        Check(EncodeFrame(f, ImageFormat::kPng, 95, NoBudget(), &good, &err3) && !good.empty(),
              "没有预算限制时同一张帧编得出去（上面那两条不是一次普遍失败）");
    }

    // 编码不许替调用方结束进程、也不许往标准流里抱怨：把 stderr 临时接到临时文件，
    // 跑一轮四种格式，读回来必须是空的。（Linux 实测；Windows 那一路的库抱怨由
    // WinRT 的异常文路承接，本轮没有对应现场。）
#if !defined(_WIN32)
    {
        const int saved = dup(2);
        char path[] = "/tmp/ecapture-encode-stderr-XXXXXX";
        const int fd = mkstemp(path);
        if (saved >= 0 && fd >= 0) {
            FILE* redirect = fdopen(fd, "w");
            if (redirect) {
                fflush(stderr);
                dup2(fileno(redirect), 2);
                std::vector<uint8_t> sink;
                Diagnostic e;
                for (const wchar_t* name : kAllFormats) {
                    (void)EncodeFrame(f, FormatOf(name), 95, NoBudget(), &sink, &e);
                }
                (void)EncodeFrame(f, ImageFormat::kBmp, 95, mid, &sink, &e);
                fflush(stderr);
                dup2(saved, 2);
                fclose(redirect);
                FILE* back = fopen(path, "rb");
                long size = 0;
                if (back) {
                    fseek(back, 0, SEEK_END);
                    size = ftell(back);
                    fclose(back);
                }
                remove(path);
                Check(size == 0, "一轮编码没有往 stderr 写任何东西（库的抱怨被接住了）");
            } else {
                dup2(saved, 2);
                close(fd);
                Check(false, "stderr 重定向失败，这条没判成");
            }
        } else {
            if (saved >= 0) close(saved);
            if (fd >= 0) close(fd);
            Check(false, "dup/mkstemp 失败，这条没判成");
        }
        close(saved);
    }
#else
    Note("Windows 上这一轮不判 stderr（本轮没有那台 MSVC 构建），判据链的是 Linux 那一份现场");
#endif

    // 编码不连显示服务器：跑完上面所有编码之后，本进程的地址映射里不许出现
    // X11 / Wayland / GTK / wx 的任何一份库。（Linux 实测。）
#if !defined(_WIN32)
    {
        FILE* maps = fopen("/proc/self/maps", "rb");
        bool loadedGui = false;
        if (maps) {
            char line[512];
            while (fgets(line, sizeof(line), maps)) {
                if (strstr(line, "libX11") || strstr(line, "libgtk") || strstr(line, "libwx") ||
                    strstr(line, "wayland") || strstr(line, "libgdk")) {
                    loadedGui = true;
                    break;
                }
            }
            fclose(maps);
        }
        Check(!loadedGui, "整个编码过程没有把任何 GUI / 显示服务器的库映射进进程");
    }
#endif
}

// ---------------------------------------------------------------------------
// 8) 同一份编码字节供主交付与历史留档
// ---------------------------------------------------------------------------
class RecordingSink final : public OutputSink {
public:
    bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& data, bool,
                  Diagnostic*) override {
        ++saveCalls;
        lastPath = path;
        bytes = data;
        return true;
    }
    bool EmitBytes(const std::vector<uint8_t>& data, uint64_t* emitted,
                   NativeErrorCode*) override {
        ++emitCalls;
        bytes = data;
        if (emitted) *emitted = data.size();
        return true;
    }
    int saveCalls = 0, emitCalls = 0;
    std::wstring lastPath;
    std::vector<uint8_t> bytes;
};

class RecordingArchiver final : public HistoryArchiver {
public:
    bool Archive(const std::wstring& primaryPath, ImageFormat format,
                 const std::vector<uint8_t>& data, HistoryRecord* record,
                 Diagnostic*) override {
        ++calls;
        primary = primaryPath;
        fmt = format;
        bytes = data;
        record->state = HistoryRecord::State::kSaved;
        record->file = L"/history/only/one/copy.png";
        return true;
    }
    int calls = 0;
    std::wstring primary;
    ImageFormat fmt = ImageFormat::kPng;
    std::vector<uint8_t> bytes;
};

void TestSharedEncodedBytes() {
    Section("同一份编码字节供主交付与历史副本");

    CapturedFrame f = MakeTestFrame(48, 32, 0);
    std::vector<uint8_t> encoded;
    Diagnostic err;
    if (!EncodeFrame(f, ImageFormat::kBmp, 95, NoBudget(), &encoded, &err)) {
        Check(false, "bmp 编码成功（下面三条都依赖它）");
        return;
    }
    const std::vector<uint8_t> encodedCopy = encoded;

    RecordingSink sink;
    RecordingArchiver archiver;
    Deadline dl;
    CaptureOutcome outcome;
    DeliveryRun run;
    run.dl = &dl;
    run.sink = &sink;
    run.clock = []() { return 42ull; };
    run.history = &archiver;
    run.format = ImageFormat::kBmp;

    DeliveryTarget target;
    target.file = L"/tmp/evernightcapture-encode-check.bmp";
    target.tag = L"0x00000010";
    target.backend = L"synthetic";
    PendingImage pending;
    pending.startedClockMs = 40;
    pending.image.file = target.file;
    pending.image.width = f.width;
    pending.image.height = f.height;

    DeliveryStep step;
    DeliverImage(run, target, std::move(pending), encoded, &outcome, &step);
    Check(step.delivered && step.recorded && outcome.images.size() == 1, "主交付入账");
    Check(sink.bytes == encodedCopy, "出口拿到的就是那一份编码字节");
    Check(archiver.calls == 1 && archiver.bytes == encodedCopy,
          "历史副本拿的是同一份字节（不重拍、不重编码、不读主文件）");
    Check(archiver.fmt == ImageFormat::kBmp && encoded == encodedCopy,
          "编码这一步在整个交付链里只发生了一次（源字节与产物都没变）");
    Check(outcome.images[0].history.state == HistoryRecord::State::kSaved &&
              outcome.images[0].bytes == encodedCopy.size(),
          "images[].bytes 与 history 那一格说的是同一次落地");
}

// ---------------------------------------------------------------------------
// 9) 能力清单本身
// ---------------------------------------------------------------------------
void TestAvailableFormatsString() {
    Section("AvailableFormats() 与登记表同源");

    const std::wstring list = AvailableFormats();
    Check(!list.empty(), "清单不为空（空清单等于说「这个构建一种都编不出」，那是缺陷）");
    for (const wchar_t* name : kAllFormats) {
        const ImageFormat fmt = FormatOf(name);
        const std::wstring alias = FormatDisplayAlias(fmt);
        const bool inList = list.find(alias) != std::wstring::npos;
        const bool supported = EncodeFormatSupported(fmt, nullptr);
        Check(inList == supported,
              ("清单里出现的格式与登记的可用一致：" + ToAscii(name)).c_str());
    }
    if (BuildCodecPath() == CodecPath::kPosixLibCodecs) {
        Check(list == L"png, jpg, bmp", "posix 那一路的清单就是 png / jpg / bmp");
        Check(list.find(L"tiff") == std::wstring::npos && list.find(L"gif") == std::wstring::npos,
              "清单里不含这条路径没有编码器的两种");
    } else {
        Check(list == L"png, jpg, bmp, tiff, gif",
              "winrt 那一路的清单与这条选项存在以来一直给的那句逐字节相同");
    }
    const CodecPath path = EncoderCurrentPath();
    Check(path == BuildCodecPath(), "这个构建报出的路径就是编译期那一条");
    Check(std::string(CodecPathName(path)) ==
              (path == CodecPath::kWinRtBitmapEncoder ? "winrt.bitmapencoder" : "posix.libcodecs"),
          "路径的 ASCII 机器名登记过");
}

}  // namespace

int main() {
    std::printf("EvernightCapture 跨平台图像处理与编码判据（编码路径：%s）\n",
                CodecPathName(BuildCodecPath()));
    TestCodecTable();
    TestFrameSpec();
    TestCropScaleRules();
    TestColorAndAlpha();
    TestHdrBoundary();
    TestErrorPaths();
    TestBudget();
    TestSharedEncodedBytes();
    TestAvailableFormatsString();

    // 汇总行的格式与其余判据一致（test-all.sh 与 core-portability-check.sh 都读这一句记账）
    std::printf("\nencode-state: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
