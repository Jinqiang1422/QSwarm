#!/usr/bin/env bash
# ============================================================================
# run_official_sitl.sh — 在 PX4 官方 SITL docker 镜像里跑 gazebo-classic iris
#
# 为什么用官方镜像：本机 PX4 是用 miniconda 的 cmake 编的、环境含 ROS noetic 污染，
# 且系统装了 new Gazebo Sim 7.9 与 gazebo-classic 命令冲突 —— 这些非标准因素导致
# 本机 gz/jmavsim 两后端 arm 都被 PX4 preflight 拒(见 SITL测试流程.md §8)。官方
# px4io/px4-dev-simulation-focal 镜像的工具链/依赖/参数是 PX4 CI 验证过的，arm/起飞正常。
#
# 用法：
#   ./run_official_sitl.sh [PX4源码=$HOME/PX4-Autopilot] [机型=iris]
# 另开终端跑场景(mavlink 经 --network host 直达宿主 14550)：
#   python3 run_scenario.py scenarios/01_area_search.json --report /tmp/r01.json
#
# 建议在 tmux 里跑：镜像拉取(GB级) + 源码同步(~2GB) + 容器内 fresh build(~15-20min) 较长。
# ============================================================================
set -euo pipefail

PX4_SRC="${1:-$HOME/PX4-Autopilot}"
MODEL="${2:-iris}"
IMG="px4io/px4-dev-simulation-focal:latest"
COPY="/tmp/PX4-official-src"

[[ -d "$PX4_SRC" ]] || { echo "[official] 未找到 PX4 源码：$PX4_SRC"; exit 1; }

echo "[official] 0) 确保官方镜像在本地(不在则拉取，走 docker daemon 代理)..."
docker image inspect "$IMG" >/dev/null 2>&1 || docker pull "$IMG"

echo "[official] 1) 同步源码副本到 $COPY(排除 build/，避免本机污染的产物冲突)..."
rsync -a --delete --exclude '/build/' "$PX4_SRC/" "$COPY/"

echo "[official] 2) 官方镜像内 fresh build + 起 gazebo-classic_${MODEL}(HEADLESS)..."
echo "[official]    mavlink → 宿主 14550;另开终端跑 run_scenario.py 连 14550"
echo "[official]    Ctrl+C 停止"
# --network host：容器 mavlink UDP 直达宿主 14550(SITL headless 无需 X11，无串号风险)
# HEADLESS=1：仅 gzserver 不开 gui
docker run --rm --network host -e HEADLESS=1 \
    -v "$COPY":/px4 -w /px4 \
    "$IMG" bash -lc "git config --global --add safe.directory /px4 && make px4_sitl gazebo-classic_${MODEL}"
