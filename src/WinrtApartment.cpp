#include "WinrtApartment.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <roapi.h>

namespace ecapture {
namespace {

// 一线程一份：构造时建立套间，析构（线程退出）时配平还回去。
// 用 thread_local 的动态初始化 —— 它只在**本线程第一次用到**这个对象时才构造，
// 于是"没跑过 WinRT 的线程"不会莫名其妙多出一个套间。
class ThreadApartment {
public:
    ThreadApartment() {
        hr_ = RoInitialize(RO_INIT_MULTITHREADED);
        // S_FALSE = 这条线程已经初始化过（也算可用）；RPC_E_CHANGED_MODE = 已按别的并发模式
        // 初始化过：能用，但那次初始化不归我们还，所以 owned_ 保持 false。
        ok_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
        owned_ = hr_ == S_OK;
    }
    ~ThreadApartment() {
        if (owned_) RoUninitialize();
    }
    ThreadApartment(const ThreadApartment&) = delete;
    ThreadApartment& operator=(const ThreadApartment&) = delete;

    bool ok() const { return ok_; }
    HRESULT hr() const { return hr_; }

private:
    bool ok_ = false;
    bool owned_ = false;
    HRESULT hr_ = S_OK;
};

thread_local ThreadApartment g_apartment;

}  // namespace

bool EnsureWinrtOnThisThread(HRESULT* hr) {
    if (hr) *hr = g_apartment.hr();
    return g_apartment.ok();
}

}  // namespace ecapture
