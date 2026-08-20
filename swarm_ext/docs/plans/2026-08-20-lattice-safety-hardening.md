# Lattice 安全/可靠性收尾 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Swarm 集群平台补齐控制租约、任务持久化与 SITL 验收脚本,让内嵌应用满足最小 Lattice MVP 的控制安全(人工接管即释放)与重启可靠性。

**Architecture:** 两个纯 QtCore 可独立单测的模块——`SwarmControlLease`(per-vehicle 租约状态机,飞行模式监听自动释放)与 `SwarmPersistence`(任务态 JSON 快照 + 重启 RUNNING→PAUSED 恢复);`SwarmManager`/`SwarmTaskController` 只做薄集成。租约不持久化(重启回 OBSERVE_ONLY)。

**Tech Stack:** C++20 · QtCore · QtTest · CMake · Docker 镜像 `qgc-ubuntu-docker`(Qt6.11)· 独立测试用 `qt-cmake -S swarm_ext/tests/cpp` · 整机用 `deploy/docker/_docker-exec.sh --fuse`。

**容器测试命令模板**(每个 tst 任务用):
```bash
docker run --rm --entrypoint bash -v "$PWD":/project -w /project qgc-ubuntu-docker -lc \
  'qt-cmake -S swarm_ext/tests/cpp -B build-qa >/tmp/cfg.log 2>&1 && cmake --build build-qa --target <TARGET> -j4 >/tmp/bld.log 2>&1 && echo "[build OK]" || { tail -25 /tmp/bld.log; exit 1; } && ./build-qa/<TARGET>'
```

---

### Task 1: SwarmControlLease 模块(租约状态机)

**Files:**
- Create: `src/Swarm/SwarmControlLease.h`
- Create: `src/Swarm/SwarmControlLease.cc`
- Create: `swarm_ext/tests/cpp/tst_lease.cc`
- Modify: `src/Swarm/CMakeLists.txt`(target_sources 加两行)
- Modify: `swarm_ext/tests/cpp/CMakeLists.txt`(加 tst_lease 目标)

- [ ] **Step 1: 写 SwarmControlLease.h**

```cpp
#pragma once
#include <QtCore/QMap>
#include <QtCore/QString>

// per-vehicle 控制租约状态机(纯 QtCore,可独立单测)。
// 语义:自动化路径下发前对涉及机 requestLease;操作员在 QGC 切飞行模式即自动释放。
class SwarmControlLease
{
public:
    enum State { OBSERVE_ONLY, LEASE_REQUESTED, LEASED, RELEASED, MANUAL_OVERRIDE };
    struct Lease {
        int     vehicleId = 0;
        QString taskId;
        QString op;                 ///< 操作员
        State   state = OBSERVE_ONLY;
        qint64  expireMs = 0;
        QString token;
    };

    /// 授予租约(LEASED),返回 token;ttlMs 后过期需重新申请。
    QString requestLease(int vehicleId, const QString &taskId, const QString &op,
                         qint64 nowMs, qint64 ttlMs = 300000);
    /// 飞行模式变化:操作员模式 → MANUAL_OVERRIDE(释放)。
    void onFlightModeChanged(int vehicleId, const QString &mode);
    /// 是否可自动下发(LEASED 且未过期、未被接管)。
    bool canAutomate(int vehicleId, qint64 nowMs) const;
    /// 主动释放。
    void release(int vehicleId);
    /// 定时过期释放(LEASED 且 now>expire → RELEASED)。
    void expireCheck(qint64 nowMs);
    State stateOf(int vehicleId) const;

    /// 飞行模式是否属操作员接管模式(非 Mission/Offboard;未知一律视为操作员=保守)。
    static bool isOperatorMode(const QString &mode);

private:
    QMap<int, Lease> _leases;
};
```

- [ ] **Step 2: 写 tst_lease.cc(失败测试)**

