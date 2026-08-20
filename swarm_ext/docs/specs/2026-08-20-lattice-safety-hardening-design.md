# Lattice 补齐 · 安全/可靠性收尾 — 设计文档

> 日期：2026-08-20 · 分支：main · 定位：安全/可靠性收尾(最小范围)

## 1. 背景与目标

Lattice 评估结论:当前软件在应用层(C2 + Autonomy + AI + 治理)强满足甚至超出,平台层(Mesh + SDK + 统一数据模型)缺失,得出 6 项补齐清单。本轮开发计划取**「安全/可靠性收尾」**定位,补齐其中三项工程欠账,让现有应用更安全可靠,**不改内嵌架构路线**:

- **① 控制租约**(🔴):七轮 QA 审查未覆盖的跨 QGC/Core 控制权问题——SwarmManager(自动化)与 QGC/操作员共用同一条 MAVLink,操作员手动接管某机时,Swarm 的状态机推进/AI 下发不应再对该机发指令。
- **④ 任务持久化 + 重启恢复**(🟡):对应最小架构 MVP 验收 #6「重启不自动恢复未确认飞行任务」。
- **⑤ SITL 闭环验证**(🟢):环境受阻于 headless 服务器,脚本就绪待环境。

## 2. 范围

### 2.1 做
- `SwarmControlLease`:per-vehicle 控制租约状态机 + 飞行模式接管自动释放。
- `SwarmPersistence`:任务态 JSON 快照 + 重启恢复(RUNNING→PAUSED)。
- 集成:`executeActions` 逐机查租约、`SwarmManager` 监听飞行模式、`SwarmTaskController` 落盘/恢复。
- SITL 验证脚本整理(pymavlink,不依赖 QGC GUI),标记待环境执行。
- 单测:`tst_lease` / `tst_persistence` / `tst_pipeline` 补契约。

### 2.2 不做(YAGNI,留待后续轮次)
- Lattice Mesh 分布式数据网格 / 多节点边缘分发。
- 对外 REST/WebSocket API / 通用 SDK。
- 统一 Entity/Task/Object 数据模型层。
- 修改 PX4 上游。

## 3. 架构与模块划分(方案 A:两个可测模块)

```
SwarmControlLease (新·纯 QtCore)   per-vehicle 租约状态机          —— 可独立单测
SwarmPersistence  (新·纯 QtCore)   任务/租约 JSON 快照读写         —— 可独立单测
        ↑ 集成(薄)
SwarmManager        executeActions 前逐机查租约 + 监听 Vehicle::flightModeChanged → 通知 Lease
SwarmTaskController 任务态变化落盘 + 启动时恢复
```

两个新模块延续项目一贯的「纯 QtCore、单一职责、可脱离 QGC 独立单元测试」风格;SwarmManager 已 ~920 行(第六轮标🟡),故集成保持薄,不再撑大它。

## 4. 控制租约(SwarmControlLease)

### 4.1 状态机(每 vehicleId 独立)
```
OBSERVE_ONLY → LEASE_REQUESTED → LEASED → RELEASED
                                   │
                                   └→ MANUAL_OVERRIDE(飞行模式接管即释放)
```

### 4.2 纯逻辑接口(便于容器 Qt6 单测)
- `requestLease(vehicleId, taskId, operator)` — 授予租约,生成 token + 过期时间(过期后需重新申请)。
- `onFlightModeChanged(vehicleId, mode)` — mode 属**操作员模式**则 `MANUAL_OVERRIDE` + 释放。
- `canAutomate(vehicleId)` → bool — 是否可自动下发(LEASED 且未被接管)。
- `release(vehicleId)` / `expireCheck(nowMs)` — 主动释放 / 定时过期释放。

### 4.3 飞行模式判定
- **操作员模式(视为接管,释放租约)**:Position、Hold、Altitude、Manual、Stabilized、RTL 等。
- **自动化模式(不释放)**:Mission、Offboard。
- 判定基于模式名匹配(QGC `Vehicle::flightMode` 字符串),保守策略:未知模式一律视为操作员模式(接管)。

