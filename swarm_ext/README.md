# QGC 集群指挥控制扩展

本仓库已集成多机监控与批量指挥、编队、矩形区域覆盖、AI 计划审查、多阶段任务和感知态势。无需再次按历史补丁说明复制源文件。交付构建（`QGC_SWARM_AI_AUTODISPATCH=OFF`，非测试构建的默认值）不提供模型免确认自动下发：所有 AI 动作都要操作员逐条确认后才执行，纯人工、纯大模型、人工+大模型三种编排模式仍均可用；设计与边界依据见[任务编排与下发锁设计](docs/specs/2026-09-12-swarm-task-authoring-and-dispatch-lock-design.md)。

## 使用文档

- [完整使用手册](docs/用户使用手册.md)：24 章，涵盖基础 QGC、集群、AI、感知、日志与故障排查。
- [离线阅读版](docs/用户使用手册.html)：支持目录、章节筛选和打印。
- [PDF 打印版](docs/用户使用手册.pdf)：用于分发和留档。
- [功能分析与核验](docs/功能分析与手册核验.md)：功能依据、操作差异和剩余问题。
- [当前功能矩阵](docs/FEATURES.md)、[版本验证摘要](docs/reports/2026-09-24-sitl-verification.md)。

软件支持四种编队和三种覆盖分配算法。九套模板资产不代表九种完整业务闭环均可直接使用：接力值守模板在当前严格选机规则下存在冲突；停止编队、任务中止、飞控暂停及返航也有不同语义，操作前阅读手册。

## 构建与验证

在仓库根目录、已准备的开发环境中使用 canonical 工作流：

~~~bash
just configure
just build
~~~

运行环境和工具见 [开发工具说明](../tools/README.md)。集群测试入口：

~~~bash
python3 swarm_ext/tests/run_all.py --profile standard
~~~

完整步骤见 [测试说明](tests/README.md)、[SITL 仿真说明](sim/README.md) 和 [感知服务说明](services/perception/README.md)。测试与模拟器应在隔离环境运行，结果不能替代现场设备验收。

## 目录

| 路径 | 内容 |
| --- | --- |
| ../src/Swarm | 集群控制、AI、约束、编队、任务、感知和审计实现 |
| ../src/FlyView/Swarm*.qml | 定制面板与地图图层 |
| ../test/Swarm | 真实 QGC/MockLink 执行链路回归 |
| templates | 模板参考资产；应用实际读取用户数据目录中的 SwarmTemplates |
| services/perception | 独立视频检测、跟踪和结构化航迹服务 |
| sim | SITL 场景与验收驱动 |
| tests | 分层验证工具、独立模块和模型契约检查 |
| docs | 用户文档、开发记录、计划与验证摘要 |

## 许可

遵循仓库根目录的 QGroundControl 许可文件。
