#include "Encoder.h"

#include <cstring>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#include "CaptureWgc.h"

namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wss = winrt::Windows::Storage::Streams;
namespace wfc = winrt::Windows::Foundation::Collections;

namespace ecapture {
namespace {

std::wstring HresultText(winrt::hresult hr) {
    wchar_t buf[40];
    swprintf(buf, 40, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

bool Err(Diagnostic* out, const wchar_t* code, std::wstring message, const std::wstring& format,
         const std::wstring& detail) {
    if (!out) return false;
    std::wstring hint = L"当前可用：" + AvailableFormats();
    *out = Diagnostic{code, std::move(message), L"--format", format,
                      detail.empty() ? hint : hint + L"（" + detail + L"）"};
    return false;
}

winrt::guid EncoderIdFor(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::kPng: return wgi::BitmapEncoder::PngEncoderId();
        case ImageFormat::kJpeg: return wgi::BitmapEncoder::JpegEncoderId();
        case ImageFormat::kBmp: return wgi::BitmapEncoder::BmpEncoderId();
        case ImageFormat::kTiff: return wgi::BitmapEncoder::TiffEncoderId();
        case ImageFormat::kGif: return wgi::BitmapEncoder::GifEncoderId();
        case ImageFormat::kIco: return wgi::BitmapEncoder::JpegXREncoderId();  // 占位，稍后判失败
        default: return winrt::guid{};
    }
}

const wchar_t* FormatLabel(ImageFormat fmt) { return FormatName(fmt); }

// WGC 给的行距可能大于 width*4，编码接口要求紧凑行，先重排
std::vector<uint8_t> PackTight(const CapturedFrame& frame) {
    const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
    std::vector<uint8_t> tight(rowBytes * frame.height);
    for (uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(tight.data() + y * rowBytes,
                    frame.pixels.data() + static_cast<size_t>(y) * frame.stride, rowBytes);
    }
    return tight;
}

}  // namespace

std::wstring AvailableFormats() {
    return L"png, jpg, bmp, tiff, gif";
}

bool EncodeFrame(const CapturedFrame& frame, ImageFormat fmt, int jpegQuality,
                 std::vector<uint8_t>* bytes, Diagnostic* err) {
    const std::wstring label = FormatLabel(fmt);
    if (frame.width == 0 || frame.height == 0)
        return Err(err, codes::kCaptureFailed, L"帧尺寸为 0，无法编码", label, std::wstring());

    if (fmt == ImageFormat::kIco || fmt == ImageFormat::kWebp)
        return Err(err, codes::kEncoderUnavailable, L"该格式没有可用的编码器", label,
                   L"ICO/WebP 需要额外的系统组件");

    try {
        EnsureWinrtInitialized();

        auto tight = PackTight(frame);
        wss::InMemoryRandomAccessStream stream;
        const winrt::guid id = EncoderIdFor(fmt);
        wgi::BitmapEncoder encoder{nullptr};

        if (fmt == ImageFormat::kJpeg) {
            wgi::BitmapPropertySet props;
            props.Insert(L"ImageQuality",
                         wgi::BitmapTypedValue(winrt::box_value(static_cast<float>(jpegQuality) / 100.0f),
                                              winrt::Windows::Foundation::PropertyType::Single));
            encoder = wgi::BitmapEncoder::CreateAsync(id, stream, props).get();
        } else {
            encoder = wgi::BitmapEncoder::CreateAsync(id, stream).get();
        }

        encoder.SetPixelData(wgi::BitmapPixelFormat::Bgra8, wgi::BitmapAlphaMode::Ignore, frame.width,
                             frame.height, 96.0, 96.0,
                            winrt::array_view<const uint8_t>(tight.data(), static_cast<uint32_t>(tight.size())));
        encoder.FlushAsync().get();

        const uint64_t size = stream.Size();
        bytes->assign(static_cast<size_t>(size), 0);
        if (size > 0) {
            wss::DataReader reader(stream.GetInputStreamAt(0));
            reader.LoadAsync(static_cast<uint32_t>(size)).get();
            reader.ReadBytes(winrt::array_view<uint8_t>(bytes->data(), static_cast<uint32_t>(bytes->size())));
        }
        return !bytes->empty();
    } catch (const winrt::hresult_error& e) {
        return Err(err, codes::kEncoderUnavailable, L"编码失败", label, L"HRESULT " + HresultText(e.code()));
    } catch (const std::exception&) {
        return Err(err, codes::kEncoderUnavailable, L"编码时发生未预期的异常", label, std::wstring());
    }
}

}  // namespace ecapture
