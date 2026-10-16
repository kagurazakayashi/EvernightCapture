#!/usr/bin/env bash
# GUI 层的隔离图形判据入口（Linux）。
#
# 两段：
#   1) 有图形会话：真实创建 wxWidgets 对话框，判同意 / 拒绝 / 关框 / 超时 / 取消、
#      长中文标题与长路径、默认焦点、可访问名称与尺寸上限。
#   2) 没有图形会话：env -u DISPLAY -u WAYLAND_DISPLAY 跑 --init-failure，
#      要求初始化失败并带出原因，绝不能假装成功。
#
# 用法：bash tests/gui-runtime.sh <构建目录或 ecapture-gui-tests 路径>
# 退出码：0 = 两段都通过；1 = 至少一条判据失败；2 = 前置不成立（没产物/没图形会话）。
set -euo pipefail

_self="${BASH_SOURCE[0]}"
_self_dir="${_self%/*}"
if [ "$_self_dir" = "$_self" ]; then _self_dir="."; fi
ROOT="$(cd "$_self_dir/.." && pwd -P)"

BIN="${1:-}"
if [ -z "$BIN" ]; then
    echo "用法：bash tests/gui-runtime.sh <构建目录或 ecapture-gui-tests 路径>" >&2
    exit 2
fi
if [ -d "$BIN" ]; then BIN="$BIN/ecapture-gui-tests"; fi
if [ ! -x "$BIN" ]; then
    echo "前置不成立：找不到可执行的 $BIN（先构建 ecapture-gui-tests）" >&2
    exit 2
fi

LOG_DIR="${TMPDIR:-/tmp}/evernightcapture-gui-runtime-$$"
mkdir -p "$LOG_DIR"
trap 'rm -rf -- "$LOG_DIR"' EXIT

run_and_capture() {
    # $1 = 输出文件；其余 = 命令
    local out="$1"; shift
    local rc=0
    "$@" >"$out" 2>&1 || rc=$?
    cat "$out"
    return "$rc"
}

parse_counts() {
    # 从日志里取最后一条 "N 条通过，M 条失败"
    sed -n 's/^\([0-9][0-9]*\) 条通过，\([0-9][0-9]*\) 条失败$/\1 \2/p' "$1" | tail -1
}

echo "== GUI 判据（有图形会话）=="
display_rc=0
run_and_capture "$LOG_DIR/display.log" "$BIN" || display_rc=$?
display_counts="$(parse_counts "$LOG_DIR/display.log")"
read -r display_pass display_fail <<<"$display_counts" || true
: "${display_pass:=0}"
: "${display_fail:=1}"

echo
echo "== GUI 判据（GDK_SCALE=2，检查高 DPI 尺寸封顶）=="
scaled_rc=0
run_and_capture "$LOG_DIR/scaled.log" env GDK_SCALE=2 "$BIN" || scaled_rc=$?
scaled_counts="$(parse_counts "$LOG_DIR/scaled.log")"
read -r scaled_pass scaled_fail <<<"$scaled_counts" || true
: "${scaled_pass:=0}"
: "${scaled_fail:=1}"

echo
echo "== GUI 初始化失败（去掉 DISPLAY / WAYLAND_DISPLAY）=="
nodisplay_rc=0
run_and_capture "$LOG_DIR/no-display.log" \
    env -u DISPLAY -u WAYLAND_DISPLAY -u WAYLAND_SOCKET -u XDG_SESSION_TYPE \
    "$BIN" --init-failure || nodisplay_rc=$?
nodisplay_counts="$(parse_counts "$LOG_DIR/no-display.log")"
read -r nodisplay_pass nodisplay_fail <<<"$nodisplay_counts" || true
: "${nodisplay_pass:=0}"
: "${nodisplay_fail:=1}"

total_pass=$((display_pass + scaled_pass + nodisplay_pass))
total_fail=$((display_fail + scaled_fail + nodisplay_fail))
echo
echo "GUI 判据：有会话 exit=$display_rc，2x 缩放 exit=$scaled_rc，无会话 exit=$nodisplay_rc"
echo "$total_pass 条通过，$total_fail 条失败"

if [ "$display_rc" -ne 0 ] || [ "$scaled_rc" -ne 0 ] || [ "$nodisplay_rc" -ne 0 ] ||
   [ "$total_fail" -ne 0 ]; then
    exit 1
fi
exit 0
