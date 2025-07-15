#pragma once
// WinRT / COM 套间：**按线程**初始化，按线程释放。
//
// 为什么不能用进程级"只初始化一次"：RoInitialize 的作用域是**线程**，一个 static bool
// 只会让第一个调用它的线程建立套间，之后别的线程以为"已经好了"而根本没初始化，
// 症状是那一条线程上每个 WinRT 调用都抛 RPC_E_WRONG_THREAD / CO_E_NOTINITIALIZED。
// 本工具现在有第二条线程（确认框跑在专门的线程上，好让人不答时按期限关掉它），
// 以后也可能有别的等待线程，所以这个假设必须拆掉。
//
// 释放同样按线程：线程退出时把本线程那一次 RoInitialize 配平还回去，不留下
// "计数只加不减"的套间。主线程的配平发生在进程退出时，与 CRT 的清理顺序一致。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace ecapture {

// 本线程是否已经有可用的 WinRT / COM 套间（没有就当场建立）。
// 返回值是"这一次调用之后能不能用 WinRT"，失败时调用方要照实报错，
// 而不是拿着空的 WinRT 对象继续往下走；hr 回收 RoInitialize 的原值（拿得到就别顶 E_FAIL）。
bool EnsureWinrtOnThisThread(HRESULT* hr = nullptr);

}  // namespace ecapture
