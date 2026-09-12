# 待确认任务可编辑 + AI 自动下发构建锁 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让操作员能逐项编辑大模型生成的多阶段任务再确认执行，并用编译期开关钉死「模型动作必须经人工确认」，使交付构建不具备自动下发能力。

**Architecture:** Part A 把待确认任务的 JSON 原文留在 `SwarmAIAgent` 里，编辑作用于 JSON 副本后整体重跑 `SwarmTaskController::parseStages()`——该函数严格 fail-closed，返回非空即代表全部校验通过，所以「结果非空」就是唯一判据，零重复复用既有校验。Part B 新增 CMake 选项 `QGC_SWARM_AI_AUTODISPATCH`（默认 `${QGC_BUILD_TESTING}`），`OFF` 时把自主等级钉死 L2、把「所有 AI 动作均需确认」钉死为真，二者不可改且忽略持久化旧值；这条路径不触碰任何现有安全闸门。

**Tech Stack:** C++20 / Qt 6.11.1 / CMake + Ninja / QML / QTest（`VehicleTest` + MockLink）/ ctest 标签 / Docker 容器构建

**设计依据：** `swarm_ext/docs/specs/2026-09-12-swarm-task-authoring-and-dispatch-lock-design.md`

**强制顺序：** Part A（Task 1–7）必须全部完成并合入后，才能开始 Part B（Task 8–13）。否则交付的是「必须确认但确认时改不动」的系统。

---

## 文件结构

| 文件 | 职责 | Part |
| --- | --- | :---: |
| `src/Swarm/SwarmTaskController.cc` | 修正误导性告警文案（唯一改动） | A |
| `src/Swarm/SwarmAIAgent.h` | 待确认任务 JSON 原文、只读模型属性、两个编辑 API、私有重校验 | A/B |
| `src/Swarm/SwarmAIAgent.cc` | 上述实现；三处挂起点同时保存 JSON；锁定构建钉死 `_requireConfirm` | A/B |
| `test/Swarm/SwarmTaskEditTest.{h,cc}` | Part A 行为回归（两种构建都跑） | A |
| `src/FlyView/SwarmAIPanel.qml` | 阶段/动作嵌套编辑界面；锁定构建下的 UI 收敛 | A/B |
| `CMakeLists.txt`（仓库根） | `QGC_SWARM_AI_AUTODISPATCH` 选项 | B |
| `src/Swarm/CMakeLists.txt` | `QGC_SWARM_AI_AUTODISPATCH_DISABLED` 宏 | B |
| `src/Swarm/SwarmManager.{h,cc}` | 钉死自主等级、`aiAutodispatchAvailable`、交付横幅文案 | B |
| `test/Swarm/SwarmDispatchLockTest.{h,cc}` | Part B 行为回归（仅锁定构建） | B |
| `test/Swarm/CMakeLists.txt` | 注册两个新测试目标与标签 | A/B |
| `swarm_ext/tests/run_all.py` | `qgc.configure_locked` / `qgc.build_locked` / `qgc.ctest_locked` | B |
| `swarm_ext/tests/audit_release_artifact.py` | 两条发布锁断言 | B |
| `swarm_ext/docs/用户使用手册.md`、`FEATURES.md`、`README.md` | 文档同步 | B |

**注意：** 本计划把新测试的标签定为 `Integration Vehicle`（外加 Part B 的 `SwarmLocked`），
而不是 spec §8.2 写的 `Unit`。原因：这些用例需要 `VehicleTest` 提供的 MockLink 飞行器才能让
`parseAndValidate` 产出带目标机的动作。Spec 的标签写法应以本计划为准。

**每个改 C++ 的任务，提交前必须跑格式检查**（计划各任务的步骤里不再重复写）：

```bash
clang-format -i <本任务改过的 .h/.cc>
clang-format --dry-run --Werror <同上>   # 预期无输出
```

宿主的 clang-format 22.1.5 与 `.pre-commit-config.yaml` 钉住的版本一致。
漏掉这步会让 `source.clang_format` 这道 L0 门禁失败。
格式化后确认只动了本任务新增的行——若 clang-format 想重排既有代码，停下来上报，
不要把无关的重排混进提交。

**容器构建的挂载：** 源码目录必须挂成**可写**（`-v "$PWD:/project/source"`，不加 `:ro`）。
`build/ccache-launcher` 把 `CCACHE_DIR` 指向 `/project/source/.ccache`，只读挂载会让 ccache
在写临时文件时报 `Read-only file system` 而构建失败。`run_all.py` 的 `qgc.build`
（`qgc_mounts`，`:497`）用的也是可写挂载。只有纯执行测试的步骤可以用 `:ro`。

---

# Part A：待确认任务可编辑

## Task 1: 修正 parseStages 的误导性告警文案

`SwarmTaskController.cc:127` 的文案说「该阶段已忽略」，但代码是 `return {}` 整体失败。
Part A 的错误提示会把这些 warnings 直接展示给操作员，文案必须准确。

**Files:**
- Modify: `src/Swarm/SwarmTaskController.cc:127`、`:190`

- [ ] **Step 1: 改两处文案**

`src/Swarm/SwarmTaskController.cc` 第 127 行附近：

```cpp
        if (!kinds.contains(stage.conditionKind)) {
            warnings.append(tr("阶段「%1」完成条件类型无效（%2），整个任务已拒绝").arg(stage.name, stage.conditionKind));
            return {};
        }
```

同文件第 190 行附近：

```cpp
        if (stage.actions.isEmpty() && stage.conditionKind != QStringLiteral("manual") &&
            stage.conditionKind != QStringLiteral("duration")) {
            warnings.append(tr("阶段「%1」无有效动作，整个任务已拒绝").arg(stage.name));
            return {};
        }
```

- [ ] **Step 2: 确认没有测试断言这两条旧文案**

Run: `grep -rn "该阶段已忽略\|无有效动作，已忽略" test/ swarm_ext/ src/`
Expected: 无输出（若有命中，同步更新该断言）

- [ ] **Step 3: 提交**

```bash
git add src/Swarm/SwarmTaskController.cc
git commit -m "fix(Swarm): describe stage validation failures as task rejection

parseStages returns {} on any stage problem, so the warning saying the stage
was ignored misstated what happened. The pending-task editor surfaces these
strings to the operator, where an inaccurate one would be actively misleading."
```

---

## Task 2: 保存待确认任务的 JSON 原文

编辑要作用在 JSON 上再整体重解析，因此三处挂起点必须把 JSON 一并留下。

**Files:**
- Modify: `src/Swarm/SwarmAIAgent.h`（成员、`Candidate` 结构、`_handleTaskJson` 签名）
- Modify: `src/Swarm/SwarmAIAgent.cc:832`、`:917`、`:969`、`:1038`、`:870`

- [ ] **Step 1: 加成员与 Candidate 字段**

`src/Swarm/SwarmAIAgent.h`，在 `_pendingStages` 声明（约 :193）之后加：

```cpp
    QJsonArray _pendingStagesJson;  ///< 与 _pendingStages 对应的 JSON 原文，编辑的作用对象
```

`Candidate` 结构（约 :199）加一个字段：

```cpp
    struct Candidate
    {
        bool isTask = false;
        QList<SwarmAction> plan;                   ///< 动作型候选
        QList<SwarmTaskController::Stage> stages;  ///< 模板/任务型候选
        QJsonArray stagesJson;                     ///< 与 stages 对应的 JSON 原文
        QString taskName;
    };
```

若 `SwarmAIAgent.h` 尚未包含 `<QtCore/QJsonArray>`，在头部 include 区加上。

- [ ] **Step 2: 让 `_handleTaskJson` 回传 JSON**

`src/Swarm/SwarmAIAgent.h` 中该方法的声明改为：

```cpp
    bool _handleTaskJson(const QJsonObject& taskObj, QString& taskName,
                         QList<SwarmTaskController::Stage>& stages, QJsonArray& stagesJsonOut,
                         QStringList& warnings);
```

`src/Swarm/SwarmAIAgent.cc:870` 的定义相应改为：

```cpp
bool SwarmAIAgent::_handleTaskJson(const QJsonObject& taskObj, QString& taskName,
                                   QList<SwarmTaskController::Stage>& stages, QJsonArray& stagesJsonOut,
                                   QStringList& warnings)
{
    taskName = taskObj.value(QStringLiteral("task_name")).toString(tr("未命名任务"));
    const QJsonArray stagesJson = taskObj.value(QStringLiteral("stages")).toArray();
    if (stagesJson.isEmpty()) {
        warnings.append(tr("start_swarm_task 缺少 stages，已忽略"));
        return false;
    }
    stages = _swarmManager->taskController()->parseStages(
        stagesJson, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
    if (stages.isEmpty()) {
        warnings.append(tr("start_swarm_task 没有有效阶段，已忽略"));
        return false;
    }
    stagesJsonOut = stagesJson;
    return true;
}
```

