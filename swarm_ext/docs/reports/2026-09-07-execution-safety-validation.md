# 集群执行安全改进验证摘要

日期：2026-09-07。实现基于 `3de0014f7d2f8bd26cff8615b158befdf608047a`，对应 [改进计划](../plans/2026-09-07-execution-safety.md)。

## 提交内容

- 将自主等级强制确认落实到执行入口，来源字符串不再作为确认凭证；确认、排队下发和任务阶段执行均重新检查条件。
- 动作绑定解析时的目标机号，覆盖上传、持续编队和中止/超时暂停沿用该集合；非法动作、参数和目标拒绝整计划。
- 统一覆盖坐标解析，补齐完整区域、覆盖高度、转场及返航的保守围栏检查；任务受阻后停止推进，人工重试须重新检查。
- 真实视频输入读取 BGR 帧并传入检测器，处理有限重连与资源释放；API Key 仅会话保存并清理历史明文配置键。
- 增加真实 Manager/AI/Task/MockLink 调用链测试，覆盖两机选取和上传、JSON/Function Calling 确认、约束重验、阻塞恢复及候选方案；修正 CI 依赖、权限和 lint 安装配置。

## 已执行验证

环境为现有 `qgc-ubuntu-docker:latest`、Qt 6.11.1、Debug 测试版 QGC，没有连接真实飞行器。提交前核对校验和：C++、QML、CMake、Python 源码和可执行文件与最后验证记录一致；本次提交整理仅新增验证摘要并调整文档链接。

| 检查 | 结果 | 本地证据 |
| --- | --- | --- |
| `just build` | 通过 | `just-build-complete.log` |
| Swarm standard | 15/15，含独立 C++ 9/9、Python 感知 22 项 | `standard-complete/SUMMARY.md` |
| QGC Unit，排除 Flaky/Network | 183/183 | `qgc-unit-final/SUMMARY.md` |
| QGC Integration，串行、排除 Flaky/Network | 58/58 | `qgc-integration/SUMMARY.md` |
| 最终隔离 SwarmExecutionTest | 1/1 CTest | `swarm-execution-final-isolated.log` |
| 最终 Python 感知复核 | 22 PASS / 0 FAIL | `perception-final.log` |
| 8 机编队、控制租约、任务编排 | 场景 02/07/08 通过 | `sitl-targeted/SUMMARY.md` |
| 8 机区域搜索 | 场景 01 独立重跑通过 | `sitl-search-serial/SUMMARY.md` |
| GUI/COP 感知链 | 场景 06 串行重跑通过；150 帧、148 条航迹报文、30 次心跳 | `qml-serial/SUMMARY.md` |
| 定向静态检查 | 19 个 C++ 文件通过 clang-format 22.1.5；Ruff、YAML、Markdown、工作流安全、cmake-lint 和执行规则通过 | `scoped-lint-results.json` |
| 直接 Qt qmllint | 退出码 0，保留现有类型解析和 unqualified 警告 | `qmllint-panel-direct.log` |
| 全量 `just lint` | **未通过，退出码 1** | `just-lint-vale-fixed.log` |

证据路径均相对于本地 `test_results/2026-09-07-implementation/`。原始日志、校验和与详细实施报告保留为本地验收产物；本摘要用于仓库内审查，不依赖将构建产物或临时缓存纳入版本管理。

## 未通过项与结论边界

- 全量 lint 有仓库原有 JSON/C++/Python 格式、脚本权限、文档和规则违规，容器也缺少 clang-tidy。本次涉及的 Ruff、Markdown、CI 权限及 Vale/CMake 安装问题已修正并定向检查；不能据此认定全量 lint 通过。
- `test/CMakeLists.txt` 仅增加 Swarm 子目录，其原有格式问题已从 HEAD 复现。新增 `test/Swarm/CMakeLists.txt` 格式通过；clazy 包装脚本的成功退出未计为完整静态分析成果。
- Integration 运行期间补充过候选方案测试，最终 Unit 和 Swarm 专项测试在代码冻结后复核；未将其表述为最终完整发布资格。
- 首次 GUI 运行与 headless QGC 共用 UDP 端口而失败，串行重跑通过。首轮搜索有一架飞机未满足高度门限并伴随起飞前自动上锁、GPS/航向告警，独立重跑通过。这些结果不代表多轮或长时间可靠性达标。
- 视频接线测试使用受控适配器，GUI 指标来自模拟馈送；没有验证真实相机/模型精度或时延。移动相机还需逐帧同步遥测与云台姿态。
- 真机/HIL、长时间飞行、容量、跨平台、网络整形、系统密钥环和完整任务断点恢复不在本次完成范围。围栏采用保守二维几何，异步物理执行不具备事务回滚能力。

本次完成计划中的代码、回归与文档，完整发布门禁仍未通过。
