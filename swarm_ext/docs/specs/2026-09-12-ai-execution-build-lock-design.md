# AI 执行构建期锁定 — 设计文档

> 日期：2026-09-12 · 分支：fix/verification-harness-gates（或新开）· 定位：交付范围收窄（最小范围）

## 1. 背景与目标

2026-09-10 的全量验证结论：产品代码本身回归全绿，但**当前版本的模型意图准确率从未测量**
（历史 88% 被文档明令禁止沿用），且独立审计的缺陷发现曲线尚未走平——09-08 两轮审计分别查出
11 项（6 P1）与 9 项（3 P1），最近一轮距今仅数日。

据此收窄交付范围：**把模型移出执行回路**，交付一个不能由 AI 下发执行的构建，
同时**保住操作员授权的多阶段任务与模板库**（这条链路无模型参与，是确定性的）。

目标是让「交付构建不具备 AI 执行能力」成为**编译期事实**，而不是流程约定或运行期配置。

## 2. 范围

### 2.1 做

- 新增 CMake 选项 `QGC_SWARM_AI_EXECUTION`，默认取 `${QGC_BUILD_TESTING}`。
- `OFF` 时：自主等级钉死 L1、忽略持久化旧值、UI 移除档位与白名单入口。
- `OFF` 时：放宽统一执行管线，使「操作员已确认的 task 来源」等同手动，保住任务编排。
- `ON` 时：L1/L2/L3 语义与行为**逐字不变**。
- 对应回归测试与 Q2 发布审计断言。
- 手册 / FEATURES / README 补交付构建的能力边界。

### 2.2 不做（YAGNI）

- 不引入任务来源（Operator / Model）provenance 字段。经核实 `_runTaskTemplate` 只挂起
  `_pendingTask`，实际启动必过 `SwarmAIAgent::confirmPlan()`（`SwarmAIAgent.cc:1183` 有
  `aiExecutionBlocked()` 闸），模型无法绕过，不需要在 sink 侧再加一层识别。
- 不改运行期可切换的开关。锁必须是编译期的。
- 不动 `abort_swarm_task` 当前未过闸的现状（属降级方向动作，另案评估）。
- 不改 AI 计划的生成与审查展示。L1 保留计划预览是这个档位的价值所在。

## 3. 关键设计决策：放宽绑定构建锁，不绑定档位

`SwarmManager.cc:1382` 有一道显式的纵深防御兜底闸，原注释：

> 纵深防御兜底闸门:任何入口(含 task 状态机自动推进)在 L1 档位一律拒绝下发,
> 防止运行中降级自主等级或新增入口绕过前置 autonomy 闸门。

它是 `adee41030`（2026-09-09）为修 QA 批次 P1 专门加的，SITL 场景 10
（`10_policy_revocation`）就是守它的。

若把「operator-confirmed 的 task 放行」直接写进 L1 语义，后果是 **L1 不再能当
「一键停掉所有自动化」使用**——运行中切到 L1，已启动的模板任务会继续飞。这正是
QA 批次里「权限收紧后仍继续执行」那一类 P1 的形态。

因此放宽**只在 `QGC_SWARM_AI_EXECUTION=OFF` 时生效**：

| 构建 | 自主等级 | 兜底闸强度 | L1 能否急停自动化 |
| --- | --- | --- | --- |
| `ON`（开发 / CI） | L1/L2/L3 可选，语义不变 | 满强度 | 能 |
| `OFF`（交付） | 恒 L1 | 对 operator-confirmed task 放行 | 不适用 |

交付构建里不需要 L1 充当急停：该构建中模型本就无法下发，急停手段是「任务中止」
与操作员「暂停」（`origin=="operator"`，任何档位都放行）。

## 4. 构建开关

选项在仓库根 `CMakeLists.txt` 声明，与既有的 `QGC_VERBOSE_CONFIGURE`（:42）等并列，
必须放在 `QGC_BUILD_TESTING` 声明**之后**才能取到它的值：

