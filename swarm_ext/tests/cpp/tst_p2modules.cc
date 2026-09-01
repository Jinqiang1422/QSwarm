/****************************************************************************
 *
 * P2 模块单元测试 —— 仅依赖 Qt6 Core/Test：
 *   - SwarmConstraintChecker：围栏/电量/间距/链路/模板级
 *   - SwarmRehearsalEstimator：时长与裕度估算
 *   - SwarmTaskAllocator：拍卖分配正确性与对比基线
 *
 ****************************************************************************/

#include <QtTest/QtTest>

#include "SwarmConstraintChecker.h"
#include "SwarmRehearsalEstimator.h"
#include "SwarmTaskAllocator.h"

class TestP2Modules : public QObject
{
    Q_OBJECT

private slots:
    void constraint_takeoffAltitude();
    void constraint_fenceExclude();
    void constraint_batteryError();
    void constraint_spacing();
    void constraint_templateFleet();
    void estimator_basicPlan();
    void allocator_correctness();
    void allocator_beatsSequentialOnReversed();
    // P2 约束校验加固：补充允许空域/链路分级/多区重叠/间距 warning/电量未知/
    // hasError 语义/无围栏跳过/几何/配置往返等边界与分级分支
    void constraint_allowedAirspace();
    void constraint_commRangeBands();
    void constraint_overlapWarning();
    void constraint_spacingWarningBand();
    void constraint_batteryUnknownWarning();
    void constraint_hasErrorSemantics();
    void constraint_noFenceConfigSkips();
    void coverageGeometry_math();
    void config_jsonRoundTrip();
    void constraint_largeAreasOverlapMissed();
    void constraint_smallAreasNoFalsePositive();
    // estimator/allocator 边界加固
    void estimator_emptyPlan();
    void estimator_actionsAccumulate();
    void estimator_summaryFormat();
    void estimator_configAffectsDuration();
    void estimator_biggerAreaLessMargin();
    void allocator_sequentialAssign();
    void allocator_auctionNoWorseThanSeq();
    void allocator_singleNode();
    void allocator_assignmentCostMath();
    void allocator_alignedZeroCost();
    // CBBA 分布式一致性拍卖(移植自 zehuilu/CBBA-Python)
    void allocator_cbbaSurjective();
    void allocator_cbbaAlignedOptimal();
    void allocator_cbbaInCompare();
    void allocator_cbbaBundleCoversAll();
    void allocator_cbbaBundleCapacity();
    void allocator_moreVehiclesThanTargets();
    void allocator_emptyInput();
    void allocator_bothBeatSequentialReversed();

private:
    static SwarmAction coverageAt(double length, double width, double spacing, double lat, double lon)
    {
        SwarmAction a;
        a.type = QStringLiteral("plan_coverage");
        a.params = {{QStringLiteral("length"), length},
                    {QStringLiteral("width"), width},
                    {QStringLiteral("spacing"), spacing},
                    {QStringLiteral("center_lat"), lat},
                    {QStringLiteral("center_lon"), lon}};
        return a;
    }

    static bool hasRule(const QList<SwarmConstraintChecker::Violation>& vs, const QString& rule,
                        const QString& severity)
    {
        for (const auto& v : vs) {
            if (v.rule == rule && v.severity == severity)
                return true;
        }
        return false;
    }

    static bool hasAnyRule(const QList<SwarmConstraintChecker::Violation>& vs, const QString& rule)
    {
        for (const auto& v : vs)
            if (v.rule == rule)
                return true;
        return false;
    }

    static SwarmAction coverage(double length, double width, double spacing)
    {
        SwarmAction a;
        a.type = QStringLiteral("plan_coverage");
        a.params = {
            {QStringLiteral("length"), length}, {QStringLiteral("width"), width}, {QStringLiteral("spacing"), spacing}};
        return a;
    }

    static QList<SwarmTypes::VehicleState> fleet(int n, double battPct)
    {
        QList<SwarmTypes::VehicleState> out;
        for (int i = 1; i <= n; i++) {
            SwarmTypes::VehicleState v;
            v.id = i;
            v.lat = 39.9;
            v.lon = 116.4;
            v.batteryPct = battPct;
            out.append(v);
        }
        return out;
    }
};

