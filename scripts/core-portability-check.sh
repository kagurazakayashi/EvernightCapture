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
# 2026-10（迁移命令行文本处理与多语言资源）新加进来的那一组：文本编码、命令行参数表、
# 文案嵌入接口，以及四个平台服务的**声明**头（HostPath / HostFile / OutputStream / CliArgs）。
# 声明头在清单里，实现在平台层里 —— 这正是这一步要的形状。
NEUTRAL_HEADERS=(
    core/Platform.h
    core/TextCoding.h
    core/CliTokens.h
    CliArgs.h
    Config.h
    Lang.h
    LangData.h
    HostPath.h
    HostFile.h
    HostDirectory.h
    HostProcess.h
    OutputStream.h
    core/Geometry.h
    core/Error.h
    core/Status.h
    core/Frame.h
    core/Target.h
    core/WorkerFraming.h
    core/HelperTiming.h
    core/Authorization.h
    core/Capability.h
    Main.h
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
         WorkerProtocol.h WorkerTiming.h NotificationToast.cpp SystemCompat.cpp \
         platform/windows/WinCliArgs.cpp platform/windows/WinLocale.cpp \
         platform/windows/WinHostPath.cpp platform/windows/WinHostFile.cpp \
         platform/windows/WinHostDirectory.cpp platform/windows/WinFileSave.cpp \
         platform/windows/WinOutputStream.cpp \
         platform/linux/PosixCliArgs.cpp platform/linux/PosixLocale.cpp \
         platform/linux/PosixHostPath.cpp platform/linux/PosixHostFile.cpp \
         platform/linux/PosixHostDirectory.cpp platform/linux/PosixFileSave.cpp \
         platform/linux/PosixOutputStream.cpp; do
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
# 3b) 命令行文本这一层：不经 CMake 目标，自己生成嵌入文案再编再跑。
#     为什么单跑一份：CMake 那条目标把生成物与依赖关系都管好了，但"改完文案要不要重编"
#     这件事要在不信任构建系统的那一层也能被证明 —— 这里直接调那条生成脚本，
#     再用 g++ 把共用 CLI 层与本平台的服务编成一个判据程序跑一遍。
#     它同时是"共用 CLI 层真的不含平台头"的第二份机器证明（编译不过 = 渗进了 Windows 类型）。
# ---------------------------------------------------------------------------
echo "== 3b) 生成嵌入文案并编译命令行文本判据 =="
GEN="$OUT_DIR/ec_lang_data.cpp"
if ! cmake -DECP_RESOURCES_DIR="$ROOT/resources" -DECP_OUTPUT="$GEN" \
        -P "$ROOT/scripts/gen-lang-data.cmake" >"$OUT_DIR/gen.log" 2>&1; then
    echo "  FAIL  文案嵌入生成失败："
    sed 's/^/          /' "$OUT_DIR/gen.log" | tail -20
    fail=1
fi
if [ "$fail" -eq 0 ]; then
    grep -c 'static const char' "$GEN" >/dev/null 2>&1 || true
    echo "  ok    嵌入物已生成：$GEN ($(wc -c <"$GEN") 字节)"
    CLI_BIN="$OUT_DIR/cli_text_tests"
    # 数组形式，绝不拼成空格分隔的字符串：仓库路径本身可能含空格（实测中文 + 空格目录），
    # 拼字符串会在空格处劈开，把一份源文件拆成两个不存在的参数。
    PLATFORM_SRC=(
        "$SRC/platform/linux/PosixCliArgs.cpp"
        "$SRC/platform/linux/PosixLocale.cpp"
        "$SRC/platform/linux/PosixHostPath.cpp"
        "$SRC/platform/linux/PosixHostFile.cpp"
        "$SRC/platform/linux/PosixOutputStream.cpp"
    )
    if ! "$CXX" -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -I"$SRC" "$ROOT/tests/cli_text_state.cpp" \
            "$ROOT/tests/platform/cli_text_posix_checks.cpp" \
            "$SRC/CliOptions.cpp" "$SRC/Config.cpp" "$SRC/Lang.cpp" "$SRC/CliText.cpp" \
            "$SRC/Json.cpp" "$SRC/CaptureScope.cpp" "$SRC/core/TextCoding.cpp" "$GEN" \
            "${PLATFORM_SRC[@]}" -o "$CLI_BIN" 2>"$OUT_DIR/cli_compile.log"; then
        echo "  FAIL  命令行文本层在 Linux 上编不过（说明共用源文件仍然渗着平台类型）："
        sed 's/^/          /' "$OUT_DIR/cli_compile.log" | head -40
        fail=1
    else
        echo "  ok    编译通过：$CLI_BIN"
    fi
fi

# ---------------------------------------------------------------------------
# 3c) 文件 / 目录 / 保存 / 历史这一层：不经 CMake，直接编译生产本体并在本次临时目录里跑。
#     它证明这几份源文件真的不含 Windows 类型，同时把"正常保存、不覆盖、并发、权限失败、
#     符号链接、中文路径、历史部分成功、XDG / 便携目录"逐条落成真实磁盘现场。
# ---------------------------------------------------------------------------
if [ "$fail" -eq 0 ]; then
    echo "== 3c) 编译并运行文件 / 保存 / 历史判据 =="
    SAVE_BIN="$OUT_DIR/save_tests"
    SAVE_PLATFORM_SRC=(
        "$SRC/platform/linux/PosixHostPath.cpp"
        "$SRC/platform/linux/PosixHostFile.cpp"
        "$SRC/platform/linux/PosixHostDirectory.cpp"
        "$SRC/platform/linux/PosixFileSave.cpp"
        "$SRC/platform/linux/PosixLocale.cpp"
    )
    if ! "$CXX" -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -I"$SRC" "$ROOT/tests/save_state.cpp" \
            "$SRC/CliOptions.cpp" "$SRC/Config.cpp" "$SRC/Lang.cpp" "$SRC/OutputPlan.cpp" \
            "$SRC/HistoryArchive.cpp" "$SRC/Delivery.cpp" "$SRC/Deadline.cpp" \
            "$SRC/CaptureScope.cpp" "$SRC/core/TextCoding.cpp" "$GEN" \
            "${SAVE_PLATFORM_SRC[@]}" -pthread -o "$SAVE_BIN" 2>"$OUT_DIR/save_compile.log"; then
        echo "  FAIL  文件 / 保存 / 历史层在 Linux 上编不过："
        sed 's/^/          /' "$OUT_DIR/save_compile.log" | head -40
        fail=1
    else
        echo "  ok    编译通过：$SAVE_BIN"
    fi
fi

# ---------------------------------------------------------------------------
# 3d) 辅助进程通信这一层：不经 CMake，直接 g++ 编生产本体并真跑父子进程判据。
#     证明 PosixHostProcess / WorkerFraming / HelperTiming 不含平台渗漏，
#     同时把协议错误、双向通信、超时、取消、异常退出落成真实进程现场。
# ---------------------------------------------------------------------------
if [ "$fail" -eq 0 ]; then
    echo "== 3d) 编译并链接跨平台辅助进程判据 =="
    IPC_BIN="$OUT_DIR/worker_ipc_tests"
    IPC_PLATFORM_SRC=(
        "$SRC/platform/linux/PosixHostProcess.cpp"
        "$SRC/platform/linux/PosixHostPath.cpp"
        "$SRC/platform/linux/PosixHostFile.cpp"
        "$SRC/platform/linux/PosixHostDirectory.cpp"
        "$SRC/platform/linux/PosixFileSave.cpp"
        "$SRC/platform/linux/PosixLocale.cpp"
    )
    if ! "$CXX" -std=c++20 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -I"$SRC" "$ROOT/tests/worker_ipc_state.cpp" \
            "$SRC/core/WorkerFraming.cpp" "$SRC/core/HelperTiming.cpp" \
            "$SRC/Deadline.cpp" "$SRC/ImageOps.cpp" \
            "$SRC/CliOptions.cpp" "$SRC/Config.cpp" "$SRC/Lang.cpp" \
            "$SRC/core/TextCoding.cpp" "$GEN" "${IPC_PLATFORM_SRC[@]}" \
            -pthread -o "$IPC_BIN" 2>"$OUT_DIR/ipc_compile.log"; then
        echo "  FAIL  辅助进程通信层在 Linux 上编不过："
        sed 's/^/          /' "$OUT_DIR/ipc_compile.log" | head -40
        fail=1
    else
        echo "  ok    编译通过：$IPC_BIN"
    fi
fi

# ---------------------------------------------------------------------------
# 4) 跑判据
# ---------------------------------------------------------------------------
# 两份判据程序各自写"N 条通过，M 条失败"；这里汇总成一行放在最后，
# 因为 ./test-all.sh 取的是日志里**最后**那句汇总 —— 各份的原始行也照旧留在日志里可查。
ec_total_pass=0
ec_total_fail=0
ec_run_state() {
    local label="$1" bin="$2" out rc
    out="$("$bin" 2>&1)"; rc=$?
    printf '%s\n' "$out"
    local p f
    p="$(printf '%s' "$out" | sed -n 's/.*:[[:space:]]*\([0-9]\+\)[[:space:]]*条通过.*/\1/p' | tail -1)"
    f="$(printf '%s' "$out" | sed -n 's/.*，[[:space:]]*\([0-9]\+\)[[:space:]]*条失败.*/\1/p' | tail -1)"
    ec_total_pass=$((ec_total_pass + ${p:-0}))
    ec_total_fail=$((ec_total_fail + ${f:-0}))
    if [ "$rc" -ne 0 ]; then
        echo "  FAIL  $label 退出码 $rc（见上面那几条 FAIL）"
        fail=1
    fi
}

if [ "$fail" -eq 0 ] && [ "$BUILD_ONLY" -eq 0 ]; then
    echo "== 4) 运行核心边界判据 =="
    ec_run_state "核心边界" "$BIN"
    echo "== 4b) 运行命令行文本判据 =="
    ec_run_state "命令行文本" "$OUT_DIR/cli_text_tests"
    echo "== 4c) 运行文件 / 保存 / 历史判据 =="
    ec_run_state "文件与保存" "$OUT_DIR/save_tests"
    echo "== 4d) 运行辅助进程通信判据 =="
    ec_run_state "辅助进程通信" "$OUT_DIR/worker_ipc_tests"
    echo "core-portability: $ec_total_pass 条通过，$ec_total_fail 条失败"
    if [ "$ec_total_fail" -ne 0 ]; then fail=1; fi
fi

if [ "$fail" -ne 0 ]; then
    echo "核心边界检查：未通过"
    exit 1
fi
echo "核心边界检查：通过（共用模型能在没有 Windows SDK 的环境里编译，判据全部成立）"
