# 交付收尾验证记录（2026-09-16）

基线提交 `0d2ae5586`（合并 `feat/swarm-task-authoring-and-dispatch-lock`）。
本轮是该分支合并进 `main` 后的收尾验证：在交付形态上跑完整 full profile，
并对发布制品本身做一次审计。

## 一、结论

**产品代码未查出缺陷。** full profile 22 项在修正一次由本轮操作引入的环境错误后全部通过；
SITL 11 个场景全部通过，但不在同一次运行内——场景 01、02 命中已知的 Gazebo 传感器桥
间歇失效，复跑通过（见第五节）。

交付形态的核心约束已在真实发布制品上验证：`QGC_SWARM_AI_AUTODISPATCH=OFF` 的 Release
产物中，AI 自动下发不可用，验收接口符号为零。

## 二、验证环境

| 项 | 值 |
| --- | --- |
| 基线 | `0d2ae5586`，工作树干净（仅 `.understand-anything/`、`test.md` 未跟踪） |
| 构建 | Qt 6.11.1 容器 `qgc-ubuntu-docker:latest`（`sha256:b7385fd646288`），GCC 13 |
| SITL | 本地 `px4_sitl_default` + Gazebo，PX4 `85df8c2281` |
| 宿主 | Linux 5.13.0-30，40 核，Python 3.8.18，CMake 3.26.4 |
| lint 固定版 | ruff 0.15.20、shellcheck 0.11.0、clang-format 22.1.5（均与 `.pre-commit-config.yaml` 一致） |
| 入口 | `run_all.py --profile full --sitl-backend local --qgc-mode managed-gui --qgc-token-file <token>` |
| 证据 | `test_results/2026-09-16-delivery-closure{,-rerun,-sitl0102}/` |

## 三、本轮交付内容

相对上一轮验证基线 `0bad14926`：49 个非合并提交，其中 13 个 `feat`/`fix`/`perf`。

**Part A —— 待确认任务可逐项编辑。** 大模型生成的多阶段任务以结构化模型呈现，
操作员可在确认前逐阶段编辑动作参数与完成条件。人工与模型产出的任务仍共用
`SwarmTaskController::parseStages` 一套表达，纯人工／纯大模型／人工+大模型三种编排模式均可用。

**Part B —— 交付构建锁死人工确认。** 构建选项 `QGC_SWARM_AI_AUTODISPATCH`（默认跟随
`QGC_BUILD_TESTING`）把交付版钉在 L2 监督执行，且每个 AI 动作必须人工确认。
**钉 L2 而非 L1** 是本次的关键设计决定：L1 会连带杀死任务编排（任务态机以 `origin=="task"`
下发，会被 `adee41030` 的后备闸拒绝），而钉 L2 保住了编排、保住了后备闸、也保住了 L1
的紧急停止角色，且未改动任何一行门禁代码。

同期修复的既有缺陷：面板溢出导致确认界面的应用按钮被裁出可点击区域（两个编辑器均中招）、
`SwarmSafetyTest` 约 1/4 概率的偶发失败、参数编辑可静默改掉动作类型。

## 四、分层验证结果

full profile 22 项：**PASS 22 / FAIL 0 / BLOCKED 0**（含下述修正后复跑）。

| 层级 | 结果 |
| --- | --- |
| L0 源码契约（8 项） | 全 PASS。**首次在固定版工具下取得可信判定**（详见第五节） |
| L1 Python/算法（4 项） | 全 PASS。感知编解码/几何/跟踪、协议 v2 分片与跨语言字节契约、LLM 数据集 559 条一致、解锁就绪门判定 |
| L2 场景解析 | PASS，12/12 场景动作解析与报告编排 |
| L3 独立 C++ | PASS，**11/11** |
| L4 QGC 集成（4 项） | 全 PASS。**Unit 186/186**、**Integration 63/63**（CI 口径，排除 Flaky/Network）、生命周期 SIGINT/SIGTERM 与 Secret 脱敏 |
| L5 SITL 验收 | 11 个场景全部 PASS（非同一次运行，见第五节） |
| L6 GUI/QML 验收 | PASS，场景 06 感知链路 35.7 s |

直接以 ctest 全量口径复核（不排除标签）：Unit **187/187**、Integration **65/65**。
锁定构建 `SwarmLocked` 标签通过。

### 发布制品审计

对 `QGC_BUILD_TESTING=OFF` 的 Release 产物（本轮以分支代码重建）执行
`audit_release_artifact.py`：

```
PASS  testing=OFF  markers=0  symbols=0  autodispatch=OFF(locked=True)  required_markers=1
```

