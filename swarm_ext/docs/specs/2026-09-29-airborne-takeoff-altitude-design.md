# 已在空中的飞机不再重复起飞 — 设计文档

> 日期：2026-09-29 · 定位：行为修正（任务切换时的重复起飞）

## 1. 背景与问题

操作员在机群已升空后切换任务（中止当前任务 → 启动另一个模板或 AI 任务），新任务的第一
阶段几乎总是「起飞」（`takeoff` 动作 + `all_airborne` 完成条件，见 `swarm_ext/templates/`
与 `SwarmTaskLibrary.cc` 内置模板）。该阶段会对**已在空中的飞机再次下发起飞**。

对 PX4 这比“多余”更糟：`PX4FirmwarePlugin::takeoffAsync` 把目标高度算成
**当前海拔 + 起飞高度**（`src/FirmwarePlugin/PX4/PX4FirmwarePlugin.cc`，`takeoffAsync`
首部）。在 30 米悬停的飞机“起飞至 30 米”，实际会爬到约 60 米，并切入 Takeoff 模式。
此外每架机还要等 2.5 秒起飞错峰（`SwarmManager::_takeoffStaggerMs`）。

面板“起飞”按钮（`SwarmManager::takeoffAll`）和 AI 单次计划走同一条管线，问题相同。

## 2. 目标与非目标

**目标**

- 飞机只起飞一次。已在空中的飞机收到 `takeoff` 时不再起飞，而是**升或降到指定的相对高度**。
- 地面上的飞机行为不变。
- 任务模板、AI 计划、面板按钮三条入口一致生效。

**非目标**

- 不实现 ArduPilot 的空中调高（见 §6）。
- 不改变 `all_airborne` 完成条件的语义。
- 不新增动作类型，不改动作 Schema。

## 3. 行为规格

`takeoff` 动作对每架目标机在**实际下发时刻**逐机判定：

| 飞机状态 | 判定 | 行为 |
|---|---|---|
| 未解锁，或 `flying()` 为假 | 在地面 | 原流程：`takeoffAsync`，起飞错峰 2.5 秒 |
| 已解锁且 `flying()`，与目标相对高度之差 ≤ 0.5 米 | 已在目标高度 | 不下发任何指令，该机记为成功 |
| 已解锁且 `flying()`，高度差 > 0.5 米 | 在空中 | `changeAltitudeAsync` 升/降到目标相对高度，不下发起飞 |

- `flying()` 来自飞控的 `EXTENDED_SYS_STATE.landed_state`（TAKEOFF / IN_AIR / LANDING 为真）。
  飞控不报该消息时 `flying()` 恒为假，退回原起飞流程，与现状一致。
- 处于 LANDING 的飞机也按“在空中”处理：会中断降落并回到指定高度。这与“启动新任务就是要
  机群到指定高度”的意图一致，也好于现状（现状是在降落途中再发起飞）。
- 目标海拔 = home 海拔 + 目标相对高度。home 位置或其海拔未知时该机失败（不猜测）。
- 高度调整只改高度，保持当前水平位置，**绝不下发降落**。

## 4. 设计

### 4.1 固件插件：`FirmwarePlugin::changeAltitudeAsync`

新增虚函数，签名与 `takeoffAsync` 同构：

```cpp
virtual void changeAltitudeAsync(Vehicle* vehicle, double altitudeRelative, QObject* context,
                                 std::function<bool()> allowed, std::function<void(bool)> finished) const;
```

- 基类默认 `finished(false)`（同 `takeoffAsync` 基类）。
- **PX4 实现**：发送 `MAV_CMD_DO_REPOSITION`，`param2 = MAV_DO_REPOSITION_FLAGS_CHANGE_MODE`，
  偏航/纬度/经度为 NaN（保持当前），`param7 = home 海拔 + altitudeRelative`。
  监听 `Vehicle::mavCommandResult`：忽略 `IN_PROGRESS`；首个最终结果为 `ACCEPTED` 且
  `allowed()` 仍为真则成功，否则失败；10 秒无结果判失败。与 `takeoffAsync` 的 ACK 处理
  模式一致，迟到的 ACK 不会二次回调。

放在固件插件里，是因为“怎么只改高度”随固件不同（AGENTS.md Firmware Plugin 规则）；
“空中就不起飞”的**决策**与固件无关，放在集群层。

### 4.2 集群管线：`SwarmManager::_dispatchExecutionAction`

- `takeoff` 分支在下发时刻按 §3 判定，分别调用 `takeoffAsync`、`changeAltitudeAsync`
  或直接回报成功。许可检查（`allowed()`、控制租约、执行代次）与原分支完全相同。
