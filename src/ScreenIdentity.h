#pragma once
// EvernightCapture —— 一块屏有哪几种"名字"，以及它们各自稳在哪一层
//
// 为什么要有这一层：在此之前，调用方要说"我要那一块屏"只有一种写法 ——
// `--monitor <n>`，那个 n 是**本次进程这一次 EnumDisplayMonitors 的顺序里的位置**。
// 它既不等于「显示设置」里显示的标识号，也不保证插拔、改分辨率、换接口之后还指同一块屏。
// 让调用方（尤其是 AI）去猜一个数字，猜中了出图、猜错了出一张没人批准过的画面，
// 是这条链里最坏的失败形状。所以这里把"这块屏是谁"拆成几种各自说不同事情的事实，
// 并给每一种配一条**能与事实对上**的选择器写法。
//
// 三种身份（数字与字符串都只在问出来的那一刻有效，这里不作任何长期承诺）：
//
//   设备名   `\\.\DISPLAY1`。GDI 的视图设备名。同一块屏在本次桌面连接里一直是这个名字，
//            比编号稳；但这个名字是系统按连接顺序**发**出去的，拔掉重插、换接口、
//            禁用再启用之后可能发给另一块屏。选择器：`--monitor=device:DISPLAY1`。
//
//   监视器 devnode 路径   `\\?\DISPLAY#GSM41A2#5&...&0&UID8388688#{...}`。设备接口路径由
//            设备节点自己决定，跨会话、跨重启成立，是这三种里唯一能"下一次开机还认得它"的
//            一条。选择器：`--monitor=id:...`。边界：它标识的是"接在这个适配器输出上的这台
//            监视器"，换一根线、换一块显卡上的接口就是另一个节点；本工具只在开发机上观察过
//            同一会话内的稳定性，跨重启没有实测（见 --screens 文档里的 caveats）。
//
//   适配器 LUID + 目标号   `DISPLAYCONFIG_PATH_TARGET_INFO` 那两个数。LUID 顾名思义是
//            **本地唯一**：它只在这次登录会话（这次开机）里唯一，重启就换。所以它适合用来
//            回答"这两块屏是不是同一块显卡带出来的"，不适合用来长期点名，本工具因此
//            **不**给它选择器，只在 --screens 里作为关联信息交回（跨会话的那一条是适配器
//            自己的设备接口路径，也一并交回）。
//
// 四条规矩（改代码前先对齐这里）：
//
//   1. **问不出来 ≠ 没匹配上，也 ≠ 匹配上了。** 每一问各自带下场（ReadState + 失败点当场取走的
//      Win32 码）。显示配置那一路整个没能回答时，按标识选屏报 `match.monitor_id_unverifiable`，
//      而不是"找不到"，更不是"那就用主屏"。
//   2. **绝不静默换一块屏。** 编号/主屏/all 这三种老写法的语义一条不动；两条标识写法定位不到
//      唯一一块屏时（没有 / 有多个）一律报错，交回候选列表让人重新点名。
//   3. **取帧之前按身份复核。** 选定之后、读像素之前要再核对一次"还是不是那块屏"：当初问得到
//      devnode 路径就按路径核（名字被系统重新发出去了要能发现），当初问不到就照旧按设备名核，
//      并**不**因为核不到就升级成失败 —— 那是一次没做出来的判定，不是失败的判定
//      （与 WindowIdentity.h 那条"基线当时就没有的判据整个跳过"同源）。
//   4. **这一层只读。** 不取一个像素、不弹确认框、不写文件、不联网，也不改任何显示设置：
//      本工具绝不为了"看清旋转"去调 ChangeDisplaySettings / SetDisplayConfig。
//
// 判据本体（SelectScreenCandidates / CompareScreenIdentity / MonitorSelectorLabel）是纯函数，
// 候选表由调用方交进来，所以"设备名被重新发给另一块屏""同一标识命中两块""显示配置问不出来"
// 这些现场都由 tests\screens_state.cpp 注入逐条判 —— 这台机器只接了一块屏，
// 多屏、旋转屏、热插拔的现场在本机造不出来（照实记未验证，不伪造）。

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "CliOptions.h"
#include "ScreenMatch.h"
#include "WindowMatch.h"   // ReadState（与窗口那一路同一个三值枚举，线上取值同源）

