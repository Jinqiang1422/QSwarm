#!/usr/bin/env bash
# ============================================================================
# launch_swarm_sitl.sh — PX4 集群 SITL 一键拉起脚本（WP1.5）
#
# 用法：
#   ./launch_swarm_sitl.sh [机数=8] [PX4 目录=$HOME/PX4-Autopilot] [仿真后端=gz]
#
# 后端：gz（默认，新 Gazebo，PX4 1.14+）/ gazebo-classic / jmavsim / none
# 前提：已在 PX4 目录构建对应 SITL target（如 make px4_sitl gz_x500）
#
# 多机接入原理（与 QGC 验证过的机制一致）：
#   每实例 -i N 启动，MAV_SYS_ID = N+1；全部实例把 GCS 流发往本机 14550，
#   QGC 按系统 ID 自动区分接入多机。
# ============================================================================
set -euo pipefail

N="${1:-8}"
PX4_DIR="${2:-$HOME/PX4-Autopilot}"
BACKEND="${3:-gz}"
WORK_DIR="${WORK_DIR:-/tmp/px4_swarm_sitl}"
PX4_BIN="$PX4_DIR/build/px4_sitl_default/bin/px4"

if [[ ! -x "$PX4_BIN" ]]; then
    echo "[launch] 未找到 SITL 二进制：$PX4_BIN"
    echo "[launch] 请先在 $PX4_DIR 执行：make px4_sitl gz_x500（或对应后端 target）"
    exit 1
fi

mkdir -p "$WORK_DIR"
PIDS=()

cleanup() {
    echo "[launch] 停止全部 SITL 实例..."
    for pid in "${PIDS[@]:-}"; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[launch] 机数=$N 后端=$BACKEND 工作目录=$WORK_DIR"

for i in $(seq 0 $((N - 1))); do
    INST_DIR="$WORK_DIR/inst_$i"
    mkdir -p "$INST_DIR"
    # 各实例初始位置在 x 方向错开 5 米，避免地面重叠
    POSE_X=$((i * 5))
    case "$BACKEND" in
        gz)
            (cd "$INST_DIR" && \
             PX4_GZ_MODEL_POSE="$POSE_X,0" PX4_GZ_MODEL="${PX4_GZ_MODEL:-x500}" \
             PX4_SYS_AUTOSTART="${PX4_SYS_AUTOSTART:-4001}" \
             "$PX4_BIN" -i "$i" -d "$INST_DIR" > "$INST_DIR/px4.log" 2>&1) &
            ;;
        gazebo-classic)
            (cd "$INST_DIR" && \
             PX4_SIM_MODEL="${PX4_SIM_MODEL:-iris}" \
             PX4_SYS_AUTOSTART="${PX4_SYS_AUTOSTART:-4001}" \
             "$PX4_BIN" -i "$i" -d "$INST_DIR" > "$INST_DIR/px4.log" 2>&1) &
            ;;
        jmavsim|none)
            (cd "$INST_DIR" && \
             PX4_SYS_AUTOSTART="${PX4_SYS_AUTOSTART:-4001}" \
             "$PX4_BIN" -i "$i" -d "$INST_DIR" > "$INST_DIR/px4.log" 2>&1) &
            ;;
        *)
            echo "[launch] 未知后端：$BACKEND"; exit 1 ;;
    esac
    PIDS+=($!)
    echo "[launch] 实例 $i 已启动（PID ${PIDS[-1]}，sysid $((i + 1))）"
    sleep 1     # 错开启动，避免仿真器资源争抢
done

echo "[launch] 全部 $N 个实例已拉起。启动 QGC（定制版）后将自动接入 $N 架机（UDP 14550）"
echo "[launch] Ctrl+C 停止全部实例"
wait
