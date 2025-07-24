// 运行环境能力判据的离线判据（由 tests\compat.ps1 运行 build\ecapture-compat-tests.exe）。
//
// 为什么单独一个可执行文件：这一批判据要的是"这台机器上的 Windows 版本是 X"那个现场，
// 而版本没法在一台真机上改出来（降不了级，也不该为测试去装别的系统）。所以把判据本体
// （src/SystemCompat.cpp 的那三个纯函数）逐条注入假版本判：哪一道门槛在哪一个内部版本上
// 跨过、显式指定的通道被挡时绝不换成别的、auto 链少的是哪一条、版本问不出来时不许瞎筛。
// 真机那层（tests\compat.ps1 的第二层）判的是另一件事：本机探测到的版本确实是系统真正
// 那份，以及本机这次一条通道都没被挡下。
//
// 只用 C 风格的 printf 汇报；任何一条不过就返回非 0。
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/Lang.h"
#include "../src/SystemCompat.h"

using namespace ecapture;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL  %s\n", what);
    } else {
        std::printf("  PASS  %s\n", what);
    }
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

std::string Narrow(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out.push_back(static_cast<char>(c > 0 && c < 128 ? c : '?'));
    return out;
}

// 一个已知版本。判据只读 build 那一个数，major/minor 写在这里只是为了让用例读得懂。
OsVersion Ver(uint32_t major, uint32_t minor, uint32_t build) {
    OsVersion v;
    v.major = major;
    v.minor = minor;
    v.build = build;
    v.known = true;
    return v;
}

// "没能问出来"那一份：三个数全空，且 known=false。
OsVersion Unknown() { return OsVersion(); }

const wchar_t* ChannelOf(CaptureMethod m) { return CaptureMethodName(m); }

bool ChainIs(const std::vector<CaptureMethod>& chain, std::initializer_list<const wchar_t*> names) {
    if (chain.size() != names.size()) return false;
    size_t i = 0;
    for (const wchar_t* name : names) {
        if (std::wcscmp(ChannelOf(chain[i]), name) != 0) return false;
        ++i;
    }
    return true;
}

std::wstring ChainText(const std::vector<CaptureMethod>& chain) {
    std::wstring s = L"[";
    for (const CaptureMethod m : chain) {
        if (s.size() > 1) s += L",";
        s += CaptureMethodName(m);
    }
    return s + L"]";
}

