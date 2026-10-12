// 命令行文本处理与多语言资源的离线判据（Windows：build\ecapture-cli-text-tests.exe，
// Linux：build-linux/<编译器>-<配置>/ecapture-cli-text-tests）。
//
// 判的是**生产本体**，不是测试里另抄的一份规则：
//   * src/core/TextCoding.cpp —— UTF-8 编解码与 ASCII 判定；
//   * src/Lang.cpp + 构建期生成的嵌入文案 —— 四语查表、回退、占位符、语言标签；
//   * src/CliOptions.cpp —— 参数解析、区间、互斥、配置合并与"本平台不成立的那几条说法"；
//   * src/Config.cpp + src/HostFile.h + src/HostPath.h —— 真文件的读取与路径形状；
//   * src/CliText.cpp + src/Json.cpp —— --help / --version / --show-config 与 JSON 转义；
//   * src/OutputStream.h 的平台实现 —— 通道的归属纪律（图片占用 stdout 后文字必须改道）。
//
// 这个文件一条都不 include 平台头：入口那一段怎么把系统 argv 换成 CliTokens，由
// tests/platform/ 下与本平台对应的那一份补充判据来判（Windows 一份、Linux 一份）。
// 只用合成数据与本次自建的临时文件，不碰仓库、不碰任何用户配置，也不取一个像素。
#include <cstdio>
#include <cstdint>
#include <system_error>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <vector>

#include "../src/CliOptions.h"
#include "../src/CliArgs.h"
#include "../src/Report.h"
#include "../src/Config.h"
#include "../src/HostFile.h"
#include "../src/HostPath.h"
#include "../src/Json.h"
#include "../src/Lang.h"
#include "../src/LangData.h"
#include "../src/OutputStream.h"
#include "../src/core/CliTokens.h"
#include "../src/core/Platform.h"
#include "../src/core/TextCoding.h"
#include "CliTextPlatformChecks.h"

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
    std::fflush(stdout);  // 崩溃时最后一条用例不会被缓冲吞掉
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

std::wstring W(const char* ascii) { return DecodeUtf8(ascii, std::string(ascii).size()).text; }

bool HasError(const ParseResult& r, const wchar_t* code) {
    for (const Diagnostic& d : r.errors) {
        if (d.code == code) return true;
    }
    return false;
}
const Diagnostic* FindError(const ParseResult& r, const wchar_t* code) {
    for (const Diagnostic& d : r.errors) {
        if (d.code == code) return &d;
    }
    return nullptr;
}
size_t CountErrors(const ParseResult& r, const wchar_t* code) {
    size_t n = 0;
    for (const Diagnostic& d : r.errors) {
        if (d.code == code) ++n;
    }
    return n;
}
const wchar_t* SourceOf(const Options& opt, const wchar_t* id) {
    for (const Options::ResolvedSetting& s : opt.settings) {
        if (s.id == id) return s.source.c_str();
    }
    return L"<missing>";
}

// 一条命令：程序名 + 若干个参数（参数用 UTF-8 的 char* 写，进来先按严格 UTF-8 解码一次 ——
// 与 Linux 入口同一件事，所以用例里的中文与空格就是真会被解析成文本的那一串字节）。
CliTokens Cmd(std::vector<std::string> args) {
    CliTokens tokens;
    tokens.PushBack(L"ecapture");
    for (const std::string& a : args) {
        const Utf8Decoded d = DecodeUtf8(a.data(), a.size());
        tokens.PushBack(d.text, d.WellFormed() ? std::wstring::npos : d.firstBadOffset);
    }
    return tokens;
}

ParseResult Run(std::vector<std::string> args, Language lang = Language::kEn) {
    SetLanguage(lang);  // 语言由 --lang 那一路自己会再定一次；这里只是把起点固定下来
    return ParseCommandLine(Cmd(std::move(args)));
}

// ---------------------------------------------------------------------------
// 临时文件：只建在本次自定的名字上，用完就删；不碰仓库，也不碰 ECAPTURE.ini
// ---------------------------------------------------------------------------

// 路径从本平台的原生表示换成内部文字：Windows 的 native() 本来就是 wstring（UTF-16，
// 与内部约定同一种，原样交回），POSIX 的 native() 是字节串，那里才需要按 UTF-8 解一次。
// 用两个重载而不是 if constexpr：非模板函数里 if constexpr 的两个分支都照样做类型检查，
// 编不过的那一支照样编不过。
std::wstring FromNativePath(const std::wstring& native) { return native; }
std::wstring FromNativePath(const std::string& native) {
    return DecodeUtf8(native.data(), native.size()).text;
}

std::wstring TempDir() {
    // 用 std::filesystem 而不是 getenv("TMPDIR"/"TEMP")：后者在 MSVC 上是那条 C4996，
    // 而两个平台都真有这么一个"临时目录"的问题要问，标准库给的答案就是本平台的正确答案。
    // 问不出来（或路径里带着本平台表示不了的字节）就退回 "/tmp" —— 这是本判据程序自己的
    // 临时文件位置，不是产品行为，退回去也只影响这次运行能不能建出那份 INI。
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::temp_directory_path(ec);
    if (ec) return L"/tmp";
    return FromNativePath(p.native());
}

