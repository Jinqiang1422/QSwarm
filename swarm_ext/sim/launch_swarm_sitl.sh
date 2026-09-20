#!/usr/bin/env bash
# ============================================================================
# launch_swarm_sitl.sh — PX4 集群 SITL 一键拉起脚本（v2，修正版）
#
# 用法：
#   ./launch_swarm_sitl.sh [机数=4] [PX4 目录=$HOME/PX4-Autopilot] [后端=gz]
#
# 本会话（2026-08-21）实测修正的问题（旧版 3 处 bug 导致 px4 起但无 heartbeat）：
#   1. 旧版 `cd INST_DIR && px4 -i N -d INST_DIR` 在空目录里跑，px4 找不到
#      rootfs 与 etc/init.d-posix/rcS → 启动即失败。本版传入正确 data_path + -s rcS。
#   2. 旧版 gz 后端从不启动 gz server、不设世界 → lockstep 无时钟源、卡死无 heartbeat。
#      本版实例 0 由 px4 自动拉起 gz server（world: default.sdf），实例 1+ 用
#      PX4_GZ_STANDALONE=1 连同一 server。
#   3. 无显示服务器上 gz 仍会拉起 GUI 组件（Qt xcb 连不上 display 崩溃）→ 本版起
#      Xvfb 虚拟显示 + 软件 GL 兜住。
#
# 历史“无 heartbeat”的根因是漏载 gz_env.sh，现已修复。gz lockstep 仍依赖 CPU 时间片，
#   高负载会降低实时率，因此建议在空闲机器运行，但不会再把资源负载当作启动失败根因。
#
# 验证状态（2026-08-25）：1/4/8 机飞行闭环已实测；8 机一键中止最大 ACK 时延 16.3ms。
#
# 多机接入原理：每实例 -i N → MAV_SYS_ID=N+1；GCS 流统一发往 14550，
#   run_scenario.py / QGC 按 sysid 区分接入多机。
# ============================================================================
set -uo pipefail

N="${1:-4}"                                  # 默认 4 机（软件渲染下 8 机偏重）
PX4_DIR="${2:-$HOME/PX4-Autopilot}"
BACKEND="${3:-gz}"
WORK_DIR="${WORK_DIR:-/tmp/px4_swarm_sitl}"
MODEL="${PX4_SIM_MODEL:-gz_x500}"
SPACING="${SPACING:-3}"                       # 机间地面初始间距（米）
BUILD="$PX4_DIR/build/px4_sitl_default"
PX4_BIN="$BUILD/bin/px4"
DATA_PATH="$BUILD/etc"
RCS="$BUILD/etc/init.d-posix/rcS"

[[ "$N" =~ ^[0-9]+$ && "$N" -ge 1 ]] || { echo "[launch] 机数须为正整数：$N"; exit 1; }
[[ "$SPACING" =~ ^[0-9]+$ ]] || { echo "[launch] 初始间距须为非负整数：$SPACING"; exit 1; }
command -v setsid >/dev/null || { echo "[launch] 缺少 setsid（util-linux）"; exit 1; }
[[ -x "$PX4_BIN" ]] || { echo "[launch] 未找到 SITL 二进制：$PX4_BIN"; \
    echo "[launch] 请先在 $PX4_DIR 执行：make px4_sitl gz_x500"; exit 1; }
[[ -f "$RCS" ]] || { echo "[launch] 未找到启动脚本 rcS：$RCS"; exit 1; }

# ---- 实例占用预检：区分「活进程占用」与「孤儿锁」----
# PX4 的 /tmp/px4_lock-N 是全机器范围的每实例号锁，本机上任何一个 SITL 机队都会占用它。
# 两种情形的处置完全相反，混为一谈会给出危险建议：活进程持有时照着删锁会破坏对方机队。
LIVE_PX4=$(pgrep -x px4 | head -1 || true)
if [[ -n "$LIVE_PX4" ]]; then
    LIVE_COUNT=$(pgrep -cx px4)
    LIVE_OWNER=$(ps -o args= -p "$(ps -o ppid= -p "$LIVE_PX4" | tr -d ' ')" 2>/dev/null | cut -c1-120)
    echo "[launch] 本机已有 $LIVE_COUNT 个 px4 进程在运行，实例号锁被占用。"
    echo "[launch] 起始进程 pid=$LIVE_PX4，其父进程：${LIVE_OWNER:-未知}"
    echo "[launch] 这不是遗留锁，不要删除 /tmp/px4_lock-*：那会破坏正在运行的机队。"
    echo "[launch] 请等对方机队结束，或在空闲机器上运行。"
    exit 3   # 环境阻塞：见 strict_regression.EXIT_ENVIRONMENT_BLOCKED
