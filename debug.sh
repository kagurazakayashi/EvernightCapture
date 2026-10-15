#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# EvernightCapture —— Linux 调试入口（GDB / LLDB）
#
# 三条硬规矩：
#   1) 只调"本平台当前真能构建"的那几份可执行文件，要显式点名（--target / --exe）。
#      完整 CLI（ecapture）在 Linux 上还没迁移，指名要它会当场得到原因，不会被换成别的进程；
#      也就是说：这个入口永远不会默认发起一次真实截图。
#   2) 默认配置是 Debug（带 -g 符号）；产物没有调试符号时如实警告，不假装能看清源码行。
#   3) 不 sudo、不装东西。调试器不在 PATH 上就报缺依赖和建议命令。
#
# 不依赖 PowerShell，也不碰 Windows 那套入口。
# ---------------------------------------------------------------------------
set -euo pipefail

# 不用 dirname：coreutils 不在 PATH 上时它只是一行噪音，而 cd/pwd 是 bash 自带。
_self="${BASH_SOURCE[0]}"
_self_dir="${_self%/*}"
if [ "$_self_dir" = "$_self" ]; then _self_dir="."; fi
EC_ROOT="$(cd "$_self_dir" && pwd -P)"
# shellcheck source=scripts/dev-common.sh
source "$EC_ROOT/scripts/dev-common.sh"

usage() {
    cat <<'EOF'
用法：./debug.sh [选项] [-- 传给被调试程序的参数]

  --target core-tests|wgc-tests|cli-text-tests|save-tests|ipc-tests|crop-tests|
           image-tests|encode-tests|gui-probe      要调试哪个目标，默认 core-tests
  --exe <路径>                               直接点名要调试的可执行文件
  --debugger gdb|lldb                        调试器，默认自动挑（先 gdb 后 lldb）
  --config Debug|Release|RelWithDebInfo       从哪份构建目录取产物，默认 Debug
  --compiler gcc|clang|<路径>                 决定构建目录名，默认按 CXX / PATH 挑
  --eval "<调试器命令>"                       可重复：一条调试器命令作为一个整体递进去
                                              （gdb 走 -ex，lldb 走 -o；空格在引号里不会劈开）
  --script <文件>                            可重复：调试器脚本文件（gdb -x，lldb -s）。
                                              要递带空格的路径或长串命令就用它，别拼进 --eval
  --batch                                    非交互：批处理模式跑完就退（配 --eval / --script 用）
  --rebuild                                  先重新构建这一份产物再进调试器
  --no-build                                 只用现成产物；没有就按前置不成立报，不会去调试旧文件
  -h, --help                                 本帮助

例子：
  ./debug.sh                                    # gdb 打开核心边界判据（Debug）
  ./debug.sh --target wgc-tests --config RelWithDebInfo
  ./debug.sh --exe ./build-linux/gcc-debug/ecapture-core-tests
  ./debug.sh --batch --eval "break main" --eval run --eval bt
  ./debug.sh --batch --script /tmp/ecapture-debug.gdb
  ./debug.sh --debugger lldb --eval "break set -f core_boundary_state.cpp -l 343" --eval run

说明：
  * 本脚本不发起真实截图，也没有任何目标会取像素。
  * 需要断点在源码行上就用 Debug 或 RelWithDebInfo；Release 通常只剩栈信息。
  * 调试器缺失时报缺依赖，不会自动 sudo apt install。
  * --eval 一条就是一整个参数：gdb 的 "-ex break main" 必须把 "break main" 当一句，
    按空格劈成 -ex break main 会让 gdb 在装载可执行文件之前就去下断点，当场下不上。
EOF
}

