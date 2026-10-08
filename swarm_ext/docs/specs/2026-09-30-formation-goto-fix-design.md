# 编队中的「飞往位置」修复 — 设计文档

> 日期：2026-09-30 · 定位：行为修正（编队运行时操作员 goto 失效、编队失联）

## 1. 背景与问题（SITL 5 机实测取证）

编队已建立且可切换队形时，操作员从地图下发「飞往位置」（`Go to location`）出现两类失效：

- **RC1 僚机跟不上移动的长机**：引导目标限速（`_horizontalTargetSpeed = 2 m/s`）作用于目标的
  **绝对运动**。长机被 goto 后以巡航速度（≈10 m/s）飞离，僚机槽位目的地随之移动，但每拍指令
  目标最多前进 2 m——差距按 (v−2) m/s 无限增大，编队散架。垂直方向同理（1 m/s 限速）。
- **RC2 成员静默失联**：`_updateFormation` 对无槽位成员直接 `continue`（不指令、不报告），
  `follower_count` 却照常计数。槽位丢失的现实入口：`_handover()` 跳过坐标瞬态无效/当时离线的
  僚机。SITL 实测：1 号机长时间零指令、悬停在长机旁，面板仍显示「编队运行中：长机 2，僚机 4 架」。
- **RC3 操作员 goto 僚机被编队覆盖**：地图 goto 只下发一次 `DO_REPOSITION`，编队以 1 Hz 重发
  槽位指令立即覆盖，飞机无法离开——操作员视角「goto 无效」。
- **RC4 重复指令报错**：编队每拍无条件下发 `DO_REPOSITION`。上一条尚未 ACK（链路慢，或该机正在
  执行其他引导指令，如空中调高）时，命令队列以重复指令拒发，并每拍向操作员弹出
  「Unable to send command: Waiting on previous response to same command.」。

链路取证（tcpdump，lo 口）：编队对僚机 3/4/5 每秒 1 条 COMMAND_INT(DO_REPOSITION) 且全部
ACK=0、坐标正确；僚机 1 零指令；操作员 takeoff_all 的 COMMAND_LONG 能到达全部 5 机。

## 2. 目标与非目标

**目标**

- 长机移动（含 goto）时僚机跟随，编队形状保持。
- 编队成员不再静默失联：槽位缺失被检测并自愈。
- 编队运行中，对任一编队成员的「飞往位置」都移动整个编队：对长机即长机飞往该点；对僚机则
  改由长机飞往该点，该僚机保持在自己的槽位（操作员 2026-09-30 确认的行为）。
- 编队不再因等待 ACK 的指令弹出重复指令错误。

**非目标**

- 不改 `maxGoToLocationDistance`（QGC 原生 1000 m 限制，提示已存在）。
- 不改引导动作抽屉的滑块确认交互。
- 不处理 ArduPilot 空中调高（沿用既有决定）。

## 3. 设计

### 3.1 引导目标：随槽位平移 + 有界追赶（RC1）

新增 `FormationController::advanceGuidanceTarget(current, previousDestination, destination, dt)`：

1. `base = current` 沿「上一拍槽位目的地 → 本拍槽位目的地」的位移矢量平移（高度同理）。
   跟随槽位（长机）运动不限速——这是编队飞行的本体。
2. 目标从 `base` 朝 `destination` 前进至多 `_horizontalTargetSpeed·dt`（高度
   `_verticalTargetSpeed·dt`）。限速只约束**追赶**，防止单拍大幅跳变与时钟跳变放大。
3. 首拍（无上一目的地）从实测位置起步，行为与现状一致。

`GuidanceTarget` 增加 `destination` 字段，仅在指令实际下发（含约束检查通过）时更新，
保持「被拒绝的目标不推进迁移」的既有语义（`activeFormationEditsUpdateTargets` 覆盖）。

### 3.2 槽位自愈（RC2）

`_updateFormation` 收集完僚机后，若存在无槽位成员 → 调用 `_rebuildSlots()` 重建并写状态文本。
静默 `continue` 分支仅在重建后仍缺槽位时兜底保留。

### 3.3 编队中的 goto 移动整个编队（RC3）

- 新增 `SwarmManager::gotoLocation(vehicle, coordinate, loiterRadius)`；Fly View 地图的
  「飞往位置」（`GuidedActionsController.actionGoto`）改经此入口下发。
- 编队未激活、目标是长机或不在编队目标集（`_formationTargetIds`）中时，行为与原生 QGC 相同：
  `vehicle->guidedModeGotoLocation(...)`。
- 目标是编队僚机时，改为 `leader->guidedModeGotoLocation(...)`：该僚机本身不收到 goto，
  其槽位随长机移动即跟随（依赖 3.1）。记 `formation / operator_goto_via_leader` 审计，反馈
  「N 号机在编队中：改由长机 M 飞往目标点，编队随长机移动」。
