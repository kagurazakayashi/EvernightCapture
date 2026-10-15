#pragma once
// 跨平台核心：图片编码格式的支持矩阵 —— "这一条编码路径带不带得回这一格式"这一句的唯一来源。
//
// 为什么这一份要单独存在（与 core/Capability.h 那张截图矩阵分开写）：
//   * 截图矩阵问的是"能不能对某个目标做成这件事"，取值分编译期/运行时/实测三档；
//   * 这一份问的是"字节已经在这一条路径手里了，它编得出哪一种图片"，与目标、会话、授权全无关系。
//     两件事折成一张表就会有一格同时回答两个问题，读的人分不清是哪一路给的答复。
//   * 编码路径的身份也不是平台：Windows 用 WinRT 的 BitmapEncoder，Linux 用 libpng + libjpeg
//     加自写的 BMP，同一条路径在 X11 与 Wayland 上取值完全相同 —— 按路径登记，不按桌面协议登记，
//     免得把"还没实现的后端"与"编码器根本没有这一格式"混成同一条。
//
// 三条规矩：
//   1. **登记表与实际请求同源。** 用户写 --format 时得到的答复（成功、或 enc.format_unsupported）
//      必须由这张表给的那一句支撑，`AvailableFormats()` 也从这张表算。判据逐格式核对
//      "表里说支持" 与 "拿一张合法小图真编一次有没有字节"两件事一致（tests/encode_state.cpp）。
//   2. **没有登记就是缺陷，不是默认值。** kUnregistered 出现在任何一条已列出的路径 × 格式组合上，
//      判据直接红。新增 ImageFormat 取值必须同时补这张表。
//   3. **暂不支持要说是哪一种"不支持"。** 这条路径压根没有这一格式的编码器（kNoEncoder）与
//      "实现写了但这个构建没链上那家编码库"（kLibraryNotLinked）是两种下场：前者的下一步是
//      换格式，后者的下一步是装依赖再配置一次。折成一条就把可执行的动作说没了。

#include <cstdint>

#include "CliOptions.h"   // ImageFormat 的取值集合（--format 的允许列表就是这张表的行）

