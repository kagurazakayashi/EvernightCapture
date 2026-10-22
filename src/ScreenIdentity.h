#pragma once
// EvernightCapture —— 一块屏有哪几种"名字"，以及它们各自稳在哪一层
//
// 为什么要有这一层：在此之前，调用方要说"我要那一块屏"只有一种写法 ——
// `--monitor <n>`，那个 n 是**本次进程这一次屏幕枚举里的位置**（Windows 是
// EnumDisplayMonitors 的顺序，X11 是 XRRGetMonitors 交回的监视器列表顺序，见
// core/Platform.h 的 MonitorOrdinalKind）。它既不等于「显示设置」里显示的标识号，
// 也不保证插拔、改分辨率、换接口之后还指同一块屏。
// 让调用方（尤其是 AI）去猜一个数字，猜中了出图、猜错了出一张没人批准过的画面，
// 是这条链里最坏的失败形状。所以这里把"这块屏是谁"拆成几种各自说不同事情的事实，
// 并给每一种配一条**能与事实对上**的选择器写法。
//
// 身份**不是每个平台都有同一套**，所以下面逐条写明它在哪种系统上存在、叫什么、稳到哪一层。
// "本构建到底有哪几条、哪条能拿去点名"以 core/Platform.h 的那几条事实为准（解析期拒绝、
// --help 的取值提示、--screens 的 usableAsSelector 读的都是它）；本节只解释为什么。
//
//   编号（ordinal）  本次枚举里的第几块，1 起。两个平台都有，`--monitor <n>` 两边都收，
//            但**给出这个顺序的枚举不是同一个东西**：Windows 是 EnumDisplayMonitors 的驱动
//            枚举顺序，X11 是 XRRGetMonitors 交回的监视器列表顺序（见 MonitorOrdinalKind）。
//            共同点才是要紧的那一条：它只说明"这次枚举里的位置"——既不等于「显示设置」里
//            那个标识号，也不保证插拔、改分辨率、换接口之后还指同一块屏。
//
//   设备名（deviceName）  "本次桌面连接里这块逻辑显示器叫什么"。两个平台都有这一层，也都
//            能用 `device:` 点名：Windows 是 GDI 视图设备名 `\\.\DISPLAY1`；X11 是
//            XRandR monitor 名（拿不到时退回第一个输出名或 `X11-<编号>`）。它比编号稳
//            （同一块屏在本次连接里一直是这个名字），但名字是系统按连接顺序**发**出去的：
//            拔掉重插、换接口、禁用再启用之后可能发给另一块屏。
//
//   X11 输出名（outputName）  物理输出设备这一层（`HDMI-1` / `eDP-1`）。镜像模式下多个输出
//            共享同一个逻辑显示器，`--monitor output:<name>` 因此仍落到那个唯一逻辑范围；
//            对不上或对上一个以上逻辑范围时照实报歧义，绝不替人挑。该标识只在本次桌面连接
//            成立；RROutput XID 只在本次 X server 会话内可比较，不作选择器也不跨会话持久化。
//
//   ---- 以下三层只有答得上显示配置的平台（当前是 Windows，QueryDisplayConfig）才有 ----
//
//   监视器 devnode 路径  `\\?\DISPLAY#GSM41A2#5&...&0#UID8388688#{...}`。设备接口路径由
//            设备节点自己决定，跨会话、跨重启成立，是这几条里唯一能"下一次开机还认得它"的
//            一条。选择器：`--monitor=id:...`。边界：它标识的是"接在这个适配器输出上的这台
//            监视器"，换一根线、换一块显卡上的接口就是另一个节点；跨重启没有实测
//            （见 --screens 文档里的 caveats）。
//
//   适配器 LUID + 目标号  `DISPLAYCONFIG_PATH_TARGET_INFO` 那两个数。LUID 顾名思义是
//            **本地唯一**：它只在这次登录会话（这次开机）里唯一，重启就换。所以它适合用来
//            回答"这两块屏是不是同一块显卡带出来的"，不适合用来长期点名，本工具因此
//            **不**给它选择器，只在 --screens 里作为关联信息交回。
//
//   适配器设备接口路径  适配器自己的 devnode 路径，跨会话那一条；同样只作关联、不作选择器。
//
//   --screens 的字段集不随平台增减（契约稳定），变的是每一条背后的**事实**：答不上这些
//   问句的平台，那三个 key 照旧出现，但 usableAsSelector=false、不写 selectorForm、每块屏
//   不带对应取值，ScreenFacts.hasFacts 恒为 false —— 交回"本构建答不上来"，绝不拿"看起来
//   像"的设备名去冒充跨会话标识，也不把问不出来折叠成没有。
//
// 四条规矩（改代码前先对齐这里）：
//
//   1. **问不出来 ≠ 没匹配上，也 ≠ 匹配上了。** 每一问各自带下场（ReadState + 失败点
//      当场取走的错误码：Windows 那一路写 GetLastError 原值，shcore 的写 HRESULT；没有
//      这两条 API 的平台就只留 ReadState，`win32` 键不出现）。显示配置那一路整个没能回答时，
//      按标识选屏报 `match.monitor_id_unverifiable`，而不是"找不到"，更不是"那就用主屏"。
//   2. **绝不静默换一块屏。** 编号/主屏/all 这三种老写法的语义一条不动；两种标识写法
//      （device: / id:；本构建不收的那一条在解析期就报 cli.platform_unsupported）定位不到
//      唯一一块屏时（没有 / 有多个）一律报错，交回候选列表让人重新点名。
//   3. **取帧之前按身份复核。** 选定之后、读像素之前要再核对一次"还是不是那块屏"：当初问得到
//      devnode 路径就按路径核（名字被系统重新发出去了要能发现），当初问不到就照旧按设备名核，
//      并**不**因为核不到就升级成失败 —— 那是一次没做出来的判定，不是失败的判定
//      （与 WindowIdentity.h 那条"基线当时就没有的判据整个跳过"同源）。
//   4. **这一层只读。** 不取一个像素、不弹确认框、不写文件、不联网，也不改任何显示设置：
//      本工具绝不为了"看清旋转"去改显示配置（Windows 是 ChangeDisplaySettings /
//      SetDisplayConfig，X11 是 XRRSetCrtcConfig 这一类）。
//
// 判据本体（SelectScreenCandidates / CompareScreenIdentity / RecheckScreenSampling /
// MonitorSelectorLabel / ScreenIdEquals）是纯函数，候选表由调用方交进来，所以"设备名被重新发给另一块屏""同一标识命中两块""显示配置问不出来"
// 这些现场都由 tests\screens_state.cpp 注入逐条判 —— 这台机器只接了一块屏，
// 多屏、旋转屏、热插拔的现场在本机造不出来（照实记未验证，不伪造）。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "ScreenMatch.h"
#include "WindowMatch.h"   // ReadState（与窗口那一路同一个三值枚举，线上取值同源）
#include "core/Geometry.h"  // PhysRect：批准区域与屏幕矩形（共用坐标模型）

