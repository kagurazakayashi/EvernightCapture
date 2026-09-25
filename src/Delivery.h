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
// 离线判据（tests\delivery_state.cpp）注入受控的出口、假时钟与一次受控的分配失败，为的是把
// "提交之后预算才跨"与"提交之后记账时缺内存"这两瞬间安排得出来；判据链的是本文件的编排本体与
// 生产 FileSave.cpp，不是测试里抄的一份算式。

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
#include "HistoryArchive.h"   // 主交付之外那一份历史副本（接口在这，命名与提交在 HistoryArchive.h）

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

// 交付这一步要问的几件事：同一份预算（整批共用那一份，不许重新领）、哪一个出口、
// 用哪把时钟量 elapsedMs，以及（可选）历史归档那一位与本次实际编码所用的格式。
// history 为空 = 这一次没有归档这一步（离线判据里那些只判交付本体的用例就这么摆），
// 那时 images 条目里连 history 这个键都不出现；生产那一路一定给一个。
struct DeliveryRun {
    const Deadline* dl = nullptr;
    OutputSink* sink = nullptr;
    DeliveryClock clock;
    HistoryArchiver* history = nullptr;
    ImageFormat format = ImageFormat::kPng;
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
    // 这一格在出口接口返回 true 的那一刻就写好，早于本函数后面任何一个动作：step 由调用方
    // 持有，所以即使后面抛了东西、即使这一张的账没能记完，调用方仍然分得出"图到底交没交出去"。
    bool delivered = false;
    // 这一张的账已经在这里记完（images / notes / errors 至少一处），调用方不必再记第二次。
    // 交付成功时它随入列那一刻写 true：入列只用第 2 段预配好的容量与不抛的搬移，中间再没有别
    // 的分配点，所以"接口已返回 true 而这一格还是 false"不是一种普通故障现场，而是预配被改坏
    // 之后的缺陷 —— 真到那一步，调用方按"账没记完"处理，记的是那一次抛出本身的原因，不假装成功。
    bool recorded = false;
};

// 交付一张已编码的图，把结果记进 outcome，两段结论写进**调用方持有**的 step（不用返回值传：
// 抛异常时返回值拿不到，而那一刻最需要知道的恰恰是"有没有交出去、账记到哪儿"）。四段，其中
// 第 4 段再分 4a~4d，顺序不能换：
//   1. 开工之前预算已经用尽 —— 一个字节都不发，记一条 io.timeout（stage=write / stdout），
//      这一张不落地。
//   2. 开工之前把这一张落地之后**一定要用**的记账资源一次备齐：images 与 notes 的容量、
//      errors 的余量（交付这一段自己的结论、历史那一条、调用方那层，加"交付之后才看见跨限"
//      共四条），两条跨限记录里不依赖提交之后读数的稳定字段，以及历史那一份结论的码位。
//      备这些自己就要分配，所以排在输出之前：那时磁盘与管道上还没有任何东西收不回来，
//      一次分配失败就是一次普通失败，调用方那层记一条真实原因，而图确实没落地 —— 两头一致。
//   3. 真开始交付 —— 这一段没有可安全中断的等待点，只能让它跑完。那一次调用返回 true 之后
//      **先**写 step.delivered：那是磁盘与管道对面的外部事实，不是本函数的返回值。
//   4a. 没交付出去：只记那一次调用自己的原因，这里**不看**预算（一次已经失败的写，事后"看见"
//      期限过了并不改变它为什么失败）。
//   4b. 交付事实入列（images 条目 + bytes + **含提交这一次调用耗时**的 elapsedMs + 这一张的质量
//      提示）只用第 2 段备好的容量与不抛的搬移，于是已经落地的图不会因为一次扩容失败而从报告里
//      消失；历史那一份结论的默认码位也在这一步跟着这张图进去（成功时会被清掉）。
//   4c. 历史副本：拿**同一份编码缓冲**（不重拍、不重编码、不读主输出文件）往归档那一位提交一次，
//      结论写进刚刚入列的那一张图的 history 格。这一段排在期限核对**之前**，因为归档自己也可能
//      花掉时间；它失败或被跳过都只改这一格与一条 errors 记录，绝不动 images 里那张已经落地的图。
//      开工之前预算已经用尽那一种（主图刚落盘而期限才跨）记成 skipped + history.budget_spent：
//      归档不另领一份预算，也不留一个谁也等不起的后台写入。
//   4d. 期限合规：另一件事，另记一条 io.timeout —— 骨架先入列、那两句给人看的文字后补，
//      文字真组不起来时，那条记录里"哪个目标、哪条通道、哪一路输出、哪一段"照旧在账上。
// 退出码不在这里判（见 OutcomeExitCode）：一批里还有别的目标的账。
// 抛出来之后这里不做任何补救：不删已经写出的文件、不重发一遍、也不把已记过的账抹掉。
void DeliverImage(const DeliveryRun& run, const DeliveryTarget& target, PendingImage pending,
                  const std::vector<uint8_t>& encoded, CaptureOutcome* outcome,
                  DeliveryStep* step);

// 一批结果对应的退出码，只有一份：
//   * 有图就有交付事实，附带错误时按部分成功给截图失败码（含"交付完成之后预算才跨"：
//     那张图真在磁盘上，不许为了一个超时退出码把它说成没交付。8 留给"一张都没落地"）。
//   * 一张都没有时才按第一条错误分类：授权那几条 → 6，输出阶段的失败（io.write_failed /
//     io.file_exists / 没开工的 io.timeout）→ 8，其余 → 7。
int OutcomeExitCode(const CaptureOutcome& outcome);

}  // namespace ecapture
