#pragma once
// 平台服务：查询一个路径此刻是什么、以及建一层目录。
//
// 声明共用，实现分平台（platform/windows/WinHostDirectory.cpp 与
// platform/linux/PosixHostDirectory.cpp）。历史归档只用这一层问两件事：
//   * 归档根 / 日期目录此刻是**普通目录**、还是被一个文件占着、还是一个符号链接
//     （junction / symlink）。照着链接写下去等于把截图放进谁也没批准过的别处，所以要停下；
//   * 真去建目录那一次交回什么（没权限、父路径不存在、盘满各说各的）。
// 中间层级的符号链接由系统按普通路径解析规则跟随；本服务只对**最后一级**做类型判断。
// 这里不创建、不删除、不改权限、不提权，也不把"问不出来"当成"没有问题"。

#include <string>

#include "core/Error.h"

namespace ecapture {

enum class PathKind {
    kMissing,    // 这一级不存在（含中间某一级不是目录）
    kFile,       // 被一个普通文件占着
    kDirectory,  // 普通目录
    kSymlink,    // 符号链接 / junction / 重解析点
    kOther,      // 设备、管道、socket 或这一问本身没有答案
};

PathKind QueryPathKind(const std::wstring& path);

enum class DirectoryOutcome {
    kCreated,        // 本次建出来了
    kAlreadyExists,  // 已经有一个普通目录在那里（别的进程先建好了，不算失败）
    kBlockedFile,    // 名字被一个文件占着
    kBlockedSymlink, // 名字是一个符号链接 / junction / 重解析点
    kFailed,         // 其余失败（权限、盘满、路径不合法……），native 带系统原值
};

struct HostDirectoryResult {
    DirectoryOutcome outcome = DirectoryOutcome::kFailed;
    NativeErrorCode native = kNativeErrorNone;
};

// 建一层目录（不建中间层）。符号链接即使指向目录也报 kBlockedSymlink ——
// 调用方（历史归档）据此拒绝穿过一个没有被明确认可的链接。
HostDirectoryResult EnsureDirectory(const std::wstring& path);

}  // namespace ecapture
