#include "Config.h"

#include <algorithm>
#include <limits>

#include "HostFile.h"     // 整份读一个文件：判据共用，动作在各平台的服务里（WinHostFile / PosixHostFile）
#include "HostPath.h"     // 程序所在目录与路径拼接：分隔符与"问不出位置"的下场由平台答
#include "Lang.h"
#include "core/TextCoding.h"  // INI 的严格 UTF-8 解码（整份合才读，第一处坏字节的偏移进 detail）

namespace ecapture {
namespace {

// 每个受支持的键的类型取值表就写在下面这份 schema 里（唯一出处：校验、默认值、--show-config
// 与示例模板都从它读，不另抄一份键名清单）。
//
// 两条取值域与 CLI **故意不同**，因为它们说的是两件 CLI 没有词的事：
//   capture.cursor / capture.hdr 多一条 unset —— "本次没有提出这项要求"，合并那一步把它翻译成
//     "不生成对应的结果键"，而不是 CLI 那条 --cursor default（那是"照通道默认交回，但要报给我"）。
//   output.format 多一条 auto —— "继续按扩展名推断"，不等于 CLI 那条 --format png。
// 与 CLI 重合的那些数（quality 的区间、两条期限的上限、边长与像素天花板、分页上限）一律引用
// cli_limits，不在这里另写一份数字。
const wchar_t* const kCfgLang[] = {L"auto", L"zh-CN", L"zh-TW", L"en", L"ja", nullptr};
const wchar_t* const kCfgVerbosity[] = {L"normal", L"quiet", L"verbose", nullptr};
const wchar_t* const kCfgCapture[] = {L"wgc", L"dwm", L"printwindow", L"bitblt", L"duplication",
                                      L"auto", nullptr};
const wchar_t* const kCfgCursor[] = {L"unset", L"default", L"include", L"exclude", nullptr};
const wchar_t* const kCfgHdr[] = {L"unset", L"auto", L"tonemap", L"refuse", nullptr};
const wchar_t* const kCfgFormat[] = {L"auto", L"png", L"jpg", L"jpeg", L"bmp", L"tiff", L"gif",
                                     nullptr};
const wchar_t* const kCfgCropMode[] = {L"none", L"client-area", L"roi", nullptr};
// notification.detail 的三档与 --notify-detail **完全同一份词表**：配置侧不需要额外的 unset
// 那一档，因为"这次不要弹"写在 notification.enabled = false 里，不是 detail 的一个取值。
// 所以这里引用 option_values 那一份，不在配置层抄第二份拼法（上面那几条各自多出一个配置专用词，
// 才需要自己列一份）。
constexpr const wchar_t* const* kCfgNotifyDetail = option_values::kNotifyDetail;

using cli_limits::kJpegQualityMax;
using cli_limits::kJpegQualityMin;
using cli_limits::kMaxTimeoutMs;
using cli_limits::kMaxWindowListItems;
using cli_limits::kScaleMaxPixels;
using cli_limits::kScaleMaxSide;

// section / key 与枚举取值的"大小写不敏感"说的就是 ASCII 那几个字母：比较前先折成小写。
// 用 ASCII 折叠而不是 towlower：后者跟着 C 运行期 locale 走，会把非 ASCII 文字（例如路径与
// roi 那类 kText 取值）也一起改写字母大小写，那是这套合同没有承诺过的行为。
std::wstring LowerAscii(const std::wstring& s) { return AsciiLower(s); }

bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t'; }

// section / key 的合法字符：ASCII 字母数字与 - _ 。大小写不敏感，比较一律先转小写。
bool IsKeyNameChar(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return true;
    if (c >= L'a' && c <= L'z') return true;
    if (c >= L'0' && c <= L'9') return true;
    return c == L'-' || c == L'_' || c == L'.';
}

std::wstring TrimWs(const std::wstring& s) {
    size_t b = 0;
    while (b < s.size() && IsSpace(s[b])) ++b;
    size_t e = s.size();
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 一条 config.* 诊断。message 用的资源 key 与 code 同名（config.* 那一批在四份 strings 里
// 各有一份），detail 是不随 --lang 变的 ASCII 原因 token，line 是源文件行号（0 = 与行无关）。
// option 放配置文件的位置：这一族要能指到"哪一个文件的哪一行"，而路径本身是这次调用的
// 输入，不是隐私内容（--show-config 与文档里的排障步骤都靠它）。
Diagnostic ConfigDiag(const wchar_t* code, const std::wstring& path, const std::wstring& section,
                      const std::wstring& key, size_t line, const std::wstring& detail) {
    Diagnostic d;
    d.code = code;
    d.message = Msg(code);
    // 补充说明按约定"<code>_hint 有就带、没有就整个字段省略"。这里用 HasMsg 而不是直接 Msg：
    // 后者对不存在的 key 返回 "?key"，那是留给写错的 key 的痕迹，不该当成一句真的文案送出去。
    const std::wstring hintKey = std::wstring(code) + L"_hint";
    if (HasMsg(hintKey.c_str())) d.hint = Msg(hintKey.c_str());
    d.option = path;
    // value 一律写成 "section.key"；只有节名的那一类（未知节、重复节）没有键名，
    // 这时就只报节名，不许凑出一个 "general." 这种看着像键其实不是键的形状。
    std::wstring id = section;
    if (!id.empty() && !key.empty()) id += L"." + key;
    else if (id.empty()) id = key;
    d.value = id;
    d.detail = detail;
    d.line = static_cast<uint32_t>(line > std::numeric_limits<uint32_t>::max()
                                       ? std::numeric_limits<uint32_t>::max()
                                       : line);
    d.stage = stages::kParse;
    return d;
}

// 一份 Windows 路径的"有限长度"写法：报错与 --show-config 都要指得出是哪个文件，
// 而一条几百字符的路径会把整份 JSON 撑开。超长时保留头尾并写明省了多少个字符，
// 不静默截断（静默截断会让人照着改不到那个文件）。
std::wstring BriefPath(const std::wstring& path) {
    constexpr size_t kKeep = 96;
    if (path.size() <= kKeep * 2) return path;
    return path.substr(0, kKeep) + L"...[+" + std::to_wstring(path.size() - kKeep * 2) +
           L" chars]..." + path.substr(path.size() - kKeep);
}

}  // namespace

// ---------------------------------------------------------------------------
// schema
// ---------------------------------------------------------------------------

const std::vector<ConfigField>& ConfigSchema() {
    static const std::vector<ConfigField> kFields = {
        {L"config.version", L"config", L"version", ConfigValueKind::kVersion,
         nullptr, 1, 1, L"1", L""},
        {L"general.language", L"general", L"language", ConfigValueKind::kEnum,
         kCfgLang, 0, 0, L"auto", L"lang"},
        {L"general.verbosity", L"general", L"verbosity", ConfigValueKind::kEnum,
         kCfgVerbosity, 0, 0, L"normal", L""},
        {L"capture.method", L"capture", L"method", ConfigValueKind::kEnum,
         kCfgCapture, 0, 0, L"wgc", L"capture"},
        {L"capture.cursor", L"capture", L"cursor", ConfigValueKind::kEnum,
         kCfgCursor, 0, 0, L"unset", L"cursor"},
        {L"capture.hdr", L"capture", L"hdr", ConfigValueKind::kEnum,
         kCfgHdr, 0, 0, L"unset", L"hdr"},
        {L"output.path", L"output", L"path", ConfigValueKind::kText,
         nullptr, 0, 0, L"-", L"out"},
        {L"output.format", L"output", L"format", ConfigValueKind::kEnum,
         kCfgFormat, 0, 0, L"auto", L"format"},
        {L"output.jpeg_quality", L"output", L"jpeg_quality", ConfigValueKind::kDecimal,
         nullptr, kJpegQualityMin, kJpegQualityMax, L"100", L"quality"},
        {L"output.overwrite", L"output", L"overwrite", ConfigValueKind::kBool,
         nullptr, 0, 0, L"true", L"no-overwrite"},
        {L"crop.mode", L"crop", L"mode", ConfigValueKind::kEnum,
         kCfgCropMode, 0, 0, L"none", L""},
        {L"crop.roi", L"crop", L"roi", ConfigValueKind::kText, nullptr, 0, 0, L"", L"roi"},
        {L"scale.max_width", L"scale", L"max_width", ConfigValueKind::kDecimal,
         nullptr, 0, kScaleMaxSide, L"0", L"scale"},
        {L"scale.max_height", L"scale", L"max_height", ConfigValueKind::kDecimal,
         nullptr, 0, kScaleMaxSide, L"0", L"scale"},
        {L"scale.max_pixels", L"scale", L"max_pixels", ConfigValueKind::kDecimal,
         nullptr, 0, kScaleMaxPixels, L"0", L"scale"},
        {L"timeouts.processing_ms", L"timeouts", L"processing_ms", ConfigValueKind::kDecimal,
         nullptr, 0, kMaxTimeoutMs, L"0", L"timeout-ms"},
        {L"timeouts.consent_ms", L"timeouts", L"consent_ms", ConfigValueKind::kDecimal,
         nullptr, 0, kMaxTimeoutMs, L"0", L"consent-timeout-ms"},
        {L"history.enabled", L"history", L"enabled", ConfigValueKind::kBool,
         nullptr, 0, 0, L"true", L"history"},
        // 截图后的桌面通知。三条都归"偏好"，不参与授权判据：桌面像素那一条该问人照样问人，
        // 关掉通知也一个像素都不多截，打开也不代替那一次批准。
        // 默认是**弹**（enabled = true），而附图这一件没有单独的开关：它恒等于历史副本那一张
        // （images[].history.file），所以历史关掉时通知照发、只是不带图。
        {L"notification.enabled", L"notification", L"enabled", ConfigValueKind::kBool,
         nullptr, 0, 0, L"true", L"notify"},
        {L"notification.merge", L"notification", L"merge", ConfigValueKind::kBool,
         nullptr, 0, 0, L"false", L"notify-merge"},
        {L"notification.detail", L"notification", L"detail", ConfigValueKind::kEnum,
         kCfgNotifyDetail, 0, 0, L"title", L"notify-detail"},
        {L"diagnostics.integrity_hints", L"diagnostics", L"integrity_hints",
         ConfigValueKind::kBool, nullptr, 0, 0, L"true", L"integrity-hints"},
        {L"query.window_list_limit", L"query", L"window_list_limit", ConfigValueKind::kDecimal,
         nullptr, 1, kMaxWindowListItems, L"50", L"limit"},
    };
    return kFields;
}

const ConfigField* FindConfigField(const std::wstring& section, const std::wstring& key) {
    const std::wstring s = LowerAscii(section);
    const std::wstring k = LowerAscii(key);
    for (const ConfigField& f : ConfigSchema()) {
        if (s == f.section && k == f.key) return &f;
    }
    return nullptr;
}

// schema id 的下标：id 是**文本**，所以按内容比。这里比较的两侧会来自不同的翻译单元
// （调用点写的那一条字面量与 ConfigSchema() 里那一条），它们有没有落到同一个地址，
// 取决于字符串合并与链接器的相同折叠 —— 那不是这一条查找可以借用的前提。
namespace {
size_t IndexOfFieldId(std::wstring_view id) {
    const auto& table = ConfigSchema();
    for (size_t k = 0; k < table.size(); ++k) {
        if (id == std::wstring_view(table[k].id)) return k;
    }
    return std::wstring_view::npos;
}
}  // namespace

const ConfigField* FindConfigFieldById(std::wstring_view id) {
    const size_t k = IndexOfFieldId(id);
    if (k == std::wstring_view::npos) return nullptr;
    return &ConfigSchema()[k];
}

bool ConfigFieldDefault(std::wstring_view id, std::wstring* out) {
    const ConfigField* f = FindConfigFieldById(id);
    if (!f || !out) return false;
    *out = f->defaultValue ? std::wstring(f->defaultValue) : std::wstring();
    return true;
}

const std::vector<std::wstring>& ConfigSections() {
    static const std::vector<std::wstring> kSections = [] {
        std::vector<std::wstring> out;
        for (const ConfigField& f : ConfigSchema()) {
            if (std::find(out.begin(), out.end(), f.section) == out.end())
                out.push_back(f.section);
        }
        return out;
    }();
    return kSections;
}

const wchar_t* SettingSourceName(SettingSource s) {
    switch (s) {
        case SettingSource::kCli: return L"cli";
        case SettingSource::kConfig: return L"config";
        case SettingSource::kBuiltin: break;
    }
    return L"builtin";
}

const wchar_t* ConfigStateName(ConfigLoadState state) {
    switch (state) {
        case ConfigLoadState::kAbsent: return L"absent";
        case ConfigLoadState::kLoaded: return L"loaded";
        case ConfigLoadState::kFailed: return L"failed";
        case ConfigLoadState::kNotRead: break;
    }
    return L"not_read";
}

bool IsConfigReadFailure(const std::wstring& code) {
    return code == codes::kConfigNotFound || code == codes::kConfigReadFailed;
}

// ---------------------------------------------------------------------------
// 语法层：字节 -> 条目
// ---------------------------------------------------------------------------

bool ParseIniBytes(const std::vector<uint8_t>& bytes, std::vector<IniEntry>* entries,
                   std::vector<IniIssue>* issues) {
    if (!entries || !issues) return false;
    entries->clear();
    issues->clear();
    bool ok = true;

    size_t start = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        start = 3;  // BOM 允许，且只认文件最前面这一处
    }
    const std::vector<uint8_t> body(bytes.begin() + static_cast<std::ptrdiff_t>(start),
                                     bytes.end());
    // 严格 UTF-8 解码（拒绝 NUL、过长编码、代理区码点、超过 U+10FFFF 的序列与残缺尾字节）
    // 走 core/TextCoding 那一份判据：整份合才继续读，第一处坏字节的偏移交回调用方写进 detail。
    // **不**按系统代码页再试一遍 —— 那正是"用会静默截断或按代码页转换的接口来假装满足
    // UTF-8 严格解析"的那条路。
    const StrictUtf8Text decoded = DecodeUtf8Strict(body, /*rejectNul=*/true);
    if (!decoded.ok) {
        issues->push_back(IniIssue{codes::kConfigEncoding, std::wstring(), std::wstring(),
                                   std::wstring(),
                                   L"byte:offset=" + std::to_wstring(decoded.badOffset), 1});
        return false;
    }
    const std::wstring& text = decoded.text;

