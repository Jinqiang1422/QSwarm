# 全量功能验证与验证体系修复记录（2026-09-10）

起点提交 `0bad14926`（工作树含未提交文档改动）。本轮先对全部功能域执行 full profile 验证，
再修复验证体系本身暴露的 6 个问题。修复提交见分支 `fix/verification-harness-gates`。

## 一、验证环境

| 项 | 值 |
| --- | --- |
| 构建 | Qt 6.11.1 容器 `qgc-ubuntu-docker:latest`（`sha256:b7385fd6`），GCC 13 |
| SITL | 本地 `px4_sitl_default` + Gazebo，PX4 `85df8c2281` |
| 宿主 | Linux 5.13.0-30，40 核，Python 3.8.18，CMake 3.26.4，GStreamer 1.16.3 |
| 入口 | `swarm_ext/tests/run_all.py --profile full --sitl-backend local --qgc-mode managed-gui` |
| 证据 | `test_results/2026-09-10-full-feature-test/`（396 MB） |

## 二、修复前的全量结果

full profile 22 项：**PASS 20 / FAIL 2 / BLOCKED 0 / SKIP 0**。

| 层级 | 结果 |
| --- | --- |
| L0 源码契约（8 项） | 7 PASS、1 FAIL（`source.ruff`，实为工具版本错配，见 F1） |
| L1 Python/算法（4 项） | 全 PASS。感知 22/22、协议 5/5、几何与 COP 真值 15/15、LLM 数据集 559 条一致 |
| L2 场景解析 | PASS，12/12 场景动作解析与报告编排 |
| L3 独立 C++ | PASS，11/11（P1/P2/P3、战术、管线、租约、持久化、COP、性能、协议、安全资源） |
| L4 QGC 集成 | 全 PASS。**Unit 186/186**、**Integration 61/61**（含 SwarmExecution/Regression/Safety）、生命周期 SIGINT/SIGTERM 与 Secret 脱敏 |
| L5 SITL 验收 | FAIL，7 个场景中 6 个通过；场景 05 失败，原因见 F3 |
| L6 GUI/QML 验收 | PASS |

补充执行（full profile 之外）：

- **Q2 发布资格 3/3 PASS** —— Release 产物不含验收服务符号、环境开关和端口入口。
- **场景 09–12 首次真机执行 4/4 PASS** —— 这四个场景此前在任何 profile 中都不会运行，详见 F5。

### 实测指标

| 指标 | 实测 | 判据 |
| --- | --- | --- |
| 一键暂停时延（8 机） | max 16.4 / 16.8 / 17.2 ms（三次） | ≤1000 ms |
| 感知端到端 | 150 帧 → 355 检测 → 148 航迹报文，心跳 1.0005 Hz，wire latency 2.76 ms | ≤500 ms |
| COP 航迹地理误差 | 达标，渲染 2 条 confirmed 航迹，`qml_ready=true`，首个 delegate 1206 ms | ≤8 m |
| 控制租约 | `leased` → `manual_override` → `observe_only` → 重新 `leased` 全链路正确 | — |
| 8 机 SITL | 区域搜索 168 s、编队巡航 183 s、单机 kill 141 s、链路冻结/恢复 137 s | 各场景 `max_total_sec` |

**产品代码本身没有查出缺陷。** 下述 6 项全部属于验证体系。

## 三、验证体系的 6 个问题与修复

### F1 L0 lint 门禁只查工具存在、不查版本

| 工具 | 宿主 | `.pre-commit-config.yaml` 固定 | 后果 |
| --- | --- | --- | --- |
| ruff | 0.9.0 | v0.15.20 | **假失败** —— 6 条 `UP038`，该规则新版已移除 |
| shellcheck | 0.7.0 | v0.11.0.1 | **假通过** —— 缺 0.8–0.11 新增的全部检查 |
| clang-format | 22.1.5 | v22.1.5 | 恰好一致 |

