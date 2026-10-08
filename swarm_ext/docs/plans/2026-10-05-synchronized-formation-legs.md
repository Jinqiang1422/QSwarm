# 同步编队航段 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 编队移动（地图 goto、编队航线、启动/变队形）时全队含长机同时受令、按航程比例设速、同时到达，途中按进度共识调速，队形整齐收敛。

**Architecture:** 新增两个纯算法单元——`FormationLegPlanner`（生成航段计划：槽位最小位移分配、成对间距闭式校验、均分拆段与垂直分层兜底、路径区域检查）与 `FormationLegTracker`（逐拍共识调速、完成/超时/中止判定）；`FormationController` 增加 Transit 阶段执行航段队列，`SwarmManager` 把地图 goto 与新的「编队按航线飞」接到航段上；PX4 固件插件新增带地速的 `DO_REPOSITION`。

**Tech Stack:** C++20 / Qt 6.11（QtPositioning、QtTest）、QML、MAVLink、Python 3（SITL 验收脚本）。

**设计文档:** `swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md`（下称「设计」）。

---

## 执行约定（先读）

- **构建与测试只能在容器里跑**（宿主无 Qt/just）。Task 0 会生成两个辅助脚本：
  - `/tmp/qgc_build.sh`：增量编译 `QGroundControl` 目标。
  - `/tmp/qgc_ctest.sh '<正则>'`：在容器里跑 `ctest -R '<正则>'`（与 CI 同一 ctest 调用方式）。
- 改动 `FirmwarePlugin.h` 会触发大量重编译（ccache 可缓解），Task 5 的编译会比其他任务慢。
- **不要逐任务 `git commit`**：工作区里有 9/30 未提交的改动，且与本计划修改同一批文件
  （`FormationController.*`、`SwarmManager.*`、`SwarmRegressionTest.*` 等），逐任务提交会把它们混进去。
  用户的习惯是设计/实现完成后统一决定提交方式；每个任务以「测试通过」作为检查点，最后在 Task 11 询问用户。
- 正在运行的 GUI 容器 `qgc-swarm5-gui` 只读挂载 `build/`，重编译不会影响它（旧进程继续用旧 inode）；
  SITL 验收前要 `docker restart qgc-swarm5-gui`（Task 11）。
- 新增/修改的 C++ 一律用宿主 `clang-format`（22.1.5，与 pre-commit 一致）格式化。
- 用户可见文字用 `tr()`（QObject 内）或 `QCoreApplication::translate()`（命名空间内）；不要用 `Q_ASSERT`；
  测试里不要用 `QTest::qWait(<n>)`、不要用 `QTest::ignoreMessage`。

## 文件结构

| 文件 | 动作 | 职责 |
|---|---|---|
| `src/Swarm/FormationLegPlanner.h/.cc` | 新建 | 航段规划纯函数（设计 3.2） |
| `src/Swarm/FormationLegTracker.h/.cc` | 新建 | 航段执行状态机（设计 3.3） |
| `src/Swarm/CMakeLists.txt` | 修改 | 注册上面 4 个文件 |
| `src/Swarm/SwarmMissionSnapshot.h/.cc` | 修改 | `formationWaypoints()`：长机航线 → 编队航点 |
| `src/FirmwarePlugin/FirmwarePlugin.h/.cc` | 修改 | 新虚函数 `guidedModeGotoLocationAtSpeed`（基类退化为不带速度） |
| `src/FirmwarePlugin/PX4/PX4FirmwarePlugin.h/.cc` | 修改 | PX4 覆写：param1 = 地速 |
| `src/Comms/MockLink/MockLink.h/.cc` | 修改 | `setRepositionSupported` 同时作用于 COMMAND_INT |
| `src/Swarm/FormationController.h/.cc` | 重写 | StationKeeping + Transit 两阶段、编队朝向、航段入口 |
| `src/Swarm/SwarmManager.h/.cc` | 修改 | goto 改走航段、航线入口、路径/间距回调、航段结束反馈 |
| `src/Swarm/SwarmAcceptanceServer.cc` | 修改 | `status.formation` 新字段、`formation_route`、`upload_route` |
| `src/FlyView/SwarmControlPanel.qml` | 修改 | 「编队按航线飞」按钮与确认框 |
| `test/Swarm/FormationLegPlannerTest.h/.cc` | 新建 | Unit：规划器、跟踪器、航线提取 |
| `test/Swarm/CMakeLists.txt` | 修改 | 注册 `FormationLegPlannerTest`（`LABELS Unit`） |
| `test/Swarm/SwarmVehicleTest.h/.cc` | 新建 | Swarm 多机集成测试夹具（从 `SwarmRegressionTest` 抽出 init/cleanup/addVehicles，加 takeOffAll 与重定位解析辅助） |
| `test/Swarm/SwarmFormationTest.h/.cc` | 新建 | Integration：编队航段集成测试（含迁入并改写的 `operatorGotoMovesWholeFormation`） |
| `test/Swarm/SwarmRegressionTest.h/.cc` | 修改 | 改为继承夹具；调整既有编队测试；迁出 `operatorGotoMovesWholeFormation` |

> 为什么拆类：2026-10-07 基线实测 `SwarmRegressionTest` 28 例全过但耗时 114 s（负载 17），
> 加进程启动后超过 Integration 标签 120 s 超时；再塞入本计划的集成测试必然超时。
| `swarm_ext/sim/qgc_acceptance.py` | 修改 | `formation_sync_metrics`、槽位/朝向感知的几何指标 |
| `swarm_ext/sim/test_qgc_acceptance.py` | 修改 | 上述指标的单测 |
| `swarm_ext/sim/formation_sync_check.py` | 新建 | SITL 验收驱动（S1–S5） |
| `swarm_ext/sim/README.md` | 修改 | 登记新脚本 |
| 设计文档 | 修改 | 第 5 节验证记录 |

---

### Task 0: 准备与基线

**Files:** 无代码改动

- [ ] **Step 1: 检查 build/ 与 .ccache/ 没有 root 属主文件**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
find build .ccache -maxdepth 3 ! -user 1000 2>/dev/null | head -3
```
Expected: 无输出。若有输出，先修复属主：
```bash
docker run --rm --user 0:0 --entrypoint /bin/chown -v $PWD/build:/project/build -v $PWD/.ccache:/project/ccache qgc-ubuntu-docker:latest -R 1000:1000 /project/build /project/ccache
```

- [ ] **Step 2: 生成编译与测试辅助脚本**

```bash
cat > /tmp/qgc_build.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
QGC=/media/hello/data1/Jinqiang/QGC/qgroundcontrol
docker run --rm -u 1000:1000 --entrypoint bash \
  -v "$QGC:/project/source" -v "$QGC/build:/project/build" \
  qgc-ubuntu-docker:latest -lc 'cmake --build /project/build --target QGroundControl --parallel 20'
EOF
cat > /tmp/qgc_ctest.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
QGC=/media/hello/data1/Jinqiang/QGC/qgroundcontrol
docker run --rm -u 1000:1000 --entrypoint bash \
  -v "$QGC:/project/source" -v "$QGC/build:/project/build" \
  -e QT_QPA_PLATFORM=offscreen -e HOME=/tmp/qgc-test-home \
  qgc-ubuntu-docker:latest -c "cd /project/build && ctest --output-on-failure $*"
EOF
chmod +x /tmp/qgc_build.sh /tmp/qgc_ctest.sh
```

- [ ] **Step 3: 基线编译并跑现有编队测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -3
/tmp/qgc_ctest.sh -R "'^SwarmRegressionTest$'" 2>&1 | tail -5
```
Expected: 编译成功；`100% tests passed`。若基线就失败，停下来报告给用户，不要在失败的基线上继续。

---

### Task 1: 规划器几何原语

**Files:**
- Create: `src/Swarm/FormationLegPlanner.h`, `src/Swarm/FormationLegPlanner.cc`
- Create: `test/Swarm/FormationLegPlannerTest.h`, `test/Swarm/FormationLegPlannerTest.cc`
- Modify: `src/Swarm/CMakeLists.txt`, `test/Swarm/CMakeLists.txt`

- [ ] **Step 1: 写头文件（只含原语，后续任务追加）**

`src/Swarm/FormationLegPlanner.h`：
```cpp
#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <functional>

/// 同步编队航段规划：纯函数，不依赖 Vehicle 与定时器。
/// 设计见 swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md 第 3.2 节。
namespace FormationLegPlanner {

/// 局部东-北-天坐标（米）
struct Vector3
{
    double east = 0;
    double north = 0;
    double up = 0;
};

/// point 相对 origin 的局部东北天偏移（水平用球面距离与方位角换算，适用于数公里范围）
Vector3 localOffset(const QGeoCoordinate& origin, const QGeoCoordinate& point);

/// 同步直线运动下两机的最小间距：相对位置 r(s) = r0 + s·delta，s ∈ [0, 1]
double minimumSeparation(const Vector3& r0, const Vector3& delta);

/// 行数 ≤ 列数的最小总代价分配（匈牙利算法）：返回每行分到的列。输入非法时返回空。
QList<int> minimumCostAssignment(const QList<QList<double>>& cost);

/// 编队朝向：from→to 的水平距离不小于 spacing 时取其方位角，否则保持 currentHeadingDeg。结果归一到 [0, 360)。
double legHeading(const QGeoCoordinate& from, const QGeoCoordinate& to, double currentHeadingDeg, double spacing);

}  // namespace FormationLegPlanner
```

- [ ] **Step 2: 写失败的测试**

`test/Swarm/FormationLegPlannerTest.h`：
```cpp
#pragma once

#include "UnitTest.h"

class FormationLegPlannerTest : public UnitTest
{
    Q_OBJECT

private slots:
    void _closedFormSeparationMatchesSampling();
    void _assignmentMatchesBruteForce();
    void _legHeadingKeepsOrientationForShortMoves();
};
```

`test/Swarm/FormationLegPlannerTest.cc`：
```cpp
#include "FormationLegPlannerTest.h"

#include <QtCore/QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

#include "FormationLegPlanner.h"

using namespace FormationLegPlanner;

namespace {

const QGeoCoordinate kOrigin(47.397, 8.5455, 500);

/// 密集采样求最小间距，用于独立复核闭式解
double sampledSeparation(const Vector3& r0, const Vector3& delta)
{
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i <= 10000; ++i) {
        const double s = i / 10000.0;
        best = std::min(best, std::hypot(r0.east + s * delta.east, r0.north + s * delta.north, r0.up + s * delta.up));
    }
    return best;
}

}  // namespace

void FormationLegPlannerTest::_closedFormSeparationMatchesSampling()
{
    std::mt19937 random(20261005);
    std::uniform_real_distribution<double> value(-50.0, 50.0);
    for (int i = 0; i < 200; ++i) {
        const Vector3 r0{value(random), value(random), value(random)};
        const Vector3 delta{value(random), value(random), value(random)};
        QVERIFY(qAbs(minimumSeparation(r0, delta) - sampledSeparation(r0, delta)) < 0.01);
    }
    // A head-on swap passes through zero; a pure translation keeps its separation.
    QVERIFY(qAbs(minimumSeparation({0, 10, 0}, {0, -20, 0})) < 1e-9);
    QVERIFY(qAbs(minimumSeparation({3, 4, 0}, {0, 0, 0}) - 5.0) < 1e-9);
}

void FormationLegPlannerTest::_assignmentMatchesBruteForce()
{
    std::mt19937 random(7);
    std::uniform_real_distribution<double> value(0.0, 100.0);
    for (int rows = 1; rows <= 5; ++rows) {
        for (int columns = rows; columns <= 6; ++columns) {
            QList<QList<double>> cost(rows, QList<double>(columns));
            for (QList<double>& row : cost) {
                for (double& entry : row)
                    entry = value(random);
            }
            const QList<int> assignment = minimumCostAssignment(cost);
            QCOMPARE(assignment.size(), rows);
            QCOMPARE(QSet<int>(assignment.cbegin(), assignment.cend()).size(), rows);
            double total = 0;
            for (int r = 0; r < rows; ++r)
                total += cost[r][assignment[r]];
            QList<int> order(columns);
            std::iota(order.begin(), order.end(), 0);
            double best = std::numeric_limits<double>::max();
            do {
                double sum = 0;
                for (int r = 0; r < rows; ++r)
                    sum += cost[r][order[r]];
                best = std::min(best, sum);
            } while (std::next_permutation(order.begin(), order.end()));
            QVERIFY(qAbs(total - best) < 1e-9);
        }
    }
    QVERIFY(minimumCostAssignment({{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}}).isEmpty());
}

void FormationLegPlannerTest::_legHeadingKeepsOrientationForShortMoves()
{
    const double north = legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(100, 0), 90, 15);
    QVERIFY(north < 0.01 || north > 359.99);
    QVERIFY(qAbs(legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(100, 225), 90, 15) - 225) < 0.01);
    QCOMPARE(legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(10, 180), 90, 15), 90.0);
    QCOMPARE(legHeading(kOrigin, kOrigin.atDistanceAndAzimuth(10, 180), -90, 15), 270.0);
}

UT_REGISTER_TEST(FormationLegPlannerTest, TestLabel::Unit)
```

- [ ] **Step 3: 注册到 CMake**

`src/Swarm/CMakeLists.txt`：在 `FormationController.h` 之后加两行：
```cmake
            FormationLegPlanner.cc
            FormationLegPlanner.h
```

`test/Swarm/CMakeLists.txt`：文件开头加：
```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE FormationLegPlannerTest.cc FormationLegPlannerTest.h)
add_qgc_test(FormationLegPlannerTest LABELS Unit)

```

再建一个只含 include 的 `src/Swarm/FormationLegPlanner.cc`（Step 5 填入实现），让下一步的失败是链接错误而不是 CMake 找不到源文件：
```cpp
#include "FormationLegPlanner.h"
```

- [ ] **Step 4: 编译，确认链接失败（函数未实现）**

Run: `/tmp/qgc_build.sh 2>&1 | grep -E "undefined reference|error" | head -5`
Expected: `undefined reference to FormationLegPlanner::minimumSeparation` 等。

- [ ] **Step 5: 实现原语**

`src/Swarm/FormationLegPlanner.cc`（整体替换 Step 3 的占位内容）：
```cpp
#include "FormationLegPlanner.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QtMath>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace FormationLegPlanner {

namespace {

double dot(const Vector3& a, const Vector3& b)
{
    return a.east * b.east + a.north * b.north + a.up * b.up;
}

double normalizedHeading(double headingDeg)
{
    const double wrapped = std::fmod(headingDeg, 360.0);
    return wrapped < 0 ? wrapped + 360.0 : wrapped;
}

}  // namespace

Vector3 localOffset(const QGeoCoordinate& origin, const QGeoCoordinate& point)
{
    const double distance = origin.distanceTo(point);
    const double azimuth = qDegreesToRadians(origin.azimuthTo(point));
    return {distance * std::sin(azimuth), distance * std::cos(azimuth), point.altitude() - origin.altitude()};
}

double minimumSeparation(const Vector3& r0, const Vector3& delta)
{
    const double deltaSquared = dot(delta, delta);
    const double s = deltaSquared > 0 ? std::clamp(-dot(r0, delta) / deltaSquared, 0.0, 1.0) : 0.0;
    const Vector3 closest{r0.east + s * delta.east, r0.north + s * delta.north, r0.up + s * delta.up};
    return std::sqrt(dot(closest, closest));
}

QList<int> minimumCostAssignment(const QList<QList<double>>& cost)
{
    const int rows = cost.size();
    if (rows == 0)
        return {};
    const int columns = cost.first().size();
    if (columns < rows)
        return {};
    for (const QList<double>& row : cost) {
        if (row.size() != columns ||
            std::any_of(row.cbegin(), row.cend(), [](double entry) { return !std::isfinite(entry); }))
            return {};
    }
    // 势函数 + 增广路的匈牙利算法，O(rows²·columns)。下标从 1 起，第 0 列为虚拟列。
    constexpr double infinity = std::numeric_limits<double>::infinity();
    QList<double> rowPotential(rows + 1, 0.0);
    QList<double> columnPotential(columns + 1, 0.0);
    QList<int> columnRow(columns + 1, 0);  // 列当前匹配的行，0 表示未匹配
    QList<int> previousColumn(columns + 1, 0);
    for (int row = 1; row <= rows; ++row) {
        columnRow[0] = row;
        int column = 0;
        QList<double> slack(columns + 1, infinity);
        QList<bool> visited(columns + 1, false);
        do {
            visited[column] = true;
            const int currentRow = columnRow[column];
            double delta = infinity;
            int nextColumn = 0;
            for (int j = 1; j <= columns; ++j) {
                if (visited[j])
                    continue;
                const double reduced = cost[currentRow - 1][j - 1] - rowPotential[currentRow] - columnPotential[j];
                if (reduced < slack[j]) {
                    slack[j] = reduced;
                    previousColumn[j] = column;
                }
                if (slack[j] < delta) {
                    delta = slack[j];
                    nextColumn = j;
                }
            }
            for (int j = 0; j <= columns; ++j) {
                if (visited[j]) {
                    rowPotential[columnRow[j]] += delta;
                    columnPotential[j] -= delta;
                } else {
                    slack[j] -= delta;
                }
            }
            column = nextColumn;
        } while (columnRow[column] != 0);
        do {
            const int previous = previousColumn[column];
            columnRow[column] = columnRow[previous];
            column = previous;
        } while (column != 0);
    }
    QList<int> assignment(rows, -1);
    for (int j = 1; j <= columns; ++j) {
        if (columnRow[j] != 0)
            assignment[columnRow[j] - 1] = j - 1;
    }
    return assignment;
}

double legHeading(const QGeoCoordinate& from, const QGeoCoordinate& to, double currentHeadingDeg, double spacing)
{
    if (!from.isValid() || !to.isValid() || from.distanceTo(to) < spacing)
        return normalizedHeading(currentHeadingDeg);
    return normalizedHeading(from.azimuthTo(to));
}

}  // namespace FormationLegPlanner
```

- [ ] **Step 6: 编译并跑测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh -R "'^FormationLegPlannerTest$'" 2>&1 | tail -4
```
Expected: `100% tests passed, 0 tests failed out of 1`。

---

### Task 2: 航段规划 `plan()`

**Files:**
- Modify: `src/Swarm/FormationLegPlanner.h`, `src/Swarm/FormationLegPlanner.cc`
- Modify: `test/Swarm/FormationLegPlannerTest.h`, `test/Swarm/FormationLegPlannerTest.cc`

- [ ] **Step 1: 在头文件 `}  // namespace FormationLegPlanner` 之前追加类型与入口**

```cpp
inline constexpr double kTrackingMargin = 1.0;         ///< 间距判据中的跟踪余量（米）
inline constexpr double kVerticalSpeedEstimate = 1.0;  ///< 估算垂直耗时用的保守升降速率（米/秒）
inline constexpr double kNegligibleDistance = 0.5;     ///< 小于此水平航程的成员不做速度同步（米）
inline constexpr int kMaxSubLegs = 4;                  ///< 均分拆段的最大段数

/// 槽位几何，与 FormationController 的槽位一致：相对参考点的水平距离、相对编队朝向的方位偏移、高度增量
struct SlotGeometry
{
    double distance = 0;
    double bearing = 0;
    double altitude = 0;
};

/// 槽位位置：参考点沿（编队朝向 + 方位偏移）推移 distance，高度 = 参考点高度 + 高度增量
QGeoCoordinate slotPosition(const QGeoCoordinate& reference, double headingDeg, const SlotGeometry& slot);

struct Member
{
    int id = 0;
    QGeoCoordinate position;  ///< 含 AMSL 高度
};

struct LegMember
{
    int id = 0;
    QGeoCoordinate start;
    QGeoCoordinate end;
    double distance = 0;  ///< 水平航程（米）
    double speed = 0;     ///< 指令地速（米/秒）；0 表示水平航程可忽略，由飞控默认速度完成
};

struct Leg
{
    QList<LegMember> members;  ///< 受令成员；原地航段不含长机
    QGeoCoordinate reference;  ///< 本段结束时的参考点（长机位置），含高度
    double headingDeg = 0;     ///< 本段结束时的编队朝向
    double durationSec = 0;    ///< 预计耗时：水平同步时长与垂直估算时长取大
};

struct Request
{
    QList<Member> members;              ///< 参与规划的成员当前位置（含长机）
    int leaderId = 0;
    QList<SlotGeometry> slotGeometries; ///< 全部槽位，下标即槽位编号（不叫 slots：与 Qt 的 slots 宏冲突）
    QSet<int> reservedSlots;            ///< 被未参与成员占用、不可分配的槽位
    QHash<int, int> currentAssignment;  ///< 现有槽位持有者（含未参与成员）id → 槽位下标
    QGeoCoordinate startReference;      ///< 当前参考点（长机位置），含高度
    double startHeadingDeg = 0;         ///< 当前编队朝向
    QList<QGeoCoordinate> waypoints;    ///< 依次到达的参考点（长机终点），含 AMSL 高度
    double spacing = 10;                ///< 编队间距：位移小于该值时不改朝向
    double speed = 5;                   ///< 编队速度 V（米/秒）
    double minSeparation = 3;           ///< 约束最小间距 D_min（米）
    bool commandLeader = true;          ///< false：原地航段，长机不受令，作为静止点参与间距校验
    /// 路径区域检查（电子围栏、高度上限）；为空表示不限制
    std::function<bool(int, const QGeoCoordinate&, const QGeoCoordinate&)> pathAllowed;
};

struct Plan
{
    QList<Leg> legs;
    QHash<int, int> assignment;  ///< 参与规划的僚机 id → 槽位下标
    QString error;               ///< 非空表示规划失败，内容可直接展示给操作员
    bool ok() const { return error.isEmpty() && !legs.isEmpty(); }
    double durationSec() const;
};

/// 生成航段计划：每个航点一段；成对间距不满足时依次尝试均分为 2～kMaxSubLegs 小段与垂直分层三段（设计 3.2）。
Plan plan(const Request& request);
```

- [ ] **Step 2: 写失败的测试**

`FormationLegPlannerTest.h` 的 `private slots:` 追加：
```cpp
    void _pureTranslationKeepsShape();
    void _routePlansOneLegPerWaypoint();
    void _largeTurnSplitsIntoSubLegs();
    void _layersWhenNoSplitSeparates();
    void _rejectsEndSlotsCloserThanMinimum();
    void _closePairMayOnlyDiverge();
    void _keepsCurrentAssignmentWhenNearlyOptimal();
    void _crossingCurrentAssignmentFallsBackToOptimal();
    void _reservedSlotsStayWithTheirHolders();
    void _pathOutsideAreaRejected();
```