```cpp
#include <QtTest/QtTest>
#include "SwarmControlLease.h"

class TstLease : public QObject { Q_OBJECT
private slots:
    void grantsAndAutomates();
    void flightModeOverrideReleases();
    void expiresAfterTtl();
    void perVehicleIndependent();
    void operatorModeClassification();
};

void TstLease::grantsAndAutomates() {
    SwarmControlLease l;
    QVERIFY(!l.canAutomate(1, 1000));                       // 无租约不可自动化
    const QString tok = l.requestLease(1, "t1", "alice", 1000);
    QVERIFY(!tok.isEmpty());
    QCOMPARE(l.stateOf(1), SwarmControlLease::LEASED);
    QVERIFY(l.canAutomate(1, 1000));                        // 授予后可自动化
}
void TstLease::flightModeOverrideReleases() {
    SwarmControlLease l;
    l.requestLease(1, "t1", "alice", 1000);
    l.onFlightModeChanged(1, "Hold");                       // 操作员接管
    QCOMPARE(l.stateOf(1), SwarmControlLease::MANUAL_OVERRIDE);
    QVERIFY(!l.canAutomate(1, 1000));                       // 接管后不可自动化
}
void TstLease::expiresAfterTtl() {
    SwarmControlLease l;
    l.requestLease(1, "t1", "alice", 1000, 5000);           // ttl 5s
    QVERIFY(l.canAutomate(1, 4000));
    l.expireCheck(7000);                                    // 超时
    QVERIFY(!l.canAutomate(1, 7000));
    QCOMPARE(l.stateOf(1), SwarmControlLease::RELEASED);
}
void TstLease::perVehicleIndependent() {
    SwarmControlLease l;
    l.requestLease(1, "t", "a", 1000);
    l.requestLease(2, "t", "a", 1000);
    l.onFlightModeChanged(1, "RTL");                        // 只接管 1 号
    QVERIFY(!l.canAutomate(1, 1000));
    QVERIFY( l.canAutomate(2, 1000));                       // 2 号不受影响
}
void TstLease::operatorModeClassification() {
    QVERIFY( SwarmControlLease::isOperatorMode("Position"));
    QVERIFY( SwarmControlLease::isOperatorMode("Hold"));
    QVERIFY( SwarmControlLease::isOperatorMode("Manual"));
    QVERIFY( SwarmControlLease::isOperatorMode("完全未知模式"));  // 未知→保守视为接管
    QVERIFY(!SwarmControlLease::isOperatorMode("Mission"));
    QVERIFY(!SwarmControlLease::isOperatorMode("Offboard"));
}

QTEST_GUILESS_MAIN(TstLease)
#include "tst_lease.moc"
```

- [ ] **Step 3: 挂 CMake**(两处)

`src/Swarm/CMakeLists.txt` 在 `SwarmConstraintChecker.h` 行后加:
```cmake
        SwarmControlLease.cc
        SwarmControlLease.h
```

`swarm_ext/tests/cpp/CMakeLists.txt` 末尾加:
```cmake
add_executable(tst_lease
    tst_lease.cc
    ${SWARM_SRC}/SwarmControlLease.cc
    ${SWARM_SRC}/SwarmControlLease.h
)
target_include_directories(tst_lease PRIVATE ${SWARM_SRC})
target_link_libraries(tst_lease PRIVATE Qt6::Core Qt6::Test)
add_test(NAME lease COMMAND tst_lease)
```

- [ ] **Step 4: 跑测试确认失败**(实现前;此时 SwarmControlLease.cc 尚无实现 → 链接失败)

Run: 容器模板,`<TARGET>=tst_lease`。Expected: build FAIL(undefined reference)。

- [ ] **Step 5: 写 SwarmControlLease.cc**

