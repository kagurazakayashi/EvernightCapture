#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 跨平台核心边界的可移植性检查（Linux 侧入口）
#
# 判什么：src/core/ 与那几份"共用数据模型"头文件必须**在没有 Windows SDK 的机器上**
# 编译得过，并且 tests/core_boundary_state.cpp 跑完全部判据。这是"边界是真的"唯一的机器证明 ——
# 光靠人读代码找 HWND 是不够的：只要有一处渗进平台类型，这一条就会红。
#
# 它**不**构建截图功能，也不碰桌面、不弹框、不取像素：本仓库的 Linux 取帧后端还没有实现，
# 这条脚本只判模型层，不假装 Linux 已经能截图（AGENTS.md：不伪造 Linux 成功后端）。
# Windows 侧对应的入口是 tests/core_boundary.ps1（同一个源文件、同一份判据）。
#
# 用法：
#   scripts/core-portability-check.sh                 # 三关都跑
#   scripts/core-portability-check.sh --build-only    # 只编译不运行
#   CXX=clang++ scripts/core-portability-check.sh     # 换一个编译器
# 退出码：0 = 边界成立；1 = 静态清单或编译或判据没过；2 = 没有可用编译器（不猜、不装依赖）。
# ---------------------------------------------------------------------------
set -euo pipefail

# 从脚本自己的位置找仓库根：不依赖当前工作目录，路径里有空格与中文也照常工作。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd -P)"
ROOT="$(cd "$SCRIPT_DIR/.." >/dev/null 2>&1 && pwd -P)"
SRC="$ROOT/src"
BUILD_ONLY=0
if [ "${1:-}" = "--build-only" ]; then BUILD_ONLY=1; fi

CXX="${CXX:-g++}"
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "缺少编译器 $CXX：本脚本不自动安装依赖（AGENTS.md：环境缺失时给出原因与建议操作）。" >&2
    echo "建议：apt install g++（或指定 CXX=clang++）后重试。" >&2
    exit 2
fi

OUT_DIR="${TMPDIR:-/tmp}/evernightcapture-core-check"
mkdir -p "$OUT_DIR"

# ---------------------------------------------------------------------------
# 1) 静态一关：共用模型头里不许出现 windows.h 与那一族原生类型。
#    这份清单 = "共用模型"的名单，由本步的划分决定，不多不少：真正的平台层
#    （CaptureCommon.h / Worker*.h / D3dDevice.h / WinrtApartment.h / 各通道头）照旧 include
#    <windows.h>，那是它们各自的职责，第 2 关反而要求它们把归属写明白。
#    判之前先去掉行注释（// 之后的一切）：这些头文件在注释里大量讨论 HWND / HRESULT 这些名字，
#    那是说明边界，不是渗进边界。这一关是 lint，真正的证明在第 3 关的编译。
# ---------------------------------------------------------------------------
NEUTRAL_HEADERS=(
    core/Platform.h
    core/Geometry.h
    core/Error.h
    core/Status.h
    core/Frame.h
    core/Target.h
    core/Authorization.h
    core/Capability.h
    Capture.h
    CaptureScope.h
    CliOptions.h
    Config.h
    Consent.h
    CropGeometry.h
    CursorControl.h
    Deadline.h
    Delivery.h
    DupGeometry.h
    EnvReport.h
    FileSave.h
    HdrColor.h
    HistoryArchive.h
    ImageOps.h
    Json.h
    Notification.h
    OutputPlan.h
    ProcessIntegrity.h
    Report.h
    ScreenIdentity.h
    ScreenMatch.h
    ScreenQuery.h
    SystemCompat.h
    WindowIdentity.h
    WindowMatch.h
    WindowQuery.h
    WindowQueryRun.h
    WgcGeometry.h
)

LEAK_RE='#include[[:space:]]*<[Ww]indows\.h>|#include[[:space:]]*<(winrt|wrl|d3d11|dxgi|gdi)[/"]|\bHWND\b|\bHDC\b|\bHBITMAP\b|\bHGDIOBJ\b|\bHMONITOR\b|\bHANDLE\b|\bRECT\b|\bMSG\b|\bDWORD\b|\bUINT\b|\bLONG\b|\bBOOL\b|\bHRESULT\b|\bINFINITE\b|\bLARGE_INTEGER\b|Windows::|Microsoft::WRL|winrt::'

fail=0
echo "== 1) 共用模型头：不许渗进平台类型 =="
for h in "${NEUTRAL_HEADERS[@]}"; do
    f="$SRC/$h"
    if [ ! -f "$f" ]; then
        echo "  FAIL  清单里的头文件不存在：src/$h"
        fail=1
        continue
    fi
    hits="$(sed 's://.*::' "$f" | grep -nE "$LEAK_RE" || true)"
    if [ -n "$hits" ]; then
        echo "  FAIL  src/$h 的代码里出现了平台类型："
        printf '%s\n' "$hits" | sed 's/^/          /'
        fail=1
    else
        echo "  ok    src/$h"
    fi
done

# ---------------------------------------------------------------------------
# 2) 反向一关：平台层的文件要**自己声明**自己是平台层。
#    不写这一句，下一个人就不知道新代码该落在哪一侧 —— 这是清单，不是编译判据，
#    所以缺标记只报 TODO、不算失败（标记补全属于本步的文档工作）。
# ---------------------------------------------------------------------------
echo "== 2) 平台层归属标记 =="
for h in CaptureCommon.h CaptureWgc.h CaptureDwm.h CapturePrintWindow.h CaptureBitBlt.h \
         CaptureDuplication.h D3dDevice.h WinrtApartment.h Worker.h WorkerIo.h \
         WorkerProtocol.h WorkerTiming.h NotificationToast.cpp Lang.cpp SystemCompat.cpp; do
    f="$SRC/$h"
    if [ -f "$f" ] && ! grep -q '【平台层' "$f"; then
        echo '  TODO  src/'"$h"' 还没有"我属于平台层"那一句归属说明'
    fi
done

# ---------------------------------------------------------------------------
# 3) 真编译：整份 TU 不含 windows.h，在 Linux 上编得过才算边界成立。
#    链的源文件只有 CaptureScope.cpp —— 共用模型里那部分判据的本体（登记表与 --yes 例外）。
# ---------------------------------------------------------------------------
echo "== 3) 编译核心边界判据（$($CXX --version | head -1)）=="
BIN="$OUT_DIR/core_boundary_tests"
if ! "$CXX" -std=c++20 -Wall -Wextra -Wno-unused-parameter \
        -I"$SRC" "$ROOT/tests/core_boundary_state.cpp" "$SRC/CaptureScope.cpp" \
        -o "$BIN" 2>"$OUT_DIR/compile.log"; then
    echo "  FAIL  编译不过（说明共用模型仍然渗着平台类型，或判据本身有错）："
    sed 's/^/          /' "$OUT_DIR/compile.log" | head -40
    fail=1
else
    echo "  ok    编译通过：$BIN"
fi

# ---------------------------------------------------------------------------
# 4) 跑判据
# ---------------------------------------------------------------------------
if [ "$fail" -eq 0 ] && [ "$BUILD_ONLY" -eq 0 ]; then
    echo "== 4) 运行核心边界判据 =="
    if ! "$BIN"; then
        echo "  FAIL  判据没过（见上面那几条 [FAIL]）"
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "核心边界检查：未通过"
    exit 1
fi
echo "核心边界检查：通过（共用模型能在没有 Windows SDK 的环境里编译，判据全部成立）"
