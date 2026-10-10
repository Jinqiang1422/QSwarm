# QGroundControl 集群无人机指挥控制二次开发方案

> 版本：v1.3（新增任务模板库，见第 10 章）｜ 基线：QGroundControl master（v5.x，Qt6/CMake）+ PX4 v1.14～v1.16 SITL
> 配套交付：`src/Swarm/`（C++ 扩展源码）、`src/FlyView/`（集群面板 + AI 面板）、`templates/`（内置任务模板）、《集成补丁说明.md》、《大模型集群任务接口扩展方案.md》
> 代码管理：全部源码与文档纳入 git 仓库（见仓库 README.md 与提交历史）

---

## 1. 概述

### 1.1 目标

在开源 QGroundControl（下称 QGC）基础上扩展集群（Swarm）指挥控制能力，覆盖四大核心场景：

| 能力 | 说明 |
|---|---|
| 多机实时监控 | 地图/面板同屏监视多架无人机：编号、解锁状态、飞行模式、高度、地速、电量，支持点选分组 |
| 集群一键指令 | 全体/选中分组的一键解锁、起飞、降落、返航、暂停、开始任务、紧急上锁 |
| 编队飞行 | 长机-僚机模式：楔形/纵队/横队/环形，间距与高度分层可调，队形实时跟随长机 |
| 集群任务规划 | 矩形区域自动条带划分 + 割草机航线生成，一键上传到各机并协同执行 |

### 1.2 总体思路（关键决策）

1. **不改 MAVLink 协议、不改 PX4 固件**：集群协同逻辑全部实现在地面站侧。QGC 通过标准 MAVLink 指令（`MAV_CMD_*`）和任务上传协议驱动各机，真机与 SITL 行为一致。
2. **完全复用 QGC 既有多机架构**：QGC master 的 `MultiVehicleManager` 已支持同一链路上按 `MAV_SYS_ID` 区分多机，并提供 `vehicles` / `selectedVehicles`（多机选中）模型与批量操作基础[^3^]。本方案在其上新增 Swarm 模块，不重造轮子。
3. **与 QGC master 架构对齐**：新模块采用与 `FollowMe`、`MultiVehicleManager` 相同的 `Q_APPLICATION_STATIC` 单例 + `QML_ELEMENT` 模式，C++ 源码直接编入主程序目标[^3^]。
4. **交互融合**：集群面板作为 Fly View 的覆盖层（overlay）注入，与 QGC 自带的 `MultiVehicleList`、地图、工具条共存，不改变单机操作习惯。

### 1.3 基线版本说明

QGC 在 v4.x（QGCToolbox 工具箱模式）与 master v5.x（单例管理器模式）之间架构差异较大。
本方案的代码与补丁针对 **master 分支（v5.x）** 编写并已逐一对照源码核实接口
（`Vehicle::guidedModeTakeoff(double)`、`Vehicle::sendMavCommand(...)`、
`PlanManager::writeMissionItems(...)`、`MultiVehicleManager::selectedVehicles` 等）[^3^]。
若你的工程锁定在 v4.4 等旧分支，接口名基本一致，主要差异在于管理器需挂到 `QGCToolbox` 上，按需微调补丁 2～5 即可。

---

## 2. QGC 多机连接机制（集群能力的地基）

### 2.1 一条链路，多架飞机

- 每架 PX4 实例启动后周期性广播 `HEARTBEAT`，报文中携带自己的 **MAV_SYS_ID**。
- QGC 默认在 UDP **14550** 端口监听；无论多少个飞控实例把 GCS 流量发往 14550，QGC 都在**同一条链路**上接收[^1^][^2^]。
- `MAVLinkProtocol` 按 system id 区分来源，`MultiVehicleManager::_vehicleHeartbeatInfo` 为每个新 system id 创建一个 `Vehicle` 对象[^3^]。

结论：**地面站侧不需要为集群配置任何额外链路**，N 架 SITL 实例默认全部接入 QGC。

### 2.2 MultiVehicleManager 提供的能力

| 接口 | 作用 |
|---|---|
| `vehicles()` | 全部在线车辆（`QmlObjectListModel`），QML 可直接作模型 |
| `selectedVehicles()` / `selectVehicle(id)` / `deselectVehicle(id)` | 多机选中集合，集群指令的“作用域” |
| `activeVehicle` | 当前激活车辆（QGC 单机 UI 的焦点） |
| `getVehicleById(id)` | 按系统 id 取车辆指针 |
| `vehicleAdded / vehicleRemoved` | 上下线信号 |

