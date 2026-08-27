# SITL 集群场景库 v1（WP1.5）

基于 PX4 SITL + Gazebo 的集群仿真验证环境，支撑 8 机规模回归与"一键中止 ≤1s"实测。
P4 将扩展至 ≥10 场景与 16/32 机回归。

## 组成

| 文件 | 说明 |
|---|---|
| `SITL测试流程.md` | **分层测试流程设计**：L1 机队/MAVLink 契约（自动化）+ L2 QGC 应用逻辑（半自动），覆盖全部功能域与验收指标 |
| `launch_swarm_sitl.sh` | 一键拉起 N 机 PX4 SITL（v2 修正版，默认 4 机，gz 后端，内置 Xvfb + 软件 GL + heartbeat 就绪探测 + 负载预警） |
| `scenarios/*.json` | 6 个场景定义：时间线动作 + 故障注入 + 通过准则 |
| `run_scenario.py` | 场景运行器：MAVLink 驱动、故障注入、时延实测、JSON 报告 |
| `regression.py` | **一键回归**：跑完全部场景 + 汇总 Markdown 报告(PASS/FAIL + 关键指标)，退出码可接 CI，`--dry-run` 自检 |
| `run_official_sitl.sh` | **官方 PX4 SITL docker 方案**：拉官方镜像 + 源码副本 fresh build，打通起飞(本机固件环境受阻的推荐路径，见测试流程 §9) |
| `auto_official_test.sh` | 官方 Docker 共享机队试跑：定向管理单个容器；严格验收仍须逐场景重启机队 |
| `新功能验收checklist.md` | 控制租约 / 任务持久化两项新功能的可勾选验收单(A 7 例 / B 5 例) |

## 控制安全与可靠性（Lattice 安全收尾）

控制租约（`SwarmControlLease`）与任务持久化（`SwarmPersistence`）的核心安全逻辑已由容器
单测覆盖（`tst_lease` / `tst_persistence` / `tst_pipeline::leaseGatesAutomation`），不依赖 SITL：
- 集成层用 Vehicle/FirmwarePlugin 的规范飞行模式访问器（Mission/Pause/RTL/SmartRTL/Land/Follow/
  Goto/Takeoff）构成自主模式集；仅在自动化已有租约时，集合外的模式切换才判定为人工接管，
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
|---|---|---|
| 01 | 区域搜索飞行骨架 | 8 机起飞/保持/返航；覆盖航线上传仍由 QGC L2 验证 |
| 02 | 编队巡航飞行骨架 | 8 机起飞/保持/返航；楔形几何收敛仍由 QGC L2 验证 |
| 03 | 单机故障注入 | 安全移除 Gazebo 模型后 kill 1 机，验证余下 7 机返航 |
| 04 | 链路退化注入 | 丢弃单机 GCS MAVLink UDP 20s，验证离线与自动恢复 |
| 05 | 一键中止时延实测 | 暂停指令→ACK 逐机时延 **≤1s**（验收指标） |
| 06 | 感知链路馈送 | 感知服务实时 UDP 输出；QGC COP 显示仍由 L2 验证 |

## 使用

```bash
# 1. 构建 PX4 SITL（一次性）
cd ~/PX4-Autopilot && make px4_sitl gz_x500

# 2. 拉起 8 机集群
./launch_swarm_sitl.sh 8 ~/PX4-Autopilot gz

# 3. 另一个终端：启动定制 QGC，确认 8 机接入

# 4. 运行场景（CI/自动化回归）
pip install pymavlink
python3 run_scenario.py scenarios/01_area_search.json --report report_01.json

# 5. 只校验场景定义（无需机队）
python3 run_scenario.py scenarios/05_emergency_stop.json --dry-run
```

## 说明

- 多机接入：每实例 `-i N` 启动（sysid=N+1），GCS 流统一发往 14550，QGC 按 sysid 自动接入。
- 严格飞行验收应为**每个场景重启一套全新机队**。连续复用同一机队会让前一场景的估计器、
  解锁和故障状态污染后一场景；`regression.py` 适合 dry-run 或已由外层逐场景重启机队的环境。
- 场景 01/02 中的任务上传步骤当前经 QGC 面板（AI 或模板）执行；后续迭代将把
  QGC 侧统一管线操作纳入运行器，实现端到端无人值守回归。
- 报告中的 `pause_latency.per_vehicle_ms` 即"一键中止响应 ≤1s"的实测证据。

## 2026-08-25 实测结果

| 规模/场景 | 结果 | 关键指标 |
|---|---|---|
| 单机冒烟 | PASS | arm→takeoff→RTL→落地，36.3s |
| 4 机严格飞行 | PASS | 4/4 越过 5m并落地；暂停 ACK max 18.9ms |
| 01 | PASS | 8/8 越过 25m并落地，80.4s |
| 02 | PASS | 8/8 越过 30m并落地，106.4s |
| 03 | PASS | sysid 3 离线后 7/7 在线并返航，101.8s |
| 04 | PASS | sysid 4 离线→恢复，8/8 返航，93.3s |
| 05 | PASS | 暂停 ACK max **16.3ms**，73.4s |
| 06 | PASS | 150 帧、148 航迹批次、30 心跳/30s，30.7s |

证据边界：上述 01/02 是 MAVLink 飞行骨架，并未自动操作 QGC 的覆盖规划/编队控制；06 已断言
航迹批次数与发送心跳频率，但未自动断言 QGC COP 渲染、地理误差或接收端端到端时延。

## Headless QGC 界面诊断

在 `qgc-ubuntu-docker` 容器内可运行 `../tests/screenshot_qgc.sh [标签] [等待秒数]`，用 Xvfb +
llvmpipe 截取 QGC；若仍为黑屏，再运行 `../tests/screenshot_diag.sh` 对比软件后端、软件 GL 和
`glxgears`，结果写入 `swarm_ext/docs/shots/`。两个脚本都会安装临时诊断依赖，应只在一次性容器中运行。
