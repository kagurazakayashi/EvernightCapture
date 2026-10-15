// 【平台层 · Windows】WinRT 编码实现：Windows.Graphics.Imaging.BitmapEncoder。
//
// 为什么用它而不是 WIC：它与 WGC 同一套依赖（同一个 windowsapp.lib，不额外引入 COM 组件），
// 而且 JPEG 质量接口明确；WIC 那一路的编码器属性袋 CLSID 在当前 SDK 里没有公开声明。
//
// 这一份只兑现 EncodeFrameNative()：帧形状、紧凑行、颜色空间、格式支持与预算开工判据
// 全部由共用层（src/Encoder.cpp）判完才会到这里，所以这里不再重复任何一道检查
// （一条判据只写一处）。唯一留在这里的是 WinRT 自己那几条文路的失败归类与真码。
//
// 与这条选项存在之前相比，Windows 上发出的稳定码、message、hint 与 hresult 逐字节不变。

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

#include "Lang.h"             // 编码那一段的诊断文案（enc.*）
#include "WinrtApartment.h"
#include "platform/windows/WinBridge.h"   // WaitTimeout：截止时间到 Win32 等待参数的换算在平台侧

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
        return EncodeFailure(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                             Msgf(L"err.win32_code", gle), std::wstring(), nullptr);
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

// 这五种编码器都由 windowsapp.lib 里那个 BitmapEncoder 提供，本次构建一定链得上
// （没有"实现写了而库没链上"这一档 —— 那一档属于 Linux 侧的 libpng / libjpeg）。
bool CodecLibraryLinked(ImageFormat format) {
    switch (format) {
        case ImageFormat::kPng:
        case ImageFormat::kJpeg:
        case ImageFormat::kBmp:
        case ImageFormat::kTiff:
        case ImageFormat::kGif:
            return true;
    }
    return false;
}

bool EncodeFrameNative(const CapturedFrame& frame, const std::vector<uint8_t>& tight,
                       ImageFormat fmt, int jpegQuality, const Deadline& dl,
                       std::vector<uint8_t>* bytes, Diagnostic* err) {
    const std::wstring label = FormatName(fmt);
    try {
        // 套间是按线程的：编码可能跑在不是第一次初始化 COM 的那条线程上，
        // 所以这里不能拿"进程里初始化过一次"当凭证（见 WinrtApartment.h）。
        HRESULT aptHr = S_OK;
        if (!EnsureWinrtOnThisThread(&aptHr)) {
            return EncodeFailure(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                                 Msgf(L"enc.apartment", HresultText(aptHr)), HresultText(aptHr),
                                 nullptr);
        }

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
        if (size > 0xFFFFFFFFull) {
            // DataReader 一次最多读 4GB，这条上限先说清楚而不是把长度截断成一个小值去读
            return EncodeFailure(err, codes::kEncoderUnavailable, Msg(L"enc.too_large"), label,
                                 std::to_wstring(size), std::wstring(), nullptr);
        }
        bytes->assign(static_cast<size_t>(size), 0);
        wss::DataReader reader(stream.GetInputStreamAt(0));
        auto loadAsync = reader.LoadAsync(static_cast<uint32_t>(size));
        if (!AwaitAsync(loadAsync, dl, err, label)) return false;
        ResultsOf(loadAsync);
        reader.ReadBytes(winrt::array_view<uint8_t>(bytes->data(), static_cast<uint32_t>(bytes->size())));
        return true;
    } catch (const winrt::hresult_error& e) {
        return EncodeFailure(err, codes::kEncoderUnavailable, Msg(L"enc.failed"), label,
                             Msgf(L"cap.hresult", HresultText(e.code())), HresultText(e.code()),
                             nullptr);
    } catch (const std::exception&) {
        return EncodeFailure(err, codes::kEncoderUnavailable, Msg(L"enc.exception"), label,
                             std::wstring(), std::wstring(), nullptr);
    }
}

}  // namespace ecapture
