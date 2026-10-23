#pragma once
// 【平台层 · Linux】XDG Desktop Portal ScreenCast 的会话 D-Bus 驱动。
//
// 工厂没有 D-Bus 依赖泄漏到共用层：调用方只看到 IPortalScreenCast。本构建若没有链接
// libdbus-1，工厂仍返回一个对象，但每次调用都如实报 unavailable，绝不伪造授权。

#include <memory>

#include "PortalScreenCast.h"

namespace ecapture {
namespace portal {

std::unique_ptr<IPortalScreenCast> CreateDBusPortalScreenCast();

}  // namespace portal
}  // namespace ecapture
