// 隔离执行那份内部消息格式与执行期限的离线判据。
//
// 为什么要单独一个可执行文件：管道两端的代码只能被"真正的父进程 + 真正的辅助进程"跑到，
// 而格式这一层最要紧的判据恰恰是"畸形消息必须整条作废"——截断、超长、nonce 不对、
// 报头与内容互相打脸、像素字节数与行距不一致。这些用真进程去造既慢又不完整（有些字节
// 组合在真实调用里根本发不出去），所以直接把编解码拎出来判。
//
// 期限本体（Deadline）也放在这里判：它最要紧的语义是"后一步只能拿到剩下的预算，
// 不会重新领一份"，而这条语义用真机截图去验就要造一个能卡住的动作，既不稳也不必要。
// 发布版 ECAPTURE.EXE 里没有任何测试旁路：这个程序是另外编的一个测试可执行文件。
//
// 只用 C 风格的 printf 汇报，判据写在断言里；任何一条不过就返回非 0。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../src/Deadline.h"
#include "../src/Lang.h"
#include "../src/WorkerProtocol.h"

namespace {

using namespace ecapture;
using namespace ecapture::worker;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}

// 把一条合法任务序列化出来，作为"逐字节破坏"的基准
std::vector<uint8_t> SampleTaskPayload() {
    Task t;
    t.kind = kTaskMatchWindows;
    t.hwnd = 0x1234;
    t.waitMs = 700;
    t.match.titles = {L"报告", L"a title with \"quotes\" and D:\\shots\\rpt.png"};
    t.match.titleRegexes = {L"^(a+)+b"};
    t.match.classes = {L"Shell_TrayWnd"};
    t.onScreens = {RECT{-64000, -4000, 1920, 1080}};
    std::vector<uint8_t> out;
    Check(EncodeTask(t, &out), "样例任务能序列化");
    return out;
}

std::vector<uint8_t> FrameReplyPayload() {
    Reply r;
    r.status = BlockedStatus::kOk;
    r.width = 2;
    r.height = 2;
    r.stride = 8;
    r.pixels = std::vector<uint8_t>(16, 0xAB);
    std::vector<uint8_t> out;
    Check(EncodeReply(r, &out), "样例帧应答能序列化");
    return out;
}

// 报头 + 载荷合成一条完整消息，用于检查报头层
std::vector<uint8_t> Sealed(uint16_t kind, const std::vector<uint8_t>& payload, uint64_t nonce) {
    Header h{kMagic, kProtocolVersion, kind, static_cast<uint32_t>(payload.size()), nonce};
    std::vector<uint8_t> framed;
    EncodeHeader(h, &framed);
    framed.insert(framed.end(), payload.begin(), payload.end());
    return framed;
}

// ---------------------------------------------------------------------------
// 报头
// ---------------------------------------------------------------------------
void CheckHeader() {
    const std::vector<uint8_t> payload = {1, 2, 3};
    const std::vector<uint8_t> framed = Sealed(kTaskMatchWindows, payload, 0xDEADBEEFCAFEull);

    Check(framed.size() == kHeaderSize + payload.size(), "报头长度与 kHeaderSize 一致");

    Header h{};
    Check(DecodeHeader(framed.data(), framed.size(), &h), "报头能原样读回");
    Check(h.kind == kTaskMatchWindows && h.payloadLen == 3 && h.nonce == 0xDEADBEEFCAFEull,
          "报头里的 kind / 长度 / nonce 都对得上");

    std::vector<uint8_t> shortCopy = framed;
    shortCopy.resize(kHeaderSize - 1);
    Check(!DecodeHeader(shortCopy.data(), shortCopy.size(), &h), "少一个字节（截断）就拒收");

    shortCopy = framed;
    shortCopy[0] = static_cast<uint8_t>(shortCopy[0] ^ 0xFF);
    Check(!DecodeHeader(shortCopy.data(), shortCopy.size(), &h), "magic 被打花就拒收");

    shortCopy = framed;
    shortCopy[4] = static_cast<uint8_t>(shortCopy[4] + 1);   // version
    Check(!DecodeHeader(shortCopy.data(), shortCopy.size(), &h), "协议版本不一致就拒收");

    shortCopy = framed;
    shortCopy[14] = 1;   // 保留段的最低位（payloadLen 之后那 4 字节）
    Check(!DecodeHeader(shortCopy.data(), shortCopy.size(), &h), "保留段非 0 就拒收");

    Header evil{kMagic, kProtocolVersion, kTaskPrintWindow, kMaxReplyPayload + 1, 1};
    std::vector<uint8_t> evilFramed;
    Check(!EncodeHeader(evil, &evilFramed), "超出上限的声明长度在发送端就被拦下");
}

