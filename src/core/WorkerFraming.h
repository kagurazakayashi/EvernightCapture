#pragma once
// 跨平台核心：辅助进程消息的**外层帧**——报头、上限常量与不带平台语义的字节原语。
//
// 这里是"父进程 <-> 本工具自己的工作辅助进程"的内部格式里**真正跨平台的那一层**：
//   * 报头（magic / version / kind / payloadLen / 保留 / nonce）的字节布局；
//   * 与载荷语义无关的尺寸上限（任务/应答总量、帧边长与总字节、预算上限）；
//   * 小端字节原语（Put/Take 系列）——载荷编解码也用它，Windows 与 Linux 写同一份。
// 载荷本体（Task / Reply 里那些 RECT / DWORD / HRESULT 字段）仍然只在
// src/WorkerProtocol.h【平台层 · Windows】；过渡桥接的账记在 MEMORY.md，拆它是后续步骤。
//
// 与 WorkerProtocol.h 同一份规矩：
//   1. 这份格式**不是**对外契约的一部分：不在 --help 里、没有稳定的 JSON，外部程序不该依赖它；
//      版本对不上直接拒收。父进程与辅助进程编在同一份产物里，演进必须两端同改。
//   2. 一切长度先校验再使用：报头声明的长度必须与实际读到的字节数一致，任何一项越界
//      就是整条消息作废（截断、伪造、版本错都不能变成"半张图"或"少几个目标"）。
//   3. 帧的像素上限按既有 Dib 的同一规矩算（边长与总字节都有界），超了就报错，
//      不分配一个巨大的缓冲区。
//
// 时钟域（第 4 类期限的传递方式）：报头与载荷里**没有任何绝对时刻**——父进程交下去的
// budgetMs 是"还剩多少毫秒"的时长，两个进程各自在自己的单调时钟上数这段时长。
// 因此不存在"把父进程时钟的读数与子进程时钟的读数直接相减"这回事；
// 辅助进程执行段末尾的交回宽限（kDeliverGraceMs）抵消的正是"两套时钟的粒度差"，
// 换算本体在 core/HelperTiming.h。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../CliOptions.h"   // cli_limits::kMaxTimeoutMs：预算上限与公开期限上限同源

namespace ecapture {
namespace worker {

inline constexpr uint32_t kMagic = 0x31434557u;   // "WEC1" 的字节序写法，只做一眼能认的标记
// 2：WindowInfo 多了枚举那一刻的进程创建时间（身份复核的基线，见 WindowIdentity.h）。
// 3：WindowInfo 多了三条跨进程问句各自的下场（映像路径 / 进程创建时间 / 窗口矩形）。
//    结构化窗口查询（--list / --inspect）要靠它们把"这一项问不出来"与"这一项是空的"分开写，
//    而这些问答发生在辅助进程里，父进程事后补问已经不是同一个时刻的答案了。
// 4：任务里 waitMs 后面那个保留段改成 budgetMs —— 父进程把"这一笔交易我还肯等多久"随任务交下来
//    （见 WorkerTiming.h）。辅助进程自己的执行段期限从这里取，不再用一个和预算无关的固定秒数，
//    否则用户显式接受的 60 秒会被辅助进程自己的时钟在第 30 秒掐掉。
// 这份格式不是对外契约，父进程与辅助进程是同一个 exe，所以演进只要两边一起改 + 这里加一。
inline constexpr uint16_t kProtocolVersion = 4;
// 报头固定 24 字节：magic(4) + version(2) + kind(2) + payloadLen(4) + 保留(4) + nonce(8)。
// 保留那段必须写 0、读时校验是 0 —— 将来要加字段就先占在这里，两边对不上就是整条作废，
// 不允许"多出来的字节当没看见"。
inline constexpr uint32_t kHeaderSize = 24u;

// 一条任务最多能带多少字节（条件里的字符串总量）。一条应答最多能带多少字节（帧像素）。
inline constexpr uint32_t kMaxTaskPayload = 64u * 1024u;
inline constexpr uint32_t kMaxReplyPayload = 64u * 1024u * 1024u;
inline constexpr uint32_t kMaxFrameSide = 16384u;             // 与 Dib::Create 同一条上限
inline constexpr uint64_t kMaxFrameBytes = 64ull * 1024ull * 1024ull;
// 任务里"这一笔隔离交易还能活多久"的上限，就是 --timeout-ms 自己的上限（见 CliOptions.h）。
// 这条不是另立一套常量：它是公开选项能接受的最大值，父进程不可能交出比这更大的一笔预算，
// 而辅助进程拿它当"最晚也要自己退出"的那道兜底。0 不合法（那等于无限），所以区间是 [1, 上限]。
inline constexpr uint32_t kMaxWorkerBudgetMs = static_cast<uint32_t>(cli_limits::kMaxTimeoutMs);

struct Header {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t kind = 0;
    uint32_t payloadLen = 0;
    uint64_t nonce = 0;
};

// ---------------------------------------------------------------------------
// 报头编解码：任何一处不合就返回 false，调用方整条消息作废。
// 不做"尽力而为的解析"，也不对越界数据做截断后继续用。
// ---------------------------------------------------------------------------
bool EncodeHeader(const Header& h, std::vector<uint8_t>* out);
bool DecodeHeader(const uint8_t* data, size_t size, Header* out);

// ---------------------------------------------------------------------------
// 小端字节原语：一律显式小端，解码不需要"猜这台机器的对齐和字节序"，
// 离线判据也能直接按字节构造畸形消息。载荷编解码（各平台的 WorkerProtocol）用同一份。
// ---------------------------------------------------------------------------
void PutU16(std::vector<uint8_t>* b, uint16_t v);
void PutU32(std::vector<uint8_t>* b, uint32_t v);
void PutU64(std::vector<uint8_t>* b, uint64_t v);
void PutI32(std::vector<uint8_t>* b, int32_t v);
bool TakeU16(const uint8_t** p, size_t* left, uint16_t* out);
bool TakeU32(const uint8_t** p, size_t* left, uint32_t* out);
bool TakeU64(const uint8_t** p, size_t* left, uint64_t* out);
bool TakeI32(const uint8_t** p, size_t* left, int32_t* out);

}  // namespace worker
}  // namespace ecapture
