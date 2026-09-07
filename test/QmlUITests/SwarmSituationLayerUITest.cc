#include "SwarmSituationLayerUITest.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QScopeGuard>

#include "SwarmAcceptanceServer.h"
#include "SwarmCOPModel.h"
#include "SwarmManager.h"
#include "SwarmTrackFusion.h"

UT_REGISTER_TEST(SwarmSituationLayerUITest, TestLabel::Integration)

void SwarmSituationLayerUITest::cleanup()
{
    SwarmManager* manager = SwarmManager::instance();
    manager->trackFusion().clear();
    manager->copModel()->updateFromFusion({}, 0);
    QmlUITestBase::cleanup();
}

void SwarmSituationLayerUITest::_testDelegatesFollowCOPModel()
{
    startUI();
    if (QTest::currentTestFailed()) {
        return;
    }
    const auto uiGuard = qScopeGuard([this] { stopUI(); });

    SwarmManager* manager = SwarmManager::instance();
    SwarmAcceptanceServer acceptanceServer(manager);
    QJsonObject rendered;
    const bool situationLayerReady = UnitTest::waitForCondition(
        [&]() {
            rendered = acceptanceServer._renderedItemCounts();
            return rendered.value(QStringLiteral("situation_layers")).toInt() == 1;
        },
        TestTimeout::longMs(), QStringLiteral("single swarm situation layer to enter the visual tree"));
    QVERIFY2(situationLayerReady,
             qPrintable(QStringLiteral("Expected one situation layer, observed %1")
                            .arg(rendered.value(QStringLiteral("situation_layers")).toInt())));

    manager->trackFusion().clear();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (int index = 0; index < 3; ++index) {
        SwarmTrackFusion::TrackReport report;
        report.trackId = index + 1;
        report.sensorId = index + 1;
        report.category = index + 1;
        report.lat = 1.0 + index * 10.0;
        report.lon = 2.0 + index * 20.0;
        report.confidence = 0.9;
        report.timeUsec = nowMs * 1000;
        QCOMPARE(manager->trackFusion().ingest(report, nowMs), index + 1);
        QCOMPARE(manager->trackFusion().ingest(report, nowMs + 1), index + 1);
    }

    QCOMPARE(manager->trackFusion().tracks().count(), 3);
    const bool allTracksRendered = UnitTest::waitForCondition(
        [&]() {
            rendered = acceptanceServer._renderedItemCounts();
            return rendered.value(QStringLiteral("tracks")).toInt() == 3;
        },
        TestTimeout::longMs(), QStringLiteral("all COP track delegates to enter the visual tree"));
    QVERIFY2(allTracksRendered,
             qPrintable(QStringLiteral("Expected %1 rendered tracks, observed %2 (situation layers: %3)")
                            .arg(3)
                            .arg(rendered.value(QStringLiteral("tracks")).toInt())
                            .arg(rendered.value(QStringLiteral("situation_layers")).toInt())));
    QCOMPARE(rendered.value(QStringLiteral("situation_layers")).toInt(), 1);

    manager->trackFusion().clear();
    manager->copModel()->updateFromFusion({}, 2000);
    QVERIFY(UnitTest::waitForCondition(
        [&]() {
            rendered = acceptanceServer._renderedItemCounts();
            return rendered.value(QStringLiteral("tracks")).toInt() == 0;
        },
        TestTimeout::mediumMs(), QStringLiteral("COP track delegates to leave the visual tree")));
}