// ---------------------------------------------------------------------------
// 任务
// ---------------------------------------------------------------------------
void CheckTask() {
    const std::vector<uint8_t> payload = SampleTaskPayload();
    Task back;
    Check(DecodeTask(payload.data(), payload.size(), &back), "任务能原样读回");
    Check(back.kind == kTaskMatchWindows && back.hwnd == 0x1234 && back.waitMs == 700,
          "任务里的标量字段一致");
    Check(back.match.titleRegexes.size() == 1 && back.match.titleRegexes[0] == L"^(a+)+b",
          "正则原样送达（不改动语法）");
    Check(back.match.titles.size() == 2 && back.match.titles[0] == L"报告" &&
              back.match.titles[1].find(L"\"quotes\"") != std::wstring::npos &&
              back.match.titles[1].find(L"D:\\shots\\rpt.png") != std::wstring::npos,
          "带引号与反斜杠的取值原样送达");
    Check(back.onScreens.size() == 1 && back.onScreens[0].left == -64000,
          "屏幕矩形原样送达");

    // 截断：少最后 4 个字节
    std::vector<uint8_t> cut = payload;
    cut.resize(cut.size() - 4);
    Check(!DecodeTask(cut.data(), cut.size(), &back), "被截断的任务整条作废");

    // 长度不符：报头声明得比实际多
    Header over{kMagic, kProtocolVersion, kTaskMatchWindows,
                static_cast<uint32_t>(payload.size() + 1), 7};
    std::vector<uint8_t> overFramed;
    Check(EncodeHeader(over, &overFramed), "超长声明能编出来（由接收端判破）");
    Task ignored;
    std::vector<uint8_t> shortPayload = payload;
    shortPayload.pop_back();
    Check(!DecodeTask(shortPayload.data(), shortPayload.size(), &ignored),
          "内容与声明长度不一致时不接受'少一个字节也算读过'");
    (void)overFramed;

    // 未知任务种类
    Task badKind;
    badKind.kind = 99;
    std::vector<uint8_t> blob;
    Check(!EncodeTask(badKind, &blob) || !DecodeTask(blob.data(), blob.size(), &ignored),
          "没登记过的任务种类读不回来");

    // waitMs 过大：不许用"泵消息等待"把辅助进程按在原地
    Task slow;
    slow.kind = kTaskDwmThumbnail;
    slow.waitMs = kMaxTaskWaitMs + 1;
    std::vector<uint8_t> slowBlob;
    Check(EncodeTask(slow, &slowBlob) && !DecodeTask(slowBlob.data(), slowBlob.size(), &ignored),
          "超出上限的等待时长在解码时被拒");

    // 空矩形不是合法屏幕
    Task emptyScreen;
    emptyScreen.kind = kTaskMatchWindows;
    emptyScreen.onScreens = {RECT{0, 0, 0, 0}};
    std::vector<uint8_t> esBlob;
    Check(EncodeTask(emptyScreen, &esBlob) && !DecodeTask(esBlob.data(), esBlob.size(), &ignored),
          "零面积屏幕矩形被拒");

    // 条数超限
    Task many;
    many.kind = kTaskMatchWindows;
    for (uint32_t i = 0; i < kMaxListItems + 1; ++i) many.match.titles.push_back(L"x");
    std::vector<uint8_t> manyBlob;
    const bool encoded = EncodeTask(many, &manyBlob);
    Check(!encoded || !DecodeTask(manyBlob.data(), manyBlob.size(), &ignored),
          "条件条数超出上限时不会被接受");
}

