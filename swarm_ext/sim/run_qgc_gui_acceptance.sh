#!/usr/bin/env bash
# Run a test-build QGC with its normal QML scene on an isolated Xvfb display.
set -euo pipefail

SIM_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SIM_DIR/../.." && pwd)"
BUILD_ROOT="$(realpath -m "${1:-$REPO_ROOT/build}")"
QGC_HOME_DIR_INPUT="${2:-/tmp/qgc-swarm-gui-home}"
QGC_IMAGE="${QGC_ACCEPTANCE_IMAGE:-qgc-ubuntu-docker:latest}"
QGC_CONTAINER="${QGC_ACCEPTANCE_CONTAINER_NAME:-qgc-swarm-gui-$$}"
QGC_PORT="${QGC_SWARM_ACCEPTANCE_PORT:-15610}"
QGC_TOKEN_FILE_INPUT="${QGC_SWARM_ACCEPTANCE_TOKEN_FILE:-}"
QGC_BINARY="${QGC_ACCEPTANCE_BINARY:-Debug/QGroundControl}"
QGC_CONTAINER_TOKEN_FILE="/run/secrets/qgc-swarm-acceptance-token"
QGC_DISPLAY_NUMBER="${QGC_ACCEPTANCE_DISPLAY_NUMBER:-$((100 + $$ % 400))}"

[[ -n "$QGC_TOKEN_FILE_INPUT" && -f "$QGC_TOKEN_FILE_INPUT" && ! -L "$QGC_TOKEN_FILE_INPUT" ]] || {
    echo "[qgc-gui] QGC_SWARM_ACCEPTANCE_TOKEN_FILE 必须指向普通文件" >&2
    exit 2
}
QGC_TOKEN_FILE="$(realpath "$QGC_TOKEN_FILE_INPUT")"
QGC_TOKEN_MODE="$(stat -c '%a' "$QGC_TOKEN_FILE")"
(( (8#$QGC_TOKEN_MODE & 0077) == 0 && (8#$QGC_TOKEN_MODE & 0400) != 0 )) || {
    echo "[qgc-gui] 令牌文件必须仅允许所有者访问" >&2
    exit 2
}
IFS= read -r QGC_TOKEN_FIRST_LINE < "$QGC_TOKEN_FILE" || true
[[ ${#QGC_TOKEN_FIRST_LINE} -ge 16 && ${#QGC_TOKEN_FIRST_LINE} -le 512 ]] || {
    echo "[qgc-gui] 令牌必须包含 16 到 512 个字符" >&2
    exit 2
}
[[ "$(wc -l < "$QGC_TOKEN_FILE")" -le 1 ]] || {
    echo "[qgc-gui] 令牌文件只能包含一行" >&2
    exit 2
}
unset QGC_TOKEN_FIRST_LINE
[[ "$QGC_PORT" =~ ^[0-9]+$ && "$QGC_PORT" -ge 1024 && "$QGC_PORT" -le 65535 ]] || {
    echo "[qgc-gui] 非法端口：$QGC_PORT" >&2
    exit 2
}
[[ "$QGC_DISPLAY_NUMBER" =~ ^[0-9]+$ && "$QGC_DISPLAY_NUMBER" -ge 1 && "$QGC_DISPLAY_NUMBER" -le 999 ]] || {
    echo "[qgc-gui] 非法 X display：$QGC_DISPLAY_NUMBER" >&2
    exit 2
}
[[ "$QGC_CONTAINER" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]+$ ]] || {
    echo "[qgc-gui] 非法容器名：$QGC_CONTAINER" >&2
    exit 2
}
[[ -x "$BUILD_ROOT/$QGC_BINARY" ]] || {
    echo "[qgc-gui] 未找到可执行文件：$BUILD_ROOT/$QGC_BINARY" >&2
    exit 2
}
command -v Xvfb >/dev/null || {
    echo "[qgc-gui] 未找到 Xvfb" >&2
    exit 2
}
if docker container inspect "$QGC_CONTAINER" >/dev/null 2>&1; then
    echo "[qgc-gui] 容器名已被占用：$QGC_CONTAINER" >&2
    exit 2
fi

mkdir -p "$QGC_HOME_DIR_INPUT"
QGC_HOME_DIR="$(realpath "$QGC_HOME_DIR_INPUT")"
QGC_UID="$(id -u)"
QGC_GID="$(id -g)"
[[ "$QGC_UID" -ne 0 ]] || {
    echo "[qgc-gui] 请以非 root 用户运行，QGC 不允许 root 启动" >&2
    exit 2
}

QGC_DISPLAY=":$QGC_DISPLAY_NUMBER"
QGC_X_SOCKET="/tmp/.X11-unix/X$QGC_DISPLAY_NUMBER"
[[ ! -e "$QGC_X_SOCKET" ]] || {
    echo "[qgc-gui] X display 已被占用：$QGC_DISPLAY" >&2
    exit 2
}

XVFB_PID=""
cleanup() {
    trap - EXIT INT TERM
    if [[ -n "$XVFB_PID" ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

Xvfb "$QGC_DISPLAY" -screen 0 1680x1050x24 -ac +extension GLX +render \
    >"$QGC_HOME_DIR/xvfb.log" 2>&1 &
XVFB_PID=$!
for _ in {1..50}; do
    [[ -S "$QGC_X_SOCKET" ]] && break
    kill -0 "$XVFB_PID" 2>/dev/null || {
        echo "[qgc-gui] Xvfb 提前退出" >&2
        exit 1
    }
    sleep 0.1
done
[[ -S "$QGC_X_SOCKET" ]] || {
    echo "[qgc-gui] Xvfb 未创建 display socket：$QGC_X_SOCKET" >&2
    exit 1
}

echo "[qgc-gui] 启动正常 GUI QGC：127.0.0.1:$QGC_PORT，display=$QGC_DISPLAY"
docker run --rm --name "$QGC_CONTAINER" --network host \
    --user "$QGC_UID:$QGC_GID" \
    --entrypoint "/project/build/$QGC_BINARY" \
    -e DISPLAY="$QGC_DISPLAY" \
    -e HOME=/qgc-home \
    -e QT_QPA_PLATFORM=xcb \
    -e QT_OPENGL=software \
    -e LIBGL_ALWAYS_SOFTWARE=1 \
    -e GALLIUM_DRIVER=llvmpipe \
    -e QGC_SWARM_ACCEPTANCE_PORT="$QGC_PORT" \
    -e QGC_SWARM_ACCEPTANCE_TOKEN_FILE="$QGC_CONTAINER_TOKEN_FILE" \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v "$REPO_ROOT:/project/source:ro" \
    -v "$BUILD_ROOT:/project/build:ro" \
    -v "$QGC_HOME_DIR:/qgc-home" \
    -v "$QGC_TOKEN_FILE:$QGC_CONTAINER_TOKEN_FILE:ro" \
    "$QGC_IMAGE" --allow-multiple
