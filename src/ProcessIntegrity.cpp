#include "ProcessIntegrity.h"

#include <Windows.h>

#include <algorithm>
#include <string>

#include "Lang.h"

namespace ecapture {
namespace {

// 标签值一律取 SDK 的符号（winnt.h 里那组 SECURITY_MANDATORY_*_RID），一个十六进制数都不手写。
// 这一层踩过的教训：照印象写表会把整组级别错开一档 —— Medium(8192) 报成 Low、Low(4096) 报成
// Untrusted，两份错答案看起来都像"问出来了"，只有对着自己机器上的标签值才看得出。判据
// （tests\compat.ps1 的真机层）就是拿系统给的名字核对这一组符号，别再拿实现里的数字当期望。

// "这一步是不是被完整性规则挡住的那一类落点"：取帧、写文件、写标准输出。
// 匹配、编码、确认框那几步不是（低完整性不参与那里的决定）。
bool DeniableStage(const std::wstring& stage) {
    return stage == stages::kCapture || stage == stages::kWrite || stage == stages::kStdout;
}

}  // namespace

const wchar_t* IntegrityName(Integrity value) {
    switch (value) {
        case Integrity::kUntrusted: return L"untrusted";
        case Integrity::kLow: return L"low";
        case Integrity::kMedium: return L"medium";
        case Integrity::kHigh: return L"high";
        case Integrity::kSystem: return L"system";
        case Integrity::kProtectedProcess: return L"protected_process";
        case Integrity::kUnknown: break;
    }
    return L"unknown";
}

Integrity ProbeProcessIntegrity(uint32_t* probeError) {
    if (probeError) *probeError = 0;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (probeError) *probeError = GetLastError();
        return Integrity::kUnknown;
    }

    // 第一问只讨"这一问要写多少字节"：TokenIntegrityLevel 交回的是令牌自己那条 SID 的指针，
    // 而各 Windows 对"至少给多大才算够"的算法并不一致 —— 本机 19045 实测：只给一个指针的大小
    // 会撞上 ERROR_INSUFFICIENT_BUFFER（122），于是这一问整个答不出来。所以先讨长度，再按
    // max(讨来的, sizeof(TOKEN_MANDATORY_LABEL)) 给足，两种写法都覆盖。
    DWORD needed = 0;
    const BOOL sized = GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
    const DWORD sizedError = sized ? 0 : GetLastError();

    uint8_t storage[64]{};
    const DWORD want = std::max(needed, static_cast<DWORD>(sizeof(TOKEN_MANDATORY_LABEL)));

    DWORD returned = 0;
    BOOL ok = FALSE;
    DWORD error = 0;
    DWORD rid = 0;
    bool ridKnown = false;
    if (sized || sizedError != ERROR_INSUFFICIENT_BUFFER) {
        // sized 真成功了 = 这一问的形状与文档不符；别的错误码 = 那一问本身没跑成。
        // 两种都不去猜它写了什么，只把错误码带出去。
        error = sizedError;
    } else if (want == 0 || want > sizeof(storage)) {
        error = ERROR_BUFFER_OVERFLOW;
    } else {
        ok = GetTokenInformation(token, TokenIntegrityLevel, storage, want, &returned);
        error = ok ? 0 : GetLastError();
        if (ok && returned >= sizeof(SID*)) {
            // 那条 SID 由令牌持有，所以趁令牌还开着把它读完（CloseHandle 之后那个指针不再算有效）。
            SID* label = *reinterpret_cast<SID**>(storage);
            if (label) {
                const BYTE count = *GetSidSubAuthorityCount(label);
                if (count > 0) {
                    rid = *GetSidSubAuthority(label, static_cast<DWORD>(count) - 1);
                    ridKnown = true;
                }
            }
        }
    }
    CloseHandle(token);

    if (error) {
        if (probeError) *probeError = error;
        return Integrity::kUnknown;
    }
    if (!ridKnown) return Integrity::kUnknown;
    return IntegrityFromLabelRid(rid);
}

