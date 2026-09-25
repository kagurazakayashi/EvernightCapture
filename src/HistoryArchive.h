#pragma once
// 截图历史归档：把**已经完成主交付**的那一张图，在主输出之外再交一份独立副本。
//
// 为什么单独有这一个组件：副本这件事属于"交付"，不属于"取帧"。哪条通道出的图、编成什么格式、
// 裁过没有、缩过没有，都已经在共享交付那一步（src/Delivery.h）汇成同一条出口了，所以归档只在
// 那一步之后挂一次，六条后端一条都不必知道历史的存在。Report 也只把这里算好的结果写出去，
// 不在渲染时判第二次。
//
// 四条规矩（与 Delivery.h 那一份分开记账的口径连起来读）：
//   1. **根目录跟着实际运行的那一个 ECAPTURE.EXE。** 由模块位置算出 <程序目录>\history\，
//      不读当前工作目录、不用用户给的主输出路径、不认源码树或某个默认安装位置。安装目录可以
//      自定义、开发版就躺在 build\ 里，这两种现场都只有"问模块自己"才答得对。
//   2. **副本用的是那一份已经编好的字节**：不重拍、不重编码、不去读主输出文件（它可能正被别的
//      程序改），也不建硬链接冒充两份。于是删掉或覆盖主输出不会动到历史里那一张。
//   3. **只在主交付完成之后开始。** 主图没落地就没有副本可读；写了一半的标准输出也不算交付。
//      反过来，主图已经收不回来而副本失败时，主交付的事实与 images 条目原样保留，退给调用方的
//      是"已交付 + 有错误"那一种部分成功，不是"什么都没写"。
//   4. **独占提交，从不覆盖。** 目录与文件名出自归档决策时取样的**同一次**本地时间（跨午夜不会
//      目录写今天、文件名写明天）；名字里带本进程标识与本次决策的序号，撞上已有名字就换一个再试，
//      最终提交走"不许替换"的原子改名（复用生产 FileSave 那一份），换不出来就如实报
//      history.file_exists。多个进程同时截图、系统时钟往回拨，都不该抹掉已经存下的历史。
//
// 这一层**不做**的事：不轮转、不按天数或容量删、不后台扫描、不上传；不改 ACL、不改进程完整性
// 标签、不提权，也不在写不下去时悄悄换个目录或塞进临时目录。历史是持续保留的截图数据，不是
// 可以随便重建的缓存，所以"清掉它"永远是用户自己的一次显式操作。
//
// 离线判据（tests\history_state.cpp）用下面那个显式根目录的构造注入固定时钟与固定抗冲突标识，
// 于是"同刻两判""时钟回拨""名字已被占""路径组件是个文件""重解析点"这些都排得出确定的现场；
// 被 judged 的仍然是本文件与生产 FileSave.cpp，不是测试里另抄的一份命名算式。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Capture.h"     // HistoryRecord（images[].history 那一份事实的本体）
#include "CliOptions.h"  // ImageFormat / Diagnostic

namespace ecapture {

// 归档那一次决策取样到的本地日期与时间。目录名与文件名都只出自这**一份**取值：
// 生产用 GetLocalTime（截图历史按人所在时区归档，和 --out 模板里 %d/%t 同一把尺）；
// 离线判据注入固定读数，才能把"跨午夜""时钟回拨"摆成可核对的数而不是等真到半夜。
struct HistoryInstant {
    int year = 0;
    int month = 0;     // 1..12
    int day = 0;       // 1..31
    int hour = 0;
    int minute = 0;
    int second = 0;
};

using HistoryClockFn = std::function<HistoryInstant()>;

// 生产那把时钟：GetLocalTime。
HistoryInstant LocalHistoryInstant();

// 归档根的解析结果。这一步**只解析、不创建、不试写**：一次只读查询（--capabilities、--list）
// 因此不会因为"想看看那里能不能写"就多出一个目录来，而"这一次究竟写不写得下去"留给真正的
// 提交那一步回答（这里任何一条都不预测落盘结果）。
struct HistoryRoot {
    std::wstring dir;       // <实际运行的程序所在目录>\history，已绝对化；说不通时可能仍留有值
    bool usable = false;    // 这一路能不能安全地往下走（false 时 failure 就是那一条原因）
    Diagnostic failure;     // 仅 usable=false：history.unavailable + ASCII 原因 token 在 message 末尾
};

// 从模块位置解析归档根。问不出程序在哪、那个位置被一个文件占着、或者是一个不可安全跟随的
// 重解析点（junction / 符号链接：照着它写就可能写进谁也没批准过的别处）时 usable=false。
// 其它问答不出来的情况（例如属性那一步本身失败）**不**在这里下结论，交给提交那一步如实报错。
HistoryRoot ResolveHistoryRoot();

// 交付层只按这个接口问归档：给它主输出的名字、实际编码所用的格式与那一份编好的字节，
// 拿回 images[].history 那一份事实。交付层不认识日期目录、命名规则与重解析点。
class HistoryArchiver {
public:
    virtual ~HistoryArchiver() = default;

