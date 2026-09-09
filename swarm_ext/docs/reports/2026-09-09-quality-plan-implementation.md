# 代码完善计划实施与验证记录（2026-09-09）

本轮针对 QA-01 至 QA-09、感知资源增长和围栏配置同步实施修复，范围为 QGC 集群扩展及其依赖的任务、固件和验收接口。
行为说明见[集群执行安全与资源边界](../安全执行与资源边界.md)。
实现提交为 `adee41030`，验收工具提交为 `fcfd730fd`。本轮功能回归通过，全仓库 lint 存在下述既有格式问题。

## 实施对应关系

| 问题 | 实施结果 | 主要验证 |
| --- | --- | --- |
| QA-01：权限收紧后仍执行排队自动动作 | 每次交错派发及固件后续动作重查许可、租约、绑定成员和当前策略；人工确认与自动受理分开处理 | `SwarmSafetyTest::policyRevocationMustStopQueuedAutomaticArm` 六组策略；SITL 10 |
| QA-02：排队起飞绕过新限高 | 派发前重查共享配置，撤销后不因恢复策略而自动续发 | `queuedTakeoffMustRecheckChangedAltitudeLimit` |
| QA-03：已有机载航线启动缺少完整校验 | 整批新鲜回读、规范化内容快照、内容与执行版本分离；所有成员通过航线/转场/返航/限高/通信/电量校验后才启动 | `missionSnapshotValidation`、`missionReplacementBlocksLaterStart`、SITL 11 |
| QA-04：忙碌时清空被误报成功 | 整批预检；飞控清空确认后回读空任务；失败、超时、成员退出按批次结束 | `clearingBusyMissionMustNotReportSuccess`、`clearMustVerifyEmptyReadback` |
| QA-05：终态通知重入清除新任务 | 清理、持久化、不可变结果快照先于通知；任务 UUID 和派发批次隔离 | 三种终态通知重入测试、`uploadNotificationMustNotCompleteNewReadback` |
| QA-06：非法坐标触发浮点转整数未定义行为 | 感知入口和 COP 模型分别检查有限值及经纬度范围，接受有效 `(0,0)` | 非法坐标、队列边界测试与 UBSan |
| QA-07：目标离开允许区没有告警 | 按允许区并集判定越界，保留禁飞区进入检测；边界语义与执行检查一致 | `targetLeavingIncludeFenceMustRaiseAlert`、`safety_resources` |
| QA-08：感知文档版本不符 | 服务说明统一到 v2 帧头、分片及来源隔离契约，说明实际传输验证范围 | 资产契约与感知协议测试 |
| QA-09：失败/中止被验收成成功 | 验收绑定 taskId、显式终态、成员集合和任务内派发证据；清理不能覆盖失败 | `test_task_verdict.py` 四项测试；SITL 08/09/12 |
| 感知资源增长 | 航迹 1024、来源 8192、每航迹贡献来源 64/10 秒、队列 8192、每轮 128 条或 2 毫秒、冷却记录 8192；失效状态清理与过载计数 | 连续 30000 来源更新、10000 条队列输入、满载保留已知更新、聚集候选索引与性能测试 |
| 围栏配置不一致 | 文件和父目录监听、版本化共享快照、下发前同步检查、停感知后仍每 500 毫秒刷新；无效配置保留旧图层并显示错误 | 原子替换、损坏、删除、恢复和消费者一致性测试 |

`start_mission` 对地形高度、跳转、未知命令、非连续任务及无法解析的返航采取拒绝策略。标准 QGC 人工任务工作流保留。
返航解析经 FirmwarePlugin 读取 Fact，支持 PX4 和 ArduCopter 的可验证直接返航设置；航点结束任务也检查保守返航储备路线。

## 验证环境与证据

起点提交为 `c9f2ac38e5f22513a984297e51bc7f7687e2c67f`。验证期间为未提交工作区，不能用起点提交代替被测源码版本。
本轮证据独立存放于 `test_results/2026-09-09-quality-completion/`，包含源文件 SHA-256 清单、构建报告、CTest 原始日志、场景报告和发布产物检查。
这些运行产物保留在本地，未纳入源码提交；此前审计证据未覆盖。

主环境为 Linux x86_64、`qgc-ubuntu-docker:latest`、Qt 6.11.1、GCC 13；PX4 SITL 使用本地配套源码与 Gazebo。
QGC 单元和集成回归使用 `.github/scripts/cmake_helper.py ctest` 的 CI 入口，排除 `Flaky|Network`，集成测试串行。
标准 Python 检查使用仓库 CI 指定的 Ruff 0.15.20。

