/****************************************************************************
 *
 * COP 模型与 UDP 感知桥端到端测试。
 *
 ****************************************************************************/

#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QUdpSocket>
#include <QtTest/QSignalSpy>
#include <QtTest/QtTest>
#include <algorithm>

#include "SwarmCOPModel.h"
#include "SwarmPerceptionBridge.h"

class tst_cop : public QObject
{
    Q_OBJECT

private slots:
    void modelExportsStableTrackContract();
    void coverageGridIsBounded();
    void perceptionBridgeReceivesUdpTrackBatch();
};

void tst_cop::modelExportsStableTrackContract()
{
    SwarmTrackFusion::FusedTrack track;
    track.fusedId = 17;
    track.category = 2;
    track.lat = 39.9005;
    track.lon = 116.4005;
    track.ve = 3.0;
    track.vn = 4.0;
    track.confidence = 0.91;
    track.sensors = {1, 2};
    track.lastSeenMs = 900;
    track.state = QStringLiteral("confirmed");

    SwarmCOPModel model;
    QSignalSpy changedSpy(&model, &SwarmCOPModel::tracksChanged);
    model.updateFromFusion({track}, 1000);

    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(model.trackCount(), 1);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0), SwarmCOPModel::CategoryRole).toInt(), 2);
    QCOMPARE(model.data(model.index(0), SwarmCOPModel::SensorsRole).toInt(), 2);

    const QVariantMap exported = model.trackAt(0);
    QCOMPARE(exported.value(QStringLiteral("fusedId")).toInt(), 17);
    QCOMPARE(exported.value(QStringLiteral("category")).toString(), QStringLiteral("车辆"));
    QCOMPARE(exported.value(QStringLiteral("speed")).toDouble(), 5.0);
    QVERIFY(model.trackAt(1).isEmpty());
}

void tst_cop::coverageGridIsBounded()
{
    SwarmCOPModel model;
    model.ingestCoverage(39.9000, 116.4000, 2);
    model.ingestCoverage(39.9000, 116.4000, 2);
    model.ingestCoverage(39.9010, 116.4010, 2);
    model.ingestCoverage(39.9020, 116.4020, 2);

    QCOMPARE(model.heatmapCellCount(), 2);
    const QVariantList cells = model.heatmap();
    QVERIFY(std::any_of(cells.cbegin(), cells.cend(), [](const QVariant& cell) {
        return cell.toMap().value(QStringLiteral("weight")).toInt() == 2;
    }));

    model.clearHeatmap();
    QCOMPARE(model.heatmapCellCount(), 0);
}

void tst_cop::perceptionBridgeReceivesUdpTrackBatch()
{
    QUdpSocket portProbe;
    QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
    const quint16 port = portProbe.localPort();
    portProbe.close();

    SwarmPerceptionBridge bridge;
    QSignalSpy trackSpy(&bridge, &SwarmPerceptionBridge::trackBatchReceived);
    QVERIFY(bridge.listen(port));

    const qint64 sentUsec = QDateTime::currentMSecsSinceEpoch() * 1000;
    const QJsonArray wireTrack{7, 2, 399005000, 1164005000, -1, 150, -50, 870, 1, 0, double(sentUsec)};
    const QJsonObject payload{
        {QStringLiteral("kind"), QStringLiteral("track_batch")},
        {QStringLiteral("data"),
         QJsonObject{{QStringLiteral("t"), double(sentUsec)}, {QStringLiteral("tracks"), QJsonArray{wireTrack}}}},
    };
    QByteArray datagram;
    datagram.append(char(SwarmTunnel::PT_SINGLE >> 8));
    datagram.append(char(SwarmTunnel::PT_SINGLE & 0xFF));
    datagram.append(char(0));
    datagram.append(QJsonDocument(payload).toJson(QJsonDocument::Compact));

    QUdpSocket sender;
    QCOMPARE(sender.writeDatagram(datagram, QHostAddress::LocalHost, port), qint64(datagram.size()));
    QVERIFY(trackSpy.wait(1000));
    QCOMPARE(bridge.lastTrackCount(), 1);
    QVERIFY(bridge.wireLatencyMs() >= 0.0);
    QVERIFY(bridge.wireLatencyMs() < 1000.0);

    const QVariantList received = trackSpy.takeFirst().at(0).toList();
    QCOMPARE(received.count(), 1);
    QCOMPARE(received.first().toMap().value(QStringLiteral("category")).toInt(), 2);
}

QTEST_GUILESS_MAIN(tst_cop)
#include "tst_cop.moc"
