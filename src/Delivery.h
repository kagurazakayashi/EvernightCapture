#pragma once
// 交付那一步：把已经编码好的那一张图交出去（原子写文件 / 写标准输出），并且**只在这里**
// 决定这一张算不算落地、预算有没有守住、这些事实记在哪里。
//
// 为什么"交付结果"与"期限合规"是两件事，必须分开记：
//   * 磁盘写、写往被人堵住的管道，都没有可以安全中断的等待点 —— 期限对它们只能在
//     **开工之前**拒一次、**完工之后**核一次（见 Deadline.h 顶部那条边界）。
//   * 于是"图已经在磁盘上了"与"这一次运行没守住预算"会同时成立。前者是一条事实，
//     不会因为超时就被删掉，也不会从 JSON 里假装消失；后者是一条错误，不会因为
//     图交出去了就被藏起来。把任何一头折进另一头都是在说谎。
//   * 反过来，写失败就是写失败：原因以那一次调用交回来的为准，不许被随后"看见"的
//     超时覆盖成 io.timeout；而半段标准输出、失败的原子改名都不算交付，images 里不许有它们。
//
// 这里不碰取帧、不碰授权、不碰编码：进来的已经是编好的字节。发布版 ECAPTURE.EXE 里
// 没有"换一种交付结果"的开关 —— 出口是一个接口（OutputSink），生产用的是真文件与真标准输出，
// 离线判据（tests\delivery_state.cpp）注入受控的出口与假时钟，为的是把"提交之后预算才跨"
// 这一瞬间安排得出来；判据链的是本文件的编排本体与生产 FileSave.cpp，不是测试里抄的一份算式。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // DWORD（那一次写交回来的 Win32 错误码原值）

#include "Capture.h"
#include "Deadline.h"

namespace ecapture {

// 量 elapsedMs 用的单调毫秒（生产 = GetTickCount64，与整条流水线同一把尺）。离线判据注入假时钟，
// 才能把"提交这一步自己花了多久"与"预算在哪一刻跨过"摆成确定的数。
using DeliveryClock = std::function<uint64_t()>;

// 一张已编码图像的出口。两条路都没有可安全中断的等待点，所以调用方只能等它返回。
class OutputSink {
public:
    virtual ~OutputSink() = default;

    // 原子写文件：true = 已经以 path 这个名字提交（目标要么保持原样要么整体换成新内容）。
    // 失败时 err 带**这一次调用自己的**原因（io.write_failed / io.file_exists）与 win32 原值。
    virtual bool SaveFile(const std::wstring& path, const std::vector<uint8_t>& bytes,
                          bool overwrite, Diagnostic* err) = 0;

    // 图片字节写标准输出：true = 全部字节已达。emitted 回写实际发出的字节数 ——
    // "一个字节都没出去"与"半段流留在管道里"是两种现场，调用方要分得开。
    virtual bool EmitBytes(const std::vector<uint8_t>& bytes, uint64_t* emitted,
                           DWORD* ioError) = 0;
};

// 交付这一步要问的三件事：同一份预算（整批共用那一份，不许重新领）、哪一个出口、
// 用哪把时钟量 elapsedMs。
struct DeliveryRun {
    const Deadline* dl = nullptr;
    OutputSink* sink = nullptr;
    DeliveryClock clock;
};

// 交付的对象是谁、写到哪个名字。file 与实际写入的那个名字是同一个字符串。
struct DeliveryTarget {
    std::wstring file;          // 已展开、已绝对化的最终路径；"-" = 标准输出
    std::wstring tag;           // errors[].target：窗口给 0x…，屏幕给设备名
    std::wstring backend;       // 真正出图那条通道（auto 回退后与请求值不同）
    bool overwrite = true;      // --no-overwrite 时提交用"不许替换"的语义
    bool implicitStdout = false;   // 本次根本没写 --out（那条 hint 只在它成立时才补）
};

// 等着交付的那一张：元数据已经填好，bytes / elapsedMs 由交付这一步补。
struct PendingImage {
    CapturedImage image;
    // 质量提示（单色帧、丢区域、HDR 那两条、屏幕原点核实不出来）：这一张真交出去才送，
    // 没交出去就不提示 —— 提示的是"你手上那张图"，图没落地就没有那张图。
    std::vector<Diagnostic> notes;
    uint64_t startedClockMs = 0;   // 与 run.clock 同一把尺的起点（这个目标开工那一刻）
};

struct DeliveryStep {
    // 图真落地了（文件已提交 / 全部字节已达）。"落地之后预算才跨"仍然算落地。
    bool delivered = false;
    // 这一张的账已经在这里记完（images / notes / errors 至少一处），调用方不必再记第二次。
    // 交付那一段抛异常时它是 false：账一页没记，由调用方那层异常边界记一条真实原因。
    bool recorded = false;
};

// 交付一张已编码的图，并把结果记进 outcome。三段，顺序不能换：
//   1. 开工之前预算已经用尽 —— 一个字节都不发，记一条 io.timeout（stage=write / stdout），
//      这一张不落地。
//   2. 真开始交付 —— 这一段没有可安全中断的等待点，只能让它跑完。
//   3. 跑完之后分两头记：交付事实（images 条目 + bytes + **含提交这一次调用耗时**的
//      elapsedMs + 这一张的质量提示）与期限合规（完工之后预算已尽时另记一条 io.timeout，
//      带上是哪个目标、哪条通道、写到哪个名字）。
// 退出码不在这里判（见 OutcomeExitCode）：一批里还有别的目标的账。
// 抛出来的异常不在这里接：由调用方那层边界接住，那时这一张既没落地也没记账。
DeliveryStep DeliverImage(const DeliveryRun& run, const DeliveryTarget& target,
                          PendingImage pending, const std::vector<uint8_t>& encoded,
                          CaptureOutcome* outcome);

// 一批结果对应的退出码，只有一份：
//   * 有图就有交付事实，附带错误时按部分成功给截图失败码（含"交付完成之后预算才跨"：
//     那张图真在磁盘上，不许为了一个超时退出码把它说成没交付。8 留给"一张都没落地"）。
//   * 一张都没有时才按第一条错误分类：授权那几条 → 6，输出阶段的失败（io.write_failed /
//     io.file_exists / 没开工的 io.timeout）→ 8，其余 → 7。
int OutcomeExitCode(const CaptureOutcome& outcome);

}  // namespace ecapture
