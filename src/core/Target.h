#pragma once
// 跨平台核心：目标描述与后端身份。
//
// 为什么要单独一份，而不是把 HWND 换个名字继续用：三个平台对"一扇窗口"能拿到的身份根本不一样。
//   * Windows：HWND 是一个进程内有效的整数句柄，可以被另一扇窗口复用，所以要 PID + 进程创建时间
//     + 类名一起才谈得上复核（src/WindowIdentity.h）。
//   * X11：XID 是 32 位、在服务端连接范围内有效；换一次 DISPLAY 连接同一个窗口就不是同一个数。
//   * Wayland：客户端**没有**全局窗口列表，也没有整数窗口号。能拿到的只有 xdg_toplevel 那种
//     不透明句柄（Portal 的 Handle 参数是一段字符串），而且它只在本客户端与合成器的这次会话里
//     有意义。把这种句柄硬压成一个 uint64 再当成统一身份，等于用 Windows 的形状冒充三家共用。
// 所以这里给的是一个**带来源的形状**：所属平台 + 所属会话 + 那一家自己认得的标识 + 标识是哪一种形状。
//
// 三条规矩：
//   1. 目标标识只在它所属的后端与会话边界内可比。跨会话比较一律返回 kUnknown，
//      不返回 kNo —— "换了会话所以比不了"与"确认是两个对象"对调用方是不同的下一步。
//   2. 拿不到标识就是拿不到：kUnknownNativeId 与 form::kUnknown 是合法取值，
//      不许填一个"大概是 0 号窗口"的数进去（AGENTS.md：不编造默认值）。
//   3. 这里**不**替代既有的 WindowIdentity / ScreenInfo / ScreenCandidate：那三份是各条链路
//      复核身份用的快照与判据（进程创建时间、类名、设备名……），本文件只规定"一个目标怎么被
//      中性地说出来"。将来 Linux 后端按这个形状交目标，Windows 那一路由适配层映射进来。

#include <cstdint>
#include <string>

#include "core/Error.h"     // Tri：比较不出来时写 unknown
#include "core/Platform.h"  // PlatformKind

namespace ecapture {

// 目标是哪一类。与 images[] 里既有的 screen 布尔同源（true == kScreen），
// 这一条说的是"截图范围能落在什么上"，不是"用户请求了什么"。
enum class TargetDomain : uint32_t {
    kWindow = 0,   // 一扇窗口 / 一个对话框
    kScreen = 1,   // 一块显示器输出
    kUnknown = 2,  // 认不出来：既不按窗口处理也不按屏幕处理
};

inline const char* TargetDomainName(TargetDomain domain) {
    switch (domain) {
        case TargetDomain::kWindow: return "window";
        case TargetDomain::kScreen: return "screen";
        case TargetDomain::kUnknown: return "unknown";
    }
    return "unknown";
}

// 标识是哪一种形状。
enum class NativeIdForm : uint32_t {
    kNumber = 0,   // HWND / HMONITOR / XID 这类整数
    kText = 1,     // Wayland 的 xdg_toplevel 句柄、Portal 对象路径这类不透明字符串
    kUnknown = 2,  // 这个后端给不出可用的标识（例如纯按标题命中的那一扇窗口当时还没有句柄）
};

inline const char* NativeIdFormName(NativeIdForm form) {
    switch (form) {
        case NativeIdForm::kNumber: return "number";
        case NativeIdForm::kText: return "text";
        case NativeIdForm::kUnknown: return "unknown";
    }
    return "unknown";
}

// 整数标识里"没有标识"那一个值。既有链路一直用 0 表示这个（WindowInfo.hwnd == 0 就是没量到），
// 搬到这一层是为了让"0 是一个合法句柄值还是没值"这件事在类型上有说法：0 永远不是句柄。
inline constexpr uint64_t kUnknownNativeId = 0;

// 一次会话的边界。同一个 native 在另一次会话里通常指到别的东西，所以比较身份
// 必须先确认会话相同。会话自己问不出来时 known = false，比较结果随之变成 kUnknown。
struct SessionRef {
    bool known = false;
    // Windows：终端会话号（WTSGetActiveConsoleSessionId / ProcessIdToSessionId 那一个整数）。
    // X11：一次 DISPLAY 打开的序号。Wayland：一次 wl_registry / Portal 会话的序号。
    // 它是**本进程内**用来认"还是同一次会话"的记号，不承诺跨进程、跨重启可比。
    uint64_t id = 0;
};

// 一个目标的中性描述：谁（域）+ 在哪（平台、会话）+ 叫什么（那一家自己的标识）。
struct TargetRef {
    TargetDomain domain = TargetDomain::kUnknown;
    PlatformKind platform = PlatformKind::kUnknown;
    SessionRef session{};
    NativeIdForm form = NativeIdForm::kUnknown;
    uint64_t number = kUnknownNativeId;  // form == kNumber 时有效
    std::string text;                    // form == kText 时有效（恒 ASCII，不放本地化文案）

    // 标识本身拿不拿得出来。注意这与"目标存不存在"是两件事：
    // 按标题命中的一扇窗口照样是合法目标，只是这一刻还没有可用于复核的标识。
    bool HasNativeId() const {
        if (form == NativeIdForm::kNumber) return number != kUnknownNativeId;
        return form == NativeIdForm::kText && !text.empty();
    }
};

// 两个引用说的是不是同一个目标。三值：
//   kYes     同平台、会话可比且相同、标识可比且相同
//   kNo      同平台、会话相同，而标识明确不同（这才敢断言"换对象了"）
//   kUnknown 任一侧平台/会话/标识拿不出来，或两侧跨平台、跨会话 —— 不作断言，
//            调用方按"问不出来"处理（与 WindowIdentity 那一条"问不出来 ≠ 相同"同源）
inline Tri SameTargetAs(const TargetRef& a, const TargetRef& b) {
    if (a.platform == PlatformKind::kUnknown || b.platform == PlatformKind::kUnknown)
        return Tri::kUnknown;
    if (a.platform != b.platform || a.domain != b.domain || a.domain == TargetDomain::kUnknown)
        return Tri::kUnknown;
    if (!a.session.known || !b.session.known || a.session.id != b.session.id) return Tri::kUnknown;
    if (!a.HasNativeId() || !b.HasNativeId()) return Tri::kUnknown;
    if (a.form != b.form) return Tri::kUnknown;
    const bool same = (a.form == NativeIdForm::kNumber) ? (a.number == b.number) : (a.text == b.text);
    return same ? Tri::kYes : Tri::kNo;
}

}  // namespace ecapture