假通过比假失败更危险，因此四项门禁一并加版本门。新增
`tools/common/tool_version.pre_commit_hook_version()`（正则解析，保持零第三方依赖）与
`run_all.py` 的 `_pinned_tool()` prerequisite。不一致判 **BLOCKED**（退出码 2），既不判 PASS
也不判 FAIL；逐项宿主/固定版本与比对结果记入 `environment.json` → `tool_pins`。

比较语义是较短一方的前缀相等，不是「不低于」：ruff 在 0.9→0.15 之间既移除又新增了规则，
两个方向都会失真，且 `ruff format --check` 是逐字节比对。

**装上固定版本后复验：4/4 PASS，退出码 0**（`shellcheck 0.11.0` 未发现新问题）。

### F2 证据目录既未 gitignore 也未 lint 排除

`test_results/`（83 个子目录、0 个跟踪文件）与 `profile_output/` 长期以 `??` 出现在
`git status`；全仓 `ruff check` 的 29 条命中**全部**来自 `test_results/` 里的一次性取证脚本。
按 `.gitignore` 中 `Testing/` 的同一理由加锚定忽略，并在 `ruff.toml` 的 `exclude` 补两项
（`--no-respect-gitignore` 时仍成立）。

### F3 直连 MAVLink 的 `arm_all` 没有预检就绪门

场景 05 三次运行失败两次，两次都是某一架 PX4 全程收不到 Gazebo 传感器数据
（`Accel/Gyro/Compass Sensor 0 missing`，另一次 `barometer 0 missing`），解锁被拒 60 s 后判失败。
QGC 侧行为正确（识别拒绝 ACK、重试、明确报失败），但 harness 把仿真器故障报成了软件失败。

修复：`Fleet.remember()` 缓存 `SYS_STATUS`，广播 ARM 前比对传感器 `present` 位；
`strict_regression.py` 对此类运行退出码 3，`run_all.py` 经 `TestSpec.blocked_exit_codes` 判 BLOCKED。

**判据以机队多数为参照，不是固定的必需位清单。** 第一版用固定清单，实测把 8 架全健康的机队
整体误判成阻塞——PX4 在本机 SITL 上**从不**上报 `3D_ACCEL` 的 present 位（全机 `present=0x220802d`）。
多数参照是自校准的，且正好刻画要抓的现象：个别实例比同伴少报传感器。

防掩盖边界（各有单测覆盖，共 9 条）：只看 `present` 不看 `health` 更不看
`MAV_SYS_STATUS_PREARM_CHECK`（PX4 用 `can_arm_mode_flags` 填该位，用它做门等价于「解锁一定成功」）；
无正向证据不阻塞；无对照（单机或全体同样退化）不阻塞；只作用于直连路径的 `arm_all` 一个动作；
`cleanup_errors` 一票否决降级；`BLOCKED` 永不等于 `PASS`。

**验证**：场景 05 ×3 真机 SITL **3/3 PASS，退出码 0**（99.2/99.1/98.2 s，解锁门每次 11.1 s 通过，
与基线一致；暂停时延 15.5–16.6 ms）。用先前三轮采集的真实位图离线回放，新判据只阻塞传感器桥
确实故障的那一架，两个健康机队均通过。

### F4 解锁重试窗口不一致

01/02 在 JSON 里写 `retry_timeout_sec: 180`，03/04/05 落到默认值，而默认值 `60` 在
`run_scenario.py` 里有三份可漂移的副本。统一为常量 `ARM_RETRY_TIMEOUT_SEC = 180`，删除两处
JSON 覆写，并在资产契约中拒绝非正数的 `retry_timeout_sec`。

**不动 `max_total_sec`**：它今天就低于最坏路径（03 预算 400 s、最坏 423 s），但 F3 的就绪门已给
解锁阶段封顶——要么秒级完成，要么 45 s 判 BLOCKED 并停止场景。180 s 窗口只可能被真实的慢启动
吃满，那本就该计入总时长。将来若实测超标，正确修法是带证据提高该场景的 `max_total_sec`。

### F5 12 个场景中有 4 个从不执行

