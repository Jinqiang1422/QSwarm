# SITL 集群场景库 v1（WP1.5）

基于 PX4 SITL + Gazebo 的集群仿真验证环境，支撑 8 机规模回归与"一键中止 ≤1s"实测。
P4 将扩展至 ≥10 场景与 16/32 机回归。

## 组成

| 文件 | 说明 |
|---|---|
| `launch_swarm_sitl.sh` | 一键拉起 N 机 PX4 SITL（默认 8 机，gz 后端） |
| `scenarios/*.json` | 5 个场景定义：时间线动作 + 故障注入 + 通过准则 |
| `run_scenario.py` | 场景运行器：MAVLink 驱动、故障注入、时延实测、JSON 报告 |

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
