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

private:
    static SwarmAction coverageAt(double length, double width, double spacing,
                                  double lat, double lon)
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
    static bool hasRule(const QList<SwarmConstraintChecker::Violation> &vs,
                        const QString &rule, const QString &severity)
    {
        for (const auto &v : vs) {
            if (v.rule == rule && v.severity == severity) return true;
        }
        return false;
    }
    static bool hasAnyRule(const QList<SwarmConstraintChecker::Violation> &vs, const QString &rule)
    {
        for (const auto &v : vs) if (v.rule == rule) return true;
        return false;
    }
    static SwarmAction coverage(double length, double width, double spacing)
    {
        SwarmAction a;
        a.type = QStringLiteral("plan_coverage");
        a.params = {{QStringLiteral("length"), length},
                    {QStringLiteral("width"), width},
                    {QStringLiteral("spacing"), spacing}};
        return a;
    }
    static QList<SwarmTypes::VehicleState> fleet(int n, double battPct)
    {
        QList<SwarmTypes::VehicleState> out;
        for (int i = 1; i <= n; i++) {
            SwarmTypes::VehicleState v;
            v.id = i;
            v.lat = 39.9; v.lon = 116.4;
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
    cfg.excludePolygons = {{{39.895, 116.395}, {39.895, 116.405},
                            {39.905, 116.405}, {39.905, 116.395}}};
    // 作业区中心落在禁飞区内 → error
    auto violations = checker.check({coverage(300, 200, 25)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(SwarmConstraintChecker::hasError(violations));
    bool hasFenceError = false;
    for (const auto &v : violations) {
        hasFenceError |= (v.rule == QStringLiteral("fence") && v.severity == QStringLiteral("error"));
    }
    QVERIFY(hasFenceError);

    // 作业区中心远离禁飞区 → 无 fence error
    cfg.commRangeM = 1e9;   // 屏蔽链路告警干扰
    violations = checker.check({coverage(300, 200, 25)}, fleet(4, 90.0), cfg, 40.1, 117.1);
    for (const auto &v : violations) {
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
    for (const auto &v : violations) {
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
    QVERIFY(SwarmConstraintChecker::hasError(v));            // 机数不足
    bool hasPayloadWarn = false;
    for (const auto &x : v) {
        hasPayloadWarn |= (x.rule == QStringLiteral("payload") && x.severity == QStringLiteral("warning"));
    }
    QVERIFY(hasPayloadWarn);

    v = checker.checkTemplate(4, QStringLiteral("camera"), 8, true);
    QVERIFY(v.isEmpty());                                    // 规模够且载荷已知
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
    QCOMPARE(usedTargets.count(), 8);   // 无重复分配
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
    QVERIFY(cmp.value(QStringLiteral("auction_total_m")).toDouble()
            < cmp.value(QStringLiteral("seq_total_m")).toDouble() * 0.5);
}

// === P2 约束校验加固测试(补充边界/分级/几何/配置) ===

void TestP2Modules::constraint_allowedAirspace()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1e9; cfg.enduranceMin = 1e6;   // 屏蔽链路/电量干扰
    cfg.includePolygons = {{{39.895, 116.395}, {39.895, 116.405},
                            {39.905, 116.405}, {39.905, 116.395}}};
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
    cfg.homeLat = 39.9; cfg.homeLon = 116.4;
    cfg.enduranceMin = 1e6;   // 屏蔽电量
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
    cfg.commRangeM = 1e9; cfg.enduranceMin = 1e6;
    // 两作业区中心相距 ~11m (<50) → deconflict warning
    auto v = checker.check({coverageAt(100, 100, 25, 39.9, 116.4),
                            coverageAt(100, 100, 25, 39.9001, 116.4)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(hasRule(v, QStringLiteral("deconflict"), QStringLiteral("warning")));
    // 相距 ~222m (>50) → 无 deconflict
    v = checker.check({coverageAt(100, 100, 25, 39.9, 116.4),
                       coverageAt(100, 100, 25, 39.902, 116.4)}, fleet(4, 90.0), cfg, 39.9, 116.4);
    QVERIFY(!hasAnyRule(v, QStringLiteral("deconflict")));
}

void TestP2Modules::constraint_spacingWarningBand()
{
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.minSeparationM = 3.0;
    SwarmAction f;
    f.type = QStringLiteral("start_formation");
    f.params = {{QStringLiteral("spacing"), 5.0}};   // 3 < 5 < 6 → warning(非 error)
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
    QVERIFY(!SwarmConstraintChecker::hasError({}));   // 空 → false
}

void TestP2Modules::constraint_noFenceConfigSkips()
{
    // 无 include/exclude 配置 → 任意中心都不产生 fence 违例
    SwarmConstraintChecker checker;
    SwarmConstraintChecker::Config cfg;
    cfg.commRangeM = 1e9; cfg.enduranceMin = 1e6;
    const auto v = checker.check({coverageAt(300, 200, 25, 12.3, 45.6)}, fleet(4, 90.0), cfg, 12.3, 45.6);
    QVERIFY(!hasAnyRule(v, QStringLiteral("fence")));
}

void TestP2Modules::coverageGeometry_math()
{
    int bands = 0; double sweep = 0.0;
    SwarmConstraintChecker::coverageGeometry(600, 400, 20, bands, sweep);
    QCOMPARE(bands, 20);                              // ceil(400/20)
    QCOMPARE(sweep, 20 * 600.0 + 19 * 20.0);          // 12380
    // spacing=0 保护：不除零，bands≥1
    SwarmConstraintChecker::coverageGeometry(100, 50, 0, bands, sweep);
    QVERIFY(bands >= 1);
    QVERIFY(sweep > 0.0);
}

void TestP2Modules::config_jsonRoundTrip()
{
    SwarmConstraintChecker::Config cfg;
    cfg.maxAltitudeM = 120.0; cfg.commRangeM = 800.0; cfg.minSeparationM = 4.0;
    cfg.cruiseSpeedMs = 15.0; cfg.enduranceMin = 30.0; cfg.reserveFactor = 0.75;
    const auto back = SwarmConstraintChecker::configFromJson(SwarmConstraintChecker::configToJson(cfg));
    QCOMPARE(back.maxAltitudeM, 120.0);
    QCOMPARE(back.commRangeM, 800.0);
    QCOMPARE(back.minSeparationM, 4.0);
    QCOMPARE(back.cruiseSpeedMs, 15.0);
    QCOMPARE(back.enduranceMin, 30.0);
    QCOMPARE(back.reserveFactor, 0.75);
}

QTEST_GUILESS_MAIN(TestP2Modules)
#include "tst_p2modules.moc"