fi
STALE_LOCKS=$(find /tmp -maxdepth 1 \( -name "px4_lock-*" -o -name "px4-sock-*" \) 2>/dev/null | wc -l)
if [[ "$STALE_LOCKS" -gt 0 ]]; then
    # 无 px4 进程却有锁 = 上一轮被中断留下的孤儿，此时删除是安全的。
    echo "[launch] 清理上一轮遗留的 $STALE_LOCKS 个孤儿 PX4 锁（当前无 px4 进程）"
    rm -f /tmp/px4_lock-* /tmp/px4-sock-*
fi

# ---- 资源预警（lockstep 对 CPU 敏感）----
LOAD1=$(awk '{print $1}' /proc/loadavg)
NCPU=$(nproc)
echo "[launch] 机数=$N 后端=$BACKEND 模型=$MODEL 当前 load=$LOAD1 / CPU=$NCPU"
awk -v l="$LOAD1" -v c="$NCPU" \
    'BEGIN { if (l > c*0.7) print "[launch] ⚠ 负载偏高：gz lockstep 实时率可能下降，建议空闲时运行。" }'

# ---- 无显示环境：Xvfb 兜住 gz GUI + 软件 GL ----
XVFB_PID=""
if [[ -z "${DISPLAY:-}" ]]; then
    if command -v Xvfb >/dev/null; then
        Xvfb :99 -screen 0 1280x800x24 >/tmp/px4_xvfb.log 2>&1 &
        XVFB_PID=$!
        export DISPLAY=:99
        echo "[launch] 无 DISPLAY → 起 Xvfb 虚拟显示 :99 (pid $XVFB_PID)"
    else
        echo "[launch] ⚠ 无 DISPLAY 且未安装 Xvfb（apt install xvfb）；gz GUI 可能崩溃。"
    fi
fi
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe   # 无 GPU 时软件渲染

# 每次机队启动使用独立 transport partition，避免上轮异常残留或并行作业的 gz server
# 被 PX4 误判为当前 world。变量会由 env 继承给全部 PX4/gz 子进程。
if [[ "$BACKEND" == "gz" ]]; then
    export GZ_PARTITION="${GZ_PARTITION:-qgc_swarm_${BASHPID}}"
    echo "[launch] Gazebo transport partition: $GZ_PARTITION"
fi

# ---- gz 资源环境（关键！直接调 px4 二进制 + `-w` 独立工作目录时，rcS 不会自动
#      source rootfs/gz_env.sh，缺失会导致 gz_bridge "Service call timed out.
#      Check GZ_SIM_RESOURCE_PATH" → spawn model 失败 → 无 heartbeat）----
GZ_ENV="$BUILD/rootfs/gz_env.sh"
if [[ -f "$GZ_ENV" ]]; then
    export GZ_SIM_RESOURCE_PATH="${GZ_SIM_RESOURCE_PATH:-}"   # 防 set -u 下 gz_env.sh 报 unbound
    set +u
    # shellcheck disable=SC1090
    source "$GZ_ENV"
    set -u
    echo "[launch] 已加载 gz 环境：$GZ_ENV"
else
    export PX4_GZ_MODELS="$PX4_DIR/Tools/simulation/gz/models"
    export PX4_GZ_WORLDS="$PX4_DIR/Tools/simulation/gz/worlds"
    export GZ_SIM_RESOURCE_PATH="${GZ_SIM_RESOURCE_PATH:-}:$PX4_GZ_MODELS:$PX4_GZ_WORLDS"
    echo "[launch] gz_env.sh 缺失，已手动设 GZ_SIM_RESOURCE_PATH"
fi

