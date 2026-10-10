#include "Notification.h"

#include <algorithm>

#include "CaptureCommon.h"  // HResultText：诊断里那个 0x%08X 的写法与全仓同一份
#include "Lang.h"

namespace ecapture {
namespace {

// toast 能稳定落到屏幕上的文本槽位只有一份：一个标题行、两个正文行，再加一行可选的小字
// （attribution）。这是系统模板给的量，不是本工具自定的偏好 —— 写进 XML 的第四行正文并不会
// 让人多看见一行，而是被模板直接丢掉。所以这里把可用的槽位数当作**硬上限**，超出的信息
// 要么并进已有行、要么如实放弃，绝不假装"多写一行就多显示一行"。
constexpr size_t kBodySlots = 3;

// 单个值送进通知之前的长度上限。窗口标题与 --purpose 都是外部数据，一条几千字符的标题会把整份
// XML 撑开，也会把那块小小的通知区域挤成一团省略号。截断是**展示层**的行为：JSON 里的
// images[].title 从来不受它影响，完整值始终在报告里。
constexpr size_t kValueMaxChars = 120;

// 合并模式下最多逐个列出几个目标。留一个格子给"另有 N 个"那一句，
// 否则超出部分就只是无声消失，而人无从知道清单被截过。
constexpr size_t kMergedVisibleRows = 2;

// 一个值的展示形态：过长的折到上限，并留下"确实折过"的痕迹。
// 用省略号而不是删掉尾巴不吭声 —— 让人以为通知上就是完整标题，比少看几个字更糟。
std::wstring BriefValue(const std::wstring& value) {
    if (value.size() <= kValueMaxChars) return value;
    return value.substr(0, kValueMaxChars) + L"…";
}

void BriefArgs(NotifyLine* line) {
    for (std::wstring& arg : line->args) arg = BriefValue(arg);
}

// 归属那一行的内容：进程名与句柄。
// imageName 为空是合法状态（归属映像问不出来），这时不能拼出一个空进程名冒充知道。
NotifyLine OwnerLine(const CapturedImage& img) {
    NotifyLine line;
    if (!img.imageName.empty()) {
        line.key = L"notify.line.owner";
        line.args = {img.imageName, img.hwndHex};
    } else {
        line.key = L"notify.line.hwnd_only";
        line.args = {img.hwndHex};
    }
    return line;
}

// 落点那一行。附图出自历史副本，所以**优先报那张图自己的路径**：人对着通知上的图去找回文件，
// 指向的必须是通知里展示的那一份，而不是主输出（主输出可能是标准输出，那里根本没有文件）。
// 没有已提交的历史副本时退回主输出；主输出也是标准输出时，那句"写到标准输出"照实说。
NotifyLine LandingLine(const CapturedImage& img) {
    NotifyLine line;
    if (img.history.state == HistoryRecord::State::kSaved && !img.history.file.empty()) {
        line.key = L"notify.line.saved_history";
        line.args = {img.history.file};
        return line;
    }
    if (img.file == L"-") {
        line.key = L"notify.line.saved_stdout";
        return line;
    }
    line.key = L"notify.line.saved_output";
    line.args = {img.file};
    return line;
}

// 这个目标有没有一张"确实已经落在磁盘上"的历史副本可以当附图。
// 只认 kSaved：那是"独占提交完成"那一档。kFailed / kSkipped / kDisabled / kNone 都没有可读的第二份，
// 拿一个没提交成功的名字去引用图片，会让通知引用一张其实不存在、或只写了一半的文件。
bool HasHistoryImage(const CapturedImage& img) {
    return img.history.state == HistoryRecord::State::kSaved && !img.history.file.empty();
}

// 单个目标在合并清单里的那一行。档位决定这一行说多少。
NotifyLine MergedRow(const CapturedImage& img, NotifyDetail detail) {
    NotifyLine row;
    if (img.screen) {
        row.key = L"notify.row.device";
        row.args = {img.deviceName};
    } else if (detail == NotifyDetail::kTitle && !img.title.empty()) {
        row.key = L"notify.row.title_owner";
        row.args = {img.title, img.imageName};
    } else if (detail == NotifyDetail::kMinimal) {
        row.key = L"notify.row.file";
        // 标准输出不是一个路径：合并清单那一格里写 "-" 会读成"文件名叫减号"。这里换的是**那个
        // 概念的名字**（"标准输出"），不是本地化句式，所以在这一步取名而不必把判据推给渲染层。
        row.args = {img.file == L"-" ? Msg(L"notify.stdout_placeholder") : img.file};
    } else {
        row.key = L"notify.row.owner";
        row.args = {img.imageName};
    }
    return row;
}

// 目的这一行排在最前：那是调用方（通常是 AI 或脚本）自己写下的一句话，读的人第一想知道的
// 就是它。空串 = 这次没人写，那一行整个不出现，不留一句空文案占掉一个格子。
void PrependPurpose(NotifyPayload* p, const std::wstring& purpose) {
    if (purpose.empty()) return;
    NotifyLine line{L"notify.line.purpose", {purpose}};
    BriefArgs(&line);
    p->lines.insert(p->lines.begin(), std::move(line));
}

// 收进槽位：超过系统模板能显示的量就整行丢掉，而不是写进 XML 等着被模板吞掉。
void TrimToSlots(NotifyPayload* p) {
    if (p->lines.size() > kBodySlots) p->lines.resize(kBodySlots);
}

// 补充说明按约定"<code>_hint 有就带、没有就整个字段省略"（与 src/Config.cpp 那一条同源）：
// Msg() 对不存在的 key 返回 "?key"，那是留给写错的 key 的痕迹，不该当成一句真的文案送出去。
void AttachHint(Diagnostic* d) {
    const std::wstring hintKey = d->code + L"_hint";
    if (HasMsg(hintKey.c_str())) d->hint = Msg(hintKey.c_str());
}

}  // namespace

NotifyPayload MakeTargetNotifyPayload(const CapturedImage& img, NotifyDetail detail,
                                      const std::wstring& purpose) {
    NotifyPayload p;
    p.titleKey = img.screen ? L"notify.toast.screen" : L"notify.toast.window";

    // 三档的差别只在"多带哪一行用户数据"，而**落点那一行三档都有**：关掉通知内容的人仍然要
    // 知道图去哪儿了，那一行是这条通知的最小意义。
    switch (detail) {
        case NotifyDetail::kTitle:
            if (img.screen) {
                // 屏幕目标没有窗口标题可带：这一档在它身上退化成设备名那一行。
                // 退化要如实，不能让 carriesWindowTitle 谎报"这里头有标题"。
                NotifyLine device{L"notify.line.device", {img.deviceName}};
                BriefArgs(&device);
                p.lines.push_back(std::move(device));
            } else {
                NotifyLine titleLine{L"notify.line.title", {img.title}};
                BriefArgs(&titleLine);
                p.lines.push_back(std::move(titleLine));
                if (!img.title.empty()) p.carriesWindowTitle = true;
            }
            break;
        case NotifyDetail::kBasic:
            p.lines.push_back(OwnerLine(img));
            break;
        case NotifyDetail::kMinimal:
            // 一个目标字段都不带。
            break;
    }

    NotifyLine landing = LandingLine(img);
    BriefArgs(&landing);
    p.lines.push_back(std::move(landing));

    // 标题档的归属那一行（落在小字那一格）：基本档与最简档不需要它。
    // 屏幕目标本来就没有进程与句柄可报，也不为它编一行假归属。
    if (detail == NotifyDetail::kTitle && !img.screen) {
        NotifyLine owner = OwnerLine(img);
        BriefArgs(&owner);
        p.lines.push_back(std::move(owner));
    }

    PrependPurpose(&p, purpose);
    TrimToSlots(&p);
    if (HasHistoryImage(img)) p.image = img.history.file;
    return p;
}

NotifyPayload MakeMergedNotifyPayload(const std::vector<CapturedImage>& images,
                                      NotifyDetail detail, const std::wstring& purpose) {
    NotifyPayload p;
    p.titleKey = L"notify.toast.merged";
    p.titleArgs = {std::to_wstring(images.size())};

    if (!purpose.empty()) {
        NotifyLine line{L"notify.line.purpose", {purpose}};
        BriefArgs(&line);
        p.lines.push_back(std::move(line));
    }

    const size_t reserved = p.lines.size();   // 目的那一行：0 或 1
    const size_t room = kBodySlots - reserved;
    // 全部列得下就全列；列不下时让出最后一格给"另有 N 个"那一句，而不是无声截断。
    // room 恒 ≥2（reserved ≤1 而 kBodySlots=3），所以下面那个 room-1 不会退化成 0。
    const size_t visible = images.size() <= room ? images.size() : room - 1;
    for (size_t i = 0; i < visible; ++i) {
        NotifyLine row = MergedRow(images[i], detail);
        BriefArgs(&row);
        p.lines.push_back(std::move(row));
        if (!images[i].screen && detail == NotifyDetail::kTitle && !images[i].title.empty())
            p.carriesWindowTitle = true;
    }
    if (images.size() > visible) {
        NotifyLine more{L"notify.row.more", {std::to_wstring(images.size() - visible)}};
        p.lines.push_back(std::move(more));
    }

    for (const CapturedImage& img : images) {
        if (HasHistoryImage(img)) {
            p.image = img.history.file;
            break;
        }
    }
    TrimToSlots(&p);
    return p;
}

namespace {

// XML 文本转义。窗口标题、类名、路径与 --purpose 全部是外部数据，未转义直接拼进 toast XML
// 就是一条注入面：一扇把标题起成 `</text><image src=...>` 的窗口能改写这条通知的形状。
// 这里按 XML 1.0 的规矩来，而不是"看起来像标签就躲一下"：
//   * 五个 XML 预定义实体全换（属性里的 src 也要靠 &quot; 才封得住）。
//   * 控制字符（含 NUL 与 <0x20 的那些）在 XML 1.0 里根本不合法，留着会让 XmlDocument 直接拒收
//     整份文档 —— 那会把一次好好的截图变成一条没送出去的通知。换行与制表折成空格。
//   * Surrogate 与 BMP 之外的字符原样留着：XmlDocument 收合法 UTF-16 序列，本工具不改语义。
std::wstring EscapeXmlText(const std::wstring& raw) {
    std::wstring out;
    out.reserve(raw.size() + 8);
    for (const wchar_t c : raw) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            case L'\r':
            case L'\n':
            case L'\t': out += L' '; break;
            default:
                if (c < 0x20 || c == 0x7F) continue;  // XML 1.0 里不合法的字符直接丢掉
                out += c;
        }
    }
    return out;
}

