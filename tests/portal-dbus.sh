#!/usr/bin/env bash
# 在**只属于本次测试**的会话总线上运行模拟 Portal 服务判据。
# 不连接用户真实桌面总线、不启动 xdg-desktop-portal、不打开系统授权窗口、不取像素。
set -euo pipefail
self="${BASH_SOURCE[0]}"
dir="${self%/*}"
[ "$dir" = "$self" ] && dir="."
root="$(cd "$dir/.." && pwd -P)"
arg="${1:-}"
if [ -n "$arg" ] && [ -d "$arg" ]; then
    exe="$arg/ecapture-portal-dbus-tests"
elif [ -n "$arg" ] && [ -x "$arg" ]; then
    exe="$arg"
else
    exe="$root/build-linux/gcc-release/ecapture-portal-dbus-tests"
fi
if ! command -v dbus-daemon >/dev/null 2>&1; then
    echo "NOT RUN: dbus-daemon not found (install dbus-daemon/dbus-user-session package; not auto-installed)" >&2
    exit 2
fi
if [ ! -x "$exe" ]; then
    echo "NOT RUN: $exe not built" >&2
    exit 2
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/ecapture-portal-dbus.XXXXXX")"
bus_pid=""
cleanup() {
    if [ -n "$bus_pid" ]; then
        kill "$bus_pid" 2>/dev/null || true
        wait "$bus_pid" 2>/dev/null || true
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT INT TERM
cat >"$tmp/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:path=$tmp/bus</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/>
    <allow own="*"/>
    <allow send_type="method_call"/>
    <allow send_type="signal"/>
    <allow send_type="method_return"/>
    <allow send_type="error"/>
    <allow receive_type="method_call"/>
    <allow receive_type="signal"/>
    <allow receive_type="method_return"/>
    <allow receive_type="error"/>
  </policy>
</busconfig>
EOF
dbus-daemon --config-file="$tmp/bus.conf" --nofork --nopidfile >"$tmp/bus.log" 2>&1 &
bus_pid=$!
for _ in $(seq 1 100); do
    [ -S "$tmp/bus" ] && break
    sleep 0.02
done
if [ ! -S "$tmp/bus" ]; then
    echo "NOT RUN: private test D-Bus did not start" >&2
    cat "$tmp/bus.log" >&2 || true
    exit 2
fi
DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/bus" "$exe"