- 错峰：调度循环里，下发时判定为“在空中”的飞机使用常规指令间隔 `_commandIntervalMs`
  而非 `_takeoffStaggerMs`。错峰是为地面同时爬升设计的。间隔在调度时计算，
  实际走哪条分支在下发时判定；两者之间状态翻转只影响间隔长短，不影响正确性。
- 审计：空中分支额外记一条 `exec / takeoff_airborne`，含 `vehicle_id`、当前相对高度、
  目标相对高度、`adjusted`（是否实际下发了调高）。原有的 `action_dispatch` 记录保留。

### 4.3 完成条件不变

`all_airborne` 仍为“已解锁且相对高度 ≥ min_altitude − 0.5”。

- 爬升的飞机越过升空判定高度即推进，与正常起飞语义一致（起飞也是越过门槛即推进，
  不等到达最终高度）。
- 下降的飞机本就在门槛之上，宽限期后即推进；剩余下降由后续阶段接管。
  后续阶段（航线、编队）自带高度，这是可接受的。

### 4.4 AI 描述

`SwarmAIAgent` 的 takeoff 工具描述与提示词追加“已在空中的飞机直接调整到该高度”，
并用 `swarm_ext/tests/llm_eval/export_tools.py` 重新生成 `tools_schema.json`
（`validate_assets.py` 校验两者一致）。

## 5. 错误处理

| 情形 | 结果 |
|---|---|
| 调高 ACK 为非 ACCEPTED / 10 秒超时 | 该机失败 → 执行失败 → 任务阶段阻塞（沿用现有“动作部分执行或失败”路径） |
| home 位置/海拔未知 | 该机失败，同上 |
| 下发前许可被撤销（租约、人工接管、中止） | 与原分支相同：`allowed()` 为假即失败，不发指令 |
| 非 PX4 固件在空中 | 基类返回失败，阶段阻塞；不会退回“再起飞” |

## 6. 影响范围与风险

- 仅影响 `takeoff` 动作对**已在空中**飞机的处理；地面起飞路径逐字不变。
- 面板“起飞”按钮对空中飞机：从“再爬升 X 米”变为“飞到 X 米”，与确认框文字
  “起飞至 X 米”一致。
- ArduPilot：现状下 ArduCopter 对空中起飞本就拒绝；本次保持“失败”而不是改为调高，
  需要时再单独实现 `APMFirmwarePlugin::changeAltitudeAsync`。

## 7. 测试

**单元测试（MockLink，PX4）**

MockLink 目前对 `MAV_CMD_DO_REPOSITION` 回 `UNSUPPORTED`，`SwarmDispatchLockTest` 依赖这一点。
新增默认关闭的 `MockLink::setRepositionSupported(bool)`：开启后 ACCEPTED，并在 `param7`
有限时把模拟海拔设为该值。仅新用例开启。

1. 地面机执行 `takeoff`：发 `NAV_TAKEOFF`，不发 `DO_REPOSITION`（回归保护）。
2. 空中机执行 `takeoff` 到不同高度：发 `DO_REPOSITION`（param7 = home 海拔 + 目标），
   不发 `NAV_TAKEOFF`，执行成功，模拟高度到达目标。
3. 空中机已在目标高度 ±0.5 米：两种指令都不发，执行成功。
4. 空中机、飞控拒绝调高：执行失败，不退回起飞。
5. 任务级：飞机已在空中时启动带起飞阶段的任务，阶段不发 `NAV_TAKEOFF` 并推进到下一阶段。
6. 双机均在空中：两次下发间隔 < 1500 ms。阈值不取 2500：起飞错峰用粗精度定时器，
   可提前至多 5% 触发（旧行为实测 2369 ms），取 2500 会让旧行为也通过。

**SITL 实测（5 机 PX4）**

机群已在空中时切换任务：确认无 Takeoff 模式切换、各机到达新任务指定高度，任务推进到后续阶段。
飞控模式与高度取自各实例的 onboard 链路（1454x），不依赖 QGC 自身显示。

## 8. 验证记录（2026-09-29）

- 单元测试：`SwarmExecutionTest` 新增 7 例，其中 6 例在修复前失败（空中机仍收到 `NAV_TAKEOFF`；
  错峰间隔 2369 ms），另 1 例是地面起飞的回归保护，修复前后都通过；修复后 25/25 通过；`ctest -L "Unit|Integration" -LE "Flaky|Network"` 249/249 通过。
- SITL（PX4 v1.15.4，gz_x500）：1/3/4/5 号机约 30–33 米悬停时
  - 切到 `编队巡逻`（20 米）：起飞阶段 5.3 秒完成，长机降至 20.8 米并继续下降，编队启动；
  - 中止后切到 `快速出动待命`（35 米）：8.2 秒内各机升至 34.1–34.7 米；
  - 两次切换期间 PX4 进入 Takeoff 模式 0 次。
  - 这次没在 SITL 里复现修复前的行为，对旧行为的认定来自代码和上面的单元测试。
