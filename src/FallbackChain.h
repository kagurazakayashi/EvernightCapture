#pragma once
// EvernightCapture - `--capture auto` 那条回退链本体（连同"这一关换后端有没有意义"那一条判据）
//
// 为什么把这一层单独提成头文件：它是整条链里唯一"会替用户换一条取图路线"的地方，而它对
// 授权、身份、色彩策略这三关的处理**必须**与那些关各自的判据同源，又不能只写在某个 .cpp 的
// 匿名命名空间里 —— 那样离线判据就摸不到它，只能靠"真有一台 HDR 显示器""真让人在确认框上答否"
// 这种本机造不出来的现场去验。写成头文件里的模板之后，tests\hdr_state.cpp 那一份就能注入
// 假后端（每条通道返回什么、抛不抛异常、有没有被调用过全部由测试决定），逐条判这几件事：
//   * 策略结论出来之后**其余后端一次都不调用**、一张图都不交出、原码原样留着；
//   * 普通后端失败照旧往下试，而"往下试"只在链里还剩合格候选时发生（候选是闸门筛好的）；
//   * 最后一条也没成时包成 capture.failed，并把真实试过的那几条与最后那条原因一起交出去。
//
// 三条规矩（与 src/Consent.h、src/WindowIdentity.h、src/HdrColor.h 各自的判据接得上）：
//
// 1. **被拒绝与被判定不合格，不是"换个后端再试一次"的理由。** 见下面 ClassifyChainStop：
//    授权那一条（访问被拒 / 弹不出框 / 到点没人答 / 批准后目标又变了）与身份那一条（窗口目标
//    没了 / 换了样子 / 问不出身份；屏幕目标那块屏拔掉了 / 问不出身份）换一条通道同样不该给，而
//    "再试一次"在那里意味着用另一条通道去截一个没被人批准过的新对象。屏那一侧的两条尤其不能当
//    "这条通道不行"：它们说的是**那块屏此刻是谁**，换一条通道只会拿同一份当下枚举再问一次同一个
//    问题，答案不会变，而重问一遍确认框、或者顺手截了另一块屏都不是这里的规矩。色彩策略那两条
//    （capture.hdr_refused / capture.hdr_unverifiable）同理：那是**用户策略的结论**，换一条只带得
//    回 8 位的后端去出一张图，等于把拒绝换成一次静默降级。
// 2. **异常与致命错误分开。** 后端抛出东西时按性质决定：内存耗尽、显卡设备没了 —— 换一条
//    也不会有区别，立刻终止整条链；其余（WinRT 那几步的普通失败）才可以往下试。
// 3. **预算是整批一份，回退链不重新领。** 预算已经用尽就不再试下一条：这一条链最容易把
//    "一次截图"变成"四次各拿一份完整超时"，而 --timeout-ms 要管的就是这种重复领取。
//
// 这一层只**消费**闸门筛好的那条链（src/CursorControl.h 的 GateCaptureChain：版本 → 光标 → HDR），
// 它自己从不把某条通道换进链里 —— 显式点名的那条通道根本不经过这里（见 src/Capture.cpp 的
// CaptureWithMethod），所以"要哪个就要哪个"与"auto 不因为任何要求而绕开桌面确认"两条都不被动摇。

#include <string>
#include <utility>
#include <vector>

#include "CaptureCommon.h"   // CapturedFrame
#include "CliOptions.h"      // CaptureMethod / CaptureMethodName / Diagnostic / codes:: / stages::
#include "Deadline.h"        // dl.Spent()
#include "Lang.h"            // Msg / Msgf：那两条 note 与最后那条包装文案

