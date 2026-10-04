#pragma once
// 用户偏好配置（ECAPTURE.ini）：解析、逐键校验与来源记账。
//
// 这一层只回答两件事：「这个文件写了什么」与「每一个受支持的键最后落在哪个来源」。
// 截图意图、目标选择与授权**一律不在这里**（那些每次由 CLI 给出，见 src/CliOptions.h），
// 所以即使 INI 存在，裸运行仍然一次截图都不会做。
//
// 四条结构性规矩（与 README《配置文件》那一节逐条对应）：
//   1. **解析、类型判定、合并、归一化是四个分开的步骤。** ParseIniBytes 只认语法，不认得任何
//      键名；ConfigSnapshot 按 schema 判类型与区间；CliOptions 那一路才决定"这次用哪个来源"。
//      把 INI 拼成 argv 再跑一遍解析是**错**的：那会把出厂的截图默认当成用户向 --capabilities
//      这类只读命令提交的截图意图，凭空生出 query_conflict。
//   2. **"未指定"与"显式写了 false / 0 / none / unset"是两件事。** 每个键都带 present 标记，
//      合并判据读的是 present，不是"值非零 / 非空"。缺一个键才继承下一档，
//      写了而无效**不**降级成缺省。
//   3. **严格优先于宽容。** 未知 section/键、重复键、缺等号、不闭合引号、非法 UTF-8、NUL、
//      非法数字/布尔/枚举、越界、无效组合都是明确失败，带**文件路径 + 行号 + section/key +
//      稳定诊断码**（--config 那条报错要说清是哪一行）。默认文件不存在是**正常**；
//      存在却读不出或读不懂则本次请求在选目标、弹框、取帧**之前**失败。
//   4. **只读、无副作用。** 这一步不建目录、不写文件、不修配置、不展开环境变量、
//      不做 include / 命令替换 / 网络读取，也不热重载：一次调用读一次快照。
//
// 为什么不用 Windows 的 GetPrivateProfileStringW：它按系统代码页解释字节（不是 UTF-8）、
// 撞上同名的**目录**时会一直阻塞、也不报错，而重复键与未知键它都不管。这三件事本文件都要
// 如实回答，所以自己写一份小而明确的解析器，并在 tests\config_state.cpp 里逐条判它。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "CliOptions.h"  // Diagnostic / codes::config.* / cli_limits（区间与 CLI 共用同一份数）