    std::wstring currentSection;             // 已小写；空 = 还没进任何 section
    std::vector<std::wstring> seenSections;  // 判重复 section
    std::vector<std::wstring> seenKeys;      // "section.key"，判重复键
    std::wstring lineBuf;
    size_t lineNumber = 1;

    // 一行的实体部分。这里只判**语法**与"这个键存不存在"；类型、区间与组合在后面。
    // 出错时不把整份文档丢掉，而是把后面每一行继续判完：一次把能一并改掉的地方都列出来。
    auto HandleLine = [&](const std::wstring& rawLine, size_t line) {
        const std::wstring s = TrimWs(rawLine);
        if (s.empty()) return;
        const wchar_t head = s[0];
        // 独占一行的注释。行内注释一律**不**支持：路径里的 # 与 ; 会被截掉，
        // 而"看起来少了一截的文件名"比一条语法错更难查。
        if (head == L';' || head == L'#') return;

        if (head == L'[') {
            const size_t close = s.find(L']');
            if (close == std::wstring::npos || close == 1) {
                issues->push_back(IniIssue{codes::kConfigSyntax, std::wstring(), std::wstring(),
                                           s, L"section:bracket", line});
                ok = false;
                return;
            }
            if (!TrimWs(s.substr(close + 1)).empty()) {
                issues->push_back(IniIssue{codes::kConfigSyntax, std::wstring(), std::wstring(),
                                           s, L"section:trailing", line});
                ok = false;
                return;
            }
            const std::wstring name = LowerAscii(TrimWs(s.substr(1, close - 1)));
            const bool known = std::find(ConfigSections().begin(), ConfigSections().end(),
                                         name) != ConfigSections().end();
            if (!known) {
                issues->push_back(
                    IniIssue{codes::kConfigUnknownSection, name, std::wstring(), std::wstring(),
                            std::wstring(), line});
                ok = false;
                return;
            }
            if (std::find(seenSections.begin(), seenSections.end(), name) !=
                seenSections.end()) {
                issues->push_back(
                    IniIssue{codes::kConfigDuplicateSection, name, std::wstring(), std::wstring(),
                            std::wstring(), line});
                ok = false;
                return;
            }
            seenSections.push_back(name);
            currentSection = name;
            return;
        }

        const size_t eq = s.find(L'=');
        if (eq == std::wstring::npos) {
            issues->push_back(IniIssue{codes::kConfigSyntax, currentSection, s, std::wstring(),
                                       L"missing-equals", line});
            ok = false;
            return;
        }
        const std::wstring rawKey = TrimWs(s.substr(0, eq));
        if (rawKey.empty()) {
            issues->push_back(IniIssue{codes::kConfigSyntax, currentSection, std::wstring(),
                                       std::wstring(), L"empty-key", line});
            ok = false;
            return;
        }
        for (wchar_t c : rawKey) {
            if (!IsKeyNameChar(c)) {
                issues->push_back(IniIssue{codes::kConfigSyntax, currentSection, rawKey,
                                          std::wstring(), L"key:chars", line});
                ok = false;
                return;
            }
        }
        const std::wstring key = LowerAscii(rawKey);
        // 值 = 等号之后的全部（不吞行内注释），只去掉语法本身的首尾空白。
        std::wstring value = TrimWs(s.substr(eq + 1));
        if (!value.empty() && value.front() == L'"') {
            if (value.size() < 2 || value.back() != L'"') {
                issues->push_back(IniIssue{codes::kConfigSyntax, currentSection, key, value,
                                           L"unclosed-quote", line});
                ok = false;
                return;
            }
            // 成对引号包住整个值；反斜杠**不是**转义符（Windows 路径里的 \\ 就是两个反斜杠）。
            value = value.substr(1, value.size() - 2);
        }

        if (currentSection.empty()) {
            issues->push_back(IniIssue{codes::kConfigSyntax, std::wstring(), key, std::wstring(),
                                       L"no-section", line});
            ok = false;
            return;
        }
        const ConfigField* field = FindConfigField(currentSection, key);
        if (!field) {
            issues->push_back(IniIssue{codes::kConfigUnknownKey, currentSection, key,
                                       std::wstring(), std::wstring(), line});
            ok = false;
            return;
        }
        const std::wstring id = currentSection + L"." + key;
        if (std::find(seenKeys.begin(), seenKeys.end(), id) != seenKeys.end()) {
            issues->push_back(IniIssue{codes::kConfigDuplicateKey, currentSection, key,
                                       std::wstring(), std::wstring(), line});
            ok = false;
            return;
        }
        seenKeys.push_back(id);
        entries->push_back(IniEntry{currentSection, key, value, line});
    };

