# PX4 SITL 验证与人工/模型任务等价性（2026-09-24）

基线提交 `0d664d1ff`。09-20 那轮的 SITL 因本机另一项目占用 PX4 实例号锁未能执行，
本轮补齐当前 HEAD 的 SITL 执行证据，并补上一条此前缺失的核心契约测试。

## 一、SITL 结果：PASS 10 / FAIL 0 / BLOCKED 1

| 场景 | 结果 | 耗时(s) |
| --- | --- | --- |
| 01 区域搜索全流程（8 机） | ✅ PASS | 219.8 |
| 02 编队巡航（8 机，楔形） | ✅ PASS | 239.7 |
| 03 单机故障注入（8 机，任务中 kill 1 机） | ✅ PASS | 139.6 |
| 04 链路退化注入（8 机，冻结/恢复 1 机链路） | ✅ PASS | 156.5 |
| 05 一键中止时延实测（8 机） | ⛔ BLOCKED | 80.9 |
| 07 QGC 控制租约与人工接管（8 机） | ✅ PASS | 104.1 |
| 08 QGC 多阶段任务人工门控与中止 | ✅ PASS | 1.3 |
| 09 两机任务局部退出与后续阶段 | ✅ PASS | 143.2 |
| 10 L3 自动动作排队期间收紧权限 | ✅ PASS | 86.4 |
| 11 航线上传后收紧限高 | ✅ PASS | 16.8 |
| 12 任务超时明确报告失败 | ✅ PASS | 19.1 |
| 06 感知链路馈送（L6 GUI/QML） | ✅ PASS | 36.4 |

**零 FAIL。** 这是本项目 SITL 首次在一次运行内不出现任何需要人工判读的失败。

### 唯一的非通过是一次被正确分类的环境阻塞

```
环境阻塞：sysid 6 的 SYS_STATUS 未报告必需传感器 present（Gazebo 传感器桥未启动），
非 QGC 解锁缺陷
```

F3 的传感器就绪门以机队自身多数为参照，精确指认了掉队的那一架，并在广播 ARM
**之前**拦下——一条解锁命令都没有发出，因此不会掩盖任何真实解锁缺陷。
`report.json` 记 `environment_block: true`、`reason: preflight_sensors_missing`。

**这是该机制在真实运行中第一次生效。** 同样的现象在 09-10 与 09-16 两轮里都表现为
无法自证的 FAIL，各吃掉约 40 分钟墙钟。

### 启动前预检也在本轮首次生效

跑前 `/tmp` 残留 5 个孤儿 PX4 锁（上一轮被中断遗留）。新预检确认无 px4 进程后自动清理：

```
[launch] 清理上一轮遗留的 5 个孤儿 PX4 锁（当前无 px4 进程）
```

无需人工介入。若锁被活进程持有，同一预检会改判环境阻塞并拒绝删除——见
[09-20 报告](2026-09-20-full-verification.md)第三节。

## 二、补上核心契约测试：人工与模型任务必须完全等价

「人工操作与大模型操作共用一套任务格式」是本项目的核心主张。架构上由三个来源
——`SwarmTaskLibrary`（人工模板）、`SwarmAIAgent`（模型）、`SwarmAcceptanceServer`
（验收）——都经 `SwarmTaskController::parseStages` 保证。

**但此前没有任何测试断言过等价性。** 现有测试只各自直接调 `parseStages` 验解析器。
若哪天模型路径长出自己的一套校验、默认值或超时改写，现有测试一条都不会红。

新增 `SwarmTaskEditTest::humanAndModelAuthoredTasksMustStartIdentically`：同一份
stages JSON 分别经

- **人工路径**：`parseStages` → `startTask`（Plan 视图与模板库走这条）
- **模型路径**：`start_swarm_task` 工具 → 人工确认 → `confirmPlan` → `startTask`

启动后逐字段比对阶段数、阶段名与逐阶段 `timeout_sec`。两个超时刻意取非默认值
641/642：任一路径自行补默认值或改写，断言即失败，而只比较阶段数和名称是发现不了的。

### 区分度已验证

在模型路径注入一处分歧（把 `timeout_sec` 统一改写为 999）后重建：

```
FAIL! : humanAndModelAuthoredTasksMustStartIdentically() Compared values are not the same
FAIL! : pendingTaskModelMustExposeStagesActionsAndCondition() Compared values are not the same
FAIL! : typoKeyEditMustReportNoChange() Compared lists differ at index 0.
```

新测试抓到了，另两条既有测试也一并抓到——说明这类分歧本就应被多处拦住。还原后全通过。

## 三、验证汇总

| 项 | 结果 |
| --- | --- |
| SITL（当前 HEAD） | 10 PASS / 0 FAIL / 1 BLOCKED，L6 PASS |
| 一键暂停时延（8 机） | 场景 05 因环境阻塞未取得本轮数据；最近实测 max 16.5 ms（09-16） |
| `SwarmTaskEditTest` | 14/14（含新增等价性用例） |

## 四、目标对照

| 目标要素 | 状态 |
| --- | --- |
| 基于 QGroundControl | ✅ fork，上游 Unit/Integration 全通过 |
| 保持官方 QGC 功能 | ✅ 上游测试无一条被修改或跳过 |
| 集群管理 | ✅ 多机批量指挥、编队、区域覆盖、控制租约 |
| 基于大模型的任务管理与规划 | ✅ 18 个工具、多阶段任务编排、候选方案 |
| 人工与模型共用一套任务格式 | ✅ 架构由 `parseStages` 统一，**本轮补上等价性测试** |
| 统一下发给 PX4 固件无人机 | ✅ 经 `SwarmManager::executeActions` 单一管线 |
| PX4 SITL 仿真验证 | ✅ 本轮 10/11 PASS + 1 环境阻塞，0 FAIL |

交付边界（真实硬件、模型意图准确率、Q1 稳定性、16–32 机、真实视频与模型、跨平台）
未变，见[交付收尾验证记录](2026-09-16-delivery-closure.md)第七节。