`FormationLegPlannerTest.cc`：在匿名命名空间 `sampledSeparation` 之后追加辅助函数：
```cpp
/// 原点以东 east 米、以北 north 米、高 up 米处
QGeoCoordinate at(double east, double north, double up = 0)
{
    QGeoCoordinate point =
        kOrigin.atDistanceAndAzimuth(std::hypot(east, north), qRadiansToDegrees(std::atan2(east, north)));
    point.setAltitude(kOrigin.altitude() + up);
    return point;
}

Vector3 sub(const Vector3& a, const Vector3& b)
{
    return {a.east - b.east, a.north - b.north, a.up - b.up};
}

double length(const Vector3& v)
{
    return std::hypot(v.east, v.north, v.up);
}

/// 与 FormationController::_formationOffset 楔形一致的槽位
QList<SlotGeometry> wedgeSlots(int count, double spacing, double verticalStep)
{
    QList<SlotGeometry> slots;
    for (int index = 1; index <= count; ++index) {
        const int rank = (index + 1) / 2;
        const int side = index % 2 == 1 ? -1 : 1;
        slots.append({spacing * rank, 180.0 + side * 30.0, verticalStep * rank});
    }
    return slots;
}

/// 按设计 3.2 第 5 条独立复核每段的成对间距（密集采样，不依赖被测闭式解）。stationary 为不受令的静止点。
void verifyLegSeparation(const Plan& result, double minSeparation, const QList<QGeoCoordinate>& stationary = {})
{
    for (const Leg& leg : result.legs) {
        QList<std::pair<QGeoCoordinate, QGeoCoordinate>> paths;
        for (const LegMember& member : leg.members)
            paths.append({member.start, member.end});
        for (const QGeoCoordinate& point : stationary)
            paths.append({point, point});
        for (int i = 0; i < paths.size(); ++i) {
            for (int j = i + 1; j < paths.size(); ++j) {
                const Vector3 initial =
                    sub(localOffset(kOrigin, paths[i].first), localOffset(kOrigin, paths[j].first));
                const Vector3 finish =
                    sub(localOffset(kOrigin, paths[i].second), localOffset(kOrigin, paths[j].second));
                const double closest = sampledSeparation(initial, sub(finish, initial));
                const double required = std::min({minSeparation + kTrackingMargin, length(initial), length(finish)});
                QVERIFY2(closest >= required - 0.1, qPrintable(QStringLiteral("paths %1/%2 come within %3 m, need %4 m")
                                                                   .arg(i)
                                                                   .arg(j)
                                                                   .arg(closest)
                                                                   .arg(required)));
            }
        }
    }
}

const LegMember* member(const Leg& leg, int id)
{
    const auto it = std::find_if(leg.members.cbegin(), leg.members.cend(),
                                 [id](const LegMember& candidate) { return candidate.id == id; });
    return it == leg.members.cend() ? nullptr : &*it;
}

/// 长机 1 位于原点、只有一架僚机 2 的纵队（无高度分层）
Request columnPair(const QGeoCoordinate& follower, double startHeading, const QGeoCoordinate& waypoint)
{
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, follower}};
    request.slotGeometries = {{10, 180, 0}};
    request.currentAssignment = {{2, 0}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = startHeading;
    request.waypoints = {waypoint};
    request.spacing = 10;
    return request;
}
```

在文件末尾 `UT_REGISTER_TEST` 之前追加测试：
```cpp
void FormationLegPlannerTest::_pureTranslationKeepsShape()
{
    const QList<SlotGeometry> wedge = wedgeSlots(4, 15, 3);
    Request request;
    request.leaderId = 1;
    request.members.append({1, at(0, 0)});
    for (int k = 0; k < 4; ++k)
        request.members.append({k + 2, slotPosition(at(0, 0), 0, wedge[k])});
    request.slotGeometries = wedge;
    request.currentAssignment = {{2, 0}, {3, 1}, {4, 2}, {5, 3}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = 0;
    request.waypoints = {at(0, 150)};
    request.spacing = 15;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 1);
    QVERIFY(result.assignment == request.currentAssignment);
    const Leg& leg = result.legs.first();
    QVERIFY(qAbs(leg.durationSec - 30.0) < 0.01);
    QCOMPARE(leg.members.size(), 5);
    for (const LegMember& entry : leg.members) {
        QVERIFY(qAbs(entry.speed - 5.0) < 0.01);
        const QGeoCoordinate expected = entry.id == 1 ? at(0, 150) : slotPosition(at(0, 150), 0, wedge[entry.id - 2]);
        QVERIFY(entry.end.distanceTo(expected) < 0.01);
        QVERIFY(qAbs(entry.end.altitude() - expected.altitude()) < 0.01);
    }
    verifyLegSeparation(result, request.minSeparation);
}

void FormationLegPlannerTest::_routePlansOneLegPerWaypoint()
{
    const QList<SlotGeometry> wedge = wedgeSlots(2, 10, 3);
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, slotPosition(at(0, 0), 90, wedge[0])}, {3, slotPosition(at(0, 0), 90, wedge[1])}};
    request.slotGeometries = wedge;
    request.currentAssignment = {{2, 0}, {3, 1}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = 90;
    request.waypoints = {at(100, 0), at(100, 100, 5)};
    request.spacing = 10;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 2);
    QVERIFY(qAbs(result.legs[0].headingDeg - 90) < 0.01);
    const double turned = result.legs[1].headingDeg;
    QVERIFY(turned < 0.01 || turned > 359.99);
    const Leg& last = result.legs[1];
    QVERIFY(member(last, 1)->end.distanceTo(at(100, 100)) < 0.01);
    QVERIFY(qAbs(member(last, 1)->end.altitude() - 505) < 0.01);
    for (int id : {2, 3}) {
        const QGeoCoordinate expected = slotPosition(at(100, 100, 5), turned, wedge[id - 2]);
        QVERIFY(member(last, id)->end.distanceTo(expected) < 0.01);
        QVERIFY(qAbs(member(last, id)->end.altitude() - 508) < 0.01);
    }
    QVERIFY(qAbs(result.durationSec() - 40.0) < 0.1);
    verifyLegSeparation(result, request.minSeparation);
}

void FormationLegPlannerTest::_largeTurnSplitsIntoSubLegs()
{
    // Reversing over 12 m would swap leader and follower head-on; two 90° halves keep them apart.
    // (12 m rather than exactly one spacing, so the course-change threshold is not a floating-point tie.)
    const Plan result = plan(columnPair(at(0, -10), 0, at(0, -12)));
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 2);
    QVERIFY(member(result.legs[0], 1)->end.distanceTo(at(0, -6)) < 0.01);
    QVERIFY(member(result.legs[0], 2)->end.distanceTo(at(-10, -6)) < 0.01);
    QVERIFY(member(result.legs[1], 1)->end.distanceTo(at(0, -12)) < 0.01);
    QVERIFY(member(result.legs[1], 2)->end.distanceTo(at(0, -2)) < 0.01);
    verifyLegSeparation(result, 3);
}

void FormationLegPlannerTest::_layersWhenNoSplitSeparates()
{
    // With a 9.5 m minimum even quarter turns bring the pair too close; the follower crosses on its own layer.
    Request request = columnPair(at(0, -10), 0, at(0, -12));
    request.minSeparation = 9.5;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QCOMPARE(result.legs.size(), 3);
    const LegMember* lift = member(result.legs[0], 2);
    QVERIFY(lift->distance < 0.01);
    QVERIFY(qAbs(lift->end.altitude() - 510.5) < 0.01);
    const LegMember* cross = member(result.legs[1], 2);
    QVERIFY(cross->end.distanceTo(at(0, -2)) < 0.01);
    QVERIFY(qAbs(cross->end.altitude() - 510.5) < 0.01);
    QVERIFY(qAbs(member(result.legs[1], 1)->end.altitude() - 500) < 0.01);
    QVERIFY(qAbs(member(result.legs[2], 2)->end.altitude() - 500) < 0.01);
    verifyLegSeparation(result, 9.5);
}

void FormationLegPlannerTest::_rejectsEndSlotsCloserThanMinimum()
{
    Request request = columnPair(at(0, -10), 0, at(0, -12));
    request.minSeparation = 12;
    const Plan result = plan(request);
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("终点间距")), qPrintable(result.error));
}

void FormationLegPlannerTest::_closePairMayOnlyDiverge()
{
    // SITL spawns aircraft 2.5 m apart. Moving away is fine; crossing the hovering leader goes over it.
    Request diverging = columnPair(at(-2.5, 0), 90, at(0, 0));
    diverging.commandLeader = false;
    const Plan away = plan(diverging);
    QVERIFY2(away.ok(), qPrintable(away.error));
    QCOMPARE(away.legs.size(), 1);
    QCOMPARE(away.legs.first().members.size(), 1);
    verifyLegSeparation(away, 3, {at(0, 0)});

    Request crossing = columnPair(at(2.5, 0), 90, at(0, 0));
    crossing.commandLeader = false;
    const Plan over = plan(crossing);
    QVERIFY2(over.ok(), qPrintable(over.error));
    QCOMPARE(over.legs.size(), 3);
    for (const Leg& leg : over.legs)
        QVERIFY(!member(leg, 1));
    QVERIFY(qAbs(member(over.legs[1], 2)->end.altitude() - 504) < 0.01);
    QVERIFY(member(over.legs[2], 2)->end.distanceTo(at(-10, 0)) < 0.01);
    verifyLegSeparation(over, 3, {at(0, 0)});
}

void FormationLegPlannerTest::_keepsCurrentAssignmentWhenNearlyOptimal()
{
    // Followers far to the south and north reach either slot of the line for the same cost.
    for (const QHash<int, int>& current : {QHash<int, int>{{2, 1}, {3, 0}}, QHash<int, int>{{2, 0}, {3, 1}}}) {
        Request request;
        request.leaderId = 1;
        request.members = {{1, at(0, 0)}, {2, at(0, -1000)}, {3, at(0, 1000)}};
        request.slotGeometries = {{10, 270, 0}, {10, 90, 0}};
        request.currentAssignment = current;
        request.startReference = at(0, 0);
        request.startHeadingDeg = 0;
        request.waypoints = {at(0, 0)};
        request.commandLeader = false;
        const Plan result = plan(request);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QVERIFY(result.assignment == current);
    }
}

void FormationLegPlannerTest::_crossingCurrentAssignmentFallsBackToOptimal()
{
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, at(-10, -1000)}, {3, at(10, -1000)}};
    request.slotGeometries = {{10, 270, 0}, {10, 90, 0}};
    request.currentAssignment = {{2, 1}, {3, 0}};  // nearly optimal in cost, but the paths cross
    request.startReference = at(0, 0);
    request.startHeadingDeg = 0;
    request.waypoints = {at(0, 0)};
    request.commandLeader = false;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QVERIFY((result.assignment == QHash<int, int>{{2, 0}, {3, 1}}));
    QCOMPARE(result.legs.size(), 1);
}

void FormationLegPlannerTest::_reservedSlotsStayWithTheirHolders()
{
    const QList<SlotGeometry> wedge = wedgeSlots(2, 10, 3);
    Request request;
    request.leaderId = 1;
    request.members = {{1, at(0, 0)}, {2, slotPosition(at(0, 0), 0, wedge[0])}};
    request.slotGeometries = wedge;
    request.reservedSlots = {0};  // held by a member that is still on the ground
    request.currentAssignment = {{2, 1}, {4, 0}};
    request.startReference = at(0, 0);
    request.startHeadingDeg = 0;
    request.waypoints = {at(0, 0)};
    request.commandLeader = false;
    const Plan result = plan(request);
    QVERIFY2(result.ok(), qPrintable(result.error));
    QVERIFY((result.assignment == QHash<int, int>{{2, 1}}));
}

void FormationLegPlannerTest::_pathOutsideAreaRejected()
{
    Request request = columnPair(at(0, -10), 0, at(0, 100));
    request.pathAllowed = [](int id, const QGeoCoordinate&, const QGeoCoordinate&) { return id != 2; };
    const Plan result = plan(request);
    QVERIFY(!result.ok());
    QVERIFY2(result.error.contains(QStringLiteral("2 号机")), qPrintable(result.error));
}
```

- [ ] **Step 3: 编译，确认链接失败**

Run: `/tmp/qgc_build.sh 2>&1 | grep -E "undefined reference" | head -3`
Expected: `undefined reference to FormationLegPlanner::plan` / `slotPosition`。

- [ ] **Step 4: 实现规划**

在 `FormationLegPlanner.cc` 的 `legHeading()` 定义之后、`}  // namespace FormationLegPlanner` 之前追加：
```cpp
namespace {

constexpr double kSeparationTolerance = 0.05;  ///< 间距判据的数值容差（米）
constexpr double kReservedSlotCost = 1e12;     ///< 被占用槽位的分配代价：足够大，又不让算法溢出
constexpr double kPreferCurrentRatio = 1.05;   ///< 当前分配代价不超过最优的该倍数时保持当前分配

struct Pose
{
    QGeoCoordinate reference;
    double headingDeg = 0;
};

struct Path
{
    int id = 0;
    QGeoCoordinate start;
    QGeoCoordinate end;
};

Vector3 operator-(const Vector3& a, const Vector3& b)
{
    return {a.east - b.east, a.north - b.north, a.up - b.up};
}

double norm(const Vector3& v)
{
    return std::sqrt(dot(v, v));
}

double turnDegrees(double fromDeg, double toDeg)
{
    double turn = std::fmod(toDeg - fromDeg, 360.0);
    if (turn > 180.0)
        turn -= 360.0;
    else if (turn < -180.0)
        turn += 360.0;
    return turn;
}

Pose interpolatePose(const Pose& from, const Pose& to, double fraction)
{
    Pose pose;
    pose.reference = from.reference.atDistanceAndAzimuth(from.reference.distanceTo(to.reference) * fraction,
                                                         from.reference.azimuthTo(to.reference));
    pose.reference.setAltitude(from.reference.altitude() +
                               (to.reference.altitude() - from.reference.altitude()) * fraction);
    pose.headingDeg = normalizedHeading(from.headingDeg + turnDegrees(from.headingDeg, to.headingDeg) * fraction);
    return pose;
}

/// 设计 3.2 第 5 条：同步直线运动下任意两机不得比 min(D_min + 余量, 起点间距, 终点间距) 更近，
/// 且终点间距不得低于 D_min。不满足时写入 error。
bool separationAllowed(const QList<Path>& paths, double minSeparation, QString& error)
{
    if (paths.size() < 2)
        return true;
    const QGeoCoordinate origin = paths.first().start;
    QList<Vector3> starts;
    QList<Vector3> ends;
    for (const Path& path : paths) {
        starts.append(localOffset(origin, path.start));
        ends.append(localOffset(origin, path.end));
    }
    for (int i = 0; i < paths.size(); ++i) {
        for (int j = i + 1; j < paths.size(); ++j) {
            const Vector3 initial = starts[i] - starts[j];
            const Vector3 finish = ends[i] - ends[j];
            if (norm(finish) < minSeparation - kSeparationTolerance) {
                error = QCoreApplication::translate("FormationLegPlanner", "%1 与 %2 号机终点间距 %3 米，低于安全下限 %4 米")
                            .arg(paths[i].id)
                            .arg(paths[j].id)
                            .arg(norm(finish), 0, 'f', 1)
                            .arg(minSeparation, 0, 'f', 1);
                return false;
            }
            const double closest = minimumSeparation(initial, finish - initial);
            const double required = std::min({minSeparation + kTrackingMargin, norm(initial), norm(finish)});
            if (closest < required - kSeparationTolerance) {
                error = QCoreApplication::translate("FormationLegPlanner", "%1 与 %2 号机路径最小间距 %3 米，低于 %4 米")
                            .arg(paths[i].id)
                            .arg(paths[j].id)
                            .arg(closest, 0, 'f', 1)
                            .arg(required, 0, 'f', 1);
                return false;
            }
        }
    }
    return true;
}

/// 位姿 pose 下全体成员（含长机）的终点
QHash<int, QGeoCoordinate> slotEnds(const Request& request, const QHash<int, int>& assignment, const Pose& pose)
{
    QHash<int, QGeoCoordinate> ends;
    ends.insert(request.leaderId, pose.reference);
    for (auto it = assignment.cbegin(); it != assignment.cend(); ++it)
        ends.insert(it.key(), slotPosition(pose.reference, pose.headingDeg, request.slotGeometries.at(it.value())));
    return ends;
}

Leg makeLeg(const Request& request, const QHash<int, QGeoCoordinate>& starts, const QHash<int, QGeoCoordinate>& ends,
            const Pose& pose)
{
    Leg leg;
    leg.reference = pose.reference;
    leg.headingDeg = pose.headingDeg;
    QList<int> ids = ends.keys();
    std::sort(ids.begin(), ids.end());
    double longest = 0;
    double vertical = 0;
    for (int id : std::as_const(ids)) {
        if (id == request.leaderId && !request.commandLeader)
            continue;
        LegMember entry;
        entry.id = id;
        entry.start = starts.value(id);
        entry.end = ends.value(id);
        entry.distance = entry.start.distanceTo(entry.end);
        longest = std::max(longest, entry.distance);
        vertical = std::max(vertical, std::abs(entry.end.altitude() - entry.start.altitude()));
        leg.members.append(entry);
    }
    const double horizontalSec = longest / request.speed;
    for (LegMember& entry : leg.members) {
        entry.speed =
            entry.distance >= kNegligibleDistance && horizontalSec > 0 ? entry.distance / horizontalSec : 0.0;
    }
    leg.durationSec = std::max(horizontalSec, vertical / kVerticalSpeedEstimate);
    return leg;
}

/// 参与间距校验的路径：受令成员 + 原地航段中静止的长机
QList<Path> legPaths(const Request& request, const Leg& leg, const QHash<int, QGeoCoordinate>& starts)
{
    QList<Path> paths;
    for (const LegMember& entry : leg.members)
        paths.append({entry.id, entry.start, entry.end});
    if (!request.commandLeader) {
        const QGeoCoordinate leader = starts.value(request.leaderId);
        paths.append({request.leaderId, leader, leader});
    }
    return paths;
}

/// 分层高度：长机留在当前高度，僚机按槽位编号逐层升高，层距等于判据要求的间距
QHash<int, double> layerAltitudes(const Request& request, const QHash<int, int>& assignment,
                                  const QHash<int, QGeoCoordinate>& positions)
{
    const double base = positions.value(request.leaderId).altitude();
    const double step = request.minSeparation + kTrackingMargin;
    QList<int> followers = assignment.keys();
    std::sort(followers.begin(), followers.end(),
              [&assignment](int a, int b) { return assignment.value(a) < assignment.value(b); });
    QHash<int, double> layers;
    if (request.commandLeader)
        layers.insert(request.leaderId, base);
    for (int i = 0; i < followers.size(); ++i)
        layers.insert(followers[i], base + step * (i + 1));
    return layers;
}

/// from → to 的一段：依次尝试 1～kMaxSubLegs 均分，允许时再尝试垂直分层三段。
/// 成功时把航段追加到 legs，并把 positions 更新为最后一段的终点。
bool planSegment(const Request& request, const QHash<int, int>& assignment, const Pose& from, const Pose& to,
                 bool allowLayering, QHash<int, QGeoCoordinate>& positions, QList<Leg>& legs, QString& error)
{
    for (int parts = 1; parts <= kMaxSubLegs; ++parts) {
        QList<Leg> candidate;
        QHash<int, QGeoCoordinate> current = positions;
        bool separated = true;
        for (int part = 1; part <= parts && separated; ++part) {
            const Pose pose = interpolatePose(from, to, double(part) / parts);
            const Leg leg = makeLeg(request, current, slotEnds(request, assignment, pose), pose);
            separated = separationAllowed(legPaths(request, leg, current), request.minSeparation, error);
            for (const LegMember& entry : leg.members)
                current.insert(entry.id, entry.end);
            candidate.append(leg);
        }
        if (separated) {
            legs.append(candidate);
            positions = current;
            return true;
        }
    }
    if (!allowLayering)
        return false;
    // 垂直分层兜底：先只做垂直移动到各自高度层，再在层内水平移到终点正上方，最后回到槽位高度。
    const QHash<int, QGeoCoordinate> finalEnds = slotEnds(request, assignment, to);
    const QHash<int, double> layers = layerAltitudes(request, assignment, positions);
    QHash<int, QGeoCoordinate> lifted;
    QHash<int, QGeoCoordinate> crossed;
    for (auto it = layers.cbegin(); it != layers.cend(); ++it) {
        QGeoCoordinate up = positions.value(it.key());
        up.setAltitude(it.value());
        lifted.insert(it.key(), up);
        QGeoCoordinate across = finalEnds.value(it.key());
        across.setAltitude(it.value());
        crossed.insert(it.key(), across);
    }
    QList<Leg> layered;
    QHash<int, QGeoCoordinate> current = positions;
    const QList<std::pair<QHash<int, QGeoCoordinate>, Pose>> stages{{lifted, from}, {crossed, to}, {finalEnds, to}};
    for (const auto& [ends, pose] : stages) {
        const Leg leg = makeLeg(request, current, ends, pose);
        if (!separationAllowed(legPaths(request, leg, current), request.minSeparation, error))
            return false;
        for (const LegMember& entry : leg.members)
            current.insert(entry.id, entry.end);
        layered.append(leg);
    }
    legs.append(layered);
    positions = current;
    return true;
}

/// 僚机槽位分配：起点到目标位姿槽位的距离平方和最小；当前分配代价接近最优时优先保持当前分配
QHash<int, int> assignSlots(const Request& request, const QList<int>& followers,
                            const QHash<int, QGeoCoordinate>& positions, const Pose& target, bool preferCurrent)
{
    const int columns = request.slotGeometries.size();
    QList<QList<double>> cost(followers.size(), QList<double>(columns, kReservedSlotCost));
    for (int row = 0; row < followers.size(); ++row) {
        for (int column = 0; column < columns; ++column) {
            if (request.reservedSlots.contains(column))
                continue;
            const Vector3 offset = localOffset(positions.value(followers[row]),
                                               slotPosition(target.reference, target.headingDeg, request.slotGeometries[column]));
            cost[row][column] = dot(offset, offset);
        }
    }
    const QList<int> optimal = minimumCostAssignment(cost);
    QHash<int, int> best;
    QHash<int, int> current;
    double bestCost = 0;
    double currentCost = 0;
    bool currentUsable = preferCurrent;
    QSet<int> used;
    for (int row = 0; row < followers.size(); ++row) {
        best.insert(followers[row], optimal.value(row));
        bestCost += cost[row][optimal.value(row)];
        const int slot = request.currentAssignment.value(followers[row], -1);
        if (slot < 0 || slot >= columns || request.reservedSlots.contains(slot) || used.contains(slot)) {
            currentUsable = false;
            continue;
        }
        used.insert(slot);
        current.insert(followers[row], slot);
        currentCost += cost[row][slot];
    }
    return currentUsable && currentCost <= bestCost * kPreferCurrentRatio + 1e-6 ? current : best;
}

}  // namespace

QGeoCoordinate slotPosition(const QGeoCoordinate& reference, double headingDeg, const SlotGeometry& slot)
{
    QGeoCoordinate position = reference.atDistanceAndAzimuth(slot.distance, headingDeg + slot.bearing);
    position.setAltitude(reference.altitude() + slot.altitude);
    return position;
}

double Plan::durationSec() const
{
    double total = 0;
    for (const Leg& leg : legs)
        total += leg.durationSec;
    return total;
}

Plan plan(const Request& request)
{
    Plan result;
    const auto fail = [&result](const QString& error) {
        result.legs.clear();
        result.assignment.clear();
        result.error = error;
        return result;
    };
    QHash<int, QGeoCoordinate> positions;
    for (const Member& entry : request.members) {
        if (!entry.position.isValid() || !std::isfinite(entry.position.altitude()))
            return fail(QCoreApplication::translate("FormationLegPlanner", "%1 号机位置或高度无效").arg(entry.id));
        positions.insert(entry.id, entry.position);
    }
    if (!positions.contains(request.leaderId))
        return fail(QCoreApplication::translate("FormationLegPlanner", "长机 %1 不在编队成员中").arg(request.leaderId));
    if (request.waypoints.isEmpty() || !(request.speed > 0) || !(request.minSeparation > 0) ||
        !request.startReference.isValid() || !std::isfinite(request.startReference.altitude()))
        return fail(QCoreApplication::translate("FormationLegPlanner", "航段参数无效"));
    for (const QGeoCoordinate& waypoint : request.waypoints) {
        if (!waypoint.isValid() || !std::isfinite(waypoint.altitude()))
            return fail(QCoreApplication::translate("FormationLegPlanner", "航点坐标或高度无效"));
    }
    QList<int> followers = positions.keys();
    followers.removeAll(request.leaderId);
    std::sort(followers.begin(), followers.end());
    if (followers.size() + request.reservedSlots.size() > request.slotGeometries.size())
        return fail(QCoreApplication::translate("FormationLegPlanner", "编队槽位数量不足"));

    const Pose start{request.startReference, normalizedHeading(request.startHeadingDeg)};
    QList<Pose> poses;
    Pose previous = start;
    for (const QGeoCoordinate& waypoint : request.waypoints) {
        const Pose pose{waypoint, legHeading(previous.reference, waypoint, previous.headingDeg, request.spacing)};
        poses.append(pose);
        previous = pose;
    }

    // 尝试顺序（设计 3.2 第 6 条）：当前分配不分层 → 最优分配不分层 → 最优分配 + 垂直分层
    struct Attempt
    {
        bool preferCurrent;
        bool allowLayering;
    };
    QString error;
    for (const Attempt attempt : {Attempt{true, false}, Attempt{false, false}, Attempt{false, true}}) {
        const QHash<int, int> assignment =
            assignSlots(request, followers, positions, poses.first(), attempt.preferCurrent);
        QList<Leg> legs;
        QHash<int, QGeoCoordinate> current = positions;
        Pose from = start;
        bool planned = true;
        for (const Pose& pose : std::as_const(poses)) {
            if (!planSegment(request, assignment, from, pose, attempt.allowLayering, current, legs, error)) {
                planned = false;
                break;
            }
            from = pose;
        }
        if (!planned)
            continue;
        for (const Leg& leg : std::as_const(legs)) {
            for (const LegMember& entry : leg.members) {
                if (request.pathAllowed && !request.pathAllowed(entry.id, entry.start, entry.end)) {
                    return fail(QCoreApplication::translate("FormationLegPlanner", "%1 号机航段越出电子围栏或高度限制")
                                    .arg(entry.id));
                }
            }
        }
        result.legs = legs;
        result.assignment = assignment;
        return result;
    }
    return fail(error);
}
```