// ---------------------------------------------------------------------------
void TestP2Modules::constraint_takeoffAltitude()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.maxAltitudeM = 100.0;

    SwarmAction tk;
    tk.type = QStringLiteral("takeoff");
    tk.params = {{QStringLiteral("altitude"), 150.0}};

    const auto violations = checker.check({tk}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(SwarmConstraintChecker::hasError(violations));
    QCOMPARE(violations.first().rule, QStringLiteral("fence"));
    QVERIFY(!violations.first().suggestion.isEmpty());
}

void TestP2Modules::constraint_fenceExclude()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    // 以 (39.9,116.4) 为中心的禁飞方块
    cfg.excludePolygons = {{{39.895, 116.395}, {39.895, 116.405}, {39.905, 116.405}, {39.905, 116.395}}};
    // 作业区中心落在禁飞区内 → error
    auto violations = checker.check({coverage(300, 200, 25)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(SwarmConstraintChecker::hasError(violations));
    bool hasFenceError = false;
    for (const auto& v : violations) {
        hasFenceError |= (v.rule == QStringLiteral("fence") && v.severity == QStringLiteral("error"));
    }
    QVERIFY(hasFenceError);

    // 作业区中心远离禁飞区 → 无 fence error
    cfg.commRangeM = 1e9;  // 屏蔽链路告警干扰
    violations = checker.check({coverage(300, 200, 25)}, fleet(4, 90.0), cfg, 40.1, 117.1);
    for (const auto& v : violations) {
        QVERIFY(v.rule != QStringLiteral("fence"));
    }
}

void TestP2Modules::constraint_batteryError()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.enduranceMin = 20.0;
    // 超大区域 + 低电量 → error
    const auto violations = checker.check({coverage(5000, 5000, 10)}, fleet(2, 30.0), cfg, 39.9, 116.4);
    bool hasBatteryError = false;
    for (const auto& v : violations) {
        hasBatteryError |= (v.rule == QStringLiteral("battery") && v.severity == QStringLiteral("error"));
    }
    QVERIFY(hasBatteryError);
}

void TestP2Modules::constraint_spacing()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.minSeparationM = 3.0;

    SwarmAction f;
    f.type = QStringLiteral("start_formation");
    f.params = {{QStringLiteral("spacing"), 2.0}};
    QVERIFY(SwarmConstraintChecker::hasError(checker.check({f}, fleet(4, 90.0), cfg, 39.9, 116.4)));

    f.params = {{QStringLiteral("spacing"), 10.0}};
    QVERIFY(!SwarmConstraintChecker::hasError(checker.check({f}, fleet(4, 90.0), cfg, 39.9, 116.4)));
}

void TestP2Modules::constraint_templateFleet()
{
    SwarmConstraintChecker checker;
    auto v = checker.checkTemplate(4, QStringLiteral("camera"), 3, false);
    QVERIFY(SwarmConstraintChecker::hasError(v));  // 机数不足
    bool hasPayloadWarn = false;
    for (const auto& x : v) {
        hasPayloadWarn |= (x.rule == QStringLiteral("payload") && x.severity == QStringLiteral("warning"));
    }
    QVERIFY(hasPayloadWarn);

    v = checker.checkTemplate(4, QStringLiteral("camera"), 8, true);
    QVERIFY(v.isEmpty());  // 规模够且载荷已知
}

// ---------------------------------------------------------------------------
void TestP2Modules::estimator_basicPlan()
{
    SwarmRehearsalEstimator est;
    SwarmAction tk;
    tk.type = QStringLiteral("takeoff");
    tk.params = {{QStringLiteral("altitude"), 30.0}};

    const auto r = est.estimate({tk, coverage(600, 400, 20)}, fleet(4, 80.0), 39.9, 116.4);
    QVERIFY(r.durationMin > 2.0 && r.durationMin < 30.0);
    QVERIFY(r.distanceKm > 10.0);
    QVERIFY(r.batteryMarginPct < 80.0 && r.batteryMarginPct > 0.0);
    QCOMPARE(r.vehiclesUsed, 4);
    QVERIFY(!SwarmRehearsalEstimator::summary(r).isEmpty());
}