```cmake
option(QGC_SWARM_AI_EXECUTION
       "Allow AI autonomy levels above L1 (AI-authored actions may be dispatched)"
       ${QGC_BUILD_TESTING})
```

`src/Swarm/CMakeLists.txt` 现在是把源码 `target_sources` 进主目标，没有独立的 Swarm 目标，
因此宏加在主目标上：

```cmake
if(NOT QGC_SWARM_AI_EXECUTION)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QGC_SWARM_AI_EXECUTION_DISABLED)
endif()
```

`swarm_ext/tests/cpp/CMakeLists.txt` 的独立测试工程同样需要能构造两种取值，
否则 8.1 的锁定构建用例无处运行。源码一律用宏做条件编译，不读 CMake 变量名。

## 5. 受影响的闸门与改法

`aiExecutionBlocked()` 目前被 9 处消费。锁定构建下需要区分「模型来源」与「操作员授权的任务」。

新增一个集中判据，避免把同一条件抄散到各处：

```cpp
// SwarmManager.h
/// 该来源/审批组合是否属于操作员授权的手动执行（锁定构建下含操作员确认的任务阶段）。
bool _operatorAuthorized(const QString& origin, ExecutionApproval approval) const;
```

```cpp
// SwarmManager.cc
bool SwarmManager::_operatorAuthorized(const QString& origin, ExecutionApproval approval) const
{
    if (origin == QStringLiteral("operator"))
        return true;
#ifdef QGC_SWARM_AI_EXECUTION_DISABLED
    // 交付构建：模型无法下发，操作员显式授权的任务阶段不受 L1 闸门限制。
    return origin == QStringLiteral("task") && approval == ExecutionApproval::OperatorConfirmed;
#else
    Q_UNUSED(approval)
    return false;
#endif
}
```

改动点（全部改为先问 `_operatorAuthorized`，再问 `aiExecutionBlocked`）。
行号是 2026-09-12 提交 `8efe45a60` 时的位置，仅供定位，实施时以函数名为准：

| 文件:行 | 函数 | 现状 | 改后 |
| --- | --- | --- | --- |
| `SwarmManager.cc:1382` | `_dispatchExecution` 前置兜底闸 | `if (aiExecutionBlocked(&r))` 一律拒 | 操作员授权时跳过该闸 |
| `SwarmManager.cc:1600` | `executeActions` | `manual = origin=="operator"` | `manual = _operatorAuthorized(origin, approval)` |
| `SwarmManager.cc:1039` | `authorizeTaskTargets` | L1 直接 return，任务拿不到租约 | 锁定构建下按 operator-confirmed 放行 |
| `SwarmManager.cc:1947` | `_policyAllowed` | 仅 `operator` + Confirmed 早返回 true | 用 `_operatorAuthorized(execution->origin, execution->approval)` |
| `SwarmManager.cc:856` | 延时下发 lambda | `automated && aiExecutionBlocked()` | 执行上下文属操作员授权时不拦 |
| `SwarmManager.cc:910` / `:1966` | 编队引导与收尾 | `_formationAutomated && aiExecutionBlocked()` | 同上，保住模板里的编队阶段 |

`:856` 的 lambda 现在只捕获 `automated`，拿不到来源；需要把执行上下文的授权结论
（一个 `bool operatorAuthorized`）一并捕获，而不是在 lambda 里重新推导。

`SwarmAIAgent.cc:1175`（计划下发 `executeActions(plan, "ai")`）与
`SwarmAIAgent.cc:1183`（`confirmPlan` 入口闸）**不改**——那是模型来源，必须继续被拒。

## 6. 自主等级钉死

`SwarmManager.cc:324` 的加载与 `:334` 的写入：

