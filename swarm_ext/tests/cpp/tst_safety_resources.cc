#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include "SwarmCOPModel.h"
#include "SwarmConstraintStore.h"
#include "SwarmEventDetector.h"

class SafetyResourcesTest : public QObject
{
    Q_OBJECT
private slots:
    void coordinates();
    void trackCapacity();
    void sourceCapacityAndExpiry();
    void atomicConfiguration();
    void boundaryAndUnion();
    void eventHistoryAndClustering();
};

void SafetyResourcesTest::coordinates()
{
    SwarmCOPModel model;
    for (const auto& point : QList<QPair<double, double>>{{qQNaN(), 1}, {1, qInf()}, {91, 0}, {0, -181}})
        model.ingestCoverage(point.first, point.second);
    QCOMPARE(model.rejectedCoverageCount(), 4);
    QCOMPARE(model.heatmapCellCount(), 0);
    model.ingestCoverage(0, 0);
    model.ingestCoverage(-90, -180);
    model.ingestCoverage(90, 180);
    QCOMPARE(model.heatmapCellCount(), 3);
}

void SafetyResourcesTest::trackCapacity()
{
    SwarmTrackFusion fusion;
    SwarmTrackFusion::TrackReport report;
    report.confidence = 0.8;
    report.lat = 30;
    for (int i = 0; i < SwarmTrackFusion::MaxTracks; ++i) {
        report.trackId = i;
        report.lon = 120 + i * .001;
        QVERIFY(fusion.ingest(report, 0) > 0);
    }
    report.trackId = 2000;
    report.lon = 125;
    QCOMPARE(fusion.ingest(report, 1), -1);
    QCOMPARE(fusion.allTracks().size(), SwarmTrackFusion::MaxTracks);
    report.trackId = 0;
    report.lon = 120;
    QVERIFY(fusion.ingest(report, 2) > 0);
    QCOMPARE(fusion.capacityRejectedCount(), 1);
    fusion.prune(11000);
    QVERIFY(fusion.allTracks().isEmpty());
    QCOMPARE(fusion.sourceCount(), 0);
}

void SafetyResourcesTest::sourceCapacityAndExpiry()
{
    SwarmTrackFusion fusion;
    SwarmTrackFusion::TrackReport report;
    report.confidence = 0.8;
    report.lat = 30;
    for (int track = 0; track < 128; ++track) {
        report.lon = 120 + track * .001;
        for (int source = 0; source < 64; ++source) {
            report.trackId = track * 64 + source;
            report.sensorId = source;
            QVERIFY(fusion.ingest(report, 0) > 0);
        }
    }
    QCOMPARE(fusion.sourceCount(), SwarmTrackFusion::MaxSources);
    report.trackId = 9000;
    QCOMPARE(fusion.ingest(report, 1), -1);
    report.trackId = 8191;
    report.timeUsec = 2;
    QVERIFY(fusion.ingest(report, 9000) > 0);
    fusion.prune(10001);
    QCOMPARE(fusion.sourceCount(), 1);
    QCOMPARE(fusion.allTracks().size(), 1);
    QCOMPARE(fusion.allTracks().first().sourceTrackIds, QList<int>{8191});
    QCOMPARE(fusion.allTracks().first().sensors, QSet<int>{63});
    fusion.clear();
    QCOMPARE(fusion.sourceCount(), 0);
    QCOMPARE(fusion.capacityRejectedCount(), 0);
}

void SafetyResourcesTest::atomicConfiguration()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fences.json");
    SwarmConstraintStore store(path);
    const auto write = [&](const QByteArray& bytes) {
        QSaveFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        QVERIFY(file.commit());
    };
    write(R"({"max_altitude_m":20,"exclude":[{"polygon":[[30,120],[30,121],[31,120]]}]})");
    QCOMPARE(store.snapshot().maxAltitudeM, 20);
    const auto revision = store.revision();
    write("broken");
    QVERIFY(!store.snapshot().error.isEmpty());
    QCOMPARE(store.lastValid().excludePolygons.size(), 1);
    QVERIFY(store.revision() > revision);
    write(R"({"max_altitude_m":3})");
    QTRY_COMPARE_WITH_TIMEOUT(store.lastValid().maxAltitudeM, 3, 3000);
    QVERIFY(store.error().isEmpty());
    QVERIFY(QFile::remove(path));
    QVERIFY(!store.snapshot().error.isEmpty());
}

void SafetyResourcesTest::boundaryAndUnion()
{
    SwarmConstraintChecker::Config config;
    config.includePolygons = {{{0, 0}, {0, 2}, {2, 2}, {2, 0}}, {{0, 1}, {0, 3}, {2, 3}, {2, 1}}};
    QVERIFY(SwarmConstraintChecker::segmentAllowed({0, 0}, {0, 3}, config));
    QVERIFY(SwarmConstraintChecker::segmentAllowed({1, .5}, {1, 2.5}, config));
    config.excludePolygons = {{{.5, 1}, {.5, 2}, {1.5, 2}, {1.5, 1}}};
    QVERIFY(!SwarmConstraintChecker::segmentAllowed({1, .5}, {1, 2.5}, config));
    QVERIFY(!SwarmConstraintChecker::segmentAllowed({.5, 1}, {.5, 2}, config));
    config.excludePolygons.clear();
    config.includePolygons[1] = {{0, 3}, {0, 4}, {2, 4}, {2, 3}};
    QVERIFY(!SwarmConstraintChecker::segmentAllowed({1, 1}, {1, 3.5}, config));
}

void SafetyResourcesTest::eventHistoryAndClustering()
{
    SwarmEventDetector detector;
    detector.setFences({{{30, 120}, {30, 121}, {31, 121}, {31, 120}}}, {});
    SwarmTrackFusion::FusedTrack track;
    track.lat = 30;
    track.lon = 120;
    track.state = "confirmed";
    QVERIFY(detector.checkTracks({track}, 0).isEmpty());
    track.lon = 119;
    QCOMPARE(detector.checkTracks({track}, 1).size(), 1);
    QVERIFY(detector.checkTracks({track}, 2).isEmpty());
    detector.checkTracks({}, 3);
    QCOMPARE(detector.cooldownRecordCount(), 0);
    QCOMPARE(detector.speedHistoryCount(), 0);
    detector.setFences({}, {});
    QList<SwarmTrackFusion::FusedTrack> tracks;
    for (int i = 0; i < 1000; ++i) {
        track.fusedId = i;
        track.lat = 89.99;
        track.lon = 179.999 + i * .000001;
        tracks.append(track);
    }
    const auto alerts = detector.checkTracks(tracks, 4);
    QVERIFY(!alerts.isEmpty());
    QCOMPARE(alerts.first().rule, "gathering");
    QCOMPARE(alerts.first().relatedIds.size(), 1000);
}

QTEST_GUILESS_MAIN(SafetyResourcesTest)
#include "tst_safety_resources.moc"