WANT="core-tests"
EXE_PATH=""
DEBUGGER=""
EC_CONFIG_IN="Debug"
COMPILER_REQ=""
BATCH=0
EVALS=()
SCRIPTS=()
NO_BUILD=0
REBUILD=0
PROG_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --target) [ $# -ge 2 ] || ec_die_args "--target 需要一个目标名"; WANT="$2"; shift 2 ;;
        --exe) [ $# -ge 2 ] || ec_die_args "--exe 需要一个路径"; EXE_PATH="$2"; shift 2 ;;
        --debugger) [ $# -ge 2 ] || ec_die_args "--debugger 需要 gdb 或 lldb"; DEBUGGER="$2"; shift 2 ;;
        --config) [ $# -ge 2 ] || ec_die_args "--config 需要一个取值"; ec_normalize_config "$2" || ec_die_args "配置只能是 Debug / Release / RelWithDebInfo，收到 '$2'"; EC_CONFIG_IN="$EC_CONFIG"; shift 2 ;;
        --compiler) [ $# -ge 2 ] || ec_die_args "--compiler 需要一个取值"; COMPILER_REQ="$2"; shift 2 ;;
        --eval) [ $# -ge 2 ] || ec_die_args "--eval 需要一条调试器命令"; EVALS+=("$2"); shift 2 ;;
        --script) [ $# -ge 2 ] || ec_die_args "--script 需要一个文件路径"; SCRIPTS+=("$2"); shift 2 ;;
        --batch) BATCH=1; shift ;;
        --rebuild) REBUILD=1; shift ;;
        --no-build) NO_BUILD=1; shift ;;
        --) shift; while [ $# -gt 0 ]; do PROG_ARGS+=("$1"); shift; done ;;
        *) ec_die_args "不认识参数 '$1'（./debug.sh --help）" ;;
    esac
done

ec_normalize_config "$EC_CONFIG_IN" || ec_die_args "非法配置 $EC_CONFIG_IN"

case "$DEBUGGER" in
    ""|gdb|lldb) ;;
    *) ec_die_args "--debugger 只能是 gdb 或 lldb，收到 '$DEBUGGER'" ;;
esac

if [ -n "$EXE_PATH" ]; then
    if [ ! -f "$EXE_PATH" ]; then ec_die_precondition "--exe 给的路径不存在：$EXE_PATH"; fi
    if [ ! -x "$EXE_PATH" ]; then ec_die_precondition "--exe 给的文件不可执行：$EXE_PATH"; fi
    ARTIFACT="$EXE_PATH"
    TARGET_LAYER="offline"
    case "$ARTIFACT" in
        *gui-probe*) TARGET_LAYER="gui" ;;
    esac
else
    ec_resolve_cxx "$COMPILER_REQ"
    ec_set_build_dir
    ec_select_target "$WANT"          # ecapture 走到这里就会把"暂不可构建"说清楚
    TARGET_LAYER="$EC_TARGET_LAYER"

    # 符号构建：产物不在（或点名要重来）就先按选定配置建一份，构建参数只有一份出处（./build.sh）。
    # 默认配置是 Debug，所以这一步交出去的就是带 -g 的那一份。
    if [ "$NO_BUILD" -eq 0 ]; then
        if [ ! -x "$EC_BUILD_DIR/$EC_TARGET_CMAKE" ] || [ "$REBUILD" -eq 1 ]; then
            ec_note "== 构建 $EC_TARGET_CMAKE（$EC_CONFIG，$EC_BUILD_DIR）=="
            if ! "$EC_ROOT/build.sh" --config "$EC_CONFIG" --compiler "$EC_CXX" --target "$EC_TARGET_CMAKE"; then
                ec_die_failed "构建没过，不进入调试会话（调试旧产物只会把人带偏）。"
            fi
        fi
    fi
    ec_require_artifact "$EC_BUILD_DIR" "$EC_TARGET_CMAKE"
    ARTIFACT="$EC_ARTIFACT"
    ec_note "目标：$EC_TARGET_CMAKE（$EC_CXX / $EC_CONFIG） 构建目录：$EC_BUILD_DIR"
fi

# ---- 调试器挑选（不安装、不提权）----
if [ -z "$DEBUGGER" ]; then
    if ec_have gdb; then DEBUGGER=gdb
    elif ec_have lldb; then DEBUGGER=lldb
    else
        ec_die_precondition "gdb 与 lldb 都不在 PATH 上，没法起调试会话。