- [ ] **Step 3: 三处挂起点保存 JSON**

`src/Swarm/SwarmAIAgent.cc:828` 附近（`_handleActionsJson` 的 `start_swarm_task` 分支）：

```cpp
        if (type == QStringLiteral("start_swarm_task")) {
            QString taskName;
            QList<SwarmTaskController::Stage> stages;
            QJsonArray stagesJson;
            QStringList warnings;
            if (_handleTaskJson(obj, taskName, stages, stagesJson, warnings)) {
                _pendingTask = true;
                _pendingTaskName = taskName;
                _pendingStages = stages;
                _pendingStagesJson = stagesJson;
            }
```

`src/Swarm/SwarmAIAgent.cc:917` 附近（`_runTaskTemplate` 结尾）：

```cpp
    _pendingTask = true;
    _pendingTaskName = templateName;
    _pendingStages = stages;
    _pendingStagesJson = stagesJson;
```

`src/Swarm/SwarmAIAgent.cc:969` 附近（候选的模板分支，`cand.stages = ...` 之后）：

```cpp
            cand.stagesJson = stagesJson;
```

`src/Swarm/SwarmAIAgent.cc:1036` 附近（`selectCandidate` 的任务分支）：

```cpp
    if (cand.isTask) {
        _pendingTask = true;
        _pendingTaskName = cand.taskName;
        _pendingStages = cand.stages;
        _pendingStagesJson = cand.stagesJson;
```

- [ ] **Step 4: 清空点同步**

`grep -n "_pendingStages.clear()" src/Swarm/SwarmAIAgent.cc` 找到全部清空处
（`confirmPlan` 成功后、`rejectPlan`、`clearHistory` 等），每处后面补一行：

```cpp
        _pendingStagesJson = QJsonArray();
```

- [ ] **Step 5: 编译**

```bash
docker run --rm -v "$PWD:/project/source" -v "$PWD/build:/project/build" \
  -w /project/build --user "$(id -u):$(id -g)" --entrypoint /usr/bin/cmake \
  qgc-ubuntu-docker:latest --build /project/build --config Debug --parallel 20
```
Expected: 编译成功，无新警告

- [ ] **Step 6: 提交**

```bash
git add src/Swarm/SwarmAIAgent.h src/Swarm/SwarmAIAgent.cc
git commit -m "refactor(Swarm): keep the JSON behind a pending multi-stage task

Editing a pending task means re-running parseStages over modified JSON, which
needs the original array rather than the parsed Stage objects. All three sites
that set _pendingStages already have that JSON in hand."
```

---

## Task 3: 暴露待确认任务的只读模型

**Files:**
- Modify: `src/Swarm/SwarmAIAgent.h`（属性 + 取值函数声明）
- Modify: `src/Swarm/SwarmAIAgent.cc`（实现，放在 `pendingPlanParamsJson` 之后，约 :1065）
- Test: `test/Swarm/SwarmTaskEditTest.{h,cc}`（本任务创建）
- Modify: `test/Swarm/CMakeLists.txt`

- [ ] **Step 1: 建测试骨架**

Create `test/Swarm/SwarmTaskEditTest.h`：

```cpp
#pragma once
#include <memory>

#include "BaseClasses/VehicleTest.h"
class SwarmManager;

class SwarmTaskEditTest : public VehicleTest
{
    Q_OBJECT
public:
    using VehicleTest::VehicleTest;
    ~SwarmTaskEditTest() override;
private slots:
    void init() override;
    void cleanup() override;
    void pendingTaskModelMustExposeStagesActionsAndCondition();

private:
    void _stagePendingTask();
    std::unique_ptr<SwarmManager> _manager;
};
```

Create `test/Swarm/SwarmTaskEditTest.cc`：

```cpp
#include "SwarmTaskEditTest.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include "SwarmAIAgent.h"
#include "SwarmManager.h"
#include "SwarmTaskController.h"
#include "Vehicle.h"

SwarmTaskEditTest::~SwarmTaskEditTest() = default;

void SwarmTaskEditTest::init()
{
    VehicleTest::init();
    QVERIFY(vehicle());
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("(集群任务|阶段 |阶段「|[0-9]+ 号机：)"));
    _manager = std::make_unique<SwarmManager>();
    _manager->setAutonomyLevel(2);
}

void SwarmTaskEditTest::cleanup()
{
    if (_manager)
        _manager->shutdown();
    _manager.reset();
    VehicleTest::cleanup();
}

// 通过 AI 代理的 JSON 入口挂起一个两阶段任务，等价于模型返回 start_swarm_task。
void SwarmTaskEditTest::_stagePendingTask()
{
    const QString json = QStringLiteral(R"([{
        "type": "start_swarm_task",
        "task_name": "编辑用例",
        "stages": [
            {"name": "起飞", "actions": [{"type": "takeoff", "altitude": 30}],
             "complete_when": {"kind": "all_airborne", "min_altitude": 25, "timeout_sec": 600}},
            {"name": "等待", "actions": [],
             "complete_when": {"kind": "duration", "seconds": 10, "timeout_sec": 600}}
        ]}])");
    const QJsonArray actions = QJsonDocument::fromJson(json.toUtf8()).array();
    _manager->aiAgent()->handleActionsJsonForTest(actions);
    QVERIFY(_manager->aiAgent()->planPending());
}

void SwarmTaskEditTest::pendingTaskModelMustExposeStagesActionsAndCondition()
{
    _stagePendingTask();
    const QVariantList model = _manager->aiAgent()->pendingTaskModel();
    QCOMPARE(model.count(), 2);

    const QVariantMap stage0 = model.at(0).toMap();
    QCOMPARE(stage0.value("name").toString(), QStringLiteral("起飞"));
    QCOMPARE(stage0.value("conditionKind").toString(), QStringLiteral("all_airborne"));
    const QJsonObject cond0 =
        QJsonDocument::fromJson(stage0.value("conditionParamsJson").toString().toUtf8()).object();
    QCOMPARE(cond0.value("timeout_sec").toInt(), 600);

    const QVariantList actions0 = stage0.value("actions").toList();
    QCOMPARE(actions0.count(), 1);
    const QVariantMap action0 = actions0.at(0).toMap();
    QCOMPARE(action0.value("type").toString(), QStringLiteral("takeoff"));
    QVERIFY(action0.value("editable").toBool());
    const QJsonObject params0 =
        QJsonDocument::fromJson(action0.value("paramsJson").toString().toUtf8()).object();
    QCOMPARE(params0.value("altitude").toDouble(), 30.0);

    QCOMPARE(model.at(1).toMap().value("conditionKind").toString(), QStringLiteral("duration"));
}
```

- [ ] **Step 2: 加测试用的 JSON 注入入口**

`_handleActionsJson` 是私有的，测试需要一个入口。`src/Swarm/SwarmAIAgent.h` 的 public 区加：

```cpp
#ifdef QGC_BUILD_TESTING
    /// 测试入口：直接喂入模型返回的动作数组，绕过网络请求。
    void handleActionsJsonForTest(const QJsonArray& actionsJson) { _handleActionsJson(actionsJson, QString()); }
#endif
```

`QGC_BUILD_TESTING` 宏是否已对该目标定义，用以下命令确认；若未定义，改用
`#ifndef QT_NO_DEBUG` 之外的既有测试宏，以仓库现状为准：

Run: `grep -rn "QGC_BUILD_TESTING" src/Swarm/ src/*.cc CMakeLists.txt | head`

- [ ] **Step 3: 注册测试目标**

`test/Swarm/CMakeLists.txt` 末尾追加：

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmTaskEditTest.cc SwarmTaskEditTest.h)
add_qgc_test(SwarmTaskEditTest LABELS Integration Vehicle)
```

- [ ] **Step 4: 跑测试确认失败**

```bash
docker run --rm -v "$PWD:/project/source" -v "$PWD/build:/project/build" \
  -w /project/build --user "$(id -u):$(id -g)" --entrypoint /usr/bin/cmake \
  qgc-ubuntu-docker:latest --build /project/build --config Debug --parallel 20
```
Expected: **编译失败**，`pendingTaskModel` 未声明

- [ ] **Step 5: 实现 pendingTaskModel**

`src/Swarm/SwarmAIAgent.h` 属性区（约 :57，`pendingPlanParamsJson` 旁）加：

```cpp
    Q_PROPERTY(QVariantList pendingTaskModel READ pendingTaskModel NOTIFY planChanged)
