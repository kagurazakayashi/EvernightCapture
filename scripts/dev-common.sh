#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Linux 开发入口脚本的共用部分（build.sh / run.sh / debug.sh / test-all.sh 都 source 它）
#
# 这里只放"每个入口都要做而且容易做错"的几件事：定位仓库、选构建目录、解析目标名、
# 找产物、探图形会话。判据一条都不在这里 —— 判据在 tests/ 与 src/ 里。
#
# 约定（与 CMakePresets.json 一致，两边落到同一个目录，不会各建一份缓存）：
#   * Linux 构建目录永远是 build-linux/<编译器>-<配置小写>；Windows 的 build/ 一个字节都不碰。
#   * 路径里带空格与中文是常态：所有取值都加引号，禁止裸 $ROOT 拼命令。
#   * 不装东西、不提权、不改用户桌面：缺依赖就报原因和建议命令，由用户决定（AGENTS.md）。
#   * 需要"要么成功要么当场终止"的解析一律写全局变量，不用 $( ) 取值 ——
#     命令替换里的 exit 只退子进程，会把"前置不成立"变成一次静默继续。
# ---------------------------------------------------------------------------

: "${EC_ROOT:?dev-common.sh 需要调用方先给出 EC_ROOT（仓库根的绝对路径）}"

EC_BUILD_ROOT_NAME="build-linux"

# ---------------------------------------------------------------------------
# 输出与退出码
#
# 一档一个含义，与 .\test-all.ps1 的位数对齐，两个平台对得上：
#   0 = 成功；1 = 判据没过；2 = 前置/依赖不成立；3 = 参数不合法。
# ---------------------------------------------------------------------------
ec_note() { printf '%s\n' "$*"; }
ec_warn() { printf '警告：%s\n' "$*" >&2; }
ec_die_precondition() { printf '前置不成立：%s\n' "$*" >&2; exit 2; }
ec_die_args() { printf '参数不合法：%s\n' "$*" >&2; exit 3; }
ec_die_failed() { printf '失败：%s\n' "$*" >&2; exit 1; }

ec_have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# 配置与编译器
# ---------------------------------------------------------------------------
EC_CONFIG=""
ec_normalize_config() {
    case "$1" in
        Debug|debug) EC_CONFIG="Debug" ;;
        Release|release) EC_CONFIG="Release" ;;
        RelWithDebInfo|relwithdebinfo) EC_CONFIG="RelWithDebInfo" ;;
        *) EC_CONFIG=""; return 1 ;;
    esac
}

# 编译器标签：gcc / clang；用户给绝对路径时取文件名，两条路线不会撞进同一个目录
ec_compiler_tag() {
    local base
    base="${1##*/}"      # 不用 basename：coreutils 缺席时它只是噪音，而这是纯字符串活
    case "$base" in
        g++*|gcc*) printf 'gcc' ;;
        clang++*|clang*) printf 'clang' ;;
        *) printf '%s' "$base" ;;
    esac
}

EC_CXX=""
ec_resolve_cxx() {
    # --compiler 优先，其次环境变量 CXX，最后按 PATH 现成的挑；一个都没有就报缺依赖。
    local requested="${1:-}" c
    if [ -n "$requested" ]; then
        case "$requested" in
            gcc) requested="${CXX:-g++}" ;;
            clang) requested="${CXX:-clang++}" ;;
        esac
        ec_have "$requested" || ec_die_precondition "找不到编译器 $requested（本脚本不装依赖：apt install g++ 或 clang）"
        EC_CXX="$requested"
        return
    fi
    if [ -n "${CXX:-}" ]; then
        ec_have "$CXX" || ec_die_precondition "环境变量 CXX=$CXX 在 PATH 里找不到"
        EC_CXX="$CXX"
        return
    fi
    for c in g++ clang++; do
        if ec_have "$c"; then EC_CXX="$c"; return; fi
    done
    ec_die_precondition "PATH 里既没有 g++ 也没有 clang++（建议：apt install g++）"
}

EC_BUILD_DIR=""
ec_set_build_dir() {
    local tag
    tag="$(ec_compiler_tag "$EC_CXX")"
    EC_BUILD_DIR="$EC_ROOT/$EC_BUILD_ROOT_NAME/$tag-$(printf '%s' "$EC_CONFIG" | tr '[:upper:]' '[:lower:]')"
}

# 缓存里已写死的生成器：不带上它，"先手动 cmake -G Ninja 再用脚本"会当场报生成器不一致，
# 报错信息对新人完全没有指导性。读出来原样 -G 传回去，同一份缓存就一直在同一套规则下用。
EC_GENERATOR=""
ec_read_cache_generator() {
    local cache="$1/CMakeCache.txt"
    EC_GENERATOR=""
    [ -f "$cache" ] || return 0
    EC_GENERATOR="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=\(.*\)$/\1/p' "$cache" | head -1)"
}

ec_pick_generator() {
    # 新配置才需要挑：有 Ninja 用 Ninja（与 Windows 同一套，行为更接近），否则退回 Makefiles。
    if ec_have ninja; then EC_GENERATOR="Ninja"
    elif ec_have make; then EC_GENERATOR="Unix Makefiles"
    else ec_die_precondition "既没有 ninja 也没有 make：构建工具缺失（建议 apt install ninja-build）"; fi
}