- [ ] **Step 5: 编译并跑测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh -R "'^FormationLegPlannerTest$'" 2>&1 | tail -4
```
Expected: 全部通过。若 `_routePlansOneLegPerWaypoint` 或 `_largeTurnSplitsIntoSubLegs` 的段数不符，打印 `result.legs.size()` 与各段成员终点排查（不要放宽断言——几何已在设计/本计划里手算过）。

---

### Task 3: 航段跟踪器

**Files:**
- Create: `src/Swarm/FormationLegTracker.h`, `src/Swarm/FormationLegTracker.cc`
- Modify: `src/Swarm/CMakeLists.txt`
- Modify: `test/Swarm/FormationLegPlannerTest.h/.cc`

- [ ] **Step 1: 写头文件**

`src/Swarm/FormationLegTracker.h`：
```cpp
#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>

#include "FormationLegPlanner.h"

/// 执行一个同步编队航段：每拍按各机进度做共识调速，并判定完成、超时与中止（设计 3.3）。
/// 纯状态机，不依赖 Vehicle 与定时器。
class FormationLegTracker
{
public:
    enum class State
    {
        Idle,
        Running,
        Completed,
        TimedOut,
        Aborted
    };

    struct Command
    {
        int id = 0;
        QGeoCoordinate target;
        double speed = 0;  ///< 0 表示由飞控默认速度完成
    };

    void start(const FormationLegPlanner::Leg& leg, qint64 nowMs);
    void reset();

    /// 每拍调用。positions：成员实测位置（含 AMSL 高度）；pending：上一条指令仍待 ACK 的成员；
    /// maxSpeed：成员最大水平速度中的最小值，未知时传 NaN。返回本拍需要（重新）下发的指令。
    QList<Command> update(const QHash<int, QGeoCoordinate>& positions, const QSet<int>& pending, qint64 nowMs,
                          double maxSpeed);

    /// 指令确已下发后调用，记录下发速度
    void markSent(int id, double speed);
    void removeMember(int id);
    void abort(const QString& reason);

    State state() const { return _state; }
    QString reason() const { return _reason; }
    const FormationLegPlanner::Leg& leg() const { return _leg; }
    double elapsedSec(qint64 nowMs) const;
    double consensusProgress() const { return _consensus; }
    double deviationMeters() const { return _deviation; }        ///< 本拍偏离同步理想位置的最大值
    double maxDeviationMeters() const { return _maxDeviation; }  ///< 本段迄今的最大偏离
    QHash<int, double> progress() const { return _progress; }
    QHash<int, double> distanceToEnd() const { return _distanceToEnd; }

    static constexpr double kArrivalHorizontalMeters = 1.0;
    static constexpr double kArrivalVerticalMeters = 1.0;
    static constexpr double kSpeedGain = 0.5;        ///< 每米掉队增加的速度（1/秒）
    static constexpr double kResendThreshold = 0.3;  ///< 速度变化达到此值才重发（米/秒）
    static constexpr double kMinSpeedFactor = 0.5;   ///< 超前者最低降至基准速度的比例
    static constexpr double kMaxSpeedFactor = 0.9;   ///< 速度上限占最大水平速度的比例
    static constexpr double kTimeoutSlackSec = 20.0;

private:
    bool _arrived(const FormationLegPlanner::LegMember& member, const QGeoCoordinate& position) const;
    double _speedCap(double baseSpeed, double maxSpeed) const;

    FormationLegPlanner::Leg _leg;
    qint64 _startMs = 0;
    State _state = State::Idle;
    QString _reason;
    QHash<int, double> _sentSpeed;
    QHash<int, double> _progress;
    QHash<int, double> _distanceToEnd;
    double _consensus = 0;
    double _deviation = 0;
    double _maxDeviation = 0;
};
```

- [ ] **Step 2: 写失败的测试**

`FormationLegPlannerTest.h` 的 `private slots:` 追加：
```cpp
    void _trackerSynchronizesSpeeds();
    void _trackerCompletesAndTimesOut();
    void _trackerContinuesWithoutRemovedMember();
```

`FormationLegPlannerTest.cc`：加 `#include "FormationLegTracker.h"`；在匿名命名空间末尾加：
```cpp
/// 两机并排向东飞 100 m（相距 10 m），基准速度 5 m/s，时长 20 s
Leg parallelLeg()
{
    Leg leg;
    leg.durationSec = 20;
    leg.members = {{1, at(0, 0), at(100, 0), 100, 5}, {2, at(0, 10), at(100, 10), 100, 5}};
    return leg;
}
```
测试：
```cpp
void FormationLegPlannerTest::_trackerSynchronizesSpeeds()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    auto commands = tracker.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 0, 12);
    QCOMPARE(commands.size(), 2);
    for (const auto& command : commands) {
        QCOMPARE(command.speed, 5.0);
        tracker.markSent(command.id, command.speed);
    }

    // 1 is 10 m ahead of the consensus (s = 0.5 vs 0.4), 2 is 10 m behind.
    commands = tracker.update({{1, at(50, 0)}, {2, at(30, 10)}}, {}, 5000, 12);
    QCOMPARE(commands.size(), 2);
    for (const auto& command : commands) {
        // Progress comes back through a spherical round trip; compare with a tolerance.
        QVERIFY(qAbs(command.speed - (command.id == 1 ? 2.5 : 10.0)) < 0.01);
        tracker.markSent(command.id, command.speed);
    }
    QVERIFY(qAbs(tracker.consensusProgress() - 0.4) < 0.001);
    QVERIFY(qAbs(tracker.deviationMeters() - 10.0) < 0.05);

    // Unchanged speeds are not resent, and a member awaiting an ACK is skipped.
    QVERIFY(tracker.update({{1, at(50, 0)}, {2, at(30, 10)}}, {}, 6000, 12).isEmpty());
    QVERIFY(tracker.update({{1, at(50, 0)}, {2, at(35, 10)}}, {2}, 7000, 12).isEmpty());
    QCOMPARE(tracker.state(), FormationLegTracker::State::Running);
}

void FormationLegPlannerTest::_trackerCompletesAndTimesOut()
{
    FormationLegTracker arriving;
    arriving.start(parallelLeg(), 0);
    QVERIFY(arriving.update({{1, at(99.5, 0)}, {2, at(100, 10.5)}}, {}, 21000, 12).isEmpty());
    QCOMPARE(arriving.state(), FormationLegTracker::State::Completed);

    FormationLegTracker stuck;
    stuck.start(parallelLeg(), 0);
    QVERIFY(stuck.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 60001, 12).isEmpty());
    QCOMPARE(stuck.state(), FormationLegTracker::State::TimedOut);
    QVERIFY2(stuck.reason().contains(QStringLiteral("1")) && stuck.reason().contains(QStringLiteral("2")),
             qPrintable(stuck.reason()));
}

void FormationLegPlannerTest::_trackerContinuesWithoutRemovedMember()
{
    FormationLegTracker tracker;
    tracker.start(parallelLeg(), 0);
    tracker.removeMember(2);
    tracker.update({{1, at(100, 0)}}, {}, 20000, 12);
    QCOMPARE(tracker.state(), FormationLegTracker::State::Completed);

    FormationLegTracker rejected;
    rejected.start(parallelLeg(), 0);
    rejected.abort(QStringLiteral("2 号机拒绝航段指令"));
    rejected.abort(QStringLiteral("ignored once finished"));
    QCOMPARE(rejected.state(), FormationLegTracker::State::Aborted);
    QCOMPARE(rejected.reason(), QStringLiteral("2 号机拒绝航段指令"));
    QVERIFY(rejected.update({{1, at(0, 0)}, {2, at(0, 10)}}, {}, 1000, 12).isEmpty());
}
```

- [ ] **Step 3: 注册源文件并确认链接失败**

`src/Swarm/CMakeLists.txt` 在 `FormationLegPlanner.h` 之后加：
```cmake
            FormationLegTracker.cc
            FormationLegTracker.h
```
并建只含 include 的 `src/Swarm/FormationLegTracker.cc`（Step 4 填入实现）：
```cpp
#include "FormationLegTracker.h"
```
Run: `/tmp/qgc_build.sh 2>&1 | grep -E "undefined reference" | head -3`
Expected: `undefined reference to FormationLegTracker::start` 等。

- [ ] **Step 4: 实现**

`src/Swarm/FormationLegTracker.cc`（整体替换 Step 3 的占位内容）：
```cpp
#include "FormationLegTracker.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QStringList>
#include <algorithm>
#include <cmath>

using FormationLegPlanner::LegMember;

void FormationLegTracker::start(const FormationLegPlanner::Leg& leg, qint64 nowMs)
{
    reset();
    _leg = leg;
    _startMs = nowMs;
    _state = State::Running;
}

void FormationLegTracker::reset()
{
    *this = FormationLegTracker();
}

double FormationLegTracker::elapsedSec(qint64 nowMs) const
{
    return _state == State::Idle ? 0.0 : (nowMs - _startMs) / 1000.0;
}

void FormationLegTracker::markSent(int id, double speed)
{
    _sentSpeed.insert(id, speed);
}

void FormationLegTracker::removeMember(int id)
{
    _leg.members.removeIf([id](const LegMember& member) { return member.id == id; });
    _sentSpeed.remove(id);
    _progress.remove(id);
    _distanceToEnd.remove(id);
}

void FormationLegTracker::abort(const QString& reason)
{
    if (_state != State::Running)
        return;
    _state = State::Aborted;
    _reason = reason;
}

bool FormationLegTracker::_arrived(const LegMember& member, const QGeoCoordinate& position) const
{
    return position.distanceTo(member.end) <= kArrivalHorizontalMeters &&
           std::abs(position.altitude() - member.end.altitude()) <= kArrivalVerticalMeters;
}

double FormationLegTracker::_speedCap(double baseSpeed, double maxSpeed) const
{
    if (std::isfinite(maxSpeed) && maxSpeed > 0)
        return std::max(baseSpeed, kMaxSpeedFactor * maxSpeed);
    return std::max(1.5 * baseSpeed, baseSpeed + 1.0);
}

QList<FormationLegTracker::Command> FormationLegTracker::update(const QHash<int, QGeoCoordinate>& positions,
                                                                const QSet<int>& pending, qint64 nowMs,
                                                                double maxSpeed)
{
    if (_state != State::Running)
        return {};

    // 进度：实测位置在起点→终点连线上的投影比例；水平航程可忽略的成员不参与共识
    double progressSum = 0;
    int moving = 0;
    QStringList late;
    for (const LegMember& member : std::as_const(_leg.members)) {
        const QGeoCoordinate position = positions.value(member.id);
        if (!position.isValid() || !std::isfinite(position.altitude())) {
            late.append(QString::number(member.id));
            continue;
        }
        _distanceToEnd.insert(member.id, position.distanceTo(member.end));
        if (!_arrived(member, position))
            late.append(QString::number(member.id));
        if (member.distance < FormationLegPlanner::kNegligibleDistance) {
            _progress.insert(member.id, 1.0);
            continue;
        }
        const auto path = FormationLegPlanner::localOffset(member.start, member.end);
        const auto actual = FormationLegPlanner::localOffset(member.start, position);
        const double s = std::clamp((actual.east * path.east + actual.north * path.north) /
                                        (member.distance * member.distance),
                                    0.0, 1.0);
        _progress.insert(member.id, s);
        progressSum += s;
        ++moving;
    }
    _consensus = moving > 0 ? progressSum / moving : 1.0;

    // 偏离：实测位置与「共识进度处的同步理想位置」的水平距离
    _deviation = 0;
    for (const LegMember& member : std::as_const(_leg.members)) {
        const QGeoCoordinate position = positions.value(member.id);
        if (member.distance < FormationLegPlanner::kNegligibleDistance || !position.isValid())
            continue;
        const auto path = FormationLegPlanner::localOffset(member.start, member.end);
        const auto actual = FormationLegPlanner::localOffset(member.start, position);
        _deviation = std::max(_deviation, std::hypot(actual.east - _consensus * path.east,
                                                     actual.north - _consensus * path.north));
    }
    _maxDeviation = std::max(_maxDeviation, _deviation);

    if (late.isEmpty()) {
        _state = State::Completed;
        return {};
    }
    if (elapsedSec(nowMs) > 2.0 * _leg.durationSec + kTimeoutSlackSec) {
        _state = State::TimedOut;
        _reason = QCoreApplication::translate("FormationLegTracker", "航段超时：%1 号机未到位")
                      .arg(late.join(QStringLiteral("、")));
        return {};
    }

    QList<Command> commands;
    for (const LegMember& member : std::as_const(_leg.members)) {
        const QGeoCoordinate position = positions.value(member.id);
        if (pending.contains(member.id) || !position.isValid())
            continue;
        if (!_sentSpeed.contains(member.id)) {
            commands.append({member.id, member.end, member.speed});
            continue;
        }
        if (member.speed <= 0 || _arrived(member, position))
            continue;
        const double lag = (_consensus - _progress.value(member.id)) * member.distance;
        const double speed = std::clamp(member.speed + kSpeedGain * lag, kMinSpeedFactor * member.speed,
                                        _speedCap(member.speed, maxSpeed));
        if (std::abs(speed - _sentSpeed.value(member.id)) >= kResendThreshold)
            commands.append({member.id, member.end, speed});
    }
    return commands;
}
```

- [ ] **Step 5: 编译并跑测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh -R "'^FormationLegPlannerTest$'" 2>&1 | tail -4
```
Expected: 全部通过。

---

### Task 4: 长机航线 → 编队航点

**Files:**
- Modify: `src/Swarm/SwarmMissionSnapshot.h`, `src/Swarm/SwarmMissionSnapshot.cc`
- Modify: `test/Swarm/FormationLegPlannerTest.h/.cc`

- [ ] **Step 1: 写失败的测试**

`FormationLegPlannerTest.h` 追加 slot：`void _missionWaypointsForFormation();`

`FormationLegPlannerTest.cc` 加 include：
```cpp
#include <QtCore/QScopeGuard>

#include "MissionItem.h"
#include "SwarmMissionSnapshot.h"
```
测试：
```cpp
void FormationLegPlannerTest::_missionWaypointsForFormation()
{
    const auto item = [](int sequence, MAV_CMD command, MAV_FRAME frame, double lat, double lon, double alt) {
        return new MissionItem(sequence, command, frame, 0, 0, 0, NAN, lat, lon, alt, true, false);
    };
    QList<MissionItem*> items{item(0, MAV_CMD_NAV_TAKEOFF, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.397, 8.5455, 20),
                              item(1, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 47.398, 8.5455, 25),
                              item(2, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 47.398, 8.547, 540),
                              item(3, MAV_CMD_NAV_RETURN_TO_LAUNCH, MAV_FRAME_MISSION, 0, 0, 0)};
    const auto cleanup = qScopeGuard([&items] { qDeleteAll(items); });
    bool trailingRecovery = false;
    QString reason;
    const QList<QGeoCoordinate> waypoints =
        SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason);
    QCOMPARE(waypoints.size(), 2);
    QCOMPARE(waypoints[0].altitude(), 525.0);
    QCOMPARE(waypoints[1].altitude(), 540.0);
    QVERIFY(trailingRecovery);

    items.insert(2, item(2, MAV_CMD_DO_CHANGE_SPEED, MAV_FRAME_MISSION, 0, 0, 0));
    QVERIFY(SwarmMissionSnapshot::capture(items, false).formationWaypoints(500, trailingRecovery, reason).isEmpty());
    QVERIFY2(reason.contains(QStringLiteral("不支持")), qPrintable(reason));
}
```

- [ ] **Step 2: 声明接口**

`SwarmMissionSnapshot.h`：
- 加 `#include <QtPositioning/QGeoCoordinate>`；
- 在 `validate(...)` 声明之后加：
```cpp
    /// 编队航线可用的航点（AMSL 高度）：首项起飞跳过；末项降落/返航不由编队执行（置 trailingRecovery）；
    /// 其余必须是全局或相对高度坐标系的 NAV_WAYPOINT。不满足时返回空并写 reason。
    QList<QGeoCoordinate> formationWaypoints(double homeAltitude, bool& trailingRecovery, QString& reason) const;
```

- [ ] **Step 3: 编译确认链接失败**

Run: `/tmp/qgc_build.sh 2>&1 | grep -E "undefined reference" | head -3`
Expected: `undefined reference to SwarmMissionSnapshot::formationWaypoints`。

- [ ] **Step 4: 实现**

`SwarmMissionSnapshot.cc` 末尾追加：
```cpp
QList<QGeoCoordinate> SwarmMissionSnapshot::formationWaypoints(double homeAltitude, bool& trailingRecovery,
                                                               QString& reason) const
{
    trailingRecovery = false;
    QList<QGeoCoordinate> waypoints;
    QStringList unsupported;
    for (int index = 0; index < _items.size(); ++index) {
        const auto& item = _items[index];
        const int command = int(item[0]), frame = int(item[1]);
        if (command == MAV_CMD_NAV_TAKEOFF && index == 0)
            continue;
        if (index == _items.size() - 1 && (command == MAV_CMD_NAV_LAND || command == MAV_CMD_NAV_RETURN_TO_LAUNCH)) {
            trailingRecovery = true;
            continue;
        }
        if (command != MAV_CMD_NAV_WAYPOINT || (frame != MAV_FRAME_GLOBAL && frame != MAV_FRAME_GLOBAL_RELATIVE_ALT) ||
            !SwarmTypes::isValidLatLon(item[6], item[7]) || !std::isfinite(item[8])) {
            unsupported.append(QStringLiteral("#%1(%2)").arg(index + 1).arg(command));
            continue;
        }
        waypoints.append(
            QGeoCoordinate(item[6], item[7], item[8] + (frame == MAV_FRAME_GLOBAL_RELATIVE_ALT ? homeAltitude : 0.0)));
    }
    if (!unsupported.isEmpty()) {
        reason = QStringLiteral("航线包含编队不支持的项：%1（仅支持航点；首项起飞、末项降落/返航除外）")
                     .arg(unsupported.join(QStringLiteral("、")));
        return {};
    }
    if (waypoints.isEmpty()) {
        reason = QStringLiteral("航线没有可执行的航点");
        return {};
    }
    return waypoints;
}
```
（本文件其余提示也用 `QStringLiteral`，保持一致。若 `QStringList` 未包含，加 `#include <QtCore/QStringList>`。）

- [ ] **Step 5: 编译并跑测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh -R "'^FormationLegPlannerTest$'" 2>&1 | tail -4
```
Expected: 全部通过。

---

### Task 4.5: 抽出 Swarm 集成测试夹具，新建 SwarmFormationTest

**Files:**
- Create: `test/Swarm/SwarmVehicleTest.h`, `test/Swarm/SwarmVehicleTest.cc`
- Create: `test/Swarm/SwarmFormationTest.h`, `test/Swarm/SwarmFormationTest.cc`
- Modify: `test/Swarm/SwarmRegressionTest.h`, `test/Swarm/SwarmRegressionTest.cc`, `test/Swarm/CMakeLists.txt`

纯重构：行为不变，`SwarmRegressionTest` 与新类全部通过即可。QtTest 只把 **private** slots 当测试函数，
夹具里的 `init`/`cleanup` 是 protected slots（与 `VehicleTest` 一致），不会被当作测试。

- [ ] **Step 1: 夹具头文件**

`test/Swarm/SwarmVehicleTest.h`：
```cpp
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QPointer>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>
#include <memory>

#include "BaseClasses/VehicleTest.h"

class SwarmManager;
class Vehicle;

/// Swarm 多机集成测试夹具：每个用例前连接主 MockLink、隔离集群约束/任务配置并创建 SwarmManager，
/// 用例后断开附加 MockLink 并恢复配置。只含夹具，不含测试函数。
class SwarmVehicleTest : public VehicleTest
{
    Q_OBJECT

public:
    using VehicleTest::VehicleTest;
    ~SwarmVehicleTest() override;

protected slots:
    void init() override;
    void cleanup() override;

protected:
    struct Reposition
    {
        QGeoCoordinate target;
        double speed = 0;
        bool valid() const { return target.isValid(); }
    };

    /// MockLink 记录的最后一条 COMMAND_INT(DO_REPOSITION)；编队指令总是经 COMMAND_INT 下发
    static Reposition lastFormationReposition(MockLink* link);
    /// 覆盖长机的地速遥测（MockLink 下一帧遥测会改回）
    static void setLeaderGroundSpeed(Vehicle* vehicle, double speed);

    /// 追加 count 架 PX4 MockLink 并等待初始连接完成
    void addVehicles(int count);
    /// 全体起飞到 10 m 并等待；followers 返回主机以外的成员（按 id 升序）
    void takeOffAll(QList<Vehicle*>& followers);