Integrity IntegrityFromLabelRid(uint32_t rid) {
    switch (rid) {
        case SECURITY_MANDATORY_UNTRUSTED_RID: return Integrity::kUntrusted;
        case SECURITY_MANDATORY_LOW_RID: return Integrity::kLow;
        case SECURITY_MANDATORY_MEDIUM_RID:
        case SECURITY_MANDATORY_MEDIUM_PLUS_RID: return Integrity::kMedium;
        case SECURITY_MANDATORY_HIGH_RID: return Integrity::kHigh;
        case SECURITY_MANDATORY_SYSTEM_RID: return Integrity::kSystem;
        case SECURITY_MANDATORY_PROTECTED_PROCESS_RID: return Integrity::kProtectedProcess;
        default: break;
    }
    // 认不出的标签值不折成"够用"也不折成"被降级"：这一问等于没有答案，而错误码那一格仍是 0
    // —— 系统确实答了，只是答的是本工具没登记过的一档。这两种 unknown 分开，调用方才分得开
    // "问答没跑成"与"读本工具不认识的取值"。
    return Integrity::kUnknown;
}

bool IntegrityBelowMedium(Integrity value) {
    return value == Integrity::kUntrusted || value == Integrity::kLow;
}

bool IsAccessDenied(const Diagnostic& diag) {
    if (diag.win32 == static_cast<uint32_t>(ERROR_ACCESS_DENIED)) return true;
    // hresult 是"0x80070005"那种八位十六进制写法（长度恒 10：0x 加八位）；上游那一处
    // 用的是同一个格式串，所以这里按整串比较，不拿前缀匹配蒙。
    if (diag.hresult.size() == 10 && diag.hresult[0] == L'0' &&
        (diag.hresult[1] == L'x' || diag.hresult[1] == L'X')) {
        return static_cast<uint32_t>(std::wcstoul(diag.hresult.c_str() + 2, nullptr, 16)) ==
               static_cast<uint32_t>(E_ACCESSDENIED);
    }
    return false;
}

size_t AnnotateIntegrityDenials(Integrity integrity, std::vector<Diagnostic>* errors,
                                std::vector<Diagnostic>* notes) {
    if (!errors || !notes || !IntegrityBelowMedium(integrity)) return 0;

    size_t touched = 0;
    const std::wstring level = IntegrityName(integrity);
    for (auto& e : *errors) {
        // 只补"系统说不让"这一类，而且限定在会被完整性规则影响的三步上。
        // 成因不明的 access-denied（更高完整性/不同归属的目标窗口）也被补，但补的那句话本身
        // 就写着"这一档完整性通常是主因，但不是唯一成因"，不替调用方下结论。
        if (!DeniableStage(e.stage) || !IsAccessDenied(e)) continue;
        const std::wstring extra = Msgf(L"env.integrity_hint", level);
        e.hint = e.hint.empty() ? extra : e.hint + L" " + extra;
        ++touched;
    }
    if (touched == 0) return 0;

    // 整轮只追加一条：这三类失败在低完整性下经常同时出现（auto 链里 wgc 与 printwindow 都被拒，
    // 紧接着落盘又被挡），逐条发同一句话会让调用方以为有 N 件不同的事要处理。
    Diagnostic note;
    note.code = codes::kLowIntegrity;
    note.message = Msgf(L"env.integrity_denied", level, static_cast<uint64_t>(touched));
    note.hint = Msg(L"env.integrity_note_hint");
    // 定位字段跟着第一条被补写的那条失败走：同一件事的两个视图，坐标不该各写一份。
    const auto firstMatched = std::find_if(errors->begin(), errors->end(), [](const Diagnostic& d) {
        return DeniableStage(d.stage) && IsAccessDenied(d);
    });
    if (firstMatched != errors->end()) {
        note.target = firstMatched->target;
        note.backend = firstMatched->backend;
        note.stage = firstMatched->stage;
    }
    notes->push_back(std::move(note));
    return touched;
}

}  // namespace ecapture