// ---------------------------------------------------------------------------
// 应答
// ---------------------------------------------------------------------------
void CheckReply() {
    const std::vector<uint8_t> payload = FrameReplyPayload();
    Reply back;
    Check(DecodeReply(payload.data(), payload.size(), &back), "帧应答能原样读回");
    Check(back.width == 2 && back.height == 2 && back.stride == 8 && back.pixels.size() == 16,
          "帧的宽高、行距与像素条数一致");
    Check(back.pixels.size() == static_cast<size_t>(back.stride) * back.height,
          "像素字节数与行距 x 行数自洽（截断帧不会伪装成一张图）");

    std::vector<uint8_t> cut = payload;
    cut.resize(cut.size() - 1);
    Check(!DecodeReply(cut.data(), cut.size(), &back), "被截断一行的帧整条作废");

    // 形状不合法：行距小于宽 x 4
    Reply thin;
    thin.width = 4;
    thin.height = 2;
    thin.stride = 4;
    thin.pixels = std::vector<uint8_t>(8, 0);
    std::vector<uint8_t> thinBlob;
    Check(!EncodeReply(thin, &thinBlob) || !DecodeReply(thinBlob.data(), thinBlob.size(), &back),
          "行距小于宽度 x 4 的帧被拒");

    // 边长超限
    Reply huge;
    huge.width = kMaxFrameSide + 1;
    huge.height = 1;
    huge.stride = (kMaxFrameSide + 1) * 4;
    std::vector<uint8_t> hugeBlob;
    Check(!EncodeReply(huge, &hugeBlob) || !DecodeReply(hugeBlob.data(), hugeBlob.size(), &back),
          "边长超出 Dib 上限的帧不会被接受");

    // 状态码越界（协议版本不同 / 伪造）
    std::vector<uint8_t> bogus = payload;
    bogus[0] = 200;   // status 的最低字节
    Check(!DecodeReply(bogus.data(), bogus.size(), &back), "没登记过的状态码整条作废");

    // 失败应答不许带着像素
    Reply failWithFrame;
    failWithFrame.status = BlockedStatus::kPrintWindowFailed;
    failWithFrame.width = 1;
    failWithFrame.height = 1;
    failWithFrame.stride = 4;
    failWithFrame.pixels = std::vector<uint8_t>(4, 9);
    std::vector<uint8_t> failBlob;
    const bool encoded = EncodeReply(failWithFrame, &failBlob);
    Check(!encoded || !DecodeReply(failBlob.data(), failBlob.size(), &back),
          "标记为失败的应答里不会捎着一张图");

    // 窗口列表往返
    Reply list;
    list.status = BlockedStatus::kOk;
    WindowInfo w;
    w.hwnd = 0x001A0B4C;
    w.pid = 31468;
    w.title = L"标题 with D:\\path";
    w.className = L"CabinetWClass";
    w.imageName = L"explorer.exe";
    w.x = -8; w.y = 0; w.width = 1936; w.height = 1048; w.zOrder = 3; w.iconic = true;
    // 三条跨进程问句各自的下场：结构化窗口查询要靠它们区分"问不出来"与"是空的"，
    // 所以辅助进程问出来的答案必须逐项原样抵达父进程，不能被折成空值或默认成"能读"。
    w.pathRead = ecapture::ReadState::kDenied;
    w.pathWin32 = 5;               // ERROR_ACCESS_DENIED
    w.startRead = ecapture::ReadState::kReadable;
    w.processStartTicks = 0x0123456789ABCDEFull;
    w.rectRead = ecapture::ReadState::kFailed;
    w.rectWin32 = 1400;
    list.hits.push_back(w);
    std::vector<uint8_t> listBlob;
    Check(EncodeReply(list, &listBlob) && DecodeReply(listBlob.data(), listBlob.size(), &back),
          "命中窗口列表能往返");
    Check(back.hits.size() == 1 && back.hits[0].hwnd == 0x001A0B4C && back.hits[0].iconic &&
              back.hits[0].title == L"标题 with D:\\path",
          "列表里的句柄、标志与标题逐项一致");
    Check(back.hits.size() == 1 &&
              back.hits[0].pathRead == ecapture::ReadState::kDenied &&
              back.hits[0].pathWin32 == 5 &&
              back.hits[0].startRead == ecapture::ReadState::kReadable &&
              back.hits[0].processStartTicks == 0x0123456789ABCDEFull &&
              back.hits[0].rectRead == ecapture::ReadState::kFailed &&
              back.hits[0].rectWin32 == 1400,
          "三条问句的下场与原因码逐项原样抵达父进程");

    // 没登记过的"问句下场"整条作废：两边不是同一份代码时不能把未知值当成"能读"往下走
    Reply bogusState;
    bogusState.hits = back.hits;
    bogusState.hits[0].pathRead = static_cast<ecapture::ReadState>(99);
    std::vector<uint8_t> bogusBlob;
    Check(!EncodeReply(bogusState, &bogusBlob) ||
              !DecodeReply(bogusBlob.data(), bogusBlob.size(), &back),
          "没登记过的问句下场不会被接受");

    // HWND 为 0 的条目不合法：解码端就挡掉，不给调用方 reinterpret_cast 的机会
    std::vector<uint8_t> zeroCopy = listBlob;
    zeroCopy[kMaxStringUnits * 4] = 0;   // 位置猜不准就别改这条，见下面显式构造
    (void)zeroCopy;
    Reply zero;
    zero.hits = back.hits;
    zero.hits[0].hwnd = 0;
    std::vector<uint8_t> zeroBlob;
    Check(!EncodeReply(zero, &zeroBlob) || !DecodeReply(zeroBlob.data(), zeroBlob.size(), &back),
          "句柄为 0 的窗口条目不会被接受");
}