```cpp
#include "SwarmControlLease.h"

QString SwarmControlLease::requestLease(int vehicleId, const QString &taskId,
                                        const QString &op, qint64 nowMs, qint64 ttlMs)
{
    Lease ls;
    ls.vehicleId = vehicleId;
    ls.taskId    = taskId;
    ls.op        = op;
    ls.state     = LEASED;
    ls.expireMs  = nowMs + ttlMs;
    ls.token     = QStringLiteral("lease-%1-%2").arg(vehicleId).arg(nowMs);
    _leases.insert(vehicleId, ls);
    return ls.token;
}

void SwarmControlLease::onFlightModeChanged(int vehicleId, const QString &mode)
{
    if (!isOperatorMode(mode)) {
        return;                                 // 自动化模式,不影响租约
    }
    auto it = _leases.find(vehicleId);
    if (it != _leases.end()) {
        it->state = MANUAL_OVERRIDE;            // 操作员接管即释放
    } else {
        Lease ls; ls.vehicleId = vehicleId; ls.state = MANUAL_OVERRIDE;
        _leases.insert(vehicleId, ls);
    }
}

bool SwarmControlLease::canAutomate(int vehicleId, qint64 nowMs) const
{
    const auto it = _leases.constFind(vehicleId);
    if (it == _leases.constEnd()) {
        return false;                           // 无租约只读
    }
    return it->state == LEASED && nowMs <= it->expireMs;
}

void SwarmControlLease::release(int vehicleId)
{
    auto it = _leases.find(vehicleId);
    if (it != _leases.end()) {
        it->state = RELEASED;
    }
}

void SwarmControlLease::expireCheck(qint64 nowMs)
{
    for (auto it = _leases.begin(); it != _leases.end(); ++it) {
        if (it->state == LEASED && nowMs > it->expireMs) {
            it->state = RELEASED;
        }
    }
}

SwarmControlLease::State SwarmControlLease::stateOf(int vehicleId) const
{
    const auto it = _leases.constFind(vehicleId);
    return (it == _leases.constEnd()) ? OBSERVE_ONLY : it->state;
}

bool SwarmControlLease::isOperatorMode(const QString &mode)
{
    // 自动化模式白名单;其余(含未知)一律视为操作员接管(保守)。
    static const QStringList kAutomation = {
        QStringLiteral("Mission"), QStringLiteral("Offboard")
    };
    for (const QString &a : kAutomation) {
        if (mode.compare(a, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 6: 跑测试确认通过** — Run 容器模板 `<TARGET>=tst_lease`。Expected: `Totals: 5 passed, 0 failed`(+init/cleanup)。

- [ ] **Step 7: Commit**
```bash
git add src/Swarm/SwarmControlLease.cc src/Swarm/SwarmControlLease.h \
        src/Swarm/CMakeLists.txt swarm_ext/tests/cpp/tst_lease.cc swarm_ext/tests/cpp/CMakeLists.txt
git commit -m "feat(swarm): 控制租约 SwarmControlLease(per-vehicle 状态机+飞行模式释放)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: SwarmPersistence 模块(任务 JSON 快照 + 恢复)

**Files:**
- Create: `src/Swarm/SwarmPersistence.h`
- Create: `src/Swarm/SwarmPersistence.cc`
- Create: `swarm_ext/tests/cpp/tst_persistence.cc`
- Modify: `src/Swarm/CMakeLists.txt` + `swarm_ext/tests/cpp/CMakeLists.txt`

- [ ] **Step 1: 写 SwarmPersistence.h**

```cpp
#pragma once
#include <QtCore/QList>
#include <QtCore/QString>

// 任务态 JSON 快照读写(纯 QtCore,可独立单测)。原子写防损坏;
// 加载时恢复语义:RUNNING → PAUSED(重启不自动续飞,MVP #6)。租约不持久化。
class SwarmPersistence
{
public:
    struct TaskRecord {
        QString id;
        QString type;
        QString assignee;
        QString approvalStatus;    ///< PENDING / APPROVED / REJECTED
        QString status;            ///< DRAFT / RUNNING / PAUSED / SUCCEEDED / ...
        int     version = 1;
    };

    /// 原子写(tmp+rename)。成功返回 true。
    static bool saveTasks(const QString &path, const QList<TaskRecord> &tasks);
    /// 读回;RUNNING→PAUSED;文件缺失/损坏 → 空列表。
    static QList<TaskRecord> loadTasks(const QString &path);
};
```

