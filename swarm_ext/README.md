# QGC 集群指挥控制扩展（QGroundControl Swarm Extension）

基于开源 QGroundControl 的集群无人机指挥控制扩展，覆盖：

- **多机实时监控**：同屏监视多架无人机状态，点选分组
- **集群一键指令**：全体/分组的解锁、起飞、降落、返航、暂停、开始任务
- **编队飞行**：长机-僚机模式，楔形/纵队/横队/环形，间距与高度分层可调
- **集群任务规划**：矩形区域自动条带划分 + 割草机航线，一键上传
- **大模型任务接口**：自然语言 → 任务理解 → 规划 → 分配（Function Calling，支持离线 Ollama）
- **任务状态机**：多阶段任务编排，监控 MISSION_CURRENT 自动推进
- **任务模板库**：常用多阶段任务存为 JSON 模板（内置 4 套），人工/AI 按名引用
- **统一任务管线**：AI 规划与人工 Plan 视图设计同一套系统

## 目录结构

```
├── src/
│   ├── Swarm/                     # C++ 扩展模块（拷入 qgroundcontrol/src/Swarm/）
│   │   ├── SwarmManager           # 集群指令 + 统一动作管线（parseAndValidate/executeActions）
│   │   ├── SwarmAction.h          # 统一动作结构（AI/状态机/人工共用）
│   │   ├── FormationController    # 编队控制（长机-僚机）
│   │   ├── SwarmMissionPlanner    # 区域覆盖任务规划（与 Plan 视图同一 writeMissionItems 通道）
│   │   ├── SwarmAIAgent           # 大模型接口（Function Calling + JSON 回退）
│   │   ├── SwarmTaskController    # 任务状态机（多阶段编排）
│   │   └── SwarmTaskLibrary       # 任务模板库（JSON 模板，人工/AI 引用）
│   └── FlyView/                   # QML 面板（拷入 qgroundcontrol/src/FlyView/）
│       ├── SwarmControlPanel.qml  # 集群监控/指令/编队/任务规划面板
│       └── SwarmAIPanel.qml       # AI 对话/计划确认/任务状态机/模板库面板
├── templates/                     # 内置任务模板参考副本（运行时自动落盘到用户目录）
├── QGC集群指挥控制二次开发方案.md      # 总体设计（架构/SITL/安全/验证）
├── 大模型集群任务接口扩展方案.md        # AI 接口设计（tools/状态机/统一管线）
├── 集成补丁说明.md                     # 6 处追加式集成补丁
└── README.md
```

## 快速开始

1. 按《集成补丁说明.md》把 `src/Swarm/`、`src/FlyView/*.qml` 集成进 QGroundControl 源码树（6 处追加式改动）；
2. 编译：`cmake --preset default && cmake --build build/default -j$(nproc)`；
3. 启动 PX4 SITL 多机仿真（3 架）与改造后的 QGC，按主方案第 8 章验证集群功能；
4. 配置大模型端点（默认 DeepSeek，外场可切 Ollama 本地模型离线运行），按 AI 方案第 8 章验证文本指挥。

## 基线版本

- QGroundControl master（v5.x，Qt6/CMake，单例管理器架构）
- PX4 v1.14～v1.16（Gazebo / Gazebo Classic 多机仿真）

## 许可

与 QGroundControl 相同（Apache-2.0 / GPL-3.0 双许可，遵循上游）。