| 验证项 | 结果与证据目录/文件 |
| --- | --- |
| Debug 增量构建 | 通过；`build-final/`；最终 CMake 格式整理后的二进制哈希与完整回归被测产物一致 |
| 标准检查 | 16/16 通过，含独立 C++ 11 个目标；`standard-final/` |
| 关键 QGC 集成 | MissionManager、SwarmExecution、SwarmRegression、SwarmSafety 4/4 通过；`batch-isolation-probe.log`、`batch-isolation-LastTest.log` |
| 最终完整 Unit/Integration | Unit 183/183、Integration 61/61 通过，包含 RC 参数与相关 QML UI 测试；`qgc-final/`、`qgc-final-LastTest.log` |
| UBSan | 独立 C++ 11/11 通过；启用 `undefined,float-cast-overflow` 和 `-fno-sanitize-recover=all`；`ubsan-ctest.log`、`ubsan-LastTest.log` |
| 1000 航迹性能 | 原有 30 帧测试 P50=11.641 ms、P95=12.334 ms、P99=106.728 ms，满足 P95≤200 ms；`resource-LastTest.log` |
| QML | 两个相关面板 0 错误；警告仍为基线 180 条（176 unqualified、4 unresolved-type），无新增；`qmllint-final.log` |
| SITL 01/02/05/08/09/11 | 6/6 通过；`sitl-final/` |
| 最终 SITL 10/12 | 2/2 通过；`sitl-faults-final/` |
| SITL 09 五次独立重复 | 5/5 通过，每次重启机群并使用独立 QGC 数据目录；`sitl-withdrawal-five/` |
| Release | 配置、编译、接口剔除检查均通过；`QGC_BUILD_TESTING=OFF`，验收环境变量、服务符号命中均为 0；`release-final/` |
| 修改文件静态检查 | 35 个 C++ 文件 clang-format、3 个 CMake 文件格式/语法、3 份 Markdown 规范，以及标准套件中的 Python、Shell 和 Vehicle 空指针检查通过 |
| 全仓库 `just lint` | 未通过完整门禁：依赖安装未完成；独立运行相同版本的全仓库 C++ 格式检查确认 1701 个文件中有 1216 个既有文件失败，本次修改文件失败数为 0；`clang-format-full.log`、`lint-summary.json` |

全仓库检查在隔离工作树中运行，以免自动修改用户已有文档。为工具安装补齐了 Go 和 Node，仍遇到工具链下载与依赖连接问题。
在确认全局格式门禁已因未修改文件失败后，停止未完成的依赖安装；未执行完的 TruffleHog 等完整钩子不能计为通过。
本轮没有扩大范围重排 1216 个既有文件。可先单独治理仓库格式基线和工具依赖，再重新运行 `just lint`。

被测 Debug SHA-256：`02ca7f1e0f831941b4b70d7a9d7e817e2fc3c935a8d71250382743db4cf99f3b`。
Release SHA-256：`e44a1780d237cca24696ef445610b7f2d261452531b10f0c7dbf7d3ceb3d5f02`。
逐文件清单见本地 `source-manifest.json`，产物清单见 `artifact-manifest.json`。

第一次全量集成回归暴露了无头环境压力传感器日志和单调时钟测试的异步通知等待问题。
测试仅声明缺失后端的特定日志，并在推进模拟时钟前交付排队的执行完成通知；原业务断言保留。
MockLink 起飞自身可解锁，重试测试改为核验起飞请求及实际 armed 状态，避免依赖额外 ARM 包的心跳时序。

新增 SITL 11 首次失败源于验收脚本使用固定地图中心；软件当时在规划阶段正确拒绝任务。
脚本改用当前机队位置后重新运行，确认上传成功后收紧限高会阻止启动，且两机没有收到解锁或启动请求。

## 结论边界

本轮证明所列回归场景和资源边界在上述环境下成立。CTest 目标通过不代表所有内部用例均执行；
最终集成日志保留 20 个内部跳过项，涉及视频平台能力、蓝牙硬件、日志后端和 ArduSub MockLink 参数等既有边界。
`Flaky|Network` 标签不在本轮完整回归范围内。

尚未覆盖 ArduPilot 实飞动力学、真实多机硬件、其他操作系统和 8 小时连续运行。
通信与电量校验使用现有估算模型；静态路线检查不等于实时避障或碰撞规避保证。
外部飞控任务变更只能依据协议中可观测的内容、事务与任务标识检测；不能保证识别尚未上报的远端替换。
已送达飞控的指令可能仍有 MAVLink 协议重试，软件撤销后续业务派发不等于撤销已经生效的飞行操作。
