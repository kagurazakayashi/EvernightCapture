#pragma once
// 【平台层 · Linux】PipeWire 图像获取后端工厂。
//
// 调用方先把 XDG Desktop Portal ScreenCast 的 OpenPipeWireRemote() 返回 fd 交进来；
// 本后端只用这个 fd 建立 pw_context，目标节点只来自当前 Portal 会话的 stream 元数据。
// 没有链接 libpipewire-0.3 的构建返回一个每次调用都如实报 unavailable 的替身。

#include <memory>

#include "../../PipeWireCapture.h"

namespace ecapture {
namespace pipewire {

std::unique_ptr<IPipeWireCapture> CreatePipeWireCapture();

}  // namespace pipewire
}  // namespace ecapture
