#pragma once
// EvernightCapture - 结构化的窗口发现与检查（--list / --inspect）
//
// 为什么要有这一层：在此之前，调用方（尤其是 AI）想知道「哪些窗口命中这批条件、它们各自是什么」
// 只有一条路 —— 要么跑 `--dry-run` 再从 note.dry_run 那**一行文字**里把 hwnd / pid / 矩形 /
// 类名 / 标题抠出来（拼出来的东西没有类型、没有字段名，标题里带空格或竖线就会解析错位），
// 要么真去截一张图。后者更糟：它要求一个输出路径、会按截图那一级弹确认框，而且「命中多扇窗口」
// 在截图语境里是错误（退出码 5），在「我只是想看看有哪些窗口」的语境里却是完全正常的答复。
//
// 这一层把「窗口有什么」变成一次**只读**的结构化问答，规矩五条：
//
//   1. **一个像素都不取。** 不调用任何取帧通道，不建 D3D/GDI 的屏幕取样，不弹确认框，不写文件，
//      不联网。它只问 user32/kernel32 那几个不往目标线程发消息的判据（句柄、归属进程、类名、
//      窗口矩形），外加同一次枚举里顺手问到的归属进程事实。
//   2. **不动目标。** 不恢复、不激活、不置顶、不移动任何窗口 —— 列一遍窗口不该改变屏幕上的样子。
//   3. **复用截图那同一套匹配语义，但不复用截图那一级后果。** 条件求值走同一个
//      EnumerateMatches / IsolatedMatch（同类 OR、跨类 AND、--monitor 按屏过滤、期限与辅助进程
//      那一条判据也一样），所以「列出来的窗口」与「真去截图会选中的窗口」不可能各按一套规则算。
//      差别只在消歧：--list 把多匹配当正常结果分页交出去；--inspect 需要唯一目标时才按同一套
//      选择策略判歧义，歧义就是 match.ambiguous_window + 退出码 5，**绝不替用户挑一个**。
//   4. **列表是一份快照，会过期。** 交回的每个字段都是问那一刻的值；句柄会被复用、标题会变、
//      进程会退出。真正截图时仍要按《窗口选择与身份一致性》那一节复核，这一层不是、也不能是
//      一种可以长期持有的凭证（每次都发一条 note.window_query_stale 说这件事）。
//   5. **问不出来就说问不出来。** 跨进程的问答有三种下场（读到了 / 被挡下 / 问过而失败），
//      字段级一律写成 readable / denied / failed 加系统原因码，不拿空值冒充答案，也不因为
//      「读不到」就建议改用管理员身份再跑一遍 —— 那是一条本工具无法兑现的建议。
//
// 隐私：默认不写归属映像的**完整路径**（那里面常含用户名，例如安装位置在某个用户目录下），
// 只写映像文件名；要完整路径得显式给 --inspect=path。标题、类名是窗口自己公开的身份，原样交付，
// 不做任何「拼成人话」的加工 —— 调用方按字段读，不该再解析一段描述字符串。
//
// 可见性策略（默认，写在文档的 policy 段里而不是只写在这里）：不可见与零尺寸的窗口根本不进
// 快照，与截图链路的枚举同一条规则；最小化的窗口默认也不进列表，要 --list=all。
// 系统窗口没有可判的身份（没有公开 API 说「我是系统窗口」），所以这一层对它们不作任何断言。
//
// 判据本体（MakeWindowQuerySpec / BuildWindowQueryResult / MakeWindowRecord /
// WindowIdentityOf / RenderWindowQuery）全是纯函数：不碰 Win32、不弹框、不取帧，
// 所以「无匹配 = 空列表而不是错误」「分页边界」「inspect 的多匹配报歧义」「字段级不可读」
// 「路径有没有漏进默认那份」这些形状都由 tests\windows_state.cpp 注入假候选逐条判。
// 真机问答那一步在 WindowQueryRun.h —— 它要枚举真实窗口、并按截图同一条判据把求值整步交给
// 辅助进程，那些都不适合放进离线判据的可执行文件里（那边注入假候选就够了）。