    for (size_t i = 0; i <= text.size(); ++i) {
        const bool atEnd = i == text.size();
        const wchar_t c = atEnd ? L'\n' : text[i];
        if (c == L'\n' || c == L'\r') {
            if (static_cast<uint64_t>(lineBuf.size()) > config_limits::kMaxLineChars) {
                issues->push_back(IniIssue{codes::kConfigLineTooLong, currentSection,
                                           std::wstring(), std::wstring(),
                                           L"limit:" + std::to_wstring(config_limits::kMaxLineChars),
                                           lineNumber});
                ok = false;
            } else if (!atEnd || !lineBuf.empty()) {
                HandleLine(lineBuf, lineNumber);
            }
            lineBuf.clear();
            if (!atEnd) ++lineNumber;
            if (!atEnd && c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            continue;
        }
        lineBuf.push_back(c);
    }
    return ok && issues->empty();
}

// ---------------------------------------------------------------------------
// 逐键判定：条目 -> 带来源的 typed 结果
// ---------------------------------------------------------------------------

void ApplySchemaToEntries(const std::vector<IniEntry>& entries, ConfigSnapshot* snapshot,
                          std::vector<IniIssue>* issues) {
    if (!snapshot || !issues) return;
    const auto& table = ConfigSchema();
    snapshot->settings.assign(table.size(), ConfigSetting{});

    for (const IniEntry& e : entries) {
        size_t slot = 0;
        for (; slot < table.size(); ++slot) {
            if (table[slot].section == e.section && table[slot].key == e.key) break;
        }
        if (slot >= table.size()) continue;  // 未知键已在语法层报过，这里不重复判
        const ConfigField& field = table[slot];
        ConfigSetting& setting = snapshot->settings[slot];
        setting.present = true;
        setting.line = e.line;

        switch (field.kind) {
            case ConfigValueKind::kVersion:
            case ConfigValueKind::kDecimal: {
                uint64_t v = 0;
                if (!ParseStrictDecimal(e.value, &v)) {
                    setting.present = false;
                    issues->push_back(IniIssue{codes::kConfigInvalidNumber, e.section, e.key,
                                               e.value, L"decimal:strict", e.line});
                    continue;
                }
                const bool isVersion = field.kind == ConfigValueKind::kVersion;
                const uint64_t lo = isVersion ? config_limits::kSupportedVersion : field.minValue;
                const uint64_t hi = isVersion ? config_limits::kSupportedVersion : field.maxValue;
                if (v < lo || v > hi) {
                    setting.present = false;
                    issues->push_back(
                        IniIssue{isVersion ? codes::kConfigUnsupportedVersion
                                          : codes::kConfigOutOfRange,
                                 e.section, e.key, e.value,
                                 L"range:" + std::to_wstring(lo) + L"-" + std::to_wstring(hi),
                                 e.line});
                    continue;
                }
                setting.value = std::to_wstring(v);
                break;
            }
            case ConfigValueKind::kBool: {
                // 只认 true / false 两种写法。1 / yes / on / 关掉不算开 ——
                // "猜一个布尔"会把一次写错的开关变成谁都没决定的决定。
                const std::wstring v = LowerAscii(e.value);
                if (v == L"true") {
                    setting.value = L"true";
                } else if (v == L"false") {
                    setting.value = L"false";
                } else {
                    setting.present = false;
                    issues->push_back(IniIssue{codes::kConfigInvalidBoolean, e.section, e.key,
                                               e.value, L"true|false", e.line});
                }
                break;
            }
            case ConfigValueKind::kEnum: {
                const std::wstring v = LowerAscii(e.value);
                bool known = false;
                for (const wchar_t* const* p = field.allowed; p && *p; ++p) {
                    if (v == LowerAscii(*p)) {
                        // 存 allowed 表里那一条**规范写法**（zh-CN 不是 zh-cn），
                        // --show-config 与合并那一步读的都是它。
                        setting.value = *p;
                        known = true;
                        break;
                    }
                }
                if (!known) {
                    std::wstring list;
                    for (const wchar_t* const* p = field.allowed; p && *p; ++p) {
                        if (p != field.allowed) list += L"|";
                        list += *p;
                    }
                    setting.present = false;
                    issues->push_back(
                        IniIssue{codes::kConfigInvalidValue, e.section, e.key, e.value, list,
                                 e.line});
                }
                break;
            }
            case ConfigValueKind::kText: {
                // 路径与 roi 那四个数**不在这里判**：各自的判据（扩展名推断、roi 语法与坐标上限、
                // 相对路径的基准目录）与 CLI 是同一条线，写在 CliOptions.cpp 的合并那一步，
                // 两处不许各写一套。
                setting.value = e.value;
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 取值与读文件
// ---------------------------------------------------------------------------

ConfigSnapshot EmptyConfigSnapshot() {
    ConfigSnapshot s;
    s.state = ConfigLoadState::kNotRead;
    s.settings.assign(ConfigSchema().size(), ConfigSetting{});
    return s;
}

const ConfigSetting* ConfigSnapshot::Find(const std::wstring& fieldId) const {
    // 与 FindConfigFieldById 共用同一条按内容的查找（见 Config.h 那一条规矩）。
    const size_t k = IndexOfFieldId(std::wstring_view(fieldId));
    if (k == std::wstring_view::npos) return nullptr;
    return k < settings.size() ? &settings[k] : nullptr;
}

bool ConfigSnapshot::Has(const std::wstring& fieldId) const {
    const ConfigSetting* s = Find(fieldId);
    return s && s->present;
}

const std::wstring& ConfigSnapshot::ValueOf(const std::wstring& fieldId) const {
    static const std::wstring kEmpty;
    const ConfigSetting* s = Find(fieldId);
    return (s && s->present) ? s->value : kEmpty;
}

std::wstring DefaultConfigPath(bool portable) {
    // 默认位置是平台的事：Windows 问模块路径并用程序同目录（既有行为）；
    // Linux 用户模式按 XDG 目录，--portable 才回到 exe 旁。问不出来就交回空串，
    // 由调用方报 config.read_failed —— 不猜当前目录、不猜源码树、也不猜安装位置。
    AppPaths paths;
    if (!ResolveAppPaths(portable, &paths) || paths.configDir.empty()) return std::wstring();
    return JoinDirAndFile(paths.configDir, L"ECAPTURE.ini");
}

namespace {

// 整份读取一个文件走平台服务（src/HostFile.h）：那三条"上限在分配之前判 / 撞上同名目录要
// 说清 / 原生码只作核对"的规矩在两套实现里各自满足，判据词表在这一层共用。
// DetailWithNative 把系统给的原值按本平台的叫法（win32= / errno=）附在原因 token 之后，
// 分支从来不读那个数 —— 那两个数空间互不相干，同一个 5 在两边是两件事。
std::wstring DetailWithNative(const wchar_t* token, NativeErrorCode code) {
    return std::wstring(token) + L" " + NativeErrorCodeLabel() + L"=" + std::to_wstring(code);
}

// 语法/类型问题 -> 已经带上文件位置的诊断。语法层与逐键判定都只认 "section.key" 与行号，
// 到这一层才把**哪一个文件**填进去：一次调用可能读默认位置也可能读 --config 指的那一条，
// 而报错必须说清是哪一个（两处不许各写一份路径拼接）。
void MoveIssuesForLoad(const std::vector<IniIssue>& issues, const std::wstring& path,
                       std::vector<Diagnostic>* out) {
    const std::wstring brief = BriefPath(path);
    for (const IniIssue& i : issues) {
        out->push_back(ConfigDiag(i.code.c_str(), brief, i.section, i.key, i.line, i.detail));
    }
}

}  // namespace

ConfigSnapshot LoadConfigFile(const std::wstring& absolutePath, bool explicitSelection) {
    ConfigSnapshot snap = EmptyConfigSnapshot();
    snap.path = absolutePath;
    if (absolutePath.empty()) {
        snap.state = ConfigLoadState::kFailed;
        snap.errors.push_back(ConfigDiag(codes::kConfigReadFailed, std::wstring(), L"", L"", 0,
                                        L"path:unresolvable"));
        return snap;
    }

    const HostFileRead raw = ReadWholeFileBounded(absolutePath, config_limits::kMaxFileBytes);
    if (raw.outcome == ReadOutcome::kNotFound) {
        // 默认位置没有文件 = 正常状态，内置默认继续有效，不报错也不写文件；
        // 显式 --config 指的那一条不在 = 本次失败，且**不回退**到默认位置那一份。
        if (!explicitSelection) {
            snap.state = ConfigLoadState::kAbsent;
            return snap;
        }
        snap.state = ConfigLoadState::kFailed;
        snap.errors.push_back(ConfigDiag(codes::kConfigNotFound, BriefPath(absolutePath), L"", L"",
                                         0, DetailWithNative(L"path:not-found", raw.native)));
        return snap;
    }
    if (raw.outcome != ReadOutcome::kOk) {
        const wchar_t* token = raw.tooLarge      ? L"size:limit"
                               : raw.outcome == ReadOutcome::kDirectory ? L"path:directory"
                                                                         : L"read:failed";
        snap.state = ConfigLoadState::kFailed;
        // 超限是"说不通"（用法错），其余是"读不出来"（I/O 失败）：两条下一步不同。
        snap.errors.push_back(ConfigDiag(raw.tooLarge ? codes::kConfigTooLarge
                                                      : codes::kConfigReadFailed,
                                         BriefPath(absolutePath), L"", L"", 0,
                                         raw.tooLarge
                                             ? L"limit:" +
                                                   std::to_wstring(config_limits::kMaxFileBytes)
                                             : DetailWithNative(token, raw.native)));
        return snap;
    }

    std::vector<IniEntry> entries;
    std::vector<IniIssue> issues;
    if (!ParseIniBytes(raw.bytes, &entries, &issues)) {
        MoveIssuesForLoad(issues, absolutePath, &snap.errors);
    }
    std::vector<IniIssue> typeIssues;
    ApplySchemaToEntries(entries, &snap, &typeIssues);
    MoveIssuesForLoad(typeIssues, absolutePath, &snap.errors);

    snap.state = snap.errors.empty() ? ConfigLoadState::kLoaded : ConfigLoadState::kFailed;
    return snap;
}

}  // namespace ecapture