namespace ecapture {

// 一问的下场。两条 API 用两套错误码：user32/GDI 那几条写 GetLastError() 原值（win32），
// shcore 那一条返回 HRESULT（hresult 写成 0x… 形式，与 Diagnostic 的那个字段同形）。
// 空的那一个不写进 JSON，所以调用方不会拿 0 当成"报了个零"。
struct ScreenQuestion {
    ReadState read = ReadState::kFailed;
    uint32_t win32 = 0;
    std::wstring hresult;
};

// 一块屏的附加事实。默认值一律是"这一问没有答案"而不是"答案是空/0/false"：
// 没问过（EnumScreenCandidates(needFacts=false)）与问过而失败，靠 hasFacts 与各自的
// read 字段分开，渲染层据此决定写 unknown 还是整个键不出现。
struct ScreenFacts {
    bool hasFacts = false;           // 这一次到底有没有问显示配置那一路

    // 整次 QueryDisplayConfig 的下场（同一份表里各屏共用一条；不成立时下面这些一律没答案）
    ScreenQuestion config;
    // 这块屏在显示配置里有没有对上一条活动路径，以及有几条路径挂在同一个视图设备上
    // （复制模式下两块面板共享一个桌面，那还是一个桌面里的**一块**屏）
    bool hasPath = false;
    uint32_t pathsMatched = 0;

    ScreenQuestion monitorPathQ;                    // 跨会话那一条（`\\?\DISPLAY#…`）
    std::wstring monitorPath;
    std::wstring friendlyName;                      // 监视器自报的名字（可能被用户改过）
    std::wstring adapterLuid;                       // "0x000000000000c2d3"，只在本次会话内唯一
    uint32_t targetId = 0;                          // 该适配器内的目标号（同样只在本次会话内）
    ScreenQuestion adapterPathQ;                    // 适配器自己的设备接口路径（跨会话那一条）
    std::wstring adapterPath;
    std::wstring outputTechnology;                  // hdmi / displayport_external / virtual / …
    bool targetAvailable = false;                   // 显示配置说这块 target 现在在不在
    uint32_t edidManufactureId = 0;
    uint32_t edidProductCode = 0;
    bool edidIdsValid = false;
    std::wstring panelRotation;                     // 相对面板原生朝向（与下面那条不是一件事）