void AppendText(std::wstring* xml, const std::wstring& text, bool attribution) {
    xml->append(L"<text");
    if (attribution) xml->append(L" placement=\"attribution\"");
    xml->append(L">");
    xml->append(EscapeXmlText(text));
    xml->append(L"</text>");
}

// 取文案并代入原文。args 里每一项都先过长度上限：判据在构造层折过一次，渲染这一层再兜一道，
// 因为合并模式与屏幕目标的行是在别处拼的，漏折就会把整份 XML 撑开。
std::wstring Localize(const std::wstring& key, const std::vector<std::wstring>& args) {
    std::vector<std::wstring> bounded;
    bounded.reserve(args.size());
    for (const std::wstring& arg : args) bounded.push_back(BriefValue(arg));
    return detail::Substitute(Msg(key.c_str()), bounded);
}

}  // namespace

std::wstring RenderToastXml(const NotifyPayload& payload) {
    std::wstring xml = L"<toast><visual><binding template=\"ToastGeneric\">";
    AppendText(&xml, Localize(payload.titleKey, payload.titleArgs), false);

    // 槽位映射是固定的一条线：第 1、2 行走正文，第 3 行走 attribution 小字。
    // 构造层已经把 lines 截到 kBodySlots 之内，这里按位置判而不重复截一次。
    for (size_t i = 0; i < payload.lines.size() && i < kBodySlots; ++i) {
        const bool attribution = (i + 1 == kBodySlots);
        AppendText(&xml, Localize(payload.lines[i].key, payload.lines[i].args), attribution);
    }

    // 附图走的是文件系统路径那一格：它引用的必须是**已经在磁盘上**的那一份（历史副本），
    // 而且这一步不为了引用它把文件复制到别处、也不为它新建临时文件。
    if (!payload.image.empty()) {
        xml += L"<image src=\"";
        xml += EscapeXmlText(payload.image);
        xml += L"\"/>";
    }
    xml += L"</binding></visual></toast>";
    return xml;
}