每个 `Vehicle` 对象提供集群所需的全部原语：
`setArmed` / `guidedModeTakeoff(alt)` / `guidedModeLand` / `guidedModeRTL` /
`guidedModeGotoLocation(coord)` / `pauseVehicle` / `startMission` /
`sendMavCommand(...)` / `missionManager()->writeMissionItems(...)` /
`vehicleFactGroup()`（heading、altitudeAMSL、altitudeRelative、groundSpeed 等遥测）[^3^]。

---

## 3. 系统架构

```
┌───────────────────────────── QGroundControl ─────────────────────────────┐
│                                                                          │
│  Fly View                                                                │
│  ┌──────────────────────────┐      ┌──────────────────────────────┐      │
│  │ SwarmControlPanel.qml    │      │ FlyViewMap / MultiVehicleList│      │
│  │ ·多机监控 ·集群指令       │      │ （既有：地图多机图标/多机列表）│      │
│  │ ·编队控制 ·任务规划       │      └──────────────────────────────┘      │
│  └────────────┬─────────────┘                                            │
│               │ QGroundControl.swarmManager（QML 属性）                   │
│  ┌────────────▼─────────────┐                                            │
│  │      SwarmManager        │  集群指令调度（交错下发、作用域解析、反馈） │
│  │  ┌────────────────────┐  │                                            │
│  │  │FormationController │  │  编队解算（长机位置+航向→僚机目标点）      │
│  │  ├────────────────────┤  │                                            │
│  │  │SwarmMissionPlanner │  │  区域划分/航线生成/任务上传                │
│  │  └────────────────────┘  │                                            │
│  └────────────┬─────────────┘                                            │
│               │                                                          │
│  ┌────────────▼─────────────┐     heartbeats      ┌─────────────────┐    │
│  │   MultiVehicleManager    │◄──── 14550/udp ─────│ PX4 SITL × N    │    │
│  │   Vehicle[1..N]          │──── commands ──────►│ (MAV_SYS_ID 1..N)│   │
│  └──────────────────────────┘                     └─────────────────┘    │
└──────────────────────────────────────────────────────────────────────────┘
```

模块职责：

- **SwarmManager**：单例。集群指令的唯一入口；解析作用域（选中集优先，否则全体）；按可配间隔（默认 300ms，起飞 2500ms）**交错下发**指令，避免链路瞬时拥塞与多机同拍离地；通过 `swarmCommandFeedback` 向 UI 反馈。
- **FormationController**：1 Hz 定时器驱动。每拍读取长机位置/航向/海拔，按队形几何计算各僚机目标点（含高度分层），用 `guidedModeGotoLocation`（`MAV_CMD_DO_REPOSITION`）下发；长机异常时自动停止编队、僚机原地悬停。
- **SwarmMissionPlanner**：把矩形作业区按垂直航线方向均分为 N 个条带，条带内生成割草机航线，组装 `MissionItem`（起飞→航点→返航）后经各机 `MissionManager` 上传。
- **SwarmControlPanel.qml**：四大能力的统一操作界面（见第 4 章）。

---

## 4. 核心能力详细设计

### 4.1 多机实时监控

- 面板列表绑定 `MultiVehicleManager::vehicles`，每行显示：**系统 id、解锁状态、飞行模式、相对高度、地速、电量**；绿色描边标识 QGC 激活机，高亮底色标识选中机。
- 点击行即选中/取消选中（`selectVehicle / deselectVehicle`），“全选/全不选”快捷按钮；所有集群指令默认作用于**选中集**，未选择时作用于全体——这一作用域规则在 `SwarmManager::_targetVehicles()` 中统一实现。
- 地图侧复用 QGC 既有能力：所有在线飞机图标、轨迹线已在 FlyViewMap 中显示，无需新增代码。

### 4.2 集群一键指令

| 按钮 | 调用 | MAVLink 消息 | 说明 |
|---|---|---|---|
| 解锁/上锁 | `Vehicle::setArmed(arm, true)` | `MAV_CMD_COMPONENT_ARM_DISARM` (400) | 机间间隔 300ms |
| 起飞 | `Vehicle::guidedModeTakeoff(alt)` | `MAV_CMD_NAV_TAKEOFF` (22) | 未解锁机自动先解锁；机间间隔 2500ms |
| 降落 | `Vehicle::guidedModeLand()` | `MAV_CMD_NAV_LAND` (21) | 原地降落 |
| 返航 | `Vehicle::guidedModeRTL(true)` | `MAV_CMD_NAV_RETURN_TO_LAUNCH` (20) | |
| 暂停 | `Vehicle::pauseVehicle()` | `MAV_CMD_DO_REPOSITION`（暂停标志） | **0 间隔同时下发**（安全指令不允许排队） |
| 开始任务 | `Vehicle::startMission()` | `MAV_CMD_MISSION_START` (300) | 配合 4.4 上传的航线 |
| 紧急上锁 | `sendMavCommand(..., 0, 21196)` | `MAV_CMD_COMPONENT_ARM_DISARM` | param2=21196 强制上锁，**飞行中使用将坠机**，UI 带二次确认 |

