# 集群任务编排：待确认任务可编辑 + 模型不得直接下发 — 设计文档

> 日期：2026-09-12 · 分支：fix/verification-harness-gates（或新开）· 定位：能力补齐 + 交付范围收窄

## 1. 背景与目标

2026-09-10 全量验证的结论是：产品代码回归全绿，但**当前版本的模型意图准确率从未测量**，
且独立审计的缺陷发现曲线尚未走平。据此收窄交付范围。

初版设计把 AI 执行锁在 L1，等于只交付「纯人工」一种编排模式。评审指出产品要求是
**三种模式都要支持**：纯人工、纯大模型、人工+大模型；且模型生成的任务应当可以由人工
进一步修改、完善、确认，确认后再执行。

因此收窄的正确形态不是「模型的产物不得执行」，而是：

> **模型不得直接下发。模型的产物必须经操作员逐项审查/编辑后确认，才能进入执行管线。**

这带来两块工作：

- **Part A（能力补齐）**：待确认的多阶段任务目前**不可编辑**，人工确认只能整体点是/否。
  在这个前提下谈「人工确认」是橡皮图章，挡不住模型的错误。必须先补齐。
- **Part B（交付锁）**：编译期钉住「每一个模型动作都要人工确认」，使其不可被配置绕过。

**Part B 不得先于 Part A 落地**——否则交付的是一个「必须确认但确认时改不动」的系统。

## 2. 现状事实（设计前提）

### 2.1 统一表达已经存在

三种来源汇到同一条通道，产出的 `Stage` 对象没有任何区别：

```
人工：SwarmTaskLibrary::instantiate()   ──┐
大模型：SwarmAIAgent::_handleTaskJson()  ──┼─→ SwarmTaskController::parseStages()
验收接口：SwarmAcceptanceServer          ──┘        （统一校验）
                                                      ↓
                                          QList<Stage> → startTask()
```

`SwarmTaskLibrary.h:12` 已把这条通道写成契约。本设计**依赖并保持**该不变量，不引入
按来源分叉的第二套表达。

`Stage` 的结构（`SwarmTaskController.h:76`）：

```cpp
struct Stage {
    QString      name;
    QList<SwarmAction> actions;
    QString      conditionKind;    // all_airborne / all_disarmed / all_mission_finished
                                   // / formation_running / duration / manual
    QVariantMap  conditionParams;  // min_altitude / seconds / timeout_sec / grace_sec
};
```

### 2.2 缺口：待确认任务不可编辑

- `SwarmAIAgent::updatePendingAction()`（`:1071`）与 `pendingPlanParamsJson()`（`:1057`）
  都只遍历 `_pendingPlan`，即**普通动作**。
- 多阶段任务挂在 `_pendingStages`（`SwarmAIAgent.h:193`），`planSummary()`（`:148`）能把
  阶段名、动作、完成条件渲染出来供**阅读**，但没有任何编辑入口。
- QML 的参数编辑框（`SwarmAIPanel.qml:384/389`）绑的是 `pendingPlanParamsJson`，同样不覆盖任务。

### 2.3 `parseStages` 是 fail-closed 的

`SwarmTaskController.cc:128` 在遇到非法完成条件时 `return {}`，整体失败而非丢弃该阶段。
这对编辑路径是有利的：一次坏编辑会让整个重解析失败，可以安全拒绝并保留原状态。

（附带修正：该处警告文案写的是「该阶段已忽略」，与实际的整体失败行为不符，一并改掉。）

## 3. 范围

### 3.1 做

**Part A**

- 待确认多阶段任务的逐项编辑：**动作参数** + **阶段完成条件（含 kind 与参数）**。
- 编辑后经 `parseStages()` **整体重校验**，失败则拒绝编辑并保留原状态。
- QML 中按阶段/动作展开的编辑界面。

**Part B**

- CMake 选项 `QGC_SWARM_AI_AUTODISPATCH`，默认 `${QGC_BUILD_TESTING}`。
- `OFF` 时：自主等级钉死 L2、「所有 AI 动作均需确认」钉死开启，二者皆不可改、忽略持久化旧值。
- UI 移除档位下拉框、L3 白名单编辑器、确认开关；增加交付构建说明横幅。
- 回归测试与 Q2 发布审计断言。
- 手册 / FEATURES / README 同步。

### 3.2 不做（YAGNI）

