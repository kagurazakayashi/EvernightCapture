#include "Encoder.h"

#include <cstring>
#include <memory>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <synchapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

#include "ImageOps.h"
#include "WinrtApartment.h"

namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wss = winrt::Windows::Storage::Streams;
namespace wf = winrt::Windows::Foundation;
namespace wfc = winrt::Windows::Foundation::Collections;

namespace ecapture {
namespace {

std::wstring HresultText(winrt::hresult hr) {
    wchar_t buf[40];
    swprintf(buf, 40, L"0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

bool Err(Diagnostic* out, const wchar_t* code, std::wstring message, const std::wstring& format,
         const std::wstring& detail, const std::wstring& hresult = std::wstring()) {
    if (!out) return false;
    const std::wstring hint = Msgf(L"enc.available", AvailableFormats());
    *out = Diagnostic{code, std::move(message), L"--format", format,
                      detail.empty() ? hint : Msgf(L"enc.detail", hint, detail),
                      std::wstring(), std::wstring(), stages::kEncode};
    out->hresult = hresult;   // 只在真拿得到 HRESULT 的那条文路上出现，别填个 0 进去
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

// ---------------------------------------------------------------------------
// 带期限的异步等待
// ---------------------------------------------------------------------------

// 完成事件。句柄由回调持有的那份 shared_ptr 管着：哪怕调用方先按期限走人，
// 操作后来才结束，SetEvent 写的仍然是一个还活着的句柄。
// 反过来（先关句柄、回调之后才触发）就是往别人复用的句柄上写 —— 那是未定义行为。
struct CompletionEvent {
    HANDLE h = CreateEventEx(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET,
                             EVENT_MODIFY_STATE | SYNCHRONIZE);
    CompletionEvent() = default;
    ~CompletionEvent() { if (h) CloseHandle(h); }
    CompletionEvent(const CompletionEvent&) = delete;
    CompletionEvent& operator=(const CompletionEvent&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// 等一个 WinRT 异步操作到终态，等待时长只按剩余预算给。
// 到点就 Cancel() 请求取消并返回 false —— 不"先等到有结果再判断是不是超时"。
// 取消是合作式的：组件不理这次取消时，那条等待由它自己结束（我们不再等它），
// 期限覆盖不到的这一段照实记进文档，不假装已经强制停下来了。
template <typename Async>
bool AwaitAsync(const Async& async, const Deadline& dl, Diagnostic* err, const std::wstring& label) {
    auto ev = std::make_shared<CompletionEvent>();
    if (!*ev) {
        const DWORD gle = GetLastError();
        return Err(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                   Msgf(L"err.win32_code", gle));
    }
    async.Completed([ev](auto const&, wf::AsyncStatus) { SetEvent(ev->h); });
    if (WaitForSingleObject(ev->h, WaitTimeout(dl)) == WAIT_OBJECT_0) return true;
    async.Cancel();
    Diagnostic d = BudgetSpent(dl, codes::kCaptureTimeout, stages::kEncode, nullptr);
    d.option = L"--format";
    d.value = label;
    if (err) *err = d;
    return false;
}

// 终态 -> 结果。失败与取消都让 GetResults() 抛出 hresult_error，由外层统一换成诊断 + 真码。
template <typename Async>
auto ResultsOf(const Async& async) { return async.GetResults(); }

}  // namespace

std::wstring AvailableFormats() {
    return L"png, jpg, bmp, tiff, gif";
}

bool EncodeFrame(const CapturedFrame& frame, ImageFormat fmt, int jpegQuality, const Deadline& dl,
                 std::vector<uint8_t>* bytes, Diagnostic* err) {
    const std::wstring label = FormatLabel(fmt);
    if (frame.width == 0 || frame.height == 0)
        return Err(err, codes::kCaptureFailed, Msg(L"enc.size_zero"), label, std::wstring());
    // 预算已经用尽就别开工：这一步一旦开始，能中断的只有那几个异步等待点
    if (dl.Spent()) {
        Diagnostic d = BudgetSpent(dl, codes::kCaptureTimeout, stages::kEncode, nullptr);
        d.option = L"--format";
        d.value = label;
        *err = d;
        return false;
    }

    try {
        // 套间是按线程的：编码可能跑在不是第一次初始化 COM 的那条线程上，
        // 所以这里不能拿"进程里初始化过一次"当凭证（见 WinrtApartment.h）。
        HRESULT aptHr = S_OK;
        if (!EnsureWinrtOnThisThread(&aptHr)) {
            return Err(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                       Msgf(L"enc.apartment", HresultText(aptHr)), HresultText(aptHr));
        }

        auto tight = PackTight(frame);
        wss::InMemoryRandomAccessStream stream;
        const winrt::guid id = EncoderIdFor(fmt);
        wgi::BitmapEncoder encoder{nullptr};

        if (fmt == ImageFormat::kJpeg) {
            wgi::BitmapPropertySet props;
            props.Insert(L"ImageQuality",
                         wgi::BitmapTypedValue(winrt::box_value(static_cast<float>(jpegQuality) / 100.0f),
                                              wf::PropertyType::Single));
            auto createAsync = wgi::BitmapEncoder::CreateAsync(id, stream, props);
            if (!AwaitAsync(createAsync, dl, err, label)) return false;
            encoder = ResultsOf(createAsync);
        } else {
            auto createAsync = wgi::BitmapEncoder::CreateAsync(id, stream);
            if (!AwaitAsync(createAsync, dl, err, label)) return false;
            encoder = ResultsOf(createAsync);
        }

        encoder.SetPixelData(wgi::BitmapPixelFormat::Bgra8, wgi::BitmapAlphaMode::Ignore, frame.width,
                             frame.height, 96.0, 96.0,
                            winrt::array_view<const uint8_t>(tight.data(), static_cast<uint32_t>(tight.size())));
        auto flushAsync = encoder.FlushAsync();
        if (!AwaitAsync(flushAsync, dl, err, label)) return false;
        ResultsOf(flushAsync);   // IAsyncAction 没有返回值，但失败要在这里抛出来

        const uint64_t size = stream.Size();
        if (size == 0) {
            // 编码器一声不响地给了 0 字节：这也是一次失败，必须留下诊断，
            // 否则调用方拿到的 errors 里会出现一条没有 code 的条目。
            return Err(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                       Msg(L"enc.empty"));
        }
        if (size > 0xFFFFFFFFull) {
            // DataReader 一次最多读 4GB，这条上限先说清楚而不是把长度截断成一个小值去读
            return Err(err, codes::kEncoderUnavailable, Msg(L"enc.too_large"), label,
                       std::to_wstring(size));
        }
        bytes->assign(static_cast<size_t>(size), 0);
        wss::DataReader reader(stream.GetInputStreamAt(0));
        auto loadAsync = reader.LoadAsync(static_cast<uint32_t>(size));
        if (!AwaitAsync(loadAsync, dl, err, label)) return false;
        ResultsOf(loadAsync);
        reader.ReadBytes(winrt::array_view<uint8_t>(bytes->data(), static_cast<uint32_t>(bytes->size())));
        return !bytes->empty();
    } catch (const winrt::hresult_error& e) {
        return Err(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                     Msgf(L"cap.hresult", HresultText(e.code())), HresultText(e.code()));
    } catch (const std::exception&) {
        return Err(err, codes::kEncoderUnavailable, Msg(L"enc.exception"), label, std::wstring());
    }
}

}  // namespace ecapture
