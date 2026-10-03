// 用户偏好配置（ECAPTURE.ini）的离线判据。
//
// 判的是**生产本体**：src/Config.cpp 的语法层与逐键判定、src/CliOptions.cpp 的合并与那一条
// 输出格式优先级链。测试里不另写一份"看着一样的规则"当判据 —— 那样只会确认同一个错误假设。
// 合并这一层因此一律通过 ParseCommandLine（真实入口）来判：给它一条 --config 与若干命令行写法，
// 核对落下来的 Options 与每一项的来源。
//
// 临时文件一律建在本次自建的 %TEMP% 目录里，结束整棵删掉；不碰仓库、也不碰任何用户配置。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/CliOptions.h"
#include "../src/Config.h"
#include "../src/Lang.h"

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
    std::fflush(stdout);  // 崩溃时最后一条用例不会被缓冲吞掉（排障用的判据本身要能看见）
}

void Section(const char* title) { std::printf("\n=== %s ===\n", title); }

std::wstring Wide(const std::string& s) {
    std::wstring out;
    for (char c : s) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}

std::string Narrow(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out.push_back(c > 0 && c < 128 ? static_cast<char>(c) : '?');
    return out;
}

std::vector<uint8_t> Bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ---- 本次自建的临时目录与临时 INI ----
std::wstring g_tempDir;

std::wstring TempIni(const char* name, const std::string& utf8) {
    const std::wstring path = g_tempDir + L"\\" + Wide(name);
    const std::string bytes = "\xEF\xBB\xBF" + utf8;  // 模板现场一律带 BOM，顺带判一次 BOM 可接受
    DWORD written = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        CloseHandle(h);
    }
    return path;
}

// 一次命令行解析。argv 里给的是 ASCII 窄串（路径也是本次自建的短路径）。
struct Parsed {
    ParseResult r;
    bool ok() const { return r.ok; }
    const Options& o() const { return r.options; }
    bool HasCode(const wchar_t* code) const {
        for (const Diagnostic& d : r.errors)
            if (d.code == code) return true;
        return false;
    }
    const Diagnostic* Find(const wchar_t* code) const {
        for (const Diagnostic& d : r.errors)
            if (d.code == code) return &d;
        return nullptr;
    }
    bool HasNote(const wchar_t* code) const {
        for (const Diagnostic& d : r.warnings)
            if (d.code == code) return true;
        return false;
    }
    // --show-config 与 -v 的来源表：按 key 找那一项
    const Options::ResolvedSetting* Setting(const wchar_t* id) const {
        for (const Options::ResolvedSetting& s : r.options.settings)
            if (s.id == id) return &s;
        return nullptr;
    }
    std::wstring SourceOf(const wchar_t* id) const {
        const Options::ResolvedSetting* s = Setting(id);
        return s ? s->source : std::wstring();
    }
    std::wstring ValueOf(const wchar_t* id) const {
        const Options::ResolvedSetting* s = Setting(id);
        return s ? s->value : std::wstring();
    }
};

Parsed Run(const std::vector<std::string>& args) {
    std::vector<std::wstring> wide;
    wide.push_back(L"ECAPTURE.EXE");
    for (const std::string& a : args) wide.push_back(Wide(a));
    std::vector<wchar_t*> argv;
    for (std::wstring& w : wide) argv.push_back(w.data());
    Parsed p;
    p.r = ParseCommandLine(static_cast<int>(argv.size()), argv.data());
    return p;
}

std::string WideArg(const std::wstring& w) { return Narrow(w); }

// 一条配置文件判据的最小写法：--show-config --config <path> [extra...]
Parsed ShowVia(const std::wstring& iniPath, const std::vector<std::string>& extra = {}) {
    std::vector<std::string> a{"--show-config", "--config", WideArg(iniPath)};
    for (const std::string& e : extra) a.push_back(e);
    return Run(a);
}

// ---- 语法层的小工具封装（判的就是生产那两份）----
struct ParsedIni {
    bool ok = false;
    std::vector<IniEntry> entries;
    std::vector<IniIssue> issues;
    bool HasCode(const wchar_t* code) const {
        for (const IniIssue& i : issues)
            if (i.code == code) return true;
        return false;
    }
    const IniIssue* Find(const wchar_t* code) const {
        for (const IniIssue& i : issues)
            if (i.code == code) return &i;
        return nullptr;
    }
};

ParsedIni Parse(const std::string& utf8) {
    ParsedIni p;
    p.ok = ParseIniBytes(Bytes(utf8), &p.entries, &p.issues);
    return p;
}

// 语法过了之后再判类型，返回快照与问题表（与 LoadConfigFile 用的同一份函数）。
struct Typed {
    ConfigSnapshot snap;
    std::vector<IniIssue> issues;
    bool HasCode(const wchar_t* code) const {
        for (const IniIssue& i : issues)
            if (i.code == code) return true;
        return false;
    }
};