指令可靠性：均走 QGC 既有的 `sendMavCommand` 重传机制（未收到 ACK 自动重发）[^3^]。
执行反馈：每次批量指令经 `swarmCommandFeedback` → `QGCApplication::showAppMessage` 显示在 QGC 顶部消息条。

### 4.3 编队飞行

**控制模型**：长机-僚机（Leader-Follower）。长机由操作员用 QGC 单机功能驾驶（或执行任务），僚机位置由地面站解算后持续引导。

**队形几何**（第 k 架僚机，僚机按 id 升序编号 k=1..n；r=(k+1)/2 为排数，s=±1 为左右侧）：

| 队形 | 距离 | 相对长机航向的方位偏移 | 高度增量 |
|---|---|---|---|
| 楔形 Wedge | spacing × r | 180° + s×30°（长机后两翼） | step × r |
| 纵队 Column | spacing × k | 180°（正后方） | step × k |
| 横队 Line | spacing × r | s × 90°（左右两翼） | 0 |
| 环形 Circle | spacing | 360°×(k−1)/n 均布 | 0 |

每拍目标点：`target = leaderPos.atDistanceAndAzimuth(距离, 长机航向 + 方位偏移)`，高度 = 长机 AMSL + 高度增量，经 `guidedModeGotoLocation(target)` 下发（PX4 在 Position/Hold 模式下执行 DO_REPOSITION）。

**安全策略**（代码内置）：
- 间距硬下限 2 米；楔形/纵队默认 3 米高度分层，避免僚机进入长机下洗流；
- 仅对已解锁且相对高度 > 1.5 m 的僚机下发指令（未离地机自动等待入队）；
- 长机丢失/上锁/定位失效 → 自动停止编队，全部僚机 `pauseVehicle()` 原地悬停；
- 编队运行中可随时“停止编队”，僚机立即悬停待命。

**性能说明与升级路径**：1 Hz DO_REPOSITION 适合 SITL 验证与低速编队。需要更平滑或更高速的编队时，把 `FormationController::_updateFormation()` 中的下发方式替换为 **OFFBOARD + `SET_POSITION_TARGET_GLOBAL_INT` 设定点流（≥2 Hz，经 `Vehicle::sendMessageOnLinkThreadSafe` 发送）**，几何解算部分完全复用。

### 4.4 集群任务规划（区域覆盖）

**算法**：
1. 输入：区域中心（默认取当前地图中心 `flightMapPosition`）、长 L（沿航线方向）、宽 W、航线方位角 θ、作业高度 h、行距 s。
2. 沿垂直航线方向把区域均分为 N 个条带（N = 目标车辆数），条带宽 W/N。
3. 每个条带内按行距 s 生成割草机（boustrophedon）航线，逐行交替方向，减少转场距离。
4. 本地平面坐标 (x, y) 经旋转和平移换算为 WGS84：
   `east = x·sinθ + y·cosθ`，`north = x·cosθ − y·sinθ`，再由 `atDistanceAndAzimuth` 反算经纬度。
5. 每架机的任务项序列：`MAV_CMD_NAV_TAKEOFF(h)` → 若干 `MAV_CMD_NAV_WAYPOINT(h)` → `MAV_CMD_NAV_RETURN_TO_LAUNCH`，帧类型 `MAV_FRAME_GLOBAL_RELATIVE_ALT_INT`，经 `missionManager()->writeMissionItems()` 上传（QGC 任务协议自动处理分包/重传）[^3^]。

**协同执行**：上传完成 → 面板“开始任务”→ `SwarmManager::startMissionAll()` 交错下发 `startMission()`，各机并行执行各自条带，互不干扰。

---

## 5. PX4 SITL 多机仿真环境

### 5.1 端口与 ID 约定

- 每个实例拥有唯一 **MAV_SYS_ID = 实例号 + 1**；GCS 流量统一发往 **UDP 14550**（QGC 默认监听，自动接入全部实例）[^1^][^2^]。
- Offboard API（MAVSDK/MAVROS）端口 14540 起按实例递增（本方案不需要）[^1^]。

### 5.2 方式 A：Gazebo（gz，PX4 v1.14+ 推荐）

