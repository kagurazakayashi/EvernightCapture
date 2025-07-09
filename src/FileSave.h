#pragma once
// 截图落盘：先写同目录下的唯一临时文件，写完并刷新之后才提交成目标名。
//
// 旧做法是直接 CREATE_ALWAYS 打开目标：光这一句就把旧文件清空了，写一半失败时又补一句
// DeleteFileW 把路径删掉，于是"写失败"与"目标本来没有文件"这两种情况都留下过现场被毁的后果。
// 现在目标路径要么保持原样，要么整体换成新内容，中间状态不会出现在目标名上。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"

namespace ecapture {

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
