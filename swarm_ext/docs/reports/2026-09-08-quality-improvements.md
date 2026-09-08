# 质量改进实施与验证记录

本轮落实 2026-09-08 软件质量审查的代码完善计划。原始审查基线为
`e3226eccb1ae52bc0ce9cec0931c564464376307`。原审查报告、故障探针及此前文档改动保留。

## 已实现的行为

| 编号 | 原问题 | 修复与回归证据 |
| --- | --- | --- |
| Q01 | 暂停后错峰解锁仍继续 | 逐机指令代数、执行上下文和下发前权限检查；测试直接暂停、确认动作暂停及迟到起飞 ACK 均不产生新的解锁 |
| Q02 | 中止任务后编队仍活动 | 中止先撤销任务成员的自动动作及编队引导，再发送暂停；局部退出保留其他成员任务；测试长机接替与剩余一机停止引导 |
| Q03 | PX4 上传移除了起飞项 | 使用标准 Home 占位项，让 FirmwarePlugin 决定是否发送；PX4/APM MockLink 上传回读均保留 TAKEOFF |
| Q04 | 上传未完成便启动任务 | 上传确认后回读核验指令、坐标及参数；失败不启动；同序列及后续阶段启动前检查已核验任务版本 |
| Q05 | 未启动或退出 Mission 被误判完成 | 完成须绑定当前执行并观察 ACTIVE→COMPLETE；未知状态采用带执行令牌的人工确认；暂停、任务缺失及版本改变阻止确认 |
| Q06 | 分片缓冲无界增长 | 每消息 64 KiB、最多 64 个上下文、每来源最多 8 个；固定 TTL 和每轮处理预算；20,000 帧异常输入验证上限 |
| Q07 | 不同来源和消息混片 | v2 引入会话、消息编号、片号、总长度和 CRC；C++/Python 共享固定字节样本，并验证乱序、缺片和冲突重复片 |
| Q08 | 心跳到期不通知界面、停止后统计残留 | 独立在线状态通知、100 ms 到期检查、停止时清空桥接状态；真实 QML 绑定测试验证心跳到期 |
| Q09 | 接力模板空选择与固定分组 | 按实际任务成员生成默认分组并恢复全集；九套模板统一运行时校验；旧模板升级备份与自定义保留测试 |
| Q10 | 非数字被转成 0 或静默截断 | 完整数值解析、有限值及范围校验、未知参数和重复目标拒绝；QML 保留原输入；持续时间与超时联合检查 |
| Q11 | 无活动车辆时 RC 参数映射崩溃 | QPointer 绑定真实 Vehicle/Fact，切换或销毁时失效；提交前再次检查归属、连接可用状态及数值关系 |

还处理了任务所有权与后续分组授权、当前分组全部退出后的自动推进、taskFinished 发出前的取消与终态保存、
异步 PX4/APM 起飞与任务启动、机载任务版本变化、单调时钟、旧融合量测拒绝，以及保存失败反馈。
逐机动作结果和航线核验状态已接入集群面板。

本轮保留“选中成员暂停/返航/降落时退出，其他成员继续整个多阶段任务”的操作语义。
当前成员全部退出后，仍有其他任务成员时继续后续分组。新任务需要明确启动，退出成员不会自动重新加入。

## 正式测试入口

- `test/Swarm/SwarmRegressionTest.cc`：取消、上传、完成判据、局部退出、后续分组、
  长机交接、九套模板、迁移、自定义保留及单调时钟。
- `test/QmlControls/RCToParamDialogControllerTest.cc`：失效绑定与活动车辆切换。
- `test/MissionManager/MissionManagerTest.cc`：保留原有传输信号契约及故障处理回归。
- `test/QmlUITests/SwarmSituationLayerUITest.cc`：使用新量测时间戳验证态势显示。
- `swarm_ext/tests/cpp/tst_protocol.cc`、`tst_cop.cc` 及 Python `test_protocol.py`：
  协议边界、共享字节样本、资源上限与在线状态通知。
- 新增 `swarm_ext/sim/scenarios/09_partial_withdrawal.json`：两机 PX4 中一机退出、另一机继续返航阶段。

没有修改原始质量审查的故障探针。新增用例进入正式 CMake/CTest 或 Python 测试入口。

## 验证环境与结果