namespace ecapture {

// 一条编码路径。取值只增不改名；名字进 -v 的诊断与判据（恒 ASCII，不随 --lang 变）。
enum class CodecPath : uint32_t {
    kWinRtBitmapEncoder = 0,  // Windows：Windows.Graphics.Imaging.BitmapEncoder
    kPosixLibCodecs = 1,      // Linux：libpng + libjpeg(+zlib) + 自写的 32 位 BMP
    kUnregistered = 255,      // 没有登记的编码路径 —— 判据不许它出现在结果里
};

inline const char* CodecPathName(CodecPath path) {
    switch (path) {
        case CodecPath::kWinRtBitmapEncoder: return "winrt.bitmapencoder";
        case CodecPath::kPosixLibCodecs: return "posix.libcodecs";
        case CodecPath::kUnregistered: return "unregistered";
    }
    return "unregistered";
}

// 一条格式在某条路径上的支持状态。
enum class CodecSupport : uint32_t {
    kSupported = 0,          // 这条路径带得回这一格式
    kNoEncoder = 1,          // 这条路径没有这一格式的编码器（换格式是唯一可执行的下一步）
    kLibraryNotLinked = 2,   // 实现写了，但这个构建没链上所需的编码库（补依赖后重新配置）
    kUnregistered = 3,       // 没登记 = 缺陷，不当兜底值用
};

inline const char* CodecSupportName(CodecSupport support) {
    switch (support) {
        case CodecSupport::kSupported: return "supported";
        case CodecSupport::kNoEncoder: return "no_encoder";
        case CodecSupport::kLibraryNotLinked: return "library_not_linked";
        case CodecSupport::kUnregistered: return "unregistered";
    }
    return "unregistered";
}

// 这一状态算不算"现在就能编"。kLibraryNotLinked 与 kNoEncoder 都不算 —— 调用方据此
// 决定是报错还是换格式，不拿"实现了但没链上"冒充能力，也不拿"这台机器缺库"说成"本工具不支持"。
inline bool CodecIsUsable(CodecSupport support) { return support == CodecSupport::kSupported; }

struct CodecFormatCell {
    CodecPath path;
    ImageFormat format;
    CodecSupport support;
    const char* reason;   // 恒 ASCII：这一格为什么是这个状态，人类读、判据也读
};

// 登记表本体。行序 = CodecPath 的顺序 × ImageFormat 的顺序，README《图片格式支持》那一节写的
// 就是这张表，改一处要改两处（README 与这张表）。
//
// 两条路径的差异只在"手上有没有那台编码器"：
//   * WinRT 的 BitmapEncoder 提供 png / jpeg / bmp / tiff / gif 五种，五种全支持；
//   * posix.libcodecs 带 png / jpeg / bmp 三种。tiff 与 gif 明确写成 kNoEncoder：
//     本机没链 libtiff，而 gif 只能写 256 色调色板 —— 把一张真彩截图硬压成调色板是一条
//     会静默改变画面的降级，不做"能写出文件就算支持"。要扩这两格，先补这一行再补实现。
//   kLibraryNotLinked 不在静态表里出现：它取决于本次配置真链上了哪几家库，由
//   Encoder.h 的 EncodeFormatSupported() 在运行时按编译期事实覆盖（见那边说明）。
inline const CodecFormatCell* CodecFormatCells(uint32_t* count) {
    static const CodecFormatCell kCells[] = {
        {CodecPath::kWinRtBitmapEncoder, ImageFormat::kPng, CodecSupport::kSupported,
         "BitmapEncoder::PngEncoderId"},
        {CodecPath::kWinRtBitmapEncoder, ImageFormat::kJpeg, CodecSupport::kSupported,
         "BitmapEncoder::JpegEncoderId with ImageQuality"},
        {CodecPath::kWinRtBitmapEncoder, ImageFormat::kBmp, CodecSupport::kSupported,
         "BitmapEncoder::BmpEncoderId"},
        {CodecPath::kWinRtBitmapEncoder, ImageFormat::kTiff, CodecSupport::kSupported,
         "BitmapEncoder::TiffEncoderId"},
        {CodecPath::kWinRtBitmapEncoder, ImageFormat::kGif, CodecSupport::kSupported,
         "BitmapEncoder::GifEncoderId"},

        {CodecPath::kPosixLibCodecs, ImageFormat::kPng, CodecSupport::kSupported,
         "libpng png_write_row with a deadline-checked sink"},
        {CodecPath::kPosixLibCodecs, ImageFormat::kJpeg, CodecSupport::kSupported,
         "libjpeg baseline with jpeg_set_quality and a deadline-checked destination"},
        {CodecPath::kPosixLibCodecs, ImageFormat::kBmp, CodecSupport::kSupported,
         "self-written 32bpp BI_RGB (no colour table, rows bottom-up)"},
        {CodecPath::kPosixLibCodecs, ImageFormat::kTiff, CodecSupport::kNoEncoder,
         "no tiff encoder is linked into this path (libtiff absent); not a probe result"},
        {CodecPath::kPosixLibCodecs, ImageFormat::kGif, CodecSupport::kNoEncoder,
         "gif is 256-colour indexed: writing one would silently poster the frame"},
    };
    *count = sizeof(kCells) / sizeof(kCells[0]);
    return kCells;
}

inline uint32_t CodecFormatCellCount() {
    uint32_t n = 0;
    CodecFormatCells(&n);
    return n;
}

// 取某一格。没登记的那一格返回 kUnregistered —— 调用方据此知道"这一格还没填"，
// 而不是拿到一个看起来可用的默认支持状态。
inline CodecSupport CodecSupportFor(CodecPath path, ImageFormat format) {
    uint32_t n = 0;
    const CodecFormatCell* cells = CodecFormatCells(&n);
    for (uint32_t i = 0; i < n; ++i) {
        if (cells[i].path == path && cells[i].format == format) return cells[i].support;
    }
    return CodecSupport::kUnregistered;
}

// 同一格的 ASCII 理由（进诊断细节与判据）。没有登记的那一格给 "unregistered"。
inline const char* CodecSupportReason(CodecPath path, ImageFormat format) {
    uint32_t n = 0;
    const CodecFormatCell* cells = CodecFormatCells(&n);
    for (uint32_t i = 0; i < n; ++i) {
        if (cells[i].path == path && cells[i].format == format) return cells[i].reason;
    }
    return "unregistered";
}

// 这个构建实际用的是哪条编码路径。与 core/Platform.h 的 BuildPlatform() 同一类编译期事实，
// 也同样是"全仓只在这一处 #if"的那一条：platform/windows/WinEncoder.cpp 与
// platform/linux/PosixEncoder.cpp 各自兑现 Encoder.h 的平台入口，这里只说选的是哪一份。
// 不按 DISPLAY / Wayland 之类的运行时线索猜 —— 编码器与桌面协议无关，
// 加载它不需要连任何显示服务器（判据见 tests/encode_state.cpp 的无显示会话那一节）。
inline CodecPath BuildCodecPath() {
#if defined(_WIN32)
    return CodecPath::kWinRtBitmapEncoder;
#else
    return CodecPath::kPosixLibCodecs;
#endif
}

// 格式名里给"当前可用格式"那一句用的短别名。与 --format 的允许取值同源，
// 但**不等于** FormatName()（那一条进 JSON 的是 jpeg / tiff 这种全称，不能改）。
// 这一份别名的存在理由就是"只写一处"：AvailableFormats() 与 README 的格式表读的都是它。
inline const wchar_t* FormatDisplayAlias(ImageFormat format) {
    switch (format) {
        case ImageFormat::kPng: return L"png";
        case ImageFormat::kJpeg: return L"jpg";
        case ImageFormat::kBmp: return L"bmp";
        case ImageFormat::kTiff: return L"tiff";
        case ImageFormat::kGif: return L"gif";
    }
    return L"?";
}

}  // namespace ecapture
