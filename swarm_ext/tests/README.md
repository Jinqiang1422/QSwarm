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
| L2 场景解析 | 12 个场景的动作识别和报告编排 | ✓ | ✓ | ✓ | ✓ |
| L3 C++ 模块 | P1/P2/P3、战术、管线、租约、持久化、COP、容量门槛 |  | ✓ | ✓ | ✓ |
| L4 QGC 集成 | 容器内增量构建、QGC Unit/Integration、进程生命周期 |  |  | ✓ | ✓ |
| L5 SITL/QGC | 8 机覆盖、编队、故障、租约、任务人工门控与中止；局部退出、L3 权限收紧、上传后限高收紧、任务超时判失败 |  |  | ✓ | ✓ |
| L6 GUI/QML | runner 托管正常 GUI/Xvfb，验证 delegate、时延和地理误差 |  |  | ✓ | ✓ |
| Q1 稳定性资格 | 生命周期 20 轮、Unit/Integration 50 轮、场景 03 20 轮 |  |  |  | ✓ |
| Q2 发布资格 | 非测试 Release 构建与产物审计；交付构建（关自动下发）的锁行为、面板布局与全量 Unit |  |  |  | ✓ |
| L7 外部模型 | 559 条自然语言指令在线评测 |  |  | 可选 | 可选 |

### L0 lint 的工具版本要求

`source.ruff`、`source.ruff_format`、`source.shellcheck`、`source.clang_format` 会先比对宿主
工具版本与 `.pre-commit-config.yaml` 中固定的版本。不一致判 `BLOCKED`（退出码 2），不判 FAIL
也不判 PASS：规则集不同的工具给出的结论不能代表 CI 判定。已实测的两类失真——ruff 0.9 报的
`UP038` 在 0.15 已被移除（假失败），shellcheck 0.7 缺少 0.8–0.11 新增的全部检查（假通过）。

首次遇到 `BLOCKED` 时按提示安装对应版本，不要用 `--allow-blocked` 绕过。逐项的宿主/固定版本
和比对结果记录在报告的 `environment.json` → `tool_pins`。

**提供固定版工具时，只把需要的二进制放进 `PATH`，不要前置整个 venv 的 `bin` 目录。**
venv 里的 `python3` 会一并顶掉系统解释器，而 `python.perception` 和直连 MAVLink 的场景
03/04/05 依赖系统 Python 的 `numpy`、`pymavlink`，会在 0.1 秒内崩于 `ModuleNotFoundError`
并报成 FAIL——本轮收尾验证即因此产生四个假失败。推荐做法：

```bash
python3.12 -m venv /tmp/lintvenv
/tmp/lintvenv/bin/pip install ruff==<pin> shellcheck-py==<pin>
mkdir -p /tmp/pinned-lint-bin
ln -sf /tmp/lintvenv/bin/ruff /tmp/lintvenv/bin/shellcheck /tmp/pinned-lint-bin/
PATH=/tmp/pinned-lint-bin:$PATH python3 swarm_ext/tests/run_all.py ...
```

放行前用 `PATH=... python3 -c "import numpy, pymavlink"` 自检一次。

**长验证运行期间不要在同一台机器上跑其他重负载任务。** 与 20 并行的容器构建重叠时，
8 机 SITL 场景会在 6–8 秒内「SITL 提前退出」（仿真器进程起不来），`SwarmSafetyTest`
的实时约束检查也可能失败——两者都是资源饥饿，不是产品缺陷，但排查成本很高。

### SITL 环境阻塞的判定与防掩盖边界

直连 MAVLink 的场景（03/04/05）在广播 ARM 前先比对各机 `SYS_STATUS` 的传感器 `present`
位：**参照系是机队自身的多数，不是固定的必需位清单**。个别实例比同伴少报传感器，说明
它的 Gazebo 传感器桥没起来，属环境问题——`strict_regression.py` 退出码 3，`run_all.py`
把该场景判 `BLOCKED`（整体仍退出 2），SUMMARY 用 `⛔` 标记。

用多数而不是写死清单，是因为 PX4 上报哪些位随版本和机型变化：本机 SITL 实测**从不**上报
`3D_ACCEL`，写死清单会把 8 架全健康的机队整体误判成阻塞（该退化已由
`test_bit_no_peer_reports_is_not_required` 守住）。

这条降级只在下列全部成立时生效，任何一条不满足都仍判 FAIL：

1. 只看 `present` 位，不看 `health`，更不看 `MAV_SYS_STATUS_PREARM_CHECK`。
   `present` 齐全而 `health` 无＝真实预检失败；用预检位做门等价于「解锁一定成功」，
   会给所有真实解锁缺陷发免罪符。
2. 必须有正向 `SYS_STATUS` 证据。在线但一条都没收到时维持改动前行为，不判阻塞。
3. 必须有对照。机队不足 2 架、或全体同样退化时没有多数可依，一律不判阻塞。
4. 只作用于直连 MAVLink 的 `Fleet.arm_all`。经 QGC 中介的场景另有一套更严的判据，见下节。
5. 只作用于 `arm_all` 一个动作，不是全场景兜底。
6. `cleanup_errors`（容器/端口残留）一票否决降级——那是 harness 自身缺陷。
7. `report.json` 保留逐机 `present/enabled/health` 位图、`expected_sensors` 和缺失传感器名，
   可离线复核。
8. `BLOCKED` 永远不等于 `PASS`；`--allow-blocked` 必须显式给出。

离线回归见 `sim/test_arm_readiness.py`（9 条，含「预检不过不得降级」「无证据不得阻塞」
「无对照不得阻塞」「同伴都不报的位不得要求」四条负例）。

### QGC 中介路径：只在能正面证明 QGC 正常时才降级

经 QGC 的场景等待就绪时，`_wait_qgc` 曾在任何原因下都空转满 `--ready-timeout`（默认
2400 秒）再判一个无法自证的 FAIL。2026-09-10 与 09-16 两轮验证各命中一次，每次吃掉约
40 分钟墙钟。

现在 `_minority_instance_stall()` 识别「QGC 正常、少数仿真实例掉队」这一签名：持续
`QGC_MINORITY_STALL_SEC`（180 秒）成立即判环境阻塞并立刻收口，不再空转。

判据与上节的传感器就绪门同源——参照系是机队自身的多数——但方向上更严：这条路径上
QGC 本身是被测对象，所以只有在能**正面证明 QGC 工作正常**时才降级。下列任何一条
不成立都仍判 FAIL：

1. QGC 在应答且 `ok`——它挂了或卡了根本取不到 status；
2. 上报机数达到预期——少报机是 QGC 的连接缺陷，必须暴露；
3. 每一架 `initial_connect_complete` 均为真——证明握手对全体有效；
4. 缺遥测的是**严格少数**——QGC 遥测管线若坏了会是全体缺失，不会只缺一架；
5. 机队至少两架——单机没有多数可参照，无法区分，不降级。

每条守卫在 `sim/test_qgc_readiness.py` 里都有对应反例（13 条）。移除少数派守卫会让
其中 3 条失败——没有反例的判别式不是判别式，是免罪机器。

### 场景执行覆盖契约

L5/L6 实际执行哪些场景由 `sim/scenarios/*.json` 的 `requires_qml` 推导，不写死在 `run_all.py`
里；`source.asset_contracts` 会断言这一点，并要求每个场景文件都归入 L5 或 L6。此前 L5 的场景
列表是字面量，导致新增的 09–12 长期只有 L2 dry-run 覆盖。Q1 的场景 03 × 20 轮是稳定性压测，
故意只取单一场景，不属于该契约。

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
