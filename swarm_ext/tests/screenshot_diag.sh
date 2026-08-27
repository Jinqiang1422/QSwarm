#!/bin/bash
# 多路诊断：定位 headless 下 QGC 黑屏根因（GL present / 软件 backend / 窗口激活）
set -u
SD=/project/swarm_ext/docs/shots
mkdir -p "$SD" /tmp/qgchome
chown -R ubuntu:ubuntu "$SD" /tmp/qgchome

echo "[0] 安装工具(每次 --rm 新容器都需装)"
apt-get update -qq >/tmp/apt-up.log 2>&1 || true
apt-get install -y -qq scrot x11-utils fonts-dejavu-core fonts-noto-cjk \
    libgl1-mesa-dri mesa-utils libglx-mesa0 matchbox-window-manager wmctrl \
    >/tmp/apt-i.log 2>&1 && echo "  工具就绪" || echo "  装失败: $(grep -ci 'unable' /tmp/apt-i.log)"

Xvfb :99 -screen 0 1680x1050x24 -ac +extension GLX +render >/tmp/xvfb.log 2>&1 &
sleep 4
DISPLAY=:99 matchbox-window-manager -use_titlebar no >/tmp/wm.log 2>&1 &
sleep 2
chmod 1777 /tmp/.X11-unix 2>/dev/null || true

echo "=== 测试1: glxgears（验证软件 GL 能否 present 到 Xvfb） ==="
# shellcheck disable=SC2016
runuser -u ubuntu -- bash -c '
  export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
  glxgears >/tmp/gears.log 2>&1 & GP=$!
  sleep 6
  scrot /project/swarm_ext/docs/shots/diag-glxgears.png 2>/dev/null && echo "  glxgears 截图完成"
  kill $GP 2>/dev/null'

echo "=== 测试2: QGC software backend（QPainter，不依赖 GLX present） ==="
# shellcheck disable=SC2016
runuser -u ubuntu -- bash -c '
  export DISPLAY=:99 QT_QUICK_BACKEND=software QT_QPA_PLATFORM=xcb HOME=/tmp/qgchome
  cd /project/build/Debug; ./QGroundControl >/tmp/qgc-sw.log 2>&1 & QP=$!
  sleep 40
  wmctrl -a QGroundControl 2>/dev/null; sleep 2
  scrot /project/swarm_ext/docs/shots/diag-qgc-sw.png 2>/dev/null && echo "  QGC-sw 截图完成"
  kill $QP 2>/dev/null'
sleep 2

echo "=== 测试3: QGC 软件 GL + 窗口激活 + 55s ==="
# shellcheck disable=SC2016
runuser -u ubuntu -- bash -c '
  export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe QT_QPA_PLATFORM=xcb HOME=/tmp/qgchome MESA_GL_VERSION_OVERRIDE=4.5
  cd /project/build/Debug; ./QGroundControl >/tmp/qgc-gl.log 2>&1 & QP=$!
  sleep 55
  wmctrl -a QGroundControl 2>/dev/null; sleep 3
  scrot /project/swarm_ext/docs/shots/diag-qgc-gl.png 2>/dev/null && echo "  QGC-gl 截图完成"
  kill $QP 2>/dev/null'

echo "=== 截图文件大小（明显大于 ~5KB = 有内容） ==="
ls -la "$SD"/diag-*.png 2>/dev/null
