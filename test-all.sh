#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# EvernightCapture —— Linux 测试总入口（Windows 侧仍然是 .\test-all.ps1）
#
# 三件事：先构建，再按计划**串行**跑每一套判据，最后把"跑到并通过 / 判据没过 / 前置缺失没跑成 /
# 要人来做"分开记账。
#
# 分层（这是本脚本的骨架，不是可选装饰）：
#   offline  离线：一个像素都不取，不需要图形会话。Linux 当前就这一层有真判据。
#   isolated 隔离图形环境：需要图形会话（有 xvfb-run 就跑在虚拟显示上，否则明确说明用的是当前会话）。
#   manual   真实桌面人工验证：需要真人点确认框、真机多屏与 HDR 等现场，脚本不代跑也不算通过。
#
# 三条硬规矩：
#   1) 构建失败立即停，绝不拿旧产物继续跑（构建这一步委派给 ./build.sh，参数只有一份出处）。
#   2) 计划里点名的套件因为缺依赖/缺产物没跑起来 = NOT RUN，逐条写出原因，判失败；
#      跳过、未启动、没构建出来，任何一条都不会被算进"通过"，也不会被合并成一个数字。
#   3) 不装依赖、不 sudo、不碰用户的显示设置与桌面内容；需要人做的那一层只列清单，不代答确认框。
#
# 退出码：0 = 计划内该跑的套件全部跑到并通过（人工层的"未验证"如实列出）；
#         1 = 至少一套判据没过，或计划里的套件因缺前置根本没启动（NOT RUN）；
#         2 = 全局前置不成立（没有 cmake/编译器、构建阶段失败、构建目录不是本项目生成的）；
#         3 = 参数不合法（含不认识的名字、筛完没有可执行测试）。
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
用法：./test-all.sh [选项]

  --list                       只打印执行计划（含每套的层、判什么、依赖），不构建不跑
  --layer offline|isolated|manual|all   跑哪一层，默认 offline
  --only a,b / --except a      按套件名筛（名字见 --list）
  --config Debug|Release|RelWithDebInfo   构建配置，默认 Release
  --compiler gcc|clang|<路径>             编译器，决定构建目录；默认按 CXX / PATH 挑
  --no-build                   用现成产物（仍然逐条判产物在不在，不在就按前置不成立报）
  --stop-on-fail               第一套没过就收
  --log-dir <目录>             日志位置，默认 <构建目录>/test-logs/<时间戳>
  -h, --help                   本帮助

例子：
  ./test-all.sh --list                     # 先看计划
  ./test-all.sh                            # 离线层：构建 + 跑离线判据
  ./test-all.sh --layer isolated           # 需要 wxWidgets；缺依赖会直接报前置不成立
  ./test-all.sh --config Debug --only wgc-geometry
  ./test-all.sh --layer manual             # 真实桌面人工验证清单（脚本不代跑）
EOF
}

# ---------------------------------------------------------------------------
# 套件登记表：一层的归属、判什么、怎么起、缺依赖时说哪一句
#
# 目标名必须是 CMake 里真定义的那一份；跑之前还会拿构建目录里的
# ecapture-buildable-targets.txt 复核"本平台当前确实生成了这个目标"，
# 所以这张表不可能长期漂在实现前面。
# ---------------------------------------------------------------------------
declare -A SUITE_LAYER SUITE_KIND SUITE_TARGET SUITE_NOTE SUITE_PREREQ

SUITE_ORDER="core-portability core-boundary wgc-geometry cli-text save-paths worker-ipc consent-state \
 image-ops crop-geometry codec-encode gui-probe-init gui-runtime"

SUITE_LAYER[core-portability]="offline"
SUITE_KIND[core-portability]="script"
SUITE_TARGET[core-portability]=""
SUITE_NOTE[core-portability]="共用模型不漏平台类型：静态清单 + 平台层归属 + 直接 g++ 编译 + 判据（绕开 CMake 的独立证明）"
SUITE_PREREQ[core-portability]="$EC_ROOT/scripts/core-portability-check.sh"

SUITE_LAYER[core-boundary]="offline"
SUITE_KIND[core-boundary]="cmake"
SUITE_TARGET[core-boundary]="ecapture-core-tests"
SUITE_NOTE[core-boundary]="跨平台核心边界判据（CMake 构建出来的那一份，141 条）"
SUITE_PREREQ[core-boundary]=""