- [ ] **Step 2: 写 tst_persistence.cc(失败测试)**

```cpp
#include <QtTest/QtTest>
#include <QtCore/QTemporaryDir>
#include "SwarmPersistence.h"

class TstPersistence : public QObject { Q_OBJECT
private slots:
    void roundTrip();
    void runningRecoversAsPaused();
    void missingFileReturnsEmpty();
    void corruptFileReturnsEmpty();
};

static SwarmPersistence::TaskRecord mk(const QString &id, const QString &status) {
    SwarmPersistence::TaskRecord r; r.id=id; r.type="SURVEY"; r.assignee="1";
    r.approvalStatus="APPROVED"; r.status=status; r.version=2; return r;
}

void TstPersistence::roundTrip() {
    QTemporaryDir dir; const QString p = dir.path()+"/tasks.json";
    QVERIFY(SwarmPersistence::saveTasks(p, {mk("t1","DRAFT"), mk("t2","SUCCEEDED")}));
    const auto out = SwarmPersistence::loadTasks(p);
    QCOMPARE(out.count(), 2);
    QCOMPARE(out[0].id, QStringLiteral("t1"));
    QCOMPARE(out[0].version, 2);
    QCOMPARE(out[1].status, QStringLiteral("SUCCEEDED"));
}
void TstPersistence::runningRecoversAsPaused() {
    QTemporaryDir dir; const QString p = dir.path()+"/tasks.json";
    SwarmPersistence::saveTasks(p, {mk("t1","RUNNING")});
    const auto out = SwarmPersistence::loadTasks(p);
    QCOMPARE(out[0].status, QStringLiteral("PAUSED"));      // 安全:重启不自动续飞
}
void TstPersistence::missingFileReturnsEmpty() {
    QVERIFY(SwarmPersistence::loadTasks("/nonexistent/xx.json").isEmpty());
}
void TstPersistence::corruptFileReturnsEmpty() {
    QTemporaryDir dir; const QString p = dir.path()+"/bad.json";
    QFile f(p); f.open(QIODevice::WriteOnly); f.write("{not json"); f.close();
    QVERIFY(SwarmPersistence::loadTasks(p).isEmpty());
}

QTEST_GUILESS_MAIN(TstPersistence)
#include "tst_persistence.moc"
```

- [ ] **Step 3: 挂 CMake**

`src/Swarm/CMakeLists.txt` 加 `SwarmPersistence.cc` / `SwarmPersistence.h`。
`swarm_ext/tests/cpp/CMakeLists.txt` 加:
```cmake
add_executable(tst_persistence
    tst_persistence.cc
    ${SWARM_SRC}/SwarmPersistence.cc
    ${SWARM_SRC}/SwarmPersistence.h
)
target_include_directories(tst_persistence PRIVATE ${SWARM_SRC})
target_link_libraries(tst_persistence PRIVATE Qt6::Core Qt6::Test)
add_test(NAME persistence COMMAND tst_persistence)
```

- [ ] **Step 4: 跑测试确认失败** — 容器模板 `<TARGET>=tst_persistence`,Expected: FAIL(undefined reference)。

- [ ] **Step 5: 写 SwarmPersistence.cc**