Typed ParseAndType(const std::string& utf8) {
    ParsedIni p = Parse(utf8);
    Typed t;
    ApplySchemaToEntries(p.entries, &t.snap, &t.issues);
    for (const IniIssue& i : p.issues) t.issues.push_back(i);
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1) 语法层：严格、可解释、无副作用
// ---------------------------------------------------------------------------
static void SyntaxLayer() {
    Section("1) INI 语法层");

    Check(Parse("[general]\nlanguage = auto\n").ok, "LF 换行与基本赋值通过");
    Check(Parse("[general]\r\nlanguage = auto\r\n").ok, "CRLF 换行同样通过");
    Check(Parse("").ok, "空文件是合法的（一个键都没写，不是错误）");
    Check(Parse("\n\n   \n").ok, "空白行不影响判定");

    {
        ParsedIni p = Parse("; 整行注释\n# 另一种整行注释\n[general]\n  language = auto  \n");
        Check(p.ok && p.entries.size() == 1, "整行 ;/# 注释被跳过，取值去掉首尾空白");
        Check(p.entries.size() == 1 && p.entries[0].value == L"auto",
              "取值两端空白被去掉而本体不变");
    }

    {
        // 行内注释**不许**支持：Windows 路径里的 ; 与 # 会被截掉，那比一条报错更难查。
        ParsedIni p = Parse("[output]\npath = C:\\a;b#c\\x.png\n");
        Check(p.ok && p.entries.size() == 1 && p.entries[0].value == L"C:\\a;b#c\\x.png",
              "取值里的 ; 与 # 原样保留（没有行内注释这条规则）");
        // 不做环境变量展开、命令替换或 include：那一条 % 就只是模板里的那一条 %。
        ParsedIni q = Parse("[output]\npath = %USERPROFILE%\\%d-%t.png\n");
        Check(q.ok && q.entries.size() == 1 &&
                  q.entries[0].value == L"%USERPROFILE%\\%d-%t.png",
              "% 记号原样留着：配置解析不展开环境变量，也不把模板的 % 当别的东西");
    }

    {
        ParsedIni p = Parse("[output]\npath = \"  C:\\x y\\a.png \"\n");
        Check(p.ok && p.entries.size() == 1 && p.entries[0].value == L"  C:\\x y\\a.png ",
              "成对引号包住整个值：内部空白与反斜杠都留着");
        Check(Parse("[output]\npath = \"unclosed\n").HasCode(codes::kConfigSyntax),
              "不闭合的引号报错");
    }

    Check(Parse("[general]\nlanguage\n").HasCode(codes::kConfigSyntax), "缺等号报错");
    Check(Parse("[general]\n= auto\n").HasCode(codes::kConfigSyntax), "空键名报错");
    Check(Parse("language = auto\n").HasCode(codes::kConfigSyntax), "还没进节就写赋值报错");
    Check(Parse("[general]extra\nlanguage = auto\n").HasCode(codes::kConfigSyntax),
          "节名右括号后面还有东西报错");
    Check(Parse("[nosuch]\nx = 1\n").HasCode(codes::kConfigUnknownSection), "未知节报错");
    Check(Parse("[general]\nnosuchkey = 1\n").HasCode(codes::kConfigUnknownKey),
          "未知键报错，不静默忽略");
    Check(Parse("[general]\nlanguage = auto\n[general]\nverbosity = quiet\n")
              .HasCode(codes::kConfigDuplicateSection),
          "重复节报错（分两处写同一组设置时只会改到一处）");
    Check(Parse("[general]\nlanguage = auto\nlanguage = en\n").HasCode(codes::kConfigDuplicateKey),
          "重复键报错（既不后赢也不先赢）");
    Check(Parse("[general]\nLANGUAGE = auto\nLanguage = auto\n").HasCode(codes::kConfigDuplicateKey),
          "键名大小写不敏感，所以这两行是同一个键");

    {
        // 非法 UTF-8：一个 0xFF 起手的序列。判的是"按字节拒"，不是"按代码页猜一遍"。
        std::string bad = "[general]\nlanguage = ";
        bad.push_back(static_cast<char>(0xFF));
        bad += "\n";
        Check(Parse(bad).HasCode(codes::kConfigEncoding), "非法 UTF-8 字节序列报 config.encoding");
        std::string over = "[general]\nlanguage = ";
        over += "\xC0\xAF";  // 过长编码写的 '/'
        over += "\n";
        Check(Parse(over).HasCode(codes::kConfigEncoding), "过长编码序列同样被拒");
        std::string lone = "[general]\nlanguage = \x80\n";  // 没有起始字节的 continuation
        Check(Parse(lone).HasCode(codes::kConfigEncoding), "落单的 continuation 字节被拒");
        // NUL 要按长度拼进去：`std::string s = "a\x00b"` 走的是 strlen，那个 NUL 之后的内容
        // 会当场被截掉，用例就变成"什么都没测"而照样通过。
        std::string nul = "[general]\nlanguage = au";
        nul.push_back('\0');
        nul += "to\n";
        Check(nul.size() == 27 && nul[23] == '\0', "用例自己确实带着一个 NUL 字节");
        Check(Parse(nul).HasCode(codes::kConfigEncoding), "NUL 字节被拒（不当成看不见的字符）");
    }

    {
        const std::string head = "[general]\nlanguage = ";
        const std::string tail(head + std::string(4100, 'a') + "\n");
        Check(Parse(tail).HasCode(codes::kConfigLineTooLong), "单行超过 4096 字符报上限");
        Check(Parse(head + std::string(4000, 'a') + "\n").ok, "上限之内的长行照常通过");
    }

    {
        // 负面对照：一份坏文件里多条问题要**都**被列出来，而不是修一条报一条。
        ParsedIni p = Parse("[general]\nbogus = 1\n[output]\nnopath\n");
        Check(p.issues.size() >= 2 && !p.ok, "多处问题一次报全，且不折叠成一条");
    }
}