SUITE_LAYER[cli-text]="offline"
SUITE_KIND[cli-text]="cmake"
SUITE_TARGET[cli-text]="ecapture-cli-text-tests"
SUITE_NOTE[cli-text]="命令行文本处理与多语言资源判据（编码边界、四语回退、参数与配置、JSON 转义、通道纪律，146 条）"
SUITE_PREREQ[cli-text]=""

SUITE_LAYER[wgc-geometry]="offline"
SUITE_KIND[wgc-geometry]="cmake"
SUITE_TARGET[wgc-geometry]="ecapture-wgc-tests"
SUITE_NOTE[wgc-geometry]="WGC 动态尺寸与帧池生命周期判据（纯算术，21 条）"
SUITE_PREREQ[wgc-geometry]=""

SUITE_LAYER[save-paths]="offline"
SUITE_KIND[save-paths]="cmake"
SUITE_TARGET[save-paths]="ecapture-save-tests"
SUITE_NOTE[save-paths]="跨平台文件 / 目录 / 原子保存 / 历史 / 配置位置判据（正常、不覆盖、并发、权限、中文路径、部分成功、--portable）"
SUITE_PREREQ[save-paths]=""

SUITE_LAYER[worker-ipc]="offline"
SUITE_KIND[worker-ipc]="cmake"
SUITE_TARGET[worker-ipc]="ecapture-ipc-tests"
SUITE_NOTE[worker-ipc]="跨平台辅助进程通信与超时取消判据（真父子进程：协议错误、双向通信、阻塞、超时、取消、异常退出、端点防伪、并行排干、收尾与泄漏记账，120 条）"
SUITE_PREREQ[worker-ipc]=""

SUITE_LAYER[consent-state]="offline"
SUITE_KIND[consent-state]="cmake"
SUITE_TARGET[consent-state]="ecapture-consent-tests"
SUITE_NOTE[consent-state]="授权状态机与弹框生命周期（假应答器 + 假拓扑：--yes 边界、拒绝传播、拓扑失效、超时/关框/初始化失败/取消、预算暂停，179 条）"
SUITE_PREREQ[consent-state]=""

SUITE_LAYER[image-ops]="offline"
SUITE_KIND[image-ops]="cmake"
SUITE_TARGET[image-ops]="ecapture-image-tests"
SUITE_NOTE[image-ops]="帧内存形状与像素操作判据（合成棋盘/条纹/行末填充/短缓冲、裁剪、旋转裁取、等比缩放、单色判定、上限与文案 key）"
SUITE_PREREQ[image-ops]=""

SUITE_LAYER[crop-geometry]="offline"
SUITE_KIND[crop-geometry]="cmake"
SUITE_TARGET[crop-geometry]="ecapture-crop-tests"
SUITE_NOTE[crop-geometry]="窗口内部裁剪判据（--roi / --client-area：越界拒绝不自动裁短、问不出来与放不下分两档、相加绕回在 64 位里判）"
SUITE_PREREQ[crop-geometry]=""

SUITE_LAYER[codec-encode]="offline"
SUITE_KIND[codec-encode]="cmake"
SUITE_TARGET[codec-encode]="ecapture-encode-tests"
SUITE_NOTE[codec-encode]="跨平台图像处理与编码判据（裁剪→缩放→编码一条链：容器结构独立核对、通道顺序与 alpha、HDR 直通拒绝、预算与产出上限、能力与实请求一致、同一份字节供主交付与历史、不连显示服务器）"
SUITE_PREREQ[codec-encode]=""

SUITE_LAYER[gui-probe-init]="isolated"
SUITE_KIND[gui-probe-init]="cmake"
SUITE_TARGET[gui-probe-init]="ecapture-gui-probe"
SUITE_NOTE[gui-probe-init]="wxWidgets 依赖接线在图形会话里真能初始化（探针不建窗口、不弹框、不取像素）"
SUITE_PREREQ[gui-probe-init]=""

SUITE_LAYER[gui-runtime]="isolated"
SUITE_KIND[gui-runtime]="script"
SUITE_TARGET[gui-runtime]="ecapture-gui-tests"
SUITE_NOTE[gui-runtime]="确认框与公共对话框：长中文/长路径、默认焦点、可访问名称、同意/拒绝/关框/超时/取消、GDK_SCALE=2 高 DPI，以及无图形会话时初始化失败"
SUITE_PREREQ[gui-runtime]="$EC_ROOT/tests/gui-runtime.sh"