- **增删阶段、调整阶段顺序**。需要结构性改动时重新向模型提要求生成，或后续另开
  「另存为模板」再用模板库编辑。本轮不做完整任务编辑器。
- **编辑 `set_selection`**。沿用 `updatePendingAction:1082` 的现有策略（拒绝并提示重新生成），
  理由不变：改目标机会让后续动作的目标一致性失效。见 §11。
- **任务来源 provenance 字段**。本设计不需要区分任务由谁生成——两种来源都要过同一道人工确认。
- **改动 L1 / L3 语义、`SwarmManager.cc:1382` 兜底闸、`executeActions` 的 `manual` 判据。**
  初版设计曾计划放宽它们；改用「钉 L2」后完全不需要，这些安全判据保持原样。

## 4. 关键设计决策：钉 L2 而不是钉 L1

初版把等级钉在 L1，会连带关掉多阶段任务状态机（任务以 `origin=="task"` 下发，
在 L1 被 `:1382` 兜底闸拒绝），为此需要放宽 6 处闸门——其中 `:1382` 是 `adee41030`
为修 QA 批次 P1 专门加的，SITL 场景 10 就是守它的，放宽它会让 L1 失去
「一键停掉所有自动化」的作用。

钉在 L2 则**一处闸门都不用动**：

| | 钉 L1（初版） | 钉 L2（本版） |
| --- | --- | --- |
| 任务状态机 | 被闸死，需放宽 6 处 | 正常工作，零改动 |
| `:1382` 兜底闸 | 必须放宽 | **保持满强度** |
| L1 的急停语义 | 被破坏 | **完全不变** |
| 模型自动下发 | 禁止 | 禁止（靠钉住确认开关） |
| 现有测试与场景 | 需改 L1 断言 | **零修改** |

在 L2 下，`SwarmAIAgent::_queueOrExecute()`（`:1117`，闸门在 `:1157`–`:1176`）的判据是
`needsConfirmation(plan) || _requireConfirm` → 挂起待确认。把 `_requireConfirm` 钉死为 true，
**每一个模型动作都必然走人工确认**，没有自动下发路径。多阶段任务本来就必过
`confirmPlan()`，无需额外处理。

## 5. Part A：待确认任务可编辑

### 5.1 数据模型

`SwarmAIAgent` 增加待确认任务的 JSON 原文，编辑作用在 JSON 上、再整体重解析：

```cpp
QJsonArray _pendingStagesJson;   ///< 与 _pendingStages 对应的原始 JSON，编辑的作用对象
```

三处设置 `_pendingStages` 的地方（`_handleTaskJson` 内 `:832`、`_runTaskTemplate` `:917`、
候选方案 `:969` 附近）同时保存对应 JSON——这三处本来就手里有 JSON。

**为什么编辑 JSON 而不是编辑 `Stage` 结构**：`parseStages()` 里的完成条件校验
（kind 白名单、`timeout_sec` ≥ 1、`duration` 的 `seconds`/`grace_sec`/`timeout_sec` 协调、
`all_airborne` 的最小高度、空动作阶段只允许 `manual`/`duration`）全部是内联的。
改 JSON 后重跑 `parseStages()` 可以零重复地复用**全部**校验，且与 `startTask()` 要求的
是同一条校验路径。

### 5.2 暴露给 QML 的模型

```cpp
Q_PROPERTY(QVariantList pendingTaskModel READ pendingTaskModel NOTIFY planChanged)
```

结构（每个阶段一项）：

```
{ name: "区域覆盖",
  conditionKind: "all_mission_finished",
  conditionParamsJson: "{\"timeout_sec\":600}",
  actions: [ { type: "plan_coverage",
               description: "...",
               paramsJson: "{\"length_m\":200,...}",
               editable: true } ] }        // set_selection 为 false
```

`pendingPlanParamsJson` 保持不变，继续服务普通动作路径。

### 5.3 编辑 API

```cpp
Q_INVOKABLE void updatePendingStageAction(int stageIndex, int actionIndex, const QString& paramsJson);
Q_INVOKABLE void updatePendingStageCondition(int stageIndex, const QString& conditionJson);
```

两者共用一条私有流程：

1. 索引与 JSON 合法性检查，失败 → `agentFeedback` 提示，不改状态。
2. `set_selection` 动作直接拒绝（沿用 `:1082` 策略与文案）。
3. 在 `_pendingStagesJson` 的**副本**上合并编辑值。
4. `parseStages(副本, 地图中心, warnings)` 整体重解析。
5. 结果为空 → **拒绝编辑**，保留原 `_pendingStagesJson` / `_pendingStages`，
   把 warnings 回报给操作员。
