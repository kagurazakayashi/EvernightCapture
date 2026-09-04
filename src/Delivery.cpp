#include "Delivery.h"

#include <string>
#include <utility>

#include "Lang.h"

namespace ecapture {
namespace {

// 输出阶段的两条 io.timeout 共用的那副骨架：BudgetSpent 已经写好"预算多少、实际用了多少"
// 那段文字与 --timeout-ms 那一对取值，这里改成输出阶段的口径 —— 调用方下一步要动的是
// 输出那一头（换到快一点的盘、先把读走标准输出的那一方准备好、加大预算再来一次），
// 所以 option / value 说的是"哪一路输出"，不是"哪一条参数"。
// 两条 io.timeout 的区别只在 message：一条是"还没开工就被拒"，另一条是"已经交付完"。
Diagnostic IoTimeout(const Deadline& dl, const wchar_t* stage, const DeliveryTarget& target,
                     std::wstring message, const std::wstring& hint) {
    Diagnostic d = BudgetSpent(dl, codes::kIoTimeout, stage, target.backend.c_str());
    d.option = L"--out";
    d.value = target.file;
    d.target = target.tag;
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

DeliveryStep DeliverImage(const DeliveryRun& run, const DeliveryTarget& target,
                          PendingImage pending, const std::vector<uint8_t>& encoded,
                          CaptureOutcome* outcome) {
    DeliveryStep step;
    const bool toStdout = target.file == L"-";
    const wchar_t* stage = toStdout ? stages::kStdout : stages::kWrite;

    // ---- 1) 开工之前：预算已经用尽就不开始输出 ----
    // 帧与编码好的字节都在这里被丢掉，一个字节都不落地：已经花掉的那一笔预算不该再换来一次
    // 谁也等不起的写。这一条与"写了一半"是两件事，所以 images 里不许有它。
    if (run.dl && run.dl->Spent()) {
        outcome->errors.push_back(IoTimeout(*run.dl, stage, target, std::wstring(),
                                            std::wstring()));
        step.recorded = true;
        return step;
    }

    // ---- 2) 开工：这一段没有可安全中断的等待点，只能让它跑完 ----
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

    // 真实结束时刻：慢盘与被堵住的管道本来就是这一张耗时的一部分，所以 elapsedMs 量在
    // 那一次调用**返回之后**。写在返回之前会把最关键的那一段等待从报告里抹掉。
    // 超过 uint32 上限时饱和在天花板：宁可报一个"至少这么大"的数，也不绕回成一个小数字。
    const uint64_t nowMs = run.clock ? run.clock() : 0;
    const uint64_t elapsed =
        nowMs > pending.startedClockMs ? nowMs - pending.startedClockMs : 0;
    CapturedImage image = std::move(pending.image);
    image.elapsedMs = elapsed > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(elapsed);

    // ---- 3a) 没交付出去：只记那一次调用自己的原因 ----
    // 这里**不看**预算：一次已经失败的写，事后"看见"期限过了并不改变它为什么失败。
    if (!delivered) {
        StampIoError(&ioErr, target, stage);
        outcome->errors.push_back(std::move(ioErr));
        step.recorded = true;
        return step;
    }

    // ---- 3b) 交付事实先入列：质量提示这时才有意义（说的是你手上这张图） ----
    image.bytes = encoded.size();
    for (Diagnostic& n : pending.notes) outcome->notes.push_back(std::move(n));
    outcome->images.push_back(std::move(image));
    step.delivered = true;
    step.recorded = true;

    // ---- 3c) 期限合规：另一件事，另记一条 ----
    // 文件已经在磁盘上 / 字节已经全部到达标准输出，这一条超时不许把它删掉，也不许让 captured
    // 少一。它要说的是"这一批没在预算内结束"，所以带齐是哪个目标、哪条通道、写到哪个名字，
    // 以及那一次交付实际交回去的字节数。
    if (run.dl && run.dl->Spent()) {
        Diagnostic d = IoTimeout(*run.dl, stage, target,
                                 Msgf(L"io.timeout_delivered", encoded.size(),
                                      run.dl->TotalMs(), run.dl->ElapsedMs()),
                                 Msg(L"io.timeout_delivered_hint"));
        outcome->errors.push_back(std::move(d));
    }
    return step;
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
