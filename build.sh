#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# EvernightCapture —— Linux 构建入口（Windows 侧仍然用 .\build.ps1，两边不共享缓存）
#
# 干什么：定位编译器 -> 配置到 build-linux/<编译器>-<配置> -> 构建。
# 不干什么：不装依赖、不提权、不改用户桌面；Linux 上本步能构建的目标就是清单里那几份，
#           完整 CLI 仍然只在 Windows 分支定义（./build.sh --list-targets 会说清楚）。
#
# 退出码：0 成功；1 配置或编译失败；2 前置/依赖不成立；3 参数不合法。
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
用法：./build.sh [选项] [-- 转发给 cmake --build 的额外参数]

  --config Debug|Release|RelWithDebInfo   构建配置，默认 Release
                                          （RelWithDebInfo 带调试符号，调试时可优先选它）
  --compiler gcc|clang|<编译器路径>        指定编译器；留空则用 $CXX，再退到 PATH 上的 g++/clang++
  --target <名字>                          只构建某个目标，可重复给
                                          可选：core-tests wgc-tests cli-text-tests gui-probe
                                          （ecapture 会如实告知它在 Linux 上暂不可构建）
  --list-targets                          打印当前平台可构建与暂不可构建的目标清单，不构建
  --clean                                 先删掉这一份构建目录再重新配置（只删 build-linux 下那一个）
  --gui                                   wxWidgets 缺失时直接失败（默认找到了才建 GUI 目标）
  --no-gui                                完全不配置 GUI 目标
  --wx-config <路径>                       多版本共存时点名 wx-config
  --verbose                               构建时打印完整编译命令
  -h, --help                              本帮助

例子：
  ./build.sh                                   # Release，g++
  ./build.sh --config Debug --compiler clang   # Debug，clang++
  ./build.sh --target wgc-tests -- -j2         # 只建 wgc 判据，把 -j2 转给 cmake --build
  ./build.sh --clean --config Debug            # 重来一份带符号的构建目录

构建目录：仓库根的 build-linux/<编译器>-<配置小写>，与 Windows 的 build/ 互不影响。
EOF
}

# ---- 参数 ----
EC_CONFIG_IN="Release"
COMPILER_REQ=""
TARGETS=()
DO_CLEAN=0
GUI_MODE="optional"      # optional | required | off
WX_CONFIG_PATH=""
VERBOSE=0
LIST_ONLY=0
BUILD_EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --config) [ $# -ge 2 ] || ec_die_args "--config 需要一个取值"; ec_normalize_config "$2" || ec_die_args "配置只能是 Debug / Release / RelWithDebInfo，收到 '$2'"; EC_CONFIG_IN="$EC_CONFIG"; shift 2 ;;
        --compiler) [ $# -ge 2 ] || ec_die_args "--compiler 需要一个取值"; COMPILER_REQ="$2"; shift 2 ;;
        --target) [ $# -ge 2 ] || ec_die_args "--target 需要一个目标名"; TARGETS+=("$2"); shift 2 ;;
        --clean) DO_CLEAN=1; shift ;;
        --gui) GUI_MODE="required"; shift ;;
        --no-gui) GUI_MODE="off"; shift ;;
        --wx-config) [ $# -ge 2 ] || ec_die_args "--wx-config 需要一个路径"; WX_CONFIG_PATH="$2"; shift 2 ;;
        --verbose) VERBOSE=1; shift ;;
        --list-targets) LIST_ONLY=1; shift ;;
        --) shift; while [ $# -gt 0 ]; do BUILD_EXTRA+=("$1"); shift; done ;;
        -*) ec_die_args "不认识选项 $1（./build.sh --help）" ;;
        *) ec_die_args "不认位置参数 '$1'（本脚本没有位置参数；要转发给 cmake --build 请先写 --）" ;;
    esac
done

ec_normalize_config "$EC_CONFIG_IN" || ec_die_args "非法配置 $EC_CONFIG_IN"
ec_resolve_cxx "$COMPILER_REQ"
ec_set_build_dir

