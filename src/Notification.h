#pragma once
// 截图完成后的桌面通知（Windows toast）。
//
// 这一层做的事只有一件：把**已经交付完成**的那一张图与它的目标，交给人看一眼。它不属于取帧、
// 不属于授权，也不属于交付事实 —— 图早在这一刻之前就已经落在磁盘上了。
//
// 五条规矩：
//   1. **通知是展示状态，不是事实来源。** 一次 Show() 成功只说明"这一条已经交给系统的通知平台"，
//      绝不说明屏幕上真的出现过它 —— 专注助手、通知设置、锁屏策略、应用静默规则都能把它藏掉，
//      而本工具一个字都问不出来。所以状态名用的是 submitted（交出去了）而不是 shown（显示了），
//      调用方与文档都不许把它读成"用户已经看到了"。真正的事实只有 images[] 那一份。
//   2. **它从不改动已经成立的结果。** 一条都没送出去、送的时候崩了、系统压根不给送 —— 这三种
//      情况下，images 里的交付事实、errors 里的失败码与退出码**原样不动**。通知失败永远不是
//      截图失败，也永远不该让人为了"补一条通知"去重拍一张图。
//   3. **只在真的有图落地之后才发。** 一次都没交付成功（人拒绝了、没命中、写坏了）时这里一步都不做：
//      那种场合告诉人"截好了"是假的。只读查询（--list / --inspect / --capabilities / --screens /
//      --show-config）同一条线上也永远不发。
//   4. **附图只有历史那份。** 通知里要带图就复用主交付之外那一份历史副本（images[].history.file），
//      于是这一步不新建任何文件、不重编码、不去读主输出（它可能是标准输出，也可能正被别人改）。
//      历史关掉或那张副本没落地时，通知照发，只是不带图 —— 少一张图不是失败，也不为此报错。
//   5. **发不出去就不发。** 未打包的 Win32 程序要发 toast，必须有一个已在开始菜单快捷方式上注册过的
//      AppUserModelID（安装器负责登记那一条）。没有它、通知平台问不出来、套间起不来，都按"这次不发"
//      处理：留下如实的状态与 HRESULT 原值，不重试、不提权、不换个通道硬塞、也不静默改成弹一个
//      真的对话框去顶替它。
//
// 隐私这一头写在档级里，不写在文案里：通知的内容会进入系统通知中心，而那块地方在锁屏上也读得到。
// 三档 detail 由用户在配置或命令行上自己选（见 NotifyDetail），本层不在文案里额外添置任何
// 未在那一档承诺过的字段。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Capture.h"     // CapturedImage / HistoryRecord
#include "CliOptions.h"  // Diagnostic / NotifyDetail / NotifyMode / stages

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // HRESULT