const wchar_t* NotifyStatusName(NotifyStatus status, bool disabled) {
    switch (status) {
        case NotifyStatus::kSubmitted: return L"submitted";
        case NotifyStatus::kFailed: return L"failed";
        case NotifyStatus::kNone: break;
    }
    // kNone 的两种成因对调用方是不同的下一步：一条是"这个功能你关掉了"，
    // 另一条是"这次没有可报的东西"。把它们混成一个词，排障时就得靠猜。
    return disabled ? L"disabled" : L"no_delivery";
}

NotifyOutcome SendCaptureNotifications(const std::vector<CapturedImage>& images, NotifyMode mode,
                                       NotifyDetail detail, const std::wstring& purpose,
                                       const ToastSender& sender) {
    NotifyOutcome out;
    if (images.empty()) return out;  // kNone / no_delivery：一个像素都没落地，这一步根本没发生

    // 先按 mode 把要发的每一份 payload 备齐，再逐条交出去：这样"合并"只是 payload 的数量差，
    // 而不是两套发送流程。发送那一步的成败判据对两种模式完全相同。
    std::vector<NotifyPayload> payloads;
    payloads.reserve(mode == NotifyMode::kMerged ? 1 : images.size());
    if (mode == NotifyMode::kMerged) {
        payloads.push_back(MakeMergedNotifyPayload(images, detail, purpose));
    } else {
        for (const CapturedImage& img : images)
            payloads.push_back(MakeTargetNotifyPayload(img, detail, purpose));
    }

    for (const NotifyPayload& payload : payloads) {
        NativeErrorCode hr = kNativeErrorNone;
        if (sender(payload, &hr)) {
            ++out.submitted;
            if (!payload.image.empty()) ++out.withImage;
            continue;
        }
        ++out.failed;
        // 留住这一次的返回值本身。拿 E_FAIL 顶上去会把"接口没实现""通知平台拒了""没有注册的
        // AUMID 可对号"这三种完全不同的故障写成同一条 —— 而那正是下一步该看的东西。
        out.lastError = hr;
    }

    if (out.submitted > 0) {
        out.status = NotifyStatus::kSubmitted;
        if (out.failed > 0) {
            // 部分没送出去也照实留一条：submitted 那一个数说的是"交出去几条"，
            // 不因为还有一条失败就少报，也不因为有一条成功就整批算成功。
            out.note = Diagnostic{};
            out.note.code = codes::kNotifySendFailed;
            out.note.message = Msg(codes::kNotifySendFailed);
            AttachHint(&out.note);
            out.note.stage = stages::kNotify;
            out.note.hresult = HResultText(static_cast<HRESULT>(out.lastError));
        }
        return out;
    }

    // 一条都没交出去。这里给的是"这一步自己失败了"，**不是**截图失败：images 里那一份交付
    // 事实与退出码都在调用方手里，本函数一个字段都没碰。
    out.status = NotifyStatus::kFailed;
    out.note = Diagnostic{};
    // 拿不出任何一次调用结果时（发送器压根没跑起来）用 unavailable 那一族：它说的是"这条路说不通"，
    // 而 send_failed 说的是"这条路试了而系统拒了"，两者的下一步不同。
    out.note.code = out.lastError == 0 ? codes::kNotifyUnavailable : codes::kNotifySendFailed;
    out.note.message = Msg(out.note.code.c_str());
    out.note.stage = stages::kNotify;
    if (out.lastError != 0) out.note.hresult = HResultText(static_cast<HRESULT>(out.lastError));
    return out;
}

}  // namespace ecapture
