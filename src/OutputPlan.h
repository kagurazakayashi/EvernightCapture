#pragma once
// 输出名规划：把 --out 的模板替整批目标展开成最终的绝对路径，并在取帧之前把碰撞查出来。
//
// 为什么单独有这一步：旧实现是"边截边算名字"，只要模板里含 % 就不追加序号，于是 %d、同进程的 %p、
// 同标题的 %n、%% 以及未知的 %x 都会让后一张悄悄盖掉前一张；而且非要到第二张才看得见。
// 现在一次算完整批：碰撞直接报错，既不静默改名，也不会先截了第一张才发现。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"

namespace ecapture {

// 一个待展开的目标。屏幕目标没有窗口可归属：hwnd / pid 给 0，name 给设备名。
struct OutputTarget {
    uint64_t hwnd = 0;
    uint32_t pid = 0;
    std::wstring name;  // 窗口标题或设备名，喂给 %n
};

// %n 的来源（窗口标题、设备名）不能直接当文件名用，这里统一过一遍：
// 非法字符换成 _、去掉尾部的点与空格、按 UTF-16 代理对边界截断、
// 整段撞上保留设备名（CON / COM1 / LPT1…）时加前缀。全被清空时返回 "_"。
std::wstring SafeFileNamePart(const std::wstring& raw);

// 一个编码格式对应的那个扩展名（带点）。整批输出名与历史归档名共用**这一份**映射：
// 归档文件的扩展名要说的是"这一张真被编成了什么容器"，所以它必须由编码实际所用的那个
// ImageFormat 算出来，而不是从主输出的名字里抠出来再猜一遍。
std::wstring ExtensionFor(ImageFormat fmt);

// Windows 路径比较（不区分大小写、Ordinal）。整批碰撞预检与"这一次要写的归档名是不是就是
// 主输出那一个"共用同一条判据，免得两处对"同一个文件"给出两种答案。
bool SameWindowsPath(const std::wstring& a, const std::wstring& b);

// 替整批目标算出最终的绝对输出路径，下标与 targets 一一对应（序号从 1 起，就是 %i 的值）。
// * 模板里没有 % 且目标多于一个 -> 沿用"在扩展名前追加 _序号"的约定；
// * 没有扩展名 -> 按 --format 补上，并只发一条 note.output_extension_appended；
// * 任意两个目标展开成同一个名字（不区分大小写）-> 报 io.output_collision，整批不作。
// --out - 不走这里（标准输出不是路径），调用方自己先判掉。
bool PlanOutputPaths(const Options& opt, const std::vector<OutputTarget>& targets,
                     std::vector<std::wstring>* paths, std::vector<Diagnostic>* notes,
                     Diagnostic* err);

}  // namespace ecapture