// ---------------------------------------------------------------------------
// 执行期限：一条预算被各步共用，后一步只能拿到剩下的
// ---------------------------------------------------------------------------
void CheckDeadline() {
    using ecapture::Deadline;
    using ecapture::kIsolatedCallMs;

    // 不给预算 = 不限：ClampWait 原样放行，等待换成 INFINITE，而且永远不会"已用尽"
    const Deadline free = Deadline::FromTotalMs(0);
    Check(!free.Enabled(), "0 表示不设总预算");
    Check(!free.Spent(), "不设预算时不存在'已用尽'");
    Check(free.RemainingMs() == Deadline::kNoLimit, "不设预算时剩余是'无限'而不是 0");
    Check(free.ClampWait(2000) == 2000, "不设预算时不压缩任何等待");
    Check(ecapture::WaitTimeout(free) == INFINITE, "不设预算时超时参数换成 INFINITE");
    // 换算按 QPC 取整，读出来只会比名义值小一两个毫秒，所以判"落在这一秒之内"而不是相等
    const DWORD cap = ecapture::WaitTimeout(Deadline::FromTotalMs(kIsolatedCallMs));
    Check(cap <= kIsolatedCallMs && cap + 100 >= kIsolatedCallMs,
          "内置上限真的被用作等待时长");

    // 设了预算：先能看到剩余，消耗之后必须变小
    const Deadline dl = Deadline::FromTotalMs(400);
    Check(dl.Enabled() && dl.TotalMs() == 400, "总预算按毫秒原样记住");
    const uint64_t before = dl.RemainingMs();
    Check(before > 0 && before <= 400, "剩余预算落在 (0, 总预算] 之内");
    Sleep(120);
    const uint64_t after = dl.RemainingMs();
    Check(after < before, "时间往前走，剩余预算必须变小（而不是每步重新领一份）");
    Check(dl.ClampWait(2000) <= after && dl.ClampWait(2000) > 0,
          "把 2000 ms 的等待压进剩余预算：既不许超过剩余，也不许直接归零");
    Check(!dl.Spent(), "预算还没用尽时不报耗尽");
    Check(dl.ElapsedMs() >= 100, "实际用时按单调时钟计出来了");

    // 用尽之后：任何一步都拿不到等待时间，ClampWait 归零
    const Deadline spent = Deadline::FromTotalMs(30);
    Sleep(120);
    Check(spent.Spent(), "超过总预算之后判为已用尽");
    Check(spent.RemainingMs() == 0, "用尽之后剩余是 0");
    Check(spent.ClampWait(2000) == 0, "用尽之后不再给任何一步等待时间");
    Check(ecapture::WaitTimeout(spent) == 0, "用尽之后等待上限是 0（不是 INFINITE）");

    // 期限耗尽那条诊断：code / stage / option / value 都要齐，调用方才不必去抠 message
    const ecapture::Diagnostic d =
        ecapture::BudgetSpent(spent, ecapture::codes::kCaptureTimeout,
                              ecapture::stages::kCapture, L"printwindow");
    Check(d.code == ecapture::codes::kCaptureTimeout && d.stage == ecapture::stages::kCapture,
          "期限诊断带稳定码与阶段名");
    Check(d.option == L"--timeout-ms" && d.value == L"30" && d.backend == L"printwindow",
          "期限诊断指着 --timeout-ms 与实际预算");
    Check(!d.message.empty() && d.message[0] != L'?', "诊断文案取到了（不是 ?key 那种占位）");

    // 阶段名与码都集中在 CliOptions.h：这里钉住"只增不改名"的那几条新值
    Check(ecapture::stages::kMatch == std::wstring(L"match"), "新增的阶段名 match");
    Check(ecapture::codes::kMatchTimeout == std::wstring(L"match.timeout"), "新增的诊断码 match.timeout");
    Check(ecapture::codes::kCaptureTimeout == std::wstring(L"capture.timeout"), "新增码 capture.timeout");
    Check(ecapture::codes::kIoTimeout == std::wstring(L"io.timeout"), "新增码 io.timeout");
    Check(ecapture::codes::kWorkerFailed == std::wstring(L"capture.worker_failed"),
          "辅助进程自身故障的码是 capture.worker_failed（与通道截不到分开）");
    Check(ecapture::codes::kConsentTimeout == std::wstring(L"capture.consent_timeout"),
          "确认超时的码是 capture.consent_timeout（与'人答否'同为拒绝但可分开分支）");
}