// ---------------------------------------------------------------------------
void TestP2Modules::allocator_correctness()
{
    SwarmTaskAllocator alloc;
    // 8 机散布 + 8 条带目标，拍卖分配必须是一对一满射
    QList<SwarmTaskAllocator::Node> vs, ts;
    for (int i = 0; i < 8; i++) {
        vs.append({i + 1, (i % 4) * 50.0, (i / 4) * 300.0});
        ts.append({101 + i, (i % 4) * 50.0, (i / 4) * 300.0});
    }
    const auto a = alloc.auctionAssign(vs, ts);
    QCOMPARE(a.count(), 8);
    QSet<int> usedTargets;
    for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
        usedTargets.insert(it.value());
    }
    QCOMPARE(usedTargets.count(), 8);  // 无重复分配
}

void TestP2Modules::allocator_beatsSequentialOnReversed()
{
    SwarmTaskAllocator alloc;
    // 反转布局：顺序分配最差，拍卖应显著优于基线
    QList<SwarmTaskAllocator::Node> vs, ts;
    vs.append({1, 0.0, -100.0});
    vs.append({2, 0.0, 100.0});
    ts.append({101, 0.0, 90.0});
    ts.append({102, 0.0, -90.0});
    const auto cmp = alloc.compare(vs, ts);
    QVERIFY(cmp.value(QStringLiteral("auction_total_m")).toDouble() <
            cmp.value(QStringLiteral("seq_total_m")).toDouble() * 0.5);
}

// === P2 约束校验加固测试(补充边界/分级/几何/配置) ===