# ---------------------------------------------------------------------------
# 参数
# ---------------------------------------------------------------------------
LIST_ONLY=0
LAYER="offline"
ONLY=""
EXCEPT=""
EC_CONFIG_IN="Release"
COMPILER_REQ=""
NO_BUILD=0
STOP_ON_FAIL=0
LOG_DIR_IN=""

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --list) LIST_ONLY=1; shift ;;
        --layer) [ $# -ge 2 ] || ec_die_args "--layer 需要一个取值"; LAYER="$2"; shift 2 ;;
        --only) [ $# -ge 2 ] || ec_die_args "--only 需要套件名"; ONLY="$2"; shift 2 ;;
        --except) [ $# -ge 2 ] || ec_die_args "--except 需要套件名"; EXCEPT="$2"; shift 2 ;;
        --config) [ $# -ge 2 ] || ec_die_args "--config 需要一个取值"; ec_normalize_config "$2" || ec_die_args "配置只能是 Debug / Release / RelWithDebInfo，收到 '$2'"; EC_CONFIG_IN="$EC_CONFIG"; shift 2 ;;
        --compiler) [ $# -ge 2 ] || ec_die_args "--compiler 需要一个取值"; COMPILER_REQ="$2"; shift 2 ;;
        --no-build) NO_BUILD=1; shift ;;
        --stop-on-fail) STOP_ON_FAIL=1; shift ;;
        --log-dir) [ $# -ge 2 ] || ec_die_args "--log-dir 需要一个路径"; LOG_DIR_IN="$2"; shift 2 ;;
        *) ec_die_args "不认识参数 '$1'（./test-all.sh --help）" ;;
    esac
done

case "$LAYER" in
    offline|isolated|manual|all) ;;
    *) ec_die_args "--layer 只能是 offline / isolated / manual / all，收到 '$LAYER'" ;;
esac
ec_normalize_config "$EC_CONFIG_IN" || ec_die_args "非法配置 $EC_CONFIG_IN"

# ---------------------------------------------------------------------------
# 组计划：先按层，再按 --only / --except；名字不认识当场拒。
# ---------------------------------------------------------------------------
PLAN=()
EXCLUDED=()

if [ -n "$ONLY" ]; then
    IFS=',' read -r -a _only <<< "$ONLY"
    for n in "${_only[@]}"; do
        n="$(printf '%s' "$n" | tr -d ' ')"
        [ -n "$n" ] || continue
        case " $SUITE_ORDER " in
            *" $n "*) ;;
            *) ec_die_args "--only 里有不认识的套件名：$n（./test-all.sh --list 看清单）" ;;
        esac
    done
fi
if [ -n "$EXCEPT" ]; then
    IFS=',' read -r -a _except <<< "$EXCEPT"
    for n in "${_except[@]}"; do
        n="$(printf '%s' "$n" | tr -d ' ')"
        [ -n "$n" ] || continue
        case " $SUITE_ORDER " in
            *" $n "*) ;;
            *) ec_die_args "--except 里有不认识的套件名：$n（./test-all.sh --list 看清单）" ;;
        esac
    done
fi

for s in $SUITE_ORDER; do
    sl="${SUITE_LAYER[$s]}"
    if [ "$LAYER" != "all" ] && [ "$LAYER" != "$sl" ]; then
        EXCLUDED+=("$s（属于 $sl 层，本次 --layer $LAYER 没排）")
        continue
    fi
    if [ -n "$ONLY" ]; then
        case ",$ONLY," in
            *",$s,"*) ;;
            *) EXCLUDED+=("$s（--only 没点它）"); continue ;;
        esac
    fi
    if [ -n "$EXCEPT" ]; then
        case ",$EXCEPT," in
            *",$s,"*) EXCLUDED+=("$s（--except 排掉）"); continue ;;
        esac
    fi
    PLAN+=("$s")
done

