#pragma once
// 【平台接口】会话探测：交给共用判据的只有已经收好的事实。
//
// 实现按平台分：Linux 在 src/platform/linux/PosixSessionProbe.cpp 里真正去连 X11 / Wayland /
// D-Bus / PipeWire 的套接字。这里不声明任何原生类型，也不提供"猜一个"的默认实现 ——
// 没有这一份实现的平台就不该声称自己探测过会话。

#include "core/SessionCapability.h"

namespace ecapture {

// 只读探测：连接显示服务器与用户会话总线、读协议/接口版本、检查 PipeWire 套接字是否存在。
// 不选来源、不创建捕获会话、不读像素、不弹框、不启动任何服务、不安装依赖、不提权。
SessionFacts ProbeSessionFacts();

}  // namespace ecapture