`run_all.py` 把 L5 场景列表写死为 `01,02,03,04,05,07,08`。09–12（撤离后不再派发、动作排队中
收紧 L3 策略、任务上传后收紧高度上限、任务超时显式判失败）只有 L2 dry-run 覆盖——那不是执行证据。
改为按各场景的 `requires_qml` 推导分层。

**本轮首次真机执行 4/4 PASS**：140.0 s / 86.3 s / 78.3 s / 80.2 s，机队仅 1–2 架。

### F6 没有契约阻止 F5 那类漂移

`validate_assets.py` 新增 `validate_scenario_coverage()`，断言三件事：两个 TestSpec 的 `--only`
不得退回字面量（AST 检查）、常量与从 `scenarios/*.json` 独立重推导的结果一致、场景编号是唯一的
两位数字前缀（`--only` 按前缀匹配，`1` 会同时命中 10/11/12）。三条负例均已实测触发。

不引入 `harness_tier` 之类新字段：`requires_qml` 已完美区分两层，新字段等于用一个新的同步面
去修一个同步面。

## 四、修复后的验证结果

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| standard profile（16 项，固定版工具） | **16/16 PASS，退出码 0** | `standard/SUMMARY.md` |
| L0 lint 四项（固定版工具） | 4/4 PASS，退出码 0 | 同上 |
| L0 lint 四项（宿主旧版工具） | 3 BLOCKED + 1 PASS，退出码 2 | 见 F1，符合设计 |
| 解锁就绪门单测 | 9/9 PASS | `sim/test_arm_readiness.py` |
| `tools/` 版本解析与漂移守卫 | 33 passed / 1 skipped | `tools/tests/`（skip 项因宿主无 `uv`） |
| 场景 05 ×3 真机 SITL | 3/3 PASS，退出码 0 | 见 F3 |
| 场景 09–12 真机 SITL | 4/4 PASS | 见 F5 |
| 资产契约（含三条负例） | PASS | `asset-contracts.json` |

## 五、剩余问题与验证边界

### 未能完成修复后的 full profile 端到端复跑

两次尝试都在 8 机场景上被同一类仿真器故障打断：第 8 个 Gazebo 实例的传感器桥没起来
（一次 `Compass Sensor 0 missing`、一次无相对高度）。本日统计，**8 机启动约有 30–40% 概率
出现某一架实例传感器桥失效**；1–2 机场景（09–12）与复跑的场景 05 均未复现。

### 新发现：`_wait_qgc` 对同一故障会空转 40 分钟（未修）

F3 的降级只覆盖直连 MAVLink 路径。同一仿真器故障落在经 QGC 的场景上时，会卡在
`strict_regression._wait_qgc` 的 `--ready-timeout` 默认 2400 s 里空转，最后仍是一个普通 FAIL。
这是既有 harness 弱点，本轮按计划范围没有处理——该路径上 QGC 本身是被测对象，
把「QGC 一直没报就绪」降级成环境阻塞会给 QGC 的连接/就绪缺陷发免罪符，需要独立设计。
**建议作为下一轮的首要项。**

### 本轮未覆盖

- **L7 外部模型评测**（559 条自然语言指令）：需模型 API Key，未运行。
  **不得沿用历史 88% 意图准确率作为当前质量保证。**
- **Q1 稳定性资格**：Unit/Integration ×50 轮、生命周期 ×20、场景 03 ×20，数小时，未跑。
- 真机、半实物、30 分钟持续飞行、8 小时浸泡、16/32 机容量、真实视频与在线模型、网络整形、
  跨平台与硬件在环 —— 均属现场验收层，SITL 结果不能替代。

### 其他

- 本轮**不触碰任何 C++**，`src/` 下未改一行；`just build` 与 `ctest -L Unit` 属形式性确认。
- 宿主 `.ruff_cache/0.15.20` 曾被容器以 root 属主创建，会让固定版 ruff 报「Permission denied」
  而退出码 2。已清理；这是本机环境问题，不是代码问题。
- 全仓库 lint 的其余既有失败项（Dockerfile、Actions、Clang-Tidy/Clazy/QML 分析入口缺工具）
  与本轮无关，未处理。