#include <cstdint>
#include <string>
#include <vector>

#include "CliOptions.h"
#include "WindowIdentity.h"
#include "WindowMatch.h"

namespace ecapture {

// 一次窗口查询的取舍（数字与开关由解析层判过区间后交进来）。
struct WindowQuerySpec {
    uint64_t offset = 0;              // --offset
    uint64_t limit = 0;               // --limit，0 = 用默认值
    bool includeIconic = false;       // --list=all
    bool exePath = false;             // --inspect=path（写不写完整路径）
    WindowAction action = WindowAction::kNone;
    // --inspect 需要的选择策略与索引。判据直接复用截图那一份的 MultiMatch，
    // 所以「inspect 说歧义」与「截图说歧义」是同一条线算出来的。
    MultiMatch inspectPolicy = MultiMatch::kAsk;
    int inspectIndex = 1;
    uint64_t timeoutMs = 0;           // 只为 -v 的回显；实际预算由 Deadline 带着走
    std::wstring monitorLabel;        // all / primary / 编号；空 = 没给 --monitor
    // 规范化后的条件，只在 --verbose 时写进 input 段。**由 BuildWindowQueryResult 就地填**
    //（它拿到的就是这一次参与求值的那份 MatchOptions），不从 Options 另抄一遍。
    std::vector<std::wstring> echoHwnds;
    std::vector<std::wstring> echoPids;
    std::vector<std::wstring> echoProcesses;
    std::vector<std::wstring> echoExePaths;
    std::vector<std::wstring> echoTitles;
    std::vector<std::wstring> echoTitleContains;
    std::vector<std::wstring> echoTitleRegexes;
    std::vector<std::wstring> echoClasses;
};

// 命中列表的一份快照（可见窗口与最小化窗口两段，与截图链路的枚举同一套划分）。
struct WindowQuerySnapshot {
    std::vector<WindowInfo> hits;     // 可见且命中，按当下 Z 序
    std::vector<WindowInfo> iconic;   // 命中但最小化
};

// ---------------------------------------------------------------------------
// 结果里的窗口条目：每个字段都是「当时问出来的值 + 这一问的下场」。
// 不可读的值一律用哨兵（0 / 空串）并把对应的 read 写成 denied / failed，
// 所以调用方不会把一个读不到的 0 当成「真的是 0」。
// ---------------------------------------------------------------------------
struct WindowRecord {
    std::wstring hwndHex;             // 恒有：这个值就是判据本身，问不出来就没有这一条
    std::wstring title;               // 原样交付，不截断、不拼接、不加引号
    std::wstring windowClass;
    std::wstring imageName;           // 映像文件名，问得出来才有
    std::wstring imagePath;           // 完整路径；只在 --inspect=path 且问得出来时填
    uint64_t pid = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    bool iconic = false;
    bool visible = true;              // 默认策略下的条目恒为真；--list=all 时说明这一层
    int32_t zOrder = 0;               // 当下叠放次序中的位置，0 = 最前
    // 三条跨进程问句各自的下场：readable / denied / failed（ReadState 的机器名）。
    // 值与下场必须成对读：读不到的那几项，上面那些数是哨兵而不是答案。
    std::wstring processRead;         // 归属进程开句柄 + 两条问句的综合下场（取更糟的那一个）
    std::wstring imagePathRead;
    std::wstring startRead;           // 进程创建时间那一问
    std::wstring rectRead;            // 窗口矩形那一问
    uint32_t imagePathWin32 = 0;      // 只有对应的 read 不是 readable 时才有意义
    uint32_t startWin32 = 0;
    uint32_t rectWin32 = 0;
    bool exePathRequested = false;    // --inspect=path 给了（想写完整路径）
    bool exePathIncluded = false;     // 这一条真把 imagePath 写出来了
    bool exePathReadable = false;     // 这一问有答案；想写而问不出来时报告里要看得见
    // 与 images[] 同形的那一组身份字段（--list 与 --inspect 共用一个形状）。
    // 进程创建时间不在这里另存一份：它就在 identity 里，两处各存迟早打脸。
    WindowIdentity identity;
};

// ---------------------------------------------------------------------------
// 一次窗口查询的完整结果（判据的输出，也是渲染器的唯一输入）。
// ---------------------------------------------------------------------------
struct WindowQueryResult {
    // 条件**全部命中**的条数，分页之前。--list=all 时含最小化那些。
    size_t matched = 0;
    // 命中但被「默认只列可见窗口」这条策略挡掉的条数（includeIconic 为假时 matched 不含它们）。
    size_t iconicExcluded = 0;
    // 本次真正交回的条数（= windows.size()）。
    size_t returned = 0;
    uint64_t offset = 0;
    uint64_t limit = 0;             // 生效的那一个：--limit 写的值，或默认值
    bool limitDefaulted = false;    // 本批是被默认条数截断的，而不是调用方指定的
    bool truncated = false;         // offset + limit 之后还有
    // --inspect 唯一确定的那一扇（为真时 targetHwnd 可用，形状与 errors[].target 同源）。
    bool hasTarget = false;
    uint64_t targetHwnd = 0;
    // 0 = 文档正常出完（包括「一条都没命中」的空列表）。歧义 5 / 越界 1 / 无匹配（inspect）4 /
    // 期限与求值失败 7 —— 与截图那套退出码同一张表，不新增编号。
    int exitCode = 0;
    // 生效的那一份取舍（含 -v 要回显的条件）。判据负责填进去，渲染层据此出 input 段 ——
    // 不在渲染处从 Options 现推，免得「报告里写的 limit」与「实际分页用的 limit」各算一份。
    WindowQuerySpec spec;
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> notes;
    std::vector<WindowRecord> windows;
};

// 由规范化后的选项做出这次查询的取舍（含 -v 的 input 段要回显的那几组条件）。
// 只搬值、不判断据：区间在解析期已经判完，这里不许再判一次（两处各判迟早不一致）。
WindowQuerySpec MakeWindowQuerySpec(const Options& opt);

// 由一条候选做出结果条目 + 身份快照。纯函数：不碰 Win32，所以不可读的现场能注入判。
WindowRecord MakeWindowRecord(const WindowInfo& w, const WindowQuerySpec& spec);

// 交回「后续截图能带回的身份约束字段」：句柄 + 归属 PID + 该 PID 的进程创建时间 + 类名，
// 外加「当初是不是要靠重跑条件才认出它」这一条（易变属性按条件判，不逐字比标题）。
// 这些是**判据**，不是许可、不是凭证：截图那一次仍要复核，而且复核不保证竞态窗口为零。
void WindowIdentityOf(const WindowIdentity& id, std::wstring* hwndHex, std::wstring* className,
                      uint32_t* pid, uint64_t* processStartTicks, bool* selectionNeedsRecheck);

// ReadState 的机器名（readable / denied / failed）与那三值枚举同源，写在 WindowMatch.h：
// 屏幕那一路（--screens）交回的是同一个词表，不在两处各写一份。

// 一条诊断 -> 退出码。判据与 RunCapture 里那一条同源（同一批 code 决定调用方的下一步），
// 窗口查询与截图两处必须给同一个数，所以这份映射只写在这里一次。0 不在其中：
// 窗口查询里「一个都没命中」是空列表 + 0，只有 --inspect 需要唯一目标时才报 match.no_window + 4。
int WindowQueryExitCodeFor(const std::wstring& code);

// ---------------------------------------------------------------------------
// 条件求值那一步失败（本进程枚举，或整步交给辅助进程的那一条路线）-> 查询结果里的那一条。
// 判据只有一条：**原样保留求值那一步给出的稳定码**，只把 match.timeout 的 hint 换成
// 「一次窗口查询没有通道可换」那一版，退出码照 WindowQueryExitCodeFor 判。
// 这里绝不把 capture.worker_failed / cli.invalid_regex 这类已经有专门语义的码包成
// capture.failed —— 那等于把「查执行环境」与「再截一次」抹成同一个下一步。
// 真该包的那一种（内部异常）在 BlockedToDiagnostic 那一条现有边界里就已经是 capture.failed。
// ---------------------------------------------------------------------------
void RecordMatchFailure(WindowQueryResult* result, Diagnostic err);

// ---------------------------------------------------------------------------
// 判据：从一份快照算出这次查询的条目与诊断。
//   * --list    多匹配不是错误：分页、如实报告命中总数；一个都没命中也是空列表 + 成功
//   * --inspect 需要唯一目标：按与截图同一套的选择策略消歧，歧义/越界/无匹配都是稳定诊断
// ---------------------------------------------------------------------------
WindowQueryResult BuildWindowQueryResult(const WindowQuerySnapshot& snapshot,
                                         const WindowQuerySpec& spec, const MatchOptions& match,
                                         const std::wstring& monitorLabel);

// ---------------------------------------------------------------------------
// 渲染：只读的窗口查询文档（与 --capabilities / --diagnostics 同级的**另一份契约**，
// 所以带 contract / contractVersion，也**不**塞进截图那份精简 JSON 的形状里）。
// 机器可读的取值全 ASCII；标题 / 类名 / 映像名是窗口自己的内容，原样输出（可以是中文）。
// verbose = 追加 input 段（规范化后的这一次查询）；quiet = 只省略 notes，caveats 不省略。
// 契约名由调用方给：--list 是 windowquery，--inspect 是 windowinspect —— 两份的形状不同
//（一个是数组、一个是单个对象），不让调用方拿同一份解析器去猜。
// ---------------------------------------------------------------------------
std::wstring RenderWindowQuery(const WindowQueryResult& result, const std::wstring& contract,
                               bool verbose, bool quiet);

// 窗口查询文档的契约名与版本（与 EnvReport 那份 kEnvContractVersion 各自独立演进）。
inline constexpr const wchar_t* kWindowQueryContractName = L"windowquery";
inline constexpr uint32_t kWindowQueryContractVersion = 1;

// caveats 的稳定 token：这一组说的都是「这份文档**没有**断言什么」，
// 与 --capabilities 那份的 caveat.* 同一思路（只增不改名）。
namespace window_caveat {
// 每个字段都是问那一刻的值，会过期；截图时仍要身份复核。
inline constexpr const wchar_t* kSnapshotExpires = L"snapshot_expires";
// 交回的身份字段不是许可、不是凭证，不能拿来代替确认或跳过复核。
inline constexpr const wchar_t* kIdentityNotToken = L"identity_fields_are_not_a_token";
// 这一次没有取任何像素。
inline constexpr const wchar_t* kNoCapture = L"no_capture_performed";
// 这一次没有弹确认框，也没有恢复 / 激活 / 移动任何窗口。
inline constexpr const wchar_t* kNoDialog = L"no_consent_dialog_shown";
inline constexpr const wchar_t* kNoWindowTouched = L"no_window_touched";
// 默认只列可见且非零尺寸的窗口（与截图链路的枚举同一条策略）；最小化的要靠 --list=all。
// 系统窗口没有可判的身份，所以本工具不声称能区分它们。
inline constexpr const wchar_t* kVisibilityPolicy = L"invisible_and_zero_sized_excluded";
// 被挡下的问答只报「读不到」，不据此断言目标截不到，也不建议改用管理员身份。
inline constexpr const wchar_t* kDeniedNotGuarantee = L"unreadable_fields_are_not_a_prediction";
// 本批没有交回全部命中（分页截断，或有最小化窗口被默认策略挡掉）：windows[] 的条数
// 不等于「整机满足条件的窗口数」，后者看 pagination.matched。
inline constexpr const wchar_t* kListMayBePartial = L"list_may_be_partial";
}  // namespace window_caveat

}  // namespace ecapture