// ---------------------------------------------------------------------------
// 2) 逐键类型判定：与命令行同一套区间，INI 不接受 CLI 拒掉的写法
// ---------------------------------------------------------------------------
static void SchemaLayer() {
    Section("2) 逐键类型与区间");

    Check(ParseAndType("[config]\nversion = 1\n").snap.Has(L"config.version"),
          "version=1 通过");
    Check(ParseAndType("[config]\nversion = 2\n").HasCode(codes::kConfigUnsupportedVersion),
          "不认识的文件版本明确拒绝，不当兼容格式猜读");
    Check(ParseAndType("[config]\nversion = yes\n").HasCode(codes::kConfigInvalidNumber),
          "version 写成非数字报 invalid_number");

    Check(ParseAndType("[output]\noverwrite = true\n").snap.ValueOf(L"output.overwrite") == L"true",
          "布尔只认 true");
    Check(ParseAndType("[output]\noverwrite = TRUE\n").snap.ValueOf(L"output.overwrite") == L"true",
          "布尔大小写不敏感");
    Check(ParseAndType("[output]\noverwrite = 1\n").HasCode(codes::kConfigInvalidBoolean),
          "1 不算 true（不做模糊推断）");
    Check(ParseAndType("[output]\noverwrite = on\n").HasCode(codes::kConfigInvalidBoolean),
          "on 同样不算");
    // 负面对照：写坏了不叫"没写"。present 必须落回 false，否则合并会拿一个空值当显式决定。
    Check(!ParseAndType("[output]\noverwrite = on\n").snap.Has(L"output.overwrite"),
          "非法布尔不留下 present（不能当成一次显式决定）");

    Check(ParseAndType("[output]\njpeg_quality = 82\n").snap.ValueOf(L"output.jpeg_quality") ==
                  L"82",
          "数字沿用严格十进制语义");
    Check(ParseAndType("[output]\njpeg_quality = 0\n").HasCode(codes::kConfigOutOfRange),
          "quality 下界是 1");
    Check(ParseAndType("[output]\njpeg_quality = 101\n").HasCode(codes::kConfigOutOfRange),
          "quality 上界是 100");
    Check(ParseAndType("[output]\njpeg_quality = -1\n").HasCode(codes::kConfigInvalidNumber),
          "负数不是数字写法");
    Check(ParseAndType("[output]\njpeg_quality = 1e3\n").HasCode(codes::kConfigInvalidNumber),
          "1e3 不是十进制字面量");
    Check(ParseAndType("[output]\njpeg_quality = 99999999999999999999999\n")
              .HasCode(codes::kConfigInvalidNumber),
          "溢出直接拒，不回绕成一个小数字");
    Check(ParseAndType("[timeouts]\nprocessing_ms = 86400001\n").HasCode(codes::kConfigOutOfRange),
          "期限上限与 --timeout-ms 同一个数");
    Check(ParseAndType("[scale]\nmax_pixels = 268435457\n").HasCode(codes::kConfigOutOfRange),
          "像素天花板与 CLI 同一个数");

    Check(ParseAndType("[capture]\nmethod = bitblt\n").snap.ValueOf(L"capture.method") == L"bitblt",
          "取法取值表与 --capture 同一份");
    Check(ParseAndType("[capture]\nmethod = mangle\n").HasCode(codes::kConfigInvalidValue),
          "认不了的取法整条拒，不退化成 wgc");
    Check(ParseAndType("[capture]\ncursor = unset\n").snap.ValueOf(L"capture.cursor") == L"unset",
          "配置独有的 unset 是合法取值（CLI 没有这个词）");
    Check(ParseAndType("[output]\nformat = auto\n").snap.ValueOf(L"output.format") == L"auto",
          "配置独有的 auto 是合法取值（不等于 png）");
    Check(ParseAndType("[crop]\nmode = client-area\n").snap.ValueOf(L"crop.mode") == L"client-area",
          "裁剪方式取值表与 --roi / --client-area 同一件事");
    // 文本类不在这里判：roi 的语法与路径的展开都归合并那一步（与 CLI 共用同一份判据）。
    Check(ParseAndType("[crop]\nroi = nonsense\n").snap.ValueOf(L"crop.roi") == L"nonsense",
          "roi 作为文本先收下，由合并那一步按 --roi 的同一条判据判");
}