最终结果见 [验证清单](2026-09-08-quality-validation.json)以及工作区 `test_results/2026-09-08-improvement/`。
构建使用现有 Qt 6.11.1 Debug 容器路径与增量 CMake 命令；Unit/Integration 通过
`.github/scripts/cmake_helper.py ctest` 执行，与 CI 标签和排除规则一致。

| 检查 | 最终结果 | 工作区证据 |
| --- | --- | --- |
| 增量构建 | PASS | `build-release-candidate/SUMMARY.md` |
| Swarm standard | 16/16 PASS，含 10 个独立 C++ 程序、22 个感知测试和 5 个协议测试 | `standard-verified/SUMMARY.md` |
| 新场景契约与 dry-run | 2/2 PASS | `scenario-closure/SUMMARY.md` |
| QGC Unit | 183/183 PASS | `final-acceptance/SUMMARY.md` |
| QGC Integration | 60/60 PASS | `final-acceptance/SUMMARY.md` |
| PX4 SITL | 5/5 PASS：01、02、05、08、09 | `sitl-release-candidate/SUMMARY.md` |
| 本次改动的静态检查 | 13/13 PASS，含 CI 固定版本 Ruff 0.15.20 | `quality-gates/changed-checks.log` |
| 完整 just lint | 未完成：Go 工具链阻塞 | `quality-gates/full-lint-environment-failure.log` |
| 实际 qmllint | 仍有告警：基线 153 条，当前 151 条 | `quality-gates/qml-check.log` |

表中路径均相对于 `test_results/2026-09-08-improvement/`。通过数按 CTest 测试程序统计；
Integration 内另有 20 个因平台或测试环境条件跳过的 QtTest 用例，不能计为已验证功能。
SwarmRegressionTest 的 19 个 PASS 包含初始化和清理；实际用例名称保存于验证清单。

本轮测试使用提交前构建的二进制，所有改动源文件的 SHA-256 已与代码提交
`40f3e0d901db19c4271aaa8d7fa5323e3370236a` 核对一致。二进制摘要和逐文件摘要保存在验证清单。

```bash
python3 swarm_ext/tests/run_all.py --profile standard --jobs 4
python3 swarm_ext/tests/run_all.py --profile full --jobs 4 \
  --only qgc.build,qgc.ctest_unit,qgc.ctest_integration
```

SITL 使用 `strict_regression.py --sitl-backend local --qgc-mode managed-headless`
和 `--only 01,02,05,08,09`；每个场景使用独立 PX4/Gazebo 机群与 QGC 数据目录。
验收令牌通过临时权限受限文件传递，未写入报告。


## 验证过程中修正的问题

完整回归发现新增的通用任务状态通知破坏了原有“只发一个传输状态信号”的契约；
该通知没有使用方，已移除。原态势界面测试复用了同一时间戳，现改为两条新量测，
保留真实重复数据拒绝规则。AI 确认恢复动作时也已避免把正在确认的计划误报为放弃。

一次早期 Unit 执行与增量链接重叠，产生可执行文件暂时不存在的 Not Run 结果。
该轮证据保留；之后严格等待构建完成，再重跑全部 Unit。不同 Python/Ruff 环境下的
依赖缺失和既有静态告警也保留在中间报告中，未作为最终通过证据。

## 发布边界

- 完整 `just lint` 在隔离工作树中执行，因主机 Go 版本不能构建固定版本 TruffleHog 而失败。
  本次改动相关的 13 个可运行检查均通过；完整 lint 不能标记为通过。
- Qt 的实际 qmllint 已运行。两处页面仍有既有的未限定属性访问等告警；
  不把构建成功当作所有 QML 静态告警清零。
- 本轮执行 Linux/Qt、MockLink 与 PX4 SITL；ArduPilot 真实飞行动力学 SITL、真机、
  其他桌面/移动平台及长时间持续运行验证未完成。
- 感知协议 v2 是一次明确的兼容性变更，QGC 与 Python 服务须同时升级。
  CRC 与来源白名单不提供身份认证。4 MiB 是重组 JSON 缓冲上限，不是整个进程 RSS 上限。
- 飞控已经接收的任务及命令无法撤回。动作确认、航线回读一致及任务完成是不同状态；
  不用测试数量替代真机发布判定，也不声称软件不存在其他缺陷。

操作变化见 [质量改进使用说明](../QUALITY_UPDATE_USAGE.md)，
部署契约见 [感知协议 v2](../PERCEPTION_PROTOCOL_V2.md)。