namespace ecapture {

// 本工具的通知身份。它必须与安装器写进开始菜单快捷方式的那一条**逐字相同**：toast 归到哪一个
// 应用、叫什么名、用哪个图标，全靠这个字符串对上号。改它要同时改 installer/ecapture.iss。
inline constexpr const wchar_t* kNotifyAumid = L"yashi.EvernightCapture";

// 每一行是"一条资源 key + 代入它的那份原文"。故意不在这里就拼成字符串：
//   * 离线判据因此能核对**内容本身**（标题在不在这一档里、路径是不是那一条），而不必把
//     嵌入资源的英文文案抄一份进测试当预言 —— 那是拿同一个假设自证。
//   * 本地化发生在渲染那一步，与 --lang 的实际取值同源。
struct NotifyLine {
    std::wstring key;                  // resources/strings-*.txt 里的 key（稳定，不随 --lang 变）
    std::vector<std::wstring> args;    // 代入 %1..%n 的原文（窗口标题、路径这类用户数据留在这一头）
};

struct NotifyPayload {
    std::wstring titleKey;             // 通知标题那一句的资源 key
    std::vector<std::wstring> titleArgs;
    std::vector<NotifyLine> lines;
    // 要附在通知上的那张图。空 = 这一条不带图。它恒等于某个 images[].history.file ——
    // 这一步既不新建文件，也不把主输出的名字当成"另一份已经存在的副本"。
    std::wstring image;
    // 这一份 payload 究竟有没有把某个窗口标题交出去。判据由构造那一步据实填，
    // 不让调用方靠"哪一档"去倒推（--notify-detail title 配屏幕目标时本来就没有标题可带）。
    bool carriesWindowTitle = false;
};

// 单个目标 -> 这一条通知该写什么。纯函数：不碰 WinRT、不碰磁盘、不取任何新的窗口信息，
// 只读 img 里已经落定的那些字段。purpose 是 --purpose 那句原文（可为空），它是调用方自己
// 写下的一句话，本层不据它判任何事，只把它排在最前面供人阅读。
NotifyPayload MakeTargetNotifyPayload(const CapturedImage& img, NotifyDetail detail,
                                      const std::wstring& purpose);

// 整批目标 -> 一条合并通知。同样纯函数。
NotifyPayload MakeMergedNotifyPayload(const std::vector<CapturedImage>& images,
                                      NotifyDetail detail, const std::wstring& purpose);

// 把一份 payload 渲染成 toast 的 XML 文本（ToastGeneric 模板）。
// 这一步做三件构造本身做不了的事：按 --lang 取文案、把用户数据**转义**成 XML 文本、把过长的
// 值折到长度上限之内。窗口标题与 --purpose 都是外部数据，未经转义绝不进 XML —— 那是一条注入面。
std::wstring RenderToastXml(const NotifyPayload& payload);

// 发送的下场。三条各说一件事，谁也不冒充谁：
//   kNone      这一步根本没做（配置关了、这次没有交付成功的图、只读查询那一路）
//   kSubmitted 已经交给系统的通知平台 —— **不**证明它显示过（见文件开头第 1 条）
//   kFailed    那一次调用自己就失败了；原因在 notes 同码那一条里（含 HRESULT 原值）
enum class NotifyStatus { kNone, kSubmitted, kFailed };

// 进 JSON 的 ASCII 机器名（只增不改名）："disabled" / "no_delivery" / "submitted" / "failed"。
// kNone 分两种成因，它们对调用方是不同的下一步，所以这里给两个词而不是一个：
//   disabled    用户关掉了这一功能（配置里 notification.enabled=false 或 --notify off）
//   no_delivery 这次没有任何完成交付的图（人拒绝、没命中、写盘失败）
// mode 与 detail 那两份机器名不在这里：它们是枚举到词的映射，与 FormatName / MultiKey 同族，
// 所以声明在 CliOptions.h、定义在 CliOptions.cpp，由解析层、--show-config 与渲染层共读一份。
const wchar_t* NotifyStatusName(NotifyStatus status, bool disabled);

struct NotifyOutcome {
    NotifyStatus status = NotifyStatus::kNone;
    bool disabled = false;      // 是"用户关掉了"，不是"没送到"
    uint32_t submitted = 0;     // 交出去几条（merged 时至多 1）
    uint32_t withImage = 0;     // 其中带图的几条（附图恒出自历史副本）
    uint32_t failed = 0;
    HRESULT lastError = 0;      // 仅 failed：那一次调用自己的返回值，不拿 E_FAIL 顶
    // 送不出去时给人看的那一条诊断（含稳定码与 HRESULT 文本）。它**不**进 errors：
    // 通知失败不改退出码，也不该被读成一次截图失败。
    Diagnostic note;
};

// 发送那一次调用。生产实现走 WinRT toast（NotificationToast.cpp），离线判据注入假应答 ——
// 被 judged 的始终是下面那个编排函数与上面那两个构造函数，不是测试里另抄的一份。
// 返回 true = 已交给通知平台；false 时 *hr 是这一次调用自己的结果。
using ToastSender = std::function<bool(const NotifyPayload& payload, HRESULT* hr)>;

// 整批的编排：按 mode 决定发几条、按 detail 决定写什么、按历史副本是否真落地决定带不带图，
// 然后把每一条交出去并把下场汇总成一份如实的结果。
//
// sender 由调用方**显式**给，不留一个默认实参指向生产那份 WinRT：那会让每一个链进
// Notification.cpp 的目标（包括一个像素都不碰的离线判据）都不得不把系统的通知平台一起链进来，
// 判据也就失去了"这里绝不会真的弹出一条东西"这个保证。调用方只有两处 —— 渲染层传
// ShowToastViaWinrt，判据传自己的假应答。
//
// 这一步不判"要不要发"（那是调用方的事：关闭与零交付都在它之前拦掉），也不改
// images / errors / 退出码。送不出去的那几条在这里汇总成 kFailed + 一条 note，到此为止。
NotifyOutcome SendCaptureNotifications(const std::vector<CapturedImage>& images, NotifyMode mode,
                                       NotifyDetail detail, const std::wstring& purpose,
                                       const ToastSender& sender);

// 生产那份发送器：WinRT toast。放在这里是为了让编排的默认实参与离线注入点同在一处声明。
// 它要求本线程已有套间（调用方用 EnsureWinrtOnThisThread 保证），失败时如实给出 HRESULT。
bool ShowToastViaWinrt(const NotifyPayload& payload, HRESULT* hr);

}  // namespace ecapture