6. 通过 → 提交副本，`emit planChanged()`，审计记录一条编辑事件。

**判据只需看「结果是否为空」**：`parseStages` 的每一条 warning 后面都跟着 `return {}`
（条件类型、条件参数范围、`duration` 超时协调、动作非数组、`parseAndValidate` 产生的告警、
`all_airborne` 与起飞高度矛盾、非 `manual`/`duration` 阶段无动作），因此成功返回时
`warnings` 必为空、阶段数必等于输入数。额外再查 warnings 或阶段数是冗余的。

这同时说明：初始被接受的任务本身就是零 warning 解析出来的，不存在「基线告警」，
编辑时不需要跟历史 warnings 做差集。

### 5.4 审计

编辑动作要可追溯。复用 `SwarmAuditLogger`，新增事件：

```
log("plan", "pending_task_edited",
    { stage_index, action_index | -1, field: "action" | "condition",
      before: <JSON>, after: <JSON> })
```

`before`/`after` 只记被编辑的那个对象，不记整棵树。

### 5.5 QML

`SwarmAIPanel.qml` 计划确认区（`:346`–`:397`）现在对任务只渲染 `planSummary` 文本。
改为：`_ai.pendingTaskModel` 非空时用嵌套 Repeater 渲染——外层阶段，内层动作，
每个动作一个参数编辑框 + 应用按钮（复用 `:384/389` 的现有样式），每个阶段一个
完成条件编辑框 + 应用按钮。`editable: false` 的动作只读并显示原因。

`planSummary` 的文本渲染保留给普通动作路径。

## 6. Part B：交付构建锁

### 6.1 构建开关

仓库根 `CMakeLists.txt`，与既有 `QGC_VERBOSE_CONFIGURE`（`:42`）并列，
必须在 `QGC_BUILD_TESTING` 声明之后：

```cmake
option(QGC_SWARM_AI_AUTODISPATCH
       "Allow AI-authored actions to dispatch without per-plan operator confirmation"
       ${QGC_BUILD_TESTING})
```

`src/Swarm/CMakeLists.txt` 目前把源码 `target_sources` 进主目标，没有独立 Swarm 目标，
因此宏加在主目标上：

```cmake
if(NOT QGC_SWARM_AI_AUTODISPATCH)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QGC_SWARM_AI_AUTODISPATCH_DISABLED)
endif()
```

源码一律用宏做条件编译，不读 CMake 变量名。

### 6.2 钉死两个设置

`SwarmManager.cc:324` 的加载、`:334` `setAutonomyLevel`、以及 `SwarmAIAgent` 的
`setRequireConfirm`：

```cpp
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    _autonomyLevel = static_cast<int>(SwarmAutonomy::Level::L2Supervised);  // 忽略 QSettings 旧值
    _l3AllowedTypes.clear();
#else
    _autonomyLevel = qBound(1, settings.value(...).toInt(), 3);
#endif
```

- `setAutonomyLevel()` / `setL3AllowedTypes()` / `setRequireConfirm()` 在锁定构建下直接返回，
  不写 QSettings、不发信号。
- `_requireConfirm`（`SwarmAIAgent.h:186`）本来就不落 QSettings，只是个进程内成员，
  因此只需把初值在锁定构建下改为 `true` 并守住 setter，不涉及持久化迁移。
- 自主等级则是持久化的（`Swarm/autonomyLevel`），历史值 `3` 必须被忽略，
  否则装过旧版本的机器升级后会带着 L3 启动。这是两者处理方式不同的原因。

### 6.3 UI

`SwarmManager` 增只读属性，QML 不直接依赖编译宏：

```cpp
Q_PROPERTY(bool aiAutodispatchAvailable READ aiAutodispatchAvailable CONSTANT)
```

`SwarmAIPanel.qml` 在 `aiAutodispatchAvailable === false` 时：

| 位置 | 处理 |
| --- | --- |
| `:133` 档位下拉框 + `:139` 描述 | 隐藏 |
| `:147` L3 白名单编辑器 | 隐藏 |
| `:116`「所有 AI 动作均需确认」开关 | 隐藏（恒开） |
| 配置区顶部 | 增加常驻说明横幅（文案见 §6.4） |