// ---------------------------------------------------------------------------
// 自动预算的暂停（F04）：人工确认等待不占预算，暂停也不许回填预算。
// 全部用假时钟踩精确毫秒数；生产 QPC 时钟另给一条带容差的实测判据。
// ---------------------------------------------------------------------------
struct FakeClock {
    int64_t ms = 0;
};

void CheckDeadlinePause() {
    using ecapture::Deadline;
    FakeClock c;
    const auto now = [&c]() { return c.ms; };

    // 文档里的数值场景：总预算 5000；先自动处理 1000，人工等待 20000 加关闭缓冲 1000，
    // 恢复后仍剩 4000；再自动处理 500 之后剩 3500
    c.ms = 0;
    {
        const Deadline dl = Deadline::FromTotalMs(5000, now);
        c.ms += 1000;
        Check(dl.RemainingMs() == 4000, "假时钟：自动处理 1000 之后剩 4000");
        {
            const auto pause = dl.PauseForHumanWait();
            c.ms += 20000 + 1000;   // 真人等待 + 同意后约 1 秒关闭动画缓冲
            Check(dl.RemainingMs() == 4000, "暂停在世：人工等待一分不烧自动预算");
            Check(dl.ElapsedMs() == 1000, "暂停在世：流逝读数只算自动处理");
            Check(!dl.Spent(), "暂停在世：预算没被人等没");
        }
        Check(dl.RemainingMs() == 4000, "恢复后：暂停区间被原样跳过（既不烧也不回填）");
        c.ms += 500;
        Check(dl.RemainingMs() == 3500, "恢复后的自动处理继续烧同一份预算");
        Check(dl.ElapsedMs() == 1500, "总流逝 = 自动处理之和，与人工等待的 21000 无关");
    }

    // 嵌套暂停：只由最外层进出账，内层重复进入不重复扣
    c.ms = 0;
    {
        const Deadline dl = Deadline::FromTotalMs(5000, now);
        c.ms += 1000;
        {
            const auto outer = dl.PauseForHumanWait();
            c.ms += 500;
            {
                const auto inner = dl.PauseForHumanWait();
                c.ms += 3000;
                Check(dl.RemainingMs() == 4000, "嵌套暂停内层：剩余照样冻结");
            }
            c.ms += 500;
            Check(dl.RemainingMs() == 4000, "嵌套暂停内层退出、外层仍在：冻结不中断");
        }
        Check(dl.RemainingMs() == 4000, "嵌套暂停合计 4000，只扣一次（不重复扣除）");
        c.ms += 200;
        Check(dl.RemainingMs() == 3800, "最外层恢复后计时照常推进");
    }

    // 异常退栈：暂停作用域靠析构恢复，不存在"忘了 resume"的分支
    c.ms = 0;
    {
        const Deadline dl = Deadline::FromTotalMs(5000, now);
        bool caught = false;
        try {
            const auto pause = dl.PauseForHumanWait();
            c.ms += 21000;
            throw 1;
        } catch (...) {
            caught = true;
        }
        Check(caught, "判据自己确实从暂停里抛了出去");
        Check(dl.RemainingMs() == 5000, "暂停中异常退出：作用域照常恢复，人工段不扣预算");
        c.ms += 1000;
        Check(dl.RemainingMs() == 4000, "异常退栈后同一份预算继续正常计时");
    }

    // 复制件（查询闭包 / 隔离调用拿到的那份）与原件共享状态：暂停互相看得见
    c.ms = 0;
    {
        const Deadline dl = Deadline::FromTotalMs(5000, now);
        const Deadline copy = dl;   // 值复制再带进闭包的那一份
        c.ms += 800;
        {
            const auto pause = copy.PauseForHumanWait();   // 由复制件发起暂停
            c.ms += 12000;
            Check(dl.RemainingMs() == 4200, "复制件发起的暂停对原件同样生效（没有独立盲副本）");
        }
        c.ms += 200;
        Check(dl.RemainingMs() == 4000 && copy.RemainingMs() == 4000,
              "原件与复制件烧的是同一份预算，读数一致");
    }

    // 弹框之前就已耗尽的预算，绝不借一次确认复活
    c.ms = 0;
    {
        const Deadline dl = Deadline::FromTotalMs(1000, now);
        c.ms += 1500;
        Check(dl.Spent() && dl.RemainingMs() == 0 && dl.ClampWait(2000) == 0,
              "耗尽：任何一步都拿不到等待时间");
        {
            const auto pause = dl.PauseForHumanWait();
            c.ms += 30000;
            Check(dl.RemainingMs() == 0, "耗尽之后进入暂停：剩余照样是 0");
        }
        Check(dl.Spent() && ecapture::WaitTimeout(dl) == 0,
              "已耗尽的自动预算不借人工确认复活（恢复后仍是 0，不是 INFINITE）");
    }

    // 0 = 不设总预算：暂停是惰性对象，"不限"语义原样保持
    c.ms = 0;
    {
        const Deadline free = Deadline::FromTotalMs(0, now);
        c.ms += 999999;
        Check(!free.Enabled() && free.RemainingMs() == Deadline::kNoLimit,
              "0 预算：不设限，也不存在已用尽");
        {
            const auto pause = free.PauseForHumanWait();
            c.ms += 999999;
            Check(free.RemainingMs() == Deadline::kNoLimit && !free.Spent(),
                  "0 预算上的暂停是惰性的：不冻结任何东西也不报错");
        }
        Check(free.RemainingMs() == Deadline::kNoLimit, "惰性暂停退出后语义不变");
    }

    // 整批一份预算：目标 A 的确认不刷新预算，目标 B 也不能借第二次确认重新领一份
    c.ms = 0;
    {
        const Deadline dl = Deadline::FromTotalMs(5000, now);
        c.ms += 300;                       // 目标 A：匹配
        c.ms += 200;                       // 目标 A：等帧
        {
            const auto pause = dl.PauseForHumanWait();   // 整批第一次问人（含缓冲）
            c.ms += 20000 + 1000;
        }
        c.ms += 400;                       // 目标 A：通道回退再来一趟
        Check(dl.RemainingMs() == 4100, "回退与确认之后仍是同一份预算（剩余 = 5000 - 900）");
        c.ms += 4100;                      // 目标 B：取帧 + 编码 + 写盘，烧穿预算
        Check(dl.Spent(), "目标 B 只能花剩下的：整批没有第二份预算");
        {
            const auto pause = dl.PauseForHumanWait();   // 授权失效后的重新确认
            c.ms += 10000;
        }
        Check(dl.Spent() && dl.RemainingMs() == 0,
              "第二个人工确认之后预算照样是 0：暂停不重置整批预算");
    }

    // 假时钟算式必须也成立于生产 QPC 时钟：暂停段用真实 Sleep，判"基本不烧"（±30 ms 容差）
    {
        const Deadline dl = Deadline::FromTotalMs(1000);
        Sleep(60);
        const uint64_t before = dl.RemainingMs();
        {
            const auto pause = dl.PauseForHumanWait();
            Sleep(200);
            Check(dl.RemainingMs() + 30 >= before, "生产时钟：暂停在世剩余不被 200 ms 的 Sleep 烧掉");
        }
        const uint64_t after = dl.RemainingMs();
        Check(after + 30 >= before && after <= before,
              "生产时钟：恢复后预算与暂停前一致（QPC 粒度容差 30 ms）");
    }
}

}  // namespace