if [ ${#PLAN[@]} -eq 0 ]; then
    ec_note "计划是空的（--layer $LAYER 与 --only/--except 筛完没有任何套件）。"
    if [ "$LAYER" = "manual" ]; then
        ec_note "manual 层本来就没有可自动跑的套件：Linux 截图后端还没实现，真机验证项从后端落地那一步才开始登记。"
        ec_note "这一层的结论由做验证的人写，脚本不代跑、不代答确认框，也不会把它算成通过。"
    fi
    ec_note "被排除的："
    for e in "${EXCLUDED[@]}"; do ec_note "  - $e"; done
    ec_die_args "没有可执行测试"
fi

print_plan() {
    ec_note "执行计划（--layer $LAYER，配置 $EC_CONFIG_IN）："
    for s in "${PLAN[@]}"; do
        ec_note "  [$s]  层=${SUITE_LAYER[$s]}  ${SUITE_NOTE[$s]}"
        if [ "${SUITE_KIND[$s]}" = "cmake" ]; then
            ec_note "        起法：构建目录里的 ${SUITE_TARGET[$s]}"
        else
            ec_note "        起法：${SUITE_PREREQ[$s]}"
        fi
    done
    if [ ${#EXCLUDED[@]} -gt 0 ]; then
        ec_note "按本次条件移出计划（不是跑了但跳过）："
        for e in "${EXCLUDED[@]}"; do ec_note "  - $e"; done
    fi
    if [ "$LAYER" = "manual" ] || [ "$LAYER" = "all" ]; then
        ec_note "manual 层当前没有任何自动化套件：Linux 截图后端还没实现，真实桌面验证从后端落地那一步开始登记。"
    fi
    ec_note "人工验证层（manual）不在脚本里代跑：需要真人点确认框、真机多屏与 HDR 现场，"
    ec_note "以及 Portal/PipeWire 的系统授权。这一层的结论只能由做的人写，不会被算成通过。"
}

if [ "$LIST_ONLY" -eq 1 ]; then
    print_plan
    exit 0
fi

# ---------------------------------------------------------------------------
# 构建目录与产物：先确定位置，再决定是否构建
# ---------------------------------------------------------------------------
ec_resolve_cxx "$COMPILER_REQ"
ec_set_build_dir
ec_read_buildable "$EC_BUILD_DIR"

if [ "$NO_BUILD" -eq 0 ]; then
    ec_note "== 构建（委派给 ./build.sh，构建参数只有一份出处）=="
    # 不带 --target：一次把本平台定义的目标全建出来。
    # 点名要 ecapture-gui-probe 这类"本平台还没定义"的目标会让 cmake 先报 unknown target，
    # 于是真正的缺口（没装 wxWidgets）被埋在构建日志里 —— 让下面的前置复核来说，说得准。
    BUILD_CMD=("$EC_ROOT/build.sh" --config "$EC_CONFIG" --compiler "$EC_CXX")
    ec_note "  ${BUILD_CMD[*]}"
    if ! "${BUILD_CMD[@]}"; then
        ec_die_precondition "构建阶段没过，本次一套判据都不会跑（绝不拿旧产物继续）。上面有构建日志。"
    fi
else
    ec_note "--no-build：用现成产物（仍然逐条判存在，缺了按前置不成立报）"
fi

if [ ! -f "$EC_BUILDABLE_FILE" ]; then
    if [ ! -d "$EC_BUILD_DIR" ]; then
        ec_die_precondition "构建目录 $EC_BUILD_DIR 还不存在（$EC_CXX / $EC_CONFIG 这一份没配置过）。
去掉 --no-build 让本脚本先构建，或先跑 ./build.sh --config $EC_CONFIG --compiler $EC_CXX。"
    fi
    ec_die_precondition "构建目录里没有 $EC_BUILDABLE_FILE —— 这一份不是本项目生成的，
先 ./build.sh --clean 重来。"
fi

# ---------------------------------------------------------------------------
# 前置复核：每一套计划内的套件都要能说出"凭什么它现在能跑"。
#
# 缺口按套件记账，不是一把全盘停：缺 wxWidgets 不该顺手把三条离线判据也一起扣住。
# 但每一条缺口都要写进成绩里 —— 有缺口的这套记 NOT RUN(原因)，绝不因为"没法跑"就滑成
# 通过、也不滑成 SKIP。缺口列全（同一套缺两样就写两样），本脚本不安装任何东西。
# ---------------------------------------------------------------------------
declare -A SUITE_GAP
GAP_COUNT=0

ec_add_gap() {
    # $1 = 套件名，$2 = 缺口描述（多行也行）
    if [ -n "${SUITE_GAP[$1]:-}" ]; then
        SUITE_GAP[$1]="${SUITE_GAP[$1]}"$'\n'"      以及：$2"
    else
        SUITE_GAP[$1]="$2"
        GAP_COUNT=$((GAP_COUNT + 1))
    fi
}

for s in "${PLAN[@]}"; do
    layer="${SUITE_LAYER[$s]}"
    if [ "$layer" = "manual" ]; then
        continue
    fi
    if [ "${SUITE_KIND[$s]}" = "script" ]; then
        if [ ! -f "${SUITE_PREREQ[$s]}" ]; then
            ec_add_gap "$s" "判据脚本不存在 ${SUITE_PREREQ[$s]}"
        fi
        # 脚本型套件也可以点名一个构建产物（gui-runtime 就是这种）：产物不在 = 缺前置。
        t="${SUITE_TARGET[$s]}"
        if [ -n "$t" ]; then
            if ! ec_target_is_buildable "$t"; then
                ec_add_gap "$s" "GUI 判据目标没生成 = 本机没找到 wxWidgets。
      装上再跑（本脚本不自动装）：Debian/Ubuntu apt install libwxgtk3.2-dev
      wx-config 不在 PATH 上时：./build.sh --gui --wx-config /全路径/wx-config"
            elif [ ! -x "$EC_BUILD_DIR/$t" ]; then
                ec_add_gap "$s" "目标可构建但产物不在 $EC_BUILD_DIR/$t（去掉 --no-build 先构建）"
            fi
        fi
        if [ "$layer" = "isolated" ] && ! ec_graphic_prefix; then
            ec_add_gap "$s" "这一层要图形会话，当前既没有 DISPLAY/WAYLAND_DISPLAY 也没有 xvfb-run。
      建议（不自动装、不替你启动服务）：apt install xvfb"
        fi
        continue
    fi
    t="${SUITE_TARGET[$s]}"
    if ! ec_target_is_buildable "$t"; then
        if [ "$t" = "ecapture-encode-tests" ]; then
            ec_add_gap "$s" "编码判据目标没生成 = 本机没找到 libpng 或 libjpeg（不是截图后端缺失，也不是 Windows-only）。
      装上再跑（本脚本不自动装）：Debian/Ubuntu apt install libpng-dev libjpeg-dev
      想让缺依赖变成硬错误：./build.sh --codec"
        elif [ "$t" = "ecapture-gui-probe" ]; then
            ec_add_gap "$s" "GUI 探针目标没生成 = 本机没找到 wxWidgets。
      装上再跑（本脚本不自动装）：Debian/Ubuntu apt install libwxgtk3.2-dev
      wx-config 不在 PATH 上时：./build.sh --gui --wx-config /全路径/wx-config"
        else
            ec_add_gap "$s" "目标 $t 在当前平台不可构建（见 $EC_BUILDABLE_FILE 的 windows-only= 那一行）"
        fi
    elif [ ! -x "$EC_BUILD_DIR/$t" ]; then
        ec_add_gap "$s" "目标可构建但产物不在 $EC_BUILD_DIR/$t（去掉 --no-build 先构建）"
    fi
    # 图形会话这一条单独判：缺 wx 的那一套往往同时也缺显示，两句都要写出来，
    # 别让人装完 wx 再撞第二次才知道还差 xvfb。
    if [ "$layer" = "isolated" ] && ! ec_graphic_prefix; then
        ec_add_gap "$s" "这一层要图形会话，当前既没有 DISPLAY/WAYLAND_DISPLAY 也没有 xvfb-run。
      建议（不自动装、不替你启动服务）：apt install xvfb"
    fi
done

if [ "$GAP_COUNT" -gt 0 ]; then
    ec_note "以下 $GAP_COUNT 套计划因缺依赖/缺产物不会启动（记 NOT RUN，不算通过也不算跳过）："
    for s in "${PLAN[@]}"; do
        if [ -n "${SUITE_GAP[$s]:-}" ]; then
            ec_note "  - $s：${SUITE_GAP[$s]}"
        fi
    done
    ec_note "本脚本不安装任何东西；上面的建议命令由你决定要不要执行。"
    ec_note ""
fi

# ---------------------------------------------------------------------------
# 日志目录
# ---------------------------------------------------------------------------
STAMP="$(date +%Y%m%d-%H%M%S)"
if [ -n "$LOG_DIR_IN" ]; then
    LOG_DIR="$LOG_DIR_IN"
else
    LOG_DIR="$EC_BUILD_DIR/test-logs/$STAMP"
fi
mkdir -p -- "$LOG_DIR"

# ---------------------------------------------------------------------------
# 逐套串行执行
#
# 串行是有意的：将来 isolated / 真机层会建窗口、数像素，两套同时跑会互相看到对方的画面。
# 结果分类只信两样：进程退出码，和判据程序自己写下的那句"N 条通过，M 条失败"。
# 读不到汇总行就照实写 "-"，不拿猜测的数字冒充计数。
# ---------------------------------------------------------------------------
declare -A RESULT
PASSED=0; FAILED=0; NOTRUN=0

ec_run_suite() {
    local name="$1" log="$2"
    local -a cmd
    if [ "${SUITE_KIND[$name]}" = "script" ]; then
        cmd=(bash "${SUITE_PREREQ[$name]}")
        # 脚本型套件可以点名一个产物：把构建目录当作第一个参数交给脚本，
        # 脚本自己按平台与配置找那份可执行文件（不在脚本里再抄一遍目录规则）。
        if [ -n "${SUITE_TARGET[$name]}" ]; then
            cmd+=("$EC_BUILD_DIR")
        fi
    else
        cmd=("$EC_BUILD_DIR/${SUITE_TARGET[$name]}")
    fi
    # isolated 层要图形会话：有 xvfb-run 时把那个前缀数组挂在最前面（数组形式来自
    # dev-common.sh，--server-args 的取值自带空格，绝不能再按词拆分），没有就用当前会话 ——
    # 前置复核已经保证这两者至少有一个成立。
    if [ "${SUITE_LAYER[$name]}" = "isolated" ] && [ ${#EC_GFX_PREFIX_ARGS[@]} -gt 0 ]; then
        cmd=("${EC_GFX_PREFIX_ARGS[@]}" "${cmd[@]}")
    fi
    local rc=0
    "${cmd[@]}" >"$log" 2>&1 || rc=$?
    LAST_RC="$rc"
}

LAST_RC=0
ec_note ""
print_plan
ec_note ""
ec_note "== 逐套执行（串行，日志：$LOG_DIR）=="

for s in "${PLAN[@]}"; do
    log="$LOG_DIR/$s.log"
    if [ -n "${SUITE_GAP[$s]:-}" ]; then
        # 缺口在前面已经打印过，这里只记账：一个字节都没启动，所以既不是通过也不是 SKIP。
        first_gap="$(printf '%s' "${SUITE_GAP[$s]}" | head -1)"
        RESULT[$s]="NOT RUN(缺前置)"
        NOTRUN=$((NOTRUN + 1))
        printf '  %-18s %-28s %s\n' "$s" "${RESULT[$s]}" "$first_gap"
        if [ "$STOP_ON_FAIL" -eq 1 ]; then
            ec_note "--stop-on-fail：在这里收，剩余计划没跑（不是通过，也不算未验证）。"
            break
        fi
        continue
    fi
    ec_run_suite "$s" "$log"
    rc="$LAST_RC"
    summary="$(grep -E '[0-9]+ 条通过' "$log" | tail -1 || true)"
    [ -n "$summary" ] || summary="-"
    if [ "$rc" -eq 0 ]; then
        # 退出码 0 但汇总行里写着非零失败条数，仍然判失败：只信退出码会把"判据没跑全"当成通过
        fails="$(printf '%s' "$summary" | sed -n 's/.*[，,][[:space:]]*\([0-9]\+\)[[:space:]]*条失败.*/\1/p')"
        if [ -n "$fails" ] && [ "$fails" != "0" ]; then
            RESULT[$s]="FAIL(汇总写着 $fails 条失败)"; FAILED=$((FAILED + 1))
        else
            RESULT[$s]="PASS"; PASSED=$((PASSED + 1))
        fi
    else
        RESULT[$s]="FAIL(退出码 $rc)"; FAILED=$((FAILED + 1))
    fi
    printf '  %-18s %-28s %s\n' "$s" "${RESULT[$s]}" "$summary"
    if [ "$STOP_ON_FAIL" -eq 1 ] && [ "${RESULT[$s]}" != "PASS" ]; then
        ec_note "--stop-on-fail：在这里收，剩余计划没跑（不是通过，也不算未验证）。"
        break
    fi
done

ec_note ""
ec_note "== 汇总 =="
ec_note "  跑到并通过：$PASSED"
ec_note "  判据没过：$FAILED"
ec_note "  意外没跑成：$NOTRUN"
ec_note "  主动没排：${#EXCLUDED[@]}（原因见上面计划段逐条列出，不合并成一个数字）"
ec_note "  日志目录：$LOG_DIR"
if [ "$LAYER" = "manual" ] || [ "$LAYER" = "all" ]; then
    ec_note "  manual 层：Linux 截图后端未实现，本层暂无可跑套件；真机验证待人工，不算通过也不算失败。"
fi

if [ "$FAILED" -gt 0 ] || [ "$NOTRUN" -gt 0 ]; then
    ec_die_failed "至少一套没过或没跑成（见上面逐条）"
fi
ec_note "测试总跑：计划内该跑的套件全部跑到并通过。"
exit 0