- 不在 `Vehicle` 中加 swarm 专用信号：是否改道由 swarm 层在下发前决定，僚机不会先收到一条
  随即被覆盖的指令。

### 3.4 等待 ACK 的僚机本拍跳过（RC4）

`_updateFormation` 在下发前检查 `isMavCommandPending(defaultComponentId, MAV_CMD_DO_REPOSITION)`，
为真则本拍跳过该僚机（不推进引导状态、不调用约束检查），下一拍 ACK 到达后照常下发。

## 4. 测试

`SwarmRegressionTest` 新增：

1. `guidanceTargetTracksSlotMotion`：纯函数断言——在槽位上时槽位每拍移动 10 m，目标随动到槽位；
   未在槽位时随槽位平移后仅以 2 m/s（垂直 1 m/s）追赶残余差距；首拍从实测位置起步。
   不用 MockLink：其遥测会覆盖测试注入的长机位置与航向，编队自身的 1 Hz 定时器也会插入周期。
2. `formationSkipsFollowerAwaitingRepositionAck`：上一条重定位未 ACK 时下一拍不下发、不弹错误，
   ACK 后恢复下发。
3. `formationRebuildsSlotsWhenMemberLacksOne`：编队运行中 `setMembers` 扩员（清空槽位的公开
   入口），下一拍所有成员恢复受令。
4. `operatorGotoMovesWholeFormation`：管理器级编队运行中，对僚机 goto → 长机收到指向目标点的
   DO_REPOSITION、该僚机未收到 goto 且继续收到槽位指令、反馈发出、编队保持激活；对长机 goto
   由长机执行；停止编队后对僚机 goto 由该僚机执行。

SITL 复验：5 机编队中对长机 goto → 4 僚机保持队形跟随；对僚机 goto → 长机飞往目标点、
全部僚机保持队形跟随；全程无 Takeoff 模式、无成员静默失联。

## 5. 验证记录（2026-09-30）

- 首版（僚机移出编队）实际未通过：`formationFollowsMovingLeader` 假设 15 拍内追赶到槽位，
  但 MockLink 按 sysid 错开初始位置（0.0001°/号），距槽位可达 30 m 以上，前提不成立而失败；
  `formationRebuildsSlotsWhenMemberLacksOne` 因 RC4 的重复指令弹窗在严格日志模式下间歇失败；
  类总耗时 175 s，超过 Integration 标签 120 s 超时。
- 本版：`operatorGotoMovesWholeFormation` 在改道实现前失败（长机未收到 goto），实现后通过；
  `formationSkipsFollowerAwaitingRepositionAck` 在去掉 3.4 的检查后失败、恢复后通过。
  `SwarmRegressionTest` 26 例全部通过，耗时约 85 s；`ctest -R "Swarm|Formation|FlyView|Guided"`
  7/7 通过（容器内，与 CI 同样的 ctest 调用）。
  `ctest -L "Unit|Integration" -LE "Flaky|Network"` 249/249 通过（容器内，与 CI 同命令）。
- SITL（5 机 PX4 gz，楔形，间距 15 m，分层 3 m，长机 1，经验收接口 `goto`——与地图同一入口
  `SwarmManager::gotoLocation`；误差为僚机在长机航向坐标系下相对编队基准偏移的偏差）：
  - A 对长机 goto 150 m：长机 33.7 s 到达；途中僚机 3/4/5 误差稳定在 15–19 m（拖后约一个间距），
    到达后约 7 s 收敛到 ≤0.4 m；编队全程激活。
  - B 对僚机 3 goto 125 m：审计 `operator_goto_via_leader`，长机 28.4 s 到达目标点，
    僚机 3 未单独飞往该点，到达后约 7 s 全部僚机收敛到 ≤0.2 m。
  - 操作员同时在 GUI 地图上对僚机 4 下发 goto，审计同样记为改由长机 1 执行。
- SITL 暴露的遗留问题（本修复未处理）：
  - **起始间距死锁**：SITL 中 1、2 号机出生点相距约 2.5 m。编队启动后 2 号机 60 s 内停在
    长机旁（机体系偏移 (2.4, −0.8) m），3/4/5 均到位。其槽位 (−13, +7.5) m 的直线追赶路径
    经过长机 0.7 m 以内，平滑目标每拍被最小间距（3 m）约束拒绝且不推进，因此一直不下发；
    长机 goto 离开后才解除。§1 中「1 号机零指令、悬停在长机旁」与此吻合，未必是缺槽位。
  - **途中拖后**：僚机与长机同为默认巡航速度，且 1 Hz 的 DO_REPOSITION 每条都按「到点停」执行，
    途中队形被拉长约一个间距。可用长机速度前馈或提高僚机指令速度改善。
