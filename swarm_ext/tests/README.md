# QGC Swarm 全面测试

本目录的测试对象是定制 QGroundControl 集群软件，而不是把上游 QGC 的数千个测试重复实现一遍。
基础平台由 QGC `Unit|Integration` 测试守护；这里重点覆盖 `src/Swarm`、集群 QML、感知 sidecar、
任务模板、LLM 工具契约和 PX4 SITL 端到端链路。

## 功能分析

| 功能域 | 主要实现 | 必须验证的核心契约 |
|---|---|---|
| 集群状态与批量控制 | `SwarmManager`、`Vehicle`、`FirmwarePlugin` | 多机选择、命令错峰、空指针安全、不同固件飞行模式抽象 |
| AI 指挥与安全 | `SwarmAIAgent`、`SwarmInputFilter`、`SwarmAutonomy` | 防注入、工具白名单、L1/L2/L3 自主门控、危险动作确认 |
| 约束与规划 | `SwarmConstraintChecker`、`SwarmRehearsalEstimator`、`SwarmMissionPlanner` | 围栏/电量/通信/间隔检查、覆盖规划、逐机上传状态 |
| 编队与协同战术 | `FormationController`、`SwarmTaskAllocator`、`SwarmTactics` | 4 种队形、水平/垂直误差、分配稳定性、控制租约持续守门 |
| 多阶段任务 | `SwarmTaskController`、`SwarmTaskLibrary`、`SwarmPersistence` | 模板参数、阶段条件、人工门控、超时中止、重启后暂停恢复 |
| 控制权安全 | `SwarmControlLease` | AI 初始接管拒绝、操作员授权、人工模式立即释放、租约过期 |
| 感知与 COP | `SwarmPerceptionBridge`、`SwarmTrackFusion`、`SwarmCOPModel` | TUNNEL/UDP、融合去重、地理误差、时延、热力图和 QML delegate |
| 态势与告警 | `SwarmEventDetector`、`SwarmSituationDigest`、`SwarmVideoHub` | 告警冷却、摘要来源、视频流状态与降级 |
| 可追溯性 | `SwarmAuditLogger` | 用户输入、模型输出、校验、确认和实际下发动作可审计 |
| SITL 故障安全 | `sim/run_scenario.py`、`sim/strict_regression.py` | 单机故障、链路退化、一键暂停 ≤1s、逐场景状态隔离 |

## 测试分层

`run_all.py` 对每个必测项只给出四种结果：`PASS`、`FAIL`、`BLOCKED`、`SKIP`。
缺少 Qt、PX4、显示环境或外部模型时使用 `BLOCKED`，不会把未运行项目伪装成通过。

| 层级 | 内容 | smoke | standard | full | qualification |
|---|---|:---:|:---:|:---:|:---:|
| L0 源码契约 | diff、格式、lint、场景/模板/CMake/Schema/XML 一致性 | ✓ | ✓ | ✓ | ✓ |
| L1 Python/算法 | 感知、几何、COP 真值、LLM 数据集 dry-run | ✓ | ✓ | ✓ | ✓ |
| L2 场景解析 | 8 个场景的动作识别和报告编排 | ✓ | ✓ | ✓ | ✓ |
| L3 C++ 模块 | P1/P2/P3、战术、管线、租约、持久化、COP、容量门槛 |  | ✓ | ✓ | ✓ |
| L4 QGC 集成 | 容器内增量构建、QGC Unit/Integration、进程生命周期 |  |  | ✓ | ✓ |
| L5 SITL/QGC | 8 机覆盖、编队、故障、租约、任务人工门控与中止 |  |  | ✓ | ✓ |
| L6 GUI/QML | runner 托管正常 GUI/Xvfb，验证 delegate、时延和地理误差 |  |  | ✓ | ✓ |
| Q1 稳定性资格 | 生命周期 20 轮、Unit/Integration 50 轮、场景 03 20 轮 |  |  |  | ✓ |
| Q2 发布资格 | 非测试 Release 构建及验收接口产物审计 |  |  |  | ✓ |
| L7 外部模型 | 559 条自然语言指令在线评测 |  |  | 可选 | 可选 |

## 使用

```bash
# 日常开发：静态检查 + Python + 独立 C++（宿主无 Qt 时自动使用 QGC Docker 镜像）
python3 swarm_ext/tests/run_all.py --profile standard

# 列出完整验收项
python3 swarm_ext/tests/run_all.py --profile full --list

# 全面验收；需要 PX4 源码、QGC 测试构建和 Docker
# SITL 默认 auto：已有 px4_sitl_default/gz 时优先本地，否则使用官方镜像
python3 swarm_ext/tests/run_all.py --profile full \
  --px4-src ~/PX4-Autopilot \
  --report-dir /tmp/qgc-swarm-full

# 显式选择 SITL 后端（避免首次运行时拉取大型官方镜像）
python3 swarm_ext/tests/run_all.py --profile full \
  --sitl-backend local --px4-src ~/PX4-Autopilot

# 场景 06 默认由 runner 在隔离 Xvfb/HOME/端口中托管正常 GUI，无需人工启动
python3 swarm_ext/tests/run_all.py --profile full \
  --qgc-mode managed-gui --repeat 3 --keep-workdir on-failure

# 连接人工启动的外部 GUI 时，双方使用同一个 owner-only Secret 文件
umask 077
openssl rand -hex 24 > /tmp/qgc-swarm-token
python3 swarm_ext/tests/run_all.py --profile full --qgc-mode external \
  --qgc-token-file /tmp/qgc-swarm-token

# 发布前稳定性资格入口；每轮 CTest 都保存独立 JUnit/日志，首次失败即停
python3 swarm_ext/tests/run_all.py --profile qualification \
  --sitl-backend local --px4-src ~/PX4-Autopilot

# 只跑指定项目
python3 swarm_ext/tests/run_all.py --profile standard \
  --only source.asset_contracts,cpp.ctest
```

输出目录包含：

- `SUMMARY.md`：人可读结论；
- `report.json`：机器可读结果；
- `environment.json`：提交/Dirty 状态、宿主、Qt/CMake/GStreamer/PX4 与镜像摘要；
- `logs/*.log`：每项测试的完整输出；
- 分层子报告：场景、SITL、LLM 和资产契约的原始证据；
- `report.json.artifacts`：成功证据的大小和 SHA-256 索引；失败场景另保留工作目录和截图。

退出码约定：`0` 全部必测项通过，`1` 存在实际失败，`2` 存在环境阻塞。只有明确接受不完整
证据时才使用 `--allow-blocked`。

## 证据边界

- `--dry-run` 只证明 JSON、动作分发和报告格式正确，不能证明 PX4 飞行或 QGC 内部逻辑通过。
- headless QGC 能验证 Manager/任务/租约逻辑，但不能证明 QML 已渲染。
- 场景 06 只有 `ui.qml_ready=true` 且实际 delegate 数、时延和地理误差全部达标才通过。
- LLM `--dry-run` 只验证评测资产；意图准确率必须用 `--with-llm` 连接目标模型实测。
- SITL 结果不能替代实机、半实物和人因测试；这些属于发布验收的后续证据层。