    std::unique_ptr<SwarmManager> _manager;
    QPointer<MockLink> _secondaryLink;
    QList<QPointer<MockLink>> _extraLinks;
    QString _configPath;
    QByteArray _previousConfig;
    bool _hadConfig = false;
};
```

- [ ] **Step 2: 夹具实现（init/cleanup/addVehicles 从 SwarmRegressionTest 原样迁出）**

`test/Swarm/SwarmVehicleTest.cc`：
```cpp
#include "SwarmVehicleTest.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QStandardPaths>
#include <algorithm>

#include "Fact.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmClock.h"
#include "SwarmManager.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

SwarmVehicleTest::~SwarmVehicleTest() = default;
```
然后把 `SwarmRegressionTest.cc` 中的 `init()`、`cleanup()`、`addVehicles()` 三个函数**原样剪切**过来，
只把类名前缀 `SwarmRegressionTest::` 改为 `SwarmVehicleTest::`，并把 `init()` 忽略正则开头的
`"(集群任务|阶段 |阶段「|` 改为 `"(集群任务|编队|阶段 |阶段「|`（后续任务的编队反馈会进入应用消息条）。
再在文件末尾加：
```cpp
void SwarmVehicleTest::takeOffAll(QList<Vehicle*>& followers)
{
    auto* mvm = MultiVehicleManager::instance();
    followers.clear();
    QList<Vehicle*> members;
    for (int i = 0; i < mvm->vehicles()->count(); ++i) {
        auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
        QVERIFY(member);
        member->sendMavCommand(member->defaultComponentId(), MAV_CMD_NAV_TAKEOFF, true, 0, 0, 0, 0, 0, 0, 10);
        members.append(member);
        if (member != vehicle())
            followers.append(member);
    }
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(members.cbegin(), members.cend(),
                                         [](Vehicle* v) {
                                             return v->armed() && v->altitudeRelative()->rawValue().toDouble() >= 9;
                                         }),
                             TestTimeout::longMs());
    std::sort(followers.begin(), followers.end(), [](Vehicle* a, Vehicle* b) { return a->id() < b->id(); });
}

SwarmVehicleTest::Reposition SwarmVehicleTest::lastFormationReposition(MockLink* link)
{
    mavlink_message_t message{};
    if (!link || !link->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_COMMAND_INT, message))
        return {};
    mavlink_command_int_t command{};
    mavlink_msg_command_int_decode(&message, &command);
    if (command.command != MAV_CMD_DO_REPOSITION)
        return {};
    return {QGeoCoordinate(command.x / 1e7, command.y / 1e7, command.z), command.param1};
}

void SwarmVehicleTest::setLeaderGroundSpeed(Vehicle* vehicle, double speed)
{
    qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup())->groundSpeed()->setRawValue(speed);
}
```
（若迁出的代码用到其他头文件——例如 `QScopeGuard`——一并补上 include；编译器会指出。）

- [ ] **Step 3: SwarmRegressionTest 改为继承夹具**

`SwarmRegressionTest.h`：
- 把 `#include "BaseClasses/VehicleTest.h"` 改为 `#include "SwarmVehicleTest.h"`；删除 `class SwarmManager;` 前置声明；
- `class SwarmRegressionTest : public VehicleTest` 改为 `: public SwarmVehicleTest`，`using VehicleTest::VehicleTest;`
  改为 `using SwarmVehicleTest::SwarmVehicleTest;`；删除析构函数声明；
- 从 `private slots:` 删除 `void init() override;`、`void cleanup() override;`、`void operatorGotoMovesWholeFormation();`；
- 删除整个 `private:` 段（`_manager`、`_secondaryLink`、`_extraLinks`、`addVehicles`、`_configPath`、`_previousConfig`、`_hadConfig` 已在夹具中）。

`SwarmRegressionTest.cc`：删除 `SwarmRegressionTest::~SwarmRegressionTest() = default;`，删除已剪切走的
`init()`/`cleanup()`/`addVehicles()`，并把 `operatorGotoMovesWholeFormation()` 整个函数**剪切**到下一步的新类。

- [ ] **Step 4: 新建 SwarmFormationTest，迁入 operatorGotoMovesWholeFormation**

`test/Swarm/SwarmFormationTest.h`：
```cpp
#pragma once

#include "SwarmVehicleTest.h"

/// 编队航段（设计 2026-10-05-synchronized-formation-legs）的多机集成测试
class SwarmFormationTest : public SwarmVehicleTest
{
    Q_OBJECT

public:
    using SwarmVehicleTest::SwarmVehicleTest;

private slots:
    void operatorGotoMovesWholeFormation();
};
```
`test/Swarm/SwarmFormationTest.cc`：
```cpp
#include "SwarmFormationTest.h"

#include <QtCore/QRegularExpression>
#include <QtTest/QSignalSpy>
#include <algorithm>

#include "FirmwarePlugin.h"
#include "FormationController.h"
#include "MissionItem.h"
#include "MissionManager.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmManager.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

// （此处粘贴从 SwarmRegressionTest.cc 剪切来的 operatorGotoMovesWholeFormation()，
//   把类名前缀改为 SwarmFormationTest::，函数体不变）

UT_REGISTER_TEST(SwarmFormationTest, TestLabel::Integration, TestLabel::Vehicle)
```
（上面括号内是操作说明，不要把注释留在文件里。）

`test/Swarm/CMakeLists.txt` 追加：
```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmVehicleTest.cc SwarmVehicleTest.h)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE SwarmFormationTest.cc SwarmFormationTest.h)
add_qgc_test(SwarmFormationTest LABELS Integration Vehicle)
```
（夹具不是测试，不调用 `add_qgc_test`、不 `UT_REGISTER_TEST`。）

- [ ] **Step 5: 编译并跑两类测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh --timeout 400 -R "'^SwarmRegressionTest$|^SwarmFormationTest$'" 2>&1 | tail -5
```
Expected: 两类全部通过（`--timeout 400` 只为在高负载下看清结果；拆分后正常负载下每类都应低于 120 s，记录两类实际耗时）。

---

### Task 5: 带地速的 DO_REPOSITION 与 MockLink 支持

**Files:**
- Modify: `src/FirmwarePlugin/FirmwarePlugin.h`, `src/FirmwarePlugin/FirmwarePlugin.cc`
- Modify: `src/FirmwarePlugin/PX4/PX4FirmwarePlugin.h`, `src/FirmwarePlugin/PX4/PX4FirmwarePlugin.cc`
- Modify: `src/Comms/MockLink/MockLink.h`, `src/Comms/MockLink/MockLink.cc`
- Modify: `test/Swarm/SwarmFormationTest.h`, `test/Swarm/SwarmFormationTest.cc`

- [ ] **Step 1: 写失败的测试**

`SwarmFormationTest.h` 的 `private slots:` 追加 `void px4RepositionCarriesGroundSpeed();`
（`Reposition` 与 `lastFormationReposition` 已在夹具 `SwarmVehicleTest` 中。）

在 `SwarmFormationTest.cc` 的 `UT_REGISTER_TEST` 之前加测试：
```cpp
void SwarmFormationTest::px4RepositionCarriesGroundSpeed()
{
    const QGeoCoordinate target(47.3975, 8.5460, 520.5);
    QSignalSpy results(vehicle(), &Vehicle::mavCommandResult);
    QVERIFY(vehicle()->firmwarePlugin()->guidedModeGotoLocationAtSpeed(vehicle(), target, 4.5));
    QTRY_VERIFY_WITH_TIMEOUT(lastFormationReposition(mockLink()).valid(), TestTimeout::longMs());
    const Reposition command = lastFormationReposition(mockLink());
    QCOMPARE(command.speed, 4.5);
    QVERIFY(command.target.distanceTo(target) < 0.05);
    QCOMPARE(command.target.altitude(), 520.5);
    // MockLink rejects COMMAND_INT repositions unless they are enabled, then accepts them.
    QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), TestTimeout::longMs());
    QCOMPARE(results.last()[3].toInt(), int(MAV_RESULT_UNSUPPORTED));
    mockLink()->setRepositionSupported(true);
    results.clear();
    QVERIFY(vehicle()->firmwarePlugin()->guidedModeGotoLocationAtSpeed(vehicle(), target, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!results.isEmpty(), TestTimeout::longMs());
    QCOMPARE(results.last()[3].toInt(), int(MAV_RESULT_ACCEPTED));
    QCOMPARE(lastFormationReposition(mockLink()).speed, -1.0);
}
```

- [ ] **Step 2: 声明接口**

`FirmwarePlugin.h`：在 `guidedModeGotoLocationAndAltitude(...)` 声明（约 241 行）之后加：
```cpp
    /// Command vehicle to move to the specified location and altitude at the given ground speed.
    /// The base implementation ignores the speed; firmwares supporting a DO_REPOSITION speed override it.
    /// @param groundSpeed meters/second, <= 0 for the vehicle's default speed
    virtual bool guidedModeGotoLocationAtSpeed(Vehicle* vehicle, const QGeoCoordinate& gotoCoord,
                                               double groundSpeed) const;
```
`PX4FirmwarePlugin.h`：在 `changeAltitudeAsync(...) override;` 之后加：
```cpp
    bool guidedModeGotoLocationAtSpeed(Vehicle* vehicle, const QGeoCoordinate& gotoCoord,
                                       double groundSpeed) const override;
```

- [ ] **Step 3: 编译确认链接失败**

Run: `/tmp/qgc_build.sh 2>&1 | grep -E "undefined reference" | head -3`
Expected: `undefined reference to FirmwarePlugin::guidedModeGotoLocationAtSpeed`（此次编译较慢）。

- [ ] **Step 4: 实现**

`FirmwarePlugin.cc`：在 `guidedModeGotoLocationAndAltitude` 定义之后加：
```cpp
bool FirmwarePlugin::guidedModeGotoLocationAtSpeed(Vehicle* vehicle, const QGeoCoordinate& gotoCoord,
                                                   double groundSpeed) const
{
    Q_UNUSED(groundSpeed);
    return guidedModeGotoLocationAndAltitude(vehicle, gotoCoord);
}
```
`PX4FirmwarePlugin.cc`：在 `guidedModeGotoLocation` 定义之后加：
```cpp
bool PX4FirmwarePlugin::guidedModeGotoLocationAtSpeed(Vehicle* vehicle, const QGeoCoordinate& gotoCoord,
                                                      double groundSpeed) const
{
    if (!vehicle || !gotoCoord.isValid() || !std::isfinite(gotoCoord.altitude())) {
        QGC::showAppMessage(tr("Unable to go to location, target position or altitude not known."));
        return false;
    }
    // param1 is the ground speed (-1: default). Yaw (param4) stays NaN: PX4 v1.15 reads it as radians,
    // unlike the MAVLink definition in degrees, so heading is left to the vehicle's yaw mode.
    vehicle->sendMavCommandInt(vehicle->defaultComponentId(), MAV_CMD_DO_REPOSITION, MAV_FRAME_GLOBAL,
                               true,  // show error if command fails
                               groundSpeed > 0 ? static_cast<float>(groundSpeed) : -1.0f,
                               MAV_DO_REPOSITION_FLAGS_CHANGE_MODE, 0.0f, NAN, gotoCoord.latitude(),
                               gotoCoord.longitude(), static_cast<float>(gotoCoord.altitude()));
    return true;
}
```
`MockLink.cc` 的 `_handleCommandInt`：把
```cpp
    // MockLink does not implement any COMMAND_INT commands yet, so it reports them as
    // unsupported (mirroring the COMMAND_LONG default for unrecognized commands). This
    // lets unit tests exercise "try command, fall back to legacy message" code paths
    // such as Vehicle::setEstimatorOrigin.
    const uint8_t commandResult = MAV_RESULT_UNSUPPORTED;
```
替换为：
```cpp
    // Only DO_REPOSITION is implemented, and only when enabled; everything else is reported as
    // unsupported (mirroring the COMMAND_LONG default for unrecognized commands). This lets unit
    // tests exercise "try command, fall back to legacy message" code paths such as
    // Vehicle::setEstimatorOrigin.
    uint8_t commandResult = MAV_RESULT_UNSUPPORTED;
    if (request.command == MAV_CMD_DO_REPOSITION && _repositionSupported) {
        if (std::isfinite(request.z)) {
            _vehicleAltitudeAMSL = request.z;
        }
        commandResult = MAV_RESULT_ACCEPTED;
    }
```
`MockLink.h` 中 `setRepositionSupported` 的注释改为：
```cpp
    /// Accept MAV_CMD_DO_REPOSITION (COMMAND_LONG and COMMAND_INT) and apply its altitude. Off by default:
    /// the command is then answered MAV_RESULT_UNSUPPORTED, which existing tests rely on.
```

- [ ] **Step 5: 编译并跑测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh --timeout 400 -R "'^SwarmFormationTest$|^SwarmRegressionTest$|^SwarmExecutionTest$'" 2>&1 | tail -5
```
Expected: 三个测试类全部通过（`SwarmExecutionTest` 用 COMMAND_LONG 的重定位，不受影响）。

---

### Task 6: FormationController 接入航段

**Files:**
- Rewrite: `src/Swarm/FormationController.h`, `src/Swarm/FormationController.cc`
- Modify: `test/Swarm/SwarmRegressionTest.cc`（既有编队测试的最小调整）
- Modify: `test/Swarm/SwarmFormationTest.h`, `test/Swarm/SwarmFormationTest.cc`（新测试）

- [ ] **Step 1: 写失败的集成测试，并调整既有编队测试**

`SwarmFormationTest.h` 的 `private slots:` 追加 `void formationStartConvergesInSync();` 与
`void formationLegRequiresLeaderLease();`。`setLeaderGroundSpeed`、`takeOffAll`、`lastFormationReposition`
都已在夹具 `SwarmVehicleTest` 中（Task 4.5）。

`SwarmRegressionTest.cc` 的既有编队测试调整如下（第 1–3 项已由 Task 4.5 完成，从第 4 项开始）：
4. `activeFormationEditsUpdateTargets()`：把
```cpp
    const auto tick = [&](qint64 milliseconds = 1000) {
        now += milliseconds;
        // MockLink varies yaw in its telemetry; use a fixed reference for convergence assertions.
        vehicle()->heading()->setRawValue(formationHeading);
        QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
        waitCommands();
    };
    QVERIFY(formation.start(vehicle()->id()));
    verifyMeasuredTargets();
    const QList<FormationController::FormationType> shapes{
```
替换为：
```cpp
    // MockLink varies yaw in its telemetry; use a fixed reference for convergence assertions. A leader
    // moving faster than 1 m/s skips the synchronized reshape leg, so this test covers station keeping.
    const auto driveLeader = [&] {
        vehicle()->heading()->setRawValue(formationHeading);
        setLeaderGroundSpeed(vehicle(), 2.0);
    };
    const auto tick = [&](qint64 milliseconds = 1000) {
        now += milliseconds;
        driveLeader();
        QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
        waitCommands();
    };
    driveLeader();
    QVERIFY(formation.start(vehicle()->id()));
    verifyMeasuredTargets();
    const QList<FormationController::FormationType> shapes{
```
同一函数里把
```cpp
    formation.setFormationType(FormationController::FormationLine);
    now += 1000;
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
```
改为
```cpp
    formation.setFormationType(FormationController::FormationLine);
    now += 1000;
    driveLeader();
    QVERIFY(QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection));
```
并把函数末尾
```cpp
    QVERIFY(targets.isEmpty());
    QVERIFY(formation.start(vehicle()->id()));
    verifyMeasuredTargets();
}
```
改为
```cpp
    QVERIFY(targets.isEmpty());
    driveLeader();
    QVERIFY(formation.start(vehicle()->id()));
    verifyMeasuredTargets();
}
```
5. `formationSkipsFollowerAwaitingRepositionAck()`：在 `// start() runs one cycle immediately.` 这行之前插入：
```cpp
    // A moving leader skips the synchronized reshape leg; this test covers station keeping.
    setLeaderGroundSpeed(vehicle(), 2.0);
```
6. `formationRebuildsSlotsWhenMemberLacksOne()`：把
```cpp
        QTRY_COMPARE_WITH_TIMEOUT(pendingCommands, 0, TestTimeout::longMs());
    };
    QVERIFY(formation.start(vehicle()->id()));
```
改为
```cpp
        QTRY_COMPARE_WITH_TIMEOUT(pendingCommands, 0, TestTimeout::longMs());
    };
    // A moving leader skips the synchronized reshape leg; this test covers station keeping.
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(vehicle()->id()));
```
7. 新测试（加在 `SwarmFormationTest.cc` 的 `UT_REGISTER_TEST` 之前）：
```cpp
void SwarmFormationTest::formationStartConvergesInSync()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    mockLink()->clearReceivedMavCommandCounts();
    QHash<int, QGeoCoordinate> starts;
    QSet<int> members{vehicle()->id()};
    for (Vehicle* follower : std::as_const(followers)) {
        starts.insert(follower->id(), follower->coordinate());
        members.insert(follower->id());
    }
    FormationController formation;
    formation.setMembers(members);
    // MockLink places later vehicles north-east of the leader. Facing south-west puts the wedge slots on
    // that side, so the synchronized straight-line paths stay clear of the hovering leader.
    vehicle()->heading()->setRawValue(214);
    setLeaderGroundSpeed(vehicle(), 0.0);
    QVERIFY(formation.start(vehicle()->id()));
    QVERIFY(formation.transitActive());
    QCOMPARE(formation.transitStatus().value(QStringLiteral("kind")).toString(), QStringLiteral("reshape"));
    QTRY_VERIFY_WITH_TIMEOUT(std::all_of(_extraLinks.cbegin(), _extraLinks.cend(),
                                         [](const QPointer<MockLink>& link) {
                                             return lastFormationReposition(link).valid();
                                         }),
                             TestTimeout::longMs());
    QList<double> arrivalSeconds;
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks)) {
        const Reposition command = lastFormationReposition(link);
        QVERIFY(command.speed > 0);
        arrivalSeconds.append(starts.value(link->vehicleId()).distanceTo(command.target) / command.speed);
    }
    QVERIFY2(qAbs(arrivalSeconds.first() - arrivalSeconds.last()) < 0.5,
             qPrintable(QStringLiteral("arrivals %1 s / %2 s").arg(arrivalSeconds.first()).arg(arrivalSeconds.last())));
    // An in-place reshape never commands the leader: it keeps whatever it is doing.
    QCOMPARE(mockLink()->receivedMavCommandCount(MAV_CMD_DO_REPOSITION), 0);
    formation.stop();
    QVERIFY(!formation.transitActive());
}

void SwarmFormationTest::formationLegRequiresLeaderLease()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(1);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    FormationController formation;
    formation.setMembers({vehicle()->id(), followers.first()->id()});
    bool leaderAllowed = false;
    const int leaderId = vehicle()->id();
    formation.setVehicleAutomationAllowedCallback([&](int id) { return id != leaderId || leaderAllowed; });
    setLeaderGroundSpeed(vehicle(), 2.0);
    QVERIFY(formation.start(leaderId));
    const QGeoCoordinate goal = vehicle()->coordinate().atDistanceAndAzimuth(100, 0);
    QString reason;
    QVERIFY(!formation.gotoFormation(goal, &reason));
    QVERIFY2(reason.contains(QStringLiteral("自动控制许可")), qPrintable(reason));
    QVERIFY(!formation.transitActive());
    leaderAllowed = true;
    QVERIFY2(formation.gotoFormation(goal, &reason), qPrintable(reason));
    QVERIFY(formation.transitActive());
    formation.stop();
}
```

- [ ] **Step 2: 编译确认失败**

Run: `/tmp/qgc_build.sh 2>&1 | grep -E "error:" | head -5`
Expected: `no member named 'transitActive' / 'gotoFormation' / 'transitStatus' in 'FormationController'`。

- [ ] **Step 3: 重写 `src/Swarm/FormationController.h`**