namespace ecapture {

// ---------------------------------------------------------------------------
// 语法层：字节 -> 条目
// ---------------------------------------------------------------------------

// 一条已经通过语法检查的赋值。section / key 已按小写规范化（ASCII 大小写不敏感），
// value 已去掉成对引号与语法外的首尾空白，但**没有**做任何类型判定。
struct IniEntry {
    std::wstring section;
    std::wstring key;
    std::wstring value;
    size_t line = 0;        // 1 起的源文件行号；诊断的 line 用它
};

// 一条语法或读取问题。code 是稳定的 config.* 码（只增不改名），detail 是不随 --lang 变的
// ASCII 原因 token（例如 "bom:ok" / "byte:0xE9" / "limit:262144"），由渲染层附在 message 末尾。
struct IniIssue {
    std::wstring code;
    std::wstring section;
    std::wstring key;
    std::wstring value;
    std::wstring detail;
    size_t line = 0;
};

// 整份文档的字节上限与单行字符上限。上限是**读之前**就要成立的判据：一份 256 KiB 之外的文件
// 不该先被整个搬进内存再报错（那正是这一条要防的事）。
namespace config_limits {
inline constexpr uint64_t kMaxFileBytes = 256ull * 1024ull;
inline constexpr uint64_t kMaxLineChars = 4096ull;
// 受支持的配置格式版本。目前只有 1；版本键不认识时明确拒绝，不"当成兼容格式猜读"。
inline constexpr uint64_t kSupportedVersion = 1ull;
}  // namespace config_limits

// 纯语法解析：UTF-8 字节 -> 条目 + 问题列表。没有 Win32 依赖，因此离线判据可以直接喂字节。
// 返回 false 的**唯一**含义是"这份文档不可用于读取配置值"（编码不合、超限、语法坏、重复键、
// 未知 section/键）。逐键的类型判定不在这里，在 ApplySchemaToEntries。
// 哪些 section 与键合法只认 ConfigSchema() 这一份表，解析层不另写一份键名。
bool ParseIniBytes(const std::vector<uint8_t>& bytes, std::vector<IniEntry>* entries,
                   std::vector<IniIssue>* issues);

// ---------------------------------------------------------------------------
// 来源模型
// ---------------------------------------------------------------------------

// 一个受支持设置最终生效的那一档来源。三档的优先顺序恒为 cli > config > builtin，
// 而"config 这一档有没有被写出过"看的是 present，不是取值。
enum class SettingSource { kBuiltin, kConfig, kCli };

// 机器名（进 JSON，ASCII，不随 --lang 变）："builtin" / "config" / "cli"。
const wchar_t* SettingSourceName(SettingSource s);

// ---------------------------------------------------------------------------
// schema：一份表同时驱动校验、默认值、--show-config 与示例模板
// ---------------------------------------------------------------------------

// 受支持的取值类型。判定本体在 ConfigSnapshot，与 CLI 共用同一套区间与枚举表。
enum class ConfigValueKind {
    kVersion,   // 只接受 config_limits::kSupportedVersion
    kEnum,      // allowed 里逐条比对（ASCII 小写）
    kBool,      // 只认 true / false
    kDecimal,   // 严格十进制，区间 [minValue, maxValue]
    kText,      // 原样收下（路径与 roi 那四个数由下游按各自的判据判）
};

struct ConfigField {
    const wchar_t* id;          // "output.path"：合并与 --show-config 用的稳定标识
    const wchar_t* section;     // "output"
    const wchar_t* key;         // "path"
    ConfigValueKind kind;
    const wchar_t* const* allowed;   // kEnum 用；nullptr 结尾，其它类型为 nullptr
    uint64_t minValue;          // kDecimal 用
    uint64_t maxValue;          // kDecimal 用
    const wchar_t* defaultValue;     // 出厂语义的**规范化写法**（--show-config 也报它）
    const wchar_t* cliOption;   // 对应 CLI 选项的规范名；空串 = 没有同名 CLI 选项（分组覆盖等）
};

// schema 本体（只读，顺序即 --show-config 与示例模板的书写顺序）。
const std::vector<ConfigField>& ConfigSchema();

// 按 "section.key" 找字段；找不到返回 nullptr。
const ConfigField* FindConfigField(const std::wstring& section, const std::wstring& key);

// 按 schema 的 id 找字段。**id 是文本，不是指针身份**：查找按内容做，不要求同一条字面量在
// 两个翻译单元里落到同一个地址（那取决于字符串合并与链接器的相同折叠，换一种构建配置就变成
// "查不到 => 默认值报成空串"）。找不到返回 nullptr。
const ConfigField* FindConfigFieldById(std::wstring_view id);

// 取一条 schema id 的出厂默认。找不到字段时返回 false **且不写 \*out** ——
// 调用方不许把"未命中"当成一次合法的空默认值（合法的空默认，例如 crop.roi，由 schema 自己给）。
bool ConfigFieldDefault(std::wstring_view id, std::wstring* out);

// 内部调用点报了一条 schema 里没有的 id 时，来源表交出的取值标记。它**不是**任何一项的默认值：
// 出现在 --show-config 里就说明合并那一步缺了一份依据（判据见 tests\config_state.cpp 的查找用例）。
inline constexpr const wchar_t* kNoSchemaDefault = L"<no-schema-default>";

// schema 里出现的每个不同 section（保持首次出现顺序），供语法层判"未知 section"。
const std::vector<std::wstring>& ConfigSections();

// ---------------------------------------------------------------------------
// 快照：一次调用读一次，之后不再变
// ---------------------------------------------------------------------------

// 读配置这一步的整体下场。kAbsent 是**正常**状态（默认位置没有文件），不是失败也不是错误。
enum class ConfigLoadState {
    kNotRead,    // 本次没读：--no-config，或 --help / --version 那一路
    kAbsent,     // 默认位置的 ECAPTURE.ini 不存在 => 全部走内置默认
    kLoaded,     // 文件已读完且逐键通过 schema 判定
    kFailed,     // 读不出 / 语法不合 / 版本不合 => 本次请求在取帧之前失败
};

// 一个键的落地结果：present 说"文件里写没写过"，值永远存**规范化后**的写法（小写枚举、
// 去引号的路径原文），line 是它在源文件里的行号（内置默认时 0）。
struct ConfigSetting {
    bool present = false;
    std::wstring value;
    size_t line = 0;
};

// 一份配置快照：文件位置、读取下场、逐键结果，以及已经归类好的诊断。
// 它**不**知道 CLI 给了什么 —— 合并是 CliOptions 那一步的事，这样同一份快照可以被 --show-config
// 与真实截图两条路共用，而两条路读到的类型判定是同一份代码。
struct ConfigSnapshot {
    std::wstring path;             // 实际读取（或尝试读取）的绝对路径；kNotRead 时为空
    std::wstring requestedPath;    // 用户写下的那一条原文（--config 的取值）；默认位置时为空
    ConfigLoadState state = ConfigLoadState::kNotRead;
    // 逐键结果，下标与 ConfigSchema() 一一对应
    std::vector<ConfigSetting> settings;
    // 已经编好的诊断（含文件路径、行号、section/key、稳定码）。config.* 的失败都排在这里。
    std::vector<Diagnostic> errors;
    std::vector<Diagnostic> notes;