```bash
cd PX4-Autopilot
make px4_sitl
# 终端 1～3：每架一个终端，注意不同实例号与出生点
PX4_SYS_AUTOSTART=4001 PX4_SIM_MODEL=gz_x500 \
  ./build/px4_sitl_default/bin/px4 -i 0
PX4_SYS_AUTOSTART=4001 PX4_SIM_MODEL=gz_x500 PX4_GZ_MODEL_POSE="3,3" \
  ./build/px4_sitl_default/bin/px4 -i 1
PX4_SYS_AUTOSTART=4001 PX4_SIM_MODEL=gz_x500 PX4_GZ_MODEL_POSE="6,6" \
  ./build/px4_sitl_default/bin/px4 -i 2
```

### 5.3 方式 B：Gazebo Classic（v1.16 仍提供脚本）

```bash
cd PX4-Autopilot
make px4_sitl_default gazebo-classic
./Tools/simulation/gazebo-classic/sitl_multiple_run.sh -m iris -n 3
```

### 5.4 地面站

启动改造后的 QGroundControl 即可——默认 UDP 14550 自动发现并接入全部实例，无需添加链路。
若实例运行在其他主机/容器，把该主机的 14550/udp 映射出来或在 PX4 启动脚本中追加 `mavlink start -x -u <port> -r 4000000 -m onboard -t <QGC主机IP>`。

---

## 6. 集成步骤（摘要）

详见《集成补丁说明.md》，共 6 处追加式改动：

1. 拷贝 `src/Swarm/` 与 `SwarmControlPanel.qml` 到源码树；
2. `src/CMakeLists.txt` 增加 `add_subdirectory(Swarm)`；
3. `QGCApplication.cc` 初始化 `SwarmManager::instance()->init()`；
4. `QGroundControlQmlGlobal.h/.cc` 增加 `swarmManager` QML 属性；
5. `src/FlyView/CMakeLists.txt` 登记 QML 文件；
6. `FlyViewWidgetLayer.qml` 实例化 `SwarmControlPanel`。

```bash
cmake --preset default && cmake --build build/default -j$(nproc)
```

---

## 7. 安全与可靠性设计

| 风险 | 对策 |
|---|---|
| 多机同时离地气流干扰/碰撞 | 起飞指令 2500ms 机间交错；编队高度分层 |
| 指令风暴拥塞链路 | 全部批量指令按 `commandIntervalMs`（默认 300ms）排队下发，可配置 |
| 误触危险指令 | 起飞/降落/返航/紧急上锁均带二次确认对话框 |
| 长机失效 | 编队自动停止 + 僚机原地悬停 |
| 单机失联 | `QPointer` 守护：排队中的指令自动跳过已销毁对象；QGC 既有遥测丢失告警保留 |
| 误操作扩散 | 作用域规则：有选中集时指令只作用于选中机 |
| 地理约束 | 依赖 PX4 机载 Geofence（`GF_*` 参数）作为最后防线，建议 SITL 阶段即配置 |

---

## 8. 验证流程（SITL 三机）

**场景一：多机监控 + 集群指令**
1. 按 5.2 启动 3 架 x500；启动改造后的 QGC，确认面板显示“集群指挥控制 (3 架)”。
2. 逐行核对三机的模式/高度/电量显示；点击行选中 1、2 号机，确认“已选 2 架”。
3. “全选”→ 起飞高度 10 m → “起飞”（确认）：观察三机按 2.5 s 间隔依次离地悬停。
4. “暂停”验证悬停保持；“返航”→ 三机依次返航降落；“降落/上锁”收尾。

**场景二：楔形编队**
1. 三机起飞至 10 m 悬停；长机选“载具 1”，队形“楔形”，间距 10 m，高度分层 3 m。
2. “开始编队”：观察 2、3 号机机动到 1 号机后 ±30°、后排升高 3 m。
3. 在 QGC 单机界面用“前往位置”驾驶长机平移/转向，观察僚机保持队形跟随。
4. 给长机“返航”：观察编队自动停止、僚机原地悬停（安全策略生效）。

**场景三：区域覆盖任务**
1. 地图缩放到一块约 200×120 m 的空地；面板填长 200 / 宽 120 / 角度 0 / 行距 20。
2. “全选”→“生成并上传”：确认提示“3 架无人机，共 N 个航点（条带宽度 40 米）”。
3. 切到 Plan 视图逐机检查航线（三条平行条带，割草机往返）。
4. 回 Fly 视图 →“开始任务”：三机并行覆盖各自条带；结束后自动返航。
5. “清空任务”验证 `removeAll`。

