#!/usr/bin/env bash
# ============================================================================
# auto_official_test.sh — 官方 SITL 环境「共享机队连续试跑」
#
# 挂在 tmux 里,它会自动完成整条链路,无需盯着:
#   1) 循环拉官方镜像(间隔 60s,避免 docker hub 匿名限流;已缓存层续传)
#   2) 起 N 机官方 gazebo-classic SITL(容器内 fresh build ~20min)
#   3) 轮询等 N 机 heartbeat 就绪
#   4) 跑 L1 场景 03/04/05 的共享机队试跑 → 出 SUMMARY.md 报告
#
# 注意：PX4 估计器/预检/故障状态会跨场景保留。此脚本适合环境冒烟和发现问题；严格验收
# 必须为每个飞行场景重启全新机队，再用 regression.py --only 单独执行。
#
# 用法:  tmux new -s sitl;  ./auto_official_test.sh [机数=8]
# 前提:  daemon 代理/mirror 已配(见 SITL测试流程.md §9);pymavlink 已装。
# ============================================================================
set -uo pipefail
SIM_DIR="$(cd "$(dirname "$0")" && pwd)"
IMG="px4io/px4-dev-simulation-focal:latest"
N="${1:-8}"
CONTAINER_NAME="qgc-px4-sitl-auto-$$"

[[ "$N" =~ ^[0-9]+$ && "$N" -ge 1 ]] || { echo "[auto] 机数须为正整数：$N"; exit 2; }
for cmd in docker python3 timeout; do
    command -v "$cmd" >/dev/null || { echo "[auto] 缺少依赖：$cmd"; exit 2; }
done
python3 -c 'from pymavlink import mavutil' >/dev/null 2>&1 \
    || { echo "[auto] 缺少 pymavlink：python3 -m pip install pymavlink"; exit 2; }

echo "==== [auto] 阶段 1/4: 循环拉官方镜像(间隔 60s 防限流) ===="
attempt=0
until docker image inspect "$IMG" >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    echo "[auto] 第 $attempt 次 pull ($(date '+%m-%d %H:%M'))..."
    docker pull "$IMG" && break
    echo "[auto] 未成功,60s 后续传重试(已缓存层不重下)"
    sleep 60
done
echo "[auto] ✓ 镜像就绪"

echo "==== [auto] 阶段 2/4: 起 $N 机官方 SITL(容器内 fresh build ~20min) ===="
QGC_SITL_CONTAINER_NAME="$CONTAINER_NAME" \
    bash "$SIM_DIR/run_official_sitl.sh" "$HOME/PX4-Autopilot" "$N" > /tmp/auto_sitl.log 2>&1 &
SITL_BG=$!
# Invoked by the EXIT trap; ShellCheck cannot follow this indirect call.
# shellcheck disable=SC2329
cleanup() {
    echo "[auto] 清理 SITL..."
    kill "$SITL_BG" 2>/dev/null || true
    docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
    true
}
trap cleanup EXIT

echo "==== [auto] 阶段 3/4: 等 $N 机 heartbeat 就绪(最多 ~40min,含 build) ===="
ready=0
for i in $(seq 1 240); do
    if ! kill -0 "$SITL_BG" 2>/dev/null; then
        wait "$SITL_BG"
        sitl_rc=$?
        echo "[auto] ✗ SITL 进程提前退出（退出码 $sitl_rc）"
        tail -80 /tmp/auto_sitl.log 2>/dev/null || true
        exit 1
    fi
    online=$(timeout 8 python3 - "$N" <<'PY' 2>/dev/null || echo 0
import sys, time
from pymavlink import mavutil
need = int(sys.argv[1])
m = mavutil.mavlink_connection('udpin:0.0.0.0:14550')
t0 = time.time(); seen = set()
while time.time() - t0 < 6:
    msg = m.recv_match(type='HEARTBEAT', blocking=True, timeout=1)
    if msg:
        seen.add(msg.get_srcSystem())
    if len(seen) >= need:
        break
print(len(seen))
PY
)
    if [ "${online:-0}" -ge "$N" ]; then ready=1; echo "[auto] ✓ $N 机就绪"; break; fi
    [ $((i % 6)) -eq 0 ] && echo "[auto] 等待中... ($((i * 10))s, 在线 ${online:-0}/$N; build 日志 tail -f /tmp/auto_sitl.log)"
    sleep 10
done
if [ "$ready" -ne 1 ]; then
    echo "[auto] ✗ 机队未就绪 —— 见 /tmp/auto_sitl.log(build 未完 / 启动问题)"
    exit 1
fi

echo "==== [auto] 阶段 4/4: 共享机队 L1 冒烟（03/04/05） ===="
echo "[auto] ⚠ 01/02/06/07 需要 QGC 验收接口；全量严格验收请用 strict_regression.py。"
python3 "$SIM_DIR/regression.py" --only 03,04,05 --out /tmp/sitl_regression
RC=$?
echo "==== [auto] 完成! 报告: /tmp/sitl_regression/SUMMARY.md (退出码 $RC) ===="
exit $RC