// ---------------------------------------------------------------------------
// 3) 合并与优先级：CLI 显式值 > INI 显式值 > 内置默认
// ---------------------------------------------------------------------------
static void MergeLayer() {
    Section("3) 合并与来源");

    const std::wstring empty = TempIni("empty.ini", "[general]\n");
    const std::string emptyA = Narrow(empty);

    // ---- 内置默认：与这条功能存在之前逐字节一致 ----
    {
        Parsed p = Run({"--no-config", "--class", "Shell_TrayWnd", "--dry-run", "out.png"});
        Check(p.ok(), "--no-config 下基本命令行照常解析");
        Check(p.o().capture == CaptureMethod::kWgc && !p.o().captureExplicit,
              "取法默认 wgc 且没被写成显式要求");
        Check(!p.o().cursor.given && !p.o().hdr.given,
              "光标/HDR 默认是**没提出要求**（结果里那三组键不该出现）");
        Check(p.o().format == ImageFormat::kPng && !p.o().formatExplicit, "默认 png 且非显式");
        Check(p.o().overwrite && p.o().jpegQuality == 100, "默认允许覆盖、质量 100");
        Check(p.o().timeoutMs == 0 && p.o().consentTimeoutMs == 0, "两条期限默认不限时");
        Check(p.o().historyEnabled && p.o().integrityHints, "历史与完整性解释默认开启");
        Check(p.o().listLimitDefault == 0, "分页默认条数留 0，由查询收敛到内置那个数");
        Check(p.o().configState == L"not_read" && p.o().configSelection == L"none",
              "--no-config 如实报「本次一条都没读」");
        Check(p.o().settings.size() == ConfigSchema().size(),
              "来源表把每一项都报全，不只报了被写过的那几项");
        Check(p.SourceOf(L"capture.method") == L"builtin" && p.ValueOf(L"capture.method") == L"wgc",
              "没读过文件时每一项都归 builtin");
    }

    // ---- 裸运行不截图：即使有一份配置 ----
    {
        const std::wstring ini = TempIni("bare.ini", "[capture]\nmethod = bitblt\n");
        Parsed p = Run({"--config", Narrow(ini)});
        Check(p.ok() && p.o().showHelp && p.o().helpReason == codes::kNoCondition,
              "配置存在时裸运行仍然只是帮助，不会变成一次自动截图");
        Check(p.o().configState == L"loaded" && p.o().configSelection == L"explicit",
              "点名 --config 那一路报 explicit，而不是 default");
    }

    // ---- 显式 false / 0 / none / unset 都是"写出过" ----
    {
        const std::wstring ini =
            TempIni("falsy.ini",
                    "[output]\noverwrite = false\n[capture]\ncursor = unset\n[scale]\n"
                    "max_width = 0\nmax_pixels = 0\n[timeouts]\nprocessing_ms = 0\n"
                    "[history]\nenabled = false\n");
        Parsed p = ShowVia(ini);
        Check(p.ok(), "一份只写假值的配置照常读得通");
        Parsed c = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(c.ok() && !c.o().overwrite, "overwrite=false 是一次**显式**决定，不被当成没写");
        Check(!c.o().historyEnabled, "history.enabled=false 落到本次判定上");
        Check(!c.o().scale.given, "三条天花板都是 0 就是不缩放，也不声称显式 scale");
        Check(!c.o().cursor.given, "cursor=unset 保持「没提出这项要求」");
        Check(c.SourceOf(L"capture.cursor") == L"config" && c.ValueOf(L"capture.cursor") == L"unset",
              "来源仍如实记 config：没提出要求不等于这一项没读");
        Check(c.SourceOf(L"output.overwrite") == L"config" &&
                  c.ValueOf(L"output.overwrite") == L"false",
              "来源表里 false 是配置写来的值，不是内置 true");
    }

    // ---- CLI > INI ----
    {
        const std::wstring ini = TempIni("prio.ini", "[capture]\nmethod = wgc\n[crop]\nmode = none\n");
        Parsed p = Run({"--config", Narrow(ini), "--capture", "bitblt", "--class", "Shell_TrayWnd",
                        "--dry-run", "x.png"});
        Check(p.o().capture == CaptureMethod::kBitBlt, "命令行写过的取法赢过配置");
        Check(p.SourceOf(L"capture.method") == L"cli", "来源记 cli（不是 config）");
        Parsed q = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(q.o().capture == CaptureMethod::kWgc && q.o().captureExplicit,
              "配置写过的取法同样是本次实际要求的取法（given/explicit 不只看命令行）");
        Check(q.SourceOf(L"capture.method") == L"config", "这一项来源是配置文件");
    }

    // ---- 配置提出的光标要求是真的要求：走的是同一条解析期闸门 ----
    {
        const std::wstring ini =
            TempIni("cursor.ini", "[capture]\nmethod = printwindow\ncursor = include\n");
        Parsed p = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(p.HasCode(codes::kCursorUnsupported) && !p.ok(),
              "配置里 cursor=include 配 printwindow 与命令行写 --cursor 同一个下场：解析期就拒");
        Check(p.o().cursor.given, "这条要求被记成实际提出过的要求（不是「命令行给过」那一条）");
    }

    // ---- 输出格式那条优先级链 ----
    {
        const std::wstring jpg = TempIni("p-jpg.ini", "[output]\npath = shot.jpg\n");
        Parsed a = Run({"--config", Narrow(jpg), "--class", "Shell_TrayWnd", "--dry-run"});
        Check(a.o().format == ImageFormat::kJpeg && a.o().formatBasis == L"config-output-extension",
              "配置路径的已知扩展名定下 jpeg（auto 不关掉推断）");

        const std::wstring pngJpg =
            TempIni("p-cfgfmt.ini", "[output]\npath = shot.png\nformat = jpeg\n");
        Parsed b = Run({"--config", Narrow(pngJpg), "--class", "Shell_TrayWnd", "--dry-run"});
        Check(b.o().format == ImageFormat::kJpeg && b.o().formatBasis == L"config-format",
              "同一份文件里非 auto 的 format 排在路径扩展名之前");

        const std::wstring cliOut = TempIni("p-cliout.ini", "[output]\nformat = jpeg\n");
        Parsed c = Run({"--config", Narrow(cliOut), "--class", "Shell_TrayWnd", "--dry-run",
                        "--out", "shot.png"});
        Check(c.o().format == ImageFormat::kPng &&
                  c.o().formatBasis == L"cli-output-extension",
              "命令行写的输出名的扩展名排在配置的一切之前");

        Parsed d = Run({"--config", Narrow(cliOut), "--class", "Shell_TrayWnd", "--dry-run",
                        "--format", "bmp", "--out", "shot.png"});
        Check(d.o().format == ImageFormat::kBmp &&
                  d.HasNote(codes::kFormatExtensionMismatch),
              "命令行 --format 与 --out 扩展名不一致时保持那条不一致提示，不改名也不重编");

        Parsed e = Run({"--class", "Shell_TrayWnd", "--dry-run", "--no-config"});
        Check(e.o().formatBasis == L"stdout-default-png", "什么都没给时是标准输出 + 默认 png");

        const std::wstring fmtOnly = TempIni("p-fmt.ini", "[output]\nformat = jpeg\n");
        Parsed f = Run({"--config", Narrow(fmtOnly), "--class", "Shell_TrayWnd", "--dry-run",
                        "--out", "-"});
        Check(f.o().format == ImageFormat::kJpeg && f.o().formatBasis == L"config-format",
              "配置的非 auto format 也管标准输出那一路");
        Check(!f.HasNote(codes::kPipeDefaultFormat),
              "这一次不是默认 png，就不发那条默认提示");
    }

    // ---- 路径原点：配置里的相对路径相对 INI 所在目录 ----
    {
        const std::wstring ini = TempIni("rel.ini", "[output]\npath = shots\\a.png\n");
        Parsed p = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run"});
        const std::wstring expect = g_tempDir + L"\\shots\\a.png";
        Check(p.o().output == expect, "配置里的相对输出路径按那份 INI 的目录定绝对路径");
        Check(p.ValueOf(L"output.path") == expect, "来源表报的是实际会被用的那条绝对路径");
        Parsed q = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run",
                        "--out", "cli.png"});
        Check(q.o().output == L"cli.png", "命令行的相对输出路径继续相对调用方 CWD（原样交下去）");
    }

    // ---- 裁剪：整组替换 / 整组取消 ----
    {
        const std::wstring ini = TempIni("crop.ini", "[crop]\nmode = client-area\n");
        Parsed a = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run", "--roi",
                        "1,2,30,40", "x.png"});
        Check(a.ok() && a.o().crop.mode == CropMode::kRoi && a.o().crop.width == 30,
              "命令行 --roi 替换整个配置裁剪组");
        Check(!a.HasCode(codes::kCropConflict),
              "配置那一组不参与「命令行同时写了两种写法」的判定");
        Check(a.SourceOf(L"crop.mode") == L"cli", "这一项来源记 cli");

        Parsed b = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run",
                        "--no-crop", "x.png"});
        Check(b.ok() && b.o().crop.mode == CropMode::kNone, "--no-crop 取消配置里那一组裁剪");

        Parsed c = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run",
                        "--no-crop", "--client-area", "x.png"});
        Check(c.HasCode(codes::kConflictingOptions) && !c.ok(),
              "--no-crop 与本次其他裁剪参数同时给出是冲突，不替用户挑一条");

        // 普通开关写 =false 等于没写（与解析层那条既有规则同一条判据）：这一路不该因为
        // "出现过 --no-crop 这个名字"就长出一次假冲突，也不该把已经写下的 --roi 取消掉。
        Parsed nc = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run",
                         "--no-crop=false", "--roi", "1,2,30,40", "x.png"});
        Check(nc.ok() && nc.o().crop.mode == CropMode::kRoi &&
                  !nc.HasCode(codes::kConflictingOptions),
              "--no-crop=false 不算写出过这一项，本次的 --roi 照常生效");

        const std::wstring cliBoth = TempIni("crop2.ini", "[crop]\nmode = none\n");
        Parsed d = Run({"--config", Narrow(cliBoth), "--class", "Shell_TrayWnd", "--dry-run",
                        "--roi", "1,2,3,4", "--client-area", "x.png"});
        Check(d.HasCode(codes::kCropConflict), "命令行内部同时给两种裁剪写法照旧报错");
    }

    // ---- 配置里裁剪的两条组合判据 ----
    {
        const std::wstring miss = TempIni("crop-miss.ini", "[crop]\nmode = roi\n");
        Parsed a = Run({"--config", Narrow(miss), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(a.HasCode(codes::kConfigInvalidCombination) && !a.ok(),
              "mode=roi 而没有 roi：报错，不退回整窗");
        const std::wstring bad = TempIni("crop-bad.ini", "[crop]\nmode = none\nroi = 1,2,3\n");
        Parsed b = Run({"--config", Narrow(bad), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(b.HasCode(codes::kConfigInvalidCombination),
              "非空的 roi 写法即使当前不是 roi 模式也要语法合");
        const std::wstring okRoi = TempIni("crop-ok.ini", "[crop]\nmode = roi\nroi = 5,6,70,80\n");
        Parsed c = Run({"--config", Narrow(okRoi), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(c.ok() && c.o().crop.mode == CropMode::kRoi && c.o().crop.x == 5 &&
                  c.o().crop.height == 80,
              "配置里的 roi 走的是 --roi 同一条解析判据");
        Check(!c.o().crop.roiGiven,
              "配置来源的 roi 不冒充「命令行写过 --roi」（否则会长出一条假冲突）");
    }

    // ---- 缩放：本次第一次写 --scale 就丢掉整个配置组 ----
    {
        const std::wstring ini = TempIni("scale.ini", "[scale]\nmax_pixels = 1000000\n");
        Parsed a = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run", "--scale",
                        "max-width=800", "x.png"});
        Check(a.o().scale.given && a.o().scale.hasMaxWidth && !a.o().scale.hasMaxPixels,
              "命令行 --scale 之后，配置里那条 max_pixels 不在背后继续缩图");
        Parsed b = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run",
                        "--no-scale", "x.png"});
        Check(!b.o().scale.given, "--no-scale 取消配置里那一组缩放");
        Parsed c = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run",
                        "--no-scale", "--scale", "max-width=800", "x.png"});
        Check(c.HasCode(codes::kConflictingOptions), "--no-scale 与 --scale 同时给出算冲突");
        Parsed d = Run({"--config", Narrow(ini), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(d.o().scale.given && d.o().scale.hasMaxPixels && d.o().scale.maxPixels == 1000000,
              "配置里的天花板确实进了生产链路");
    }

    // ---- 详略是一个组：配置的 verbose 不能抵消本次的 quiet ----
    {
        const std::wstring verbose = TempIni("v.ini", "[general]\nverbosity = verbose\n");
        Parsed a = Run({"--config", Narrow(verbose), "--quiet", "--class", "Shell_TrayWnd",
                        "--dry-run", "x.png"});
        Check(a.o().quiet && !a.o().verbose, "本次 --quiet 赢过配置里的 verbose");
        Parsed b = Run({"--config", Narrow(verbose), "--class", "Shell_TrayWnd", "--dry-run",
                        "x.png"});
        Check(b.o().verbose && !b.o().quiet, "没有命令行覆盖时才用配置那一档");
        Parsed c = Run({"--config", Narrow(verbose), "--verbose", "--quiet", "--class",
                        "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(c.o().verbose && !c.o().quiet &&
                  c.HasNote(codes::kFlagOverridesQuiet),
              "同一次命令行的 quiet/verbose 冲突仍按既有规则处理");
    }

    // ---- 两条新偏好的本次覆盖入口 ----
    {
        const std::wstring off = TempIni("hist-off.ini", "[history]\nenabled = false\n");
        Parsed a = Run({"--config", Narrow(off), "--history", "on", "--class", "Shell_TrayWnd",
                        "--dry-run", "x.png"});
        Check(a.o().historyEnabled && a.SourceOf(L"history.enabled") == L"cli",
              "--history on 覆盖配置里的 false");
        const std::wstring on = TempIni("hist-on.ini", "[history]\nenabled = true\n");
        Parsed b = Run({"--config", Narrow(on), "--history", "off", "--class", "Shell_TrayWnd",
                        "--dry-run", "x.png"});
        Check(!b.o().historyEnabled, "--history off 覆盖配置里的 true");
        Parsed bad = Run({"--config", Narrow(on), "--history", "maybe", "--class",
                          "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(bad.HasCode(codes::kInvalidValue) && !bad.ok(),
              "认不了的 on/off 整条作废，不退化成配置值");
        const std::wstring hints =
            TempIni("ih.ini", "[diagnostics]\nintegrity_hints = false\n");
        Parsed d = Run({"--config", Narrow(hints), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(!d.o().integrityHints, "integrity_hints 也进生产字段");
    }

    // ---- --yes 仍然只从命令行读 ----
    {
        Parsed p = Run({"--config", Narrow(empty), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(!p.o().yes, "配置里没有 --yes 这一项，本次就是没跳过确认");
    }
}

// ---------------------------------------------------------------------------
// 4) 只读查询不被污染 / help 与 version 不被坏配置改掉
// ---------------------------------------------------------------------------
static void QueryAndEscapeHatches() {
    Section("4) 查询与排障入口");

    // 一份"只写了节名、什么都没设"的文件：用来判排障入口本身不受配置内容影响。
    const std::wstring empty = TempIni("empty.ini", "[general]\n");

    const std::wstring full = TempIni(
        "q.ini",
        "[capture]\nmethod = bitblt\ncursor = include\n[output]\npath = o.png\nformat = jpeg\n"
        "[crop]\nmode = client-area\n[query]\nwindow_list_limit = 7\n");

    {
        // 自检那条约定：既有契约检查显式带 --no-config，用户偏好改不了它。
        Parsed p = Run({"--capabilities", "--no-config"});
        Check(p.ok() && p.o().capabilities, "--capabilities 显式带 --no-config 时照常出文档");
        Check(p.o().capture == CaptureMethod::kWgc && p.SourceOf(L"capture.method") == L"builtin",
              "这一路一条配置都没读，取法仍是内置默认");
        Parsed q = Run({"--config", Narrow(full), "--capabilities"});
        Check(q.ok() && q.o().capabilities,
              "--config 点名一份配置不与 --capabilities 冲突：它只决定读哪一份，不改这份查询问什么");
        Check(q.o().capture == CaptureMethod::kWgc,
              "环境查询这一路不应用配置里截图那一级的默认");
    }
    {
        // 核心一条：配置的截图那一级默认不该把只读查询污染成 query_conflict。
        Parsed p = Run({"--list", "--no-config"});
        Check(p.ok() && p.o().windowAction == WindowAction::kList,
              "--list 带 --no-config 照常出列表");
        Parsed q = Run({"--config", Narrow(full), "--list"});
        Check(q.ok(), "带一份满是截图偏好的配置时 --list 仍不是冲突");
        Check(!q.o().cursor.given && q.o().capture == CaptureMethod::kWgc,
              "查询这一路不应用配置里截图那一级的默认");
        Check(q.SourceOf(L"capture.cursor") == L"config" &&
                  q.ValueOf(L"capture.cursor") == L"include",
              "而来源表仍如实报那一项来自配置文件（读到与实际生效是两件事，都要看得见）");
    }
    {
        Parsed a = Run({"--config", Narrow(full), "--list"});
        Check(a.o().listLimitDefault == 7 && a.o().limit == 0,
              "配置的默认条数进得去，而命令行 --limit 的「写过没有」判据不被污染");
        Parsed b = Run({"--config", Narrow(full), "--list", "--limit", "3"});
        Check(b.o().limit == 3, "命令行 --limit 赢过配置默认");
        // --limit 是本次的覆盖，不改动"这一项设置"本身的来路：文件里写过 7，来源就还是 config，
        // 而本次真正用的条数与它的来路由分页结果里的 limit / limitSource 一起回答。
        Check(b.o().listLimitDefault == 7, "给了 --limit 也不把配置里的默认抹回 50");
        Check(b.SourceOf(L"query.window_list_limit") == L"config",
              "这一项的来源说的是设置本身来自哪里，不被本次的覆盖改写");
    }

    // 坏配置时帮助/版本/排障入口都还在
    {
        const std::wstring broken = TempIni("broken.ini", "[general]\nlanguage = auto\noops\n");
        Parsed h = Run({"--config", Narrow(broken), "--help"});
        Check(h.ok() && h.o().showHelp, "默认或点名的配置坏了，--help 照常给出");
        Parsed v = Run({"--config", Narrow(broken), "--version"});
        Check(v.ok() && v.o().showVersion, "--version 同理");
        Check(h.o().configState == L"not_read", "help/version 那一路一条配置都不读");
        Parsed n = Run({"--no-config", "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(n.ok() && n.o().configState == L"not_read",
              "--no-config 是排障入口：本次一条配置都不读，也不删也不改任何那份文件");
        Parsed s = Run({"--config", Narrow(broken), "--show-config"});
        Check(!s.ok() && s.HasCode(codes::kConfigSyntax),
              "--show-config 遇到坏文件报那条语法错，而不是交出一份看着正常的空表");
        const Diagnostic* d = s.Find(codes::kConfigSyntax);
        Check(d && d->line == 3 && !d->option.empty(),
              "配置诊断带行号与文件位置（不让人拿整份文件去猜）");
    }

    // --config 与 --no-config 互斥；点名的文件不在就是错，不回退
    {
        Parsed c = Run({"--config", Narrow(empty), "--no-config", "--class", "Shell_TrayWnd",
                        "--dry-run", "x.png"});
        Check(c.HasCode(codes::kConflictingOptions), "--config 与 --no-config 同时给出算冲突");
        // 用一个**真实存在的盘**下面的不存在目录：换成一个不存在的盘符时，Windows 给的是
        // ERROR_INVALID_DRIVE，那是"读不出来"那一类而不是"这个文件不在"那一类 —— 两条码本来就
        // 该按对面问出来的事实分开发，用例不能拿一个错误的现场去验另一条判据。
        Parsed m = Run({"--config", "C:\\no-such-dir-nothing-ec\\ECAPTURE.ini", "--class",
                        "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(m.HasCode(codes::kConfigNotFound) && !m.ok(),
              "点名的配置文件不存在是 config.not_found，不悄悄回退到默认那一份");
        Check(m.HasCode(codes::kConfigNotFound) &&
                  IsConfigReadFailure(m.Find(codes::kConfigNotFound)->code),
              "这一条码归「读不出来」那一类（退出码 8），不是用法错");
        Parsed s = Run({"--config", "C:\\no-such-dir-nothing-ec\\ECAPTURE.ini", "--show-config"});
        Check(s.HasCode(codes::kConfigNotFound), "--show-config 那一路同样如实报读不到");
        // 空白取值是"写法错"而不是"那份文件读不出来"：根本没有一条路径可去读，
        // 所以它归用法错那一边（cli.missing_value），不占 config.read_failed 的 I/O 归属。
        Parsed e = Run({"--config=", "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(e.HasCode(codes::kMissingValue) && !e.HasCode(codes::kConfigReadFailed) && !e.ok(),
              "--config= 这种空白取值报 cli.missing_value，不冒充 I/O 失败");
    }

    // --show-config 的允许与冲突清单
    {
        Parsed ok1 = Run({"--show-config", "--config", Narrow(empty), "--history", "off",
                          "--capture", "bitblt", "--quiet"});
        Check(ok1.ok() && ok1.o().showConfig,
              "配置视图允许带着要观察的偏好覆盖参数跑");
        Parsed bad1 = Run({"--show-config", "--class", "Shell_TrayWnd"});
        Check(bad1.HasCode(codes::kConfigViewConflict),
              "配置视图与真实截图意图混跑是冲突");
        Parsed bad2 = Run({"--show-config", "--capabilities"});
        Check(bad2.HasCode(codes::kConfigViewConflict), "两份只读文档不同时出");
        Parsed bad3 = Run({"--show-config", "--monitor", "1"});
        Check(bad3.HasCode(codes::kConfigViewConflict), "配置视图不选屏幕目标");
    }

    // 语言：配置生效，而命令行优先；一份读不懂的文件不参与
    {
        const std::wstring ja = TempIni("lang.ini", "[general]\nlanguage = ja\n");
        Parsed a = Run({"--config", Narrow(ja), "--class", "Shell_TrayWnd", "--dry-run", "x.png"});
        Check(CurrentLanguage() == Language::kJa, "配置里的语言在文件读得通时生效");
        Check(a.SourceOf(L"general.language") == L"config", "这一项来源记 config");
        Parsed b = Run({"--config", Narrow(ja), "--lang", "en", "--class", "Shell_TrayWnd",
                        "--dry-run", "x.png"});
        Check(CurrentLanguage() == Language::kEn, "命令行 --lang 赢过配置里的语言");
        Check(b.SourceOf(L"general.language") == L"cli", "来源跟着记 cli");
        const std::wstring badLang = TempIni("lang-bad.ini", "[general]\nlanguage = klingon\n");
        Parsed c = Run({"--config", Narrow(badLang), "--class", "Shell_TrayWnd", "--dry-run",
                        "x.png"});
        Check(c.HasCode(codes::kConfigInvalidValue) && !c.ok(),
              "认不了的语言整条拒，不退化成 auto，更不跳过这一行");
    }
}

// ---------------------------------------------------------------------------
// 5) 默认位置与读取的下场
// ---------------------------------------------------------------------------
static void LocationAndReadStates() {
    Section("5) 位置判据与读取下场");

    // 默认位置就是"实际运行的这个 exe 旁边"。本测试 exe 躺在 build\ 里，所以那份路径应该以
    // build\ 打头，而且文件名恒为 ECAPTURE.ini。
    {
        const std::wstring def = DefaultConfigPath();
        Check(!def.empty(), "问得出默认位置的配置文件路径");
        const size_t slash = def.find_last_of(L"\\/");
        Check(slash != std::wstring::npos && def.substr(slash + 1) == L"ECAPTURE.ini",
              "文件名恒为 ECAPTURE.ini，位置是模块自己所在目录");
        Check(def.find(L"ECAPTURE.example.ini") == std::wstring::npos,
              "默认读的是实际配置，不是那份入库模板");
    }

    // 一个不存在的目录当默认位置读：absent 是正常状态，不报错也不写文件。
    {
        const std::wstring nowhere = g_tempDir + L"\\no-such-subdir\\ECAPTURE.ini";
        ConfigSnapshot s = LoadConfigFile(nowhere, false);
        Check(s.state == ConfigLoadState::kAbsent,
              "默认位置没有文件 = 正常（内置默认继续，不报错、不创建文件）");
        Check(s.errors.empty(), "这一种一条诊断都不发");
        ConfigSnapshot e = LoadConfigFile(nowhere, true);
        Check(e.state == ConfigLoadState::kFailed &&
                  !e.errors.empty() && e.errors[0].code == codes::kConfigNotFound,
              "同一条路径在 --config 那一路是明确的失败，不回退");
    }

    // 那个名字是个目录：读失败而不是阻塞（GetPrivateProfileStringW 在这种情况下会挂住）。
    {
        const std::wstring asDir = g_tempDir + L"\\isa-dir.ini";
        CreateDirectoryW(asDir.c_str(), nullptr);
        ConfigSnapshot s = LoadConfigFile(asDir, true);
        Check(s.state == ConfigLoadState::kFailed &&
                  !s.errors.empty() && s.errors[0].code == codes::kConfigReadFailed,
              "那个名字是目录时如实报读失败，不阻塞也不当成没配置");
        RemoveDirectoryW(asDir.c_str());
    }

    // 超限：256 KiB 之外的文件在分配之前就被拒。
    {
        const std::wstring big = g_tempDir + L"\\big.ini";
        HANDLE h = CreateFileW(big.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const std::string filler(1024, 'a');
            std::string header = "[general]\n; ";
            DWORD w = 0;
            WriteFile(h, header.data(), static_cast<DWORD>(header.size()), &w, nullptr);
            for (int i = 0; i < 300; ++i) WriteFile(h, filler.data(), 1024, &w, nullptr);
            CloseHandle(h);
        }
        ConfigSnapshot s = LoadConfigFile(big, false);
        Check(s.state == ConfigLoadState::kFailed && !s.errors.empty() &&
                  s.errors[0].code == codes::kConfigTooLarge,
              "超过 256 KiB 的文件不先搬进内存再报错");
        Check(!IsConfigReadFailure(codes::kConfigTooLarge),
              "超限是「说不通」那一类（用法错），不是「读不出来」那一类");
    }

    // 只读判据：读一次配置这件事自己不产生任何副作用。
    {
        const std::wstring probe = g_tempDir + L"\\side-effect.ini";
        TempIni("side-effect.ini", "[general]\nlanguage = auto\n");
        ConfigSnapshot s = LoadConfigFile(probe, true);
        Check(s.state == ConfigLoadState::kLoaded, "带 BOM 的 UTF-8 文件照常读");
        Check(GetFileAttributesW((g_tempDir + L"\\history").c_str()) == INVALID_FILE_ATTRIBUTES,
              "读配置不会顺手建出 history 目录");
        DWORD before = GetFileAttributesW(probe.c_str());
        LoadConfigFile(probe, true);
        Check(GetFileAttributesW(probe.c_str()) == before, "读配置不改动那份文件");
    }
}

int main() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    std::wstring root = base;
    if (!root.empty() && root.back() == L'\\') root.pop_back();
    g_tempDir = root + L"\\ec-config-state";
    CreateDirectoryW(g_tempDir.c_str(), nullptr);

    std::printf("EvernightCapture 配置判据（离线层）\n");
    SyntaxLayer();
    SchemaLayer();
    MergeLayer();
    QueryAndEscapeHatches();
    LocationAndReadStates();

    std::printf("\n共 %d 项检查，失败 %d\n", g_checks, g_failures);

    // 只清理本次自己建的目录里那些本次写的临时 INI：里面没有任何用户数据。
    for (const char* name :
         {"empty.ini", "bare.ini", "falsy.ini", "prio.ini", "cursor.ini", "p-jpg.ini", "p-cfgfmt.ini",
          "p-cliout.ini", "p-fmt.ini", "rel.ini", "crop.ini", "crop2.ini", "crop-miss.ini",
          "crop-bad.ini", "crop-ok.ini", "scale.ini", "v.ini", "hist-off.ini", "hist-on.ini",
          "ih.ini", "q.ini", "broken.ini", "lang.ini", "lang-bad.ini",
          "side-effect.ini", "big.ini"}) {
        const std::wstring p = g_tempDir + L"\\" + Wide(name);
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) DeleteFileW(p.c_str());
    }
    RemoveDirectoryW(g_tempDir.c_str());
    return g_failures == 0 ? 0 : 1;
}