建议（本项目不自动安装）：apt install gdb  或  apt install lldb"
    fi
    ec_note "调试器：$DEBUGGER（PATH 上现成的挑的，可用 --debugger 指定）"
else
    ec_have "$DEBUGGER" || ec_die_precondition "--debugger $DEBUGGER 不在 PATH 上（建议：apt install $DEBUGGER）"
    ec_note "调试器：$DEBUGGER"
fi

# ---- 符号检查：只警告，不编造"能看清源码行"----
HAS_DEBUG_SECTIONS=0
if ec_have readelf; then
    if readelf --section-headers "$ARTIFACT" 2>/dev/null | grep -q '\.debug_'; then
        HAS_DEBUG_SECTIONS=1
    fi
elif ec_have file; then
    if file "$ARTIFACT" 2>/dev/null | grep -qE 'not stripped|debug'; then HAS_DEBUG_SECTIONS=1; fi
fi
if [ "$HAS_DEBUG_SECTIONS" -eq 0 ]; then
    ec_warn "$ARTIFACT 里没有 .debug_* 段（配置 $EC_CONFIG_IN 不带符号或产物已被 strip）。\
断点落不到源码行；用 --config Debug 重新构建再来。"
else
    ec_note "符号：带 .debug_* 段，可以下源码断点"
fi

# ---- 组装命令行 ----
RUN=()
# GUI 目标要图形会话：有 xvfb-run 就把整个调试会话挂在虚拟显示上（连 gdb 一起包，
# gdb 起的子进程继承同一份 DISPLAY）；没有 xvfb-run 但当前有会话就直接用，并说清楚。
if [ "$TARGET_LAYER" = "gui" ]; then
    if ! ec_graphic_prefix; then
        ec_die_precondition "GUI 目标要一个图形会话，当前既没有 DISPLAY/WAYLAND_DISPLAY，也没有 xvfb-run。
建议（本项目不自动装依赖、不替你启动显示服务）：apt install xvfb"
    fi
    ec_note "图形环境：$EC_GFX_WHERE"
    if [ ${#EC_GFX_PREFIX_ARGS[@]} -gt 0 ]; then
        RUN+=("${EC_GFX_PREFIX_ARGS[@]}")
    fi
fi
RUN+=("$DEBUGGER")
if [ "$BATCH" -eq 1 ]; then
    RUN+=(--batch)
fi
if [ ${#SCRIPTS[@]} -gt 0 ]; then
    for f in "${SCRIPTS[@]}"; do
        if [ ! -f "$f" ]; then ec_die_args "--script 指的文件不存在：$f"; fi
        case "$DEBUGGER" in
            gdb) RUN+=(-x "$f") ;;
            lldb) RUN+=(-s "$f") ;;
        esac
    done
fi
if [ ${#EVALS[@]} -gt 0 ]; then
    for e in "${EVALS[@]}"; do
        # 一条命令 = 一个 argv。绝不在这条路上做单词拆分（原因见文件头 --eval 那一段）。
        case "$DEBUGGER" in
            gdb) RUN+=(-ex "$e") ;;
            lldb) RUN+=(-o "$e") ;;
        esac
    done
fi

case "$DEBUGGER" in
    gdb)
        RUN+=(--args "$ARTIFACT")
        ;;
    lldb)
        RUN+=("$ARTIFACT")
        if [ ${#PROG_ARGS[@]} -gt 0 ]; then RUN+=(--); RUN+=("${PROG_ARGS[@]}"); fi
esac

if [ "$DEBUGGER" = gdb ] && [ ${#PROG_ARGS[@]} -gt 0 ]; then
    RUN+=("${PROG_ARGS[@]}")
fi

ec_note "启动：${RUN[*]}"
if [ -t 0 ]; then
    ec_note "（交互会话；被调试的这份程序不取任何像素）"
fi
exec "${RUN[@]}"
