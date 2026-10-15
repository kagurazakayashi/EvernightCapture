#include "CaptureDiag.h"

namespace ecapture {

void CaptureDiag(Diagnostic* err, const wchar_t* channel, const std::wstring& message,
                 const std::wstring& hint, const wchar_t* code) {
    if (!err) return;
    *err = Diagnostic{code, message, L"--capture", channel, hint, std::wstring(), channel,
                      stages::kCapture};
}

}  // namespace ecapture