```cpp
#ifdef QGC_SWARM_AI_EXECUTION_DISABLED
    _autonomyLevel = static_cast<int>(SwarmAutonomy::Level::L1Suggest);   // 忽略 QSettings 旧值
    _l3AllowedTypes.clear();
#else
    _autonomyLevel = qBound(1, settings.value(...).toInt(), 3);
    ...
#endif
```

`setAutonomyLevel()` / `setL3AllowedTypes()` 在锁定构建下直接返回，不写 QSettings、不发信号。
持久化里的历史 `Swarm/autonomyLevel=3` 不会让交付构建解锁。

`SwarmAutonomy::levelDescription(L1Suggest)` 保持现有文案（语义在 `ON` 构建下未变）；
交付构建的额外说明由两处 `#ifdef` 文案承担——必须是条件编译而非运行期分支，
理由见 §8.3：

| 位置 | `ON` | `OFF` |
| --- | --- | --- |
| `SwarmManager::aiExecutionBlocked()` reason | `当前为 L1 仅建议档位：AI 计划仅展示，禁止下发执行`（不变） | `本版本不提供 AI 下发执行：AI 计划仅供审查` |
| `SwarmAIAgent.cc:1185` `agentFeedback` | `L1 仅建议档位：禁止下发，请在面板切换自主等级`（不变） | `本版本不提供 AI 下发执行：计划仅供审查` |

## 7. UI

`src/FlyView/SwarmAIPanel.qml`，通过 `SwarmManager` 新增的只读属性
`Q_PROPERTY(bool aiExecutionAvailable READ aiExecutionAvailable CONSTANT)` 驱动，
QML 不直接依赖编译宏：

- `:133` 档位下拉框与 `:139` 描述：`visible: _swarm.aiExecutionAvailable`
- `:147` L3 白名单编辑器：同上（现有条件 `autonomyLevel === 3` 在锁定构建下本就不成立，
  但显式加条件以免将来改动漏掉）
- `:116`「所有 AI 动作均需确认」开关：`visible: _swarm.aiExecutionAvailable`（锁定构建下无意义）
- `:396`「确认执行」按钮：`visible: _swarm.aiExecutionAvailable`，只留「放弃」
- `:363` 计划区标题：锁定构建下改为「AI 行动计划（仅供审查，本版本不提供下发）」

模板库「执行」按钮（`:637`）与多阶段任务控制（`:570` / `:578`）**保持可用**——这是本设计
要保住的能力。

## 8. 测试策略

### 8.1 新增（锁定构建）

**需要一个显式覆盖的构建组合**：`QGC_BUILD_TESTING=ON` **且**
`QGC_SWARM_AI_EXECUTION=OFF`。默认值跟随 `QGC_BUILD_TESTING`，所以常规测试构建里
选项是 `ON`，锁定路径根本不参与编译——不显式配第二个构建目录，这批用例等于没跑。

实施方案：`run_all.py` 增一项 `qgc.build_locked` + `qgc.ctest_locked`，
在独立 build 目录用上述组合配置，只跑带 `SwarmLocked` 标签的用例。
`swarm_ext/tests/cpp/CMakeLists.txt` 同理提供第二个配置。

用例（新建 `test/Swarm/SwarmAiLockTest.cc`，标签 `Unit;SwarmLocked`）：

- `setAutonomyLevel(2)` / `(3)` 无效，`autonomyLevel()` 恒为 1
- QSettings 预置 `Swarm/autonomyLevel=3` 后构造，等级仍为 1，`l3AllowedTypes` 为空
- `executeActions(plan, "ai")` 返回 false，审计记录 `execution_gate_blocked`
- `SwarmAIAgent::confirmPlan()` 不下发、不启动任务
- `startTask(..., operatorConfirmed=true)` 的阶段动作**可以**下发，且租约授权成功
- 模板里的编队阶段可启动（覆盖 `:910` / `:1966`）
- `aiExecutionAvailable == false`

### 8.2 不变（`ON` 构建）