`markers=0`/`symbols=0` 表示验收服务的环境开关、端口入口与符号均不在交付产物中；
`required_markers=1` 表示条件编译的交付告示串存在，即锁确实生效。
这是 Part B 首次在真实 Release 制品上得到验证——此前只验证过 Debug 锁定构建。

## 五、本轮发现

### 发现一：F1 版本门引入了一个新的踩坑面（本轮已被踩中）

F1 要求宿主提供与 `.pre-commit-config.yaml` 一致的 lint 工具版本，否则判 BLOCKED。
满足它最自然的做法——建一个 venv 装固定版工具并前置 `PATH`——会把 venv 的 `python3`
一并顶到前面，**静默破坏所有依赖系统 Python 包的 spec**。

本轮首次运行即因此产生两个假失败：`python.perception` 在 0.09 s 崩于 `import numpy`，
SITL 场景 03/04/05 在 0.0 s 崩于 `import pymavlink`。修正方式是只软链需要的二进制而非
整个 venv 的 `bin` 目录；修正后 `python.perception` 与 03/04/05 全部通过。

门禁本身工作正常，问题在于它把使用者推向一个会造成假失败的操作。**应在
`swarm_ext/tests/README.md` 中写明推荐的工具提供方式。**

### 发现二：QGC 中介路径上的环境故障仍会产出无法自证的 FAIL（已知，未修，本轮再次命中）

场景 01、02 在一次运行中 FAIL：01 在 `_wait_qgc` 空转满 2400 s（末次状态显示 8 架
全部 `all_sensors_healthy: False`、`ready_to_fly: False`），02 的 `arm_all` 吃满 180 s
重试窗口后失败并连锁。同样的代码在本轮首次运行与事后复跑中两个场景均 PASS
（218.0 s / 180.9 s），因此不是回归。

F3 的传感器就绪门只覆盖直连 MAVLink 的 `Fleet.arm_all`；`QGCFleet.arm_all` 与 `_wait_qgc`
**故意没有**同等降级，因为那条路径上 QGC 本身是被测对象，把「QGC 一直没报就绪」
判成环境阻塞等于给 QGC 的连接/就绪缺陷发免罪符。这个不对称是设计选择而非遗漏。

代价是一次环境故障吃掉约 40 分钟墙钟时间，并留下一个人工才能判读的 FAIL。
2026-09-10 报告已将其列为下一轮首要项，本轮再次命中，**建议维持该优先级**。

## 六、实测指标

| 指标 | 实测 | 判据 |
| --- | --- | --- |
| 一键暂停时延（8 机） | 逐机 5.9–16.5 ms，**max 16.5 ms** | ≤1000 ms |
| 感知链路（L6） | 场景 06 通过，心跳 1 Hz | ≤500 ms |
| 8 机 SITL 耗时 | 区域搜索 218.0 s、编队巡航 180.9 s、单机 kill 135.4 s、链路冻结/恢复 136.2 s、一键中止 99.3 s | 各场景 `max_total_sec` |
| 1–2 机安全语义 | 局部退出 141.4 s、权限收紧 84.9 s、限高变化 74.9 s、超时判失败 78.3 s | 同上 |

## 七、交付边界（明确未覆盖）

以下各项在本轮及此前任何一轮中都**没有**证据，交付时必须如实声明：

- **真实硬件**：全部验证均在 SITL/仿真中完成，零真机、零半实物、零长航时。
- **模型意图准确率**：当前版本从未测量。历史 88% 的数字对应旧版本，不得引用。
  L7 外部模型评测（559 条自然语言指令）需模型 API Key，本轮未运行。
- **Q1 稳定性资格**：50 轮 × 2 + 20 轮 × 3 的重复性压测未执行。
- **规模**：仅验证至 8 机；16–32 机未验证。
- **真实视频与真实模型**：感知链路以合成数据验证，未接入真实视频流与真实检测模型。
- **跨平台**：仅 Linux 容器；Windows/macOS/Android 未验证。

此外，设计文档 §12 记录的已知边界仍然成立：`abort_swarm_task` 未纳入编辑门控、
`set_selection` 动作不可编辑、阶段不可增删。

## 八、建议的下一轮首要项

1. **`_wait_qgc` 的 2400 s 空转**（发现二）——需要独立设计，不能简单降级。
2. **README 补充 lint 工具提供方式**（发现一）——低成本，可立即消除一类假失败。
3. **当前版本的模型意图准确率测量**——这是交付判断中唯一仍完全空白的功能维度。
