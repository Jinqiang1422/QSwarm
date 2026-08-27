#!/bin/bash
# headless 环境下截取 QGC 界面：Xvfb 虚拟显示 + Mesa llvmpipe 软件 OpenGL（纯 CPU GL，
# 绕过多 GPU headless 服务器缺失的硬件 GL）。容器默认 root 而 QGC 拒绝 root，用 runuser 切 ubuntu。
# 用法（qgc-ubuntu-docker 容器内，root）：bash screenshot_qgc.sh [场景标签] [等待秒数]
set -u
LABEL="${1:-main}"
WAIT="${2:-45}"
SHOTDIR=/project/swarm_ext/docs/shots

[[ "$LABEL" =~ ^[a-zA-Z0-9_.-]+$ ]] || { echo "非法场景标签：$LABEL"; exit 2; }
[[ "$WAIT" =~ ^[0-9]+$ && "$WAIT" -ge 1 ]] || { echo "等待秒数须为正整数：$WAIT"; exit 2; }

echo "[1/5] 安装截图工具 + Mesa 软件 OpenGL(llvmpipe)"
apt-get update -qq >/tmp/apt-up.log 2>&1 || true
apt-get install -y -qq scrot imagemagick x11-apps x11-utils fonts-dejavu-core fonts-noto-cjk \
    libgl1-mesa-dri mesa-utils libglx-mesa0 matchbox-window-manager wmctrl xdotool >/tmp/apt-install.log 2>&1 && echo "  OK" \
    || echo "  部分缺失：$(grep -c -i 'unable to locate' /tmp/apt-install.log)"

echo "[2/5] 启动 Xvfb :99（带 GLX）+ 窗口管理器（让窗口正确 map/expose/最大化）"
Xvfb :99 -screen 0 1680x1050x24 -ac +extension GLX +render >/tmp/xvfb.log 2>&1 &
sleep 4
DISPLAY=:99 matchbox-window-manager -use_titlebar no >/tmp/wm.log 2>&1 &
sleep 2
chmod 1777 /tmp/.X11-unix 2>/dev/null || true
mkdir -p "$SHOTDIR" /tmp/qgchome
chown -R ubuntu:ubuntu "$SHOTDIR" /tmp/qgchome

echo "[3/5] 验证软件 OpenGL"
DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe glxinfo 2>/dev/null \
    | grep -iE "OpenGL renderer|OpenGL version" | head -2 || echo "  glxinfo 不可用"

echo "[4/5] 软件 GL 渲染启动 QGC（等 ${WAIT}s）并截图"
runuser -u ubuntu -- bash -c "
  export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe QT_QPA_PLATFORM=xcb HOME=/tmp/qgchome
  export QT_OPENGL=software MESA_GL_VERSION_OVERRIDE=3.3 MESA_GLSL_VERSION_OVERRIDE=330
  cd /project/build/Debug
  ./QGroundControl >/tmp/qgc.log 2>&1 &
  QPID=\$!
  sleep '$WAIT'
  WID=\$(xdotool search --name 'QGroundControl' 2>/dev/null | tail -1)
  echo \"  QGC 窗口 id=\$WID\"
  if [ -n \"\$WID\" ]; then
    xdotool windowactivate \$WID 2>/dev/null
    xdotool windowsize \$WID 1680 1010 2>/dev/null
    xdotool windowmove \$WID 0 0 2>/dev/null
    wmctrl -i -r \$WID -b add,maximized_vert,maximized_horz 2>/dev/null
  fi
  sleep 2
  scrot '$SHOTDIR/qgc-$LABEL.png' 2>/tmp/scrot.log && echo '  root 截图 OK'
  [ -n \"\$WID\" ] && import -window \$WID '$SHOTDIR/qgc-$LABEL-win.png' 2>/tmp/imp.log && echo '  窗口直抓 OK'
  kill \$QPID 2>/dev/null || true
"

echo "[5/5] 结果"
echo "=== QGC 日志(尾15) ==="; tail -15 /tmp/qgc.log 2>/dev/null
ls -la "$SHOTDIR"/ 2>/dev/null
