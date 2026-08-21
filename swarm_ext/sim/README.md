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

## 控制安全与可靠性（Lattice 安全收尾）

控制租约（`SwarmControlLease`）与任务持久化（`SwarmPersistence`）的核心安全逻辑已由容器
单测覆盖（`tst_lease` / `tst_persistence` / `tst_pipeline::leaseGatesAutomation`），不依赖 SITL：
- 集成层用 Vehicle 的规范飞行模式访问器（`missionFlightMode()`/`pauseFlightMode()`/
  `rtlFlightMode()`/`smartRTLFlightMode()`/`landFlightMode()`/`followFlightMode()`）构成自主模式集，
  与 `flightMode()` 同 tr() 上下文、locale 一致；集合外（操作员手动直控 / Offboard 外部控制 / 未知）
  → 该机租约立即释放、Swarm 跳过对其自动下发（逐机粒度，其余机不受影响）；切回自主模式即解除
  接管、可重新申请（可恢复）。不在本地化显示名上硬编码字面量（会随语言失配）
- 重启后 RUNNING 任务恢复为 PAUSED 待确认，已完成/中止任务清除持久化、租约不持久化（回 OBSERVE_ONLY）

SITL 多机闭环验收（多机识别 / 任务不串机 / 重启不恢复）脚本就绪。单机启动链路本会话已
验证（`gz_bridge` 连通 world）；**历史"无 heartbeat"经诊断为资源竞争**——gz lockstep
仿真依赖 CPU 时间片，本机长期跑 YOLO 训练（load≈8 满载）时仿真时钟推不动，非代码/配置
缺陷。在空闲机器按 `SITL测试流程.md` §2 逐步冒烟（1→4→8）即可执行完整测试。

## 场景清单

| # | 场景 | 验证点 |
|---|---|---|
| 01 | 区域搜索全流程 | 起飞→覆盖→完成→返航闭环 |
| 02 | 编队巡航 | 楔形编队收敛与保持 |
| 03 | 单机故障注入 | kill 1 机后其余机不受影响、离线可见 |
| 04 | 链路退化注入 | SIGSTOP 冻结/恢复 1 机，遥测超时与自动重连 |
| 05 | 一键中止时延实测 | 暂停指令→ACK 逐机时延 **≤1s**（验收指标） |

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
- 场景 01/02 中的任务上传步骤当前经 QGC 面板（AI 或模板）执行；后续迭代将把
  QGC 侧统一管线操作纳入运行器，实现端到端无人值守回归。
- 报告中的 `pause_latency.per_vehicle_ms` 即"一键中止响应 ≤1s"的实测证据。