```cpp
/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — FormationController
 *
 * 长机-僚机（Leader-Follower）编队控制器：
 *   - 队形：楔形 / 纵队 / 横队 / 环形
 *   - 保持阶段（StationKeeping）：每个控制周期按长机位置与编队朝向计算各僚机槽位，
 *     通过 guidedModeGotoLocation（MAV_CMD_DO_REPOSITION）下发。
 *   - 航段阶段（Transit）：同步编队航段——全队（含长机）同时受令、速度与航程成比例、同时到达，
 *     每拍按进度共识调速（FormationLegPlanner / FormationLegTracker）。地图 goto、编队航线、
 *     启动与变队形都走航段。设计见 swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md。
 *   - 编队朝向：长机水平速度超过 1 m/s（由其他方式驱动）时跟随长机航向，否则保持上一航段结束时的朝向。
 *   - 槽位分配：首次按 id 升序；航段规划按最小总位移重新分配（代价接近时保持原分配）。
 *
 * 安全设计：
 *   - 楔形/纵队默认带高度分层（verticalStepMeters），避免僚机处于长机下洗流中；
 *   - 航段规划逐对校验同步直线运动下的最小间距，不满足时拆段或垂直分层，仍不满足则拒绝；
 *   - 成员退出时尝试长机接替；无合格长机时阻塞引导并通知界面；
 *   - 仅对已解锁且已离地的僚机下发位置指令；goto/航线航段中长机也须持有自动控制租约。
 *
 * 说明：1 Hz 的 DO_REPOSITION 引导适用于 SITL 验证与低速编队；
 *       需要更平滑/高速的编队时，可改用 OFFBOARD + SET_POSITION_TARGET_GLOBAL_INT
 *       高频设定点流（见设计文档扩展路线）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QVariantMap>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>
#include <functional>

#include "FormationLegPlanner.h"
#include "FormationLegTracker.h"

class Vehicle;

class FormationController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(FormationType formationType READ formationType WRITE setFormationType NOTIFY formationTypeChanged)
    Q_PROPERTY(double spacingMeters READ spacingMeters WRITE setSpacingMeters NOTIFY spacingMetersChanged)
    Q_PROPERTY(
        double verticalStepMeters READ verticalStepMeters WRITE setVerticalStepMeters NOTIFY verticalStepMetersChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool blocked READ blocked NOTIFY activeChanged)
    Q_PROPERTY(int leaderVehicleId READ leaderVehicleId NOTIFY leaderVehicleIdChanged)
    Q_PROPERTY(int followerCount READ followerCount NOTIFY followerCountChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(bool transitActive READ transitActive NOTIFY transitChanged)

public:
    explicit FormationController(QObject* parent = nullptr);
    ~FormationController();

    enum FormationType
    {
        FormationWedge = 0,  ///< 楔形（V 字）
        FormationColumn,     ///< 纵队
        FormationLine,       ///< 横队
        FormationCircle      ///< 环形
    };
    Q_ENUM(FormationType)

    FormationType formationType() const { return _formationType; }

    void setFormationType(FormationType type);

    double spacingMeters() const { return _spacingMeters; }

    void setSpacingMeters(double spacing);

    double verticalStepMeters() const { return _verticalStepMeters; }

    void setVerticalStepMeters(double step);

    bool active() const { return _active; }

    bool blocked() const { return _blocked; }

    int leaderVehicleId() const { return _leaderVehicleId; }

    int followerCount() const { return _followerCount; }

    QString statusText() const { return _statusText; }

    bool transitActive() const { return _legCount > 0; }

    int transitLegIndex() const { return _legIndex; }

    int transitLegCount() const { return _legCount; }

    double formationHeading() const { return _formationHeading; }

    /// 以 leaderVehicleId 为长机开始编队。长机由人工/任务驾驶，僚机自动跟随队形。
    bool start(int leaderVehicleId);

    /// 编队整体飞往 coordinate：长机终点为该点，高度保持长机当前高度。失败时 reason 给出原因。
    bool gotoFormation(const QGeoCoordinate& coordinate, QString* reason = nullptr);

    /// 依次飞经 waypoints（长机终点，含 AMSL 高度），每个航点全队对齐后再飞下一段。
    bool startRoute(const QList<QGeoCoordinate>& waypoints, QString* reason = nullptr);

    /// 只规划不执行，供确认框展示航点数与预计时长
    FormationLegPlanner::Plan planRoute(const QList<QGeoCoordinate>& waypoints) const;

    /// 航段状态（阶段、序号、各机起终点与进度、偏离、上次结果），供验收接口
    QVariantMap transitStatus() const;

    /// 僚机 id → 槽位编号（1 起）
    QHash<int, int> slotAssignment() const;

    /// 推进单架僚机的引导目标。目标先随槽位目的地整体平移（跟随长机运动，不限速），
    /// 再朝目的地做限速追赶：限速只约束追赶，避免单拍大幅跳变或时钟跳变放大指令。
    /// previousDestination 无效（首拍）时从 current 起步。
    static QGeoCoordinate advanceGuidanceTarget(const QGeoCoordinate& current, const QGeoCoordinate& previousDestination,
                                                const QGeoCoordinate& destination, double dtSeconds);

    /// 停止自动编队引导；不改变各机飞行模式
    Q_INVOKABLE void stop();

    /// 设置逐机自动控制许可判定。SwarmManager 集成时采用 fail-closed 租约判定。
    void setVehicleAutomationAllowedCallback(std::function<bool(int)> callback);
    void setTargetAllowedCallback(std::function<bool(int, const QGeoCoordinate&)> callback);
    /// 航段路径的区域检查（电子围栏、高度上限）
    void setPathAllowedCallback(std::function<bool(int, const QGeoCoordinate&, const QGeoCoordinate&)> callback);
    /// 约束最小间距（米）
    void setMinSeparationCallback(std::function<double()> callback);
    void setMembers(const QSet<int>& members);
    void removeMembers(const QSet<int>& members);

signals:
    void formationTypeChanged();
    void spacingMetersChanged();
    void verticalStepMetersChanged();
    void activeChanged();
    void leaderVehicleIdChanged();
    void followerCountChanged();
    void statusTextChanged();
    void transitChanged();
    /// 航段或航线结束；success 为 false 时 reason 说明原因
    void transitFinished(bool success, const QString& reason);

private slots:
    void _updateFormation();

private:
    struct Slot
    {
        double distance = 0;
        double bearing = 0;
        double altitude = 0;
        int index = 0;  ///< 槽位编号（0 起）
    };

    struct GuidanceTarget
    {
        QGeoCoordinate coordinate;
        qint64 updatedMs = 0;
        QGeoCoordinate destination;  ///< 上一拍实际下发的槽位目的地；用于随长机运动平移目标
    };

    /// 计算第 followerIndex 架僚机（1 起）的队形偏移
    /// @param distanceMeters   相对长机的水平距离
    /// @param azimuthOffsetDeg 相对长机航向的方位角偏移（0=正前方, 90=右侧, 180=正后方）
    /// @param altitudeStepMeters 相对长机的高度增量
    void _formationOffset(int followerIndex, int totalFollowers, double& distanceMeters, double& azimuthOffsetDeg,
                          double& altitudeStepMeters) const;

    void _setStatusText(const QString& text);
    void _rebuildSlots();
    bool _handover();
    void _onShapeChanged();
    void _block(const QString& text);
    QList<Vehicle*> _eligibleFollowers() const;
    bool _leaderMoving(Vehicle* leader) const;
    double _maxHorizontalSpeed(const QList<Vehicle*>& vehicles) const;
    FormationLegPlanner::Request _legRequest(Vehicle* leader, const QList<Vehicle*>& followers,
                                             const QList<QGeoCoordinate>& waypoints, bool commandLeader) const;
    bool _planTransit(const QList<QGeoCoordinate>& waypoints, const QString& kind, QString* reason);
    void _beginTransit(const FormationLegPlanner::Plan& plan, const FormationLegPlanner::Request& request,
                       const QString& kind);
    bool _beginReshape(Vehicle* leader, const QList<Vehicle*>& followers);
    void _applyAssignment(const FormationLegPlanner::Plan& plan, const FormationLegPlanner::Request& request);
    void _startNextLeg();
    void _updateTransit();
    void _finishTransit(bool success, const QString& reason);

    FormationType _formationType = FormationWedge;
    double _spacingMeters = 10.0;
    double _verticalStepMeters = 3.0;
    int _updateIntervalMs = 1000;        ///< 队形目标点更新周期
    double _minAirborneAltMeters = 1.5;  ///< 判定僚机已离地的相对高度阈值

    QTimer _updateTimer;
    bool _active = false;
    bool _blocked = false;
    int _leaderVehicleId = 0;
    int _followerCount = 0;
    QString _statusText;
    std::function<bool(int)> _vehicleAutomationAllowed;
    std::function<bool(int, const QGeoCoordinate&)> _targetAllowed;
    std::function<bool(int, const QGeoCoordinate&, const QGeoCoordinate&)> _pathAllowed;
    std::function<double()> _minSeparation;
    QSet<int> _members;
    bool _membersBound = false;

    QHash<int, Slot> _slots;
    QHash<int, GuidanceTarget> _guidanceTargets;
    // Limit target motion, independently of the aircraft's own velocity controller.
    static constexpr double _horizontalTargetSpeed = 2.0;  ///< m/s
    static constexpr double _verticalTargetSpeed = 1.0;    ///< m/s

    double _formationHeading = 0;  ///< 编队朝向（度）
    bool _reshapePending = false;  ///< 启动或队形参数变更后，下一拍尝试同步收敛
    FormationLegTracker _tracker;
    QList<FormationLegPlanner::Leg> _legQueue;  ///< 未完成的航段，首项为当前航段
    int _legIndex = 0;
    int _legCount = 0;
    double _transitDurationSec = 0;
    QString _transitKind;  ///< goto / route / reshape
    QVariantMap _lastTransitResult;
    QList<QMetaObject::Connection> _resultConnections;
    QSet<int> _awaitingAck;  ///< 本航段已发出、尚未收到回执的成员
    static constexpr double _transitSpeed = 5.0;        ///< 编队速度 V（m/s）
    static constexpr double _transitSpeedFactor = 0.8;  ///< V 不超过成员最大水平速度最小值的比例
    static constexpr double _leaderMovingSpeed = 1.0;   ///< 长机水平速度超过此值视为由其他方式驱动（m/s）
};
```

- [ ] **Step 4: 重写 `src/Swarm/FormationController.cc`**