std::wstring TempFile(const char* leaf) {
    return JoinDirAndFile(TempDir(), W(leaf));
}

bool WriteTemp(const std::wstring& path, const std::string& bytes) {
    // 临时目录本身按 ASCII 处理（三条环境变量与 /tmp 都是 ASCII 的现实），
    // 所以这里用窄字节打开；路径里真有非 ASCII 时这条用例只是建不出来，不影响产品代码。
    std::ofstream out(EncodeUtf8(path).c_str(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

void RemoveTemp(const std::wstring& path) { std::remove(EncodeUtf8(path).c_str()); }

}  // namespace

// ---------------------------------------------------------------------------
// 判据分段：每段一个主题，函数在 ecapture 命名空间外，但只用生产接口
// ---------------------------------------------------------------------------
namespace {

void CheckTextCoding() {
    Section("文本编码：UTF-8 严格解码");
    {
        const auto d = DecodeUtf8("abc", 3);
        Check(d.WellFormed() && d.text == L"abc", "纯 ASCII 整份合");
    }
    {
        const auto d = DecodeUtf8("\xe4\xb8\xad\xe6\x96\x87", 6);  // 中文
        Check(d.WellFormed() && d.text.size() == 2 && d.text[0] == 0x4E2D,
              "三字节序列解成两个码位");
    }
    {
        const auto d = DecodeUtf8("a\xf0\x9f\x98\x80" "b", 6);  // a + U+1F600 + b
        Check(d.WellFormed() && CodePointCount(d.text) == 3, "非 BMP 字符算一个码位");
        Check(EncodeUtf8(d.text) == "a\xf0\x9f\x98\x80" "b", "非 BMP 原样编回去");
    }
    // 每一段坏字节都只算一处，且第一处的偏移要说得准
    struct Bad {
        const char* bytes;
        size_t size;
        size_t groups;
        size_t firstOffset;
        const char* what;
    };
    const Bad bads[] = {
        // C0 / C1 从来不是合法首字节（它们只能起过长编码），所以 C0 与跟在它后面的 80
        // 各算一段：前者"首字节不合"，后者"落单的 continuation 字节"。
        {"\xc0\x80", 2, 2, 0, "过长编码的首字节 C0 被拒，其后那个续字节也算独立一段"},
        {"\xed\xa0\x80", 3, 1, 0, "代理区码位（ED A0 80 = U+D800）被拒"},
        {"\xf4\x90\x80\x80", 4, 1, 0, "超过 U+10FFFF（F4 90..）被拒"},
        {"\xe4\xb8", 2, 1, 0, "尾部残缺按一段拒，不把两个字节各报一次"},
        {"\xe4\x28\x80", 3, 2, 1, "续字节位置撞上 ASCII：坏在那一个字节，之后重新起头"},
        {"\x80", 1, 1, 0, "落单的 continuation 字节被拒"},
        {"\xff", 1, 1, 0, "0xFF 这种从不合法的首字节被拒"},
        {"a\xff" "b", 3, 1, 1, "坏段之前的正常内容不丢"},
    };
    for (const Bad& b : bads) {
        const auto d = DecodeUtf8(b.bytes, b.size);
        const bool offsets = d.invalidGroups == b.groups && d.firstBadOffset == b.firstOffset;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s（段数=%zu 首处偏移=%zu）", b.what, d.invalidGroups,
                      d.firstBadOffset);
        Check(!d.WellFormed() && offsets, buf);
    }
    Check(DecodeUtf8("a\xff" "b", 3).text.find(L'\xFFFD') != std::wstring::npos,
          "坏段留下 U+FFFD 这个看得见的痕迹");

    Section("文本编码：整份合或整份拒（配置文件那一路）");
    {
        const std::vector<uint8_t> good{'#', ' ', 'a', '=', '1', '\n'};
        const auto s = DecodeUtf8Strict(good, true);
        Check(s.ok && s.text == W("# a=1\n"), "合的整份交出");
    }
    {
        const std::vector<uint8_t> over{0xE0, 0x80, 0x80};
        const auto s = DecodeUtf8Strict(over, true);
        Check(!s.ok && s.badOffset == 0 && s.text.empty(), "过长编码整份拒且不交半份文本");
    }
    {
        const std::vector<uint8_t> nul{'a', 0x00, 'b'};
        Check(!DecodeUtf8Strict(nul, true).ok, "NUL 在这条路上算不可用字节");
        Check(!DecodeUtf8Strict(nul, false).ok == false, "同一条扫描器：不当 NUL 为坏时它仍然是合的");
    }
    {
        const std::vector<uint8_t> tail{'a', 0xE4, 0xB8};
        const auto s = DecodeUtf8Strict(tail, true);
        Check(!s.ok && s.badOffset == 1, "残缺的尾部报在序列起点");
    }

    Section("文本编码：出口编码与码位数");
    {
        std::wstring wide = W("标题");
        wide.push_back(static_cast<wchar_t>(0x1F600));  // 一个码位：两个平台各自表示
        const std::string out = EncodeUtf8(wide);
        Check(IsWellFormedUtf8(out.data(), out.size()), "交出去的字节恒为合法 UTF-8");
        Check(out == "\xe6\xa0\x87\xe9\xa2\x98" "\xf0\x9f\x98\x80", "中文 + 非 BMP 编成预期字节");
    }
    {
        // 本平台表示不出来的码元/码位：换成 U+FFFD，绝不写出半个序列。
        std::wstring odd;
        if constexpr (sizeof(wchar_t) == 2) {
            odd.push_back(static_cast<wchar_t>(0xD800));  // 落单的高代理（Windows 才会遇到）
        } else {
            odd.push_back(static_cast<wchar_t>(0x110000));  // 超出 Unicode 范围（Linux 才会遇到）
        }
        const std::string out = EncodeUtf8(odd);
        Check(out == "\xef\xbf\xbd", "无法表示的那一格换成 U+FFFD，不写半个序列");
    }
    {
        std::wstring t = W("ab");
        t.push_back(static_cast<wchar_t>(0x10000));
        Check(CodePointCount(t) == 3, "长度上限按码位数算，两个平台同一个数");
    }

    Section("ASCII 判定：不跟 locale 走");
    Check(AsciiLower(W("PNG--JPEG")) == W("png--jpeg"), "ASCII 字母被折叠");
    Check(AsciiLower(W("\xe4\xb8\xad")) == W("\xe4\xb8\xad"), "非 ASCII 文字不被改写");
    Check(!IsAsciiSpace(0x3000), "全角空格不算空白（取值合同的空白就是 ASCII 空白）");
    Check(IsAsciiDigit(L'7') && !IsAsciiDigit(0xFF11), "ASCII 数字判定不扩大到全角数字");
}

void CheckLanguage() {
    Section("多语言：四份嵌入物");
    const char* names[] = {"ECP_TEXT_ZH_CN", "ECP_TEXT_ZH_TW", "ECP_TEXT_EN", "ECP_TEXT_JA"};
    for (const char* name : names) {
        const LangBlob* blob = FindLangBlob(name);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s 已嵌进本次构建", name);
        Check(blob && blob->bytes && blob->size > 4096, buf);
        if (!blob) continue;
        const auto d = DecodeUtf8(blob->bytes, blob->size);
        std::snprintf(buf, sizeof(buf), "%s 整份是合法 UTF-8（坏段 %zu 处）", name,
                      d.invalidGroups);
        Check(d.WellFormed(), buf);
    }
    Check(FindLangBlob("ECP_TEXT_NO_SUCH") == nullptr, "没有嵌进来的名字交回 nullptr，不交空表");

    Section("多语言：查表与回退");
    const Language langs[] = {Language::kZhCn, Language::kZhTw, Language::kEn, Language::kJa};
    for (Language lang : langs) {
        SetLanguage(lang);
        const std::wstring msg = Msg(L"cli.invalid_utf8");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s：新加的那条码四份都有文案",
                      EncodeUtf8(LanguageTag(lang)).c_str());
        Check(!msg.empty() && msg[0] != L'?', buf);
    }
    SetLanguage(Language::kEn);
    Check(Msg(L"cli.invalid_utf8") != Msg(L"cli.internal_error"), "不同 key 交出不同句子");
    Check(Msg(L"no.such.key") == W("?no.such.key"), "写错的 key 留下 ?key 这个可见痕迹");
    Check(!HasMsg(L"no.such.key"), "HasMsg 不把缺失当成一句真文案");
    Check(CurrentLanguage() == Language::kEn, "CurrentLanguage 反映 SetLanguage 的结果");

    Section("多语言：占位符");
    {
        const std::vector<std::wstring> args{W("one"), W("两")};
        Check(detail::Substitute(W("a %1 b %2 c"), args) == W("a one b 两 c"),
              "%1/%2 按位置代入");
        // 文案里"字面量百分号写 %%"，代入后只剩一个 %；而文件名模板用的 %i / %h 不是占位符，
        // 必须原样留着（否则会悄悄吃掉用户的模板）。
        Check(detail::Substitute(W("100%% 模板 %i"), args) == W("100% 模板 %i"),
              "%% 出一个 %，%i 原样保留");
        Check(detail::Substitute(W("越界的 %3 留着"), args) == W("越界的 %3 留着"),
              "参数不够时 %3 不当占位符吃掉");
    }

    Section("多语言：标签判据");
    struct Case {
        const char* tag;
        int expected;  // -1 = 不认
    };
    const Case cases[] = {
        {"zh-CN", 0}, {"zh_CN", 0}, {"ZH-cn", 0}, {"chs", 0}, {"cn", 0}, {"zh-Hans", 0},
        {"zh-TW", 1}, {"zh_Hant", 1}, {"cht", 1}, {"tw", 1}, {"zh-HK", 1}, {"zh-MO", 1},
        {"en", 2}, {"en-US", 2}, {"EN", 2}, {"ja", 3}, {"ja_JP", 3}, {"jp", 3},
        {"fr", -1}, {"", -1}, {"_", -1}, {"zh", 0},
    };
    bool all = true;
    for (const Case& c : cases) {
        const auto got = LanguageFromTag(W(c.tag));
        const int want = c.expected;
        const bool ok = want < 0 ? !got : (got && static_cast<int>(*got) == want);
        if (!ok) {
            all = false;
            std::printf("        %s -> %s\n", c.tag,
                        got ? EncodeUtf8(LanguageTag(*got)).c_str() : "(不认)");
        }
    }
    Check(all, "标签写法与别名按既有规则识别，认不了的返回 nullopt（不静默换成英语）");

    Section("多语言：系统语言由平台服务回答");
    const Language detected = DetectSystemLanguage();
    const bool oneOf = detected == Language::kZhCn || detected == Language::kZhTw ||
                       detected == Language::kEn || detected == Language::kJa;
    Check(oneOf, "DetectSystemLanguage 交回四种之一（本机答案不作断言）");
}

void CheckParsing() {
    Section("参数解析：中文、空格与位置参数");
    {
        const auto r = Run({"--title", "夜 空", "out.png"});
        Check(r.ok, "带空格的中文标题解析通过");
        Check(r.options.match.titles.size() == 1 && r.options.match.titles[0] == W("夜 空"),
              "标题原文（含空格）逐字交回");
        Check(r.options.output == W("out.png"), "位置参数是输出路径");
    }
    {
        const auto r = Run({"--title-contains", "报告", "-o", "中文 目录.png"});
        Check(r.ok && r.options.match.titleContains.size() == 1 &&
                  r.options.match.titleContains[0] == W("报告"),
              "--title-contains 收中文");
    }
    {
        // 吃值选项把下一个参数消费掉，哪怕它长得像另一个选项（既有规矩）
        const auto r = Run({"--title", "--lang"});
        Check(r.ok && r.options.match.titles.size() == 1 &&
                  r.options.match.titles[0] == W("--lang"),
              "取值可以是一条长得像选项的文字");
    }
    {
        const auto r = Run({"--roi=0,0,12,9", "--title", "x"});
        Check(r.ok && r.options.crop.mode == CropMode::kRoi, "内联写法 --roi=x,y,w,h 成立");
    }
    {
        const auto r = Run({"--", "--title"});
        Check(r.ok && r.options.output == W("--title"), "裸 -- 之后的参数按位置处理");
    }

    Section("参数解析：区间、互斥与错写");
    {
        const auto r = Run({"--quality", "101", "--title", "x"});
        Check(!r.ok && HasError(r, codes::kInvalidNumber), "越界的 --quality 报无效数值");
    }
    {
        const auto r = Run({"--index", "0", "--title", "x"});
        Check(!r.ok && HasError(r, codes::kInvalidNumber), "--index 从 1 起");
    }
    {
        const auto r = Run({"--index", "1", "--all", "--title", "x"});
        Check(!r.ok && HasError(r, codes::kConflictingOptions), "选择策略互斥：--index 与 --all");
    }
    {
        const auto r = Run({"--capabilities", "--title", "x"});
        Check(!r.ok && HasError(r, codes::kQueryConflict), "只读查询与截图意图互斥");
    }
    {
        const auto r = Run({"--bogus"});
        Check(!r.ok && HasError(r, codes::kUnknownOption), "不认识的选项报 cli.unknown_option");
    }
    {
        const auto r = Run({"--title"});
        Check(!r.ok && HasError(r, codes::kMissingValue), "吃值的选项少了取值");
    }
    {
        const auto r = Run({"--lang", "klingon", "--title", "x"});
        Check(!r.ok && HasError(r, codes::kUnknownLanguage), "--lang 认不了的取值不静默换成英语");
    }
    {
        const auto r = Run({"--lang", "ja", "--lang", "bogus"});
        Check(CurrentLanguage() == Language::kJa,
              "重复 --lang 以最后一个**有效**的指定为准（坏的另报错）");
        Check(HasError(r, codes::kUnknownLanguage), "坏的那条照样报出来");
    }

    Section("参数解析：非法编码的参数逐条拒");
    {
        CliTokens tokens;
        tokens.PushBack(L"ecapture");
        tokens.PushBack(W("--title"), std::wstring::npos);
        // 假装这条的第 3 字节起不合（入口就是这么交回这种形状的）
        tokens.PushBack(W("标题"), 3);
        tokens.PushBack(L"out.png");
        const auto r = ParseCommandLine(tokens);
        Check(!r.ok && HasError(r, codes::kInvalidUtf8), "不合的那条报 cli.invalid_utf8");
        const Diagnostic* d = FindError(r, codes::kInvalidUtf8);
        Check(d && d->detail == W("byte:offset=3"), "detail 带上第一处不可用字节的偏移");
        Check(r.errors.size() == 1, "只报这一条，不再拿它生成第二种解释");
    }
    {
        CliTokens tokens;
        tokens.PushBack(L"ecapture");
        tokens.PushBack(W("a\xff" "b"), 1);
        tokens.PushBack(W("c\xff"), 1);
        const auto r = ParseCommandLine(tokens);
        Check(CountErrors(r, codes::kInvalidUtf8) == 2, "两条不合的参数各报一条，不合并");
    }
}

void CheckPlatformGate() {
    Section("平台边界：Windows 专属说法");
    const bool windows = BuildPlatform() == PlatformKind::kWindows;

    {
        const auto r = Run({"--hwnd", "0x10", "out.png"});
        if (windows) {
            Check(!HasError(r, codes::kPlatformUnsupported), "Windows 构建：--hwnd 照旧有效");
        } else {
            const Diagnostic* d = FindError(r, codes::kPlatformUnsupported);
            Check(d && d->option == W("--hwnd"), "非 Windows 构建：--hwnd 明确报不支持");
            Check(!r.ok, "非 Windows 构建：这一条不会静默忽略");
        }
    }
    {
        const auto r = Run({"--class", "Notepad", "out.png"});
        Check((HasError(r, codes::kPlatformUnsupported) != 0) == !windows,
              "--class 与 --hwnd 同一条线");
    }
    {
        const auto r = Run({"--capture", "wgc", "--title", "x"});
        Check((HasError(r, codes::kPlatformUnsupported) != 0) == !windows,
              "--capture 的取值全是 Windows 的取图通道");
    }
    {
        const auto r = Run({"--monitor", "device:DISPLAY1", "out.png"});
        Check((HasError(r, codes::kPlatformUnsupported) != 0) == !windows,
              "--monitor 的两条 Windows 标识写法按取值判");
    }
    {
        const auto r = Run({"--monitor", "2", "out.png"});
        const bool okOnBoth = !HasError(r, codes::kPlatformUnsupported);
        Check(okOnBoth, "--monitor <编号> 两种平台都说得通，不在这一条上拒");
    }
    {
        const auto r = Run({"--pid", "1234", "out.png"});
        Check(!HasError(r, codes::kPlatformUnsupported), "--pid 不是 Windows 专属说法");
    }
    {
        const auto r = Run({"--help", "--hwnd", "0x10"});
        Check(r.ok && r.options.showHelp && !HasError(r, codes::kPlatformUnsupported),
              "--help 那一路先把人要的帮助交出去，不报平台专属");
    }
}

void CheckPaths() {
    Section("路径服务：形状判据按本平台");
    Check(ClassifyConfigPath(W("/tmp/x.png")) == ConfigPathFate::kQualified,
          "以 / 起手在两种平台都是完全限定");
    Check(ClassifyConfigPath(W("shots/x.png")) == ConfigPathFate::kRelativeToIni,
          "一般相对写法");
    Check(ClassifyConfigPath(W("C:shot.png")) ==
              (BuildPlatform() == PlatformKind::kWindows ? ConfigPathFate::kDriveRelative
                                                          : ConfigPathFate::kRelativeToIni),
          "盘符相对只属于 Windows：Linux 上这是一个合法的相对文件名");
    Check(LooksLikePath(W("/tmp/a")), "带 / 的写法像路径");
    Check(LooksLikePath(W("a\\b")) == (BuildPlatform() == PlatformKind::kWindows),
          "反斜杠只在 Windows 上算分隔符（Linux 上它是文件名里的普通字符）");
    Check(AcceptsSlashOptionPrefix() == (BuildPlatform() == PlatformKind::kWindows),
          "/foo=bar 这种选项写法只属于 Windows 的 cmd 习惯");

    Section("路径服务：绝对化与拆分");
    {
        std::wstring abs;
        const bool ok = NormalizeAbsolutePath(W("shot.png"), &abs);
        Check(ok && !abs.empty(), "相对写法能定出绝对路径");
        if (ok) {
            const bool rooted = BuildPlatform() == PlatformKind::kWindows
                                    ? (abs.size() >= 2 && abs[1] == L':')
                                    : (!abs.empty() && abs[0] == L'/');
            Check(rooted, "定出来的那条确实是绝对路径");
            Check(abs.size() >= 8 && abs.substr(abs.size() - 8) == W("shot.png"),
                  "最后一节还是原来那一节");
        }
        std::wstring bad;
        Check(!NormalizeAbsolutePath(W(""), &bad), "定不出来的那条交回 false，不交半条猜测值");
    }
    {
        std::wstring dir;
        std::wstring file;
        Check(SplitDirAndFile(W("/a/b/ECAPTURE.ini"), &dir, &file) && dir == W("/a/b") &&
                  file == W("ECAPTURE.ini"),
              "目录与最后一节拆得开");
        if (BuildPlatform() != PlatformKind::kWindows) {
            std::wstring d2;
            std::wstring f2;
            Check(SplitDirAndFile(W("no-separator.ini"), &d2, &f2) == false,
                  "整条里没有本平台分隔符 = 拆不出基准");
            Check(!SplitDirAndFile(W("/rootdir/"), &d2, &f2), "以分隔符收尾的目录名不当成文件");
        }
        std::wstring exe;
        Check(ExeDirectory(&exe) && !exe.empty(), "问得出本次可执行文件所在目录");
        Check(JoinDirAndFile(W("/a"), W("b.png")).find(W("//")) == std::wstring::npos ||
                  BuildPlatform() == PlatformKind::kWindows,
              "拼接不产出双分隔符（根目录那一种除外）");
    }
}

void CheckConfig(const std::wstring& iniPath) {
    Section("配置：真实文件、优先级与来源记账");
    const auto r = Run({"--config", EncodeUtf8(iniPath), "--title", "x"});
    Check(r.ok, "点名读一份配置：解析通过");
    Check(r.options.configSelection == W("explicit"), "来源记账说这份是 --config 点名的");
    Check(r.options.configState == W("loaded"), "读取下场是 loaded");
    Check(std::wstring(SourceOf(r.options, L"output.format")) == W("config"),
          "只写在 INI 里的那一条来源是 config");
    Check(std::wstring(SourceOf(r.options, L"output.jpeg_quality")) == W("config"),
          "数值型同样按 INI 落地");
    Check(CurrentLanguage() == Language::kJa, "配置里的语言在解析期就已生效");

    const auto withCli =
        Run({"--config", EncodeUtf8(iniPath), "--format", "png", "--title", "x"});
    Check(withCli.ok && std::wstring(SourceOf(withCli.options, L"output.format")) == W("cli"),
          "CLI 显式值盖过 INI（来源记成 cli）");
    Check(withCli.options.format == ImageFormat::kPng, "落地取值跟着换成 CLI 那一条");

    const auto none = Run({"--no-config", "--title", "x"});
    Check(none.ok && none.options.configSelection == W("none"),
          "--no-config 一条都不读，selection 说 none");

    const auto builtin = Run({"--no-config", "--title", "x"});
    Check(std::wstring(SourceOf(builtin.options, L"output.overwrite")) == W("builtin"),
          "两档都没写过的那一条来源是 builtin");
}

void CheckConfigFailures(const std::wstring& badIniPath, const std::wstring& nulPath) {
    Section("配置：读不懂就整份拒");
    const auto unknown = Run({"--config", EncodeUtf8(badIniPath), "--title", "x"});
    Check(!unknown.ok && HasError(unknown, codes::kConfigUnknownKey),
          "未知键报 config.unknown_key，不当没看见");

    const auto encoding = Run({"--config", EncodeUtf8(nulPath), "--title", "x"});
    const Diagnostic* d = FindError(encoding, codes::kConfigEncoding);
    Check(!encoding.ok && d != nullptr, "非法 UTF-8 的配置报 config.encoding");
    Check(d && d->detail.starts_with(W("byte:offset=")), "detail 说出第一处坏字节");

    std::wstring missing = JoinDirAndFile(TempDir(), W("ecapture-cli-text-no-such-file.ini"));
    const auto absent = Run({"--config", EncodeUtf8(missing), "--title", "x"});
    Check(!absent.ok && HasError(absent, codes::kConfigNotFound),
          "显式点名的文件不在 = config.not_found（不回退默认那份）");
}

void CheckFileService(const std::wstring& iniPath, const std::wstring& dir) {
    Section("文件服务：整份读取的下场分得开");
    const uint64_t limit = 1024 * 1024;
    const auto ok = ReadWholeFileBounded(iniPath, limit);
    Check(ok.outcome == ReadOutcome::kOk && !ok.bytes.empty(), "存在的文件整份读到");
    const auto again = ReadWholeFileBounded(iniPath, limit);
    Check(again.bytes == ok.bytes, "两次读到同一份字节（没有隐藏状态）");

    const auto missing = ReadWholeFileBounded(
        JoinDirAndFile(dir, W("ecapture-cli-text-definitely-absent.ini")), limit);
    Check(missing.outcome == ReadOutcome::kNotFound, "不存在 = kNotFound");

    const auto asDir = ReadWholeFileBounded(dir, limit);
    Check(asDir.outcome == ReadOutcome::kDirectory, "撞上同名目录要说清是目录，不阻塞也不当文件读");

    const auto tiny = ReadWholeFileBounded(iniPath, 4);
    Check(tiny.tooLarge && tiny.outcome != ReadOutcome::kOk,
          "超限在分配之前判：tooLarge 且不交出字节");
    Check(NativeErrorCodeLabel() != nullptr && *NativeErrorCodeLabel() != 0,
          "原生码的本平台叫法给得出来");
}

void CheckTextOutput() {
    Section("文本出口：帮助 / 版本 / 配置视图都不碰桌面");
    SetLanguage(Language::kZhCn);
    const std::wstring help = HelpText();
    Check(!help.empty() && help.find(W("--monitor")) != std::wstring::npos, "帮助含选项目录");
    Check(help.find(W("grp.target")) == std::wstring::npos && help.find(W("opt.hwnd")) ==
              std::wstring::npos,
          "帮助里没有裸 key（每条文案都取得到）");
    Check(help.find(L'\r') == std::wstring::npos, "内部文本只写 \\n：行尾交给出口那一层");
    SetLanguage(Language::kEn);
    Check(HelpText().find(L'\r') == std::wstring::npos, "换一种语言同样不含 \\r");
    const std::wstring version = VersionText();
    Check(version.find(W("EvernightCapture")) != std::wstring::npos &&
              version.find(kVersion) != std::wstring::npos,
          "--version 文本带程序名与版本号");

    ParseResult parse = Run({"--config", EncodeUtf8(TempFile("ecapture-cli-text-good.ini")),
                             "--show-config"});
    Check(parse.ok && parse.options.showConfig, "--show-config 是一条独立出口");
    const std::wstring view = RenderConfigView(parse.options);
    Check(view.find(W("\"contract\": \"configview\"")) != std::wstring::npos,
          "配置视图的契约名在里面");
    Check(view.find(W("no_pixels_no_files")) != std::wstring::npos,
          "这一路不取像素这件事写进 caveats");
}

void CheckJson() {
    Section("JSON：转义与形状");
    Json j;
    j.Obj().Key(L"captured").Value(0).Key(L"title")
        .Value(std::wstring(L"报\"\\文") + static_cast<wchar_t>(1) + static_cast<wchar_t>(0x1F600))
        .End();
    const std::wstring doc = j.Str();
    Check(doc.find(W("\\\"")) != std::wstring::npos && doc.find(W("\\\\")) != std::wstring::npos,
          "引号与反斜杠被转义");
    Check(doc.find(W("\\u0001")) != std::wstring::npos, "控制字符转成 \\u00XX");
    Check(doc.find(W("images")) == std::wstring::npos, "没写过的键不出现（精简契约）");

    const std::string bytes = EncodeUtf8(doc);
    Check(bytes.find("\xf0\x9f\x98\x80") != std::string::npos, "非 ASCII 原样写成 UTF-8 字节");
    Check(IsWellFormedUtf8(bytes.data(), bytes.size()), "整份 JSON 是合法 UTF-8 字节");
    Check(doc.find(L'\r') == std::wstring::npos, "JSON 的换行只用 \\n");

    Json q;
    q.Obj().Key(L"ok").Value(true).Key(L"n").Value(42).Key(L"big")
        .Value(static_cast<uint64_t>(1) << 40).Key(L"none").Null().End();
    Check(q.Str().find(W("true")) != std::wstring::npos &&
              q.Str().find(W("1099511627776")) != std::wstring::npos &&
              q.Str().find(W("null")) != std::wstring::npos,
          "布尔、64 位宽度与 null 都写对（数值宽度在两个平台同一套路）");
}

void CheckNativeEntry() {
    Section("入口：本平台的 argv 换成 CliTokens");
    // 这一段的本体在 tests/platform/ 下按平台各一份：入口换码这件事本来就是平台的事，
    // 主文件只负责调用，两个平台判的都是各自生产代码里的那一个函数。
    RunNativeArgvChecks(&Check);
}

// 判"文字确实没混进图片字节里"这件事，光看函数返回值不够，要看标准流上真实落下的字节。
// 所以自顶替一次 stdout：fork 出子进程，在子进程里把 stdout 换成一条临时文件，走一遍
// "先发图、再试着发文字"，父进程读回那份文件逐字节核对。判的还是生产代码那一条
// EmitStdout / EmitStdoutBytes（src/OutputStream.h 的平台实现），不另写一份出口。
//
// 这一条只在 POSIX 上成立：Windows 的控制台句柄不能用"重定向一个文件描述符"这一套换掉
// （那一路由 tests\cli.ps1 在真实的 ECAPTURE.EXE 上判）。所以 Linux 这份是实测，
// Windows 那份是既有脚本的现场，两边都不含糊成"应该没问题"。
void CheckEmissionBytesOnPosix(void (*check)(bool, const char*)) {
    if (BuildPlatform() == PlatformKind::kWindows) {
        // 不当成一条 PASS 计数：这一条在 Windows 上换了一条现场路线（见 tests\cli.ps1
        // 在真 exe 上判的 stdout 混流），这里如实说明"本文件这一条没在这个平台跑"。
        (void)check;  // 这一条在 Windows 上不计数，只如实说它没在这里跑
        std::printf("  SKIP  字节级通道判据（Windows 上由 tests\\cli.ps1 在 ECAPTURE.EXE 上判）\n");
        return;
    }
#ifndef _WIN32
    const std::string probe = "/tmp/ecapture-cli-text-emission";
    const std::string marker = "cli-text-must-not-appear";
    const std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

    pid_t child = fork();
    if (child < 0) {
        check(false, "通道字节判据：fork 失败");
        return;
    }
    if (child == 0) {
        const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) _exit(2);
        // 子进程里只走生产出口这三步：发图前的文字、图、发图后的文字。
        if (!EmitStdout(L"before\n")) _exit(3);
        NativeErrorCode io = kNativeErrorNone;
        uint64_t emitted = 0;
        if (!EmitStdoutBytes(png, &io, &emitted) || emitted != png.size()) _exit(4);
        if (EmitStdout(L"after-claim\n")) _exit(5);  // 占用后还写出文字 = 混流
        _exit(0);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        check(false, "通道字节判据：等不到子进程");
        return;
    }
    std::ifstream in(probe.c_str(), std::ios::binary);
    const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::remove(probe.c_str());
    const bool exited = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    check(exited, "通道字节判据：子进程按约定的三步走通（没在混流那一步自杀）");
    const size_t head = std::string("before\n").size();  // 占用前那一句实际落下的字节数
    check(got.size() >= head + png.size() && got.compare(0, head, "before\n") == 0,
          "占用前的文字落在 stdout 开头");
    check(got.find(marker) == std::string::npos,
          "图片占用 stdout 之后，一条文字都没再落到那份字节流里");
    check(got.compare(head, png.size(), reinterpret_cast<const char*>(png.data()), png.size()) == 0,
          "图片字节紧跟在文字之后原样落地，没有被插入或改写");
#endif
}

