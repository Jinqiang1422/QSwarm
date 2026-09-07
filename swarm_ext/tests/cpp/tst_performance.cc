#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtMath>
#include <QtTest/QtTest>
#include <algorithm>

#include "SwarmAuditLogger.h"
#include "SwarmCOPModel.h"
#include "SwarmTrackFusion.h"

class tst_performance : public QObject
{
    Q_OBJECT

private slots:
    void auditBurstEquivalentToThirtyMinutes();
    void fusionAndCOPAtScale();
};

namespace {
qint64 percentile(QList<qint64> values, double quantile)
{
    if (values.isEmpty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const qsizetype candidate = static_cast<qsizetype>(qCeil(quantile * values.size())) - 1;
    const qsizetype index = qBound(qsizetype(0), candidate, values.size() - 1);
    return values.at(index);
}

SwarmTrackFusion::TrackReport trackReport(int trackId, int frame)
{
    constexpr double baseLatitude = 39.9;
    constexpr double baseLongitude = 116.4;
    constexpr double spacingDegrees = 0.00032;
    SwarmTrackFusion::TrackReport report;
    report.trackId = trackId;
    report.sensorId = 1;
    report.category = 2;
    report.lat = baseLatitude + (trackId / 40) * spacingDegrees;
    report.lon = baseLongitude + (trackId % 40) * spacingDegrees;
    report.ve = 0.1 * (frame % 3);
    report.vn = 0.0;
    report.confidence = 0.9;
    return report;
}
}  // namespace

void tst_performance::auditBurstEquivalentToThirtyMinutes()
{
    constexpr int eventsPerSecond = 100;
    constexpr int durationSeconds = 30 * 60;
    constexpr int eventCount = eventsPerSecond * durationSeconds;
    QTemporaryDir temporaryDirectory;
    QVERIFY(temporaryDirectory.isValid());

    SwarmAuditLogger* audit = SwarmAuditLogger::instance();
    audit->setAuditDirForTesting(temporaryDirectory.path());
    QList<qint64> latencyUsec;
    latencyUsec.reserve(eventCount);
    QElapsedTimer totalTimer;
    totalTimer.start();
    for (int index = 0; index < eventCount; ++index) {
        QElapsedTimer eventTimer;
        eventTimer.start();
        audit->logTaskEvent(QStringLiteral("performance_sample"), {{QStringLiteral("index"), index}});
        latencyUsec.append(eventTimer.nsecsElapsed() / 1000);
    }
    const qint64 totalMs = totalTimer.elapsed();
    const qint64 p99Usec = percentile(latencyUsec, 0.99);
    QCOMPARE(audit->eventCount(), eventCount);
    QVERIFY2(p99Usec <= 50 * 1000, qPrintable(QStringLiteral("audit p99 %1us exceeds 50ms").arg(p99Usec)));

    QFile auditFile(audit->sessionFilePath());
    QVERIFY(auditFile.open(QIODevice::ReadOnly | QIODevice::Text));
    int lines = 0;
    while (!auditFile.atEnd()) {
        if (!auditFile.readLine().isEmpty()) {
            ++lines;
        }
    }
    QCOMPARE(lines, eventCount);
    qInfo().noquote() << QStringLiteral("PERF audit events=%1 total_ms=%2 p99_us=%3 bytes=%4")
                             .arg(eventCount)
                             .arg(totalMs)
                             .arg(p99Usec)
                             .arg(auditFile.size());
}

void tst_performance::fusionAndCOPAtScale()
{
    constexpr int trackCount = 1000;
    constexpr int frames = 30;
    SwarmTrackFusion fusion;
    SwarmCOPModel model;
    QList<qint64> batchLatencyUsec;
    batchLatencyUsec.reserve(frames);

    for (int frame = 0; frame < frames; ++frame) {
        QElapsedTimer batchTimer;
        batchTimer.start();
        for (int trackId = 0; trackId < trackCount; ++trackId) {
            fusion.ingest(trackReport(trackId, frame), frame * 33);
        }
        model.updateFromFusion(fusion.tracks(), frame * 33);
        batchLatencyUsec.append(batchTimer.nsecsElapsed() / 1000);
    }

    QCOMPARE(fusion.allTracks().size(), trackCount);
    QCOMPARE(model.rowCount(), trackCount);
    const qint64 p50Usec = percentile(batchLatencyUsec, 0.50);
    const qint64 p95Usec = percentile(batchLatencyUsec, 0.95);
    const qint64 p99Usec = percentile(batchLatencyUsec, 0.99);
    QVERIFY2(p95Usec <= 200 * 1000,
             qPrintable(QStringLiteral("1000-track fusion/COP p95 %1us exceeds 200ms").arg(p95Usec)));
    qInfo().noquote() << QStringLiteral("PERF fusion tracks=%1 frames=%2 p50_us=%3 p95_us=%4 p99_us=%5")
                             .arg(trackCount)
                             .arg(frames)
                             .arg(p50Usec)
                             .arg(p95Usec)
                             .arg(p99Usec);
}

QTEST_GUILESS_MAIN(tst_performance)
#include "tst_performance.moc"