---

## 9. 后续扩展路线

| 方向 | 内容 |
|---|---|
| 高频编队控制 | OFFBOARD + SET_POSITION_TARGET_GLOBAL_INT 设定点流（≥2 Hz），平滑轨迹 |
| 任意多边形区域 | `SwarmMissionPlanner` 输入改为多边形顶点表，扫描线分解 |
| 能力加权分区 | 按各机剩余电量/载荷动态调整条带宽度 |
| 在线重规划 | 单机故障退出后，其条带自动重新分配给剩余飞机 |
| 队形变换指令 | 楔形↔纵队等队形飞行中平滑切换（目标点插值过渡） |
| 集群状态大屏 | 基于 TelemetryValuesBar 扩展集中遥测墙，配合 QGC 自定义插件品牌化 |
| 真机链路 | UDP 透传换为数传电台/4G；建议 mavlink-router 做链路旁路，保持单链路多 system id 模型 |

---

## 10. 大模型集群任务接口（v1.1 新增，v1.2 升级）

在既有四大能力之上扩展 **AI 指挥层**：`SwarmAIAgent`（SwarmManager 子对象）+ `SwarmAIPanel.qml`，
操作员以自然语言下达任务，大模型理解后输出结构化动作计划，经**白名单校验、参数限幅、机号有效性检查**后，
危险动作计划挂起由操作员确认再执行（人在回路）。

v1.2 三项升级：

1. **Function Calling**：动作集注册为 OpenAI tools（15 个工具定义），模型经原生 `tool_calls` 返回，
   格式漂移大幅降低；端点不支持时可一键回退 JSON 模式（面板开关，持久化）。
2. **任务状态机（SwarmTaskController）**：多阶段任务编排——每个阶段 = 一组动作 + 完成条件
   （全部升空/全部上锁/航线完成/编队就位/计时/人工确认），1 Hz 监控各机 MISSION_CURRENT 驱动的
   模式切换，条件满足自动推进；阶段宽限期防误判，超时判失败并全体悬停，可随时中止。
3. **统一任务管线**：AI 规划与人工 Plan 视图设计是同一套系统——同一个 `MissionItem` 原语、
   同一个 `PlanManager::writeMissionItems` 上传通道（QGC MissionController 亦调用此函数）、
   上传后自动回读同步（Plan 视图可继续人工编辑 AI 任务）、同一份 MISSION_CURRENT 进度监控。
   动作解析/校验/执行统一收口在 `SwarmManager::parseAndValidate/executeActions`。

4. **任务模板库（SwarmTaskLibrary，v1.3）**：常用多阶段任务存为 JSON 模板（内置 4 套：区域搜索后返航 /
   编队巡航后返航 / 快速出动待命 / 全员返航降落），参数占位符 `"${key}"` 实例化时代入（含默认值与限幅）。
   人工在面板按参数表单直接执行；AI 通过 `run_task_template` 工具按名引用——意图匹配模板时优先于自定义编排，
   实例化后仍经统一校验与人在回路确认。模板目录可手工扩展，沉淀组织级标准作业流程。
   > 2026-10 更新：内置模板经 P2 扩充后精简为 6 套（区域搜索后返航 / 平行网格扫掠 / 编队巡航后返航 / 快速出动待命 / 全员返航降落 / 多机接力值守），见 `specs/2026-10-09-task-template-simplification-design.md`。

- **端点无关**：任意 OpenAI 兼容接口（DeepSeek / Kimi / 通义千问 / OpenAI），外场可切 Ollama 本地模型离线部署；
- **零新增全局补丁**：`QGroundControl.swarmManager.aiAgent / .taskController / .taskLibrary` 直达。

详细设计（tools 定义、状态机条件语义、统一管线论证、验证脚本）见《大模型集群任务接口扩展方案.md》。

---

[^1^]: PX4 官方文档《仿真》— 默认 MAVLink UDP 端口（GCS 14550、Offboard 14540 及多机端口分配）: https://docs.px4.io/main/zh/simulation/
[^2^]: PX4 官方文档《Multi-Vehicle Simulation with Gazebo》— 逐实例启动方式、每实例唯一 MAVLink system id: https://docs.px4.io/v1.14/en/sim_gazebo_gz/multi_vehicle_simulation.html
[^3^]: QGroundControl 源码（master）：`src/Vehicle/MultiVehicleManager.h/.cc`、`src/Vehicle/Vehicle.h`、`src/MissionManager/PlanManager.h`、`src/FlyView/MultiVehicleList.qml`、`src/FollowMe/` : https://github.com/mavlink/qgroundcontrol