void CheckStreamDiscipline() {
    Section("输出通道：图片占用 stdout 之后文字必须改道");
    CheckEmissionBytesOnPosix(&Check);
    // stdout 一旦被图片声明占用就**永久**归图片，而本程序的判据进度本身一直在往 stdout 写。
    // 所以这一段之后 stdout 上只能有图片字节，那些 PASS/FAIL 行会改落到 stderr：
    // 汇总以"两份流合起来的条数"为准（test-all.sh 一起收两份日志），stdout 那份单独看也行。
    // 被这条边界挡掉的文字只可能是这里那几句诊断，判据本身没有一条依赖 stdout 的通道。
    const std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    NativeErrorCode io = kNativeErrorNone;
    uint64_t emitted = 0;
    Check(EmitStdoutBytes(png, &io, &emitted) && emitted == png.size(),
          "图片字节按约定发到 stdout，实际字节数交回");
    Check(StdoutClaimed(), "发过图之后 stdout 归图片");
    Check(!EmitStdout(W("cli-text-must-not-appear\n")),
          "此后 EmitStdout 直接返回 false，一个字节都不写");
    Check(EmitStderrRaw(W("cli-text-on-stderr-after-claim\n")), "改道 stderr 仍然写得出去");
}

}  // namespace

// 真实 execve 现场：./ecapture-cli-text-tests --echo-argv <中文 参数> …
// 用系统真的交下来的 argv 走一遍 DecodePosixArgv，再把结果按 JSON 转义后写成 UTF-8 交回 stdout。
// 判据程序自己不是 ECAPTURE，所以这一条**不是**"整条 CLI 已经能跑"的证据；它只证明
// 从 shell 到内部文字到出口字节这一段在真参数形状下不损不改（空格、中文、引号、控制字符）。
int EchoArgv(int argc, char** argv) {
    const CliTokens tokens = DecodePosixArgv(argc, const_cast<const char* const*>(argv));
    Json j;
    // 顺带把"本平台的系统显示语言探测"报出来：这一格由平台服务回答
    // （Windows 读显示语言 LCID，Linux 按 LC_ALL > LC_MESSAGES > LANG），换环境变量就能看到它动。
    j.Obj().Key(L"program").Value(L"cli-text-tests")
        .Key(L"systemLanguage").Value(LanguageTag(DetectSystemLanguage()))
        .Key(L"argv").Arr();
    for (std::size_t i = 0; i < tokens.Count(); ++i) {
        j.Obj().Key(L"text").Value(tokens.At(i))
            .Key(L"invalidByteOffset")
            .Value(tokens.InvalidOffset(i) == std::wstring::npos
                       ? static_cast<long long>(-1)
                       : static_cast<long long>(tokens.InvalidOffset(i)))
            .End();
    }
    j.End().End();
    EmitStdout(j.Str() + L"\n");  // 出口那一层负责编成 UTF-8 字节
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--echo-argv") return EchoArgv(argc, argv);
    std::printf("ecapture-cli-text-tests —— 本平台构建：%s\n", PlatformName(BuildPlatform()));

    const std::wstring dir = TempDir();
    const std::wstring iniPath = TempFile("ecapture-cli-text-good.ini");
    const std::string iniBytes =
        "[config]\n"
        "version=1\n"
        "[general]\n"
        "language = ja\n"
        "[output]\n"
        "format = bmp\n"
        "jpeg_quality = 70\n";
    const std::wstring badIniPath = TempFile("ecapture-cli-text-unknown-key.ini");
    const std::string badIniBytes = "[config]\nversion=1\n[output]\nunknown_key = 1\n";
    const std::wstring nulPath = TempFile("ecapture-cli-text-bad-utf8.ini");
    std::string nulBytes = "[config]\nversion=1\n[output]\nformat = ";
    nulBytes.push_back('\xE4');
    nulBytes.push_back('\x28');  // 三字节序列的第二个字节位置上出现 ASCII：整份拒
    nulBytes += "\n";

    bool prepared = WriteTemp(iniPath, iniBytes) && WriteTemp(badIniPath, badIniBytes) &&
                    WriteTemp(nulPath, nulBytes);
    Check(prepared, "本次自建的临时配置写好了（判据全部建立在真实文件上）");

    CheckTextCoding();
    CheckLanguage();
    CheckParsing();
    CheckPlatformGate();
    CheckPaths();
    if (prepared) {
        CheckConfig(iniPath);
        CheckConfigFailures(badIniPath, nulPath);
        CheckFileService(iniPath, dir);
        CheckTextOutput();
    }
    CheckJson();
    CheckNativeEntry();
    CheckStreamDiscipline();

    RemoveTemp(iniPath);
    RemoveTemp(badIniPath);
    RemoveTemp(nulPath);

    // 这一句的形状与 tests/core_boundary_state.cpp 一致：./test-all.sh 就按"N 条通过，M 条失败"
    // 汇总，读不到这一行它照实写 "-"，不猜数字。
    std::printf("\ncli-text: %d 条通过，%d 条失败\n", g_checks - g_failures, g_failures);
    return g_failures == 0 ? 0 : 1;
}
