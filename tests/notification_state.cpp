// 截图后桌面通知（src/Notification.cpp）的离线判据。
//
// 为什么单独一个可执行文件：这一批判据要判的是"**这条通知该写什么、该不该带图、送不出去怎么
// 归类**"，而那些事在真机上要么得改用户的系统设置（专注助手、通知权限、AUMID 注册），要么
// 根本无法观察（屏幕上到底弹没弹）。所以这里判的是内容构造与编排归类，被 judged 的本体是：
//   src/Notification.cpp —— MakeTargetNotifyPayload / MakeMergedNotifyPayload（三档 detail、
//                          落点选择、附图资格、长度收敛）、RenderToastXml（本地化与 XML 转义）、
//                          SendCaptureNotifications（按 mode 决定几条、成败怎么汇总）
//   src/CliOptions.cpp   —— NotifyModeName / NotifyDetailName（与解析层、--show-config 同一份）
// 发送那一次调用本身（src/NotificationToast.cpp 那份 WinRT）从不在这里判：它要真问系统的通知
// 平台，判它得到的是"这台机器让不让"，而不是"我们写对了没有"。测试注入 ToastSender，
// 于是"交出去几条""部分失败""全部失败"都排得出确定现场，而被判的仍是生产那份编排函数。
//
// 三条判据线各管各的，不拿一份证据冒充另一份：
//   * 内容（哪一档写哪些字段、图出自哪一份）在这里按 key 与**原文 args** 判。判 args 而不判整句
//     文案：那不必把嵌入资源的英文句子抄一份进测试当预言，而抄来的预言只会自证。
//   * "系统拒了 / 这条路说不通 / 关掉 / 一张都没交付"四种归类的下一步不同，所以逐条判 code 与
//     status，并核对**通知失败绝不改动 images**（那是交付事实，与展示无关）。
//   * 真机上"到底弹没弹"、AUMID 有没有注册上、专注助手有没有藏掉它，写在 README 与 Skill 里
//     标成需要使用者本人确认，不在这里冒充已验证。
//
// 只用 C 风格的 printf 汇报，判据写在断言里；任何一条不过就返回非 0。

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/Lang.h"
#include "../src/Notification.h"

using namespace ecapture;

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (ok) return;
    ++failures;
    std::printf("  FAIL: %s\n", what);
}

// 一条已经交付完成的记录，默认是"窗口内容路径、主输出写了文件、历史副本已提交"这一种最常见现场。
// 每一项都可以被调用方改掉，好把"历史关了""写到标准输出""屏幕目标"这些负例摆出来。
CapturedImage MakeImage() {
    CapturedImage img;
    img.file = L"C:\\shots\\a.png";
    img.history.state = HistoryRecord::State::kSaved;
    img.history.file = L"C:\\exe\\history\\2026-10-13\\1.png";
    img.title = L"无标题 - 记事本";
    img.windowClass = L"Notepad";
    img.pid = 4242;
    img.hwndHex = L"0x001A0B4C";
    img.imageName = L"notepad.exe";
    img.width = 800;
    img.height = 600;
    return img;
}

// 找某一行的原文参数；没有那一行时返回空串（调用方据此判"这一档不该有这一行"）。
std::wstring ArgOf(const NotifyPayload& p, const wchar_t* key, size_t index = 0) {
    for (const NotifyLine& line : p.lines) {
        if (line.key == key && line.args.size() > index) return line.args[index];
    }
    return std::wstring();
}

