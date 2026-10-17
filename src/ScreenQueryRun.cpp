// --screens 的运行时入口：问一遍候选（含显示配置那一路）再交给纯判据。
//
// 与 src/ScreenQuery.cpp 分开的理由同 WindowMatchRun.cpp：判据与渲染是纯函数、两个平台
// 链同一份；这一步要真的枚举显示输出（ScreenIdentity.h 的 EnumScreenCandidates，
// 平台实现在 WinScreenSource.cpp），Linux 的 X11/Wayland 后端还没接进来。
// 分开之后 screens_state.cpp 那批判据只链纯函数，不需要一个平台显示后端才编得过。

#include "ScreenQuery.h"
#include "ScreenIdentity.h"
#include "ScreenMatch.h"

namespace ecapture {

ScreenQueryResult RunScreenQuery() {
    // 这份文档要回答的就是"每块屏的哪几种身份问得到"，所以显示配置那一路必须问。
    return BuildScreenQuery(EnumScreenCandidates(/*needFacts=*/true));
}

}  // namespace ecapture