```

public 取值函数声明：

```cpp
    QVariantList pendingTaskModel() const;
```

`src/Swarm/SwarmAIAgent.cc`，在 `pendingPlanParamsJson()` 实现之后加：

```cpp
QVariantList SwarmAIAgent::pendingTaskModel() const
{
    QVariantList stagesOut;
    if (!_pendingTask)
        return stagesOut;
    for (const SwarmTaskController::Stage& stage : _pendingStages) {
        QVariantList actionsOut;
        for (const SwarmAction& action : stage.actions) {
            actionsOut.append(QVariantMap{
                {QStringLiteral("type"), action.type},
                {QStringLiteral("description"), action.description},
                {QStringLiteral("paramsJson"),
                 QString::fromUtf8(
                     QJsonDocument(QJsonObject::fromVariantMap(action.params)).toJson(QJsonDocument::Compact))},
                // 改目标机会让后续动作的目标一致性失效，沿用 updatePendingAction 的既有策略。
                {QStringLiteral("editable"), action.type != QStringLiteral("set_selection")},
            });
        }
        stagesOut.append(QVariantMap{
            {QStringLiteral("name"), stage.name},
            {QStringLiteral("conditionKind"), stage.conditionKind},
            {QStringLiteral("conditionParamsJson"),
             QString::fromUtf8(
                 QJsonDocument(QJsonObject::fromVariantMap(stage.conditionParams)).toJson(QJsonDocument::Compact))},
            {QStringLiteral("actions"), actionsOut},
        });
    }
    return stagesOut;
}
```

- [ ] **Step 6: 跑测试确认通过**

```bash
docker run --rm -v "$PWD:/project/source" -v "$PWD/build:/project/build" \
  -w /project/build --user "$(id -u):$(id -g)" --entrypoint /usr/bin/cmake \
  qgc-ubuntu-docker:latest --build /project/build --config Debug --parallel 20
cd build && ctest -R SwarmTaskEditTest --output-on-failure
```
Expected: `1 test passed`

- [ ] **Step 7: 提交**

```bash
git add src/Swarm/SwarmAIAgent.h src/Swarm/SwarmAIAgent.cc test/Swarm/SwarmTaskEditTest.h \
        test/Swarm/SwarmTaskEditTest.cc test/Swarm/CMakeLists.txt
git commit -m "feat(Swarm): expose a pending multi-stage task as a structured model

planSummary renders a pending task as flat text, which is enough to read it
but not to address a particular stage or action for editing. set_selection is
marked uneditable up front so the UI can say so rather than fail on apply."
```

---

## Task 4: 编辑阶段内的动作参数

**Files:**
- Modify: `src/Swarm/SwarmAIAgent.h`（`updatePendingStageAction` + 私有 `_applyPendingStagesJson`）
- Modify: `src/Swarm/SwarmAIAgent.cc`
- Test: `test/Swarm/SwarmTaskEditTest.{h,cc}`

- [ ] **Step 1: 写失败测试**

`test/Swarm/SwarmTaskEditTest.h` 的 private slots 追加：

```cpp
    void editingStageActionMustRevalidateAndApply();
    void invalidStageActionEditMustLeaveTaskUnchanged();
    void editingSetSelectionMustBeRejected();
    void outOfRangeStageOrActionIndexMustBeRejected();
```

`test/Swarm/SwarmTaskEditTest.cc` 追加：

```cpp
void SwarmTaskEditTest::editingStageActionMustRevalidateAndApply()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    agent->updatePendingStageAction(0, 0, QStringLiteral(R"({"altitude": 45})"));

    const QVariantMap stage0 = agent->pendingTaskModel().at(0).toMap();
    const QVariantMap action0 = stage0.value("actions").toList().at(0).toMap();
    const QJsonObject params =
        QJsonDocument::fromJson(action0.value("paramsJson").toString().toUtf8()).object();
    QCOMPARE(params.value("altitude").toDouble(), 45.0);
}

void SwarmTaskEditTest::invalidStageActionEditMustLeaveTaskUnchanged()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    const QVariantList before = agent->pendingTaskModel();

    // 起飞高度 5 米低于该阶段 all_airborne 的 min_altitude 25，parseStages 必须整体拒绝。
    agent->updatePendingStageAction(0, 0, QStringLiteral(R"({"altitude": 5})"));

    QCOMPARE(agent->pendingTaskModel(), before);
}

void SwarmTaskEditTest::editingSetSelectionMustBeRejected()
{
    const QString json = QStringLiteral(R"([{
        "type": "start_swarm_task", "task_name": "选机用例",
        "stages": [{"name": "选机并起飞",
                    "actions": [{"type": "set_selection", "vehicle_ids": [1]},
                                {"type": "takeoff", "altitude": 30}],
                    "complete_when": {"kind": "all_airborne", "min_altitude": 25, "timeout_sec": 600}}]}])");
    _manager->aiAgent()->handleActionsJsonForTest(QJsonDocument::fromJson(json.toUtf8()).array());
    QVERIFY(_manager->aiAgent()->planPending());

    SwarmAIAgent* agent = _manager->aiAgent();
    QVERIFY(!agent->pendingTaskModel().at(0).toMap().value("actions").toList().at(0).toMap()
                 .value("editable").toBool());

    const QVariantList before = agent->pendingTaskModel();
    agent->updatePendingStageAction(0, 0, QStringLiteral(R"({"vehicle_ids": [1, 2]})"));
    QCOMPARE(agent->pendingTaskModel(), before);
}

void SwarmTaskEditTest::outOfRangeStageOrActionIndexMustBeRejected()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    const QVariantList before = agent->pendingTaskModel();

    agent->updatePendingStageAction(5, 0, QStringLiteral(R"({"altitude": 45})"));
    agent->updatePendingStageAction(0, 9, QStringLiteral(R"({"altitude": 45})"));
    agent->updatePendingStageAction(-1, 0, QStringLiteral(R"({"altitude": 45})"));

    QCOMPARE(agent->pendingTaskModel(), before);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd build && ctest -R SwarmTaskEditTest --output-on-failure`
Expected: 编译失败，`updatePendingStageAction` 未声明

- [ ] **Step 3: 实现重校验与编辑**

`src/Swarm/SwarmAIAgent.h`，public 区加：

```cpp
    /// 编辑待确认任务中某个阶段的某个动作参数；参数与原值合并后整体重校验。
    Q_INVOKABLE void updatePendingStageAction(int stageIndex, int actionIndex, const QString& paramsJson);
```

private 区加：

```cpp
    /// 用候选 JSON 整体重跑 parseStages，通过才提交。失败时原状态逐字节不变。
    bool _applyPendingStagesJson(const QJsonArray& candidate, QString* error);
```

`src/Swarm/SwarmAIAgent.cc`，在 `updatePendingAction` 实现之后加：

```cpp
bool SwarmAIAgent::_applyPendingStagesJson(const QJsonArray& candidate, QString* error)
{
    QStringList warnings;
    const QList<SwarmTaskController::Stage> parsed = _swarmManager->taskController()->parseStages(
        candidate, QGeoCoordinate(_lastMapCenterLat, _lastMapCenterLon), warnings);
    // parseStages 严格 fail-closed：每条 warning 后面都跟着 return {}，
    // 因此「结果非空」即代表全部校验通过，不必另外检查 warnings 或阶段数。
    if (parsed.isEmpty()) {
        if (error) {
            *error = warnings.isEmpty() ? tr("阶段重新校验失败") : warnings.join(QStringLiteral("；"));
        }
        return false;
    }
    _pendingStagesJson = candidate;
    _pendingStages = parsed;
    return true;
}