# ---------------------------------------------------------------------------
# 目标名
#
# 这一张表管住"脚本里能点名的可执行目标"。Linux 本步只有三份：两份离线判据 + 一个 wx
# 依赖探针。完整 CLI（ecapture）仍然只在 CMake 的【平台层 · Windows】那一段定义 ——
# 指名要它时如实拒绝并说清缺口，既不静默忽略，也不退回运行别处那份旧的 ECAPTURE.EXE。
# ---------------------------------------------------------------------------
EC_TARGET_CMAKE=""
EC_TARGET_LAYER=""
ec_select_target() {
    local want="$1"
    case "$want" in
        core-tests|ecapture-core-tests) EC_TARGET_CMAKE="ecapture-core-tests" ;;
        wgc-tests|ecapture-wgc-tests) EC_TARGET_CMAKE="ecapture-wgc-tests" ;;
        cli-text-tests|ecapture-cli-text-tests) EC_TARGET_CMAKE="ecapture-cli-text-tests" ;;
        save-tests|ecapture-save-tests) EC_TARGET_CMAKE="ecapture-save-tests" ;;
        ipc-tests|ecapture-ipc-tests) EC_TARGET_CMAKE="ecapture-ipc-tests" ;;
        gui-probe|ecapture-gui-probe) EC_TARGET_CMAKE="ecapture-gui-probe" ;;
        ecapture|ecapture.exe|ECAPTURE.EXE)
            ec_die_precondition "完整 CLI（ecapture / ECAPTURE.EXE）在 Linux 上暂不可构建。
命令行文本这一层已经跨平台了（参数解析、配置、四语文案、路径与文件、标准流都在共用源文件里），
辅助进程通信这一层的 Linux 平台服务也已经落地（PosixHostProcess，判据在 ipc-tests），
还缺的是这几段：src/Report.cpp 里响应组装要调的窗口/屏幕枚举与取帧通道、EnvReport 的 Windows
探测、NotificationToast 的 WinRT 通知、Consent 的确认框，以及 Linux 截图后端本体。
现在能跑的：core-tests、wgc-tests、cli-text-tests、save-tests、ipc-tests（都一个像素都不取），以及 gui-probe。"
            ;;
        "") ec_die_args "要跟一个可执行目标，但没给名字（可选：core-tests wgc-tests cli-text-tests save-tests ipc-tests gui-probe）" ;;
        *) ec_die_args "不认识的目标 '$want'（可选：core-tests wgc-tests cli-text-tests save-tests ipc-tests gui-probe；ecapture 会如实告知暂不可构建）" ;;
    esac
    case "$EC_TARGET_CMAKE" in
        ecapture-gui-probe) EC_TARGET_LAYER="gui" ;;
        *) EC_TARGET_LAYER="offline" ;;
    esac
}

# 产物路径写进 EC_ARTIFACT；不存在就按"前置不成立"终止，绝不拿旧产物继续跑。
EC_ARTIFACT=""
ec_require_artifact() {
    local dir="$1" cmake_target="$2" hint
    EC_ARTIFACT="$dir/$cmake_target"
    if [ ! -f "$EC_ARTIFACT" ]; then
        hint="先跑 ./build.sh --config <配置> --target $cmake_target"
        [ -d "$dir" ] || hint="$hint（这一份配置还没生成：$dir 不存在）"
        ec_die_precondition "找不到可执行文件 $EC_ARTIFACT。
入口脚本绝不运行别处的旧产物：$hint。"
    fi
    if [ ! -x "$EC_ARTIFACT" ]; then
        ec_die_precondition "$EC_ARTIFACT 存在但没有执行位（产物被动过权限？用 ./build.sh --clean 重来）"
    fi
}

# 构建目录里那份由 CMake 生成的可构建清单（没有这份文件 = 这一份配置不是本项目生成的）
EC_BUILDABLE_FILE=""
ec_read_buildable() {
    EC_BUILDABLE_FILE="$1/ecapture-buildable-targets.txt"
}

ec_target_is_buildable() {
    [ -f "$EC_BUILDABLE_FILE" ] || return 1
    grep -q "^target=$1 " "$EC_BUILDABLE_FILE"
}

# ---------------------------------------------------------------------------
# 图形会话（只读判断；不启动服务、不改 DISPLAY、不碰用户真实桌面）
#
#   1) 有 xvfb-run -> 用它，跑在虚拟显示上，与用户桌面完全隔离
#   2) 没有 xvfb-run 但当前已有 DISPLAY/WAYLAND_DISPLAY -> 直接跑，并说明用的是当前会话
#      （GUI 探针不建窗口、不取像素，所以这一步不会拍到桌面上任何东西）
#   3) 两者都没有 -> 返回 1，调用方按"缺前置"报，不算通过、也不算跳过
# ---------------------------------------------------------------------------
ec_has_display_env() {
    [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]
}

EC_GFX_PREFIX_ARGS=()
EC_GFX_WHERE=""
ec_graphic_prefix() {
    EC_GFX_PREFIX_ARGS=()
    EC_GFX_WHERE=""
    if ec_have xvfb-run; then
        # 数组而不是字符串：--server-args 的取值自带空格，用字符串再按词拆开会把它劈成三段。
        EC_GFX_PREFIX_ARGS=(xvfb-run --auto-servernum "--server-args=-screen 0 1024x768x24")
        EC_GFX_WHERE="xvfb-run 虚拟显示"
        return 0
    fi
    if ec_has_display_env; then
        EC_GFX_WHERE="当前桌面会话（DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}）"
        return 0
    fi
    return 1
}
