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
#include "ImageOps.h"

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
    const std::wstring hint = Msgf(L"enc.available", AvailableFormats());
    *out = Diagnostic{code, std::move(message), L"--format", format,
                      detail.empty() ? hint : Msgf(L"enc.detail", hint, detail)};
    return false;
}

winrt::guid EncoderIdFor(ImageFormat fmt) {
    switch (fmt) {
        case ImageFormat::kPng: return wgi::BitmapEncoder::PngEncoderId();
        case ImageFormat::kJpeg: return wgi::BitmapEncoder::JpegEncoderId();
        case ImageFormat::kBmp: return wgi::BitmapEncoder::BmpEncoderId();
        case ImageFormat::kTiff: return wgi::BitmapEncoder::TiffEncoderId();
        case ImageFormat::kGif: return wgi::BitmapEncoder::GifEncoderId();
    }
    return winrt::guid{};
}

const wchar_t* FormatLabel(ImageFormat fmt) { return FormatName(fmt); }

}  // namespace

std::wstring AvailableFormats() {
    return L"png, jpg, bmp, tiff, gif";
}

bool EncodeFrame(const CapturedFrame& frame, ImageFormat fmt, int jpegQuality,
                 std::vector<uint8_t>* bytes, Diagnostic* err) {
    const std::wstring label = FormatLabel(fmt);
    if (frame.width == 0 || frame.height == 0)
        return Err(err, codes::kCaptureFailed, Msg(L"enc.size_zero"), label, std::wstring());

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
        return Err(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                     Msgf(L"cap.hresult", HresultText(e.code())));
    } catch (const std::exception&) {
        return Err(err, codes::kEncoderUnavailable, Msg(L"enc.exception"), label, std::wstring());
    }
}

}  // namespace ecapture
