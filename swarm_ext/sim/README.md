# SITL 集群场景库 v1（WP1.5）

基于 PX4 SITL + Gazebo 的集群仿真验证环境，支撑 8 机规模回归、QGC L2 自动验收与
"一键中止 ≤1s"实测。
P4 将扩展至 ≥10 场景与 16/32 机回归。

## 组成

| 文件 | 说明 |
| --- | --- |
| `SITL测试流程.md` | **分层测试流程设计**：L1 机队/MAVLink 契约 + L2 QGC 应用逻辑，覆盖全部功能域与验收指标 |
| `launch_swarm_sitl.sh` | 一键拉起 N 机 PX4 SITL（v2 修正版，默认 4 机，gz 后端，内置 Xvfb + 软件 GL + heartbeat 就绪探测 + 负载预警） |
| `scenarios/*.json` | 12 个场景定义：时间线动作 + 故障注入 + 通过准则 |
| `run_scenario.py` | 场景运行器：MAVLink 或 QGC 接口驱动、故障注入、几何/时延实测、JSON 报告 |
| `regression.py` | **一键回归**：跑完全部场景 + 汇总 Markdown 报告(PASS/FAIL + 关键指标)，退出码可接 CI，`--dry-run` 自检 |
| `strict_regression.py` | **严格回归**：逐场景重建 PX4/Gazebo 机群；托管 QGC 时也隔离 HOME，杜绝跨场景状态污染 |
| `run_qgc_acceptance.sh` | 启动仅测试构建可用的 loopback QGC 验收接口；headless 模式不加载 QML |
| `run_qgc_gui_acceptance.sh` | 在独立 Xvfb/HOME 中托管正常 GUI QGC，供 QML 场景自动验收 |
| `test_qgc_lifecycle.py` | 重复验证 SIGINT/SIGTERM/正常 quit、启动中断、令牌脱敏、端口及容器清理 |
| `run_official_sitl.sh` | **官方 PX4 SITL docker 方案**：拉官方镜像 + 源码副本 fresh build，打通起飞(本机固件环境受阻的推荐路径，见测试流程 §9) |
| `auto_official_test.sh` | 官方 Docker 共享机队 L1 冒烟（03/04/05）；全量验收使用 `strict_regression.py` |
| `formation_sync_check.py` | **同步编队航段 SITL 验收**（S1–S5）：经 QGC 验收接口驱动启动/goto/航线/掉头，输出起步与到达时间差、偏离同步理想位置与最小间距 |
| `新功能验收checklist.md` | 控制租约 / 任务持久化两项新功能的可勾选验收单(A 8 例 / B 5 例) |

## 控制安全与可靠性（Lattice 安全收尾）

控制租约（`SwarmControlLease`）与任务持久化（`SwarmPersistence`）的核心安全逻辑已由容器
单测覆盖（`tst_lease` / `tst_persistence` / `tst_pipeline::leaseGatesAutomation`），不依赖 SITL：

- 集成层用 Vehicle/FirmwarePlugin 的规范飞行模式访问器（Mission/Pause/RTL/SmartRTL/Land/Follow/
  Goto/Takeoff）构成自主模式集；已持有租约时，集合外的模式切换立即判定为人工接管；
  首次申请租约时，未经人工授权的 AI/任务路径不得从手动、Offboard 或未知模式抢占控制；
  面板直接指令、已确认 AI 计划和模板操作视为显式人工授权，
  与 `flightMode()` 同 tr() 上下文、locale 一致；集合外（操作员手动直控 / Offboard 外部控制 / 未知）
  → 该机租约立即释放；批量指令在排队和真正触发时双重检查，持续编队和任务上传也使用同一逐机闸门。
  切回自主模式后可重新申请租约；有效编队控制会续期租约，不在本地化显示名上硬编码字面量
- 重启后 RUNNING 任务恢复为 PAUSED、原子写回磁盘并在控制器显示待确认状态；已完成/中止任务清除
  持久化，租约不持久化（回 OBSERVE_ONLY）

SITL 启动链路已在官方 PX4 v1.15.4 源码构建上完成 1→4→8 递进闭环。历史“无 heartbeat”的
根因是启动脚本漏设 `GZ_SIM_RESOURCE_PATH`，现已通过加载 `gz_env.sh` 修复。2026-08-25 的隔离
实测中，6 个场景报告均通过；8 机一键中止 ACK 最大 16.3ms（阈值 1000ms）。

## 场景清单

| # | 场景 | 验证点 |
| --- | --- | --- |
| 01 | 区域搜索全流程 | 经 QGC 规划并逐机上传覆盖航线、启动任务、返航落地 |
| 02 | 编队巡航 | 经 QGC 启动楔形编队并计算水平/垂直收敛误差 |
| 03 | 单机故障注入 | 安全移除 Gazebo 模型后 kill 1 机，验证余下 7 机返航 |
| 04 | 链路退化注入 | 丢弃单机 GCS MAVLink UDP 20s，验证离线与自动恢复 |
| 05 | 一键中止时延实测 | 暂停指令→ACK 逐机时延 **≤1s**（验收指标） |
| 06 | 感知链路馈送 | 断言 QGC 接收时延、COP 地理误差、模型航迹及实际 QML delegate 数 |
| 07 | 控制租约与人工接管 | 自动验证 AI 抢占拒绝、操作员授权、人工接管释放与恢复 |
| 08 | 多阶段任务编排 | 自动验证任务启动、人工阶段等待/继续、正常完成以及安全中止 |
| 09 | 两机任务局部退出 | 一机退出后不再收到派发，后续阶段仍按剩余目标推进 |
| 10 | L3 权限收紧 | 自动动作排队期间收紧白名单，验证在途动作被拦截 |
| 11 | 航线上传后收紧限高 | 上传完成后调低高度上限，验证阶段被阻塞而非继续下发 |
| 12 | 任务超时明确失败 | 条件不可能满足时任务判 Failed 并请求目标机上锁 |