    // true = 副本已独占提交（record->state == kSaved，record->file 就是磁盘上那个名字）。
    // false 时 record->code 是稳定的 history.* 码，detail 是这一次调用自己的原因（含 win32 原值；
    // 没开始写的那两种 detail 留在调用方手里，不进 errors）。
    // primaryPath 是主交付的那个名字（"-" = 标准输出）：只用来判"这一次要写的归档名是不是就是
    // 主输出本身"，除此之外不读它、也不假设它存在。
    virtual bool Archive(const std::wstring& primaryPath, ImageFormat format,
                         const std::vector<uint8_t>& bytes, HistoryRecord* record,
                         Diagnostic* detail) = 0;
};

// 生产实现：默认构造从模块位置解析根目录（解析失败不抛、不退出，只是之后每张图的归档都如实
// 记一条 history.unavailable，主交付不受影响）。
class HistoryArchive final : public HistoryArchiver {
public:
    HistoryArchive();

    // 离线判据用的构造：给定根目录、本地时钟、固定的抗冲突标识与起始序号，于是归档名可以被
    // 现场算出来（判据要的是"提前摆好一个同名文件"这种确凿现场）。生产调用点一律走上面那个。
    explicit HistoryArchive(HistoryRoot root, HistoryClockFn clock = nullptr,
                            std::wstring nonce = std::wstring(), uint64_t first_seq = 1);

    const HistoryRoot& root() const { return root_; }
    uint64_t next_sequence() const { return next_seq_; }

    bool Archive(const std::wstring& primaryPath, ImageFormat format,
                 const std::vector<uint8_t>& bytes, HistoryRecord* record,
                 Diagnostic* detail) override;

private:
    // 让目录与文件名同源于那一次取样，并把这一路的失败归类成稳定的码。
    // 两个都不是 const：序号是"本次决策"的一部分，取过就用掉一个。
    bool EnsureDateDirectory(const HistoryInstant& when, std::wstring* dir, Diagnostic* err);
    bool CommitCopy(const std::wstring& dir, const HistoryInstant& when, ImageFormat format,
                    const std::vector<uint8_t>& bytes, const std::wstring& primaryPath,
                    HistoryRecord* record, Diagnostic* detail);

    HistoryRoot root_;
    HistoryClockFn clock_;
    std::wstring nonce_;
    uint64_t next_seq_ = 1;
};

// 归档目录名（history\ 下面那一层）：本地日期 YYYY-MM-DD。命名判据只有这一份，
// 组件与离线判据读的都是它。
std::wstring HistoryDirectoryName(const HistoryInstant& when);

// 归档文件名（不含目录，含扩展名）：本地时间 + 本进程 PID + 抗冲突标识 + 本次决策的序号。
// 刻意不含窗口标题、设备名、用户名或任何完整路径 —— 历史文件的名字本身不该泄露截的是谁。
std::wstring HistoryFileName(const HistoryInstant& when, uint32_t pid, const std::wstring& nonce,
                             uint64_t seq, ImageFormat format);

}  // namespace ecapture