void SwarmAIAgent::updatePendingStageAction(int stageIndex, int actionIndex, const QString& paramsJson)
{
    if (!_pendingTask || stageIndex < 0 || stageIndex >= _pendingStagesJson.count()) {
        emit agentFeedback(tr("阶段序号无效"));
        return;
    }
    QJsonObject stageObj = _pendingStagesJson.at(stageIndex).toObject();
    QJsonArray actions = stageObj.value(QStringLiteral("actions")).toArray();
    if (actionIndex < 0 || actionIndex >= actions.count()) {
        emit agentFeedback(tr("动作序号无效"));
        return;
    }
    QJsonObject actionObj = actions.at(actionIndex).toObject();
    if (actionObj.value(QStringLiteral("type")).toString() == QStringLiteral("set_selection")) {
        emit agentFeedback(tr("修改目标机请重新生成计划，以保持后续动作的目标一致"));
        return;
    }
    const QJsonObject edited = QJsonDocument::fromJson(paramsJson.toUtf8()).object();
    if (edited.isEmpty()) {
        emit agentFeedback(tr("参数 JSON 解析失败，请检查格式"));
        return;
    }
    const QJsonObject before = actionObj;
    // 与 updatePendingAction 一致：动作参数是合并语义，只覆盖给出的键。
    for (auto it = edited.constBegin(); it != edited.constEnd(); ++it)
        actionObj.insert(it.key(), it.value());
    actions[actionIndex] = actionObj;
    stageObj[QStringLiteral("actions")] = actions;
    QJsonArray candidate = _pendingStagesJson;
    candidate[stageIndex] = stageObj;

    QString error;
    if (!_applyPendingStagesJson(candidate, &error)) {
        emit agentFeedback(tr("编辑未生效：%1").arg(error));
        return;
    }
    emit planChanged();
    SwarmAuditLogger::instance()->log(
        QStringLiteral("plan"), QStringLiteral("pending_task_edited"),
        {{QStringLiteral("stage_index"), stageIndex},
         {QStringLiteral("action_index"), actionIndex},
         {QStringLiteral("field"), QStringLiteral("action")},
         {QStringLiteral("before"), QString::fromUtf8(QJsonDocument(before).toJson(QJsonDocument::Compact))},
         {QStringLiteral("after"), QString::fromUtf8(QJsonDocument(actionObj).toJson(QJsonDocument::Compact))}});
    _appendChat(tr("系统"), tr("阶段 %1 第 %2 项动作参数已更新").arg(stageIndex + 1).arg(actionIndex + 1));
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd build && ctest -R SwarmTaskEditTest --output-on-failure`
Expected: `1 test passed`（5 个用例全绿）

- [ ] **Step 5: 提交**

```bash
git add src/Swarm/SwarmAIAgent.h src/Swarm/SwarmAIAgent.cc test/Swarm/SwarmTaskEditTest.h \
        test/Swarm/SwarmTaskEditTest.cc
git commit -m "feat(Swarm): let operators edit action parameters in a pending task

The edit applies to the stage JSON and re-runs parseStages over the whole
array, so every completion-condition and cross-stage rule is enforced by the
same code that startTask requires -- none of it is duplicated here. A
rejected edit leaves the task byte-identical."
```

---

## Task 5: 编辑阶段完成条件

**Files:**
- Modify: `src/Swarm/SwarmAIAgent.h`、`src/Swarm/SwarmAIAgent.cc`
- Test: `test/Swarm/SwarmTaskEditTest.{h,cc}`

- [ ] **Step 1: 写失败测试**

`test/Swarm/SwarmTaskEditTest.h` 的 private slots 追加：

```cpp
    void editingStageConditionMustApply();
    void changingConditionToManualMustApply();
    void invalidConditionEditMustLeaveTaskUnchanged();
```

`test/Swarm/SwarmTaskEditTest.cc` 追加：

```cpp
void SwarmTaskEditTest::editingStageConditionMustApply()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    agent->updatePendingStageCondition(
        0, QStringLiteral(R"({"kind": "all_airborne", "min_altitude": 25, "timeout_sec": 900})"));

    const QJsonObject cond =
        QJsonDocument::fromJson(
            agent->pendingTaskModel().at(0).toMap().value("conditionParamsJson").toString().toUtf8())
            .object();
    QCOMPARE(cond.value("timeout_sec").toInt(), 900);
}

void SwarmTaskEditTest::changingConditionToManualMustApply()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    agent->updatePendingStageCondition(0, QStringLiteral(R"({"kind": "manual", "timeout_sec": 600})"));

    QCOMPARE(agent->pendingTaskModel().at(0).toMap().value("conditionKind").toString(),
             QStringLiteral("manual"));
}

void SwarmTaskEditTest::invalidConditionEditMustLeaveTaskUnchanged()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    const QVariantList before = agent->pendingTaskModel();

    // timeout_sec 下界是 1；未知 kind 也必须被拒。
    agent->updatePendingStageCondition(0, QStringLiteral(R"({"kind": "all_airborne", "timeout_sec": 0})"));
    QCOMPARE(agent->pendingTaskModel(), before);

    agent->updatePendingStageCondition(0, QStringLiteral(R"({"kind": "no_such_kind"})"));
    QCOMPARE(agent->pendingTaskModel(), before);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd build && ctest -R SwarmTaskEditTest --output-on-failure`
Expected: 编译失败，`updatePendingStageCondition` 未声明

- [ ] **Step 3: 实现**

`src/Swarm/SwarmAIAgent.h`，紧接 `updatePendingStageAction` 声明加：

```cpp
    /// 编辑待确认任务中某个阶段的完成条件；整个 complete_when 对象被替换后重校验。
    Q_INVOKABLE void updatePendingStageCondition(int stageIndex, const QString& conditionJson);
