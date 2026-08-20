#include <QtTest/QtTest>
#include "SwarmControlLease.h"

class TstLease : public QObject { Q_OBJECT
private slots:
    void grantsAndAutomates();
    void flightModeOverrideReleases();
    void expiresAfterTtl();
    void perVehicleIndependent();
    void operatorModeClassification();
    void manualOverrideRecovers();
};

void TstLease::grantsAndAutomates() {
    SwarmControlLease l;
    QVERIFY(!l.canAutomate(1, 1000));
    const QString tok = l.requestLease(1, "t1", "alice", 1000);
    QVERIFY(!tok.isEmpty());
    QCOMPARE(l.stateOf(1), SwarmControlLease::LEASED);
    QVERIFY(l.canAutomate(1, 1000));
}
void TstLease::flightModeOverrideReleases() {
    SwarmControlLease l;
    l.requestLease(1, "t1", "alice", 1000);
    l.onFlightModeChanged(1, "Position");                   // 操作员手动直控 → 接管
    QCOMPARE(l.stateOf(1), SwarmControlLease::MANUAL_OVERRIDE);
    QVERIFY(!l.canAutomate(1, 1000));
}
void TstLease::expiresAfterTtl() {
    SwarmControlLease l;
    l.requestLease(1, "t1", "alice", 1000, 5000);
    QVERIFY(l.canAutomate(1, 4000));
    l.expireCheck(7000);
    QVERIFY(!l.canAutomate(1, 7000));
    QCOMPARE(l.stateOf(1), SwarmControlLease::RELEASED);
}
void TstLease::perVehicleIndependent() {
    SwarmControlLease l;
    l.requestLease(1, "t", "a", 1000);
    l.requestLease(2, "t", "a", 1000);
    l.onFlightModeChanged(1, "Position");                   // 只接管 1 号(操作员直控)
    QVERIFY(!l.canAutomate(1, 1000));
    QVERIFY( l.canAutomate(2, 1000));
}
void TstLease::operatorModeClassification() {
    QVERIFY( SwarmControlLease::isOperatorMode("Position"));     // 操作员直控
    QVERIFY( SwarmControlLease::isOperatorMode("Manual"));
    QVERIFY( SwarmControlLease::isOperatorMode("完全未知模式")); // 未知 → 保守接管
    QVERIFY(!SwarmControlLease::isOperatorMode("Mission"));      // 自主
    QVERIFY(!SwarmControlLease::isOperatorMode("Offboard"));
    QVERIFY(!SwarmControlLease::isOperatorMode("Hold"));         // 自主(修复:不再误判接管)
    QVERIFY(!SwarmControlLease::isOperatorMode("RTL"));
    QVERIFY(!SwarmControlLease::isOperatorMode("任务"));         // 中文 Mission → 自主(i18n 修复)
}

void TstLease::manualOverrideRecovers() {
    SwarmControlLease l;
    l.requestLease(1, "t", "a", 1000);
    l.setManualOverride(1, true);                           // 操作员接管
    QCOMPARE(l.stateOf(1), SwarmControlLease::MANUAL_OVERRIDE);
    QVERIFY(!l.canAutomate(1, 1000));
    l.setManualOverride(1, false);                          // 切回自主 → 解除接管
    QCOMPARE(l.stateOf(1), SwarmControlLease::OBSERVE_ONLY);
    l.requestLease(1, "t2", "a", 2000);                     // 解除后可重新申请
    QVERIFY(l.canAutomate(1, 2000));
}

QTEST_GUILESS_MAIN(TstLease)
#include "tst_lease.moc"