```cpp
#include "FormationController.h"

#include <QtPositioning/QGeoCoordinate>
#include <algorithm>
#include <cmath>
#include <utility>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "MultiVehicleManager.h"
#include "QmlObjectListModel.h"
#include "SwarmAuditLogger.h"
#include "SwarmClock.h"
#include "Vehicle.h"
#include "VehicleFactGroup.h"

FormationController::FormationController(QObject* parent) : QObject(parent)
{
    _updateTimer.setInterval(_updateIntervalMs);
    (void) connect(&_updateTimer, &QTimer::timeout, this, &FormationController::_updateFormation);
    _setStatusText(tr("未激活"));
}

FormationController::~FormationController() = default;

void FormationController::setVehicleAutomationAllowedCallback(std::function<bool(int)> callback)
{
    _vehicleAutomationAllowed = std::move(callback);
}

void FormationController::setTargetAllowedCallback(std::function<bool(int, const QGeoCoordinate&)> callback)
{
    _targetAllowed = std::move(callback);
}

void FormationController::setPathAllowedCallback(
    std::function<bool(int, const QGeoCoordinate&, const QGeoCoordinate&)> callback)
{
    _pathAllowed = std::move(callback);
}

void FormationController::setMinSeparationCallback(std::function<double()> callback)
{
    _minSeparation = std::move(callback);
}

void FormationController::setMembers(const QSet<int>& members)
{
    _finishTransit(false, tr("编队成员已变更，航段已取消"));
    _members = members;
    _membersBound = true;
    _slots.clear();
    _guidanceTargets.clear();
}

void FormationController::removeMembers(const QSet<int>& members)
{
    _members.subtract(members);
    if (members.contains(_leaderVehicleId))
        _finishTransit(false, tr("长机 %1 离队，航段已取消").arg(_leaderVehicleId));
    for (int id : members) {
        _slots.remove(id);
        _guidanceTargets.remove(id);
        // 剩余机对的路径已两两校验，僚机离队后其余成员继续当前航段
        _tracker.removeMember(id);
    }
    if (!_active)
        return;
    if (_members.size() < 2) {
        if (!_members.isEmpty()) {
            _leaderVehicleId = *_members.begin();
            emit leaderVehicleIdChanged();
        }
        stop();
    } else if (members.contains(_leaderVehicleId)) {
        if (!_handover())
            _setStatusText(tr("编队阻塞：没有合格继任长机"));
    }
}

bool FormationController::_handover()
{
    auto ids = _members.values();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(id);
        if (!leader || !leader->armed() || !leader->coordinate().isValid() ||
            (_vehicleAutomationAllowed && !_vehicleAutomationAllowed(id)))
            continue;
        auto* facts = qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup());
        if (!facts || facts->altitudeRelative()->rawValue().toDouble() < _minAirborneAltMeters)
            continue;
        const double heading = facts->heading()->rawValue().toDouble();
        const double altitude = facts->altitudeAMSL()->rawValue().toDouble();
        _leaderVehicleId = id;
        _slots.clear();
        _guidanceTargets.clear();
        int index = 0;
        for (int member : ids) {
            Vehicle* follower = MultiVehicleManager::instance()->getVehicleById(member);
            if (!follower || member == id || !follower->coordinate().isValid())
                continue;
            auto* followerFacts = qobject_cast<VehicleFactGroup*>(follower->vehicleFactGroup());
            if (!followerFacts)
                continue;
            _slots.insert(member, {leader->coordinate().distanceTo(follower->coordinate()),
                                   leader->coordinate().azimuthTo(follower->coordinate()) - heading,
                                   followerFacts->altitudeAMSL()->rawValue().toDouble() - altitude, index++});
        }
        _formationHeading = std::isfinite(heading) ? heading : _formationHeading;
        emit leaderVehicleIdChanged();
        _setStatusText(tr("长机交接至 %1，保持成员当前位置关系").arg(id));
        return true;
    }
    return false;
}

void FormationController::setFormationType(FormationType type)
{
    if (_formationType != type) {
        _formationType = type;
        _onShapeChanged();
        emit formationTypeChanged();
    }
}

void FormationController::setSpacingMeters(double spacing)
{
    if (!qFuzzyCompare(_spacingMeters, spacing)) {
        _spacingMeters = qMax(2.0, spacing);  // 硬性下限 2 米，防止误输入导致碰撞
        _onShapeChanged();
        emit spacingMetersChanged();
    }
}

void FormationController::setVerticalStepMeters(double step)
{
    if (!qFuzzyCompare(_verticalStepMeters, step)) {
        _verticalStepMeters = step;
        _onShapeChanged();
        emit verticalStepMetersChanged();
    }
}

void FormationController::_onShapeChanged()
{
    if (!_active)
        return;
    _finishTransit(false, tr("队形参数已变更，航段已取消"));
    _rebuildSlots();
    _reshapePending = true;
}

bool FormationController::start(int leaderVehicleId)
{
    if (_active) {
        stop();
    }

    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    if (_membersBound && !_members.contains(leaderVehicleId))
        return false;
    Vehicle* leader = mvm->getVehicleById(leaderVehicleId);
    if (!leader) {
        _setStatusText(tr("启动失败：找不到长机 %1").arg(leaderVehicleId));
        return false;
    }
    if (!leader->armed() || !leader->coordinate().isValid()) {
        _setStatusText(tr("启动失败：长机 %1 未解锁或定位无效").arg(leaderVehicleId));
        return false;
    }

    _leaderVehicleId = leaderVehicleId;
    _blocked = false;
    _active = true;
    auto* leaderFacts = qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup());
    const double heading = leaderFacts ? leaderFacts->heading()->rawValue().toDouble() : 0.0;
    _formationHeading = std::isfinite(heading) ? heading : 0.0;
    if (!_membersBound) {
        for (int i = 0; i < mvm->vehicles()->count(); ++i) {
            auto* member = qobject_cast<Vehicle*>(mvm->vehicles()->get(i));
            if (member && (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(member->id())))
                _members.insert(member->id());
        }
    }
    _rebuildSlots();
    _reshapePending = true;
    emit leaderVehicleIdChanged();
    emit activeChanged();
    _updateFormation();  // 立即执行一拍
    _updateTimer.start();
    return true;
}

void FormationController::_rebuildSlots()
{
    auto members = _members.values();
    members.removeAll(_leaderVehicleId);
    std::sort(members.begin(), members.end());
    _slots.clear();
    // Discard targets from the previous shape, even if the aircraft never reached them.
    // The first update initializes each follower at its measured position and altitude.
    _guidanceTargets.clear();
    for (int i = 0; i < members.size(); ++i) {
        Slot slot;
        _formationOffset(i + 1, members.size(), slot.distance, slot.bearing, slot.altitude);
        slot.index = i;
        _slots.insert(members[i], slot);
    }
}

void FormationController::stop()
{
    _finishTransit(false, tr("编队已停止，航段已取消"));
    _reshapePending = false;
    _guidanceTargets.clear();
    if (!_active) {
        return;
    }
    _updateTimer.stop();
    _blocked = false;
    _active = false;
    _followerCount = 0;
    emit activeChanged();
    emit followerCountChanged();
    _setStatusText(tr("已停止"));
}

void FormationController::_block(const QString& text)
{
    _finishTransit(false, text);
    _blocked = true;
    _guidanceTargets.clear();
    emit activeChanged();
    _setStatusText(text);
}

QList<Vehicle*> FormationController::_eligibleFollowers() const
{
    QList<Vehicle*> followers;
    QmlObjectListModel* vehicles = MultiVehicleManager::instance()->vehicles();
    for (int i = 0; i < vehicles->count(); i++) {
        Vehicle* vehicle = qobject_cast<Vehicle*>(vehicles->get(i));
        if (!vehicle || !_members.contains(vehicle->id()) || vehicle->id() == _leaderVehicleId || !vehicle->armed() ||
            (_vehicleAutomationAllowed && !_vehicleAutomationAllowed(vehicle->id())))
            continue;
        VehicleFactGroup* vfg = qobject_cast<VehicleFactGroup*>(vehicle->vehicleFactGroup());
        const double altRel = vfg ? vfg->altitudeRelative()->rawValue().toDouble() : 0.0;
        if (!std::isfinite(altRel) || altRel < _minAirborneAltMeters)
            continue;  // 未离地，等待下一个周期自动入队
        followers.append(vehicle);
    }
    std::sort(followers.begin(), followers.end(), [](const Vehicle* a, const Vehicle* b) { return a->id() < b->id(); });
    return followers;
}

bool FormationController::_leaderMoving(Vehicle* leader) const
{
    auto* facts = leader ? qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup()) : nullptr;
    const double speed = facts ? facts->groundSpeed()->rawValue().toDouble() : 0.0;
    return std::isfinite(speed) && speed > _leaderMovingSpeed;
}

double FormationController::_maxHorizontalSpeed(const QList<Vehicle*>& vehicles) const
{
    double limit = NAN;
    for (Vehicle* vehicle : vehicles) {
        FirmwarePlugin* plugin = vehicle ? vehicle->firmwarePlugin() : nullptr;
        const double speed = plugin ? plugin->maximumHorizontalSpeedMultirotorMetersSecond(vehicle) : NAN;
        if (std::isfinite(speed) && speed > 0)
            limit = std::isfinite(limit) ? std::min(limit, speed) : speed;
    }
    return limit;
}

void FormationController::_updateFormation()
{
    if (!_active)
        return;
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    if (!_members.contains(_leaderVehicleId) && !_handover()) {
        _block(tr("编队阻塞：没有合格继任长机"));
        return;
    }
    Vehicle* leader = mvm->getVehicleById(_leaderVehicleId);

    // 长机不满足引导条件时阻塞自动引导，由各机保持当前飞行模式。
    if (!leader || !leader->armed() || !leader->coordinate().isValid()) {
        _block(tr("长机 %1 状态异常，编队引导已阻塞").arg(_leaderVehicleId));
        return;
    }
    _blocked = false;

    const QGeoCoordinate leaderCoord = leader->coordinate();
    VehicleFactGroup* leaderFactGroup = qobject_cast<VehicleFactGroup*>(leader->vehicleFactGroup());
    const double leaderHeading = leaderFactGroup ? leaderFactGroup->heading()->rawValue().toDouble() : 0.0;
    const double leaderAltAMSL = leaderFactGroup ? leaderFactGroup->altitudeAMSL()->rawValue().toDouble() : 0.0;
    if (!leaderFactGroup || !std::isfinite(leaderHeading) || !std::isfinite(leaderAltAMSL)) {
        _block(tr("长机位置或高度无效，编队引导已阻塞"));
        return;
    }

    // 僚机：除长机外已解锁且已离地的成员，按 id 升序保证槽位稳定
    const QList<Vehicle*> followers = _eligibleFollowers();
    const QList<int> guidedIds = _guidanceTargets.keys();
    for (int id : guidedIds) {
        if (std::none_of(followers.cbegin(), followers.cend(),
                         [id](const Vehicle* follower) { return follower->id() == id; }))
            _guidanceTargets.remove(id);
    }
    if (_followerCount != followers.count()) {
        _followerCount = followers.count();
        emit followerCountChanged();
    }

    // 航段执行中由航段跟踪器指挥全队；航段期间新离地的成员待航段结束后再入队。
    if (transitActive()) {
        _updateTransit();
        emit activeChanged();
        return;
    }

    // 槽位自愈：成员在队却无槽位（长机交接瞬间坐标无效/离线、运行中刷新成员）时，
    // 静默跳过会让该机永久脱离编队引导，而僚机计数与状态文本仍报一切正常。
    const bool missingSlots = std::any_of(followers.cbegin(), followers.cend(),
                                          [this](Vehicle* follower) { return !_slots.contains(follower->id()); });
    if (missingSlots) {
        _rebuildSlots();
        _setStatusText(tr("检测到无槽位编队成员，已重建队形槽位"));
    }

    // 启动或队形参数变更后，全队同步收敛到新槽位；不可行时退回下面的逐机追赶。
    if (std::exchange(_reshapePending, false) && _beginReshape(leader, followers)) {
        emit activeChanged();
        return;
    }

    // 编队朝向：长机由其他方式驱动时跟随长机航向，否则保持上一航段结束时的朝向，
    // 避免长机悬停时的偏航带动整个队形甩动。
    if (_leaderMoving(leader))
        _formationHeading = leaderHeading;

    bool transitioning = false;
    const qint64 nowMs = SwarmClock::nowMs();
    for (Vehicle* follower : followers) {
        if (!_slots.contains(follower->id()))
            continue;
        const Slot slot = _slots.value(follower->id());

        // 目标点 = 长机位置沿 (编队朝向 + 方位偏移) 推移 slot.distance，高度 = 长机 AMSL + 高度分层
        QGeoCoordinate destination =
            leaderCoord.atDistanceAndAzimuth(slot.distance, _formationHeading + slot.bearing);
        destination.setAltitude(leaderAltAMSL + slot.altitude);
        QGeoCoordinate measured = follower->coordinate();
        measured.setAltitude(follower->altitudeAMSL()->rawValue().toDouble());
        if (!measured.isValid() || !std::isfinite(measured.altitude()) || !destination.isValid() ||
            !std::isfinite(destination.altitude())) {
            _guidanceTargets.remove(follower->id());
            _blocked = true;
            _setStatusText(tr("编队引导阻塞：%1 号机位置或高度无效").arg(follower->id()));
            continue;
        }
        // 上一条重定位指令仍在等待 ACK（链路慢，或该机正在执行其他引导指令）时本拍跳过：
        // 否则命令队列以重复指令拒发，每个周期都向操作员弹出错误。
        if (follower->isMavCommandPending(follower->defaultComponentId(), MAV_CMD_DO_REPOSITION))
            continue;
        if (!_guidanceTargets.contains(follower->id()))
            _guidanceTargets.insert(follower->id(), {measured, nowMs, QGeoCoordinate()});
        auto& guidance = _guidanceTargets[follower->id()];
        // Never catch up a delayed timer with a large command, or accumulate motion while blocked.
        const double dt = std::clamp(nowMs - guidance.updatedMs, qint64(0), qint64(_updateIntervalMs)) / 1000.0;
        guidance.updatedMs = nowMs;
        const QGeoCoordinate target = advanceGuidanceTarget(guidance.coordinate, guidance.destination, destination, dt);
        transitioning |=
            target.distanceTo(destination) > 0.1 || std::abs(target.altitude() - destination.altitude()) > 0.1;
        if (!target.isValid() || (_targetAllowed && !_targetAllowed(follower->id(), target))) {
            _blocked = true;
            _setStatusText(tr("编队引导阻塞：%1 号机目标约束未通过").arg(follower->id()));
            continue;
        }

        FirmwarePlugin* firmwarePlugin = follower->firmwarePlugin();
        if (firmwarePlugin) {
            guidance.coordinate = target;
            guidance.destination = destination;
            firmwarePlugin->guidedModeGotoLocationAndAltitude(follower, target);
        }
    }

    emit activeChanged();
    if (!_blocked) {
        _setStatusText((transitioning ? tr("编队调整中：从当前位置平滑过渡，长机 %1，僚机 %2 架")
                                      : tr("编队运行中：长机 %1，僚机 %2 架"))
                           .arg(_leaderVehicleId)
                           .arg(_followerCount));
    }
}

QGeoCoordinate FormationController::advanceGuidanceTarget(const QGeoCoordinate& current,
                                                          const QGeoCoordinate& previousDestination,
                                                          const QGeoCoordinate& destination, double dtSeconds)
{
    if (!current.isValid() || !destination.isValid() || !(dtSeconds > 0)) {
        return current;
    }
    QGeoCoordinate base = current;
    if (previousDestination.isValid()) {
        // 槽位目的地随长机移动：跟随这部分运动是编队飞行的本体，不限速。
        base = current.atDistanceAndAzimuth(previousDestination.distanceTo(destination),
                                            previousDestination.azimuthTo(destination));
        base.setAltitude(current.altitude() + (destination.altitude() - previousDestination.altitude()));
    }
    // 朝槽位的追赶保持限速：单拍指令不大幅跳变，时钟跳变不被放大。
    const double catchUp = std::min(base.distanceTo(destination), _horizontalTargetSpeed * dtSeconds);
    QGeoCoordinate target = base.atDistanceAndAzimuth(catchUp, base.azimuthTo(destination));
    target.setAltitude(base.altitude() + std::clamp(destination.altitude() - base.altitude(),
                                                    -_verticalTargetSpeed * dtSeconds,
                                                    _verticalTargetSpeed * dtSeconds));
    return target;
}

bool FormationController::gotoFormation(const QGeoCoordinate& coordinate, QString* reason)
{
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_leaderVehicleId);
    if (!coordinate.isValid() || !leader) {
        if (reason)
            *reason = tr("目标点或长机无效");
        return false;
    }
    const QGeoCoordinate target(coordinate.latitude(), coordinate.longitude(),
                                leader->altitudeAMSL()->rawValue().toDouble());
    return _planTransit({target}, QStringLiteral("goto"), reason);
}

bool FormationController::startRoute(const QList<QGeoCoordinate>& waypoints, QString* reason)
{
    if (waypoints.isEmpty()) {
        if (reason)
            *reason = tr("航线没有可执行的航点");
        return false;
    }
    return _planTransit(waypoints, QStringLiteral("route"), reason);
}

FormationLegPlanner::Plan FormationController::planRoute(const QList<QGeoCoordinate>& waypoints) const
{
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_leaderVehicleId);
    if (!_active || !leader || waypoints.isEmpty()) {
        FormationLegPlanner::Plan plan;
        plan.error = tr("编队未运行或航线为空");
        return plan;
    }
    return FormationLegPlanner::plan(_legRequest(leader, _eligibleFollowers(), waypoints, true));
}

FormationLegPlanner::Request FormationController::_legRequest(Vehicle* leader, const QList<Vehicle*>& followers,
                                                              const QList<QGeoCoordinate>& waypoints,
                                                              bool commandLeader) const
{
    FormationLegPlanner::Request request;
    request.leaderId = _leaderVehicleId;
    QList<Vehicle*> participants = followers;
    participants.prepend(leader);
    QSet<int> participantIds;
    for (Vehicle* vehicle : std::as_const(participants)) {
        QGeoCoordinate position = vehicle->coordinate();
        position.setAltitude(vehicle->altitudeAMSL()->rawValue().toDouble());
        request.members.append({vehicle->id(), position});
        participantIds.insert(vehicle->id());
    }
    // 槽位按编号压缩成连续下标；全部持有者写入 currentAssignment，未参与者的槽位标记为保留
    QList<int> holders = _slots.keys();
    std::sort(holders.begin(), holders.end(),
              [this](int a, int b) { return _slots.value(a).index < _slots.value(b).index; });
    for (int holder : std::as_const(holders)) {
        const Slot slot = _slots.value(holder);
        const int index = request.slotGeometries.size();
        request.slotGeometries.append({slot.distance, slot.bearing, slot.altitude});
        request.currentAssignment.insert(holder, index);
        if (!participantIds.contains(holder))
            request.reservedSlots.insert(index);
    }
    request.startReference = request.members.first().position;
    request.startHeadingDeg = _formationHeading;
    request.waypoints = waypoints;
    request.spacing = _spacingMeters;
    const double maxSpeed = _maxHorizontalSpeed(participants);
    request.speed = std::isfinite(maxSpeed) ? std::min(_transitSpeed, _transitSpeedFactor * maxSpeed) : _transitSpeed;
    request.minSeparation = _minSeparation ? _minSeparation() : 3.0;
    request.commandLeader = commandLeader;
    request.pathAllowed = _pathAllowed;
    return request;
}

bool FormationController::_planTransit(const QList<QGeoCoordinate>& waypoints, const QString& kind, QString* reason)
{
    const auto fail = [reason](const QString& text) {
        if (reason)
            *reason = text;
        return false;
    };
    if (!_active || _blocked)
        return fail(tr("编队未运行或已阻塞"));
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_leaderVehicleId);
    if (!leader || !leader->armed() || !leader->coordinate().isValid())
        return fail(tr("长机 %1 状态异常").arg(_leaderVehicleId));
    if (_vehicleAutomationAllowed && !_vehicleAutomationAllowed(_leaderVehicleId))
        return fail(tr("长机 %1 未获自动控制许可").arg(_leaderVehicleId));
    const QList<Vehicle*> followers = _eligibleFollowers();
    if (std::any_of(followers.cbegin(), followers.cend(),
                    [this](Vehicle* follower) { return !_slots.contains(follower->id()); }))
        _rebuildSlots();
    const FormationLegPlanner::Request request = _legRequest(leader, followers, waypoints, true);
    const FormationLegPlanner::Plan plan = FormationLegPlanner::plan(request);
    if (!plan.ok())
        return fail(plan.error);
    _finishTransit(false, tr("新的编队航段已替换当前航段"));
    _beginTransit(plan, request, kind);
    return true;
}

bool FormationController::_beginReshape(Vehicle* leader, const QList<Vehicle*>& followers)
{
    if (followers.isEmpty() || _leaderMoving(leader))
        return false;
    QGeoCoordinate reference = leader->coordinate();
    reference.setAltitude(leader->altitudeAMSL()->rawValue().toDouble());
    const FormationLegPlanner::Request request = _legRequest(leader, followers, {reference}, false);
    const FormationLegPlanner::Plan plan = FormationLegPlanner::plan(request);
    if (!plan.ok()) {
        SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("reshape_fallback"),
                                          {{QStringLiteral("reason"), plan.error}});
        return false;
    }
    _beginTransit(plan, request, QStringLiteral("reshape"));
    return true;
}

void FormationController::_applyAssignment(const FormationLegPlanner::Plan& plan,
                                           const FormationLegPlanner::Request& request)
{
    // 参与者取规划分配，未参与者保留原槽位；编号统一为请求里的压缩下标
    QHash<int, Slot> assigned;
    for (auto it = request.currentAssignment.cbegin(); it != request.currentAssignment.cend(); ++it) {
        const int index = plan.assignment.value(it.key(), it.value());
        const FormationLegPlanner::SlotGeometry& geometry = request.slotGeometries.at(index);
        assigned.insert(it.key(), {geometry.distance, geometry.bearing, geometry.altitude, index});
    }
    for (auto it = plan.assignment.cbegin(); it != plan.assignment.cend(); ++it) {
        if (assigned.contains(it.key()))
            continue;
        const FormationLegPlanner::SlotGeometry& geometry = request.slotGeometries.at(it.value());
        assigned.insert(it.key(), {geometry.distance, geometry.bearing, geometry.altitude, it.value()});
    }
    _slots = assigned;
}

void FormationController::_beginTransit(const FormationLegPlanner::Plan& plan,
                                        const FormationLegPlanner::Request& request, const QString& kind)
{
    _applyAssignment(plan, request);
    _legQueue = plan.legs;
    _legCount = plan.legs.size();
    _legIndex = 0;
    _transitDurationSec = plan.durationSec();
    _transitKind = kind;
    _reshapePending = false;
    _guidanceTargets.clear();
    QVariantMap slotNumbers;
    for (auto it = plan.assignment.cbegin(); it != plan.assignment.cend(); ++it)
        slotNumbers.insert(QString::number(it.key()), it.value() + 1);
    SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("leg_planned"),
                                      {{QStringLiteral("kind"), kind},
                                       {QStringLiteral("leg_count"), _legCount},
                                       {QStringLiteral("duration_s"), _transitDurationSec},
                                       {QStringLiteral("speed_mps"), request.speed},
                                       {QStringLiteral("slots"), slotNumbers}});
    if (kind == QStringLiteral("route"))
        SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("route_started"),
                                          {{QStringLiteral("waypoint_count"), request.waypoints.size()}});
    emit transitChanged();
    _startNextLeg();
}

void FormationController::_startNextLeg()
{
    FormationLegPlanner::Leg leg = _legQueue.first();
    leg.members.removeIf([this](const FormationLegPlanner::LegMember& member) { return !_members.contains(member.id); });
    ++_legIndex;
    _tracker.start(leg, SwarmClock::nowMs());
    for (const QMetaObject::Connection& connection : std::as_const(_resultConnections))
        disconnect(connection);
    _resultConnections.clear();
    _awaitingAck.clear();
    for (const FormationLegPlanner::LegMember& member : std::as_const(leg.members)) {
        Vehicle* vehicle = MultiVehicleManager::instance()->getVehicleById(member.id);
        if (!vehicle)
            continue;
        _resultConnections.append(connect(vehicle, &Vehicle::mavCommandResult, this,
                                          [this, id = member.id](int, int, int command, int ackResult, int) {
                                              // 只认本航段发出的指令的回执：上一航段或保持阶段的迟到回执不得中止本航段。
                                              // 待 ACK 期间不会再发新指令，所以集合里的 id 对应的回执一定属于本航段。
                                              if (command != MAV_CMD_DO_REPOSITION || ackResult == MAV_RESULT_IN_PROGRESS ||
                                                  !_awaitingAck.remove(id))
                                                  return;
                                              if (ackResult != MAV_RESULT_ACCEPTED)
                                                  _finishTransit(false, tr("%1 号机拒绝航段指令").arg(id));
                                          }));
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("leg_started"),
                                      {{QStringLiteral("kind"), _transitKind},
                                       {QStringLiteral("index"), _legIndex},
                                       {QStringLiteral("leg_count"), _legCount},
                                       {QStringLiteral("duration_s"), leg.durationSec}});
    emit transitChanged();
    _updateTransit();
}

void FormationController::_updateTransit()
{
    MultiVehicleManager* mvm = MultiVehicleManager::instance();
    QHash<int, QGeoCoordinate> positions;
    QSet<int> pending;
    QList<Vehicle*> vehicles;
    const QList<FormationLegPlanner::LegMember> members = _tracker.leg().members;
    for (const FormationLegPlanner::LegMember& member : members) {
        Vehicle* vehicle = mvm->getVehicleById(member.id);
        const bool usable = vehicle && vehicle->armed() &&
                            (!_vehicleAutomationAllowed || _vehicleAutomationAllowed(member.id));
        if (!usable) {
            if (member.id == _leaderVehicleId) {
                _finishTransit(false, tr("长机 %1 失去引导条件，航段已取消").arg(member.id));
                return;
            }
            _tracker.removeMember(member.id);
            continue;
        }
        QGeoCoordinate position = vehicle->coordinate();
        position.setAltitude(vehicle->altitudeAMSL()->rawValue().toDouble());
        positions.insert(member.id, position);
        if (vehicle->isMavCommandPending(vehicle->defaultComponentId(), MAV_CMD_DO_REPOSITION))
            pending.insert(member.id);
        vehicles.append(vehicle);
    }
    const qint64 nowMs = SwarmClock::nowMs();
    const QList<FormationLegTracker::Command> commands =
        _tracker.update(positions, pending, nowMs, _maxHorizontalSpeed(vehicles));
    switch (_tracker.state()) {
        case FormationLegTracker::State::Completed: {
            SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("leg_completed"),
                                              {{QStringLiteral("kind"), _transitKind},
                                               {QStringLiteral("index"), _legIndex},
                                               {QStringLiteral("elapsed_s"), _tracker.elapsedSec(nowMs)},
                                               {QStringLiteral("max_deviation_m"), _tracker.maxDeviationMeters()}});
            _formationHeading = _tracker.leg().headingDeg;
            _legQueue.removeFirst();
            if (_legQueue.isEmpty())
                _finishTransit(true, QString());
            else
                _startNextLeg();
            return;
        }
        case FormationLegTracker::State::TimedOut:
        case FormationLegTracker::State::Aborted:
            _finishTransit(false, _tracker.reason());
            return;
        default:
            break;
    }
    for (const FormationLegTracker::Command& command : commands) {
        Vehicle* vehicle = mvm->getVehicleById(command.id);
        FirmwarePlugin* plugin = vehicle ? vehicle->firmwarePlugin() : nullptr;
        if (plugin && plugin->guidedModeGotoLocationAtSpeed(vehicle, command.target, command.speed)) {
            _tracker.markSent(command.id, command.speed);
            _awaitingAck.insert(command.id);
        }
    }
    const double remaining = std::max(0.0, _tracker.leg().durationSec - _tracker.elapsedSec(nowMs));
    _setStatusText(tr("编队航段 %1/%2：同步飞行中，预计 %3 秒").arg(_legIndex).arg(_legCount).arg(qRound(remaining)));
}

void FormationController::_finishTransit(bool success, const QString& reason)
{
    if (!transitActive())
        return;
    for (const QMetaObject::Connection& connection : std::as_const(_resultConnections))
        disconnect(connection);
    _resultConnections.clear();
    _awaitingAck.clear();
    const QString kind = _transitKind;
    if (success) {
        if (kind == QStringLiteral("route"))
            SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("route_completed"),
                                              {{QStringLiteral("leg_count"), _legCount}});
    } else {
        SwarmAuditLogger::instance()->log(
            QStringLiteral("formation"), QStringLiteral("leg_aborted"),
            {{QStringLiteral("kind"), kind}, {QStringLiteral("index"), _legIndex}, {QStringLiteral("reason"), reason}});
        if (kind == QStringLiteral("route"))
            SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("route_aborted"),
                                              {{QStringLiteral("reason"), reason}});
    }
    _lastTransitResult = {{QStringLiteral("kind"), kind},
                          {QStringLiteral("success"), success},
                          {QStringLiteral("reason"), reason},
                          {QStringLiteral("leg_count"), _legCount}};
    _legQueue.clear();
    _tracker.reset();
    _legIndex = 0;
    _legCount = 0;
    _transitDurationSec = 0;
    _transitKind.clear();
    _guidanceTargets.clear();
    _setStatusText(success ? tr("编队已到位：长机 %1，僚机 %2 架").arg(_leaderVehicleId).arg(_followerCount) : reason);
    emit transitChanged();
    emit transitFinished(success, reason);
}

QVariantMap FormationController::transitStatus() const
{
    QVariantList members;
    const QHash<int, double> progress = _tracker.progress();
    const QHash<int, double> remaining = _tracker.distanceToEnd();
    for (const FormationLegPlanner::LegMember& member : _tracker.leg().members) {
        members.append(QVariantMap{{QStringLiteral("id"), member.id},
                                   {QStringLiteral("speed_mps"), member.speed},
                                   {QStringLiteral("start_lat"), member.start.latitude()},
                                   {QStringLiteral("start_lon"), member.start.longitude()},
                                   {QStringLiteral("start_alt_m"), member.start.altitude()},
                                   {QStringLiteral("end_lat"), member.end.latitude()},
                                   {QStringLiteral("end_lon"), member.end.longitude()},
                                   {QStringLiteral("end_alt_m"), member.end.altitude()},
                                   {QStringLiteral("progress"), progress.value(member.id)},
                                   {QStringLiteral("distance_to_end_m"), remaining.value(member.id, -1.0)}});
    }
    return {{QStringLiteral("active"), transitActive()},
            {QStringLiteral("kind"), _transitKind},
            {QStringLiteral("leg_index"), _legIndex},
            {QStringLiteral("leg_count"), _legCount},
            {QStringLiteral("elapsed_s"), _tracker.elapsedSec(SwarmClock::nowMs())},
            {QStringLiteral("duration_s"), _tracker.leg().durationSec},
            {QStringLiteral("total_duration_s"), _transitDurationSec},
            {QStringLiteral("consensus_progress"), _tracker.consensusProgress()},
            {QStringLiteral("deviation_m"), _tracker.deviationMeters()},
            {QStringLiteral("max_deviation_m"), _tracker.maxDeviationMeters()},
            {QStringLiteral("members"), members},
            {QStringLiteral("last_result"), _lastTransitResult}};
}

QHash<int, int> FormationController::slotAssignment() const
{
    QHash<int, int> assignment;
    for (auto it = _slots.cbegin(); it != _slots.cend(); ++it)
        assignment.insert(it.key(), it.value().index + 1);
    return assignment;
}

void FormationController::_formationOffset(int followerIndex, int totalFollowers, double& distanceMeters,
                                           double& azimuthOffsetDeg, double& altitudeStepMeters) const
{
    const int rank = (followerIndex + 1) / 2;             // 排数（1 起）
    const int side = (followerIndex % 2 == 1) ? -1 : +1;  // 奇数槽位在左，偶数在右

    switch (_formationType) {
        case FormationWedge:  // 楔形：长机后方 ±30° 逐排展开，逐排升高
            distanceMeters = _spacingMeters * rank;
            azimuthOffsetDeg = 180.0 + side * 30.0;
            altitudeStepMeters = _verticalStepMeters * rank;
            break;
        case FormationColumn:  // 纵队：长机正后方依次排开，逐机升高
            distanceMeters = _spacingMeters * followerIndex;
            azimuthOffsetDeg = 180.0;
            altitudeStepMeters = _verticalStepMeters * followerIndex;
            break;
        case FormationLine:  // 横队：长机左右两翼展开，同高度
            distanceMeters = _spacingMeters * rank;
            azimuthOffsetDeg = side * 90.0;
            altitudeStepMeters = 0.0;
            break;
        case FormationCircle:  // 环形：僚机均布在长机周围，同高度
            distanceMeters = _spacingMeters;
            azimuthOffsetDeg = (totalFollowers > 0) ? (360.0 * (followerIndex - 1) / totalFollowers) : 0.0;
            altitudeStepMeters = 0.0;
            break;
    }
}

void FormationController::_setStatusText(const QString& text)
{
    if (_statusText != text) {
        _statusText = text;
        emit statusTextChanged();
    }
}
```