`:396`「确认执行」按钮**保留**——这正是本版本要保住的人工确认入口。
模板库「执行」（`:637`）与多阶段任务控制（`:570`/`:578`）同样保留。

### 6.4 文案与审计标记

锁定构建需要一个可被发布审计检出的、且只在锁定构建编译的字符串。用 §6.3 的横幅承担：

```cpp
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    return tr("本版本要求所有 AI 动作经人工确认后执行，不提供自动下发");
#else
    return QString();
#endif
```

### 6.5 发布审计

`swarm_ext/tests/audit_release_artifact.py` 现有三种手法：读 `CMakeCache.txt`、
在二进制里数字节标记、`nm` 数符号。据此增两条断言：

1. `CMakeCache.txt` 中 `QGC_SWARM_AI_AUTODISPATCH:BOOL=OFF`（复用既有 `_cache_value`）。
2. 字节标记：§6.4 的横幅串**必须存在**。

不检查 `L2 监督执行` / `L3 限定域自主`：`SwarmAutonomy::levelName` 在两种构建下都编译，
这两个串必然存在，不具判别力。

`qualification.release_audit` 随之覆盖。

## 7. 三种编排模式的验收矩阵

| 模式 | 含义 | 解锁构建 | 锁定构建 |
| --- | --- | :---: | :---: |
| 纯人工 | 模板库选模板、填参数、执行 | ✓ | ✓ |
| 纯大模型 | 模型独立完成整个任务编排，人工只审查确认 | ✓ | ✓ |
| 人工 + 大模型 | 模型生成 → 人工逐项编辑 → 确认执行 | ✓ | ✓ |
| （模型自动下发） | 模型动作不经人工确认直接执行（L3 白名单） | ✓ | **✗** |

**已确认的产品决定（2026-09-12 评审）：模型生成的任务一律需要人工审查确认，不自动下发。**

因此「纯大模型」指的是**模型独立完成整个任务编排**，人工的职责是审查并确认，而不是
参与编排。它与「人工+大模型」的区别只在于人工是否动手修改，**两者都必须过确认闸**。

由此推论，本设计移除的不是某种编排模式，而是**免确认自动执行**这一条执行路径
（现由 L3 白名单提供）。三种编排模式在锁定构建里全部可用。

L3 在解锁构建中保留，理由是现有测试与 SITL 场景 07/10/11 依赖它来验证策略收紧逻辑；
它不进入交付产物。若后续确认免确认自动执行在产品上永不需要，可另案评估是否
连同解锁构建一起移除——本轮不做。

## 8. 测试策略

### 8.1 Part A（两种构建都适用）

`test/Swarm/SwarmExecutionTest.cc` 或新建 `SwarmTaskEditTest.cc`：

- 编辑阶段动作参数 → 重解析通过 → `_pendingStages` 更新、`planChanged` 发出
- 编辑完成条件 kind（如 `duration` → `manual`）→ 通过
- 编辑完成条件参数（`timeout_sec`）→ 通过
- **非法编辑被拒且状态不变**：`timeout_sec = 0`（违反 ≥ 1）、未知 kind、
  动作参数超范围 → 返回失败、`_pendingStagesJson` 与 `_pendingStages` 逐字节不变
- `set_selection` 动作编辑被拒，文案与 `:1082` 一致
- 索引越界（阶段/动作）被拒
- 编辑后 `confirmPlan()` 启动的任务，其阶段内容等于编辑后的值（端到端闭环）
- 审计中存在 `pending_task_edited` 事件且 before/after 正确

### 8.2 Part B（锁定构建）

**需要显式覆盖的构建组合**：`QGC_BUILD_TESTING=ON` **且**
`QGC_SWARM_AI_AUTODISPATCH=OFF`。默认值跟随 `QGC_BUILD_TESTING`，所以常规测试构建里
选项是 `ON`，锁定路径根本不参与编译——不显式配第二个构建目录，这批用例等于没跑。

实施：`run_all.py` 增 `qgc.build_locked` / `qgc.ctest_locked` 两项，在独立 build 目录
用上述组合配置，只跑带 `SwarmLocked` 标签的用例。

用例（新建 `test/Swarm/SwarmDispatchLockTest.cc`，标签 `Unit;SwarmLocked`）：