    // 各自独立的两问：DPI（shcore!GetDpiForMonitor，Win8.1 才有）与用户看到的朝向（DEVMODE）
    ScreenQuestion dpi;
    uint32_t dpiEffectiveX = 0;
    uint32_t dpiEffectiveY = 0;
    ScreenQuestion dpiRaw;
    uint32_t dpiRawX = 0;
    uint32_t dpiRawY = 0;
    ScreenQuestion rotation;
    int rotationDegrees = -1;                       // 0 / 90 / 180 / 270，-1 = 没问出来
};

// 一条候选 = GDI 那一份事实（ScreenInfo）+ 显示配置那一份事实（ScreenFacts）。
struct ScreenCandidate {
    ScreenInfo screen;
    ScreenFacts facts;
};

// 这一次的选择器要不要问显示配置那一路。编号 / 主屏 / all / 设备名都不需要 ——
// 为一块按名字点名的屏去跑一遍 QueryDisplayConfig 是把一次廉价问答变贵，
// 而 id: 那一条没有别的办法认屏。
bool MonitorSelectorNeedsIdentity(const MonitorSelector& sel);

// 标识比对。设备名与 devnode 路径都按 Windows 自己的看法逐字符不区分大小写比
// （这两条都不是给人读的文本，而是系统用来认对象的键），取值原样交付、不做任何"归一化"。
bool ScreenIdEquals(const std::wstring& a, const std::wstring& b);

// 真机问答。needFacts=false 时只做 EnumScreens，facts 全部保持"没问过"。
// 不取像素、不弹框、不写文件、不联网、不改任何显示设置。
std::vector<ScreenCandidate> EnumScreenCandidates(bool needFacts);

// ---------------------------------------------------------------------------
// 选择器 -> 候选表里的下标（纯函数，不碰 Win32）
// ---------------------------------------------------------------------------
// 取值只增不改名。每条都有自己那条稳定诊断码与退出码，见下面的 codes::。
enum class ScreenSelect {
    kFound,        // 对上唯一一块（kAll 时是全部）
    kOutOfRange,   // 编号越界，或者这台机器现在一块屏都没有
    kNoMatch,      // 标识写法合语法，但现在没有任何一块屏用它
    kAmbiguous,    // 同一个标识命中多块屏，绝不替调用方挑一块
    kUnverifiable, // 这一问没能给出答案，所以判不出谁对应谁
};

struct ScreenSelectResult {
    ScreenSelect outcome = ScreenSelect::kOutOfRange;
    // kFound：选中的下标（kAll 时是全部）；kAmbiguous：全部命中项（给 hint）。
    std::vector<size_t> picked;
    // 除 kFound 之外恒有一条诊断（带 option/value/hint 与 stage=match）。
    std::vector<Diagnostic> errors;
};

ScreenSelectResult SelectScreenCandidates(const MonitorSelector& sel,
                                          const std::vector<ScreenCandidate>& all);

// 真机入口：按这条选择器决定要不要问显示配置那一路（MonitorSelectorNeedsIdentity），
// 然后交回选中的那几块屏**连同它们的 facts**。屏幕目标在取帧之前还要按 facts 里那条跨会话
// 标识复核一次，所以这一份不能只交回 ScreenInfo —— 那样复核时就没得比了。
// 定位不到唯一一块屏时交回空表 + 一条诊断（绝不"那就用主屏"）。
std::vector<ScreenCandidate> SelectScreenCandidatesOf(const MonitorSelector& sel,
                                                      std::vector<Diagnostic>* errors);

// ---------------------------------------------------------------------------
// 取帧之前的身份复核（纯函数：当下的那张表由调用方交进来）
// ---------------------------------------------------------------------------
enum class ScreenIdentityCheck {
    kSame,          // 还是那块屏，形状与编号都没变
    kMoved,         // 还是那块屏，但矩形/编号/主屏归属变了：换新值重新确认，旧授权不沿用
    kGone,          // 那块屏不在桌面里了，或者它的身份已经换到别的面板上：一个像素都不读
    kUnverifiable,  // 当初问到了跨会话标识，而这一次那一路问不出来了（不等于没了，也不等于没变）
};

// 当初问得到 devnode 就按 devnode 核（名字被系统重新发给别的面板时只有这条发现得了），
// 当初问不到就照旧按设备名核 —— 与 CompareScreen 的旧行为一致，不新增失败。
ScreenIdentityCheck CompareScreenIdentity(const ScreenCandidate& wanted,
                                          const std::vector<ScreenCandidate>& current,
                                          ScreenCandidate* fresh);

// 选择器的机器写法：primary / all / 编号（十进制字符串）/ device:X / id:X。
// hint、-v 的 input 段与窗口查询那一份回显读的都是这一份，不在三处各拼一遍。
std::wstring MonitorSelectorLabel(const MonitorSelector& sel);

// 选择器是哪**一种**：primary / ordinal / all / device / id（稳定 ASCII 取值，只增不改名）。
// 与上面那条标签分开写，因为 -v 的 input.monitor 里编号一直是数字（老契约），
// 而调用方要判"这次到底是按编号还是按标识点的名"，不该靠猜那个字段的类型。
const wchar_t* MonitorSelectorKindName(const MonitorSelector& sel);

}  // namespace ecapture
