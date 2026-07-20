# P1 首战:官方 Docker 打通 QGC + Swarm 整机编译 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (推荐,因本计划含 tmux 长任务与人在环路调试) 或 superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 QGroundControl 官方 Docker 构建链,把已合并 Swarm 扩展的 QGC 在真实 Qt6 环境编译通过,产出可运行产物。

**Architecture:** 复用仓库自带 `deploy/docker/run-docker.sh ubuntu Debug`(容器基于 ubuntu:24.04,内部自动装 Qt6.x + cmake≥3.25 + gcc-12),仓库挂载进容器编译,产物写回宿主。境外流量经宿主代理(gost HTTP 127.0.0.1:8118 → socks5 VPS 10809)。首次构建放 tmux 手动跑,编译错误逐类最小化修复并单独 commit。

**Tech Stack:** Docker 28.x、QGC 官方 Dockerfile、Qt6/CMake/Ninja、C++20、tmux。

**分支:** `feature/swarm-extension`

**参考 spec:** `docs/superpowers/specs/2026-07-19-p1-docker-build-design.md`

---

## Task 0: 前置检查与 run-docker.sh 摸底

**Files:**
- Read: `deploy/docker/run-docker.sh`(全文)
- Read: `deploy/docker/variants.json`、`deploy/docker/entrypoint.sh`

- [ ] **Step 1: 确认工具与分支**

Run:
```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
docker info >/dev/null 2>&1 && echo "docker ok"
git branch --show-current
```
Expected: 打印 `docker ok` 与 `feature/swarm-extension`。

- [ ] **Step 2: 通读 run-docker.sh,确定三件事**

Run:
```bash
sed -n '1,200p' deploy/docker/run-docker.sh
```
读出并记录:(a) `BUILD_DIR` 默认值/产物写回宿主的路径;(b) 它调用 `docker build` 与 `docker run` 的确切形式;(c) 是否透传 `http_proxy/https_proxy` 或 `--network`。
Expected: 明确 ubuntu variant 的 image tag、build 输出目录、代理注入点。

- [ ] **Step 3: 记录摸底结论到计划旁注**

把 Step 2 的结论追加到 `swarm_ext/docs/build-notes.md`(新建),包含产物路径与代理注入方式,后续步骤据此填参数。

Run:
```bash
git add swarm_ext/docs/build-notes.md
git commit -m "docs(swarm): 记录 Docker 构建摸底(产物路径/代理注入点)"
```

---

## Task 1: 代理通路小实验(构建前必做)

**Files:** 无(纯验证)

- [ ] **Step 1: 验证宿主代理可达境外**

Run:
```bash
curl -s --socks5-hostname 127.0.0.1:10809 -o /dev/null -w "socks5 %{http_code}\n" https://github.com
curl -s -x http://127.0.0.1:8118 -o /dev/null -w "http8118 %{http_code}\n" https://github.com
```
Expected: 两行都是 `... 200`。若 8118 失败只用 socks5 10809。

- [ ] **Step 2: 验证容器内经宿主代理可达境外**

Run(`--network host` 让容器直接用宿主 127.0.0.1 代理):
```bash
docker run --rm --network host curlimages/curl:latest \
  -s -x http://127.0.0.1:8118 -o /dev/null -w "container->proxy %{http_code}\n" https://github.com
```
Expected: `container->proxy 200`。若失败,改用 `-e ALL_PROXY=socks5h://127.0.0.1:10809` 复测,确定容器内可用的代理形式。

- [ ] **Step 3: 若拉取 curlimages/curl 本身就需代理**

先给 Docker daemon 配代理(需要时):
```bash
sudo mkdir -p /etc/systemd/system/docker.service.d
printf '[Service]\nEnvironment="HTTP_PROXY=http://127.0.0.1:8118"\nEnvironment="HTTPS_PROXY=http://127.0.0.1:8118"\nEnvironment="NO_PROXY=localhost,127.0.0.1"\n' \
  | sudo tee /etc/systemd/system/docker.service.d/http-proxy.conf
sudo systemctl daemon-reload && sudo systemctl restart docker
```
Expected: `docker info | grep -i proxy` 显示已配置。记录到 build-notes.md。

---

## Task 2: 启动首次 Docker 构建(tmux 长任务)

**Files:** 无(执行构建)

- [ ] **Step 1: 依 Task 0 结论组装构建命令**

按 run-docker.sh 的代理注入方式,准备命令(示例,依 Task 0 结论调整代理传递):
```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
export HTTP_PROXY=http://127.0.0.1:8118 HTTPS_PROXY=http://127.0.0.1:8118
export http_proxy=http://127.0.0.1:8118 https_proxy=http://127.0.0.1:8118
deploy/docker/run-docker.sh ubuntu Debug
```

- [ ] **Step 2: 放 tmux 手动跑并落盘日志**

Run(用户在 tmux 中执行):
```bash
tmux new -s qgcbuild
# tmux 内:
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
export HTTP_PROXY=http://127.0.0.1:8118 HTTPS_PROXY=http://127.0.0.1:8118 http_proxy=http://127.0.0.1:8118 https_proxy=http://127.0.0.1:8118
deploy/docker/run-docker.sh ubuntu Debug 2>&1 | tee /tmp/qgc-build-01.log
```
Expected: 依次出现拉基础镜像 → 装 Qt/依赖 → cmake 配置 → 编译。可 `Ctrl-b d` detach,Claude 用 `tail -f /tmp/qgc-build-01.log` 盯进度。

- [ ] **Step 3: 判定构建结束状态**

