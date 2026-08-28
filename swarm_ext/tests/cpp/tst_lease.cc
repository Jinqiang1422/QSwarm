#include <QtTest/QtTest>

#include "SwarmControlLease.h"

class TstLease : public QObject
{
    Q_OBJECT
private slots:
    void grantsAndAutomates();
    void flightModeOverrideReleases();
    void expiresAfterTtl();
    void perVehicleIndependent();
    void autonomousModeClassification();
    void manualOverrideRecovers();
    void manualOverrideCannotBeClobbered();
    void activeLeaseCanRenew();
    void disconnectedVehicleIsForgotten();
    void modeAwareRequestRejectsUnconfirmedTakeover();
    void operatorAuthorizedRequestCanTakeInitialControl();
};

void TstLease::grantsAndAutomates()
{
    SwarmControlLease l;
    QVERIFY(!l.canAutomate(1, 1000));
    const QString tok = l.requestLease(1, "t1", "alice", 1000);
    QVERIFY(!tok.isEmpty());
    QCOMPARE(l.stateOf(1), SwarmControlLease::LEASED);
    QVERIFY(l.canAutomate(1, 1000));
}

void TstLease::flightModeOverrideReleases()
{
    SwarmControlLease l;
    l.requestLease(1, "t1", "alice", 1000);
    l.setManualOverride(1, true);  // 操作员手动直控 → 接管
    QCOMPARE(l.stateOf(1), SwarmControlLease::MANUAL_OVERRIDE);
    QVERIFY(!l.canAutomate(1, 1000));
}

void TstLease::expiresAfterTtl()
{
    SwarmControlLease l;
    l.requestLease(1, "t1", "alice", 1000, 5000);
    QVERIFY(l.canAutomate(1, 4000));
    QVERIFY(!l.canAutomate(1, 7000));  // 派发闸门无需先跑 expireCheck 也会拒绝
    l.expireCheck(7000);
    QVERIFY(!l.canAutomate(1, 7000));
    QCOMPARE(l.stateOf(1), SwarmControlLease::RELEASED);
}

void TstLease::perVehicleIndependent()
{
    SwarmControlLease l;
    l.requestLease(1, "t", "a", 1000);
    l.requestLease(2, "t", "a", 1000);
    l.setManualOverride(1, true);  // 只接管 1 号(操作员直控)
    QVERIFY(!l.canAutomate(1, 1000));
    QVERIFY(l.canAutomate(2, 1000));
}

void TstLease::autonomousModeClassification()
{
    // 模拟 Vehicle 规范访问器返回的实际 zh_CN 译名(取自 qgc_source_zh_CN.ts),验证 locale 安全:
    const QStringList autos = {
        QStringLiteral("任务"),
        QStringLiteral("等待"),
        QStringLiteral("返航Return"),
        QStringLiteral("降落"),
    };
    QVERIFY(SwarmControlLease::isAutonomousMode("任务", autos));           // Mission(中文)→自主
    QVERIFY(SwarmControlLease::isAutonomousMode("等待", autos));           // Hold(中文,pauseAll 场景)→自主
    QVERIFY(SwarmControlLease::isAutonomousMode("返航Return", autos));     // RTL(中英串接)→自主
    QVERIFY(SwarmControlLease::isAutonomousMode("降落", autos));
    QVERIFY(!SwarmControlLease::isAutonomousMode("定点Position", autos));  // 操作员定点接管 → 让路
    QVERIFY(!SwarmControlLease::isAutonomousMode("板外Offboard", autos));  // 不在自主集 → 让路
    QVERIFY(!SwarmControlLease::isAutonomousMode(QString(), autos));       // 空 → 非自主
}

void TstLease::manualOverrideRecovers()
{
    SwarmControlLease l;
    l.requestLease(1, "t", "a", 1000);
    l.setManualOverride(1, true);  // 操作员接管
    QCOMPARE(l.stateOf(1), SwarmControlLease::MANUAL_OVERRIDE);
    QVERIFY(!l.canAutomate(1, 1000));
    l.setManualOverride(1, false);       // 切回自主 → 解除接管
    QCOMPARE(l.stateOf(1), SwarmControlLease::OBSERVE_ONLY);
    l.requestLease(1, "t2", "a", 2000);  // 解除后可重新申请
    QVERIFY(l.canAutomate(1, 2000));
}

void TstLease::manualOverrideCannotBeClobbered()
{
    SwarmControlLease l;
    l.setManualOverride(1, true);
    QVERIFY(l.requestLease(1, "t", "automation", 1000).isEmpty());
    QVERIFY(!l.renewLease(1, 1000));
    l.release(1);
    QCOMPARE(l.stateOf(1), SwarmControlLease::MANUAL_OVERRIDE);
}

void TstLease::activeLeaseCanRenew()
{
    SwarmControlLease l;
    l.requestLease(1, "t", "automation", 1000, 5000);
    QVERIFY(l.renewLease(1, 4000, 5000));
    QVERIFY(l.canAutomate(1, 8000));
    QVERIFY(!l.renewLease(1, 10000, 5000));
}

void TstLease::disconnectedVehicleIsForgotten()
{
    SwarmControlLease l;
    l.setManualOverride(1, true);
    l.forgetVehicle(1);
    QCOMPARE(l.stateOf(1), SwarmControlLease::OBSERVE_ONLY);
    QVERIFY(!l.requestLease(1, "reconnected", "automation", 2000).isEmpty());
}

void TstLease::modeAwareRequestRejectsUnconfirmedTakeover()
{
    SwarmControlLease l;
    const QStringList autonomousModes = {QStringLiteral("Mission"), QStringLiteral("Hold")};

    QVERIFY(l.requestLeaseForMode(1, "ai", "swarm", 1000, "Position", autonomousModes, false).isEmpty());
    QCOMPARE(l.stateOf(1), SwarmControlLease::MANUAL_OVERRIDE);
    QVERIFY(!l.canAutomate(1, 1000));
}

void TstLease::operatorAuthorizedRequestCanTakeInitialControl()
{
    SwarmControlLease l;
    const QStringList autonomousModes = {QStringLiteral("Mission"), QStringLiteral("Hold")};

    QVERIFY(!l.requestLeaseForMode(1, "operator", "swarm", 1000, "Position", autonomousModes, true).isEmpty());
    QVERIFY(l.canAutomate(1, 1000));

    SwarmControlLease autonomousLease;
    QVERIFY(!autonomousLease.requestLeaseForMode(2, "ai", "swarm", 1000, "Hold", autonomousModes, false).isEmpty());
    QVERIFY(autonomousLease.canAutomate(2, 1000));
}

QTEST_GUILESS_MAIN(TstLease)
#include "tst_lease.moc"