// worker_io_state.cpp 提供：异步管道 I/O 生命周期的注入式判据（回报本层检查数与失败数），
// 以及判"进程正常退出时未决资源不会被析构"那个子进程现场的下场（--io-exit 模式）。
// worker_connect_state.cpp 提供：Worker 连接时序的真命名管道判据（F03：抢先连接不得误报成
// 超时），以及它判据五那个"外部客户端"傀儡进程的下场（--f03-peer 模式）。
namespace ecapture {
int RunWorkerIoStateChecks(int* checksOut, int* failuresOut);
int RunWorkerIoExitPeerMode(const wchar_t* evidencePath);
int RunWorkerConnectStateChecks(int* checksOut, int* failuresOut);
int RunWorkerConnectPeerMode(const wchar_t* pipeName);
}

#include <cwchar>

// 傀儡模式要拿宽字符 argv（管道名与证据文件路径按原样送达），所以入口用 wmain 而不是 main。
int wmain(int argc, wchar_t* argv[]) {
    // 判据五的傀儡模式：不起判据、不打印，只连管、握手、等测试方收尾（见 worker_connect_state.cpp）。
    if (argc == 3 && std::wcscmp(argv[1], L"--f03-peer") == 0) {
        return ecapture::RunWorkerConnectPeerMode(argv[2]);
    }
    // WorkerIo 退出次序判据的子进程：造好未决现场后正常退出，把收尾时的观察值写进证据文件。
    if (argc == 3 && std::wcscmp(argv[1], L"--io-exit") == 0) {
        return ecapture::RunWorkerIoExitPeerMode(argv[2]);
    }

    std::printf("隔离执行消息格式（WorkerProtocol）与执行期限（Deadline）离线判据\n");
    CheckHeader();
    CheckTask();
    CheckReply();
    CheckDeadline();
    CheckDeadlinePause();
    // 异步管道 I/O 生命周期（WorkerIo + 注入假后端）判据在 worker_io_state.cpp，
    // 连接时序（真命名管道）判据在 worker_connect_state.cpp；三层计数并进来打同一行摘要，
    // 真机脚本按最后一行核对总数。
    int ioChecks = 0, ioFails = 0;
    const int ioFailures = ecapture::RunWorkerIoStateChecks(&ioChecks, &ioFails);
    int connChecks = 0, connFails = 0;
    const int connFailures = ecapture::RunWorkerConnectStateChecks(&connChecks, &connFails);
    std::printf("共 %d 项检查，失败 %d\n", g_checks + ioChecks + connChecks, g_failures + ioFails + connFailures);
    return (g_failures || ioFailures || connFailures) ? 1 : 0;
}
