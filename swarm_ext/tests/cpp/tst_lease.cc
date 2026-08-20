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
    QVERIFY(!l.canAutomate(1, 1000));
    const QString tok = l.requestLease(1, "t1", "alice", 1000);
    QVERIFY(!tok.isEmpty());
    QCOMPARE(l.stateOf(1), SwarmControlLease::LEASED);
    QVERIFY(l.canAutomate(1, 1000));
}
void TstLease::flightModeOverrideReleases() {
    SwarmControlLease l;
    l.requestLease(1, "t1", "alice", 1000);
    l.onFlightModeChanged(1, "Hold");
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
    l.onFlightModeChanged(1, "RTL");
    QVERIFY(!l.canAutomate(1, 1000));
    QVERIFY( l.canAutomate(2, 1000));
}
void TstLease::operatorModeClassification() {
    QVERIFY( SwarmControlLease::isOperatorMode("Position"));
    QVERIFY( SwarmControlLease::isOperatorMode("Hold"));
    QVERIFY( SwarmControlLease::isOperatorMode("Manual"));
    QVERIFY( SwarmControlLease::isOperatorMode("完全未知模式"));
    QVERIFY(!SwarmControlLease::isOperatorMode("Mission"));
    QVERIFY(!SwarmControlLease::isOperatorMode("Offboard"));
}

QTEST_GUILESS_MAIN(TstLease)
#include "tst_lease.moc"
