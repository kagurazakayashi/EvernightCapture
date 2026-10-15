#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# EvernightCapture —— Linux 运行入口
#
# 这一层现在能跑的东西一个像素都不取：两份离线判据 + wxWidgets 依赖探针。
# 本脚本绝不发起真实截图、绝不弹确认框、绝不因为"找不到产物"而退回去跑别处的旧可执行文件。
#
# 退出码：0 成功；1 被运行的程序自己返回非 0；2 前置/依赖不成立；3 参数不合法。
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
用法：./run.sh [选项] [-- 传给被运行程序的参数]

  --target core-tests|wgc-tests|cli-text-tests|save-tests|ipc-tests|crop-tests|
           image-tests|encode-tests|gui-probe      要运行哪个目标，默认 core-tests
  --exe <路径>                               直接点名一个可执行文件（跳过 --target 的解析）
  --config Debug|Release|RelWithDebInfo      从哪一份构建目录里取产物，默认 Release
  --compiler gcc|clang|<路径>                 同上（决定构建目录名），默认按 CXX / PATH 挑
  -h, --help                                 本帮助

说明：
  * 想跑完整 CLI（ecapture / ECAPTURE.EXE）会当场得到"暂不可构建"的原因，不会被静默换成别的东西。
  * gui-probe 需要一个图形会话：有 xvfb-run 时自动跑在虚拟显示上，否则用当前会话并说明。
    它不建窗口、不弹框、不取像素。

例子：
  ./run.sh                                   # 跑核心边界判据
  ./run.sh --target wgc-tests
  ./run.sh --config Debug --target gui-probe
  ./run.sh --exe ./build-linux/gcc-debug/ecapture-core-tests
EOF
}

WANT="core-tests"
EXE_PATH=""
EC_CONFIG_IN="Release"
COMPILER_REQ=""
PROG_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --target) [ $# -ge 2 ] || ec_die_args "--target 需要一个目标名"; WANT="$2"; shift 2 ;;
        --exe) [ $# -ge 2 ] || ec_die_args "--exe 需要一个路径"; EXE_PATH="$2"; shift 2 ;;
        --config) [ $# -ge 2 ] || ec_die_args "--config 需要一个取值"; ec_normalize_config "$2" || ec_die_args "配置只能是 Debug / Release / RelWithDebInfo，收到 '$2'"; EC_CONFIG_IN="$EC_CONFIG"; shift 2 ;;
        --compiler) [ $# -ge 2 ] || ec_die_args "--compiler 需要一个取值"; COMPILER_REQ="$2"; shift 2 ;;
        --) shift; while [ $# -gt 0 ]; do PROG_ARGS+=("$1"); shift; done ;;
        *) ec_die_args "不认识参数 '$1'（./run.sh --help）" ;;
    esac
done

ec_normalize_config "$EC_CONFIG_IN" || ec_die_args "非法配置 $EC_CONFIG_IN"

if [ -n "$EXE_PATH" ]; then
    # 点名要跑的文件：只判存在与可执行，不去猜它是不是本项目构建的那一份
    if [ ! -f "$EXE_PATH" ]; then
        ec_die_precondition "--exe 给的路径不存在：$EXE_PATH"
    fi
    if [ ! -x "$EXE_PATH" ]; then
        ec_die_precondition "--exe 给的文件不可执行：$EXE_PATH"
    fi
    ARTIFACT="$EXE_PATH"
    LAYER="offline"
    case "$ARTIFACT" in
        *gui-probe*) LAYER="gui" ;;
    esac
else
    ec_resolve_cxx "$COMPILER_REQ"
    ec_set_build_dir
    ec_select_target "$WANT"
    ec_require_artifact "$EC_BUILD_DIR" "$EC_TARGET_CMAKE"
    ARTIFACT="$EC_ARTIFACT"
    LAYER="$EC_TARGET_LAYER"
    ec_note "目标：$EC_TARGET_CMAKE（$EC_CXX / $EC_CONFIG）"
fi

if [ "$LAYER" = "gui" ]; then
    if ! ec_graphic_prefix; then
        ec_die_precondition "GUI 目标要一个图形会话，当前既没有 DISPLAY/WAYLAND_DISPLAY，也没有 xvfb-run。
建议：apt install xvfb（本项目不自动装依赖，也不替你启动显示服务）"
    fi
    ec_note "图形环境：$EC_GFX_WHERE（探针不建窗口、不弹框、不取像素）"
    if [ ${#EC_GFX_PREFIX_ARGS[@]} -gt 0 ]; then
        exec "${EC_GFX_PREFIX_ARGS[@]}" "$ARTIFACT" "${PROG_ARGS[@]}"
    fi
fi

ec_note "运行：$ARTIFACT ${PROG_ARGS[*]-}"
exec "$ARTIFACT" "${PROG_ARGS[@]}"