- [ ] **Step 5: 编译并跑测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh --timeout 400 -R "'^SwarmRegressionTest$|^SwarmFormationTest$|^FormationLegPlannerTest$'" 2>&1 | tail -5
```
Expected: 三类全部通过。`SwarmFormationTest::operatorGotoMovesWholeFormation` 此时仍是 9/30 旧断言，**允许它在本任务失败**（Task 7 重写）；其他用例必须通过。若 `formationStartConvergesInSync` 的僚机首条指令速度为 0，说明规划走了垂直分层：打印该机起点与指令目标，核对 MockLink 的出生位置是否仍在长机东北方（`MockLink.cc` 约 72 行）。

---

## 审查后追加的要求（必须一并实现）

以下来自 Task 3/5 的代码审查，优先级与上面的步骤相同。

**A. 失效保护中的成员不得再收到编队指令**（PX4 每条带 CHANGE_MODE 的 DO_REPOSITION 都是一次模式切换请求；
在失效保护的悬停延迟窗口 COM_FAIL_ACT_T 内收到，会被当作用户接管而取消返航/降落。PX4 失效保护时 HEARTBEAT
`system_status = MAV_STATE_CRITICAL`。）

1. `src/Vehicle/Vehicle.h/.cc`（该文件没有他人的未提交改动，但只做最小改动）：
   - 加 `uint8_t _systemStatus = MAV_STATE_UNINIT;` 成员（放在 `_base_mode`/`_custom_mode` 成员附近），
     公开只读访问器 `uint8_t systemStatus() const { return _systemStatus; }`，注释：最近一次 HEARTBEAT 的 system_status（MAV_STATE）。
   - `Vehicle::_handleHeartbeat` 在 `mavlink_msg_heartbeat_decode` 之后加 `_systemStatus = heartbeat.system_status;`。
2. `FormationController`：
   - 加私有静态辅助 `static bool _inFailsafe(const Vehicle* vehicle)`：`vehicle->systemStatus() >= MAV_STATE_CRITICAL`
     （CRITICAL、EMERGENCY、POWEROFF、FLIGHT_TERMINATION），注释写明上述 PX4 原因。
   - `_eligibleFollowers()` 排除 `_inFailsafe` 的僚机（于是保持阶段也不再给它发指令）。
   - `_updateFormation()` 的长机检查：长机 `_inFailsafe` 时 `_block(tr("长机 %1 处于失效保护，编队引导已阻塞").arg(_leaderVehicleId))` 并 return。
   - `_planTransit()`：长机 `_inFailsafe` 时以 `tr("长机 %1 处于失效保护")` 失败。
   - `_updateTransit()` 的 `usable` 判定加上 `!_inFailsafe(vehicle)`（僚机移出航段、长机终止航段，与现有逻辑一致）。
3. 测试（`SwarmFormationTest`，新 slot `formationSkipsMemberInFailsafe`）：2 架僚机起飞、长机 `setLeaderGroundSpeed(vehicle(), 2.0)`
   让启动走保持阶段、`setTargetAllowedCallback` 记录每拍受令的僚机 id；
   给其中一架僚机注入一条 `system_status = MAV_STATE_CRITICAL` 的 HEARTBEAT（用该僚机 MockLink 的
   `respondWithMavlinkMessage`，base_mode/custom_mode 取该机当前值，可用 `mavlink_msg_heartbeat_pack_chan` 与该 link 的
   `outgoingMavlinkChannel()`），`QTRY_COMPARE_WITH_TIMEOUT(follower->systemStatus(), MAV_STATE_CRITICAL, …)` 后**立即**
   `QMetaObject::invokeMethod(&formation, "_updateFormation", Qt::DirectConnection)`，断言这一拍该僚机没有受令、另一架受令。
   （MockLink 自己的心跳会在约 1 s 后把状态改回，所以断言必须紧接在 QTRY 之后、中间不进事件循环。）

**B. 回执归属**（Task 3 审查）：已写入上面 Step 4 的 `_awaitingAck` 方案。另外，`failureCode == MavCmdResultFailureDuplicateCommand`
的结果不是飞控的拒绝（是本地命令队列拒发重复指令），lambda 里遇到它直接 return，不中止航段。

**C. 速度不得为 0 以外的非正数**：`FormationLegTracker` 已保证下限 0.5·base，无需改动；只确认 `guidedModeGotoLocationAtSpeed`
收到的 0 只来自"可忽略航程"成员。


---

### Task 7: SwarmManager 入口

**Files:**
- Modify: `src/Swarm/SwarmManager.h`, `src/Swarm/SwarmManager.cc`
- Modify: `test/Swarm/SwarmFormationTest.h`, `test/Swarm/SwarmFormationTest.cc`

- [ ] **Step 1: 改写 goto 测试、新增航线测试**

`SwarmFormationTest.h` 追加 slot：`void formationRouteFromLeaderMission();`

`SwarmFormationTest.cc`：把整个 `operatorGotoMovesWholeFormation()`（Task 4.5 迁入的 9/30 版本）替换为：
```cpp
void SwarmFormationTest::operatorGotoMovesWholeFormation()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    auto* mvm = MultiVehicleManager::instance();
    QHash<int, MockLink*> links{{vehicle()->id(), mockLink()}};
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        links.insert(link->vehicleId(), link);
    for (MockLink* link : std::as_const(links))
        link->setRepositionSupported(true);
    // MockLink places later vehicles north-east of the leader. A formation facing south-west keeps every
    // synchronized path clear of the others, so each goto below is planned as a single leg.
    vehicle()->heading()->setRawValue(214);
    QVERIFY(_manager->startFormation(vehicle()->id()));
    QSignalSpy feedback(_manager.get(), &SwarmManager::swarmCommandFeedback);

    // A goto on any member moves the whole formation: every member, the leader included, is commanded in
    // the same cycle with a ground speed that makes all of them arrive together.
    const auto verifySynchronizedGoto = [&](Vehicle* target, const QGeoCoordinate& goal) {
        QTRY_VERIFY_WITH_TIMEOUT(std::none_of(links.cbegin(), links.cend(),
                                              [mvm](MockLink* link) {
                                                  Vehicle* member = mvm->getVehicleById(link->vehicleId());
                                                  return member && member->isMavCommandPending(
                                                                       member->defaultComponentId(), MAV_CMD_DO_REPOSITION);
                                              }),
                                 TestTimeout::longMs());
        QHash<int, QGeoCoordinate> starts;
        for (MockLink* link : std::as_const(links)) {
            link->clearReceivedMavlinkMessageCounts();
            starts.insert(link->vehicleId(), mvm->getVehicleById(link->vehicleId())->coordinate());
        }
        feedback.clear();
        QVERIFY(_manager->gotoLocation(target, goal));
        QVERIFY(_manager->formation()->transitActive());
        QCOMPARE(_manager->formation()->transitLegCount(), 1);
        QVERIFY(std::any_of(feedback.cbegin(), feedback.cend(), [](const QList<QVariant>& arguments) {
            return arguments.first().toString().contains(QStringLiteral("编队整体飞往目标点"));
        }));
        QTRY_VERIFY_WITH_TIMEOUT(std::all_of(links.cbegin(), links.cend(),
                                             [](MockLink* link) { return lastFormationReposition(link).valid(); }),
                                 TestTimeout::longMs());
        QVERIFY(lastFormationReposition(mockLink()).target.distanceTo(goal) < 1.0);
        QList<double> arrivalSeconds;
        for (MockLink* link : std::as_const(links)) {
            const Reposition command = lastFormationReposition(link);
            QVERIFY(command.speed > 0);
            arrivalSeconds.append(starts.value(link->vehicleId()).distanceTo(command.target) / command.speed);
        }
        const auto [earliest, latest] = std::minmax_element(arrivalSeconds.cbegin(), arrivalSeconds.cend());
        QVERIFY2(*latest - *earliest < 0.5, qPrintable(QStringLiteral("arrival spread %1 s").arg(*latest - *earliest)));
    };
    verifySynchronizedGoto(followers.last(), vehicle()->coordinate().atDistanceAndAzimuth(150, 214));
    if (QTest::currentTestFailed())
        return;
    // A second goto, issued on the leader, replaces the leg in progress.
    verifySynchronizedGoto(vehicle(), vehicle()->coordinate().atDistanceAndAzimuth(100, 214));
    if (QTest::currentTestFailed())
        return;

    // Without an active formation the goto applies to the selected vehicle only. MockLink does not
    // advertise COMMAND_INT, so this native goto arrives as COMMAND_LONG.
    Vehicle* follower = followers.last();
    MockLink* followerLink = links.value(follower->id());
    _manager->stopFormation();
    QVERIFY(!_manager->formation()->transitActive());
    QTRY_VERIFY_WITH_TIMEOUT(!follower->isMavCommandPending(follower->defaultComponentId(), MAV_CMD_DO_REPOSITION),
                             TestTimeout::longMs());
    const auto nativeGotoTarget = [](MockLink* link) {
        mavlink_message_t message{};
        if (!link->lastReceivedMavlinkMessage(MAVLINK_MSG_ID_COMMAND_LONG, message))
            return QGeoCoordinate();
        mavlink_command_long_t command{};
        mavlink_msg_command_long_decode(&message, &command);
        return command.command == MAV_CMD_DO_REPOSITION ? QGeoCoordinate(command.param5, command.param6)
                                                        : QGeoCoordinate();
    };
    const QGeoCoordinate soloGoal = follower->coordinate().atDistanceAndAzimuth(80, 180);
    QVERIFY(_manager->gotoLocation(follower, soloGoal));
    QTRY_VERIFY_WITH_TIMEOUT(
        nativeGotoTarget(followerLink).isValid() && nativeGotoTarget(followerLink).distanceTo(soloGoal) < 1.0,
        TestTimeout::longMs());
}
```
在其后加：
```cpp
void SwarmFormationTest::formationRouteFromLeaderMission()
{
    // The headless test host has no physical pressure sensor backend.
    ignoreLogMessage("Utilities.QGCSensors", QtWarningMsg,
                     QRegularExpression("Failed to connect to pressure backend|Error Initializing Pressure Sensor"));
    addVehicles(2);
    QList<Vehicle*> followers;
    takeOffAll(followers);
    if (QTest::currentTestFailed())
        return;
    mockLink()->setRepositionSupported(true);
    for (const QPointer<MockLink>& link : std::as_const(_extraLinks))
        link->setRepositionSupported(true);
    QTRY_VERIFY_WITH_TIMEOUT(vehicle()->homePosition().isValid(), TestTimeout::longMs());
    const QGeoCoordinate home = vehicle()->homePosition();
    const auto uploadRoute = [this, &home](const QList<std::pair<MAV_CMD, QGeoCoordinate>>& route) {
        // Index 0 is the Home placeholder that PlanManager strips for PX4.
        QList<MissionItem*> items{new MissionItem(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 0, 0, 0, 0,
                                                  home.latitude(), home.longitude(), home.altitude(), true, false)};
        for (const auto& [command, point] : route) {
            items.append(new MissionItem(items.size(), command, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0, NAN,
                                         point.latitude(), point.longitude(), point.altitude(), true, false));
        }
        auto* mission = vehicle()->missionManager();
        QSignalSpy sent(mission, &MissionManager::sendComplete);
        mission->writeMissionItems(items);
        QTRY_VERIFY_WITH_TIMEOUT(!sent.isEmpty(), TestTimeout::longMs());
        QVERIFY(!sent.first().first().toBool());
        QSignalSpy read(mission, &MissionManager::newMissionItemsAvailable);
        mission->loadFromVehicle();
        QTRY_VERIFY_WITH_TIMEOUT(!read.isEmpty(), TestTimeout::longMs());
    };
    // Relative altitudes: 120 m south-west, then 120 m north-west, then a landing the formation leaves alone.
    QGeoCoordinate first = vehicle()->coordinate().atDistanceAndAzimuth(120, 214);
    first.setAltitude(25);
    QGeoCoordinate second = first.atDistanceAndAzimuth(120, 304);
    second.setAltitude(30);
    QGeoCoordinate landing = second;
    landing.setAltitude(0);
    uploadRoute({{MAV_CMD_NAV_WAYPOINT, first}, {MAV_CMD_NAV_WAYPOINT, second}, {MAV_CMD_NAV_LAND, landing}});
    if (QTest::currentTestFailed())
        return;
    vehicle()->heading()->setRawValue(214);
    QVERIFY(_manager->startFormation(vehicle()->id()));

    const QString summary = _manager->formationRouteSummary();
    QVERIFY2(summary.contains(QStringLiteral("2 个航点")) && summary.contains(QStringLiteral("末尾降落")),
             qPrintable(summary));
    QVERIFY(_manager->startFormationRoute());
    QCOMPARE(_manager->formation()->transitStatus().value(QStringLiteral("kind")).toString(), QStringLiteral("route"));
    QVERIFY(_manager->formation()->transitLegCount() >= 2);

    // Any item other than a waypoint (apart from a leading takeoff and a trailing landing) rejects the route.
    _manager->stopFormation();
    uploadRoute({{MAV_CMD_NAV_WAYPOINT, first}, {MAV_CMD_DO_CHANGE_SPEED, first}, {MAV_CMD_NAV_WAYPOINT, second}});
    if (QTest::currentTestFailed())
        return;
    QVERIFY(_manager->startFormation(vehicle()->id()));
    QSignalSpy feedback(_manager.get(), &SwarmManager::swarmCommandFeedback);
    QVERIFY(!_manager->startFormationRoute());
    QVERIFY(_manager->formation()->transitStatus().value(QStringLiteral("kind")).toString() != QStringLiteral("route"));
    QVERIFY(!feedback.isEmpty());
    QVERIFY2(feedback.last().first().toString().contains(QStringLiteral("不支持")),
             qPrintable(feedback.last().first().toString()));
}
```

- [ ] **Step 2: 编译确认失败**

Run: `/tmp/qgc_build.sh 2>&1 | grep -E "error:" | head -3`
Expected: `no member named 'formationRouteSummary' / 'startFormationRoute' in 'SwarmManager'`。

- [ ] **Step 3: 修改 `SwarmManager.h`**

把
```cpp
    /// 地图「飞往位置」。编队运行中对僚机下发时移动整个编队：改由长机飞往该点，僚机保持队形跟随。
    Q_INVOKABLE bool gotoLocation(Vehicle* vehicle, const QGeoCoordinate& coordinate,
                                  double forwardFlightLoiterRadius = 0.0);
```
替换为
```cpp
    /// 地图「飞往位置」。编队运行中对任一编队成员下发时移动整个编队：全队同步航段，长机终点为该点。
    Q_INVOKABLE bool gotoLocation(Vehicle* vehicle, const QGeoCoordinate& coordinate,
                                  double forwardFlightLoiterRadius = 0.0);

    /// 「编队按航线飞」确认文本：长机已上传航线的航点数、总长与预计时长；无法执行时返回原因。
    Q_INVOKABLE QString formationRouteSummary() const;
    /// 编队按长机已上传航线逐点飞行，每个航点全队对齐后再飞下一段。
    Q_INVOKABLE bool startFormationRoute();
```
并在 `bool _formationAutomationAllowed(int vehicleId);` 之后加：
```cpp
    /// 读取长机已上传航线中编队可执行的航点（AMSL 高度）
    bool _leaderRouteWaypoints(QList<QGeoCoordinate>& waypoints, bool& trailingRecovery, QString& reason) const;
```

- [ ] **Step 4: 修改 `SwarmManager.cc`**

1. 构造函数：在 `_formation->setTargetAllowedCallback(...)` 整个调用之后加：
```cpp
    _formation->setPathAllowedCallback([this](int id, const QGeoCoordinate& from, const QGeoCoordinate& to) {
        Vehicle* vehicle = MultiVehicleManager::instance()->getVehicleById(id);
        const auto config = _constraintConfig();
        if (!vehicle || !config.error.isEmpty() || !std::isfinite(from.altitude()) || !std::isfinite(to.altitude()))
            return false;
        // 高度下限对每架机生效：终点不得低于各自 Home 0.5 m 以上（与航线核验 validate() 一致）
        const QGeoCoordinate home = vehicle->homePosition();
        if (!home.isValid() || !std::isfinite(home.altitude()) || to.altitude() < home.altitude() - 0.5)
            return false;
        if (config.maxAltitudeM > 0 &&
            std::max(from.altitude(), to.altitude()) - home.altitude() > config.maxAltitudeM)
            return false;
        return SwarmConstraintChecker::segmentAllowed({from.latitude(), from.longitude()},
                                                      {to.latitude(), to.longitude()}, config);
    });
    _formation->setMinSeparationCallback([this] { return _constraintConfig().minSeparationM; });
    // 队列连接：反馈处理不得在 FormationController 状态更新途中重入它（例如在 stop() 内再下发 goto）。
    (void) connect(
        _formation, &FormationController::transitFinished, this,
        [this](bool success, const QString& reason) {
            emit swarmCommandFeedback(success ? tr("编队已到位") : tr("编队航段结束：%1").arg(reason));
        },
        Qt::QueuedConnection);
```
2. 把整个 `SwarmManager::gotoLocation(...)` 替换为：
```cpp
bool SwarmManager::gotoLocation(Vehicle* vehicle, const QGeoCoordinate& coordinate, double forwardFlightLoiterRadius)
{
    if (!vehicle) {
        return false;
    }
    if (!_formation->active() || !_formationTargetIds.contains(vehicle->id())) {
        return vehicle->guidedModeGotoLocation(coordinate, forwardFlightLoiterRadius);
    }
    // 编队运行中「飞往位置」移动整个编队：全队（含长机）同步航段，长机终点为目标点。
    QString reason;
    if (!_formation->gotoFormation(coordinate, &reason)) {
        emit swarmCommandFeedback(tr("编队飞往目标点被拒绝：%1").arg(reason));
        return false;
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("operator_goto_formation_leg"),
                                      {{QStringLiteral("vehicle_id"), vehicle->id()},
                                       {QStringLiteral("leader_id"), _formation->leaderVehicleId()},
                                       {QStringLiteral("lat"), coordinate.latitude()},
                                       {QStringLiteral("lon"), coordinate.longitude()}});
    emit swarmCommandFeedback(
        tr("编队整体飞往目标点：%1 架同步，预计 %2 秒")
            .arg(_formation->followerCount() + 1)
            .arg(qRound(_formation->transitStatus().value(QStringLiteral("total_duration_s")).toDouble())));
    return true;
}

bool SwarmManager::_leaderRouteWaypoints(QList<QGeoCoordinate>& waypoints, bool& trailingRecovery,
                                         QString& reason) const
{
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_formation->leaderVehicleId());
    if (!_formation->active() || !leader) {
        reason = tr("编队未运行");
        return false;
    }
    const QGeoCoordinate home = leader->homePosition();
    if (!home.isValid() || !std::isfinite(home.altitude())) {
        reason = tr("长机 Home 无效，无法换算航点高度");
        return false;
    }
    MissionManager* mission = leader->missionManager();
    if (!mission || mission->inProgress()) {
        reason = tr("长机航线正在读写，请稍后再试");
        return false;
    }
    const auto snapshot =
        SwarmMissionSnapshot::capture(mission->missionItems(), leader->firmwarePlugin()->sendHomePositionToVehicle());
    if (snapshot.empty()) {
        reason = tr("长机 %1 没有已上传的航线").arg(leader->id());
        return false;
    }
    waypoints = snapshot.formationWaypoints(home.altitude(), trailingRecovery, reason);
    return !waypoints.isEmpty();
}

QString SwarmManager::formationRouteSummary() const
{
    QList<QGeoCoordinate> waypoints;
    bool trailingRecovery = false;
    QString reason;
    if (!_leaderRouteWaypoints(waypoints, trailingRecovery, reason))
        return tr("无法按航线飞行：%1").arg(reason);
    const FormationLegPlanner::Plan plan = _formation->planRoute(waypoints);
    if (!plan.ok())
        return tr("无法按航线飞行：%1").arg(plan.error);
    Vehicle* leader = MultiVehicleManager::instance()->getVehicleById(_formation->leaderVehicleId());
    double length = 0;
    QGeoCoordinate previous = leader ? leader->coordinate() : waypoints.first();
    for (const QGeoCoordinate& waypoint : std::as_const(waypoints)) {
        length += previous.distanceTo(waypoint);
        previous = waypoint;
    }
    const int seconds = qRound(plan.durationSec());
    QString text = tr("长机 %1 的航线：%2 个航点，总长 %3 米，预计 %4 分 %5 秒")
                       .arg(_formation->leaderVehicleId())
                       .arg(waypoints.size())
                       .arg(qRound(length))
                       .arg(seconds / 60)
                       .arg(seconds % 60);
    if (trailingRecovery)
        text += tr("。末尾降落/返航需另行操作");
    return text;
}

bool SwarmManager::startFormationRoute()
{
    QList<QGeoCoordinate> waypoints;
    bool trailingRecovery = false;
    QString reason;
    if (!_leaderRouteWaypoints(waypoints, trailingRecovery, reason) || !_formation->startRoute(waypoints, &reason)) {
        emit swarmCommandFeedback(tr("编队按航线飞被拒绝：%1").arg(reason));
        return false;
    }
    SwarmAuditLogger::instance()->log(QStringLiteral("formation"), QStringLiteral("operator_formation_route"),
                                      {{QStringLiteral("leader_id"), _formation->leaderVehicleId()},
                                       {QStringLiteral("waypoint_count"), waypoints.size()}});
    emit swarmCommandFeedback(
        tr("编队按航线飞：%1 个航点，共 %2 段").arg(waypoints.size()).arg(_formation->transitLegCount()));
    return true;
}
```

- [ ] **Step 5: 编译并跑测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh --timeout 400 -R "'^SwarmRegressionTest$|^SwarmFormationTest$'" 2>&1 | tail -5
```
Expected: 全部通过（含改写后的 `operatorGotoMovesWholeFormation` 与新 `formationRouteFromLeaderMission`）。

---

### Task 8: 面板按钮与验收接口

**Files:**
- Modify: `src/FlyView/SwarmControlPanel.qml`
- Modify: `src/Swarm/SwarmAcceptanceServer.cc`

- [ ] **Step 1: 面板按钮**

`SwarmControlPanel.qml`：在「停止编队」按钮
```qml
                QGCButton {
                    text:    qsTr("停止编队")
                    enabled: _swarm.formation.active
                    onClicked: _swarm.stopFormation()
                }
```
之后加：
```qml
                QGCButton {
                    text:    qsTr("编队按航线飞")
                    // 不绑 blocked：单个僚机的保持目标无效会置 blocked，但不妨碍航线（规划器重新校验）
                    enabled: _swarm.formation.active
                    onClicked: {
                        // 只在打开确认框时生成摘要：摘要同时记下待确认的长机与航线指纹
                        formationRouteConfirm.text = _swarm.formationRouteSummary()
                        formationRouteConfirm.buttons = _swarm.formationRouteConfirmable()
                            ? (MessageDialog.Yes | MessageDialog.No) : MessageDialog.Ok
                        formationRouteConfirm.open()
                    }
                }
```
并在 `emergencyConfirm` 这个 `MessageDialog` 之后加：
```qml
    MessageDialog {
        id: formationRouteConfirm
        title: qsTr("编队按航线飞")
        buttons: MessageDialog.Yes | MessageDialog.No
        // 航线不可执行时只有「确定」按钮，onAccepted 不会触发执行（Ok 也会触发 accepted，故再判一次）
        onAccepted: if (_swarm.formationRouteConfirmable()) _swarm.startFormationRoute()
    }
```

- [ ] **Step 2: 验收接口**

`SwarmAcceptanceServer.cc`：
1. include 区加 `#include "MissionItem.h"` 与 `#include "MissionManager.h"`（按字母序放入现有 include 列表）。
2. 在 `if (command == QStringLiteral("goto")) { ... }` 块之后加：
```cpp
    if (command == QStringLiteral("formation_route")) {
        // Same entry point as the swarm panel "编队按航线飞" button: confirmation summary, then start.
        const QString summary = _manager->formationRouteSummary();
        const bool accepted = _manager->startFormationRoute();
        return {{QStringLiteral("ok"), accepted},
                {QStringLiteral("summary"), summary},
                {QStringLiteral("message"), _lastFeedback},
                {QStringLiteral("status"), _status()}};
    }
    if (command == QStringLiteral("upload_route")) {
        // SITL preparation for the formation route scenario: writes a waypoint-only route.
        Vehicle* vehicle =
            MultiVehicleManager::instance()->getVehicleById(request.value(QStringLiteral("vehicle_id")).toInt());
        if (!vehicle || !vehicle->missionManager() || !vehicle->homePosition().isValid()) {
            return errorResponse(QStringLiteral("vehicle_not_ready"));
        }
        const QJsonArray waypoints = request.value(QStringLiteral("waypoints")).toArray();
        if (waypoints.isEmpty()) {
            return errorResponse(QStringLiteral("empty_route"));
        }
        const QGeoCoordinate home = vehicle->homePosition();
        // Index 0 is the Home placeholder; PlanManager strips it for firmwares that keep Home on board.
        QList<MissionItem*> items{new MissionItem(0, MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL, 0, 0, 0, 0,
                                                  home.latitude(), home.longitude(), home.altitude(), true, false)};
        for (const QJsonValue& value : waypoints) {
            const QJsonObject point = value.toObject();
            items.append(new MissionItem(items.size(), MAV_CMD_NAV_WAYPOINT, MAV_FRAME_GLOBAL_RELATIVE_ALT, 0, 0, 0,
                                         NAN, point.value(QStringLiteral("lat")).toDouble(),
                                         point.value(QStringLiteral("lon")).toDouble(),
                                         point.value(QStringLiteral("alt_rel_m")).toDouble(), true, false));
        }
        vehicle->missionManager()->writeMissionItems(items);
        return {{QStringLiteral("ok"), true}};
    }
```
3. `_status()` 里在 `FormationController* formation = _manager->formation();` 之后加：
```cpp
    QJsonObject formationSlots;
    const QHash<int, int> slotAssignment = formation->slotAssignment();
    for (auto it = slotAssignment.cbegin(); it != slotAssignment.cend(); ++it)
        formationSlots.insert(QString::number(it.key()), it.value());
```
并把 `formation` 状态对象改为：
```cpp
        {QStringLiteral("formation"),
         QJsonObject{{QStringLiteral("active"), formation->active()},
                     {QStringLiteral("leader_id"), formation->leaderVehicleId()},
                     {QStringLiteral("follower_count"), formation->followerCount()},
                     {QStringLiteral("type"), formation->formationType()},
                     {QStringLiteral("spacing_m"), formation->spacingMeters()},
                     {QStringLiteral("vertical_step_m"), formation->verticalStepMeters()},
                     {QStringLiteral("heading_deg"), formation->formationHeading()},
                     {QStringLiteral("slots"), formationSlots},
                     {QStringLiteral("transit"), QJsonObject::fromVariantMap(formation->transitStatus())}}},
```

- [ ] **Step 3: 编译、qmllint、跑相关测试**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
QGC=/media/hello/data1/Jinqiang/QGC/qgroundcontrol
docker run --rm -u 1000:1000 --entrypoint bash -v $QGC:/project/source:ro qgc-ubuntu-docker:latest \
  -lc 'cd /project/source && /opt/Qt/6.11.1/gcc_64/bin/qmllint src/FlyView/SwarmControlPanel.qml' 2>&1 | grep -n -i -E "formationRoute|编队按航线" || echo "no qmllint findings on new lines"