现有 5 个依赖 L2/L3 的测试文件（`SwarmExecutionTest`、`SwarmSafetyTest`、
`SwarmRegressionTest`、`tst_pipeline`、`tst_p1modules`）与 SITL 场景 07/10/11 **不作修改**，
必须继续通过——这是「`ON` 构建语义逐字不变」的验收方式。

### 8.3 发布审计

`swarm_ext/tests/audit_release_artifact.py` 现有三种手法：读 `CMakeCache.txt` 取值、
在二进制里数字节标记、`nm` 数符号。按这三种能力增两条断言：

1. `CMakeCache.txt` 中 `QGC_SWARM_AI_EXECUTION:BOOL=OFF`（复用既有的 `_cache_value`）。
2. 字节标记：锁定构建特有的 reason 串 `本版本不提供 AI 下发执行` **必须存在**；
   解锁构建特有的 `请在面板切换自主等级`（`SwarmAIAgent.cc:1185`）**必须不存在**。
   后者要求该 `tr()` 文案在锁定构建下被 `#ifdef` 换掉，而不是运行期分支——否则两串
   都会编进二进制，审计失去判别力。这是 §6 文案改法的一条硬约束。

不检查 `L2 监督执行` / `L3 限定域自主`：`SwarmAutonomy::levelName` 在两种构建下都编译，
这两个串必然存在，不具备判别力。

`qualification.release_audit` 随之覆盖。

## 9. 错误处理

- 锁定构建下 AI 尝试下发：拒绝 + `swarmCommandFeedback` 提示 + 审计
  `safety/execution_gate_blocked`（沿用现有通道，不新增事件类型）。
- `setAutonomyLevel` 被拒：静默返回，不发 `autonomyLevelChanged`。QML 侧入口已隐藏，
  正常操作不会触发；若经验收接口触发，由接口自身的返回值反映。
- 任务阶段在锁定构建下被拒（不应发生）：走现有 `_blocked` 阻塞态，操作员可见并可重试。

## 10. 实施顺序（概要，详细留 writing-plans）

1. CMake 选项 + 宏定义 + `aiExecutionAvailable` 属性（不改行为，先打通开关）
2. 锁定构建的测试配置（`qgc.build_locked` / `qgc.ctest_locked` + 独立 C++ 第二配置）
   与一个必失败的占位用例——先让锁定路径真的被编译和执行，再写实现
3. `_operatorAuthorized()` 与 6 处闸门改造
4. 等级钉死与持久化忽略
5. 两处 `#ifdef` 文案（§6 表格）
6. QML 收敛
7. 补齐 8.1 全部用例
8. 发布审计断言
9. 文档同步（手册第 13 章确认矩阵、FEATURES、README）

## 11. 验收标准

- `ON` 构建：现有全部 Unit / Integration / 独立 C++ / SITL 场景**零修改通过**。
  这是「解锁构建语义逐字不变」的唯一验收方式，任何一处需要改断言都说明设计跑偏了。
- 锁定组合（`QGC_BUILD_TESTING=ON` + `QGC_SWARM_AI_EXECUTION=OFF`）：
  8.1 全部新增用例通过。
- 非测试 Release（`QGC_BUILD_TESTING=OFF` → 选项默认 `OFF`）：§8.3 两条审计断言通过。
- `run_all.py --profile standard` 退出码 0，且新增的 `qgc.build_locked` /
  `qgc.ctest_locked` 两项为 PASS（不是 BLOCKED）。
- 手册、FEATURES、README 中不再存在「切换至 L2/L3 后方可下发」这类对交付构建不成立的表述。

## 12. 已知边界

- 交付构建里 `abort_swarm_task` 仍是模型可直接调用且未过闸的路径。属降级方向动作
  （撤销自动动作并暂停任务成员），本轮不改，但应在交付说明中写明。
- 本设计不改变模型质量本身的未知性，只是把模型移出执行回路。若后续要恢复 AI 执行，
  前置条件是 L7 意图准确率实测 + 缺陷发现曲线走平，不是简单把选项打开。