Run:
```bash
tail -40 /tmp/qgc-build-01.log
grep -nE "error:|Error|undefined reference|FAILED" /tmp/qgc-build-01.log | head -40
```
Expected: 二者之一 —— (a) 无 error、出现产物/`Build complete` → 跳到 Task 4;(b) 有 error → 进入 Task 3。

---

## Task 3: 编译错误修复循环(响应式,迭代直到通过)

> 错误内容在真实编译前未知,故此任务是**可复用迭代范式**,每轮针对一类错误。只改 Swarm 扩展代码(`src/Swarm/`、`src/FlyView/Swarm*.qml` 及 6 处集成点),非必要不动 QGC 上游。

**每一轮循环执行 Step 1~5:**

- [ ] **Step 1: 抓取第一类错误的最小上下文**

Run:
```bash
grep -nE "error:|undefined reference|No such file|is not a member" /tmp/qgc-build-01.log | head -20
```
定位到具体文件:行号、缺失符号/头文件/类型。

- [ ] **Step 2: 归类并定位根因(常见类别 → 处理方向)**

| 错误类别 | 典型信息 | 处理方向 |
|---|---|---|
| 头文件缺失 | `fatal error: X.h: No such file` | 修 `#include` 路径 / 补 `target_include_directories` |
| Qt6 API 变更 | `is not a member of 'Qt...'` | 按 Qt6.x 正确 API 改用法 |
| 未解析符号 | `undefined reference to SwarmXxx::...` | 该 .cc 未编入 → 检查 `src/Swarm/CMakeLists.txt` 是否漏列 |
| QML 注册 | `QML module ... not found` / 类型未注册 | 检查 `QML_ELEMENT`、FlyView CMake 是否登记该 qml |
| moc/信号槽 | `undefined ... vtable` | 检查 `Q_OBJECT`、`Q_MOC_INCLUDE` |

在对应源文件用最小改动修复(展示改动前后)。

- [ ] **Step 3: 增量重编(仅重跑构建,不重建镜像)**

Run(依 Task 0 结论,增量构建通常复用已建镜像;若 run-docker.sh 每次全量,则重跑 Step,利用缓存):
```bash
deploy/docker/run-docker.sh ubuntu Debug 2>&1 | tee /tmp/qgc-build-02.log
tail -30 /tmp/qgc-build-02.log
```
Expected: 该类错误消失(可能暴露下一类)。

- [ ] **Step 4: 该类错误清零后提交**

Run:
```bash
git add -A
git commit -m "fix(swarm): 修复<错误类别>编译错误(<文件>)"
```
每类错误一个 commit,message 写清修了什么、为何。

- [ ] **Step 5: 回到 Step 1,处理下一类;直至 `grep -c error:` 为 0**

Run:
```bash
grep -cE "error:|undefined reference|FAILED" /tmp/qgc-build-*.log | tail -1
```
Expected: 最终一轮为 `0`,构建产物生成。

---

## Task 4: Swarm 集成验证

**Files:** 无(验证产物)

- [ ] **Step 1: 定位构建产物**

Run(路径依 Task 0 的 BUILD_DIR 结论,示例):
```bash
find . -maxdepth 3 -name "QGroundControl" -type f -o -name "*.AppImage" 2>/dev/null
ls -la build*/  2>/dev/null | head
```
Expected: 找到可执行文件或 AppImage。

- [ ] **Step 2: 抽查 Swarm 符号已链接**

Run(对 QGroundControl 可执行文件;若是 AppImage 先 `--appimage-extract`):
```bash
BIN=$(find . -maxdepth 3 -name QGroundControl -type f | head -1)
nm -C "$BIN" 2>/dev/null | grep -iE "SwarmManager::instance|SwarmAIAgent|SwarmCOPModel" | head
```
Expected: 出现 Swarm 相关符号,证明 21 模块编入。

- [ ] **Step 3: 确认 Swarm QML 编入资源**

Run:
```bash
strings "$BIN" 2>/dev/null | grep -iE "SwarmControlPanel|SwarmAIPanel|SwarmSituationLayer" | head
```
Expected: 出现 3 个 Swarm QML 名称,证明已注册进 FlyView QML 模块。

- [ ] **Step 4: 记录验证证据**

把 Step 1~3 输出追加到 `swarm_ext/docs/build-notes.md`。

---

## Task 5: 收尾与文档

**Files:**
- Modify: `swarm_ext/docs/build-notes.md`
- Modify: `docs/superpowers/specs/2026-07-19-p1-docker-build-design.md`(附记完成状态)

- [ ] **Step 1: 标记 P1 遗留第 1 项完成**

在 build-notes.md 记录:构建命令、产物路径、编译通过日志尾部、Swarm 符号验证结果、本轮修复的 commit 列表。

- [ ] **Step 2: 提交收尾文档**

Run:
```bash
git add swarm_ext/docs/build-notes.md docs/superpowers/specs/2026-07-19-p1-docker-build-design.md
git commit -m "docs(swarm): 记录 P1 整机编译打通结果与验证证据"
```

- [ ] **Step 3: 汇总本战成果**

回顾:编译是否通过、修了几类错误、产物可否运行(GUI 实跑属下一阶段)。据此决定 P1 下一战(LLM 评测基线 / SITL 实测 / CI 接入)。

---

## 完成定义(Definition of Done)

1. `deploy/docker/run-docker.sh ubuntu Debug` 构建全程无 error,产出 QGroundControl 产物。
2. `nm`/`strings` 抽查确认 Swarm 21 模块符号 + 3 个 Swarm QML 已编入。
3. 编译错误修复过程有清晰的逐类 commit。
4. build-notes.md 留存编译通过证据。

**非目标(后续阶段):** GUI 实跑验证、LLM 评测基线首测、SITL 8 机实测、CI 接入 .github/workflows。