- `setAutonomyLevel(1)` / `(3)` 无效，`autonomyLevel()` 恒为 2
- QSettings 预置 `Swarm/autonomyLevel=3` 后构造，等级仍为 2，`l3AllowedTypes` 为空
- `setRequireConfirm(false)` 无效，`requireConfirm()` 恒为 true
- 非危险动作的 AI 计划**不会**自动执行，必然进入待确认态
- 经 `confirmPlan()` 后**可以**执行（确认路径没被误伤）
- 操作员模板任务可正常启动执行
- `aiAutodispatchAvailable == false`，横幅文案非空

### 8.3 不变量

现有 5 个依赖 L2/L3 的测试文件（`SwarmExecutionTest`、`SwarmSafetyTest`、
`SwarmRegressionTest`、`tst_pipeline`、`tst_p1modules`）与 SITL 场景 07/10/11
**不作修改**，必须继续通过。这是「解锁构建语义逐字不变」的唯一验收方式；
任何一处需要改断言都说明设计跑偏了。

## 9. 错误处理

- 编辑被拒：`agentFeedback` 给出 `parseStages` 的 warnings 原文，状态不变，操作员可重试。
- 锁定构建下 `setAutonomyLevel` / `setRequireConfirm` 被拒：静默返回，不发变更信号。
  QML 入口已隐藏，正常操作不会触发；经验收接口触发时由接口返回值反映。
- 编辑后约束仍不通过：沿用 `confirmPlan()` 现有的实时约束复检（`:1195`），
  在确认时拦截而非在编辑时，保持「确认前必过实时约束」这一既有保证。

## 10. 实施顺序（概要，详细留 writing-plans）

1. `_pendingStagesJson` 落位 + `pendingTaskModel` 属性（只读，不改行为）
2. 两个编辑 API + 重校验流程 + 审计事件
3. Part A 测试补齐（8.1）
4. QML 嵌套编辑界面
5. CMake 选项 + 宏 + `aiAutodispatchAvailable` 属性
6. 锁定构建测试配置（`qgc.build_locked` / `qgc.ctest_locked`）+ 一个必失败占位用例，
   先让锁定路径真的被编译执行，再写实现
7. 钉死两个设置 + 横幅文案
8. Part B 测试补齐（8.2）
9. 发布审计断言
10. 文档同步（手册 13/14/15 章、FEATURES、README）

## 11. 验收标准

- 解锁构建：现有全部 Unit / Integration / 独立 C++ / SITL 场景**零修改通过**。
- 锁定组合（`QGC_BUILD_TESTING=ON` + `QGC_SWARM_AI_AUTODISPATCH=OFF`）：8.2 全部通过。
- 非测试 Release（`QGC_BUILD_TESTING=OFF` → 选项默认 `OFF`）：§6.5 两条审计断言通过。
- `run_all.py --profile standard` 退出码 0，且 `qgc.build_locked` / `qgc.ctest_locked`
  为 PASS（不是 BLOCKED）。
- §7 矩阵中锁定构建标 ✓ 的三种模式，各有一条端到端用例覆盖。
- 手册中不再存在「切换至 L2/L3 后方可下发」这类对交付构建不成立的表述。

## 12. 已知边界

- **`set_selection` 不可编辑**。想改目标机只能重新生成整个任务。这是现有策略的延续，
  但在「人工+大模型」模式下会比较硌手——改高度可以，改派哪几架不行。
  若实测中这是主要诉求，应单独设计（需要处理后续动作的目标一致性），不要顺手放开。
- **不能增删阶段**。模型给了 5 个阶段但只需要 3 个时，只能重新生成。
- ~~**`abort_swarm_task` 仍是模型可直接调用且未过闸的路径**~~ —— **已于 2026-09-16 修复。**
  本轮判断「属降级方向动作，不改」是错的：该路径同时绕过自主等级闸与交付构建钉死的
  `_requireConfirm`，使「L1 禁止自动执行」和交付告示「所有 AI 动作经人工确认后执行」
  两处声明同时不成立。现改为经 `_requestTaskAbort()` 挂起待确认，操作员自己的中止按钮
  不受影响。详见 [缺陷挖掘报告](../reports/2026-09-16-defect-hunt.md)。
- **人工确认的质量取决于人真的审了**。Part A 只是让「审」变得可能，不保证它发生。
  交付培训材料应包含确认前的检查清单。
- 本设计不改变模型质量本身的未知性。若后续要放开自动下发，前置条件是 L7 意图准确率
  实测 + 缺陷发现曲线走平，不是简单把选项打开。
