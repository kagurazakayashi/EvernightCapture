// 【跨平台核心 · CLI 文本出口】帮助、版本与 --show-config 这三份"与桌面无关"的文本。
//
// 从 src/Report.cpp 搬出来是这一步要的边界：这三份只依赖选项目录、语言表与解析层交回来的
// 结构，一条都不碰窗口、屏幕、取帧、通知，也不做任何 GUI/桌面初始化 —— 所以在还没有截图
// 后端的构建里，--help / --version / --show-config 照样能跑，而且两个平台跑的是同一份实现。
// src/Report.cpp 那份（响应组装 + 桌面查询的渲染）仍然依赖 Windows 的取帧与窗口/屏幕枚举，
// 留在平台侧，等后端迁完再分。
//
// 文本里只写 \n：行尾交给出口那一层决定（src/OutputStream.h —— Windows 归一成 CRLF，
// Linux 原样）。把 \r\n 写死在共用文本里，就等于让 Linux 也吐出 Windows 的行尾。
#include "Report.h"

#include <algorithm>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "Json.h"
#include "Lang.h"
#include "SystemCompat.h"
#include "Version.h"

namespace ecapture {
namespace {

const wchar_t* GroupTitle(const std::wstring& group) {
    if (group == L"target") return L"grp.target";
    if (group == L"match") return L"grp.match";
    if (group == L"pick") return L"grp.pick";
    if (group == L"capture") return L"grp.capture";
    if (group == L"crop") return L"grp.crop";
    if (group == L"consent") return L"grp.consent";
    if (group == L"timeout") return L"grp.timeout";
    if (group == L"output") return L"grp.output";
    if (group == L"config") return L"grp.config";
    if (group == L"query") return L"grp.query";
    return L"grp.behavior";
}

std::wstring FlagColumn(const OptionInfo& o) {
    std::wstring s = L"--" + o.name;
    if (!o.shortName.empty()) s += L", -" + o.shortName;
    return s;
}
}  // namespace

// ---------------------------------------------------------------------------
// 纯文本帮助：骨架由选项目录生成，所以不会和实现脱节；每一句文案都取自资源（见 Lang.h），
// 换行与缩进写在文案里，好让每种语言自己安排对齐。
// ---------------------------------------------------------------------------
std::wstring HelpText() {
    std::wstring t;
    t += Msg(L"help.header") + L"\n";
    t += L"\n";
    t += Msg(L"help.usage1") + L"\n";
    t += Msg(L"help.usage2") + L"\n";
    t += Msg(L"help.usage3") + L"\n";
    t += Msg(L"help.usage4") + L"\n";
    t += L"\n";

    const auto& catalog = OptionCatalog();
    // config 那一组是"这次读哪一份配置"的两条入口（--config / --no-config）。它排在 output 之后、
    // 两条查询之前：它既不是截图那一级，也不是查询那一级，而查询这一路里 --show-config 恰恰
    // 要回答"读的是哪一份"，所以这一段紧邻着查询那一节。
    const wchar_t* groups[] = {L"target", L"match", L"pick", L"capture", L"crop", L"consent",
                               L"timeout", L"output", L"config", L"query", L"behavior"};
    size_t width = 0;
    for (const auto& o : catalog) {
        std::wstring col = FlagColumn(o);
        if (!o.valueHint.empty()) col += L" " + o.valueHint;
        width = std::max(width, col.size());
    }
    for (const wchar_t* g : groups) {
        t += Msg(GroupTitle(g));
        t += L"\n";
        for (const auto& o : catalog) {
            if (o.group != g) continue;
            std::wstring col = FlagColumn(o);
            if (!o.valueHint.empty()) col += L" " + o.valueHint;
            col = L"  " + col;
            col.append(width + 3 - col.size(), L' ');
            t += col + Msg(o.messageKey.c_str()) + L"\n";
        }
        t += L"\n";
    }

    t += Msg(L"help.syntax") + L"\n";
    t += Msg(L"help.output1") + L"\n";
    t += Msg(L"help.output2") + L"\n";
    t += Msg(L"help.exit1") + L"\n";
    t += Msg(L"help.exit2") + L"\n";
    t += Msg(L"help.status") + L"\n";
    t += Msg(L"help.system") + L"\n";
    // 授权这件事写在选项目录里（grp.consent 那一行 + --yes 的说明），不另立一段正文：
    // 免得同一个规矩在两处各写一遍，改了一处忘了另一处。
    t += L"\n";
    t += Msg(L"help.examples") + L"\n";
    for (const wchar_t* key : {L"help.example1", L"help.example2", L"help.example3", L"help.example4",
                               L"help.example5", L"help.example6", L"help.example7",
                               L"help.example8", L"help.example9"}) {
        t += L"  " + Msg(key) + L"\n";
    }
    return t;
}

std::wstring VersionText() {
    // minWindowsBuild 是本工具**对外声明**的最低 Windows 内部版本，与运行时那道能力检查取的是
    // 同一个数（src/SystemCompat.h 的 kSupportedMinBuild），所以脚本不必先读文档就能问出
    // "这个 exe 打算在哪种系统上工作"。它是声明，不是实测范围：实测只有 README
    // 《系统支持》那一节写的那一个版本。
    return std::wstring(L"EvernightCapture ") + kVersion + L"  (ECAPTURE.EXE / x64)  stage=" + kStage +
           L"  minWindowsBuild=" + std::to_wstring(os_floor::kSupportedMinBuild) + L"\n";
}

// ---------------------------------------------------------------------------
// --show-config：这一次这份配置的只读视图
// ---------------------------------------------------------------------------
//
// 这份文档回答的是"读的是哪一份文件、有没有读到、每一项生效偏好各来自哪个来源"，
// 而它本身一条像素都不取：不选窗口、不弹确认框、不建输出目录、也不建历史目录。
// 取值全部来自解析层已经算好的那一份（Options::settings 与 config* 三个字段），这里**不**
// 重读文件、也不重判一次优先级 —— 两份实现迟早对同一个键给出两种答案。
// 它也不写回任何东西：一份坏掉的配置在这里只会被如实报出来，不会被"顺手修好"。
std::wstring RenderConfigView(const Options& opt) {
    Json j;
    j.Obj();
    j.Key(L"contract").Value(L"configview");
    j.Key(L"contractVersion").Value(1);
    j.Key(L"program").Obj().Key(L"version").Value(kVersion).End();

    j.Key(L"config").Obj();
    // selection：这一份是**怎么**定下来的（default = 程序同目录那份；explicit = --config 点名；
    // none = --no-config 或 help/version 那一路一条都不读）。
    j.Key(L"selection").Value(opt.configSelection.empty() ? std::wstring(L"default")
                                                          : opt.configSelection);
    OptString(j, L"file", opt.configPath);
    OptString(j, L"state", opt.configState);
    // loaded 说的是"读到了一个文件"，而不是"读到的每一项都有效"：后者看 settings 与 errors。
    // 一份不存在（absent）的文件不是失败，也不该被读成"配置坏了"。
    j.Key(L"loaded").Value(opt.configState == L"loaded");
    j.End();

    // 每一项生效偏好：value 是**规范化后**的写法（枚举小写、数字去空白），source 是三档来源。
    // 顺序恒为 schema 的顺序，调用方可以按 key 分支而不必依赖位置。
    j.Key(L"settings").Arr();
    for (const Options::ResolvedSetting& s : opt.settings) {
        j.Obj().Key(L"key").Value(s.id).Key(L"value").Value(s.value).Key(L"source")
            .Value(s.source).End();
    }
    j.End();

    // 输出格式那条链的根据（显式 --format / 命令行输出名的扩展名 / 配置的 format /
    // 配置路径的扩展名 / 落到 png）。这一项单独报，是因为"读到了配置"与"这一项由谁定下来"
    // 是两件事，而 auto 那条设置本身不关掉扩展名推断。
    OptString(j, L"formatBasis", opt.formatBasis);

    // 这份文档没说过什么（ASCII token，不随 --lang 变，也不被 --quiet 藏掉）：
    //   no_pixels_no_files             这一路不取像素、不弹框、不创建输出与历史目录
    //   not_repaired                   本工具不改写也不修复配置：坏了就只报，不"顺手改好"
    //   preferences_listed_not_executed 带着 --show-config 这一次不出图，所以截图那一级的偏好
    //          （取法 / 光标 / HDR / 裁剪 / 缩放 / 输出）只是被**读出来**并报了来源，
    //          没有被写进那一条执行链路；真去截图时同一批判据才会生效
    j.Key(L"caveats").Arr();
    j.Value(L"no_pixels_no_files");
    j.Value(L"not_repaired");
    j.Value(L"preferences_listed_not_executed");
    j.End();
    j.End();
    return j.Str();
}


}  // namespace ecapture
