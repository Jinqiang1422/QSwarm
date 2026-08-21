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
# ⚠ 关键前提（历史"无 heartbeat"的真正根因）：gz 用 lockstep 同步，仿真时钟依赖
#   CPU 时间片。若本机正跑重负载任务（如 YOLO 训练，load 接近/超过核数），lockstep
#   拿不到时间片，heartbeat 会严重延迟甚至发不出。**务必在空闲（低负载）机器运行。**
#
# 验证状态：单机启动链路（gz_bridge 连通 world）本会话已验证；多机 STANDALONE 模式
#   为 PX4 v1.15 惯例，建议首次在空闲环境按下方 heartbeat 探测确认后再跑场景。
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

[[ -x "$PX4_BIN" ]] || { echo "[launch] 未找到 SITL 二进制：$PX4_BIN"; \
    echo "[launch] 请先在 $PX4_DIR 执行：make px4_sitl gz_x500"; exit 1; }
[[ -f "$RCS" ]] || { echo "[launch] 未找到启动脚本 rcS：$RCS"; exit 1; }

# ---- 资源预警（lockstep 对 CPU 敏感）----
LOAD1=$(awk '{print $1}' /proc/loadavg)
NCPU=$(nproc)
echo "[launch] 机数=$N 后端=$BACKEND 模型=$MODEL 当前 load=$LOAD1 / CPU=$NCPU"
awk -v l="$LOAD1" -v c="$NCPU" 'BEGIN{ if (l > c*0.7) \
    print "[launch] ⚠ 负载偏高：gz lockstep 可能拿不到足够 CPU，heartbeat 会延迟/超时。建议空闲时运行。" }'

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

mkdir -p "$WORK_DIR"
PIDS=()
cleanup() {
    echo "[launch] 停止全部 SITL 实例..."
    for pid in "${PIDS[@]:-}"; do [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true; done
    pkill -f "$BUILD/bin/px4" 2>/dev/null || true
    pkill -f 'gz sim' 2>/dev/null || true
    [[ -n "$XVFB_PID" ]] && kill "$XVFB_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for i in $(seq 0 $((N-1))); do
    INST="$WORK_DIR/inst_$i"
    mkdir -p "$INST"
    POSE_X=$((i * SPACING))
    # 实例 0 拉起 gz server；其余 STANDALONE 连同一 server（否则各起一个 server 会冲突）
    GZ_STANDALONE=""
    [[ $i -gt 0 ]] && GZ_STANDALONE="PX4_GZ_STANDALONE=1"
    case "$BACKEND" in
        gz)
            env $GZ_STANDALONE \
                PX4_SYS_AUTOSTART="${PX4_SYS_AUTOSTART:-4001}" \
                PX4_SIM_MODEL="$MODEL" \
                PX4_GZ_MODEL_POSE="${POSE_X},0" \
                HEADLESS=1 \
                "$PX4_BIN" -i "$i" "$DATA_PATH" -s "$RCS" -w "$INST" -d \
                > "$INST/px4.log" 2>&1 &
            ;;
        gazebo-classic|jmavsim|none)
            env PX4_SYS_AUTOSTART="${PX4_SYS_AUTOSTART:-4001}" \
                "$PX4_BIN" -i "$i" "$DATA_PATH" -s "$RCS" -w "$INST" -d \
                > "$INST/px4.log" 2>&1 &
            ;;
        *) echo "[launch] 未知后端：$BACKEND"; exit 1 ;;
    esac
    PIDS+=($!)
    echo "[launch] 实例 $i 启动 (pid ${PIDS[-1]}, sysid $((i+1)), pose ${POSE_X},0, 日志 $INST/px4.log)"
    # 实例 0 需多给几秒让 gz server 起来，后续实例连它
    [[ $i -eq 0 ]] && sleep 6 || sleep 2
done

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

echo "[launch] 机队就绪后，另开终端运行场景（示例）："
SIM_DIR="$(cd "$(dirname "$0")" && pwd)"
echo "  python3 $SIM_DIR/run_scenario.py $SIM_DIR/scenarios/05_emergency_stop.json --report /tmp/report_05.json"
echo "[launch] Ctrl+C 停止全部实例"
wait