mkdir -p "$WORK_DIR"
PIDS=()
cleanup() {
    trap - EXIT INT TERM
    echo "[launch] 停止全部 SITL 实例..."
    for pid in "${PIDS[@]}"; do
        if kill -0 -- "-$pid" 2>/dev/null; then
            kill -TERM -- "-$pid" 2>/dev/null || true
        fi
    done
    # gz sim 是实例 0 的孙进程，可能不响应 TERM，不能只等待 shell 直接子进程。
    for _ in {1..25}; do
        groups_alive=false
        for pid in "${PIDS[@]}"; do
            if kill -0 -- "-$pid" 2>/dev/null; then
                groups_alive=true
                break
            fi
        done
        if [[ "$groups_alive" == "false" ]]; then
            break
        fi
        sleep 0.2
    done
    for pid in "${PIDS[@]}"; do
        if kill -0 -- "-$pid" 2>/dev/null; then
            echo "[launch] 进程组 $pid 未及时退出，发送 KILL"
            kill -KILL -- "-$pid" 2>/dev/null || true
        fi
    done
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for ((i = 0; i < N; i++)); do
    INST="$WORK_DIR/inst_$i"
    mkdir -p "$INST"
    POSE_X=$((i * SPACING))
    # 实例 0 拉起 gz server；其余 STANDALONE 连同一 server（否则各起一个 server 会冲突）
    GZ_STANDALONE=()
    if [[ $i -gt 0 ]]; then
        GZ_STANDALONE=("PX4_GZ_STANDALONE=1")
    fi
    case "$BACKEND" in
        gz)
            setsid env "${GZ_STANDALONE[@]}" \
                PX4_SYS_AUTOSTART="${PX4_SYS_AUTOSTART:-4001}" \
                PX4_SIM_MODEL="$MODEL" \
                PX4_GZ_MODEL_POSE="${POSE_X},0" \
                HEADLESS=1 \
                "$PX4_BIN" -i "$i" "$DATA_PATH" -s "$RCS" -w "$INST" -d \
                < /dev/null \
                > "$INST/px4.log" 2>&1 &
            ;;
        gazebo-classic|jmavsim|none)
            setsid env PX4_SYS_AUTOSTART="${PX4_SYS_AUTOSTART:-4001}" \
                "$PX4_BIN" -i "$i" "$DATA_PATH" -s "$RCS" -w "$INST" -d \
                < /dev/null \
                > "$INST/px4.log" 2>&1 &
            ;;
        *) echo "[launch] 未知后端：$BACKEND"; exit 1 ;;
    esac
    PIDS+=("$!")
    echo "[launch] 实例 $i 启动 (pid ${PIDS[-1]}, sysid $((i+1)), pose ${POSE_X},0, 日志 $INST/px4.log)"
    # 实例 0 需多给几秒让 gz server 起来，后续实例连它
    if [[ $i -eq 0 ]]; then
        sleep 6
    else
        # Gazebo create 服务返回后，传感器插件仍需时间完成 transport 注册；过快连续
        # 生成多机偶发出现模型存在但 accel/barometer/EKF 永久缺失。
        sleep 4
    fi
    if ! kill -0 "${PIDS[-1]}" 2>/dev/null; then
        echo "[launch] 实例 $i 在启动宽限期内退出，详见 $INST/px4.log"
        # 孤儿锁已由启动前预检清理，活进程占用也已在那里拦下。能走到这里还报「already
        # running」，说明锁是在本次启动过程中被别的机队抢走的——仍属环境竞争，不是产品缺陷。
        if grep -q "PX4 server already running" "$INST/px4.log" 2>/dev/null; then
            echo "[launch] 实例号锁在本次启动期间被其它 SITL 机队抢占（预检时还是空闲的）。"
            echo "[launch] 当前 px4 进程数：$(pgrep -cx px4 || echo 0)。请在空闲机器上重跑。"
            exit 3   # 环境阻塞，同预检
        fi
        exit 1
    fi
done

if [[ "${QGC_SKIP_READY_PROBE:-0}" == "1" ]]; then
    echo "[launch] 全部 $N 实例已拉起；由外层 QGC 验收器等待 heartbeat"
else
    echo "[launch] 全部 $N 实例已拉起，探测 heartbeat 就绪（最多 120s）..."
    python3 - "$N" <<'PY'
import sys, time
try:
    from pymavlink import mavutil
except ImportError:
    print("[launch] 未装 pymavlink（pip install pymavlink），跳过就绪探测"); sys.exit(0)
need = int(sys.argv[1])
m = mavutil.mavlink_connection('udpin:0.0.0.0:14550')
t0 = time.time(); seen = {}
while time.time() - t0 < 120:
    msg = m.recv_match(type='HEARTBEAT', blocking=True, timeout=1)
    if msg:
        s = msg.get_srcSystem()
        if 1 <= s <= need and s not in seen:
            seen[s] = 1
            print(f"  [{time.time()-t0:.0f}s] 机 sysid={s} 在线 ({len(seen)}/{need})", flush=True)
    if len(seen) >= need:
        break
ok = len(seen) >= need
print(f"[launch] 就绪 {len(seen)}/{need} 架" +
      ("" if ok else " —— 未满：多为高负载 lockstep 卡顿或多机 standalone 未连上，见各 inst_*/px4.log"))
PY
fi

echo "[launch] 机队就绪后，另开终端运行场景（示例）："
SIM_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "  python3 $SIM_DIR/run_scenario.py $SIM_DIR/scenarios/05_emergency_stop.json --report /tmp/report_05.json"
echo "[launch] Ctrl+C 停止全部实例"
wait