bool HasLine(const NotifyPayload& p, const wchar_t* key) {
    for (const NotifyLine& line : p.lines) {
        if (line.key == key) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 1. 三档 detail：谁能把窗口标题交出去
// ---------------------------------------------------------------------------
void TestDetailLevels() {
    std::printf("[1] detail 三档\n");
    const CapturedImage img = MakeImage();

    const NotifyPayload title = MakeTargetNotifyPayload(img, NotifyDetail::kTitle, L"");
    Check(HasLine(title, L"notify.line.title"), "title 档要有标题那一行");
    Check(ArgOf(title, L"notify.line.title") == img.title, "title 档交出的就是那个标题原文");
    Check(title.carriesWindowTitle, "title 档要如实承认带了标题");
    Check(HasLine(title, L"notify.line.owner"), "title 档仍要报归属");
    Check(HasLine(title, L"notify.line.saved_history"), "title 档要报落点");

    const NotifyPayload basic = MakeTargetNotifyPayload(img, NotifyDetail::kBasic, L"");
    Check(!HasLine(basic, L"notify.line.title"), "basic 档不许出现标题那一行");
    Check(!basic.carriesWindowTitle, "basic 档不能谎报带了标题");
    Check(ArgOf(basic, L"notify.line.owner", 0) == img.imageName, "basic 档第一行是归属");
    Check(HasLine(basic, L"notify.line.saved_history"), "basic 档照旧报落点");

    const NotifyPayload minimal = MakeTargetNotifyPayload(img, NotifyDetail::kMinimal, L"");
    Check(minimal.lines.size() == 1, "minimal 档只剩落点这一行");
    Check(!HasLine(minimal, L"notify.line.title"), "minimal 档不许有标题");
    Check(!HasLine(minimal, L"notify.line.owner"), "minimal 档不许有归属");
    Check(minimal.lines.size() > 0 && minimal.lines[0].key == L"notify.line.saved_history",
          "minimal 那一行报的必须是落点");
}

// ---------------------------------------------------------------------------
// 2. 落点与附图：只认"真在磁盘上"的那一份
// ---------------------------------------------------------------------------
void TestLandingAndImage() {
    std::printf("[2] 落点与附图资格\n");

    // 历史已提交：附图与落点那一行都出自那一份副本（通知展示的就是它）。
    CapturedImage saved = MakeImage();
    NotifyPayload p = MakeTargetNotifyPayload(saved, NotifyDetail::kBasic, L"");
    Check(p.image == saved.history.file, "副本已提交时附图就是那一份");
    Check(ArgOf(p, L"notify.line.saved_history") == saved.history.file,
          "落点那一行报的是被展示的那张图的路径");

    // 四种"没有副本"的下场各判一次：谁都不许带上图，也不许冒充落点是副本。
    for (HistoryRecord::State state : {HistoryRecord::State::kNone, HistoryRecord::State::kFailed,
                                       HistoryRecord::State::kSkipped,
                                       HistoryRecord::State::kDisabled}) {
        CapturedImage img = MakeImage();
        img.history.state = state;
        img.history.file.clear();   // kSaved 之外从不留路径：没提交的名字不算副本
        const NotifyPayload q = MakeTargetNotifyPayload(img, NotifyDetail::kBasic, L"");
        Check(q.image.empty(), "没有已提交的副本就不许附图");
        Check(!HasLine(q, L"notify.line.saved_history"), "没有副本就不许报历史落点");
        Check(HasLine(q, L"notify.line.saved_output"), "这时落点退回主输出");
    }

    // 写坏了一半：名字留着但状态是 failed —— 更不能拿它当图（那会引用一张不完整文件）。
    CapturedImage half = MakeImage();
    half.history.state = HistoryRecord::State::kFailed;
    const NotifyPayload r = MakeTargetNotifyPayload(half, NotifyDetail::kBasic, L"");
    Check(r.image.empty(), "failed 那一份留着名字也不算副本");

    // 标准输出那一路：根本没有文件，落点那句要照实说。
    CapturedImage pipe = MakeImage();
    pipe.file = L"-";
    pipe.history.state = HistoryRecord::State::kDisabled;
    pipe.history.file.clear();
    const NotifyPayload s = MakeTargetNotifyPayload(pipe, NotifyDetail::kMinimal, L"");
    Check(s.image.empty(), "写到标准输出时没有文件可附");
    Check(HasLine(s, L"notify.line.saved_stdout"), "落点要说'已写到标准输出'");
    Check(!HasLine(s, L"notify.line.saved_output"), "标准输出不算成一个路径");

    // 屏幕目标：没有进程与句柄可报，也不给它编一行假归属。
    CapturedImage screen = MakeImage();
    screen.screen = true;
    screen.deviceName = L"DISPLAY1";
    screen.title.clear();
    screen.imageName.clear();
    const NotifyPayload t = MakeTargetNotifyPayload(screen, NotifyDetail::kTitle, L"");
    Check(HasLine(t, L"notify.line.device"), "屏幕目标报设备名");
    Check(!t.carriesWindowTitle, "屏幕目标不许谎报带了窗口标题");
    Check(!HasLine(t, L"notify.line.owner"), "屏幕目标没有归属可报，不编一行假的");
}

// ---------------------------------------------------------------------------
// 3. 归属行的兜底与长度收敛
// ---------------------------------------------------------------------------
void TestOwnerFallbackAndLength() {
    std::printf("[3] 归属兜底与长度\n");

    // 归属映像问不出来是合法状态（进程信息读不到），这时不能拼一个空进程名冒充知道。
    CapturedImage img = MakeImage();
    img.imageName.clear();
    const NotifyPayload p = MakeTargetNotifyPayload(img, NotifyDetail::kBasic, L"");
    Check(HasLine(p, L"notify.line.hwnd_only"), "拿不到进程名时退回句柄那一行");
    Check(!HasLine(p, L"notify.line.owner"), "不许拼一个空进程名的归属行");
    Check(ArgOf(p, L"notify.line.hwnd_only") == img.hwndHex, "句柄那一行给的是 hwnd");

    // 超长值折到上限并留下痕迹（展示层的收敛；JSON 里的完整标题不受影响）。
    CapturedImage longImg = MakeImage();
    longImg.title = std::wstring(400, L'长');
    const NotifyPayload q = MakeTargetNotifyPayload(longImg, NotifyDetail::kTitle, L"");
    const std::wstring shown = ArgOf(q, L"notify.line.title");
    Check(!shown.empty() && shown.size() <= 130, "标题要折到上限之内");
    Check(shown.back() == L'…', "折过要留下省略号，不许装作那是完整标题");
    Check(longImg.title.size() == 400, "构造不许改动原图那份记录里的标题");

    const NotifyPayload m = MakeMergedNotifyPayload({longImg}, NotifyDetail::kTitle, L"");
    Check(ArgOf(m, L"notify.row.title_owner").size() <= 130, "合并清单里同样折");
}

// ---------------------------------------------------------------------------
// 4. --purpose 那一行
// ---------------------------------------------------------------------------
void TestPurpose() {
    std::printf("[4] purpose\n");
    const CapturedImage img = MakeImage();

    const NotifyPayload withOut = MakeTargetNotifyPayload(img, NotifyDetail::kTitle, L"为了写文档");
    Check(withOut.lines.size() == 3, "槽位仍受上限约束");
    Check(withOut.lines[0].key == L"notify.line.purpose", "purpose 排第一行");
    Check(ArgOf(withOut, L"notify.line.purpose") == L"为了写文档", "那一行给的就是原文");
    // title 档本来有 标题 / 落点 / 归属 三行，purpose 进来挤掉最后一格 —— 这是展示量的事实。
    Check(!HasLine(withOut, L"notify.line.owner"), "purpose 占一格时归属让位");
    Check(HasLine(withOut, L"notify.line.title"), "标题那一行仍在");

    // 没写 purpose 就不该占一格，更不能留一句空文案。
    const NotifyPayload none = MakeTargetNotifyPayload(img, NotifyDetail::kTitle, L"");
    Check(!HasLine(none, L"notify.line.purpose"), "没写 purpose 时那一行整个不出现");
    Check(none.lines.size() == 3, "三格留给目标信息");
}

// ---------------------------------------------------------------------------
// 5. 合并模式：列得下就全列，列不下要说清还剩几个
// ---------------------------------------------------------------------------
void TestMerged() {
    std::printf("[5] 合并模式\n");
    std::vector<CapturedImage> images;
    for (int i = 0; i < 5; ++i) {
        CapturedImage img = MakeImage();
        img.title = L"窗口" + std::to_wstring(i);
        img.history.file = L"C:\\h\\" + std::to_wstring(i) + L".png";
        images.push_back(img);
    }

    const NotifyPayload p = MakeMergedNotifyPayload(images, NotifyDetail::kTitle, L"");
    Check(p.titleKey == L"notify.toast.merged", "合并那条的标题说的是数量");
    Check(p.titleArgs.size() == 1 && p.titleArgs[0] == L"5", "标题里给的是目标总数");
    Check(p.lines.size() == 3, "两行清单 + 一行数量兜底");
    Check(p.lines[2].key == L"notify.row.more", "最后一行要说'还有几个'");
    Check(ArgOf(p, L"notify.row.more") == L"3", "5 个里列了 2 个，剩下 3 个要照实报");
    Check(ArgOf(p, L"notify.row.title_owner") == L"窗口0", "清单第一行是第一个目标");
    Check(p.image == images[0].history.file, "附图取第一个已提交副本");
    Check(p.carriesWindowTitle, "列出来的那两行里确实有标题");

    // 列得下就全列，不留一句假的"还有 N 个"。
    std::vector<CapturedImage> two(images.begin(), images.begin() + 2);
    const NotifyPayload q = MakeMergedNotifyPayload(two, NotifyDetail::kBasic, L"");
    Check(q.lines.size() == 2, "两个目标都列得下");
    Check(!HasLine(q, L"notify.row.more"), "全列下了就不许多一句'还有'");

    // 只有后面的目标有副本：附图不能因此就没有。
    std::vector<CapturedImage> mixed(2, MakeImage());
    mixed[0].history.state = HistoryRecord::State::kDisabled;
    mixed[0].history.file.clear();
    mixed[1].history.file = L"C:\\h\\second.png";
    const NotifyPayload r = MakeMergedNotifyPayload(mixed, NotifyDetail::kTitle, L"");
    Check(r.image == L"C:\\h\\second.png", "附图取第一个真有副本的，不看位置");

    // purpose 占一格时，清单只列一个而剩下的照实报数 —— 不许无声截断。
    const NotifyPayload s = MakeMergedNotifyPayload(images, NotifyDetail::kTitle, L"发布前核对");
    Check(s.lines[0].key == L"notify.line.purpose", "purpose 仍占第一行");
    Check(HasLine(s, L"notify.row.more"), "有 purpose 时也必须有数量那一行");
    Check(s.lines.size() == 3, "总槽位不变");

    // 空批：合并那条也不该被构造出来（调用方在零交付时就拦掉了），这里判它对 0 个目标不崩。
    const NotifyPayload e = MakeMergedNotifyPayload({}, NotifyDetail::kTitle, L"");
    Check(e.lines.empty(), "零个目标时没有清单行");
}

// ---------------------------------------------------------------------------
// 6. XML 渲染：外部数据必须转义
// ---------------------------------------------------------------------------
void TestXmlEscaping() {
    std::printf("[6] XML 转义\n");
    SetLanguage(Language::kEn);

    // 一扇把标题起成闭合标签再开一个 image 的窗口，能改写这条通知的形状 —— 那是注入面。
    CapturedImage img = MakeImage();
    img.title = L"</text><image src=\"evil.png\"/><text>";
    const NotifyPayload p = MakeTargetNotifyPayload(img, NotifyDetail::kTitle, L"");
    const std::wstring xml = RenderToastXml(p);
    Check(xml.find(L"<image src=\"evil.png\"/>") == std::wstring::npos,
          "标题里的 image 标签不许变成真元素");
    Check(xml.find(L"&lt;/text&gt;") != std::wstring::npos, "闭合标签要被转义");
    Check(xml.find(L"&quot;") != std::wstring::npos, "引号要被转义（属性那一格也封得住）");

    // 真正的附图仍然只有一个，且就是那一份历史副本。
    size_t imageTags = 0;
    for (size_t pos = xml.find(L"<image "); pos != std::wstring::npos; pos = xml.find(L"<image ", pos + 1))
        ++imageTags;
    Check(imageTags == 1, "整份 XML 只许有一个 image 元素");
    Check(xml.find(img.history.file) != std::wstring::npos, "那一个引用的就是历史副本");

    // & < > 之外的麻烦：XML 1.0 里控制字符根本不合法，留着会让 XmlDocument 拒收整份文档。
    CapturedImage ctrl = MakeImage();
    ctrl.title = L"a\tb\rc\x01d\x7Fe";
    const std::wstring xml2 = RenderToastXml(MakeTargetNotifyPayload(ctrl, NotifyDetail::kTitle, L""));
    Check(xml2.find(L'\t') == std::wstring::npos, "制表符不许裸进 XML");
    Check(xml2.find(L'\r') == std::wstring::npos, "回车不许裸进 XML");
    Check(xml2.find(L'\x01') == std::wstring::npos, "控制字符要被丢掉");
    Check(xml2.find(L'a') != std::wstring::npos && xml2.find(L'e') != std::wstring::npos,
          "丢掉非法字符不许把整段文字一起吞掉");

    // 结构与槽位：标题行 + 最多两行正文 + 一行小字，第四行不会被写成"多一行"。
    const std::wstring xml3 = RenderToastXml(MakeTargetNotifyPayload(MakeImage(),
                                                                    NotifyDetail::kTitle, L"一句"));
    size_t texts = 0;
    for (size_t pos = xml3.find(L"<text"); pos != std::wstring::npos; pos = xml3.find(L"<text", pos + 1))
        ++texts;
    Check(texts == 4, "标题 1 + 正文槽 3，多的一律不写进 XML");
    Check(xml3.find(L"placement=\"attribution\"") != std::wstring::npos, "最后一行走小字那一格");
    Check(xml3.find(L"template=\"ToastGeneric\"") != std::wstring::npos, "用的是通用模板");

    // 没有附图时整个 image 元素不出现（历史关了那一路不该留下一个空 src）。
    CapturedImage noimg = MakeImage();
    noimg.history.state = HistoryRecord::State::kDisabled;
    noimg.history.file.clear();
    Check(RenderToastXml(MakeTargetNotifyPayload(noimg, NotifyDetail::kTitle, L"")).find(L"<image") ==
              std::wstring::npos,
          "没图时不许留一个空 image 元素");
}

// ---------------------------------------------------------------------------
// 7. 编排与归类：四种下场各说一件事，且都不许动交付事实
// ---------------------------------------------------------------------------
void TestOrchestration() {
    std::printf("[7] 编排与归类\n");
    SetLanguage(Language::kEn);

    std::vector<CapturedImage> images(3, MakeImage());
    images[2].history.state = HistoryRecord::State::kDisabled;   // 第三张没有副本
    images[2].history.file.clear();

    // 每目标一条：三条，其中带图两条。
    std::vector<NotifyPayload> seen;
    NotifyOutcome ok = SendCaptureNotifications(
        images, NotifyMode::kPerTarget, NotifyDetail::kTitle, L"",
        [&](const NotifyPayload& p, HRESULT* hr) { seen.push_back(p); (void)hr; return true; });
    Check(ok.status == NotifyStatus::kSubmitted, "全部交出去就是 submitted");
    Check(ok.submitted == 3, "每目标一条时是三条");
    Check(ok.withImage == 2, "带图那两条按实际算");
    Check(ok.failed == 0 && ok.lastError == 0, "全成功时不留失败痕迹");
    Check(ok.note.code.empty(), "全成功时不附一条解释");
    Check(seen.size() == 3, "发送器真被叫了三次");

    // 合并成一条：一次，且带不带的图由第一个有副本的目标决定。
    seen.clear();
    NotifyOutcome m = SendCaptureNotifications(
        images, NotifyMode::kMerged, NotifyDetail::kTitle, L"",
        [&](const NotifyPayload& p, HRESULT* hr) { seen.push_back(p); (void)hr; return true; });
    Check(m.submitted == 1 && seen.size() == 1, "合并就是一条");
    Check(!seen.empty() && !seen[0].image.empty(), "合并那条仍取第一个有副本的图");

    // 部分失败：submitted 只数真交出去的，而失败那条与它的码照实留。
    int calls = 0;
    NotifyOutcome part = SendCaptureNotifications(
        images, NotifyMode::kPerTarget, NotifyDetail::kMinimal, L"",
        [&](const NotifyPayload&, HRESULT* hr) {
            ++calls;
            if (calls == 2) { *hr = static_cast<HRESULT>(0x80070005L); return false; }
            *hr = 0;
            return true;
        });
    Check(part.status == NotifyStatus::kSubmitted, "有一条没出去也仍算交出去了那些");
    Check(part.submitted == 2 && part.failed == 1, "两条成一条败要分开数");
    Check(part.lastError == static_cast<HRESULT>(0x80070005L), "留住那一次自己的码");
    Check(part.note.code == L"notify.send_failed", "试了而被拒归 send_failed");
    Check(part.note.stage == L"notify", "stage 说的是通知那一步");
    Check(part.note.hresult == L"0x80070005", "码要原样进诊断，不折成别的");

    // 全部失败但有码：仍然是 send_failed，而不是"这条路说不通"。
    NotifyOutcome all = SendCaptureNotifications(
        images, NotifyMode::kPerTarget, NotifyDetail::kMinimal, L"",
        [](const NotifyPayload&, HRESULT* hr) { *hr = static_cast<HRESULT>(0x80070057L); return false; });
    Check(all.status == NotifyStatus::kFailed, "一条都没出去是 failed");
    Check(all.submitted == 0 && all.failed == 3, "三条都算失败");
    Check(all.note.code == L"notify.send_failed", "有码就归 send_failed");

    // 一条都没出去而且**拿不出任何码**：那是"这条路说不通"，与"系统拒了"的下一步不同。
    NotifyOutcome blind = SendCaptureNotifications(
        images, NotifyMode::kPerTarget, NotifyDetail::kMinimal, L"",
        [](const NotifyPayload&, HRESULT* hr) { *hr = 0; return false; });
    Check(blind.status == NotifyStatus::kFailed, "全失败");
    Check(blind.note.code == L"notify.unavailable", "拿不出码时归 unavailable，不编一个 E_FAIL");
    Check(blind.note.hresult.empty(), "没有码就不许写一个假的码进诊断");

    // 零交付：那一步根本没发生，发送器一次都不该被叫（人拒绝时不许"告知截好了"）。
    int untouched = 0;
    NotifyOutcome empty = SendCaptureNotifications(
        {}, NotifyMode::kPerTarget, NotifyDetail::kTitle, L"",
        [&](const NotifyPayload&, HRESULT* hr) { ++untouched; (void)hr; return true; });
    Check(empty.status == NotifyStatus::kNone, "零交付时这一步没开始");
    Check(untouched == 0, "一张都没落地就一条都不发");
    Check(empty.note.code.empty(), "没开始过就不附失败解释");

    // 交付事实不许被这一层的成败改动：同一份 images 在发送前后逐字段相同。
    std::vector<CapturedImage> before = images;
    SendCaptureNotifications(images, NotifyMode::kPerTarget, NotifyDetail::kTitle, L"",
                             [](const NotifyPayload&, HRESULT* hr) {
                                 *hr = static_cast<HRESULT>(0x80004005L);
                                 return false;
                             });
    Check(before.size() == images.size(), "发送失败不增删交付记录");
    bool same = true;
    for (size_t i = 0; i < images.size(); ++i) {
        if (images[i].file != before[i].file || images[i].title != before[i].title ||
            images[i].history.state != before[i].history.state ||
            images[i].history.file != before[i].history.file ||
            images[i].bytes != before[i].bytes)
            same = false;
    }
    Check(same, "通知送不出去也不许回头改交付事实");
}

// ---------------------------------------------------------------------------
// 8. 机器名：JSON 与 --show-config 读的那几份词
// ---------------------------------------------------------------------------
void TestNames() {
    std::printf("[8] 机器名\n");
    Check(std::wstring(NotifyStatusName(NotifyStatus::kSubmitted, false)) == L"submitted",
          "submitted 说的是交给平台");
    Check(std::wstring(NotifyStatusName(NotifyStatus::kFailed, false)) == L"failed", "failed");
    // kNone 的两种成因必须给出两个词：把"用户关掉了"读成"没东西可报"会诱导人重拍一次。
    Check(std::wstring(NotifyStatusName(NotifyStatus::kNone, true)) == L"disabled", "关闭叫 disabled");
    Check(std::wstring(NotifyStatusName(NotifyStatus::kNone, false)) == L"no_delivery",
          "零交付叫 no_delivery");
    Check(std::wstring(NotifyModeName(NotifyMode::kPerTarget)) == L"per_target", "per_target");
    Check(std::wstring(NotifyModeName(NotifyMode::kMerged)) == L"merged", "merged");
    Check(std::wstring(NotifyDetailName(NotifyDetail::kTitle)) == L"title", "title");
    Check(std::wstring(NotifyDetailName(NotifyDetail::kBasic)) == L"basic", "basic");
    Check(std::wstring(NotifyDetailName(NotifyDetail::kMinimal)) == L"minimal", "minimal");
}

}  // namespace

int main() {
    std::printf("== notification state checks ==\n");
    TestDetailLevels();
    TestLandingAndImage();
    TestOwnerFallbackAndLength();
    TestPurpose();
    TestMerged();
    TestXmlEscaping();
    TestOrchestration();
    TestNames();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