```cpp
#include "SwarmPersistence.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

bool SwarmPersistence::saveTasks(const QString &path, const QList<TaskRecord> &tasks)
{
    QJsonArray arr;
    for (const TaskRecord &t : tasks) {
        arr.append(QJsonObject{
            {QStringLiteral("id"),       t.id},
            {QStringLiteral("type"),     t.type},
            {QStringLiteral("assignee"), t.assignee},
            {QStringLiteral("approval"), t.approvalStatus},
            {QStringLiteral("status"),   t.status},
            {QStringLiteral("version"),  t.version},
        });
    }
    QSaveFile f(path);                          // QSaveFile = tmp+rename 原子写
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    return f.commit();
}

QList<SwarmPersistence::TaskRecord> SwarmPersistence::loadTasks(const QString &path)
{
    QList<TaskRecord> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return out;                             // 缺失 → 空
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        return out;                             // 损坏 → 空
    }
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        TaskRecord t;
        t.id             = o.value(QStringLiteral("id")).toString();
        t.type           = o.value(QStringLiteral("type")).toString();
        t.assignee       = o.value(QStringLiteral("assignee")).toString();
        t.approvalStatus = o.value(QStringLiteral("approval")).toString();
        t.status         = o.value(QStringLiteral("status")).toString();
        t.version        = o.value(QStringLiteral("version")).toInt(1);
        if (t.status == QStringLiteral("RUNNING")) {
            t.status = QStringLiteral("PAUSED");    // 恢复语义
        }
        out.append(t);
    }
    return out;
}
```

- [ ] **Step 6: 跑测试确认通过** — Expected: `Totals: 4 passed, 0 failed`。

- [ ] **Step 7: Commit**
```bash
git add src/Swarm/SwarmPersistence.cc src/Swarm/SwarmPersistence.h \
        src/Swarm/CMakeLists.txt swarm_ext/tests/cpp/tst_persistence.cc swarm_ext/tests/cpp/CMakeLists.txt
git commit -m "feat(swarm): 任务持久化 SwarmPersistence(JSON 原子写+RUNNING→PAUSED 恢复)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: SwarmManager 集成租约(executeActions 逐机查 + 订阅飞行模式)

**Files:**
- Modify: `src/Swarm/SwarmManager.h`(加 `SwarmControlLease _lease;` 成员 + include)
- Modify: `src/Swarm/SwarmManager.cc`(构造订阅 flightModeChanged;executeActions 逐机查 canAutomate)

- [ ] **Step 1: SwarmManager.h 加成员**

在 include 区加 `#include "SwarmControlLease.h"`;在私有成员区(与 `_autonomyLevel` 同处)加:
```cpp
    SwarmControlLease _lease;
```

- [ ] **Step 2: 构造函数订阅飞行模式**（`SwarmManager` 构造里,遍历 vehicles 或在 `_vehicleAdded` 处）

在 `_vehicleAdded(Vehicle *vehicle)`(约 SwarmManager.cc:905)体内加订阅:
```cpp
void SwarmManager::_vehicleAdded(Vehicle *vehicle)
{
    if (vehicle) {
        connect(vehicle, &Vehicle::flightModeChanged, this, [this, vehicle](const QString &mode){
            _lease.onFlightModeChanged(vehicle->id(), mode);
        });
    }
    emit vehicleCountChanged();
}
```

- [ ] **Step 3: executeActions 逐机查租约**

在 `executeActions`(约 SwarmManager.cc:764,E1 兜底之后、dispatch 循环之前)——注意:当前 dispatch 是「对选中/全体下发」,租约检查在**取目标机集合**处过滤。最小侵入:在 dispatch 循环体开头对 per-vehicle 类动作查租约。由于多数动作走 `_targetVehicles()`,在 `_targetVehicles()` 里过滤被接管的机:

修改 `_targetVehicles()`(约 SwarmManager.cc:461),对每个候选机加 `canAutomate` 过滤:
```cpp
QList<Vehicle *> SwarmManager::_targetVehicles() const
{
    MultiVehicleManager *mvm = MultiVehicleManager::instance();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // ... 原有取选中/全体逻辑,得到候选 list ...
    // 过滤被人工接管的机(无租约的机在自动化路径已 requestLease;MANUAL_OVERRIDE 被排除)
    QList<Vehicle *> gated;
    for (Vehicle *v : list) {
        if (_lease.stateOf(v->id()) == SwarmControlLease::MANUAL_OVERRIDE) {
            emit swarmCommandFeedback(tr("%1 号机已人工接管，跳过自动下发").arg(v->id()));
            continue;
        }
        gated.append(v);
    }
    return gated;
}
```
（`_targetVehicles` 为 const,`_lease` 的 `stateOf` 也是 const,OK。）