### 4.4 集成语义
- `executeActions` 对每个目标机查 `canAutomate`,采用**逐机粒度**:被接管的机跳过下发 + 反馈「N 号机已人工接管,跳过」,其余机照常下发(一架被接管不影响其他机的自动化)。
- 无租约的机默认只读遥测、不下发(对应最小架构 §8「无租约只读」)。
- SwarmManager 订阅 `Vehicle::flightModeChanged` → `Lease.onFlightModeChanged`。
- 自动化路径(AI 下发 / 状态机推进)在下发前对涉及机 `requestLease`;操作员切飞行模式即自动释放。

### 4.5 与第六轮 E1 兜底的关系
E1 已在 executeActions 加了 autonomy L1 全局兜底(L1 档位禁一切下发)。租约是**更细粒度的 per-vehicle 控制权闸门**,与 E1 叠加:L1 挡「全局禁下发」,租约挡「这架机已被人接管」。两者互补,不重复。

## 5. 任务持久化(SwarmPersistence)

### 5.1 存储
- JSON 文件,置于 `AppDataLocation/SwarmConfig/`(复用 fences.json 位置习惯)。
- `tasks.json`:任务态数组,字段 `id / type / assignee / approval{status,approved_by} / status / version`。
- 原子写:先写 `tasks.json.tmp` 再 rename,防写入过程中崩溃损坏。

### 5.2 恢复语义(安全优先,MVP #6)
- 启动时 `loadTasks()`:
  - `RUNNING` 任务 → 恢复为 **PAUSED**(不自动续飞),供操作员重新确认后再启动。
  - `PENDING / DRAFT` → 保持原态供查看。
- **租约不持久化**:重启后所有机回 `OBSERVE_ONLY`。理由:重启后自动持有某机控制权是危险的,故意不恢复租约,要求重新申请。

## 6. SITL 验证(环境受阻,脚本就绪)

- 现状:多机闭环受阻于 headless 服务器 GL/gz 环境(memory 在案)。
- 计划:整理 `swarm_ext/sim/run_scenario.py`(pymavlink,不依赖 QGC GUI),覆盖 MVP 关键验收项:多机识别、租约释放、任务不串机、重启不恢复。
- **标记「脚本就绪,待显示/仿真环境执行」,不阻塞本轮交付**。
- 可单测的安全逻辑(租约状态机、恢复语义)用容器 tst 覆盖,不依赖 SITL。

## 7. 测试策略

- `tst_lease`(新):请求/授予、飞行模式接管释放、过期释放、canAutomate、逐机独立(一机接管不影响另一机)。正常 + 边界 + 异常路径。
- `tst_persistence`(新):JSON 往返一致、原子写、恢复语义(RUNNING→PAUSED)、损坏文件回退空态。
- `tst_pipeline` 补:「被接管机跳过下发」的契约(纯逻辑层)。
- 整机 Docker 编译验证 SwarmManager/TaskController 接入(0 error)。
- **每模块一 commit**(延续项目一贯的「实现+单测+容器验证+每处 commit」)。

## 8. 错误处理

- JSON 损坏/缺失 → 回退空态 + 审计告警(复用现有 config 回退模式)。
- 租约过期 → `expireCheck` 定时释放,过期机不再可自动化。
- 飞行模式信号缺失(离线机)→ 保守视为不可自动化(不下发)。

## 9. 实施顺序(概要,详细留 writing-plans)

1. `SwarmControlLease` + `tst_lease`(纯逻辑,容器验证)。
2. `SwarmPersistence` + `tst_persistence`(纯逻辑,容器验证)。
3. 集成:SwarmManager `executeActions` 逐机查租约 + 订阅 `flightModeChanged`(整机编译)。
4. `SwarmTaskController` 持久化落盘 + 启动恢复(整机编译)。
5. `tst_pipeline` 补「接管跳过」契约 + 全量重测。
6. SITL 脚本整理(待环境),更新 memory/FEATURES。

## 10. 验收标准(本轮)

- 操作员在 QGC 切某机飞行模式为 Hold/RTL 等 → Swarm 不再对该机自动下发(该机 `canAutomate=false`),其余机不受影响。
- 无租约的机:Swarm 只读遥测不下发。
- 重启后:RUNNING 任务恢复为 PAUSED,所有机回 OBSERVE_ONLY。
- 容器 tst_lease / tst_persistence / tst_pipeline 全绿;整机编译 0 error。