void TestP2Modules::constraint_allowedAirspace()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1e9;
    cfg.enduranceMin = 1e6;  // 屏蔽链路/电量干扰
    cfg.includePolygons = {{{39.895, 116.395}, {39.895, 116.405}, {39.905, 116.405}, {39.905, 116.395}}};
    // 中心在允许空域内 → 无 fence 违例
    auto v = checker.check({coverageAt(100, 100, 25, 39.9, 116.4)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(!hasAnyRule(v, QStringLiteral("fence")));
    // 中心在允许空域外 → fence error
    v = checker.check({coverageAt(100, 100, 25, 40.2, 117.2)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(hasRule(v, QStringLiteral("fence"), QStringLiteral("error")));
}

void TestP2Modules::constraint_commRangeBands()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1000.0;
    cfg.homeLat = 39.9;
    cfg.homeLon = 116.4;
    cfg.enduranceMin = 1e6;  // 屏蔽电量
    // 中心距集结点 ~1334m + 半对角 ~71m ≈ 1405m ∈ (1000,1500) → warning
    auto v = checker.check({coverageAt(100, 100, 25, 39.912, 116.4)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(hasRule(v, QStringLiteral("comm"), QStringLiteral("warning")));
    QVERIFY(!hasRule(v, QStringLiteral("comm"), QStringLiteral("error")));
    // 中心距集结点 ~2505m > 1.5×comm=1500 → error
    v = checker.check({coverageAt(100, 100, 25, 39.9225, 116.4)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(hasRule(v, QStringLiteral("comm"), QStringLiteral("error")));
}

void TestP2Modules::constraint_overlapWarning()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1e9;
    cfg.enduranceMin = 1e6;
    // 两作业区中心相距 ~11m (<50) → deconflict warning
    auto v = checker.check({coverageAt(100, 100, 25, 39.9, 116.4), coverageAt(100, 100, 25, 39.9001, 116.4)},
                           fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(hasRule(v, QStringLiteral("deconflict"), QStringLiteral("warning")));
    // 相距 ~222m (>50) → 无 deconflict
    v = checker.check({coverageAt(100, 100, 25, 39.9, 116.4), coverageAt(100, 100, 25, 39.902, 116.4)}, fleet(4, 90.0),
                      cfg, 39.9, 116.4);
    QVERIFY(!hasAnyRule(v, QStringLiteral("deconflict")));
}

void TestP2Modules::constraint_spacingWarningBand()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.minSeparationM = 3.0;
    SwarmAction f;
    f.type = QStringLiteral("start_formation");
    f.params = {{QStringLiteral("spacing"), 5.0}};  // 3 < 5 < 6 → warning(非 error)
    const auto v = checker.check({f}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(!SwarmConstraintChecker::hasError(v));
    QVERIFY(hasRule(v, QStringLiteral("spacing"), QStringLiteral("warning")));
}

void TestP2Modules::constraint_batteryUnknownWarning()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    // 全机电量未知(-1) → battery warning「电量未知」
    const auto v = checker.check({coverageAt(200, 150, 25, 39.9, 116.4)}, fleet(4, -1.0), cfg, 39.9, 116.4);
    QVERIFY(hasRule(v, QStringLiteral("battery"), QStringLiteral("warning")));
}

void TestP2Modules::constraint_hasErrorSemantics()
{
    // 仅 warning 的场景 → hasError=false
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.minSeparationM = 3.0;
    SwarmAction f;
    f.type = QStringLiteral("start_formation");
    f.params = {{QStringLiteral("spacing"), 5.0}};
    const auto v = checker.check({f}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(!v.isEmpty());
    QVERIFY(!SwarmConstraintChecker::hasError(v));
    QVERIFY(!SwarmConstraintChecker::hasError({}));  // 空 → false
}

void TestP2Modules::constraint_noFenceConfigSkips()
{
    // 无 include/exclude 配置 → 任意中心都不产生 fence 违例
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1e9;
    cfg.enduranceMin = 1e6;
    const auto v = checker.check({coverageAt(300, 200, 25, 12.3, 45.6)}, fleet(4, 90.0), cfg, 12.3, 45.6);
    QVERIFY(!hasAnyRule(v, QStringLiteral("fence")));
}

void TestP2Modules::coverageGeometry_math()
{
    int bands = 0;
    double sweep = 0.0;
    SwarmConstraintChecker::coverageGeometry(600, 400, 20, bands, sweep);
    QCOMPARE(bands, 20);                      // ceil(400/20)
    QCOMPARE(sweep, 20 * 600.0 + 19 * 20.0);  // 12380
    // spacing=0 保护：不除零，bands≥1
    SwarmConstraintChecker::coverageGeometry(100, 50, 0, bands, sweep);
    QVERIFY(bands >= 1);
    QVERIFY(sweep > 0.0);
}

void TestP2Modules::config_jsonRoundTrip()
{
    SwarmConstraintChecker::Config cfg;
    cfg.maxAltitudeM = 120.0;
    cfg.commRangeM = 800.0;
    cfg.minSeparationM = 4.0;
    cfg.cruiseSpeedMs = 15.0;
    cfg.enduranceMin = 30.0;
    cfg.reserveFactor = 0.75;
    const auto back = SwarmConstraintChecker::configFromJson(SwarmConstraintChecker::configToJson(cfg));
    QCOMPARE(back.maxAltitudeM, 120.0);
    QCOMPARE(back.commRangeM, 800.0);
    QCOMPARE(back.minSeparationM, 4.0);
    QCOMPARE(back.cruiseSpeedMs, 15.0);
    QCOMPARE(back.enduranceMin, 30.0);
    QCOMPARE(back.reserveFactor, 0.75);
}

// === estimator / allocator 边界加固 ===

void TestP2Modules::estimator_emptyPlan()
{
    SwarmRehearsalEstimator est;
    const auto r = est.estimate({}, fleet(4, 80.0), 39.9, 116.4);
    QCOMPARE(r.durationMin, 0.0);
    QCOMPARE(r.distanceKm, 0.0);
    QCOMPARE(r.vehiclesUsed, 4);
    QCOMPARE(r.batteryMarginPct, 80.0);  // minBatt - 0
    QVERIFY(!r.notes.isEmpty());         // "计划无显著耗时动作"
}

void TestP2Modules::estimator_actionsAccumulate()
{
    SwarmRehearsalEstimator est;
    SwarmAction tk;
    tk.type = QStringLiteral("takeoff");
    tk.params = {{QStringLiteral("altitude"), 30.0}};
    SwarmAction fm;
    fm.type = QStringLiteral("start_formation");
    SwarmAction rtl;
    rtl.type = QStringLiteral("rtl");
    const auto cov = coverageAt(600, 400, 20, 40.0, 116.5);  // 远处覆盖区，rtl 返场里程可观
    const auto base = est.estimate({tk, cov}, fleet(4, 80.0), 39.9, 116.4);
    const auto withFm = est.estimate({tk, fm, cov}, fleet(4, 80.0), 39.9, 116.4);
    const auto withRtl = est.estimate({tk, cov, rtl}, fleet(4, 80.0), 39.9, 116.4);
    QVERIFY(withFm.durationMin > base.durationMin);   // start_formation +0.5
    QVERIFY(withRtl.durationMin > base.durationMin);  // rtl 返场+下降
    QVERIFY(withRtl.distanceKm > base.distanceKm);    // rtl 增里程
}

void TestP2Modules::estimator_summaryFormat()
{
    SwarmRehearsalEstimator est;
    SwarmAction tk;
    tk.type = QStringLiteral("takeoff");
    tk.params = {{QStringLiteral("altitude"), 30.0}};
    const QString s =
        SwarmRehearsalEstimator::summary(est.estimate({tk, coverage(600, 400, 20)}, fleet(4, 80.0), 39.9, 116.4));
    QVERIFY(s.contains(QStringLiteral("分钟")));
    QVERIFY(s.contains(QStringLiteral("km")));
    QVERIFY(s.contains(QStringLiteral("裕度")));
}

void TestP2Modules::estimator_configAffectsDuration()
{
    SwarmRehearsalEstimator est;
    SwarmRehearsalEstimator::Config slow;
    slow.cruiseSpeedMs = 6.0;
    SwarmRehearsalEstimator::Config fast;
    fast.cruiseSpeedMs = 24.0;
    const auto cov = coverage(600, 400, 20);
    const auto rSlow = est.estimate({cov}, fleet(4, 80.0), 39.9, 116.4, slow);
    const auto rFast = est.estimate({cov}, fleet(4, 80.0), 39.9, 116.4, fast);
    QVERIFY(rFast.durationMin < rSlow.durationMin);  // 巡航更快→更短
}

void TestP2Modules::estimator_biggerAreaLessMargin()
{
    SwarmRehearsalEstimator est;
    const auto small = est.estimate({coverage(200, 150, 25)}, fleet(4, 80.0), 39.9, 116.4);
    const auto big = est.estimate({coverage(3000, 2000, 25)}, fleet(4, 80.0), 39.9, 116.4);
    QVERIFY(big.durationMin > small.durationMin);
    QVERIFY(big.batteryMarginPct < small.batteryMarginPct);  // 耗时多→裕度低
}

void TestP2Modules::allocator_sequentialAssign()
{
    SwarmTaskAllocator alloc;
    const QList<SwarmTaskAllocator::Node> vs = {{1, 0, 0}, {2, 50, 0}, {3, 100, 0}};
    const QList<SwarmTaskAllocator::Node> ts = {{101, 0, 0}, {102, 50, 0}, {103, 100, 0}};
    const auto a = alloc.sequentialAssign(vs, ts);
    QCOMPARE(a.value(1), 101);
    QCOMPARE(a.value(2), 102);
    QCOMPARE(a.value(3), 103);
}

void TestP2Modules::allocator_auctionNoWorseThanSeq()
{
    SwarmTaskAllocator alloc;
    QList<SwarmTaskAllocator::Node> vs, ts;
    for (int i = 0; i < 6; i++) {
        vs.append({i + 1, i * 40.0, (i % 2) * 60.0});
        ts.append({201 + i, (5 - i) * 40.0, (i % 3) * 30.0});
    }
    const auto cmp = alloc.compare(vs, ts);
    QVERIFY(cmp.value(QStringLiteral("auction_total_m")).toDouble() <=
            cmp.value(QStringLiteral("seq_total_m")).toDouble() + 1e-6);
}

void TestP2Modules::allocator_singleNode()
{
    SwarmTaskAllocator alloc;
    const auto a = alloc.auctionAssign({{7, 0, 0}}, {{99, 10, 10}});
    QCOMPARE(a.count(), 1);
    QCOMPARE(a.value(7), 99);
}

void TestP2Modules::allocator_assignmentCostMath()
{
    const QList<SwarmTaskAllocator::Node> vs = {{1, 0, 0}};
    const QList<SwarmTaskAllocator::Node> ts = {{101, 3, 4}};
    QMap<int, int> assign;
    assign[1] = 101;
    double maxCost = 0.0;
    const double total = SwarmTaskAllocator::assignmentCost(assign, vs, ts, maxCost);
    QVERIFY(qAbs(total - 5.0) < 1e-6);  // sqrt(3²+4²)
    QVERIFY(qAbs(maxCost - 5.0) < 1e-6);
}

void TestP2Modules::allocator_alignedZeroCost()
{
    SwarmTaskAllocator alloc;
    const QList<SwarmTaskAllocator::Node> vs = {{1, 0, 0}, {2, 100, 100}};
    const QList<SwarmTaskAllocator::Node> ts = {{101, 0, 0}, {102, 100, 100}};
    const auto a = alloc.auctionAssign(vs, ts);
    double maxCost = 0.0;
    const double total = SwarmTaskAllocator::assignmentCost(a, vs, ts, maxCost);
    QVERIFY(total < 1e-6);  // 机与目标同位置→零成本最优
}

// === CBBA 分布式一致性拍卖 ===

void TestP2Modules::allocator_cbbaSurjective()
{
    // 8 机 8 目标散布 → CBBA 收敛到一对一满射,无重复分配(conflict-free)
    SwarmTaskAllocator alloc;
    QList<SwarmTaskAllocator::Node> vs, ts;
    for (int i = 0; i < 8; i++) {
        vs.append({i + 1, (i % 4) * 50.0, (i / 4) * 300.0});
        ts.append({101 + i, ((i + 2) % 4) * 50.0, (i / 4) * 250.0});
    }
    const auto a = alloc.cbbaAssign(vs, ts);
    QCOMPARE(a.count(), 8);
    QSet<int> usedTargets;
    for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
        usedTargets.insert(it.value());
    }
    QCOMPARE(usedTargets.count(), 8);  // 无重复目标(conflict-free)
}

void TestP2Modules::allocator_cbbaAlignedOptimal()
{
    // 机与目标位置一一重合 → CBBA 应收敛到零成本(每机投自身位置目标)
    SwarmTaskAllocator alloc;
    const QList<SwarmTaskAllocator::Node> vs = {{1, 0, 0}, {2, 100, 0}, {3, 0, 100}, {4, 100, 100}};
    const QList<SwarmTaskAllocator::Node> ts = {{101, 0, 0}, {102, 100, 0}, {103, 0, 100}, {104, 100, 100}};
    const auto a = alloc.cbbaAssign(vs, ts);
    QCOMPARE(a.count(), 4);
    double maxCost = 0.0;
    const double total = SwarmTaskAllocator::assignmentCost(a, vs, ts, maxCost);
    QVERIFY(total < 1e-6);  // 收敛到零成本最优
}

void TestP2Modules::allocator_cbbaInCompare()
{
    // compare 三方对照含 cbba 指标,且 CBBA 产出合法满射(有限成本)
    SwarmTaskAllocator alloc;
    QList<SwarmTaskAllocator::Node> vs, ts;
    for (int i = 0; i < 5; i++) {
        vs.append({i + 1, i * 40.0, 0.0});
        ts.append({201 + i, (4 - i) * 40.0, 0.0});
    }
    const auto cmp = alloc.compare(vs, ts);
    QVERIFY(cmp.contains(QStringLiteral("cbba_total_m")));
    QVERIFY(cmp.value(QStringLiteral("cbba_total_m")).toDouble() >= 0.0);
    QCOMPARE(alloc.cbbaAssign(vs, ts).count(), 5);
}

// === 多任务 CBBA(bundle) ===

void TestP2Modules::allocator_cbbaBundleCoversAll()
{
    // 4 机 12 目标 → 多任务 CBBA 覆盖全部 12,无重复,每机 ≤ ceil(12/4)=3
    SwarmTaskAllocator alloc;
    QList<SwarmTaskAllocator::Node> vs, ts;
    for (int i = 0; i < 4; i++) {
        vs.append({i + 1, (i % 2) * 200.0, (i / 2) * 200.0});
    }
    for (int j = 0; j < 12; j++) {
        ts.append({101 + j, (j % 4) * 60.0, (j / 4) * 60.0});
    }
    const auto a = alloc.cbbaAssignBundle(vs, ts);
    QSet<int> covered;
    int maxBundle = 0;
    for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
        maxBundle = qMax(maxBundle, it.value().size());
        for (const int tid : it.value()) {
            covered.insert(tid);
        }
    }
    QCOMPARE(covered.count(), 12);  // 覆盖全部目标且无重复
    QVERIFY(maxBundle <= 3);        // 不超均衡容量
}

void TestP2Modules::allocator_cbbaBundleCapacity()
{
    // maxBundlePerAgent=2 → 每机最多 2 目标;3 机×2=6=目标数,应全覆盖
    SwarmTaskAllocator alloc;
    QList<SwarmTaskAllocator::Node> vs, ts;
    for (int i = 0; i < 3; i++) {
        vs.append({i + 1, i * 100.0, 0.0});
    }
    for (int j = 0; j < 6; j++) {
        ts.append({101 + j, j * 50.0, 0.0});
    }
    const auto a = alloc.cbbaAssignBundle(vs, ts, 2);
    QSet<int> covered;
    for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
        QVERIFY(it.value().size() <= 2);
        for (const int tid : it.value()) {
            covered.insert(tid);
        }
    }
    QCOMPARE(covered.count(), 6);
}

// === 分配底座健壮性补测(第三轮 QA) ===

void TestP2Modules::allocator_moreVehiclesThanTargets()
{
    // 非对称:3 机 2 目标 → 至多分配 2 机,无重复目标(暴露 qMin 截断行为)
    SwarmTaskAllocator alloc;
    const QList<SwarmTaskAllocator::Node> vs = {{1, 0, 0}, {2, 50, 0}, {3, 100, 0}};
    const QList<SwarmTaskAllocator::Node> ts = {{101, 0, 0}, {102, 100, 0}};
    const auto a = alloc.auctionAssign(vs, ts);
    QVERIFY2(a.count() <= 2, "目标仅 2 个,至多分配 2 机");
    QSet<int> used;
    for (auto it = a.constBegin(); it != a.constEnd(); ++it)
        used.insert(it.value());
    QCOMPARE(used.count(), a.count());  // 无重复目标
}

void TestP2Modules::allocator_emptyInput()
{
    SwarmTaskAllocator alloc;
    QVERIFY(alloc.auctionAssign({}, {}).isEmpty());
    QVERIFY(alloc.cbbaAssign({{1, 0, 0}}, {}).isEmpty());          // 0 目标
    QVERIFY(alloc.sequentialAssign({}, {{101, 0, 0}}).isEmpty());  // 0 机
}

void TestP2Modules::allocator_bothBeatSequentialReversed()
{
    // 反转布局:auction 与 cbba 总里程都应不劣于顺序基线
    SwarmTaskAllocator alloc;
    const QList<SwarmTaskAllocator::Node> vs = {{1, 0, 0}, {2, 0, 100}, {3, 0, 200}};
    const QList<SwarmTaskAllocator::Node> ts = {{101, 0, 200}, {102, 0, 100}, {103, 0, 0}};
    const QVariantMap cmp = alloc.compare(vs, ts);
    const double seq = cmp.value(QStringLiteral("seq_total_m")).toDouble();
    QVERIFY2(cmp.value(QStringLiteral("auction_total_m")).toDouble() <= seq + 1e-6, "auction 不劣于顺序");
    QVERIFY2(cmp.value(QStringLiteral("cbba_total_m")).toDouble() <= seq + 1e-6, "cbba 不劣于顺序");
}

// === deconflict 语义补测(第四轮 QA:按区尺寸判重叠) ===

void TestP2Modules::constraint_largeAreasOverlapMissed()
{
    // 两个 1000m 作业区中心相距 ~55m,必然重叠——修复前固定 50m 阈值漏报,现应报警
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1e9;
    cfg.enduranceMin = 1e6;  // 屏蔽 comm/battery,只看 deconflict
    const auto v = checker.check({coverageAt(1000, 1000, 30, 39.9, 116.4), coverageAt(1000, 1000, 30, 39.9005, 116.4)},
                                 fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY2(hasRule(v, QStringLiteral("deconflict"), QStringLiteral("warning")),
             "两个 1km 作业区中心相距 55m 必然重叠,应报 deconflict(修复漏报)");
}

void TestP2Modules::constraint_smallAreasNoFalsePositive()
{
    // 两个 20m 作业区中心相距 ~43m,不重叠——修复前固定 50m 阈值误报,现应放行
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1e9;
    cfg.enduranceMin = 1e6;
    const auto v = checker.check({coverageAt(20, 20, 5, 39.9, 116.4), coverageAt(20, 20, 5, 39.9, 116.4005)},
                                 fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY2(!hasAnyRule(v, QStringLiteral("deconflict")), "两个 20m 小区相距 43m 不重叠,不应误报(修复误报)");
}

QTEST_GUILESS_MAIN(TestP2Modules)
#include "tst_p2modules.moc"
