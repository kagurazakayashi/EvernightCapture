#include "Delivery.h"

#include <string>
#include <type_traits>
#include <utility>

#include "Lang.h"

namespace ecapture {
namespace {

// 提交之后那一段入账只允许「用开工之前备好的容量 + 搬移不抛的对象」，这一条就是它的另一半凭据：
// 以后谁给 CapturedImage / Diagnostic 加了会在搬移时分配的东西，这里先编译不过，而不是等一次
// 真分配失败把已经落到磁盘上的那张图从报告里抹掉。
static_assert(std::is_nothrow_move_constructible_v<CapturedImage> &&
                  std::is_nothrow_move_constructible_v<Diagnostic>,
              "交付之后的入账需要不抛的搬移");

// 两条 io.timeout 里不依赖"提交之后才有读数"的那六件：稳定码、哪一路输出、写到哪个名字、哪个
// 目标、哪条通道、哪一段。这六件决定"这条记录说的是哪一张、下一步动哪一路"，与那两句人读的文字
// 无关，所以两条共用这一份；也正因为共用，它能在开工之前就填好并留在栈上。
void StampIoTimeout(Diagnostic* d, const wchar_t* stage, const DeliveryTarget& target) {
    d->code = codes::kIoTimeout;
    d->option = L"--out";
    d->value = target.file;
    d->target = target.tag;
    d->backend = target.backend;
    d->stage = stage;
}

// 输出阶段那两条 io.timeout 共用的骨架：BudgetSpent 已经写好"预算多少、实际用了多少"那段文字
// 与 --timeout-ms 那一对取值，这里改成输出阶段的口径 —— 调用方下一步要动的是输出那一头（换到
// 快一点的盘、先把读走标准输出的那一方准备好、加大预算再来一次），所以 option / value 说的是
// "哪一路输出"，不是"哪一条参数"。
// 这一条用在**开工之前**：此刻预算读数就是最终值，连文字一起一次填完。
// 与"已经交付完"那一条的区别只在 message。
Diagnostic IoTimeout(const Deadline& dl, const wchar_t* stage, const DeliveryTarget& target,
                     std::wstring message, const std::wstring& hint) {
    Diagnostic d = BudgetSpent(dl, codes::kIoTimeout, stage, target.backend.c_str());
    StampIoTimeout(&d, stage, target);
    if (!message.empty()) d.message = std::move(message);
    if (!hint.empty()) d.hint = hint;
    return d;
}

// 那一次交付调用自己交回来的原因（SaveFileAtomic 那条路上已经填了 code / message / win32）。
// 这里只补定位那三件，而且**不覆盖**它写过的东西：通道与调用点说的是同一件事时以调用点为准，
// 那一路没填的（backend / target）这里才有机会补上。
void StampIoError(Diagnostic* d, const DeliveryTarget& target, const wchar_t* stage) {
    if (!d) return;
    if (d->code.empty()) d->code = codes::kWriteFailed;
    if (d->target.empty()) d->target = target.tag;
    if (d->backend.empty()) d->backend = target.backend;
    if (d->stage.empty()) d->stage = stage;
}

}  // namespace

void DeliverImage(const DeliveryRun& run, const DeliveryTarget& target, PendingImage pending,
                  const std::vector<uint8_t>& encoded, CaptureOutcome* outcome,
                  DeliveryStep* step) {
    *step = DeliveryStep{};
    const bool toStdout = target.file == L"-";
    const wchar_t* stage = toStdout ? stages::kStdout : stages::kWrite;

    // ---- 1) 开工之前：预算已经用尽就不开始输出 ----
    // 帧与编码好的字节都在这里被丢掉，一个字节都不落地：已经花掉的那一笔预算不该再换来一次
    // 谁也等不起的写。这一条与"写了一半"是两件事，所以 images 里不许有它。
    if (run.dl && run.dl->Spent()) {
        outcome->errors.push_back(IoTimeout(*run.dl, stage, target, std::wstring(),
                                            std::wstring()));
        step->recorded = true;
        return;
    }

    // ---- 2) 开工之前：把这一张落地之后一定要用的记账资源一次备齐 ----
    // 那一次写与那一次发字节都没有可安全中断的等待点：接口一返回 true，磁盘上那个名字就已经换过
    // 了、对面就已经读走了全部字节，什么都收不回来。从那一刻到这一张入列之间任何一次分配都可能
    // 失败，而失败一次就少一条事实 —— 报告会说"一张都没落地"，图却正在磁盘上。所以容量与那条
    // 跨限记录的六件稳定字段全部排在输出之前备好。
    // 备这些自己就要分配：现在还没有任何东西收不回来，reserve 抛出去就是一次普通失败，由调用方
    // 那层记一条真实原因，而 images 里本来就不该有这一张 —— 两头仍然一致。
    // errors 备两条：一条交付这一段自己的结论（没交付的那次原因，或那条跨限），一条调用方那层。
    const size_t pendingNotes = pending.notes.size();
    outcome->images.reserve(outcome->images.size() + 1);
    outcome->notes.reserve(outcome->notes.size() + pendingNotes);
    outcome->errors.reserve(outcome->errors.size() + 2);
    // 只有真设了预算才可能"交付之后才看见跨限"（没设预算时 Spent() 恒为 false，这一条用不上）。
    Diagnostic lateTimeout;
    if (run.dl && run.dl->Enabled()) StampIoTimeout(&lateTimeout, stage, target);

    // ---- 3) 开工：这一段没有可安全中断的等待点，只能让它跑完 ----
    Diagnostic ioErr;
    uint64_t emitted = 0;
    DWORD gle = 0;
    bool delivered = false;
    if (toStdout) {
        delivered = run.sink->EmitBytes(encoded, &emitted, &gle);
        if (!delivered) {
            // 半段流与一个字节都没出去是两种现场：管道里留下了一半的图，调用方必须知道
            // 那条流已经脏了，而不是只看到"写标准输出失败"。两种都不算交付。
            ioErr.code = codes::kWriteFailed;
            ioErr.message = emitted > 0 ? Msgf(L"io.stdout_partial", emitted, encoded.size())
                                        : Msg(L"io.stdout_failed");
            ioErr.option = L"--out";
            ioErr.value = L"-";
            // "本次没给输出路径"只在它确实是下一步可执行的那一条上才说：给了 --out - 的人
            // 本来就选定了这条道，那里不补这句话。
            if (target.implicitStdout) ioErr.hint = Msg(L"cli.missing_output_hint");
            ioErr.win32 = gle;
        }
    } else {
        delivered = run.sink->SaveFile(target.file, encoded, target.overwrite, &ioErr);
    }

    // 那一次调用交回 true 就是交付事实本身，先把它写进调用方持有的 step，再做后面任何一个动作：
    // 入账那一段抛出东西时，调用方仍然分得出"这一张到底有没有交出去"，不必拿返回值猜。
    step->delivered = delivered;

    // 真实结束时刻：慢盘与被堵住的管道本来就是这一张耗时的一部分，所以 elapsedMs 量在
    // 那一次调用**返回之后**。写在返回之前会把最关键的那一段等待从报告里抹掉。
    // 超过 uint32 上限时饱和在天花板：宁可报一个"至少这么大"的数，也不绕回成一个小数字。
    const uint64_t nowMs = run.clock ? run.clock() : 0;
    const uint64_t elapsed =
        nowMs > pending.startedClockMs ? nowMs - pending.startedClockMs : 0;
    CapturedImage image = std::move(pending.image);
    image.elapsedMs = elapsed > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(elapsed);

    // ---- 4a) 没交付出去：只记那一次调用自己的原因 ----
    // 这里**不看**预算：一次已经失败的写，事后"看见"期限过了并不改变它为什么失败。
    if (!delivered) {
        StampIoError(&ioErr, target, stage);
        outcome->errors.push_back(std::move(ioErr));
        step->recorded = true;
        return;
    }

    // ---- 4b) 交付事实先入列：质量提示这时才有意义（说的是你手上这张图） ----
    // 到这里为止不会再分配：容量在第 2 段备足，入列只做不抛的搬移（见上面那条编译期断言）。
    image.bytes = encoded.size();
    for (Diagnostic& n : pending.notes) outcome->notes.push_back(std::move(n));
    outcome->images.push_back(std::move(image));
    step->recorded = true;

    // ---- 4c) 期限合规：另一件事，另记一条 ----
    // 文件已经在磁盘上 / 字节已经全部到达标准输出，这一条超时不许把它删掉，也不许让 captured
    // 少一。它要说的是"这一批没在预算内结束"，所以带齐是哪个目标、哪条通道、写到哪个名字，
    // 以及那一次交付实际交回去的字节数。
    if (run.dl && run.dl->Spent()) {
        // 先入列、再补那两句给人看的文字：认得出是谁的那六件（含写到哪个名字）用的是第 2 段
        // 备好的字符串，入列不分配；随后组 message / hint 才可能要内存。真组不起来时，这条
        // io.timeout 的机器字段仍然已经在 errors 里，期限这条事实不靠报告文字撑着。
        outcome->errors.push_back(std::move(lateTimeout));
        Diagnostic& d = outcome->errors.back();
        d.message = Msgf(L"io.timeout_delivered", encoded.size(), run.dl->TotalMs(),
                         run.dl->ElapsedMs());
        d.hint = Msg(L"io.timeout_delivered_hint");
    }
}

int OutcomeExitCode(const CaptureOutcome& outcome) {
    if (!outcome.images.empty()) {
        // 部分成功：图已经落地，错误照样要非零 —— 用截图失败码提示调用方去看 errors。
        // 这一条**包括**"交付完成之后预算才跨"：那张图真在磁盘上，不能为了拿到一个 8
        // 就把它说成没交付（8 留给一张都没落地的输出失败）。
        return outcome.errors.empty() ? EX_OK : EX_CAPTURE_FAILED;
    }
    const std::wstring& code =
        outcome.errors.empty() ? std::wstring() : outcome.errors.front().code;
    if (code == codes::kAccessDenied || code == codes::kConsentUnavailable ||
        code == codes::kConsentTimeout) {
        return EX_DENIED;
    }
    if (code == codes::kWriteFailed || code == codes::kFileExists || code == codes::kIoTimeout) {
        return EX_IO_FAILED;
    }
    return EX_CAPTURE_FAILED;
}

}  // namespace ecapture
