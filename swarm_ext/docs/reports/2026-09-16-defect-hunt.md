# 交付后主动缺陷挖掘（2026-09-16）

基线提交 `b68a029a2`。收尾验证（[交付收尾验证记录](2026-09-16-delivery-closure.md)）给出
「产品代码未查出缺陷」后，本轮做一次**主动挖掘**：不满足于复跑求绿，而是针对交付声明
本身、以及测试套件结构上覆盖不到的配置去找问题。

结果：找到两个真实缺陷，关闭一个覆盖盲区，并记录一条本轮自身暴露的验证纪律问题。

## 发现一：`abort_swarm_task` 绕过自主等级闸与交付确认锁

**严重度：中**（安全方向的动作，但证伪了两处产品声明）

`SwarmAIAgent.cc:852` 在工具分发里直接调用：

```cpp
} else if (type == QStringLiteral("abort_swarm_task")) {
    _swarmManager->taskController()->abort();
}
```

这条路径不经 `_queueOrExecute`，因此**同时绕过**两道闸：

- `aiExecutionBlocked()` —— 只在 `_queueOrExecute`（`SwarmAIAgent.cc:1320`）和
  `confirmPlan()`（`:1344`）里检查，本路径两者都不经过；
- `_requireConfirm` —— 交付构建把它钉死为 `true`，同样只在 `_queueOrExecute` 生效。

### 实证

在锁定构建（`QGC_SWARM_AI_AUTODISPATCH=OFF`）中加入一条「若交付告示属实就应通过」的
用例：操作员亲自启动任务 → 模型请求 `abort_swarm_task` → 断言任务仍在运行。

```
PASS : SwarmDispatchLockTest::operatorTaskMustStillRun()
FAIL!: SwarmDispatchLockTest::modelAbortMustRequireConfirmation()
       '_manager->taskController()->running()' returned FALSE.
       (模型请求的 abort_swarm_task 未经人工确认即中止了任务，与交付告示不符)
```

同批其余 7 个用例全部通过，排除环境因素。探针补丁：`/tmp/abort-bypass-probe.patch`
（未提交，以免 `main` 变红）。

### 被证伪的两处声明

| 位置 | 声明 | 实际 |
| --- | --- | --- |
| `SwarmManager::deliveryNotice()` | 「本版本要求**所有** AI 动作经人工确认后执行」 | `abort_swarm_task` 不经确认 |
| `FEATURES.md` | 「L1 禁止自动执行」 | L1 下模型仍可中止运行中的任务 |

### 可辩护的一面

中止是**安全方向**的动作：它停掉自动化并让飞机悬停，把它像急停一样设计成无需确认是
合理的，设计文档 §12 也确实把它记为已知边界。

### 仍需处置的理由

1. 上表两处是无条件的事实声明，现在是假的。**行为与声明必须改一个。**
2. 可用性：模型（或经聊天/任务文本注入的提示）可反复中止，使任务编排不可用；
   而空中「暂停」意味着悬停，持续消耗续航。
3. L1 是「仅建议」档位，其存在意义就是让 AI 完全不能动执行器。这条路径破了这个不变量。

## 发现二：模型发起的中止被显示为「任务被操作员中止」

**严重度：高**（审计归因错误，且与产品核心前提直接冲突）

`SwarmTaskController.cc:268`：

```cpp
_finish(Outcome::Aborted, reason.isEmpty() ? tr("任务被操作员中止") : reason);
```

全仓生产调用点中，**唯一不传 reason 的就是模型路径**：

| 调用点 | reason | 用户看到 | 审计 `reason` |
| --- | --- | --- | --- |
| `SwarmAIPanel.qml:794`（操作员按钮） | `"操作员手动中止"` | 操作员手动中止 | `"操作员手动中止"` |
| `SwarmAIAgent.cc:852`（模型） | 空 | **任务被操作员中止** | `""` |
| `SwarmAcceptanceServer.cc:316` | `"acceptance_test"` | acceptance_test | `"acceptance_test"`（不入发布构建） |

即：UI 对模型动作做了**主动的错误归因**；审计日志不算错误归因，但记的是空字符串，
等于什么都没记——要靠读源码才知道「空 = AI」。

一个以「人工确认一切」为交付前提的系统，把模型动作显示成操作员动作，这一条没有可辩护的一面。

**最小修法**：模型路径传入明确的 reason（例如 `tr("AI 请求中止任务")`），
并把 `reason.isEmpty()` 的兜底文案从「任务被操作员中止」改成不指认发起者的中性措辞。

## 关闭的覆盖盲区：交付构建的 UI 与完整单元测试从未运行

此前锁定构建只跑过 `-L SwarmLocked`（1 项）。但该构建里实际存在
`SwarmTaskEditorUITest`、`SwarmSituationLayerUITest` 和完整的 187 项 Unit。

交付构建会隐藏自主等级控件（`aiAutodispatchAvailable` 为假），面板布局因此与非锁定构建
**不同**；而本分支刚修过一个布局缺陷（应用按钮被裁出可点击区域）。本轮补跑：

- 锁定构建 QML UI：**2/2 通过**
- 锁定构建 `-L Unit`：**187/187 通过**（无测试隐含假设自动下发可用）

交付构建的面板布局至此才第一次被验证。**建议把这两项纳入锁定构建的常规验证口径。**

## 本轮自身暴露的验证纪律问题

本轮对 `main` 跑了三次全量，前两次都被本人污染：

1. **PATH 遮蔽 python** —— 为满足 F1 固定版工具门禁，前置了一个 venv 的 `bin` 目录，
   其 python3.12 顶掉系统 3.8.18，`numpy`/`pymavlink` 消失，造成 4 个假失败。
   已在 [收尾报告](2026-09-16-delivery-closure.md) 发现一中记录并修入 README。
2. **验证期间并发跑重活** —— 在全量的 Integration 与 SITL 阶段，本人同时用 20 并行
   编译锁定构建并运行其测试。后果：8 机 SITL 场景全部在 6–8 秒内「SITL 提前退出」
   （仿真器进程起不来，典型资源饥饿），且 `SwarmSafetyTest` 失败一次。

第 2 条曾被本人一度误判为「新发现的功能性缺陷」。判定证据：

| 条件 | 结果 |
| --- | --- |
| 单跑 `SwarmSafetyTest` × 6 | 6/6 通过 |
| 完整 Integration × 2（无并发负载） | 65/65、65/65 |
| 满载（与 20 并行编译重叠） | 失败 1 次 |

合计 8 次干净通过 vs 1 次重负载失败，**没有证据支持产品缺陷**。

### 次级发现：该安全测试对负载敏感

按排除法，`policyRevocationMustStopQueuedAutomaticArm(manual-L3-to-L1)` 的失败点只能是
`planHasConstraintError`（`manual=true` 令前两个条件恒假，`approval=OperatorConfirmed`
令第三个恒假）。这意味着资源紧张时实时约束检查会失败，**CI 在繁忙机器上可能出现同样的
假失败**。严重度低，但值得在排查类似失败时优先考虑。

**建议**：README 补充一条——长验证运行期间不得在同一台机器上执行其他重负载任务。

## 处置建议

| 项 | 建议 | 理由 |
| --- | --- | --- |
| 发现二 | **应修** | 无可辩护面；三行改动 |
| 发现一 | **需决策**：改行为（中止也要确认）或改声明（限定「除中止外」） | 两种都自洽；改行为会削弱急停语义，改声明会让交付承诺变弱 |
| 覆盖盲区 | 纳入锁定构建常规口径 | 零成本，已验证通过 |
| 验证纪律 | 写入 README | 已两次实际造成假失败 |