/tmp/qgc_ctest.sh -R "'Swarm|Formation|FlyView|Guided'" 2>&1 | tail -6
```
Expected: 编译成功；新加的 QML 行没有 qmllint 告警（文件原有告警不在本任务范围）；ctest 全部通过。

**追加（Task 7 审查后）**：验收接口需要把拒绝原因带回脚本。在 `SwarmAcceptanceServer` 中保存最近一条
`SwarmManager::swarmCommandFeedback`（成员 `QString _lastFeedback`，在接入 `_manager` 的地方
`connect(_manager, &SwarmManager::swarmCommandFeedback, this, [this](const QString& m) { _lastFeedback = m; })`），
`goto` 与 `formation_route` 的响应都加 `{"message", _lastFeedback}`（goto 响应在调用前先清空 `_lastFeedback`）。
注意 `transitFinished` 的反馈是队列连接，响应里看到的是同步发出的 goto/航线反馈。

**追加（Task 7 审查后，先做）**：
1. `FormationController::guides()`：长机分支改为 `if (vehicleId == _leaderVehicleId) return _members.contains(vehicleId);`
   （长机离队且无合格继任时，原长机已不受编队引导，goto 应走原生路径）。
2. `SwarmManager::startFormationRoute()`：没有待确认记录时用单独的原因 `tr("请先查看并确认航线")`，
   只有记录存在但长机/指纹/Home 不符时才用「长机或航线已变化，请重新确认」。
3. 新增 `Q_INVOKABLE bool formationRouteConfirmable() const`：存在待确认记录（最近一次摘要可执行且尚未被
   start 消费）时为 true；面板确认框据此决定是否提供「确认」按钮（见上方 QML）。
4. `test/Swarm/SwarmFormationTest.cc` 匿名命名空间：`nativeGotoTarget` 被插在 `everyHomeKnown` 的注释下面，
   把它移到该注释之上，让两个函数各带自己的注释。

---

### Task 9: SITL 指标与驱动脚本

**Files:**
- Modify: `swarm_ext/sim/qgc_acceptance.py`, `swarm_ext/sim/test_qgc_acceptance.py`, `swarm_ext/sim/README.md`
- Create: `swarm_ext/sim/formation_sync_check.py`

- [ ] **Step 1: 写失败的 Python 测试**

`test_qgc_acceptance.py`：import 列表加 `formation_sync_metrics`；在 `test_wedge_formation_geometry` 之后加：
```python
    def test_wedge_formation_geometry_uses_reported_slots_and_heading(self):
        # Vehicle 3 holds slot 1 (left) and vehicle 2 slot 2 (right); the formation faces east while the
        # leader's own heading is irrelevant.
        left = destination(39.9, 116.4, 90.0 + 150.0, 15.0)
        right = destination(39.9, 116.4, 90.0 + 210.0, 15.0)
        status = {
            "formation": {
                "active": True,
                "leader_id": 1,
                "spacing_m": 15.0,
                "vertical_step_m": 3.0,
                "heading_deg": 90.0,
                "slots": {"2": 2, "3": 1},
            },
            "vehicles": [
                {"id": 1, "lat": 39.9, "lon": 116.4, "heading_deg": 10.0, "alt_amsl_m": 100.0},
                {"id": 2, "lat": right[0], "lon": right[1], "alt_amsl_m": 103.0},
                {"id": 3, "lat": left[0], "lon": left[1], "alt_amsl_m": 103.0},
            ],
        }
        metrics = formation_geometry_metrics(status)
        self.assertTrue(metrics["pass"])
        self.assertLess(metrics["max_horizontal_error_m"], 0.1)

    def test_formation_sync_metrics(self):
        a_start = (39.9, 116.4)
        b_start = destination(*a_start, 0.0, 20.0)
        a_end = destination(*a_start, 90.0, 100.0)
        b_end = destination(*b_start, 90.0, 100.0)
        members = [
            {"id": 2, "speed_mps": 5.0, "start_lat": a_start[0], "start_lon": a_start[1],
             "end_lat": a_end[0], "end_lon": a_end[1], "end_alt_m": 100.0},
            {"id": 3, "speed_mps": 5.0, "start_lat": b_start[0], "start_lon": b_start[1],
             "end_lat": b_end[0], "end_lon": b_end[1], "end_alt_m": 100.0},
            # A holding member (zero speed) is excluded from timing and deviation.
            {"id": 1, "speed_mps": 0.0, "start_lat": 39.0, "start_lon": 116.0,
             "end_lat": 39.0, "end_lon": 116.0, "end_alt_m": 100.0},
        ]

        def vehicles(a_east: float, b_east: float) -> list:
            a = destination(*a_start, 90.0, a_east)
            b = destination(*b_start, 90.0, b_east)
            return [
                {"id": 2, "lat": a[0], "lon": a[1], "alt_amsl_m": 100.0},
                {"id": 3, "lat": b[0], "lon": b[1], "alt_amsl_m": 100.0},
            ]

        samples = [(0.0, vehicles(0.0, 0.0)), (1.0, vehicles(10.0, 5.0)), (10.0, vehicles(100.0, 95.0))]
        tail = [(11.0, vehicles(100.0, 100.0))]
        metrics = formation_sync_metrics(members, samples, tail)
        self.assertEqual(metrics["moving_members"], 2)
        self.assertEqual(metrics["start_spread_s"], 0.0)
        self.assertEqual(metrics["arrival_spread_s"], 1.0)
        self.assertAlmostEqual(metrics["max_deviation_m"], 2.5, delta=0.05)
        self.assertAlmostEqual(metrics["min_separation_m"], 20.0, delta=0.05)
```

- [ ] **Step 2: 确认失败**

Run: `cd swarm_ext/sim && python3 -m unittest test_qgc_acceptance -v 2>&1 | tail -5`
Expected: `ImportError: cannot import name 'formation_sync_metrics'`。

- [ ] **Step 3: 实现指标**

`qgc_acceptance.py`：
1. 在 `formation_geometry_metrics` 中，把原有的
```python
    followers = [vehicles[vehicle_id] for vehicle_id in sorted(vehicles) if vehicle_id != leader_id]
    horizontal_errors = {}
    vertical_errors = {}
    for index, follower in enumerate(followers, start=1):
        rank = (index + 1) // 2
        side = -1 if index % 2 == 1 else 1
        expected_lat, expected_lon = destination(
            float(leader["lat"]),
            float(leader["lon"]),
            float(leader["heading_deg"]) + 180.0 + side * 30.0,
            spacing * rank,
        )
```
   替换为（循环体其余部分——水平/垂直误差计算与返回——保持不变，仍使用 `rank`）：
```python
    slots = {int(vehicle_id): int(slot) for vehicle_id, slot in formation.get("slots", {}).items()}
    heading = float(formation.get("heading_deg", leader.get("heading_deg", 0.0)))
    followers = [vehicles[vehicle_id] for vehicle_id in sorted(vehicles) if vehicle_id != leader_id]
    horizontal_errors = {}
    vertical_errors = {}
    for position, follower in enumerate(followers, start=1):
        index = slots.get(int(follower["id"]), position)
        rank = (index + 1) // 2
        side = -1 if index % 2 == 1 else 1
        expected_lat, expected_lon = destination(
            float(leader["lat"]),
            float(leader["lon"]),
            heading + 180.0 + side * 30.0,
            spacing * rank,
        )
```
（其后的误差计算与返回保持原样。）
2. 在 `formation_geometry_metrics` 之后加：
```python
def _local_offset(origin_lat: float, origin_lon: float, lat: float, lon: float) -> tuple[float, float]:
    """East/north metres of (lat, lon) relative to the origin (equirectangular, fine for a few km)."""
    radius = 6371000.0
    north = math.radians(lat - origin_lat) * radius
    east = math.radians(lon - origin_lon) * radius * math.cos(math.radians(origin_lat))
    return east, north


def formation_sync_metrics(
    members: list[dict],
    samples: list[tuple[float, list[dict]]],
    tail: list[tuple[float, list[dict]]] = (),
) -> dict:
    """Synchronization metrics for one formation leg (2026-10-05 synchronized-legs design, section 4).

    ``members`` are the transit members reported when the leg started (id, speed_mps, start_*/end_*).
    ``samples`` are (time_s, vehicles) observations while the leg was active; ``tail`` are observations
    shortly after it ended, used only to catch the last arrivals. Zero-speed (holding) members are excluded
    from timing and deviation.
    """
    moving = [member for member in members if float(member.get("speed_mps", 0.0)) > 0.0]
    started: dict[int, float] = {}
    arrived: dict[int, float] = {}
    max_deviation = 0.0
    min_separation = math.inf
    for index, (time_s, vehicles) in enumerate(list(samples) + list(tail)):
        in_leg = index < len(samples)
        by_id = {int(vehicle["id"]): vehicle for vehicle in vehicles}
        geometry = []
        for member in moving:
            member_id = int(member["id"])
            vehicle = by_id.get(member_id)
            if vehicle is None:
                continue
            start_lat, start_lon = float(member["start_lat"]), float(member["start_lon"])
            path = _local_offset(start_lat, start_lon, float(member["end_lat"]), float(member["end_lon"]))
            actual = _local_offset(start_lat, start_lon, float(vehicle["lat"]), float(vehicle["lon"]))
            if in_leg and math.hypot(*actual) > 1.0:
                started.setdefault(member_id, time_s)
            horizontal = distance_m(
                float(vehicle["lat"]), float(vehicle["lon"]), float(member["end_lat"]), float(member["end_lon"])
            )
            if horizontal <= 1.0 and abs(float(vehicle["alt_amsl_m"]) - float(member["end_alt_m"])) <= 1.0:
                arrived.setdefault(member_id, time_s)
            length_sq = path[0] ** 2 + path[1] ** 2
            progress = min(1.0, max(0.0, (actual[0] * path[0] + actual[1] * path[1]) / length_sq))
            geometry.append((path, actual, progress))
        if not in_leg:
            continue
        if geometry:
            consensus = sum(progress for _, _, progress in geometry) / len(geometry)
            for path, actual, _ in geometry:
                max_deviation = max(
                    max_deviation, math.hypot(actual[0] - consensus * path[0], actual[1] - consensus * path[1])
                )
        for i, first in enumerate(vehicles):
            for second in vehicles[i + 1 :]:
                horizontal = distance_m(
                    float(first["lat"]), float(first["lon"]), float(second["lat"]), float(second["lon"])
                )
                vertical = float(first["alt_amsl_m"]) - float(second["alt_amsl_m"])
                min_separation = min(min_separation, math.hypot(horizontal, vertical))

    def spread(times: dict[int, float]):
        if not moving or len(times) < len(moving):
            return None
        return round(max(times.values()) - min(times.values()), 2)

    return {
        "moving_members": len(moving),
        "start_spread_s": spread(started),
        "arrival_spread_s": spread(arrived),
        "max_deviation_m": round(max_deviation, 2),
        "min_separation_m": None if math.isinf(min_separation) else round(min_separation, 2),
    }
```

- [ ] **Step 4: 跑 Python 测试**

Run: `cd swarm_ext/sim && python3 -m unittest test_qgc_acceptance -v 2>&1 | tail -4`
Expected: `OK`。

- [ ] **Step 5: 写 SITL 驱动脚本**

`swarm_ext/sim/formation_sync_check.py`：
```python
#!/usr/bin/env python3
"""SITL acceptance for synchronized formation legs (scenarios S1-S5).

Design: swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md, section 4.
Drives a running QGC through its loopback acceptance interface and prints per-leg metrics as JSON.
The fleet must already be airborne. The operator may be using the same GUI: confirm before running.

Example:
  python3 formation_sync_check.py --token-file /tmp/qgc-swarm5-token/token --scenario goto-leader
"""

from __future__ import annotations

import argparse
import json
import sys
import time

from qgc_acceptance import (
    QGCAcceptanceClient,
    destination,
    formation_sync_metrics,
    read_acceptance_token,
)

POLL_S = 0.25
ARRIVAL_TAIL_S = 3.0
MIN_SEPARATION_M = 3.0
TIMING_LIMITS = {"start_spread_s": 1.5, "arrival_spread_s": 3.0, "max_deviation_m": 3.0}
TIMED_SCENARIOS = {"goto-leader", "goto-follower", "route"}  # S1-S3


def _formation(status: dict) -> dict:
    return status.get("formation", {})


def _vehicle(status: dict, vehicle_id: int) -> dict:
    return next(v for v in status.get("vehicles", []) if int(v["id"]) == vehicle_id)


def observe(client: QGCAcceptanceClient, timeout_s: float) -> tuple[list[dict], dict]:
    """Sample the transit that was just started until it ends; return per-leg metrics and its result."""
    origin = time.monotonic()
    legs: list[dict] = []
    ended_at = None
    status: dict = {}
    while time.monotonic() - origin < timeout_s:
        status = client.request("status")
        now = time.monotonic() - origin
        transit = _formation(status).get("transit", {})
        vehicles = status.get("vehicles", [])
        if transit.get("active"):
            if not legs or legs[-1]["index"] != transit.get("leg_index"):
                if legs:
                    legs[-1]["ended_at"] = now
                legs.append(
                    {"index": transit.get("leg_index"), "members": transit.get("members", []),
                     "samples": [], "tail": [], "ended_at": None}
                )
        elif legs and legs[-1]["ended_at"] is None:
            legs[-1]["ended_at"] = now
            ended_at = now
        for leg in legs:
            if leg["ended_at"] is None:
                leg["samples"].append((now, vehicles))
            elif now - leg["ended_at"] <= ARRIVAL_TAIL_S:
                leg["tail"].append((now, vehicles))
        if ended_at is not None and now - ended_at > ARRIVAL_TAIL_S:
            break
        time.sleep(POLL_S)
    metrics = [
        {"leg": leg["index"], **formation_sync_metrics(leg["members"], leg["samples"], leg["tail"])}
        for leg in legs
    ]
    return metrics, _formation(status).get("transit", {}).get("last_result", {})


def verdict(scenario: str, metrics: list[dict], result: dict) -> dict:
    failures = []
    if not result.get("success"):
        failures.append(f"transit did not complete: {result.get('reason', 'no transit observed')}")
    for leg in metrics:
        separation = leg["min_separation_m"]
        if separation is not None and separation < MIN_SEPARATION_M:
            failures.append(f"leg {leg['leg']}: separation {separation} m < {MIN_SEPARATION_M} m")
        if leg["moving_members"] < 2:
            continue
        limits = TIMING_LIMITS if scenario in TIMED_SCENARIOS else {}
        if scenario == "start":
            limits = {"arrival_spread_s": TIMING_LIMITS["arrival_spread_s"]}
        for key, limit in limits.items():
            value = leg[key]
            if value is None or value > limit:
                failures.append(f"leg {leg['leg']}: {key}={value} (limit {limit})")
    return {"scenario": scenario, "pass": not failures, "failures": failures, "legs": metrics, "result": result}


def run(client: QGCAcceptanceClient, args: argparse.Namespace) -> dict:
    status = client.request("status")
    if args.scenario == "start" or not _formation(status).get("active"):
        client.request("stop_formation")
        response = client.request(
            "start_formation", leader_id=args.leader, type="wedge",
            spacing_m=args.spacing, vertical_step_m=args.vertical_step,
        )
        if not response.get("ok"):
            return {"scenario": args.scenario, "pass": False, "failures": [f"start_formation refused: {response}"]}
        metrics, result = observe(client, args.timeout)
        if args.scenario == "start":
            return verdict("start", metrics, result)
        status = client.request("status")

    leader = _vehicle(status, args.leader)
    heading = float(_formation(status).get("heading_deg", 0.0))
    if args.scenario in ("goto-leader", "goto-follower", "reverse"):
        bearing, distance = (heading + 180.0, 30.0) if args.scenario == "reverse" else (heading + 90.0, 150.0)
        lat, lon = destination(float(leader["lat"]), float(leader["lon"]), bearing, distance)
        target_id = args.leader
        if args.scenario == "goto-follower":
            target_id = min(int(vehicle_id) for vehicle_id in _formation(status).get("slots", {}))
        response = client.request("goto", vehicle_id=target_id, center_lat=lat, center_lon=lon)
    else:  # route: three waypoints with two 90° turns and a 5 m climb
        alt_rel = float(leader["alt_rel_m"])
        first = destination(float(leader["lat"]), float(leader["lon"]), heading, 120.0)
        second = destination(*first, heading + 90.0, 120.0)
        third = destination(*second, heading + 180.0, 80.0)
        waypoints = [
            {"lat": first[0], "lon": first[1], "alt_rel_m": alt_rel},
            {"lat": second[0], "lon": second[1], "alt_rel_m": alt_rel + 5.0},
            {"lat": third[0], "lon": third[1], "alt_rel_m": alt_rel + 5.0},
        ]
        uploaded = client.request("upload_route", vehicle_id=args.leader, waypoints=waypoints)
        if not uploaded.get("ok"):
            return {"scenario": args.scenario, "pass": False, "failures": [f"upload_route refused: {uploaded}"]}
        response = {}
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:  # the upload completes asynchronously
            response = client.request("formation_route")
            if response.get("ok"):
                break
            time.sleep(1.0)
    if not response.get("ok"):
        return {"scenario": args.scenario, "pass": False, "failures": [f"command refused: {response}"]}
    metrics, result = observe(client, args.timeout)
    return verdict(args.scenario, metrics, result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=15615)
    parser.add_argument("--token-file", required=True)
    parser.add_argument("--leader", type=int, default=1)
    parser.add_argument("--spacing", type=float, default=15.0)
    parser.add_argument("--vertical-step", type=float, default=3.0)
    parser.add_argument("--timeout", type=float, default=240.0)
    parser.add_argument(
        "--scenario", required=True, choices=["start", "goto-leader", "goto-follower", "route", "reverse"]
    )
    args = parser.parse_args()
    client = QGCAcceptanceClient(args.host, args.port, read_acceptance_token(args.token_file))
    report = run(client, args)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
```
`chmod +x swarm_ext/sim/formation_sync_check.py`

- [ ] **Step 6: 登记到 README**

`swarm_ext/sim/README.md` 的「组成」表格中，在 `新功能验收checklist.md` 一行之前加：
```markdown
| `formation_sync_check.py` | **同步编队航段 SITL 验收**（S1–S5）：经 QGC 验收接口驱动启动/goto/航线/掉头，输出起步与到达时间差、偏离同步理想位置与最小间距 |
```

- [ ] **Step 7: 语法自检**

Run: `cd swarm_ext/sim && python3 -m py_compile formation_sync_check.py && python3 formation_sync_check.py --help | head -3`
Expected: 打印用法首行，无异常。

---

### Task 10: 全量验证

**Files:** 无新改动（只修复本步发现的问题）

- [ ] **Step 1: 格式化**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol
FILES="src/Swarm/FormationLegPlanner.h src/Swarm/FormationLegPlanner.cc src/Swarm/FormationLegTracker.h src/Swarm/FormationLegTracker.cc src/Swarm/FormationController.h src/Swarm/FormationController.cc src/Swarm/SwarmManager.h src/Swarm/SwarmManager.cc src/Swarm/SwarmMissionSnapshot.h src/Swarm/SwarmMissionSnapshot.cc src/Swarm/SwarmAcceptanceServer.cc src/FirmwarePlugin/FirmwarePlugin.h src/FirmwarePlugin/FirmwarePlugin.cc src/FirmwarePlugin/PX4/PX4FirmwarePlugin.h src/FirmwarePlugin/PX4/PX4FirmwarePlugin.cc src/Comms/MockLink/MockLink.h src/Comms/MockLink/MockLink.cc test/Swarm/FormationLegPlannerTest.h test/Swarm/FormationLegPlannerTest.cc test/Swarm/SwarmVehicleTest.h test/Swarm/SwarmVehicleTest.cc test/Swarm/SwarmFormationTest.h test/Swarm/SwarmFormationTest.cc test/Swarm/SwarmRegressionTest.h test/Swarm/SwarmRegressionTest.cc"
clang-format -i $FILES && git diff --stat -- $FILES | tail -1
clang-format --dry-run --Werror $FILES && echo "clang-format clean"
```
Expected: `clang-format clean`。

- [ ] **Step 2: 仓库自定义检查**

```bash
QGC=/media/hello/data1/Jinqiang/QGC/qgroundcontrol
docker run --rm -u 1000:1000 --entrypoint bash -v $QGC:/project/source:ro qgc-ubuntu-docker:latest -lc \
  'cd /project/source && python3 tools/analyzers/vehicle_null_check.py src/Swarm/FormationLegPlanner.cc src/Swarm/FormationLegTracker.cc src/Swarm/FormationController.cc src/Swarm/SwarmManager.cc src/Swarm/SwarmAcceptanceServer.cc src/FirmwarePlugin/PX4/PX4FirmwarePlugin.cc && echo null-check-ok'
grep -n "Q_ASSERT" src/Swarm/FormationLeg*.cc src/Swarm/FormationController.cc || echo "no Q_ASSERT"
grep -nE "qWait\([1-9]|QTest::ignoreMessage" test/Swarm/FormationLegPlannerTest.cc test/Swarm/SwarmVehicleTest.cc test/Swarm/SwarmFormationTest.cc test/Swarm/SwarmRegressionTest.cc || echo "no fixed waits"
```
Expected: `null-check-ok`、`no Q_ASSERT`、`no fixed waits`。

- [ ] **Step 3: 重新编译并跑完整 Unit|Integration（与 CI 同命令）**

```bash
/tmp/qgc_build.sh 2>&1 | tail -2
/tmp/qgc_ctest.sh -L "'Unit|Integration'" -LE "'Flaky|Network'" 2>&1 | tail -6
cd swarm_ext/sim && python3 -m unittest test_qgc_acceptance 2>&1 | tail -2
```
Expected: `100% tests passed`（9/30 记录为 249/249，本次应为 250 个以上）；Python `OK`。任何失败都要先修再进入 Task 11，并在汇报中写出失败输出。

---

### Task 11: SITL 验收与验证记录（需用户确认）

**Files:**
- Modify: `swarm_ext/docs/specs/2026-10-05-synchronized-formation-legs-design.md`（第 5 节）

- [ ] **Step 1: 征得用户同意**

用户常在 GUI 里同时手动操作。先询问：是否现在重启 GUI 容器并由脚本驱动 5 机 SITL（会解锁、起飞、编队、按场景飞行）。未获同意不要执行 Step 2 之后的步骤。

- [ ] **Step 2: 重启 GUI 加载新程序并确认**

```bash
docker restart qgc-swarm5-gui
sleep 15
pid=$(pgrep -f 'Debug/QGroundControl --allow-multiple'); ls -l /proc/$pid/exe
ss -ltn | grep 15615
```
Expected: `/proc/<pid>/exe` 不带 `(deleted)`；15615 在监听。SITL 必须仍在运行（`tmux has-session -t sitl5`），否则按 memory 中的流程先拉起 5 机 SITL。

- [ ] **Step 3: 起飞（若尚未在空中）**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol/swarm_ext/sim
python3 - <<'PY'
import time
from qgc_acceptance import QGCAcceptanceClient, read_acceptance_token
c = QGCAcceptanceClient('127.0.0.1', 15615, read_acceptance_token('/tmp/qgc-swarm5-token/token'))
print(c.request('arm_all').get('ok')); time.sleep(3)
print(c.request('takeoff_all', altitude_m=30).get('ok'))
s = c.wait_status(lambda s: all(v['alt_rel_m'] > 25 for v in s['vehicles']), timeout_sec=120)
print([round(v['alt_rel_m'], 1) for v in s['vehicles']])
PY
```
Expected: 5 架相对高度都 > 25 m。

- [ ] **Step 4: 依次跑 S4、S1、S2、S3、S5**

```bash
cd /media/hello/data1/Jinqiang/QGC/qgroundcontrol/swarm_ext/sim
for s in start goto-leader goto-follower route reverse; do
  python3 formation_sync_check.py --token-file /tmp/qgc-swarm5-token/token --scenario $s | tee /tmp/formation_sync_$s.json | head -40
done
```
Expected: 每个场景 `"pass": true`。不通过时保留 JSON，按 `failures` 排查（例如 `max_deviation_m` 超标先看共识调速重发是否引起 PX4 速度顿挫：`/tmp/px4_swarm_sitl/inst_*/px4.log` 与 QGC 审计日志中的 `leg_completed.max_deviation_m`）。若确认重发造成顿挫（掉速 > 1 m/s），把 `FormationLegTracker::kSpeedGain` 设为 0 退回开环同步，重跑并在记录中说明（设计第 4 节专项观察）。

- [ ] **Step 5: 写验证记录**

把设计文档第 5 节的「（实现后填写）」替换为实际结果，至少包含：完整 ctest 命令与通过数；每个 SITL 场景的起步/到达时间差、最大偏离、最小间距、是否触发拆段/分层（审计 `leg_planned.leg_count`）；与 9/30 基线（途中拖后 15–19 m、到达后约 7 s 收敛、2 号机启动卡 60 s）的对比；任何未通过项与原因。

- [ ] **Step 6: 询问用户如何提交**

工作区同时含 9/30 未提交改动。向用户列出本计划涉及的文件，询问是单独提交本功能（需要按文件挑选 hunk）还是与 9/30 改动一起提交。建议的提交信息：
```
feat(Swarm): synchronized formation legs for goto, routes and reshapes

Every member, the leader included, is commanded in the same cycle with a
ground speed proportional to its path so the formation starts and arrives
together; a 1 Hz progress consensus corrects stragglers. Pairwise separation
along the synchronized paths is checked in closed form, with sub-leg splits
and vertical layering as fallbacks. Adds a "fly the leader's route" action.
```