namespace ecapture {

// 一条失败诊断对回退链意味着什么。只有两种，而这条线必须与用户策略对齐。
enum class ChainStop {
    // 这一关的结论与"用哪一条通道"无关：立刻停下，把这条诊断**原样**交出去。
    // 绝不包装成 capture.failed —— 那等于把调用方判断下一步所需的码（"没人同意" vs "机器不行"
    // vs "用户不让截 HDR"）统一抹掉。
    kStopWithVerdict,
    // 这个后端自己不行（帧超时 / 纹理不合法 / 目标矩形量不出来……）：可以试下一条**合格**的后端。
    kTryNext,
};

// 唯一判据。新增一条"关"（新的授权码、新的身份码、新的策略结论码）就在这里加一行，
// 而不是在回退链里再抄一份比较 —— 也不要拿它当"错误分类表"用：这里只回答换后端有没有意义。
inline ChainStop ClassifyChainStop(const std::wstring& code) {
    if (code == codes::kAccessDenied || code == codes::kConsentUnavailable ||
        code == codes::kConsentTimeout || code == codes::kConsentStale ||
        code == codes::kTargetGone || code == codes::kTargetChanged ||
        code == codes::kTargetUnverifiable || code == codes::kMonitorChanged ||
        code == codes::kMonitorUnverifiable || code == codes::kHdrRefused ||
        code == codes::kHdrUnverifiable) {
        return ChainStop::kStopWithVerdict;
    }
    return ChainStop::kTryNext;
}

// 按顺序试到第一个成功的通道。实际用的不是链首时留一条 note，让调用方知道画面来路不同。
// tryOne(通道, 帧, 诊断, 致命标记) 由调用方负责"后端抛异常也要变成一条诊断"（那一段与后端
// 的名字有关，所以留在调用方那一侧），这里只管链的走法与停下时交回哪一条诊断。
// 空链不是"成功"：那一步没取任何像素，交回一条 capture.failed 说明"没有可用通道"（正常情况下
// 闸门早就给了 env.* 那条错误，走不到这里；这条兜底是为了让"链为空"永远不等于交付了一张图）。
template <typename Try>
bool FallbackChain(const std::vector<CaptureMethod>& chain, const Deadline& dl, CapturedFrame* out,
                   Diagnostic* err, std::vector<Diagnostic>* notes, bool* fatal, Try tryOne) {
    std::wstring tried;
    Diagnostic last{};
    for (const CaptureMethod m : chain) {
        if (dl.Spent()) {
            if (err) *err = BudgetSpent(dl, codes::kCaptureTimeout, stages::kCapture,
                                         CaptureMethodName(m));
            return false;
        }
        CapturedFrame attempt;
        Diagnostic attemptErr{};
        const wchar_t* backend = CaptureMethodName(m);
        const bool ok = tryOne(m, &attempt, &attemptErr, fatal);
        if (ok) {
            *out = std::move(attempt);
            if (m != chain.front() && notes) {
                notes->push_back(Diagnostic{
                    codes::kCaptureChannel,
                    Msgf(L"note.capture_channel", CaptureMethodName(chain.front()),
                         CaptureMethodName(m)),
                    L"--capture", L"auto", std::wstring(), std::wstring(), backend,
                    stages::kCapture});
            }
            return true;
        }
        if (ClassifyChainStop(attemptErr.code) == ChainStop::kStopWithVerdict) {
            if (err) *err = std::move(attemptErr);
            return false;   // 这一关的结论不换后端重跑：拒绝就是拒绝，位置变了就重新确认
        }
        if (fatal && *fatal) {
            if (err) *err = std::move(attemptErr);
            return false;   // 致命错误：换后端不会有区别
        }
        if (!tried.empty()) tried += L", ";
        tried += backend;
        last = std::move(attemptErr);
    }
    if (err) {
        // backend 记的是"真实试过的那几条"，不是请求值 auto —— 调用方要据此判断该重试还是换通道
        Diagnostic d{codes::kCaptureFailed, Msgf(L"cap.auto_failed", tried), L"--capture", L"auto",
                     last.message, last.target, tried, stages::kCapture};
        d.hresult = last.hresult;
        d.win32 = last.win32;
        *err = std::move(d);
    }
    return false;
}

}  // namespace ecapture
