# 全量验证与缺陷挖掘（2026-09-20）

基线提交 `074579e29`。本轮在 `main` 上跑 full profile，并对上一轮新增的中止确认路径
做针对性审查。

## 一、结论

full profile 22 项：**PASS 21 / FAIL 1**。唯一的 FAIL 是 SITL，根因**不在本项目**——
本机另一个项目（`Swarm-Claw`）的 SITL 机队占用了 PX4 的实例号锁。

代码审查查出**三个真实缺陷，全部位于 2026-09-17 新增的模型中止确认路径**，即上一轮
为修复「`abort_swarm_task` 绕过闸门」而引入的代码。三个已全部修复。

## 二、分层结果

| 层级 | 结果 |
| --- | --- |
| L0 源码契约（8 项） | 全 PASS |
| L1 Python/算法（4 项） | 全 PASS |
| L2 场景解析 | PASS |
| L3 独立 C++ | PASS |
| L4 QGC 集成（4 项） | 全 PASS，Integration 714.9 s |
| L5 SITL 验收 | **FAIL**，见第三节 |
| L6 GUI/QML 验收 | PASS |

## 三、SITL 失败：跨项目的实例号锁冲突

11 个场景中 10 个在 6–8 秒内「提前退出」。上一轮新增的诊断当场点名了根因：

```
[launch] 根因：/tmp 存在上一轮遗留的 PX4 锁。确认无 px4 进程后清理：
INFO  [px4] PX4 server already running for instance 0
```

但**这个诊断是错的**。本轮跑前已清理过锁，且确认无 px4 进程。取证发现：

- 锁文件创建于 10:40:33–10:40:42，而 SITL 阶段 10:45:30 才开始；
- 当时有 5 个 px4 进程在运行，父进程是
  `/media/hello/data1/Jinqiang/Swarm-Claw/scripts/launchers/swarm_launcher.py --backend sitl --n 5`。

即**另一个项目的 SITL 机队**占用了 `px4_lock-0..4`。PX4 的 `/tmp/px4_lock-N` 是全机器
范围的每实例号锁，本机上任意两个使用相同实例号的机队无法共存。

### 上一轮诊断的缺陷：把活占用误判成孤儿，并给出危险建议

原诊断无条件断言「上一轮遗留的锁」。在本轮这种情形下照其建议删锁会**破坏对方正在运行
的机队**。虽然它给出的命令带 `pgrep -x px4 ||` 守卫因而不会真的删除，但标题的诊断是错的，
使用者只会看到命令静默无事发生。

### 修复：启动前预检，两种情形分开处置

`launch_swarm_sitl.sh` 新增预检：

- **有活 px4 进程** → 打印进程数、起始 pid 及其父进程命令行，明确「这不是遗留锁，
  不要删除」，退出码 **3（环境阻塞）**；
- **无 px4 进程但有锁** → 确认是孤儿，自动清理后继续，不再需要人工介入。

实测（当时机器上正有 10 个属于 Swarm-Claw 的 px4 进程）：

```
[launch] 本机已有 10 个 px4 进程在运行，实例号锁被占用。
[launch] 起始进程 pid=4079457，其父进程：.../Swarm-Claw/.venv/bin/python scripts/launchers/swarm_launcher.py --backend sitl --n 5 ...
[launch] 这不是遗留锁，不要删除 /tmp/px4_lock-*：那会破坏正在运行的机队。
退出码=3
```

对方机队 10 个进程完好，5 个锁一个未删。

退出码 3 现由 `strict_regression` 的两处进程退出检查识别为 `EnvironmentBlocked`
（原 `QGCEnvironmentBlocked`，现承载两种成因故改用通用名），`run_all.py` 据此判
**BLOCKED 而非 FAIL**。

## 四、三个缺陷：均在上一轮新增的中止确认路径

三者同源——中止路径是后加的，没有对齐周边路径的既有规范。

### 缺陷一：过期的中止请求会中止下一个任务（严重度：高）

`SwarmTaskController::_finish()` 只在**失败**路径清理挂起态：

```cpp
const bool success = outcome == Outcome::Succeeded;
if (!success) {
    _swarmManager->cancelTargets(targets, message);  // → invalidatePendingWork → rejectPlan
    _swarmManager->pauseTargets(targets);
}
```

于是：任务 A 运行中模型请求中止 → 任务 A **成功**结束 → `_pendingAbort` 未被清理 →
操作员启动任务 B → 面板上那条中止请求仍在且看不出已过期 → 确认后**中止的是任务 B**。

### 缺陷二：确认过期的中止会谎报成功（严重度：中）

`abort()` 对未运行任务直接 return，但原代码不看结果照样发「操作员已确认，任务中止。」
并写 `logPlanConfirmed`。任务若已成功结束，审计里会出现一条针对它的「已确认中止」。

### 缺陷三：模型的中止请求不留审计（严重度：中）

`_queueOrExecute` 的三条挂起路径都调 `logPlanPending`，`_requestTaskAbort` 一条没有。
「模型是否试图中止过」在操作员处置前无从追溯。

### 修复：把挂起的中止绑定到发起时的 taskId

`_taskId` 是每次 `startTask` 新生成的 UUID。确认时复核任务仍在运行且 id 未变，
一次校验同时挡住三种情形——任务已结束、被换成新任务、id 复用。不匹配时明确告知
「中止请求已过期，未执行任何操作」，记 `logPlanRejected` 而非 `logPlanConfirmed`。
请求发起时补记 `logPlanPending`。

这与代码库既有的 `executionToken` / `_taskGeneration` 思路一致，比新增信号连接更严
且活动部件更少。

### 测试差点不具区分度

新增 `staleAbortMustNotKillTheNextTask`。第一版用 `abort()` 结束首个任务——**实测在
移除守卫后照样通过**：中止走失败路径，`_finish` 会顺带清掉挂起态，根本没触及缺陷。
改用 `proceed()` 让任务**成功**结束后，移除守卫即以准确消息失败：

```
FAIL! : staleAbortMustNotKillTheNextTask() '_manager->taskController()->running()'
        returned FALSE. (过期的中止请求中止了另一个任务)
```

另需先等 `waitingForProceed()`——阶段未进入等待态时 `proceed()` 是空操作。

## 五、验证

| 项 | 结果 |
| --- | --- |
| 锁定构建 `-L SwarmLocked` | 3/3（含新增用例） |
| 锁定构建 `-L Unit` | 186/186 |
| 非锁定构建 `-L Unit` | 187/187 |
| 非锁定构建 `-R Swarm` | 6/6 |
| 离线 sim 测试 | 22/22 |
| `strict_regression --dry-run` | 12/12 |
| shellcheck 0.11.0 / ruff 0.15.20 / clang-format 22.1.5 | 全干净 |

SITL 实跑因实例号锁被占用未能完成，待对方机队空闲后补测。

## 六、遗留

- 本轮 SITL 未取得执行证据。上一轮（09-16）的 11/11 通过仍是最近一次完整 SITL 结论。
- 交付边界未变，见[交付收尾验证记录](2026-09-16-delivery-closure.md)第七节：
  真实硬件、模型意图准确率、Q1 稳定性、16–32 机规模、真实视频与模型、跨平台均无证据。