```

`src/Swarm/SwarmAIAgent.cc`，紧接 `updatePendingStageAction` 实现加：

```cpp
void SwarmAIAgent::updatePendingStageCondition(int stageIndex, const QString& conditionJson)
{
    if (!_pendingTask || stageIndex < 0 || stageIndex >= _pendingStagesJson.count()) {
        emit agentFeedback(tr("阶段序号无效"));
        return;
    }
    const QJsonObject edited = QJsonDocument::fromJson(conditionJson.toUtf8()).object();
    if (edited.isEmpty()) {
        emit agentFeedback(tr("完成条件 JSON 解析失败，请检查格式"));
        return;
    }
    QJsonObject stageObj = _pendingStagesJson.at(stageIndex).toObject();
    const QJsonObject before = stageObj.value(QStringLiteral("complete_when")).toObject();
    // 与动作参数不同，完成条件是整体替换语义：改 kind 时旧 kind 的参数必须能被删掉。
    stageObj[QStringLiteral("complete_when")] = edited;
    QJsonArray candidate = _pendingStagesJson;
    candidate[stageIndex] = stageObj;

    QString error;
    if (!_applyPendingStagesJson(candidate, &error)) {
        emit agentFeedback(tr("编辑未生效：%1").arg(error));
        return;
    }
    emit planChanged();
    SwarmAuditLogger::instance()->log(
        QStringLiteral("plan"), QStringLiteral("pending_task_edited"),
        {{QStringLiteral("stage_index"), stageIndex},
         {QStringLiteral("action_index"), -1},
         {QStringLiteral("field"), QStringLiteral("condition")},
         {QStringLiteral("before"), QString::fromUtf8(QJsonDocument(before).toJson(QJsonDocument::Compact))},
         {QStringLiteral("after"), QString::fromUtf8(QJsonDocument(edited).toJson(QJsonDocument::Compact))}});
    _appendChat(tr("系统"), tr("阶段 %1 的完成条件已更新").arg(stageIndex + 1));
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd build && ctest -R SwarmTaskEditTest --output-on-failure`
Expected: `1 test passed`（8 个用例全绿）

- [ ] **Step 5: 提交**

```bash
git add src/Swarm/SwarmAIAgent.h src/Swarm/SwarmAIAgent.cc test/Swarm/SwarmTaskEditTest.h \
        test/Swarm/SwarmTaskEditTest.cc
git commit -m "feat(Swarm): let operators edit stage completion conditions

Replaces the whole complete_when object rather than merging, so switching
kind drops the previous kind's parameters instead of leaving them stranded.
Turning any stage into a manual gate is the strongest review tool an operator
has, and it was previously reachable only by regenerating the task."
```

---

## Task 6: 端到端闭环——编辑后确认执行的是编辑后的内容

**Files:**
- Test: `test/Swarm/SwarmTaskEditTest.{h,cc}`

- [ ] **Step 1: 写测试**

`test/Swarm/SwarmTaskEditTest.h` private slots 追加：

```cpp
    void confirmAfterEditMustStartTheEditedTask();
    void pendingTaskEditMustBeAudited();
```

`test/Swarm/SwarmTaskEditTest.cc` 追加（需 `#include "SwarmAuditLogger.h"`）：

```cpp
void SwarmTaskEditTest::confirmAfterEditMustStartTheEditedTask()
{
    _stagePendingTask();
    SwarmAIAgent* agent = _manager->aiAgent();
    agent->updatePendingStageCondition(
        0, QStringLiteral(R"({"kind": "all_airborne", "min_altitude": 25, "timeout_sec": 777})"));
    agent->confirmPlan();

    SwarmTaskController* task = _manager->taskController();
    QVERIFY(task->running());
    QCOMPARE(task->stageCount(), 2);
    QCOMPARE(task->stageTimeoutSecForTest(0), 777);
}

void SwarmTaskEditTest::pendingTaskEditMustBeAudited()
{
    _stagePendingTask();
    const int before = _manager->auditEventCount();
    _manager->aiAgent()->updatePendingStageAction(0, 0, QStringLiteral(R"({"altitude": 45})"));
    QVERIFY(_manager->auditEventCount() > before);

    const QString exported = _manager->exportAudit();
    QVERIFY(!exported.isEmpty());
    QFile file(exported);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString contents = QString::fromUtf8(file.readAll());
    QVERIFY(contents.contains(QStringLiteral("pending_task_edited")));
}
```

`SwarmTaskEditTest.cc` 顶部补 `#include <QtCore/QFile>`。

- [ ] **Step 2: 加测试读取入口**

`src/Swarm/SwarmTaskController.h` public 区加：

```cpp
#ifdef QGC_BUILD_TESTING
    /// 测试入口：读取已启动任务某阶段的超时值，用于校验编辑是否真的生效。
    int stageTimeoutSecForTest(int index) const
    {
        return (index >= 0 && index < _stages.count())
                   ? _stages.at(index).conditionParams.value(QStringLiteral("timeout_sec"), -1).toInt()
                   : -1;
    }
#endif
```

- [ ] **Step 3: 跑测试确认通过**

Run: `cd build && ctest -R SwarmTaskEditTest --output-on-failure`
Expected: `1 test passed`（10 个用例全绿）

若 `confirmPlan()` 因实时约束未通过而未启动任务，检查 `init()` 里
`_manager->setAutonomyLevel(2)` 是否生效、MockLink 飞行器是否在线。

- [ ] **Step 4: 提交**

```bash
git add src/Swarm/SwarmTaskController.h test/Swarm/SwarmTaskEditTest.h test/Swarm/SwarmTaskEditTest.cc
git commit -m "test(Swarm): prove a confirmed task carries the operator's edits

Editing the model and starting the task are separate paths; without this the
suite would pass even if confirmPlan quietly started the pre-edit stages."
```

---

## Task 7: QML 嵌套编辑界面

**Files:**
- Modify: `src/FlyView/SwarmAIPanel.qml:346-397`（计划确认区）

- [ ] **Step 1: 在计划确认区加任务分支**

`src/FlyView/SwarmAIPanel.qml` 的计划确认区里，现有的 `planSummary` 文本 Repeater 之后、
按钮行（`:396`）之前，插入：

```qml
                    // ==================== 待确认任务：逐阶段编辑 ====================
                    Repeater {
                        model: _ai.pendingTaskModel
                        delegate: ColumnLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: ScreenTools.defaultFontPixelHeight / 4

                            QGCLabel {
                                text:      qsTr("阶段 %1「%2」").arg(index + 1).arg(modelData.name)
                                font.bold: true
                            }

                            Repeater {
                                model: modelData.actions
                                delegate: RowLayout {
                                    required property var modelData
                                    required property int index
                                    readonly property int stageIndex: parent.parent.index
                                    Layout.fillWidth: true

                                    QGCLabel {
                                        text: qsTr("动作 %1 %2").arg(index + 1).arg(modelData.type)
                                        Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 18
                                    }
                                    QGCTextField {
                                        id:               actionParamField
                                        text:             modelData.paramsJson
                                        enabled:          modelData.editable
                                        Layout.fillWidth: true
                                    }
                                    QGCButton {
                                        text:     modelData.editable ? qsTr("应用") : qsTr("不可改")
                                        enabled:  modelData.editable
                                        onClicked: _ai.updatePendingStageAction(stageIndex, index,
                                                                                actionParamField.text)
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                QGCLabel {
                                    text: qsTr("完成条件 %1").arg(modelData.conditionKind)
                                    Layout.minimumWidth: ScreenTools.defaultFontPixelWidth * 18
                                }
                                QGCTextField {
                                    id:               conditionField
                                    text:             modelData.conditionParamsJson
                                    Layout.fillWidth: true
                                }
                                QGCButton {
                                    text: qsTr("应用")
                                    onClicked: {
                                        var merged = JSON.parse(conditionField.text)
                                        merged.kind = modelData.conditionKind
                                        _ai.updatePendingStageCondition(index, JSON.stringify(merged))
                                    }
                                }
                            }
                        }
                    }
```

**说明：** 条件编辑框只暴露参数，`kind` 由上面的 `merged.kind` 带回，
因此界面上改参数不会意外丢掉 kind。改 `kind` 本身（例如换成 `manual`）
本轮通过直接编辑 JSON 完成——把 `kind` 写进参数框即可覆盖。

- [ ] **Step 2: qmllint 检查**

Run: `pre-commit run qmllint --files src/FlyView/SwarmAIPanel.qml`
Expected: Passed（若宿主缺 Qt 6 qmllint 而 BLOCKED，改在容器内跑同一命令）

- [ ] **Step 3: 目视确认**

按 `swarm_ext/docs/用户使用手册.md` 第 3 章用 Mock Link 起一个模拟载具，
让 AI 生成一个多阶段任务，确认面板出现逐阶段编辑框，改一个高度点「应用」后数值更新，
改成非法值时给出拒绝提示且原值不变。

- [ ] **Step 4: 提交**

```bash
git add src/FlyView/SwarmAIPanel.qml
git commit -m "feat(FlyView): render a pending task as editable stages

Reading a model-authored task was possible before, changing it was not, so
confirming one was closer to a rubber stamp than a review."
```

---

## Part A 收尾检查

- [ ] **回归**

```bash
cd build && ctest --output-on-failure -L Unit
cd build && ctest --output-on-failure -L Integration -LE 'Flaky|Network'
```
Expected: 全绿，且**现有用例零修改**

- [ ] **契约与 lint**

```bash
PATH=/tmp/pinned-bin:$PATH python3 swarm_ext/tests/run_all.py --profile standard \
  --report-dir /tmp/partA-standard
```
Expected: 退出码 0

---

# Part B：AI 自动下发构建锁

> **前置条件：Part A 全部完成并合入。**

## Task 8: 构建开关与能力属性

**Files:**
- Modify: `CMakeLists.txt`（仓库根）
- Modify: `src/Swarm/CMakeLists.txt`
- Modify: `src/Swarm/SwarmManager.h`、`src/Swarm/SwarmManager.cc`

- [ ] **Step 1: 声明选项**

仓库根 `CMakeLists.txt`，在 `QGC_BUILD_TESTING` 声明**之后**（与 `QGC_VERBOSE_CONFIGURE`
所在的选项区并列）加：

```cmake
option(QGC_SWARM_AI_AUTODISPATCH
       "Allow AI-authored actions to dispatch without per-plan operator confirmation"
       ${QGC_BUILD_TESTING})
```

确认位置：`grep -n "QGC_BUILD_TESTING" CMakeLists.txt | head -3`，选项必须排在其后。

- [ ] **Step 2: 定义宏**

`src/Swarm/CMakeLists.txt` 末尾加：

```cmake
if(NOT QGC_SWARM_AI_AUTODISPATCH)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE QGC_SWARM_AI_AUTODISPATCH_DISABLED)
endif()
```

- [ ] **Step 3: 加能力属性与交付横幅**

`src/Swarm/SwarmManager.h` 属性区加：

```cpp
    Q_PROPERTY(bool aiAutodispatchAvailable READ aiAutodispatchAvailable CONSTANT)
    Q_PROPERTY(QString deliveryNotice READ deliveryNotice CONSTANT)
```

public 区加：

```cpp
    bool aiAutodispatchAvailable() const;
    QString deliveryNotice() const;
```

`src/Swarm/SwarmManager.cc`，在 `autonomyLevelDescription()` 实现之后加：

```cpp
bool SwarmManager::aiAutodispatchAvailable() const
{
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    return false;
#else
    return true;
#endif
}

QString SwarmManager::deliveryNotice() const
{
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    // 该串同时是发布审计的判别标记，见 swarm_ext/tests/audit_release_artifact.py。
    return tr("本版本要求所有 AI 动作经人工确认后执行，不提供自动下发");
#else
    return QString();
#endif
}
```

- [ ] **Step 4: 两种配置都能编译**

```bash
# 解锁（现有构建目录，选项默认 ON）
docker run --rm -v "$PWD:/project/source" -v "$PWD/build:/project/build" \
  -w /project/build --user "$(id -u):$(id -g)" --entrypoint /usr/bin/cmake \
  qgc-ubuntu-docker:latest --build /project/build --config Debug --parallel 20
```
Expected: 成功

- [ ] **Step 5: 提交**

```bash
git add CMakeLists.txt src/Swarm/CMakeLists.txt src/Swarm/SwarmManager.h src/Swarm/SwarmManager.cc
git commit -m "build(Swarm): add the AI autodispatch build option

Defaults to QGC_BUILD_TESTING so a delivery build is locked unless someone
opts in, and a test build keeps the levels the existing suite needs. The
option only carries capability reporting so far; the pins land next."
```

---

## Task 9: 锁定构建的测试配置

先把锁定路径真的编译并执行起来，再写实现——否则后面写的实现无从验证。

**Files:**
- Create: `test/Swarm/SwarmDispatchLockTest.{h,cc}`
- Modify: `test/Swarm/CMakeLists.txt`

- [ ] **Step 1: 建测试并注册标签**

Create `test/Swarm/SwarmDispatchLockTest.h`：

```cpp
#pragma once
#include <memory>

#include "BaseClasses/VehicleTest.h"
class SwarmManager;

class SwarmDispatchLockTest : public VehicleTest
{
    Q_OBJECT
public:
    using VehicleTest::VehicleTest;
    ~SwarmDispatchLockTest() override;
private slots:
    void init() override;
    void cleanup() override;
    void lockedBuildMustReportNoAutodispatch();

private:
    std::unique_ptr<SwarmManager> _manager;
};
```

Create `test/Swarm/SwarmDispatchLockTest.cc`：

```cpp
#include "SwarmDispatchLockTest.h"

#include <QtCore/QRegularExpression>

#include "SwarmAIAgent.h"
#include "SwarmManager.h"
#include "Vehicle.h"

SwarmDispatchLockTest::~SwarmDispatchLockTest() = default;

void SwarmDispatchLockTest::init()
{
    VehicleTest::init();
    QVERIFY(vehicle());
    ignoreLogMessage("API.QGCApplication.AppMessage", QtDebugMsg,
                     QRegularExpression("(集群任务|阶段 |阶段「|[0-9]+ 号机：)"));
    _manager = std::make_unique<SwarmManager>();
}

void SwarmDispatchLockTest::cleanup()
{
    if (_manager)
        _manager->shutdown();
    _manager.reset();
    VehicleTest::cleanup();
}

void SwarmDispatchLockTest::lockedBuildMustReportNoAutodispatch()
{
    QVERIFY(!_manager->aiAutodispatchAvailable());
    QVERIFY(!_manager->deliveryNotice().isEmpty());
}
```

`test/Swarm/CMakeLists.txt` 末尾加：

```cmake
if(NOT QGC_SWARM_AI_AUTODISPATCH)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmDispatchLockTest.cc SwarmDispatchLockTest.h)
    add_qgc_test(SwarmDispatchLockTest LABELS Integration Vehicle SwarmLocked)
endif()
```

- [ ] **Step 2: 配置锁定构建目录**

```bash
docker run --rm -v "$PWD:/project/source" -v "$PWD/build-locked:/project/build-locked" \
  -w /project/source --user "$(id -u):$(id -g)" \
  --entrypoint /opt/Qt/6.11.1/gcc_64/bin/qt-cmake qgc-ubuntu-docker:latest \
  -S /project/source -B /project/build-locked -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DQGC_BUILD_TESTING=ON -DQGC_SWARM_AI_AUTODISPATCH=OFF \
  -DQGC_LINK_COMPILE_COMMANDS=OFF -DQT_QML_GENERATE_QMLLS_INI=OFF \
  -DPython3_EXECUTABLE=/opt/qgc-venv/bin/python -DPython_EXECUTABLE=/opt/qgc-venv/bin/python
```
Expected: 配置成功

先建目录：`mkdir -p build-locked`

- [ ] **Step 3: 构建并运行锁定用例**

```bash
docker run --rm -v "$PWD:/project/source" -v "$PWD/build-locked:/project/build-locked" \
  -w /project/build-locked --user "$(id -u):$(id -g)" --entrypoint /usr/bin/cmake \
  qgc-ubuntu-docker:latest --build /project/build-locked --config Debug --parallel 20
cd build-locked && xvfb-run -a ctest -L SwarmLocked --output-on-failure
```
Expected: `1 test passed`

- [ ] **Step 4: 确认解锁构建里该用例不存在**

Run: `cd build && ctest -N -L SwarmLocked`
Expected: `Total Tests: 0`

- [ ] **Step 5: 提交**

```bash
git add test/Swarm/SwarmDispatchLockTest.h test/Swarm/SwarmDispatchLockTest.cc test/Swarm/CMakeLists.txt
git commit -m "test(Swarm): run the locked build's behaviour under its own label

The option defaults to QGC_BUILD_TESTING, so the locked path is not compiled
in a normal test build and would otherwise never be exercised."
```

---

## Task 10: 钉死自主等级与确认开关

**Files:**
- Modify: `src/Swarm/SwarmManager.cc:324`、`:334`、`:348`
- Modify: `src/Swarm/SwarmAIAgent.h:186`、`src/Swarm/SwarmAIAgent.cc:140`
- Test: `test/Swarm/SwarmDispatchLockTest.{h,cc}`

- [ ] **Step 1: 写失败测试**

`test/Swarm/SwarmDispatchLockTest.h` private slots 追加：

```cpp
    void autonomyLevelMustBePinnedToSupervised();
    void persistedAutonomyLevelMustBeIgnored();
    void requireConfirmMustBePinnedOn();
```

`test/Swarm/SwarmDispatchLockTest.cc` 追加（需 `#include <QtCore/QSettings>`）：

```cpp
void SwarmDispatchLockTest::autonomyLevelMustBePinnedToSupervised()
{
    QCOMPARE(_manager->autonomyLevel(), 2);
    _manager->setAutonomyLevel(3);
    QCOMPARE(_manager->autonomyLevel(), 2);
    _manager->setAutonomyLevel(1);
    QCOMPARE(_manager->autonomyLevel(), 2);
    QVERIFY(_manager->l3AllowedTypes().isEmpty());
}

void SwarmDispatchLockTest::persistedAutonomyLevelMustBeIgnored()
{
    QSettings().setValue(QStringLiteral("Swarm/autonomyLevel"), 3);
    auto fresh = std::make_unique<SwarmManager>();
    QCOMPARE(fresh->autonomyLevel(), 2);
    QVERIFY(fresh->l3AllowedTypes().isEmpty());
    fresh->shutdown();
    QSettings().remove(QStringLiteral("Swarm/autonomyLevel"));
}

void SwarmDispatchLockTest::requireConfirmMustBePinnedOn()
{
    QVERIFY(_manager->aiAgent()->requireConfirm());
    _manager->aiAgent()->setRequireConfirm(false);
    QVERIFY(_manager->aiAgent()->requireConfirm());
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cd build-locked && xvfb-run -a ctest -L SwarmLocked --output-on-failure
```
Expected: FAIL —— `autonomyLevel()` 为 2 但 `setAutonomyLevel(3)` 生效、`requireConfirm()` 为 false

- [ ] **Step 3: 钉死自主等级**

`src/Swarm/SwarmManager.cc` 第 324 行附近（构造时加载）：

```cpp
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    // 交付构建：等级固定在 L2 监督执行，忽略持久化旧值——装过旧版本的机器
    // 升级后不得带着 L3 启动。L3 白名单同时清空。
    _autonomyLevel = static_cast<int>(SwarmAutonomy::Level::L2Supervised);
    _l3AllowedTypes.clear();
#else
    _autonomyLevel = qBound(1, settings.value(QStringLiteral("Swarm/autonomyLevel"), 2).toInt(), 3);
    _l3AllowedTypes =
        settings
            .value(QStringLiteral("Swarm/l3AllowedTypes"), QVariant::fromValue(SwarmAutonomy::defaultL3AllowedTypes()))
            .toStringList();
#endif
```

（保留原有第 325–328 行的实际写法，上面 `#else` 分支按现状原样搬入。）

`setAutonomyLevel()`（约 :332）函数体开头加：

```cpp
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    Q_UNUSED(level)
    return;
#else
```

并在函数体末尾加 `#endif`。`setL3AllowedTypes()`（约 :346）同样处理。

- [ ] **Step 4: 钉死确认开关**

`src/Swarm/SwarmAIAgent.h` 第 186 行：

```cpp
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    bool _requireConfirm = true;   ///< 交付构建：每个 AI 动作都必须人工确认，不可关闭。
#else
    bool _requireConfirm = false;  ///< 可选：连低风险动作也逐项确认；强制规则独立生效。
#endif
```

`src/Swarm/SwarmAIAgent.cc:140` 的 `setRequireConfirm` 开头加：

```cpp
void SwarmAIAgent::setRequireConfirm(bool require)
{
#ifdef QGC_SWARM_AI_AUTODISPATCH_DISABLED
    Q_UNUSED(require)
    return;
#else
```

函数体末尾加 `#endif`。

- [ ] **Step 5: 跑测试确认通过**

```bash
docker run --rm -v "$PWD:/project/source" -v "$PWD/build-locked:/project/build-locked" \
  -w /project/build-locked --user "$(id -u):$(id -g)" --entrypoint /usr/bin/cmake \
  qgc-ubuntu-docker:latest --build /project/build-locked --config Debug --parallel 20
cd build-locked && xvfb-run -a ctest -L SwarmLocked --output-on-failure
```
Expected: `1 test passed`（4 个用例全绿）

- [ ] **Step 6: 解锁构建零回归**

```bash
cd build && ctest --output-on-failure -L Integration -LE 'Flaky|Network' -R 'Swarm'
```
Expected: 全绿，现有用例零修改

- [ ] **Step 7: 提交**

```bash
git add src/Swarm/SwarmManager.cc src/Swarm/SwarmAIAgent.h src/Swarm/SwarmAIAgent.cc \
        test/Swarm/SwarmDispatchLockTest.h test/Swarm/SwarmDispatchLockTest.cc
git commit -m "feat(Swarm): pin the delivery build to supervised, confirm-everything

Pinning L2 rather than L1 is what keeps this change free of gate edits: at L1
the task state machine dispatches with origin task and is refused by the
backstop at SwarmManager.cc:1382, which adee41030 added deliberately. At L2
tasks run as before and the model still cannot reach a vehicle, because
_requireConfirm forces every plan through confirmPlan.

The persisted autonomy level is ignored outright so a machine that once ran
L3 does not come back up unlocked after an upgrade."
```

---

## Task 11: 锁定构建的下发行为

证明「模型动作必须确认」和「确认路径没被误伤」两件事同时成立。

**Files:**
- Test: `test/Swarm/SwarmDispatchLockTest.{h,cc}`

- [ ] **Step 1: 写测试**

`test/Swarm/SwarmDispatchLockTest.h` private slots 追加：

```cpp
    void harmlessAiPlanMustNotAutoDispatch();
    void confirmedAiPlanMustDispatch();
    void operatorTaskMustStillRun();
```

`test/Swarm/SwarmDispatchLockTest.cc` 追加（需 `#include <QtCore/QJsonArray>`、
`<QtCore/QJsonDocument>`、`"SwarmTaskController.h"`、`"MockLink.h"`）：

```cpp
void SwarmDispatchLockTest::harmlessAiPlanMustNotAutoDispatch()
{
    // pause 不是危险动作：解锁构建在 L2 下会直接下发，锁定构建必须挂起待确认。
    const QString json = QStringLiteral(R"([{"type": "pause"}])");
    _manager->aiAgent()->handleActionsJsonForTest(QJsonDocument::fromJson(json.toUtf8()).array());

    QVERIFY(_manager->aiAgent()->planPending());
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION), 0);
}

void SwarmDispatchLockTest::confirmedAiPlanMustDispatch()
{
    const QString json = QStringLiteral(R"([{"type": "pause"}])");
    _manager->aiAgent()->handleActionsJsonForTest(QJsonDocument::fromJson(json.toUtf8()).array());
    QVERIFY(_manager->aiAgent()->planPending());

    _manager->aiAgent()->confirmPlan();
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION) > 0,
                             TestTimeout::longMs());
}

void SwarmDispatchLockTest::operatorTaskMustStillRun()
{
    const QString json = QStringLiteral(R"([{"name": "人工任务",
        "actions": [{"type": "pause"}],
        "complete_when": {"kind": "manual", "timeout_sec": 600}}])");
    QStringList warnings;
    const auto stages = _manager->taskController()->parseStages(
        QJsonDocument::fromJson(json.toUtf8()).array(), QGeoCoordinate(47.397, 8.546), warnings);
    QVERIFY(warnings.isEmpty());
    QCOMPARE(stages.count(), 1);

    QVERIFY(_manager->taskController()->startTask(QStringLiteral("人工任务"), stages, true));
    QVERIFY(_manager->taskController()->running());
    QTRY_VERIFY_WITH_TIMEOUT(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION) > 0,
                             TestTimeout::longMs());
}
```

`receivedMavCommandCount` 的确切名称以 `MockLink` 现有接口为准：
Run: `grep -n "receivedMavCommandCount\|clearReceivedMavCommandCounts" src/Comms/MockLink/MockLink.h`

- [ ] **Step 2: 跑测试**

```bash
docker run --rm -v "$PWD:/project/source" -v "$PWD/build-locked:/project/build-locked" \
  -w /project/build-locked --user "$(id -u):$(id -g)" --entrypoint /usr/bin/cmake \
  qgc-ubuntu-docker:latest --build /project/build-locked --config Debug --parallel 20
cd build-locked && xvfb-run -a ctest -L SwarmLocked --output-on-failure
```
Expected: `1 test passed`（7 个用例全绿）

这三条应当**不需要改实现**即通过——Task 10 的两个钉死已经产生了该行为。
若 `harmlessAiPlanMustNotAutoDispatch` 失败，说明 `_requireConfirm` 没生效，回到 Task 10 Step 4。

- [ ] **Step 3: 提交**

```bash
git add test/Swarm/SwarmDispatchLockTest.h test/Swarm/SwarmDispatchLockTest.cc
git commit -m "test(Swarm): prove the lock blocks autodispatch without blocking confirm

A lock that also broke the confirm path would look identical in a test that
only checked that nothing dispatched."
```

---

## Task 12: UI 收敛与发布审计

**Files:**
- Modify: `src/FlyView/SwarmAIPanel.qml:116`、`:133`、`:139`、`:147`
- Modify: `swarm_ext/tests/audit_release_artifact.py`
- Modify: `swarm_ext/tests/run_all.py`

- [ ] **Step 1: UI 收敛**

`src/FlyView/SwarmAIPanel.qml`：

- `:116`「所有 AI 动作均需确认」开关所在项加 `visible: _swarm.aiAutodispatchAvailable`
- `:133` 档位下拉框加 `visible: _swarm.aiAutodispatchAvailable`
- `:139` 档位描述 QGCLabel 加 `visible: _swarm.aiAutodispatchAvailable`
- `:147` L3 白名单容器把现有条件改为
  `visible: _swarm.aiAutodispatchAvailable && _swarm.autonomyLevel === 3`

在配置区顶部加常驻横幅：

```qml
                    QGCLabel {
                        text:             _swarm.deliveryNotice
                        visible:          _swarm.deliveryNotice !== ""
                        wrapMode:         Text.WordWrap
                        color:            qgcPal.warningText
                        Layout.fillWidth: true
                    }
```

- [ ] **Step 2: 发布审计断言**

`swarm_ext/tests/audit_release_artifact.py`，`forbidden_markers` 定义之后加：

```python
    required_markers = [
        "本版本要求所有 AI 动作经人工确认后执行，不提供自动下发".encode("utf-8"),
    ]
```

在 `with mmap.mmap(...) as binary:` 块内，`marker_hits` 之后加：

```python
            required_hits = {
                marker.decode("utf-8"): _count_marker(binary, marker) for marker in required_markers
            }
```

`autodispatch = _cache_value(cache_path, "QGC_SWARM_AI_AUTODISPATCH")` 取值，
并把通过条件扩展为同时满足：

```python
    autodispatch = _cache_value(cache_path, "QGC_SWARM_AI_AUTODISPATCH")
    autodispatch_locked = autodispatch.upper() in ("OFF", "0", "FALSE", "NO")
    ...
    ok = (
        testing_enabled is False
        and sum(marker_hits.values()) == 0
        and symbol_hits == 0
        and autodispatch_locked
        and all(count > 0 for count in required_hits.values())
    )
```

并把 `autodispatch`、`autodispatch_locked`、`required_hits` 一并写入输出 JSON。

**注意：** 具体变量名以脚本现状为准（现有通过条件在 `:78-82`）。改完先跑一次确认逻辑没写反。

- [ ] **Step 3: run_all.py 增三项**

`swarm_ext/tests/run_all.py`，在 `qgc_prerequisite` 定义（`:499-502`）之后加锁定构建的
路径与挂载：

```python
    locked_build_dir = Path(args.build_dir).resolve().parent / "build-locked"
    locked_build_dir.mkdir(parents=True, exist_ok=True)
    locked_mounts = [(REPO_ROOT, "/project/source", False), (locked_build_dir, "/project/build-locked", False)]
    locked_test_mounts = [(REPO_ROOT, "/project/source", True), (locked_build_dir, "/project/build-locked", False)]
```

在 `qgc.ctest_integration` 的 `TestSpec` 之后、`qgc.lifecycle` 之前插入三项：

```python
            TestSpec(
                "qgc.configure_locked",
                "L4 QGC 集成",
                "配置锁定 AI 自动下发的测试版 QGC",
                _container_command(
                    args.container_image,
                    "/opt/Qt/6.11.1/gcc_64/bin/qt-cmake",
                    [
                        "-S",
                        "/project/source",
                        "-B",
                        "/project/build-locked",
                        "-G",
                        "Ninja",
                        "-DCMAKE_BUILD_TYPE=Debug",
                        "-DQGC_BUILD_TESTING=ON",
                        "-DQGC_SWARM_AI_AUTODISPATCH=OFF",
                        "-DQGC_LINK_COMPILE_COMMANDS=OFF",
                        "-DQT_QML_GENERATE_QMLLS_INI=OFF",
                        "-DPython3_EXECUTABLE=/opt/qgc-venv/bin/python",
                        "-DPython_EXECUTABLE=/opt/qgc-venv/bin/python",
                    ],
                    locked_mounts,
                    workdir="/project/source",
                    environment={"CCACHE_DIR": "/project/build-locked/.ccache"},
                    user=f"{os.getuid()}:{os.getgid()}",
                ),
                timeout_sec=1800,
                min_profile="full",
                prerequisite=_docker_image(args.container_image),
            ),
            TestSpec(
                "qgc.build_locked",
                "L4 QGC 集成",
                "编译锁定 AI 自动下发的测试版 QGC",
                _container_command(
                    args.container_image,
                    "/usr/bin/cmake",
                    [
                        "--build",
                        "/project/build-locked",
                        "--config",
                        "Debug",
                        "--parallel",
                        str(args.jobs),
                    ],
                    locked_mounts,
                    workdir="/project/source",
                    user=f"{os.getuid()}:{os.getgid()}",
                ),
                timeout_sec=7200,
                min_profile="full",
                prerequisite=_docker_image(args.container_image),
            ),
            TestSpec(
                "qgc.ctest_locked",
                "L4 QGC 集成",
                "在锁定构建中执行 SwarmLocked 用例，验证交付构建不提供自动下发",
                _container_command(
                    args.container_image,
                    "/usr/bin/python3",
                    [
                        "/project/source/.github/scripts/cmake_helper.py",
                        "ctest",
                        "--junit-output",
                        "/project/build-locked/qgc-swarm-locked.xml",
                        "--ctest-output",
                        "/project/build-locked/qgc-swarm-locked.txt",
                        "--jobs",
                        "1",
                        "--include-labels",
                        "SwarmLocked",
                    ],
                    locked_test_mounts,
                    workdir="/project/build-locked",
                    user=f"{os.getuid()}:{os.getgid()}",
                ),
                timeout_sec=3600,
                min_profile="full",
                prerequisite=_docker_image(args.container_image),
            ),
```

**`--include-labels` 的参数名以 `cmake_helper.py` 现状为准**：

Run: `grep -n "include-labels\|exclude-labels" .github/scripts/cmake_helper.py | head -4`

`locked_build_dir` 与 `build/` 同级（默认 `<repo>/build-locked`），因此不会污染既有构建目录，
且已被 F2 之外的 `.gitignore` 规则 `build*/` 覆盖（`:31`），不会出现在 `git status` 里。

- [ ] **Step 4: 端到端验证**

```bash
# Release 锁定产物审计
python3 swarm_ext/tests/run_all.py --profile qualification \
  --only qualification.release_configure,qualification.release_build,qualification.release_audit \
  --report-dir /tmp/partB-release
```
Expected: 3/3 PASS，退出码 0

```bash
PATH=/tmp/pinned-bin:$PATH python3 swarm_ext/tests/run_all.py --profile full \
  --px4-src /media/hello/data1/Jinqiang/PX4-Autopilot --sitl-backend local \
  --report-dir /tmp/partB-full
```
Expected: `qgc.configure_locked` / `qgc.build_locked` / `qgc.ctest_locked` 均为 PASS

- [ ] **Step 5: 提交**

```bash
git add src/FlyView/SwarmAIPanel.qml swarm_ext/tests/audit_release_artifact.py swarm_ext/tests/run_all.py
git commit -m "feat(Swarm): hide autodispatch controls and audit the delivery lock

The release audit checks both the CMake cache value and a banner string that
only compiles into a locked build; the level names are useless as markers
because SwarmAutonomy::levelName compiles either way."
```

---

## Task 13: 文档同步

**Files:**
- Modify: `swarm_ext/docs/用户使用手册.md`（13.1、13.3、14.2、14.3、15 章）
- Modify: `swarm_ext/docs/FEATURES.md`
- Modify: `swarm_ext/README.md`

- [ ] **Step 1: 手册**

- 13.1 配置接口表：删「所有 AI 动作均需确认」行，改为说明交付构建恒开且不可改
- 13.3 确认规则：加一段「交付构建的确认矩阵」——档位固定 L2、所有 AI 动作均需确认、
  L3 白名单不可用；并删掉全文中「切换至 L2/L3 后方可下发」这类对交付构建不成立的表述
  （`grep -n "切换至 L2/L3\|切换自主等级" swarm_ext/docs/用户使用手册.md`）
- 14.3 修改参数：补多阶段任务的逐阶段编辑说明，写明目标机不可改、阶段不可增删
- 15 章：写明模板任务与 AI 任务用同一套阶段表达，人工可在确认前调整

- [ ] **Step 2: FEATURES.md**

「AI 指挥」行的验证与边界补：交付构建不提供自动下发，模型产物一律经人工确认；
三种编排模式（纯人工 / 纯大模型 / 人工+大模型）均可用。

- [ ] **Step 3: README.md**

在功能概述里补一句交付构建的能力边界，并链接本设计文档。

- [ ] **Step 4: 文档检查**

```bash
pre-commit run markdownlint --files swarm_ext/docs/用户使用手册.md swarm_ext/docs/FEATURES.md swarm_ext/README.md
grep -n "切换至 L2/L3" swarm_ext/docs/用户使用手册.md
```
Expected: markdownlint 通过；grep 无输出

- [ ] **Step 5: 提交**

```bash
git add swarm_ext/docs/用户使用手册.md swarm_ext/docs/FEATURES.md swarm_ext/README.md
git commit -m "docs(Swarm): describe the delivery build's confirmation rules

The manual told operators to switch to L2 or L3 to dispatch, which is not
something a delivery build offers."
```

---

## 最终验收

- [ ] **解锁构建零修改通过**

```bash
cd build && ctest --output-on-failure -L Unit
cd build && ctest --output-on-failure -L Integration -LE 'Flaky|Network'
```
Expected: 全绿。`git diff main --stat -- test/Swarm/SwarmExecutionTest.cc test/Swarm/SwarmSafetyTest.cc test/Swarm/SwarmRegressionTest.cc swarm_ext/tests/cpp/tst_pipeline.cc swarm_ext/tests/cpp/tst_p1modules.cc` **应为空**

- [ ] **锁定构建通过**

```bash
cd build-locked && xvfb-run -a ctest -L SwarmLocked --output-on-failure
```
Expected: 7 个用例全绿

- [ ] **SITL 场景零修改通过**

```bash
umask 077 && openssl rand -hex 24 > /tmp/qgc-swarm-token
cd swarm_ext/sim && python3 strict_regression.py --px4-src /media/hello/data1/Jinqiang/PX4-Autopilot \
  --qgc-build "$PWD/../../build" --qgc-token-file /tmp/qgc-swarm-token \
  --sitl-backend local --qgc-mode managed-headless --only 07,10,11 --out /tmp/final-sitl
rm -f /tmp/qgc-swarm-token
```
Expected: 3/3 PASS，退出码 0

- [ ] **三种编排模式各一条端到端证据**

| 模式 | 证据 |
| --- | --- |
| 纯人工 | `SwarmDispatchLockTest::operatorTaskMustStillRun` |
| 纯大模型 | `SwarmDispatchLockTest::confirmedAiPlanMustDispatch` |
| 人工+大模型 | `SwarmTaskEditTest::confirmAfterEditMustStartTheEditedTask` |

- [ ] **Definition of Done**

```bash
just build
PATH=/tmp/pinned-bin:$PATH just lint
cd build && ctest --output-on-failure -L Unit
```