- [ ] **Step 4: 自动化路径申请租约** — 在 executeActions 兜底通过后、dispatch 前,对目标机申请租约:
```cpp
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (Vehicle *v : _targetVehicles()) {
        if (_lease.stateOf(v->id()) != SwarmControlLease::MANUAL_OVERRIDE) {
            _lease.requestLease(v->id(), origin, QStringLiteral("swarm"), nowMs);
        }
    }
```

- [ ] **Step 5: 整机编译验证**
```bash
export http_proxy=http://127.0.0.1:8118 https_proxy=http://127.0.0.1:8118
deploy/docker/_docker-exec.sh --fuse qgc-ubuntu-docker Debug > /tmp/b.log 2>&1; grep -cE "error:|FAILED:" /tmp/b.log; tail -1 /tmp/b.log
```
Expected: `0` errors,`Build complete!`。

- [ ] **Step 6: Commit**
```bash
git add src/Swarm/SwarmManager.cc src/Swarm/SwarmManager.h
git commit -m "feat(swarm): SwarmManager 集成控制租约(飞行模式接管即跳过该机下发)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: SwarmTaskController 持久化落盘 + 启动恢复

**Files:**
- Modify: `src/Swarm/SwarmTaskController.cc`(startTask/状态迁移处落盘;构造/初始化处 loadTasks)
- Modify: `src/Swarm/SwarmTaskController.h`(加 `_persistPath()` 辅助声明,若需要)

- [ ] **Step 1: 落盘辅助**（TaskController.cc 加匿名 helper 或私有方法）

```cpp
static QString taskPersistPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
         + QStringLiteral("/SwarmConfig/tasks.json");
}
```
（TaskController.cc 顶部加 `#include <QtCore/QStandardPaths>` 与 `#include "SwarmPersistence.h"`。）

- [ ] **Step 2: 状态迁移落盘** — 在任务状态改变处(如 `_setStateText` 或 `startTask`/`_finish`)调用:
```cpp
void SwarmTaskController::_persistState()
{
    SwarmPersistence::TaskRecord r;
    r.id = _taskName;
    r.type = QStringLiteral("MULTISTAGE");
    r.status = running() ? QStringLiteral("RUNNING") : QStringLiteral("PAUSED");
    r.version = 1;
    SwarmPersistence::saveTasks(taskPersistPath(), {r});
}
```
在 `startTask` 末尾、`_enterStage` 推进后、`_finish` 处各调用 `_persistState()`。

- [ ] **Step 3: 启动恢复** — 在 SwarmManager 构造 TaskController 后(或 TaskController 初始化),调用一次 loadTasks 并把 RUNNING(已被 loadTasks 转成 PAUSED)反映为「有未完成任务待确认」的告警,**不自动 startTask**:
```cpp
    const auto restored = SwarmPersistence::loadTasks(taskPersistPath());
    for (const auto &t : restored) {
        if (t.status == QStringLiteral("PAUSED")) {
            emit taskFeedback(tr("检测到重启前未完成任务「%1」,已暂停,请确认后重新启动").arg(t.id));
        }
    }
```

- [ ] **Step 4: 整机编译验证** — 同 Task 3 Step 5。Expected: 0 errors。

