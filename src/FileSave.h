#pragma once
// 截图落盘：先写同目录下的唯一临时文件，写完并刷新之后才提交成目标名。
//
// 旧做法是直接打开目标：光这一句就把旧文件清空了，写一半失败时又补一句删除目标，于是
// "写失败"与"目标本来没有文件"这两种情况都留下过现场被毁的后果。现在目标路径要么保持原样，
// 要么整体换成新内容，中间状态不会出现在目标名上。
//
// 两个平台的动作各自实现（platform/<平台>/*FileSave.cpp），保证的结论相同：
//   * 临时文件只在目标同目录、以独占创建（Windows CREATE_NEW / POSIX O_CREAT|O_EXCL）建立，
//     不覆盖任何既有文件，也不跟随一个预先摆好的符号链接；
//   * overwrite=false 的提交是"不许替换"的原子动作（Windows MoveFileEx 不带 REPLACE_EXISTING，
//     POSIX link(2)），目标已存在时既有文件一个字节都不动；
//   * overwrite=true 的提交是同一卷上的原子改名，替换的是目标那个名字本身，不跟随目标符号链接；
//   * 失败路径只清掉本次建立的临时文件；写入失败、磁盘已满、权限不足都按那一次调用交回的
//     系统错误如实报告，不换目录、不提权、不自动 chmod。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "core/Error.h"

namespace ecapture {

// 写入目标的一次**只读**预检。它只用来说明"如果接下来失败，最可能卡在哪一步"：
//   * 不创建、不删除、不试写、不改权限，也绝不据此跳过真正的写入；
//   * 真正的成败仍以 SaveFileAtomic 那一次调用交回的错误为准；
//   * 目录级的权限判断可能受 ACL、挂载选项与竞态影响，所以它只进诊断提示，不参与分支。
enum class WriteBlocker {
    kNone,
    kNotAbsolute,
    kParentMissing,
    kParentNotDirectory,
    kTargetDirectory,
    kTargetSymlink,   // 目标本身是一个符号链接 / 重解析点（覆盖会替换链接本身，不会写到链接目标）
    kDenied,          // 预检未能通过目录权限检查
    kUnknown,
};

struct WritePrecheck {
    WriteBlocker blocker = WriteBlocker::kNone;
    NativeErrorCode native = kNativeErrorNone;
};

WritePrecheck ProbeWriteTarget(const std::wstring& path);

// 原子写一个文件；path 必须是已展开、已绝对化的最终路径。
//
// * overwrite = true ：提交用"允许替换"的语义。替换失败（目标被别的进程占着、目标是目录、
//   目标是只读文件等）时旧文件原样留着，只清掉本次的临时文件。
// * overwrite = false：提交用"不许替换"的语义，目标在不在由那一次重命名原子决定
//   —— 不做"先查询再创建"那种有竞态的预检。已存在就报 io.file_exists。
//
// 清理只碰本次建立的临时文件。保证边界写在 .cpp 顶部。
//
// 写出去的就是 path 这个名字（path 必须是已绝对化、已展开过的最终路径），
// 所以调用方回显给 JSON 的那个字符串与磁盘上的条目同一次判定用的是同一个名字。
bool SaveFileAtomic(const std::wstring& path, const std::vector<uint8_t>& bytes, bool overwrite,
                    Diagnostic* err);

}  // namespace ecapture