# ---- 目标名单：--target 解析 + CMakeLists 里那份"只在 Windows 定义"的清单 ----
ec_select_list_or_all() {
    if [ ${#TARGETS[@]} -gt 0 ]; then
        local t
        for t in "${TARGETS[@]}"; do
            ec_select_target "$t"
            printf '  %s\n' "$EC_TARGET_CMAKE"
        done
    else
        printf '  %s\n' "（未指定：构建本平台当前定义的全部目标）"
    fi
}

if [ "$LIST_ONLY" -eq 1 ]; then
    ec_note "编译器：$EC_CXX    配置：$EC_CONFIG    构建目录：$EC_BUILD_DIR"
    ec_read_buildable "$EC_BUILD_DIR"
    if [ -f "$EC_BUILDABLE_FILE" ]; then
        ec_note "当前可构建目标（读自 $EC_BUILD_DIR/ecapture-buildable-targets.txt）："
        grep '^target=' "$EC_BUILDABLE_FILE" | sed 's/^target=/  /; s/ artifact=.*//'
        ec_note "只在 Windows 分支定义、Linux 暂不可构建："
        sed -n 's/^windows-only=//p' "$EC_BUILDABLE_FILE" | tr ',' '\n' | sed 's/^/  /'
    else
        ec_note "这一份构建目录还没配置过，先跑一次 ./build.sh 再 --list-targets 才有清单。"
        ec_note "按 CMakeLists.txt 当前的划分，Linux 可构建：core-tests、wgc-tests、cli-text-tests，"
        ec_note "装了 wxWidgets 再加 gui-probe；完整 CLI（ecapture）暂不可构建。"
    fi
    exit 0
fi

# 提前把 --target 的名字校验一遍：拼错不该等到 cmake 才报一堆。
if [ ${#TARGETS[@]} -gt 0 ]; then
    for t in "${TARGETS[@]}"; do ec_select_target "$t"; done
fi
ec_note "目标清单："
ec_select_list_or_all

# ---- --clean：只允许删 build-linux 下面那一个目录 ----
if [ "$DO_CLEAN" -eq 1 ]; then
    case "$EC_BUILD_DIR" in
        "$EC_ROOT/$EC_BUILD_ROOT_NAME"/*) ;;
        *) ec_die_args "--clean 拒绝删除不在 $EC_BUILD_ROOT_NAME/ 下的路径：$EC_BUILD_DIR" ;;
    esac
    if [ -L "$EC_BUILD_DIR" ]; then
        ec_die_precondition "$EC_BUILD_DIR 是一个符号链接，不是本脚本建的构建目录，拒绝删除"
    fi
    if [ -d "$EC_BUILD_DIR" ]; then
        ec_note "清理构建目录：$EC_BUILD_DIR"
        rm -rf -- "$EC_BUILD_DIR"
    else
        ec_note "没有构建目录可清理（$EC_BUILD_DIR 不存在）"
    fi
fi

# ---- 前置工具 ----
ec_have cmake || ec_die_precondition "找不到 cmake（建议：apt install cmake；本项目不自动安装依赖）"
cmake --version >/dev/null || ec_die_precondition "cmake 在 PATH 里但跑不起来"

ec_read_cache_generator "$EC_BUILD_DIR"
if [ -n "$EC_GENERATOR" ]; then
    ec_note "生成器：$EC_GENERATOR（沿用这一份缓存里已写死的）"
else
    ec_pick_generator
    gen_note="$EC_GENERATOR"
    [ "$gen_note" = "Ninja" ] || ec_warn "没找到 ninja，改用 $EC_GENERATOR（Windows 侧用 Ninja，两边构建图会略有差别）"
    ec_note "生成器：$EC_GENERATOR"
fi

mkdir -p -- "$EC_BUILD_DIR"

# ---- 配置参数 ----
CFG_ARGS=(
    -S "$EC_ROOT"
    -B "$EC_BUILD_DIR"
    -G "$EC_GENERATOR"
    "-DCMAKE_BUILD_TYPE=$EC_CONFIG"
    "-DCMAKE_CXX_COMPILER=$EC_CXX"
)
# wxWidgets 两个开关每次都显式传值：CMake 缓存会把上一次 --gui 写的
# ECAPTURE_REQUIRE_WX=ON 一直留着，不覆盖的话下一次"普通构建"会被上一次的选择拖死。
case "$GUI_MODE" in
    required) CFG_ARGS+=(-DECAPTURE_ENABLE_GUI=ON -DECAPTURE_REQUIRE_WX=ON) ;;
    off) CFG_ARGS+=(-DECAPTURE_ENABLE_GUI=OFF -DECAPTURE_REQUIRE_WX=OFF) ;;
    *) CFG_ARGS+=(-DECAPTURE_ENABLE_GUI=ON -DECAPTURE_REQUIRE_WX=OFF) ;;
esac
if [ -n "$WX_CONFIG_PATH" ]; then
    if ! ec_have "$WX_CONFIG_PATH"; then
        [ -x "$WX_CONFIG_PATH" ] || ec_die_args "--wx-config 给的路径不可执行：$WX_CONFIG_PATH"
    fi
    CFG_ARGS+=("-DECAPTURE_WX_CONFIG=$WX_CONFIG_PATH")
else
    # 同上面两个开关：不显式清空，上一次 --wx-config 指过去的那一份会赖在缓存里
    CFG_ARGS+=("-DECAPTURE_WX_CONFIG=")
fi

ec_note "编译器：$EC_CXX    配置：$EC_CONFIG    构建目录：$EC_BUILD_DIR"
ec_note "wxWidgets：$([ "$GUI_MODE" = required ] && echo '必需（缺失即失败）' || { [ "$GUI_MODE" = off ] && echo '不参与' || echo '可选'; })"

if ! cmake "${CFG_ARGS[@]}"; then
    if [ "$GUI_MODE" = "required" ]; then
        ec_die_precondition "配置阶段失败。带 --gui 时 wxWidgets 缺失会让这一步硬失败：\
按上面的诊断装 libwxgtk3.2-dev（或给出 --wx-config），装好后重跑；只要离线判据就去掉 --gui。"
    fi
    ec_die_failed "cmake 配置阶段失败（见上面 cmake 的输出）"
fi

# ---- 构建 ----
BUILD_ARGS=(--build "$EC_BUILD_DIR")
if [ ${#TARGETS[@]} -gt 0 ]; then
    for t in "${TARGETS[@]}"; do
        ec_select_target "$t"
        BUILD_ARGS+=(--target "$EC_TARGET_CMAKE")
    done
fi
if [ "$VERBOSE" -eq 1 ]; then BUILD_ARGS+=(--verbose); fi
if [ ${#BUILD_EXTRA[@]} -gt 0 ]; then BUILD_ARGS+=(-- "${BUILD_EXTRA[@]}"); fi

if ! cmake "${BUILD_ARGS[@]}"; then
    ec_die_failed "编译阶段失败（见上面构建日志）。构建失败时本脚本不会去运行任何旧产物。"
fi

# ---- 产物汇报 ----
ec_read_buildable "$EC_BUILD_DIR"
if [ ${#TARGETS[@]} -gt 0 ]; then
    for t in "${TARGETS[@]}"; do
        ec_select_target "$t"
        ec_require_artifact "$EC_BUILD_DIR" "$EC_TARGET_CMAKE"
        ec_note "产物：$EC_ARTIFACT"
    done
else
    if [ -f "$EC_BUILDABLE_FILE" ]; then
        while IFS= read -r line; do
            p="${line#*artifact=}"
            if [ -x "$p" ]; then ec_note "产物：$p"; fi
        done < <(grep '^target=' "$EC_BUILDABLE_FILE")
    fi
    if [ "$GUI_MODE" = "optional" ] && ! grep -q '^target=ecapture-gui-probe ' "$EC_BUILDABLE_FILE" 2>/dev/null; then
        ec_note "说明：wxWidgets 未装，GUI 探针目标没有生成（离线判据不受影响）。"
        ec_note "      要看依赖接线的完整诊断：./build.sh --gui"
    fi
fi
ec_note "构建完成。运行：./run.sh --target core-tests ｜ 测试：./test-all.sh --list"