## 使用

```bash
# 1. 先按仓库规范构建带测试能力的 QGC
just build

# 2. 严格回归会逐场景自动启动/停止 QGC 与 PX4 机群
pip install pymavlink
umask 077
openssl rand -hex 24 > /tmp/qgc-swarm-token
export QGC_SWARM_ACCEPTANCE_TOKEN_FILE=/tmp/qgc-swarm-token
python3 strict_regression.py --px4-src ~/PX4-Autopilot --only 01,02,07

# 06 由 runner 托管正常 GUI/Xvfb；可重复执行且成功后自动删除临时 HOME
python3 strict_regression.py --only 06 --qgc-mode managed-gui --repeat 3 \
  --keep-workdir on-failure --qgc-token-file "$QGC_SWARM_ACCEPTANCE_TOKEN_FILE"

# 也可连接人工启动的正常 GUI QGC
QGC_SWARM_ACCEPTANCE_TOKEN_FILE="$QGC_SWARM_ACCEPTANCE_TOKEN_FILE" ./build/Debug/QGroundControl
python3 strict_regression.py --only 06 --qgc-mode external \
  --qgc-token-file "$QGC_SWARM_ACCEPTANCE_TOKEN_FILE"

# 3. 只校验场景定义（无需机队）
python3 run_scenario.py scenarios/05_emergency_stop.json --dry-run
```

## 说明

- 多机接入：每实例 `-i N` 启动（sysid=N+1），GCS 流统一发往 14550，QGC 按 sysid 自动接入。
- `strict_regression.py` 为每个场景重启一套全新机队；连续复用模式只保留给快速 L1 冒烟。
- 01/02/07/08/09/10/11/12 使用 QGC loopback 验收接口，因此 QGC 与 Python 驱动不会同时争抢
  UDP 14550；03/04/05 仍由 pymavlink 直接验证 MAVLink/PX4 契约。
- 解锁重试窗口是 `run_scenario.ARM_RETRY_TIMEOUT_SEC = 180`（单源，场景 JSON 不再各写一份）：
  8 机在同一个 Gazebo 世界里错峰启动，末位实例进入可解锁状态明显晚于首位。个别场景仍可用
  `retry_timeout_sec` 覆写。
- 03/04/05 在广播 ARM 前先按机队多数比对 `SYS_STATUS` 传感器 `present` 位；比同伴少报的实例
  判为环境阻塞（退出码 3），判据与防掩盖边界见 [测试说明](../tests/README.md)。
- 接口只编入 `QGC_BUILD_TESTING` 构建并绑定 `127.0.0.1`。认证 Secret 必须来自 owner-only
  普通文件（`0600` 权限、16–512 UTF-8 字节、单行、非符号链接）；不接受环境变量中的明文 Secret。
- headless QGC 可执行任务/编队/租约逻辑，但明确报告 `ui.qml_ready=false`；场景 06 的渲染硬门槛
  必须使用 `managed-gui` 或 `external`，不能降级或跳过。
- `--keep-workdir` 支持 `always|on-failure|never`，默认只保留失败场景；每轮使用独立 HOME、端口、
  日志和 JSON。托管 GUI 失败时会尽力保存 Xvfb 根窗口截图。
- 报告中的 `pause_latency.per_vehicle_ms` 即"一键中止响应 ≤1s"的实测证据。

## 2026-08-25 实测结果

| 规模/场景 | 结果 | 关键指标 |
| --- | --- | --- |
| 单机冒烟 | PASS | arm→takeoff→RTL→落地，36.3s |
| 4 机严格飞行 | PASS | 4/4 越过 5m并落地；暂停 ACK max 18.9ms |
| 01 | PASS | 8/8 越过 25m并落地，80.4s |
| 02 | PASS | 8/8 越过 30m并落地，106.4s |
| 03 | PASS | sysid 3 离线后 7/7 在线并返航，101.8s |
| 04 | PASS | sysid 4 离线→恢复，8/8 返航，93.3s |
| 05 | PASS | 暂停 ACK max **16.3ms**，73.4s |
| 06 | PASS | 150 帧、148 航迹批次、30 心跳/30s，30.7s |

证据边界：上表是 2026-08-25 的历史 L1 实测；本次新增了 01/02/06/07 的 L2 自动化与严格隔离
执行器，但历史数值不能自动升级为新链路的实测结论，仍需在具备 PX4 镜像/正常 GUI 的环境重跑。

## Headless QGC 界面诊断

在 `qgc-ubuntu-docker` 容器内可运行 `../tests/screenshot_qgc.sh [标签] [等待秒数]`，用 Xvfb +
llvmpipe 截取 QGC；若仍为黑屏，再运行 `../tests/screenshot_diag.sh` 对比软件后端、软件 GL 和
`glxgears`，结果写入 `swarm_ext/docs/shots/`。两个脚本都会安装临时诊断依赖，应只在一次性容器中运行。