namespace ecapture {

// 一问的下场。Windows 那一路的两条 API 用两套错误码：user32/GDI 写 GetLastError() 原值（win32），
// shcore 返回 HRESULT（hresult 写成 0x… 形式，与 Diagnostic 的那个字段同形）。没有这两条 API 的
// 平台两项都不填。空的那一个不写进 JSON，所以调用方不会拿 0 当成"报了个零"。
struct ScreenQuestion {
    ReadState read = ReadState::kFailed;
    uint32_t win32 = 0;
    std::wstring hresult;
};

// 一块屏的附加事实（来自显示配置那一路）。当前只有 Windows 实现这一路；其他平台的
// EnumScreenCandidates 交回 hasFacts=false，下面每一条问句都停在"没问过"。
// 默认值一律是"这一问没有答案"而不是"答案是空/0/false"：
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

// 一条候选 = 枚举那一份事实（ScreenInfo，两个平台都有）+ 显示配置那一份事实
// （ScreenFacts，当前只有 Windows 填得上）—— 两半分开放，是因为前者没它就选不了屏，
// 后者只影响能不能按跨会话标识点名与复核。
struct ScreenCandidate {
    ScreenInfo screen;
    ScreenFacts facts;
};

// 这一次的选择器要不要问显示配置那一路。编号 / 主屏 / all / 设备名都不需要 ——
// 为一块按名字点名的屏去跑一遍 QueryDisplayConfig 是把一次廉价问答变贵，而 id: 那一条
// 没有别的办法认屏（而 id: 本身只有答得上显示配置的平台收，见 core/Platform.h）。
bool MonitorSelectorNeedsIdentity(const MonitorSelector& sel);

// 标识比对。设备名与 devnode 路径都折叠大小写比（不判区域规则，也不做任何"归一化"）：
// Windows 上这与系统认设备名的办法一致；X11 的输出名本来是区分大小写的 atom，折叠是让手拄
// --screens 输出的调用方少踩一个大小写坑，属于本工具有意的宽松，不是系统判据。
bool ScreenIdEquals(const std::wstring& a, const std::wstring& b);

// 真机问答。needFacts=false 时只做 EnumScreens，facts 全部保持"没问过"。
// 不取像素、不弹框、不写文件、不联网、不改任何显示设置。
// 【平台入口】实现见 src/platform/windows/WinScreenSource.cpp（EnumDisplayMonitors）与
// src/platform/linux/PosixScreenSource.cpp（XRRGetMonitors）；判据本体（选择 / 复核）留在共用层。
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
    // kDesktop 不是一个候选屏：它必须按根窗口布局单独解析。这条值只用于防止内部把
    // `SelectScreenCandidates` 误当成整桌面入口。
    kNotAMonitor,
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

// 当初问得到 devnode 就按 devnode 核（devnode 只有 Windows 问得到；名字被系统重新发给别的
// 面板时只有这条发现得了），
// 当初问不到就照旧按设备名核 —— 与 CompareScreen 的旧行为一致，不新增失败。
ScreenIdentityCheck CompareScreenIdentity(const ScreenCandidate& wanted,
                                          const std::vector<ScreenCandidate>& current,
                                          ScreenCandidate* fresh);

// ---------------------------------------------------------------------------
// 确认之后、采样之前的采样入口复核（纯函数：当下的那张候选表由调用方交进来）
// ---------------------------------------------------------------------------
// CompareScreenIdentity 把的是"选目标到弹框前"的漂移；这一条把的是另一段时间窗：人点头、
// 桌面授权签发之后，到某条整屏通道真的去读像素之前。取帧与授权不是原子的，这段间隔里
// 屏幕同样可能被拔掉、改名、挪位置。整屏的各条采样入口（Windows 是 wgc / bitblt /
// duplication，X11 是 XGetImage 那一路）都必须在拿到 DesktopPermit 之后、读像素之前过这一问，
// 用回答换出**当下的**那块屏（含当下的屏句柄：Windows 是 HMONITOR）作为采样对象 ——
// 拿选定那一刻的旧句柄直接采，热插拔之后句柄可能已经发给另一块面板。
enum class SamplingRecheck {
    kOk,            // 还是那块屏、形状与批准时一致，且采样矩形仍在刚批准的区域之内
    kStale,         // 确认之后形状/位置变了，或当下矩形超出批准区域：一个像素都不采，
                    // 旧授权不会被用在新布局上（capture.consent_stale）
    kGone,          // 那块屏不在桌面里了，或它的身份已换到另一块面板：不采，也不替代
    kUnverifiable,  // 当初用得上的那一问这次没有答案：不能按"没变"放行
};

// approvedTarget = 授权快照那一刻的候选（身份 + 事实），approvedArea = 判定器刚批准的
// 屏幕矩形（DesktopPermit::Approved()）。kOk 时 *fresh 是本次采样要用的当下候选。
SamplingRecheck RecheckScreenSampling(const ScreenCandidate& approvedTarget,
                                      const PhysRect& approvedArea,
                                      const std::vector<ScreenCandidate>& current,
                                      ScreenCandidate* fresh);

// 选择器的机器写法：primary / all / desktop / 编号 / device:X / output:X / id:X。
// hint、-v 的 input 段与窗口查询那一份回显读的都是这一份，不在三处各拼一遍。
std::wstring MonitorSelectorLabel(const MonitorSelector& sel);

// 选择器是哪**一种**：primary / ordinal / all / desktop / device / output / id
//（稳定 ASCII 取值，只增不改名）。
// 与上面那条标签分开写，因为 -v 的 input.monitor 里编号一直是数字（老契约），
// 而调用方要判"这次到底是按编号还是按标识点的名"，不该靠猜那个字段的类型。
const wchar_t* MonitorSelectorKindName(const MonitorSelector& sel);

}  // namespace ecapture
