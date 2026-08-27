#!/usr/bin/env bash
# ============================================================================
# run_official_sitl.sh — 在 PX4 官方 SITL docker 镜像里跑 N 机 gazebo-classic iris
#
# 为什么用官方镜像：本机 PX4 是用 miniconda 的 cmake 编的、环境含 ROS noetic 污染，
# 且系统装了 new Gazebo Sim 7.9 与 gazebo-classic 的 `gz` 命令冲突 —— 这些非标准因素
# 导致本机 gz/jmavsim 两后端 arm 都被 PX4 preflight 拒(见 SITL测试流程.md §8)。官方
# px4io/px4-dev-simulation-focal 镜像的工具链/依赖/参数是 PX4 CI 验证过的，arm/起飞正常。
#
# 用法：
#   ./run_official_sitl.sh [PX4源码=$HOME/PX4-Autopilot] [机数=1] [机型=iris]
# 例：
#   ./run_official_sitl.sh                       # 单机
#   ./run_official_sitl.sh ~/PX4-Autopilot 4     # 4 机
# 另开终端跑场景(mavlink 经 --network host 聚合宿主 14550，按 sysid 区分)：
#   python3 run_scenario.py scenarios/05_emergency_stop.json --report /tmp/r05.json
#
# 建议 tmux 里跑：镜像拉取(GB级) + 源码同步(~2GB) + 容器内 fresh build(~15-20min) 较长。
# ============================================================================
set -euo pipefail

PX4_SRC="${1:-$HOME/PX4-Autopilot}"
N="${2:-1}"
MODEL="${3:-iris}"
IMG="px4io/px4-dev-simulation-focal:latest"
COPY="/tmp/PX4-official-src"
CONTAINER_NAME="${QGC_SITL_CONTAINER_NAME:-qgc-px4-sitl-$$}"

[[ -d "$PX4_SRC" ]] || { echo "[official] 未找到 PX4 源码：$PX4_SRC"; exit 1; }
[[ "$N" =~ ^[0-9]+$ ]] || { echo "[official] 机数须为整数：$N"; exit 1; }
[[ "$N" -ge 1 ]] || { echo "[official] 机数须大于 0：$N"; exit 1; }
[[ "$CONTAINER_NAME" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]+$ ]] \
    || { echo "[official] 非法容器名：$CONTAINER_NAME"; exit 1; }
command -v docker >/dev/null || { echo "[official] 缺少 docker"; exit 1; }
command -v rsync >/dev/null || { echo "[official] 缺少 rsync"; exit 1; }
if docker container inspect "$CONTAINER_NAME" >/dev/null 2>&1; then
    echo "[official] 容器名已被占用：$CONTAINER_NAME（请先处理该容器或设置 QGC_SITL_CONTAINER_NAME）"
    exit 1
fi

echo "[official] 0) 确保官方镜像在本地(不在则拉取，走 docker daemon 代理)..."
docker image inspect "$IMG" >/dev/null 2>&1 || docker pull "$IMG"

echo "[official] 1) 同步源码副本到 $COPY(排除 build/，避免本机污染的产物冲突)..."
rsync -a --delete --exclude '/build/' "$PX4_SRC/" "$COPY/"

if [[ "$N" -le 1 ]]; then
    # 单机：make target 的 sitl_run.sh 会按 HEADLESS 跳过 gzclient，无需 Xvfb
    RUN="make px4_sitl gazebo-classic_${MODEL}"
    echo "[official] 2) 单机 fresh build + 起 gazebo-classic_${MODEL}(HEADLESS)..."
else
    # 多机：sitl_multiple_run.sh 末尾硬起 gzclient(无 HEADLESS 判断)，容器内起 Xvfb 兜住；
    #       各实例 GCS mavlink 统一发 14550(sysid=1+i)，run_scenario 单入口全收。
    RUN="make px4_sitl_default gazebo-classic \
         && export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
         && (Xvfb :99 -screen 0 1280x800x24 >/tmp/xvfb_multi.log 2>&1 &) \
         && sleep 2 && export DISPLAY=:99 \
         && ./Tools/simulation/gazebo-classic/sitl_multiple_run.sh -n ${N} -m ${MODEL}"
    echo "[official] 2) ${N} 机 fresh build + sitl_multiple_run(Xvfb 兜 gzclient)..."
fi

echo "[official]    mavlink → 宿主 14550(所有实例聚合，按 sysid 区分)；另开终端跑 run_scenario.py"
echo "[official]    Ctrl+C 停止"
# --network host：容器 mavlink UDP 直达宿主(SITL headless 无需 X11，无串号风险)
docker run --rm --name "$CONTAINER_NAME" --network host \
    -v "$COPY":/px4 -w /px4 \
    "$IMG" bash -lc "git config --global --add safe.directory /px4 && ${RUN}"