    bool ReadOk() const { return state == ConfigLoadState::kLoaded || state == ConfigLoadState::kAbsent; }

    // 按 schema 的 id 取一个键的落地结果；不存在时返回 nullptr。
    // id 的比较与 FindConfigFieldById 同一条（按内容），下标由 schema 顺序算出。
    const ConfigSetting* Find(const std::wstring& fieldId) const;
    // 该键是否被文件显式写出过（present），未写出的键不参与合并。
    bool Has(const std::wstring& fieldId) const;
    // 该键的规范化取值；没写过时返回空串（调用方先判 Has）。
    const std::wstring& ValueOf(const std::wstring& fieldId) const;
};

// ---------------------------------------------------------------------------
// 取文件与判定：生产入口
// ---------------------------------------------------------------------------

// 实际运行的 ECAPTURE.EXE 所在目录里的 ECAPTURE.ini。问不出模块位置时 path 为空、
// state=kFailed 并带一条 config.read_failed —— 不猜当前目录、不猜源码树、不猜安装位置。
std::wstring DefaultConfigPath();

// 读并判定一个配置文件。这一步只有读与判，没有任何写入：
//   - 文件不存在：默认位置 = kAbsent（正常）；显式 --config = kFailed + config.not_found。
//   - 读不出（被占用、权限、是个目录）：kFailed + config.read_failed，带 win32 原值。
//   - 语法 / 编码 / 上限 / 未知键 / 版本：kFailed，逐条 config.* 诊断。
// 编码非法 UTF-8 不会"按系统代码页试一遍"，也不会在撞上同名目录时阻塞。
ConfigSnapshot LoadConfigFile(const std::wstring& absolutePath, bool explicitSelection);

// 不读文件、只按 schema 建一份"全部未写出"的快照骨架（--no-config 与 help/version 那一路用，
// 于是 --show-config 仍然能把每一项的 builtin 来源报全）。
ConfigSnapshot EmptyConfigSnapshot();

// 把一份已经判定过的条目表按 schema 落成逐键结果，并把类型/区间/版本问题写进 issues。
// 与 LoadConfigFile 共用这一份判据，离线判据因此能直接喂条目而不必碰磁盘。
// 跨键的组合判据（crop.mode 与 crop.roi）与相对路径基准**不在这里**：那些与 CLI 同一条线，
// 写在合并那一步，两处不许各写一套。
void ApplySchemaToEntries(const std::vector<IniEntry>& entries, ConfigSnapshot* snapshot,
                          std::vector<IniIssue>* issues);

// 读取下场的机器名（进 --show-config 与 -v 的 input.config，ASCII，不随 --lang 变）：
// "not_read" / "absent" / "loaded" / "failed"。
const wchar_t* ConfigStateName(ConfigLoadState state);

// 这一条 config.* 是不是"读不出来"那一类（退出码 8），而不是"说不通"那一类（退出码 1）。
// 判据只有这一份：调用方据此决定退出码与"错误 JSON 走哪条流"，不在两处各写一套 code 清单。
bool IsConfigReadFailure(const std::wstring& code);

}  // namespace ecapture
