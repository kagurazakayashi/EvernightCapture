#include "Report.h"

#include <algorithm>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Json.h"
#include "Capture.h"
#include "EnvReport.h"
#include "Lang.h"
#include "ScreenIdentity.h"   // MonitorSelectorLabel / KindName：选择器的回显只写一处
#include "ScreenQuery.h"
#include "SystemCompat.h"
#include "WindowQuery.h"
#include "WindowQueryRun.h"

namespace ecapture {
namespace {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::wstring HwndHex(uint64_t hwnd) {
    wchar_t buf[24];
    swprintf(buf, 24, L"0x%08llX", static_cast<unsigned long long>(hwnd));
    return buf;
}

std::wstring AbsoluteOf(const std::wstring& path) {
    if (path.empty() || path == L"-") return path;
    const DWORD cap = 8 * MAX_PATH;
    std::vector<wchar_t> buffer(cap);
    const DWORD n = GetFullPathNameW(path.c_str(), cap, buffer.data(), nullptr);
    if (n == 0 || n >= cap) return std::wstring();
    return std::wstring(buffer.data());
}

void StrArray(Json& j, const std::vector<std::wstring>& items) {
    j.Arr();
    for (const auto& s : items) j.Value(s);
    j.End();
}

void OptString(Json& j, const wchar_t* key, const std::wstring& value) {
    if (value.empty()) return;   // 精简契约：没内容就不出现这个键
    j.Key(key).Value(value);
}

// 空字段一律省略，调用方按存在与否取值
void DiagnosticArray(Json& j, const std::vector<Diagnostic>& items) {
    j.Arr();
    for (const auto& d : items) {
        j.Obj();
        j.Key(L"code").Value(d.code);
        OptString(j, L"message", d.message);
        OptString(j, L"option", d.option);
        OptString(j, L"value", d.value);
        OptString(j, L"hint", d.hint);
        // 定位字段：只在这一步真的拿到了值时才出现，不去凑一个"看起来有"的占位
        OptString(j, L"target", d.target);
        OptString(j, L"backend", d.backend);
        OptString(j, L"stage", d.stage);
        OptString(j, L"hresult", d.hresult);
        if (d.win32 != 0) j.Key(L"win32").Value(static_cast<long long>(d.win32));
        j.End();
    }
    j.End();
}

// 一个屏幕矩形：负坐标照写（副屏可以在主屏左边/上边），零宽高整个键省略由调用方判。
void WriteRect(Json& j, const wchar_t* key, const RECT& r) {
    j.Key(key).Obj()
        .Key(L"x").Value(static_cast<long long>(r.left))
        .Key(L"y").Value(static_cast<long long>(r.top))
        .Key(L"width").Value(static_cast<long long>(r.right - r.left))
        .Key(L"height").Value(static_cast<long long>(r.bottom - r.top))
        .End();
}

// ---------------------------------------------------------------------------
// images：每条 = 一次成功捕获。空字段省略。
// ---------------------------------------------------------------------------
void WriteImages(Json& j, const std::vector<CapturedImage>& images) {
    j.Arr();
    for (const auto& img : images) {
        j.Obj();
        OptString(j, L"file", img.file);
        j.Key(L"bytes").Value(static_cast<long long>(img.bytes));
        j.Key(L"width").Value(static_cast<long long>(img.width));
        j.Key(L"height").Value(static_cast<long long>(img.height));
        OptString(j, L"format", img.format);
        OptString(j, L"source", img.source);   // 实际出图的通道，auto 时与请求值不同
        // 来路这三项是隐私判据：这一帧是"窗口自己的画面"还是"屏幕上那块区域"，以及那块区域
        // 在哪。调用方（包括 AI）必须能看到它，--quiet 也不抑制（images 段从来不被抑制）。
        OptString(j, L"path", img.path);
        OptString(j, L"scope", img.scope);
        if (img.rect.right > img.rect.left && img.rect.bottom > img.rect.top) {
            WriteRect(j, L"rect", img.rect);
        }
        // 从整幅桌面帧里裁出目标的通道（duplication、拷屏幕的 bitblt）另外报告定位过程：
        // requestedRect 是本来要截的那一块、capturedRect 是实际截到的那一块、clipped 是两者
        // 不等价、rotation 是交付前把桌面帧顺时针转了多少度。窗口内容路径不写这几个键 ——
        // 它们截的就是整个目标，没有"丢区域"这回事，写一堆 false/0 反而让调用方以为有第二套判断。
        if (img.reportsCrop) {
            WriteRect(j, L"requestedRect", img.requestedRect);
            WriteRect(j, L"capturedRect", img.capturedRect);
            if (img.clipped) j.Key(L"clipped").Value(true);
            if (img.rotation != 0) j.Key(L"rotation").Value(static_cast<long long>(img.rotation));
        }
        // 窗口内部裁剪（--roi / --client-area）：只有这一次真裁了才写这一组。上面那几个键说的
        // 还是"整窗那一块从桌面上截得全不全"，这一组说的是在这张整窗图像之内又留下了哪一块，
        // 两层各管各的，所以同时出现时并不矛盾。
        //   cropRect      图像自己的像素坐标（左上角 = (0,0)，物理像素，不是桌面坐标）
        //   fullWidth/fullHeight  裁之前的整窗图像尺寸；width/height 是裁之后的最终尺寸
        //   cropScreenRect        同一块矩形在虚拟屏幕坐标里的那一块 —— 只在核实得出图像原点
        //                 时才写（缺这一行另有 note.crop_mapping_unavailable，不是"没查"）
        // 这一组与 requestedRect 同样是定位判据，--quiet 不许藏（images 段本来就不被抑制）。
        if (img.cropped) {
            j.Key(L"cropMode").Value(img.cropMode);
            j.Key(L"cropRect").Obj()
                .Key(L"x").Value(static_cast<long long>(img.crop.x))
                .Key(L"y").Value(static_cast<long long>(img.crop.y))
                .Key(L"width").Value(static_cast<long long>(img.crop.width))
                .Key(L"height").Value(static_cast<long long>(img.crop.height))
                .End();
            j.Key(L"fullWidth").Value(static_cast<long long>(img.fullWidth));
            j.Key(L"fullHeight").Value(static_cast<long long>(img.fullHeight));
            if (img.hasCropScreen) WriteRect(j, L"cropScreenRect", img.cropScreen);
        }
        // 等比缩小（--scale）：写过这条选项时这组键才出现（没写过时一个都不出现，
        // 与这条选项存在之前的输出逐字节相同）。四个键各说一件事、谁也不冒充谁：
        //   scaleMethod       插值策略，恒为 "nearest"：交付像素 (x,y) 取缩之前那张图的
        //                     (floor(x*srcW/dstW), floor(y*srcH/dstH))，可预测的整数映射
        //   scaleApplied      这一次真的缩小了；false = 图本来就在天花板之内，原样交付
        //   scaleFromWidth / scaleFromHeight  缩之前那张（裁之后）图的尺寸；width/height 是缩之后的
        // 映射是闭合的：scaleFrom 是缩放那一层的输入尺寸，再往上一层就是 cropRect（给了裁剪时）。
        // 与 cropRect / cursor* / hdr* 同一性质：这是内容与映射判据，--quiet 不许藏。
        // 取值全是 ASCII 机器名，不随 --lang 变。
        if (img.scaled) {
            j.Key(L"scaleMethod").Value(L"nearest");
            j.Key(L"scaleApplied").Value(img.scaleApplied);
            j.Key(L"scaleFromWidth").Value(static_cast<long long>(img.scaleFromWidth));
            j.Key(L"scaleFromHeight").Value(static_cast<long long>(img.scaleFromHeight));
        }
        // 光标（--cursor）：这一次真写过这条选项才写这三个键，没写过时一个都不出现
        // （与这条选项存在之前的输出逐字节相同）。三个键各说一件事、谁也不冒充谁：
        //   cursorRequested  用户要的那一种：default / include / exclude
        //   cursorEffective  这条路径**实际**交回的那一种：include / exclude / unverified
        //   cursorBasis      这个结论凭什么：设过并读回（wgc 那条会话的开关）/ 只读了当前值 /
        //                    这条路径的来源像素本来就没有光标 / 那一问没答案
        // 与 rect / capturedRect / cropRect 同一性质：这是定位与内容判据，--quiet 不许藏
        //（images 段本来就不被抑制）。取值全是 ASCII 机器名，不随 --lang 变。
        if (img.cursor.written) {
            j.Key(L"cursorRequested").Value(img.cursor.requested);
            j.Key(L"cursorEffective").Value(img.cursor.effective);
            j.Key(L"cursorBasis").Value(img.cursor.basis);
        }
        // HDR（--hdr）：这一次真写过这条选项才写这一组键，没写过时一个都不出现
        // （与这条选项存在之前的输出逐字节相同）。五个键各说一件事、谁也不冒充谁：
        //   hdrRequested      要求的策略：auto / tonemap / refuse
        //   hdrEffective      这一帧实际经历的处理：sdr_passthrough / tone_mapped / unverified
        //   hdrBasis          这个结论凭什么（来源那条路径登记成什么、这一帧来源核实成什么）
        //   sourceColorSpace  编码之前那一份来源色彩空间（映射过的 wide 帧保留映射前那一份）
        //   sourceBitDepth    来源每通道位数（8 / 10 / 16），认不出来时整个键不出现
        // 与 cursor* / rect / capturedRect / cropRect 同一性质：这是内容判据，--quiet 不许藏。
        if (img.hdr.written) {
            j.Key(L"hdrRequested").Value(img.hdr.requested);
            j.Key(L"hdrEffective").Value(img.hdr.effective);
            j.Key(L"hdrBasis").Value(img.hdr.basis);
            OptString(j, L"sourceColorSpace", img.hdr.sourceColorSpace);
            if (img.hdr.bitDepthKnown)
                j.Key(L"sourceBitDepth").Value(static_cast<long long>(img.hdr.bitDepth));
        }
        if (img.screen) {
            // 屏幕目标没有窗口可归属：给屏幕信息，窗口那几个键整个不出现
            j.Key(L"monitor").Value(static_cast<long long>(img.monitorOrdinal));
            OptString(j, L"device", img.deviceName);
            j.Key(L"primary").Value(img.primary);
        } else {
            OptString(j, L"hwnd", img.hwndHex);
            j.Key(L"pid").Value(static_cast<long long>(img.pid));
            OptString(j, L"title", img.title);
            OptString(j, L"class", img.windowClass);
            OptString(j, L"image", img.imageName);
        }
        j.Key(L"elapsedMs").Value(static_cast<long long>(img.elapsedMs));
        j.End();
    }
    j.End();
}

// ---------------------------------------------------------------------------
// input：--verbose 才输出的规范化输入，用于排查"程序到底理解了什么"
// ---------------------------------------------------------------------------
void WriteInputEcho(Json& j, const Options& opt) {
    const MatchOptions& m = opt.match;
    j.Obj();
    j.Key(L"hwnd").Arr();
    for (uint64_t h : m.hwnds) {
        j.Obj().Key(L"hex").Value(HwndHex(h)).Key(L"decimal").Value(h).End();
    }
    j.End();
    j.Key(L"pid").Arr();
    for (uint32_t p : m.pids) j.Value(static_cast<long long>(p));
    j.End();
    j.Key(L"process"); StrArray(j, m.processes);
    j.Key(L"exe");     StrArray(j, m.exePaths);
    j.Key(L"title");   StrArray(j, m.titles);
    j.Key(L"titleContains"); StrArray(j, m.titleContains);
    j.Key(L"titleRegex");    StrArray(j, m.titleRegexes);
    j.Key(L"class");         StrArray(j, m.classes);

    OptString(j, L"output", AbsoluteOf(opt.output));
    j.Key(L"toStdout").Value(opt.output == L"-");
    // 覆盖策略也回显出来：--no-overwrite 的布尔写法（裸写 / =true / =false）只有这样才能
    // 在不截图的情况下被断言（布尔别名一共有十种写法）。
    j.Key(L"overwrite").Value(opt.overwrite);
    // --yes 同理：它是"跳过窗口内容路径的确认"这个决定的最终结果，重复给出时最后一个生效，
    // 断言它不必真的去截一张图（也不必打扰人）。
    j.Key(L"yes").Value(opt.yes);
    // 两条期限也回显：0 = 不设这项期限。断言"参数最终落到什么值"不必真的去等一个超时。
    j.Key(L"timeoutMs").Value(static_cast<long long>(opt.timeoutMs));
    j.Key(L"consentTimeoutMs").Value(static_cast<long long>(opt.consentTimeoutMs));
    if (opt.monitor.given) {
        // 编号回显成数字（老契约不变），其余几种写法回显成它那条形如 `device:DISPLAY1` 的标签，
        // 并且**另外**回显一条 monitorKind：调用方不必去猜"这个 monitor 字段到底是数还是串"。
        if (opt.monitor.kind == MonitorSelector::Kind::kOrdinal) {
            j.Key(L"monitor").Value(static_cast<long long>(opt.monitor.ordinal));
        } else {
            j.Key(L"monitor").Value(MonitorSelectorLabel(opt.monitor));
        }
        j.Key(L"monitorKind").Value(MonitorSelectorKindName(opt.monitor));
        j.Key(L"target").Value(opt.ScreenMode() ? L"screen" : L"window");
    }
    j.Key(L"format").Value(FormatName(opt.format));
    j.Key(L"formatGiven").Value(opt.formatExplicit);
    j.Key(L"capture").Value(CaptureMethodName(opt.capture));
    j.Key(L"captureGiven").Value(opt.captureExplicit);
    // 光标这一条与 capture 同一做法：规范化取值 + "这次写没写过这条选项"各一个键，恒写。
    // 默认值因此是问得出来的（cursorGiven=false / cursor="default"），而"实际交回的那一种"
    // 在每一张图的 cursorEffective / cursorBasis 里 —— 那是按路径算的事，不在这里。
    // 断言"这次到底要求了什么"不必真的去截一张图（与 input.overwrite / input.yes 同一个理由）。
    j.Key(L"cursor").Value(CursorModeName(opt.cursor.mode));
    j.Key(L"cursorGiven").Value(opt.cursor.given);
    // HDR 这一条与 cursor 同一做法：规范化取值 + "这次写没写过这条选项"各一个键，恒写。
    // 而"这一帧实际经历的处理"（hdrEffective / sourceColorSpace / sourceBitDepth）在每一张图里，
    // 那是按来源算的事，不在这里。断言"这次到底要求了什么"不必真的去截一张 HDR 图。
    j.Key(L"hdr").Value(HdrPolicyName(opt.hdr.policy));
    j.Key(L"hdrGiven").Value(opt.hdr.given);
    // 窗口内部裁剪这一层也回显：这次是哪种裁剪、--roi 那四个数规范化成了什么。
    // 断言"参数最终落到什么值"不必真的去截一张图（与 input.overwrite / input.yes 同一个理由）。
    // 没给裁剪时整个键不出现 —— 与 input.monitor 那条"给了才写"一致。
    if (opt.crop.mode != CropMode::kNone) {
        j.Key(L"crop").Obj();
        j.Key(L"mode").Value(CropModeName(opt.crop.mode));
        if (opt.crop.mode == CropMode::kRoi) {
            j.Key(L"x").Value(static_cast<long long>(opt.crop.x));
            j.Key(L"y").Value(static_cast<long long>(opt.crop.y));
            j.Key(L"width").Value(static_cast<long long>(opt.crop.width));
            j.Key(L"height").Value(static_cast<long long>(opt.crop.height));
        }
        j.End();
    }
    // 等比缩小这一层也回显：这次给过哪几条天花板、规范化成了什么数。分开写是让调用方不必
    // 从用户原文里抠数字（与 input.crop 那条同源）。三条天花板互相独立，没给的那条整个键不出现；
    // 没写过 --scale 时 input.scale 整个键不出现（与 input.monitor 那条"给了才写"一致）。
    if (opt.scale.given) {
        j.Key(L"scale").Obj();
        if (opt.scale.hasMaxWidth)
            j.Key(L"maxWidth").Value(static_cast<long long>(opt.scale.maxWidth));
        if (opt.scale.hasMaxHeight)
            j.Key(L"maxHeight").Value(static_cast<long long>(opt.scale.maxHeight));
        if (opt.scale.hasMaxPixels)
            j.Key(L"maxPixels").Value(static_cast<long long>(opt.scale.maxPixels));
        j.End();
    }
    // 运行环境这两项是"这台机器能走哪几条通道"的答案，与 --capture 请求了什么无关：
    //   osBuild       本机 Windows 内部版本；整个键不出现 = 这一问没成功（问不出来不等于支持，
    //                 也不等于不支持，所以下面那条链这时没有被版本筛过）
    //   captureChain  这一次真正可以试的通道，按尝试顺序（auto 时被版本挡掉的那几条不在里面；
    //                 显式指定且被挡掉时是空数组，而不是偷偷换成别的那一条）
    // 有了这两项，调用方（含 AI）在不必先截图、也不必打扰人的情况下就能判出"这次失败该换后端"
    // 还是"这台机器不行"。
    const OsVersion os = ProbeOsVersion();
    if (os.known) j.Key(L"osBuild").Value(static_cast<long long>(os.build));
    j.Key(L"captureChain").Arr();
    // 与真去截图那一次同一个判据、同一条顺序：版本筛完之后还要按这一次的两种要求筛
    //（--cursor include 配 auto 时链里只剩设得进开关的那一条，--hdr tonemap/refuse 配 auto 时
    // 链里只剩真兑现得了那一条）。回显与执行不是两套答案。
    for (const CaptureMethod usable : GateCaptureChain(opt.capture, opt.ScreenMode(), os,
                                                       opt.cursor, opt.hdr)
                                             .chain) {
        j.Value(CaptureMethodName(usable));
    }
    j.End();
    j.Key(L"lang").Value(LanguageTag(CurrentLanguage()));
    j.Key(L"policy").Value(MultiKey(opt.multi));
    if (opt.multi == MultiMatch::kIndex) j.Key(L"index").Value(opt.index);
    j.End();
}

const wchar_t* GroupTitle(const std::wstring& group) {
    if (group == L"target") return L"grp.target";
    if (group == L"match") return L"grp.match";
    if (group == L"pick") return L"grp.pick";
    if (group == L"capture") return L"grp.capture";
    if (group == L"crop") return L"grp.crop";
    if (group == L"consent") return L"grp.consent";
    if (group == L"timeout") return L"grp.timeout";
    if (group == L"output") return L"grp.output";
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
    t += Msg(L"help.header") + L"\r\n";
    t += L"\r\n";
    t += Msg(L"help.usage1") + L"\r\n";
    t += Msg(L"help.usage2") + L"\r\n";
    t += Msg(L"help.usage3") + L"\r\n";
    t += Msg(L"help.usage4") + L"\r\n";
    t += L"\r\n";

    const auto& catalog = OptionCatalog();
    const wchar_t* groups[] = {L"target", L"match", L"pick", L"capture", L"crop", L"consent",
                               L"timeout", L"output", L"query", L"behavior"};
    size_t width = 0;
    for (const auto& o : catalog) {
        std::wstring col = FlagColumn(o);
        if (!o.valueHint.empty()) col += L" " + o.valueHint;
        width = std::max(width, col.size());
    }
    for (const wchar_t* g : groups) {
        t += Msg(GroupTitle(g));
        t += L"\r\n";
        for (const auto& o : catalog) {
            if (o.group != g) continue;
            std::wstring col = FlagColumn(o);
            if (!o.valueHint.empty()) col += L" " + o.valueHint;
            col = L"  " + col;
            col.append(width + 3 - col.size(), L' ');
            t += col + Msg(o.messageKey.c_str()) + L"\r\n";
        }
        t += L"\r\n";
    }

    t += Msg(L"help.syntax") + L"\r\n";
    t += Msg(L"help.output1") + L"\r\n";
    t += Msg(L"help.output2") + L"\r\n";
    t += Msg(L"help.exit1") + L"\r\n";
    t += Msg(L"help.exit2") + L"\r\n";
    t += Msg(L"help.status") + L"\r\n";
    t += Msg(L"help.system") + L"\r\n";
    // 授权这件事写在选项目录里（grp.consent 那一行 + --yes 的说明），不另立一段正文：
    // 免得同一个规矩在两处各写一遍，改了一处忘了另一处。
    t += L"\r\n";
    t += Msg(L"help.examples") + L"\r\n";
    for (const wchar_t* key : {L"help.example1", L"help.example2", L"help.example3", L"help.example4",
                               L"help.example5", L"help.example6", L"help.example7",
                               L"help.example8", L"help.example9"}) {
        t += L"  " + Msg(key) + L"\r\n";
    }
    return t;
}

std::wstring VersionText() {
    // minWindowsBuild 是本工具**对外声明**的最低 Windows 内部版本，与运行时那道能力检查取的是
    // 同一个数（src/SystemCompat.h 的 kSupportedMinBuild），所以脚本不必先读文档就能问出
    // "这个 exe 打算在哪种系统上工作"。它是声明，不是实测范围：实测只有 README
    // 《系统支持》那一节写的那一个版本。
    return std::wstring(L"EvernightCapture ") + kVersion + L"  (ECAPTURE.EXE / x64)  stage=" + kStage +
           L"  minWindowsBuild=" + std::to_wstring(os_floor::kSupportedMinBuild) + L"\r\n";
}

// ---------------------------------------------------------------------------
// 响应组装
// ---------------------------------------------------------------------------
int BuildResponse(const ParseResult& parse, int argc, wchar_t* const* argv, Response* out) {
    const Options& opt = parse.options;
    out->body.clear();
    out->toStderr = false;

    // 查询命令（--capabilities / --diagnostics / --screens / --list / --inspect）与
    // --version / --help 是几个不同的出口，而且互斥（解析层把同时给出判成
    // cli.query_conflict / cli.window_query_conflict）。
    // 所以这里必须**先**认查询这一路：参数不合查询契约时不能落到帮助那一段去，
    // 否则调用方永远看不到那条冲突。
    if (opt.WindowQueryMode()) {
        if (parse.ok) {
            // 只读的窗口发现与检查：一个像素都不取、不弹框、不写文件、不激活任何窗口
            //（判据见 src/WindowQuery.h，真机问答见 src/WindowQueryRun.cpp）。
            const WindowQueryResult wq = RunWindowQuery(opt);
            const std::wstring contract =
                opt.windowAction == WindowAction::kInspect ? L"windowinspect"
                                                            : kWindowQueryContractName;
            out->body = RenderWindowQuery(wq, contract, opt.verbose, opt.quiet);
            if (!out->body.empty() && out->body.back() != L'\n') out->body += L'\n';
            out->exitCode = wq.exitCode;
            // 窗口查询没有图片要挤 stdout（输出路径这一路恒为空），所以结果恒走 stdout。
            // 这里仍然问同一个 ResultGoesToStderr，不在两处各写一套流规则。
            out->toStderr = ResultGoesToStderr(opt);
            return out->exitCode;
        }
        // 参数在这一路就说不通（与截图那一级冲突、条件写坏）：交回与截图那一份**同形**的失败
        // 文档（captured=0 / images=[] / errors=[…]），调用方按 errors[].code 分支的那段代码
        // 不必为窗口查询另写一份。这与 --capabilities 那一路的处理完全对称。
        Json fj;
        fj.Obj();
        fj.Key(L"captured").Value(0);
        fj.Key(L"images").Arr().End();
        std::vector<Diagnostic> ferr = parse.errors;
        for (auto& d : ferr) {
            if (d.stage.empty()) d.stage = stages::kParse;
        }
        // DiagnosticArray 只写那一段数组本体，键名要调用点先写（与下面正常截图那一路同一写法）。
        fj.Key(L"errors");
        DiagnosticArray(fj, ferr);
        if (!parse.warnings.empty() && !opt.quiet) {
            fj.Key(L"notes");
            DiagnosticArray(fj, parse.warnings);
        }
        fj.End();
        out->body = fj.Str() + L"\n";
        out->exitCode = EX_USAGE;
        out->toStderr = ResultGoesToStderr(opt);
        return EX_USAGE;
    } else if (opt.QueryMode()) {
        if (parse.ok) {
            if (opt.screens) {
                // 只读的屏幕枚举：一个像素都不取、不弹框、不写文件、不改显示设置
                //（判据见 src/ScreenQuery.h，屏幕身份的问答见 src/ScreenIdentity.h）。
                const ScreenQueryResult sq = RunScreenQuery();
                out->body = RenderScreenQuery(sq, opt.verbose, opt.quiet);
                out->exitCode = sq.exitCode;
                out->toStderr = ResultGoesToStderr(opt);
                return out->exitCode;
            }
            const EnvQueryKind kind =
                opt.diagnostics ? EnvQueryKind::kDiagnostics : EnvQueryKind::kCapabilities;
            const EnvReport report = BuildEnvReport(ProbeEnvFacts(), kind);
            out->body = RenderEnvJson(report, opt.verbose, opt.quiet);
            out->exitCode = EX_OK;
            // 查询没有图片要挤 stdout（输出路径这一路恒为空），所以结果恒走 stdout。
            // 这里仍然问同一个 ResultGoesToStderr，不在两处各写一套流规则。
            out->toStderr = ResultGoesToStderr(opt);
            return EX_OK;
        }
        // 落到下面的错误 JSON：形状与每一条解析错误完全相同（captured=0、images=[]、errors=[...]），
        // 调用方按 errors[].code 分支的那段代码不必为查询另写一份。
    } else if (opt.showVersion) {
        out->body = VersionText();
        out->exitCode = EX_OK;
        return EX_OK;
    } else if (opt.showHelp) {
        if (!opt.helpReason.empty())
            out->body += Msg(L"help.no_condition") + L"\r\n\r\n";
        out->body += HelpText();
        const int code = opt.helpReason.empty() ? EX_HELP : EX_NO_CONDITION;
        out->exitCode = code;
        return code;
    }

    Json j;
    std::vector<Diagnostic> errors = parse.errors;
    std::vector<Diagnostic> notes = parse.warnings;
    std::vector<CapturedImage> images;
    int code = parse.ok ? EX_OK : EX_USAGE;

    if (parse.ok) {
        // --capture 的取值在解析期就已限定为已实现的通道，这里不再做能力判断
        CaptureOutcome outcome = RunCapture(opt);
        images = std::move(outcome.images);
        for (auto& e : outcome.errors) errors.push_back(std::move(e));
        for (auto& n : outcome.notes) notes.push_back(std::move(n));
        code = outcome.exitCode;
    }

    // 省略 --out 与显式 --out - 是同一件事：真实诊断、退出码与已经交出去的图原样送出，
    // 不再把这一条路上的失败统一换成 cli.missing_output（旧行为会把"人拒绝了""没命中窗口"
    // "写坏了文件"都说成"缺少输出路径"，调用方补上 --out 之后又试一次，白打扰人一遍）。
    // "本次没给输出路径"只在它确实是下一步可执行的那一条上以 hint 出现（见 Capture.cpp 里
    // io.stdout_failed 那处），不作为 code 出现。

    j.Obj();
    j.Key(L"captured").Value(static_cast<long long>(images.size()));
    j.Key(L"images");
    WriteImages(j, images);
    // errors 不受 --quiet 影响：调用方失败时必须能看到原因
    if (!errors.empty()) {
        j.Key(L"errors");
        DiagnosticArray(j, errors);
    }
    // --quiet 只影响 notes：opt.quiet 是解析层定过优先级的最终值（-v 与 -q 同时给出时
    // 已经是 false），所以这里不再判一次"verbose 要不要赢"——两处各判迟早不一致。
    if (!notes.empty() && !opt.quiet) {
        j.Key(L"notes");
        DiagnosticArray(j, notes);
    }
    if (opt.verbose) {
        j.Key(L"input");
        WriteInputEcho(j, opt);
    }
    j.End();
    out->body = j.Str();

    out->exitCode = code;
    // 图片要占用 stdout 时，JSON 改走 stderr，两个通道永不混流。取值与 Main 的兜底路径同源：
    // 半途异常时（见 Main.cpp）也是同一个判断，不会一处说"在 stderr"另一处写到 stdout。
    out->toStderr = ResultGoesToStderr(opt);
    (void)argc;
    (void)argv;
    return code;
}

// ---------------------------------------------------------------------------
// 输出层：UTF-8 直写，绕开 CRT 文本模式；行尾统一 CRLF，避免老式控制台阶梯错位
// ---------------------------------------------------------------------------
namespace {

// 图片写 stdout 的那个时点记在这里：BuildResponse 与 Main 的兜底路径都按同一个事实决定
// 结果送去哪条流，而不是各猜一次给出互相矛盾的答复。
bool g_stdoutClaimed = false;

std::wstring NormalizeNewlines(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'\r' && i + 1 < in.size() && in[i + 1] == L'\n') continue;  // 已有 CRLF
        if (in[i] == L'\n') out += L"\r\n";
        else out.push_back(in[i]);
    }
    return out;
}

bool WriteHandle(DWORD which, const std::wstring& text) {
    HANDLE handle = GetStdHandle(which);
    if (!handle || handle == INVALID_HANDLE_VALUE) return false;
    const std::wstring body = NormalizeNewlines(text);
    const int need = WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    std::vector<char> buf(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, body.data(), static_cast<int>(body.size()), buf.data(), need,
                        nullptr, nullptr);
    DWORD written = 0;
    return WriteFile(handle, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == static_cast<DWORD>(buf.size());
}

}  // namespace

bool EmitStdout(const std::wstring& text) {
    // 图片已占用 stdout 时这里就是硬边界：文字一律改道，绝不让 JSON 混进 PNG 里。
    // 返回 false 表示"没送到约定通道"，由调用方决定退出码（见 Main.cpp）。
    if (StdoutClaimed()) return false;
    return WriteHandle(STD_OUTPUT_HANDLE, text);
}

bool EmitStderrRaw(const std::wstring& text) { return WriteHandle(STD_ERROR_HANDLE, text); }

bool ResultGoesToStderr(const Options& opt) { return opt.output == L"-" || StdoutClaimed(); }

void ClaimStdout() { g_stdoutClaimed = true; }

bool StdoutClaimed() { return g_stdoutClaimed; }

bool EmitStdoutBytes(const std::vector<uint8_t>& bytes, DWORD* ioError, uint64_t* emittedBytes) {
    if (emittedBytes) *emittedBytes = 0;
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!handle || handle == INVALID_HANDLE_VALUE) {
        // 句柄本身就取不到：这里 GetLastError 早就不是失败原因了，照实报"句柄无效"
        if (ioError) *ioError = ERROR_INVALID_HANDLE;
        return false;
    }
    // 写第一块字节之前就声明归属：哪怕这次只发出去半张图，stdout 也不能再给文字用。
    ClaimStdout();
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        const BOOL ok = WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr);
        if (!ok || written != chunk) {
            // 立刻取错误码：断管 / 磁盘满 / 句柄失效在这一步是三种不同的故障，
            // 调用方要靠它区分，晚一步就被后续 API 覆盖了。
            if (ioError) *ioError = GetLastError();
            // 已经排出去的那一段收不回来：把它照实交回去，别让调用方以为管道还是干净的。
            if (emittedBytes) *emittedBytes = ok ? offset + written : offset;
            return false;
        }
        offset += written;
    }
    if (emittedBytes) *emittedBytes = offset;
    return true;
}

}  // namespace ecapture