- [ ] **Step 5: Commit**
```bash
git add src/Swarm/SwarmTaskController.cc src/Swarm/SwarmTaskController.h
git commit -m "feat(swarm): 任务持久化落盘+启动恢复(重启未完成任务暂停待确认)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: tst_pipeline 补「接管跳过」契约 + 全量重测

**Files:**
- Modify: `swarm_ext/tests/cpp/tst_pipeline.cc`(加 lease 契约测试)
- Modify: `swarm_ext/tests/cpp/CMakeLists.txt`(tst_pipeline 加 SwarmControlLease.cc)

- [ ] **Step 1: 加测试**（tst_pipeline.cc,include `SwarmControlLease.h`;slots 加 `leaseGatesAutomation`;QTEST 前加实现）

```cpp
void TstPipeline::leaseGatesAutomation()
{
    // 端到端契约:人工接管的机 canAutomate=false,未接管的机不受影响(逐机粒度)
    SwarmControlLease lease;
    lease.requestLease(1, "task", "op", 1000);
    lease.requestLease(2, "task", "op", 1000);
    lease.onFlightModeChanged(1, "Hold");                   // 1 号被接管
    QVERIFY2(!lease.canAutomate(1, 1000), "被接管机不可自动化");
    QVERIFY2( lease.canAutomate(2, 1000), "未接管机不受影响");
}
```

CMake tst_pipeline block 加:
```cmake
    ${SWARM_SRC}/SwarmControlLease.cc
    ${SWARM_SRC}/SwarmControlLease.h
```

- [ ] **Step 2: 容器全量重测** — 编译 `tst_lease tst_persistence tst_pipeline tst_p1modules tst_p2modules tst_p3modules tst_tactics`,全部运行。Expected: 全 `0 failed`。

- [ ] **Step 3: Commit**
```bash
git add swarm_ext/tests/cpp/tst_pipeline.cc swarm_ext/tests/cpp/CMakeLists.txt
git commit -m "test(swarm): tst_pipeline 补控制租约接管跳过契约

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: SITL 验收脚本整理 + 文档更新

**Files:**
- Modify/Create: `swarm_ext/sim/run_scenario.py`(补验收断言)
- Modify: `swarm_ext/docs/FEATURES.md`、memory `qgc-swarm-c2-project.md`

- [ ] **Step 1: run_scenario.py 补验收项**(pymavlink,不依赖 QGC GUI)——覆盖:多机识别、任务不串机(vehicle A 任务不到 B)、重启不恢复(读 tasks.json 后 RUNNING 应为 PAUSED)。若 SITL 环境不可用,脚本以 `--dry-run` 静态校验 tasks.json 恢复语义。

- [ ] **Step 2: 标注环境限制** — 在脚本头注释与 FEATURES.md 写明「SITL 多机闭环受阻于 headless 服务器,脚本就绪待显示/仿真环境执行;租约与恢复语义已由容器单测覆盖」。

- [ ] **Step 3: 更新 memory** — 追加第八轮(Lattice 安全收尾)记录:控制租约 + 任务持久化 + SITL 脚本,commit 数,健康度。

- [ ] **Step 4: Commit**
```bash
git add swarm_ext/sim/run_scenario.py swarm_ext/docs/FEATURES.md
git commit -m "docs(swarm): SITL 验收脚本补验收项+标注环境限制;更新功能存档

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review(计划 vs spec)

**Spec 覆盖**:
- §4 控制租约 → Task 1(模块)+ Task 3(集成)✅
- §5 任务持久化 + 恢复 → Task 2(模块)+ Task 4(集成)✅
- §6 SITL → Task 6 ✅
- §7 测试策略 → tst_lease(T1)/tst_persistence(T2)/tst_pipeline 契约(T5)✅
- §8 错误处理 → T2 corrupt/missing 回退、T1 过期、isOperatorMode 未知保守 ✅
- §10 验收标准 → T5 全量重测 + T3/T4 整机编译 ✅

**Placeholder 扫描**:无 TBD/TODO;每步含实际代码或确切命令。Task 3 Step 3 的 `_targetVehicles` 原有逻辑用「... 原有取选中/全体逻辑 ...」标注——执行时需读该函数现有实现后在返回前插入过滤,非 placeholder(过滤代码已给全)。

**类型一致性**:`SwarmControlLease` 的 `canAutomate(int,qint64)`/`stateOf(int)`/`MANUAL_OVERRIDE`/`requestLease(int,QString,QString,qint64,qint64)` 在 T1/T3/T5 用法一致;`SwarmPersistence::TaskRecord` 字段在 T2/T4 一致。