// 注了几条、注的是哪几条通道（跳过的那几条按通道名核对，不靠文案措辞）
bool HasNoteOn(const std::vector<Diagnostic>& notes, const wchar_t* code, const wchar_t* channel) {
    for (const Diagnostic& d : notes) {
        if (d.code == code && (!channel || d.value == channel)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 那几条下限各自跨过去的版本：逐条对到边界两边，差一个内部版本就得换结论。
// ---------------------------------------------------------------------------
void TestFloors() {
    Section("每条能力各自那道版本门槛（边界两边各判一次）");

    // 整工具下限：Windows 10 10240（唯一那套编码器）。
    Check(AssessRuntime(Ver(6, 3, 9600)).support == Support::kBelow,
          "Windows 8.1 (9600)：整工具判为版本不够");
    Check(AssessRuntime(Ver(10, 0, 10239)).support == Support::kBelow,
          "10239：仍判为不够（下限不是四舍五入）");
    Check(AssessRuntime(Ver(10, 0, os_floor::kEncoder)).support == Support::kOk,
          "10240：整工具可以开工（下限含等于）");

    // WGC：按窗口与按屏那两条互操作接口都是 18362，不是命名空间本身的 17134。
    Check(os_floor::kWgc == 18362u, "WGC 的下限写的是 18362（Windows 10 版本 1903）");
    Check(AssessChannel(CaptureMethod::kWgc, false, Ver(10, 0, 18361)).support == Support::kBelow,
          "18361：wgc 窗口路线判为不够（这就是 README 旧版写成 1803 的那个坑）");
    Check(AssessChannel(CaptureMethod::kWgc, true, Ver(10, 0, 18361)).support == Support::kBelow,
          "18361：wgc 屏幕路线同样不够（CreateForMonitor 与 CreateForWindow 同期）");
    Check(AssessChannel(CaptureMethod::kWgc, false, Ver(10, 0, 18362)).support == Support::kOk,
          "18362：wgc 可用");
    Check(AssessChannel(CaptureMethod::kWgc, false, Ver(10, 0, 17134)).support == Support::kBelow,
          "17134（1803，命名空间刚出现）：wgc 仍判为不够");

    // 桌面复制：Windows 8 的 DXGI 1.2。
    Check(AssessChannel(CaptureMethod::kDuplication, false, Ver(6, 2, 9199)).support == Support::kBelow,
          "9199：duplication 判为不够");
    Check(AssessChannel(CaptureMethod::kDuplication, false, Ver(6, 2, 9200)).support == Support::kOk,
          "9200：duplication 可用");

    // PrintWindow 与 dwm 的读回：受 PW_RENDERFULLCONTENT 那个 flag 约束（8.1）。
    Check(AssessChannel(CaptureMethod::kPrintWindow, false, Ver(6, 2, 9599)).support == Support::kBelow,
          "9599（那道门槛前一个内部版本）：printwindow 判为不够");
    Check(AssessChannel(CaptureMethod::kPrintWindow, false, Ver(6, 3, 9600)).support == Support::kOk,
          "9600（8.1）：printwindow 可用");
    Check(AssessChannel(CaptureMethod::kDwmThumbnail, false, Ver(6, 3, 9600)).support == Support::kOk,
          "9600：dwm 可用（它注册缩略图更早，但读回靠同一个 flag）");
    Check(AssessChannel(CaptureMethod::kDwmThumbnail, false, Ver(6, 1, 7601)).support == Support::kBelow,
          "7601（Windows 7）：dwm 判为不够，尽管 DwmRegisterThumbnail 本身在那时有");

    // bitblt 是唯一没有版本门槛的一条：整条回退链因此永不为空。
    Check(AssessChannel(CaptureMethod::kBitBlt, false, Ver(6, 1, 7601)).support == Support::kOk,
          "7601：bitblt 没有版本门槛");
    Check(AssessChannel(CaptureMethod::kBitBlt, true, Unknown()).support == Support::kOk,
          "版本问不出来：bitblt 也照样判可用（它没有需要查的那道门槛）");

    // 主版本号更高的未来系统：只比内部版本，所以不会因 major 更小就被误判。
    Check(AssessChannel(CaptureMethod::kWgc, false, Ver(11, 0, 26100)).support == Support::kOk,
          "26100：比下限高的版本一律放行");
}

// ---------------------------------------------------------------------------
// 版本问不出来时的行为：不筛、不报，但要说清"这一次没按版本判过"。
// ---------------------------------------------------------------------------
void TestUnknownVersion() {
    Section("问不出来本机版本：绝不拿它当不支持，也不拿它当支持");

    const ChannelGate w = GateChannels(CaptureMethod::kAuto, false, Unknown());
    Check(w.error.code.empty(), "问不出来时不报环境错误");
    Check(ChainIs(w.chain, {L"wgc", L"dwm", L"printwindow", L"bitblt"}),
          "问不出来时窗口模式的链原样不动（没有按版本筛过）");
    Check(HasNoteOn(w.notes, codes::kNoteOsUnverifiable, nullptr),
          "问不出来时留一条 note.os_unverifiable，让调用方知道这一趟没判过");

    const ChannelGate s = GateChannels(CaptureMethod::kAuto, true, Unknown());
    Check(ChainIs(s.chain, {L"wgc", L"duplication", L"bitblt"}), "屏幕模式的链同样原样不动");

    // 显式指定一条时也一样：链就是那一条，绝不因为"不知道行不行"而换成别的。
    const ChannelGate e = GateChannels(CaptureMethod::kWgc, false, Unknown());
    Check(e.error.code.empty() && ChainIs(e.chain, {L"wgc"}),
          "问不出来 + 显式 wgc：仍按用户要的那一条，不报错也不改道");
    Check(HasNoteOn(e.notes, codes::kNoteOsUnverifiable, nullptr),
          "显式指定时这条提示同样要给");

    // 整工具那一关也同理：没问到版本就不宣称这台机器不能干活。
    Check(EnvironmentError(Unknown()).code.empty(),
          "问不出来时不报 env.os_too_old（问不出来不等于不支持）");
}

// ---------------------------------------------------------------------------
// 显式指定的通道：不可用就是不可用，绝不替用户换一条。
// ---------------------------------------------------------------------------
void TestExplicitChannel() {
    Section("显式 --capture 的那一条被版本挡下时");

    const ChannelGate g = GateChannels(CaptureMethod::kWgc, false, Ver(10, 0, 14393));
    Check(g.chain.empty(), "链是空的：不会偷偷改走 dwm 或 bitblt");
    Check(g.error.code == codes::kEnvChannelUnsupported, "报 env.channel_unsupported");
    Check(g.error.stage == stages::kCapture, "stage 是 capture（这件事出在取图那一步之前）");
    Check(g.error.option == L"--capture", "option 指回 --capture");
    Check(g.error.value == L"wgc" && g.error.backend == L"wgc",
          "value 与 backend 都写被挡下的那条通道名");
    Check(g.error.target.empty(), "这是本机版本的事，不属于哪个目标，所以 target 空着");
    Check(!g.error.message.empty() && g.error.message.front() != L'?',
          ("message 取到了文案：" + Narrow(g.error.message)).c_str());
    Check(g.error.message.find(L"18362") != std::wstring::npos &&
              g.error.message.find(L"14393") != std::wstring::npos,
          "message 里同时有那条门槛的数字与本机实际的数字");
    Check(!g.error.hint.empty() && g.error.hint.front() != L'?',
          "hint 也取到了文案（换通道才有条下一步）");

    // 可用的那几条不许多嘴报错。
    const ChannelGate ok = GateChannels(CaptureMethod::kDuplication, false, Ver(10, 0, 10240));
    Check(ok.error.code.empty() && ChainIs(ok.chain, {L"duplication"}),
          "10240 上显式 duplication 在其下限之上：放行且不报错");
    const ChannelGate pw = GateChannels(CaptureMethod::kPrintWindow, false, Ver(10, 0, 10240));
    Check(pw.error.code.empty() && ChainIs(pw.chain, {L"printwindow"}),
          "10240 上显式 printwindow：放行");
    const ChannelGate blocked = GateChannels(CaptureMethod::kPrintWindow, false, Ver(6, 2, 9200));
    Check(blocked.error.code == codes::kEnvChannelUnsupported && blocked.chain.empty(),
          "9200 上显式 printwindow：挡下（printwindow 自己要 9600）");
}

// ---------------------------------------------------------------------------
// auto 的链：被挡下的那几条从链里去掉并留提示，其余照旧回退。
// ---------------------------------------------------------------------------
void TestAutoChain() {
    Section("auto 回退链按本机版本筛");

    // 正常机器（>=18362）：整条链一条不少，也不该有任何提示。
    const ChannelGate modern = GateChannels(CaptureMethod::kAuto, false, Ver(10, 0, 19045));
    Check(ChainIs(modern.chain, {L"wgc", L"dwm", L"printwindow", L"bitblt"}),
          "19045：窗口模式四条全在链里");
    Check(modern.notes.empty(), "19045：一条提示都不该有（没有东西被跳过）");
    Check(modern.error.code.empty(), "19045：不报环境错误");

    // 18361：只有 wgc 那条被挡下，其余三条照旧。
    const ChannelGate oldWin10 = GateChannels(CaptureMethod::kAuto, false, Ver(10, 0, 17763));
    Check(ChainIs(oldWin10.chain, {L"dwm", L"printwindow", L"bitblt"}),
          ("17763：链里去掉 wgc，剩下 " + Narrow(ChainText(oldWin10.chain))).c_str());
    Check(oldWin10.notes.size() == 1 &&
              HasNoteOn(oldWin10.notes, codes::kNoteChannelUnavailable, L"wgc"),
          "17763：只留一条 note.channel_unavailable，且指名是 wgc");
    Check(oldWin10.error.code.empty(), "17763：还能截，所以不报错误");

    // 10240：wgc 被挡，其余三条都在自己的下限之上。
    const ChannelGate rtm = GateChannels(CaptureMethod::kAuto, false, Ver(10, 0, 10240));
    Check(ChainIs(rtm.chain, {L"dwm", L"printwindow", L"bitblt"}), "10240：链同样是后三条");

    // 9599：dwm 与 printwindow 都卡在 9600 那道门槛下，wgc 更早，只剩没有门槛的 bitblt。
    const ChannelGate win81 = GateChannels(CaptureMethod::kAuto, false, Ver(6, 2, 9599));
    Check(ChainIs(win81.chain, {L"bitblt"}), "9599：只剩 bitblt 那一条");
    Check(win81.notes.size() == 3, "9599：被跳过的三条各留一条提示");
    Check(win81.error.code.empty(), "9599：还剩一条能用，所以不报错误");

    // 屏幕模式：链是 wgc -> duplication -> bitblt，与窗口模式不是同一份顺序。
    const ChannelGate screen = GateChannels(CaptureMethod::kAuto, true, Ver(10, 0, 17763));
    Check(ChainIs(screen.chain, {L"duplication", L"bitblt"}), "屏幕模式 17763：去掉 wgc 剩两条");
    Check(ChainIs(AutoChain(true), {L"wgc", L"duplication", L"bitblt"}),
          "屏幕模式那份未筛的链是 wgc -> duplication -> bitblt");
    Check(ChainIs(AutoChain(false), {L"wgc", L"dwm", L"printwindow", L"bitblt"}),
          "窗口模式那份未筛的链是 wgc -> dwm -> printwindow -> bitblt");

    // 无论哪一份链、哪一个版本，bitblt 永远在链里 —— 这是"auto 的链永不为空"那条不变量。
    const uint32_t probes[] = {9200u, 9600u, 10240u, 14393u, 17763u, 18362u, 26100u};
    bool bitbltAlways = true;
    for (const uint32_t b : probes) {
        for (const bool scr : {false, true}) {
            const ChannelGate g = GateChannels(CaptureMethod::kAuto, scr, Ver(10, 0, b));
            if (!g.chain.empty() &&
                std::any_of(g.chain.begin(), g.chain.end(),
                            [](CaptureMethod m) { return m == CaptureMethod::kBitBlt; })) {
                continue;
            }
            bitbltAlways = false;
        }
    }
    Check(bitbltAlways, "各档版本、两种模式下 bitblt 都在链里（所以 auto 的链永不为空）");
}

// ---------------------------------------------------------------------------
// 整工具那一关：编码器不在这台机器上，换哪条通道都没用。
// ---------------------------------------------------------------------------
void TestRuntimeFloor() {
    Section("整工具下限（唯一那套编码器）");

    const Diagnostic below = EnvironmentError(Ver(6, 3, 9600));
    Check(below.code == codes::kEnvOsTooOld, "9600 报 env.os_too_old");
    Check(below.stage == stages::kCapture, "stage 是 capture");
    Check(below.option == L"--capture", "option 指到 --capture（这条与它选哪条无关，见 hint）");
    Check(below.message.find(L"9600") != std::wstring::npos &&
              below.message.find(std::to_wstring(os_floor::kEncoder)) != std::wstring::npos,
          "message 里两个数字都在：本机实际的那份与要求的下限");
    Check(below.backend.empty(), "这条与具体某条通道无关，所以 backend 空着");
    Check(below.value == L"9600", "value 写本机那个版本号，调用方不必读 message");
    Check(!below.hint.empty() && below.hint.front() != L'?', "hint 取到了文案");

    Check(EnvironmentError(Ver(10, 0, 10240)).code.empty(), "10240 起整工具可以开工");
    Check(EnvironmentError(Ver(10, 0, 19045)).code.empty(), "19045 不报");
}

// ---------------------------------------------------------------------------
// 四语文案：这几条码的 message 与 hint 必须在四种语言里都取得到，且占位符都代进去了。
// ---------------------------------------------------------------------------
void TestStrings() {
    Section("四语文案");

    const Language langs[] = {Language::kZhCn, Language::kZhTw, Language::kEn, Language::kJa};
    for (const Language lang : langs) {
        SetLanguage(lang);
        const std::string tag = Narrow(LanguageTag(lang));
        const wchar_t* keys[] = {L"env.os_too_old",         L"env.os_too_old_hint",
                                 L"env.channel_unsupported", L"env.channel_unsupported_hint",
                                 L"note.channel_unavailable", L"note.os_unverifiable",
                                 L"help.system"};
        for (const wchar_t* key : keys) {
            const std::wstring text = Msg(key);
            Check(!text.empty() && text.front() != L'?',
                  (tag + " 取得到 " + Narrow(key)).c_str());
        }
        // 三条带占位符的都要代得进去，且数字原样出现（调用方靠它核对门槛）
        const Diagnostic e = EnvironmentError(Ver(6, 3, 9600));
        Check(e.message.find(L'%') == std::wstring::npos && e.message.find(L"9600") != std::wstring::npos,
              (tag + " 的 env.os_too_old 没有剩下占位符").c_str());
        const Diagnostic c = GateChannels(CaptureMethod::kWgc, false, Ver(10, 0, 17763)).error;
        Check(c.message.find(L'%') == std::wstring::npos &&
                  c.message.find(L"18362") != std::wstring::npos &&
                  c.message.find(L"17763") != std::wstring::npos &&
                  c.message.find(L"wgc") != std::wstring::npos,
              (tag + " 的 env.channel_unsupported 把通道名与两个数字都代进去了").c_str());
        const ChannelGate g = GateChannels(CaptureMethod::kAuto, false, Ver(10, 0, 17763));
        Check(g.notes.size() == 1 && g.notes[0].message.find(L'%') == std::wstring::npos &&
                  g.notes[0].message.find(L"wgc") != std::wstring::npos,
              (tag + " 的 note.channel_unavailable 代得干净").c_str());
        // 提示文字里不许有没代进去的占位符（EnvBase 用 Msg 取 hint，本来就没给参数）
        Check(c.hint.find(L'%') == std::wstring::npos,
              (tag + " 的 hint 里没有占位符（它取的时候没带参数）").c_str());
    }
    SetLanguage(Language::kEn);
}

}  // namespace

int main() {
    // 判据只看 code / stage / 数字，文案跟着英语那份走（与其余 *_state.cpp 同一约定）
    SetLanguage(Language::kEn);

    TestFloors();
    TestUnknownVersion();
    TestExplicitChannel();
    TestAutoChain();
    TestRuntimeFloor();
    TestStrings();

    std::printf("\ncompat-state: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
