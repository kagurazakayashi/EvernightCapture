#pragma once
// 隔离执行用的内部消息格式（父进程 <-> 本工具自己的工作辅助进程）。
//
// 这份格式**不是**对外契约的一部分：它不在 --help 里、没有稳定的 JSON、外部程序不该依赖它，
// 版本对不上时直接拒收（见 Header 里的 protocolVersion）。所以它可以随实现演进，
// 但演进必须同时改父进程与辅助进程两边 —— 两者是同一个 exe，编进同一份代码里。
//
// 三条硬规矩：
//   1. 只传**已解析的任务**（HWND、等待毫秒、匹配条件里的字符串），不传命令行、不传 shell 文本、
//      不传路径模板、也不传任何"要写到哪里去"的信息。辅助进程既没有输出文件也没有确认框，
//      它只能把窗口自己的画面读进内存交回父进程。
//   2. 一切长度先校验再使用：报头声明的长度必须与实际读到的字节数一致，条数、单条字符串长度、
//      帧的宽高与行距都有上限。任何一项越界就是整条消息作废（截断、伪造、版本错都不能
//      变成"半张图"或"少几个目标"这种看起来成功的结果）。
//   3. 帧的像素上限按 Dib 的同一条规矩算（边长 ≤16384、总字节 ≤64MiB），
//      超了就报错而不是分配一个巨大的缓冲区。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CaptureCommon.h"
#include "CliOptions.h"
#include "WindowMatch.h"

namespace ecapture {
namespace worker {

inline constexpr uint32_t kMagic = 0x31434557u;   // "WEC1" 的字节序写法，只做一眼能认的标记
// 2：WindowInfo 多了枚举那一刻的进程创建时间（身份复核的基线，见 WindowIdentity.h）。
// 3：WindowInfo 多了三条跨进程问句各自的下场（映像路径 / 进程创建时间 / 窗口矩形）。
//    结构化窗口查询（--list / --inspect）要靠它们把"这一项问不出来"与"这一项是空的"分开写，
//    而这些问答发生在辅助进程里，父进程事后补问已经不是同一个时刻的答案了。
// 这份格式不是对外契约，父进程与辅助进程是同一个 exe，所以演进只要两边一起改 + 这里加一。
inline constexpr uint16_t kProtocolVersion = 3;
// 报头固定 24 字节：magic(4) + version(2) + kind(2) + payloadLen(4) + 保留(4) + nonce(8)。
// 保留那段必须写 0、读时校验是 0 —— 将来要加字段就先占在这里，两边对不上就是整条作废，
// 不允许"多出来的字节当没看见"。
inline constexpr uint32_t kHeaderSize = 24u;

// 一条任务最多能带多少字节（条件里的字符串总量）。一条应答最多能带多少字节（帧像素）。
inline constexpr uint32_t kMaxTaskPayload = 64u * 1024u;
inline constexpr uint32_t kMaxReplyPayload = 64u * 1024u * 1024u;
inline constexpr uint32_t kMaxFrameSide = 16384u;             // 与 Dib::Create 同一条上限
inline constexpr uint64_t kMaxFrameBytes = 64ull * 1024ull * 1024ull;
inline constexpr uint32_t kMaxListItems = 1024u;              // 每个条件组最多几个取值
inline constexpr uint32_t kMaxWindowEntries = 8192u;          // 回传的窗口条数上限
inline constexpr uint32_t kMaxStringUnits = 4096u;            // 单条字符串上限（UTF-16 码元）
// BlockedStatus 里已定义的最大编号：应答带的状态码超出它就说明两边不是同一份代码。
// 加新的 BlockedStatus 要同步这里（以及协议版本），不能靠"最后一个枚举值"隐式推 ——
// 编译器不会提醒，而漏改的后果是把畸形应答当成合法状态处理。
inline constexpr uint32_t kMaxWireStatus = 13u;
// 任务里的"泵消息等多久"的上限。子进程照这个值等，所以父进程（哪怕被改成坏的）
// 也没法用它把辅助进程按在原地不放。
inline constexpr uint32_t kMaxTaskWaitMs = 15000u;

// 任务种类。只有这三条，全都是"读某个窗口自己的画面"或"把窗口列一遍"，
// 没有任何一条会去读桌面像素 —— 桌面那几条路径留在父进程里，由授权判定器把关。
enum TaskKind : uint16_t {
    kTaskNone = 0,
    kTaskPrintWindow = 1,     // PrintWindow 到新建 DIB（hwnd）
    kTaskDwmThumbnail = 2,    // 屏幕外宿主窗口 + DWM 缩略图 + PrintWindow 读回（hwnd, waitMs）
    kTaskMatchWindows = 3,    // 枚举顶层窗口并按条件求值，含 --title-regex 的回溯匹配
};

// 失败原因是哪一步：直接用 CaptureCommon.h 的 BlockedStatus（它的数值就是线上格式）。

struct Header {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t kind = 0;
    uint32_t payloadLen = 0;
    uint64_t nonce = 0;
};

// 一条任务（父 -> 子）。kind 决定哪些字段有意义，其余字段照旧序列化，解码方按 kind 取用。
struct Task {
    uint16_t kind = kTaskNone;
    uint64_t hwnd = 0;
    uint32_t waitMs = 0;
    // kTaskMatchWindows 用：已经解析好的条件 + 按屏过滤的矩形
    MatchOptions match;
    std::vector<RECT> onScreens;
};

// 一条应答（子 -> 父）。
struct Reply {
    BlockedStatus status = BlockedStatus::kOk;
    DWORD win32 = 0;
    HRESULT hresult = S_OK;
    std::wstring detail;   // 只放 ASCII 细节（异常 what()），为空时整个字段不出现
    // 取帧类
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    std::vector<uint8_t> pixels;
    // 匹配类
    std::vector<WindowInfo> hits;
    std::vector<WindowInfo> iconic;
};

// ---------------------------------------------------------------------------
// 编解码：任何一处长度、条数、上限不合就返回 false，调用方整条消息作废。
// 不做"尽力而为的解析"，也不对越界数据做截断后继续用。
// ---------------------------------------------------------------------------
bool EncodeHeader(const Header& h, std::vector<uint8_t>* out);
bool DecodeHeader(const uint8_t* data, size_t size, Header* out);

bool EncodeTask(const Task& task, std::vector<uint8_t>* out);
bool DecodeTask(const uint8_t* data, size_t size, Task* out);

bool EncodeReply(const Reply& reply, std::vector<uint8_t>* out);
bool DecodeReply(const uint8_t* data, size_t size, Reply* out);

// 应答里那一条帧的字节数（不含报头）；校验帧形状用。
bool FrameBytes(const Reply& reply, uint64_t* out);

}  // namespace worker
}  // namespace ecapture
