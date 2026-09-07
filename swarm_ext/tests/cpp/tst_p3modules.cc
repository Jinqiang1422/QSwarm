/****************************************************************************
 *
 * P3 模块独立单元测试（WP3.1/3.3/3.5/3.6/3.7）
 * 仅依赖 Qt6 Core/Test，不依赖 QGC 主工程（SwarmCOPModel 需 Qt Positioning，
 * 故本用例覆盖纯 QtCore 引擎；COPModel 经 QML 集成验证）。
 *
 * 覆盖：
 *   fusion_crossSensorMerge   双传感器同目标融合为一航迹
 *   fusion_separateTargets    远距双目标各自成轨
 *   fusion_lifecycle          tentative→confirmed→coasting→删除
 *   videoHub_pixelToGeo       像素→地理（与 Python 端同公式互验）
 *   videoHub_timestamp        统一时基换算
 *   event_batteryOffline      低电分级 + 离线跳变 + 冷却去抖
 *   event_gatheringSpeed      聚集 + 速度突变
 *   digest_structure          态势摘要来源标注与一行摘要
 *   bridge_wireDecode         TUNNEL JSON 线格式解码（单帧/分片/丢首帧）
 *
 ****************************************************************************/

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QtTest>

#include "SwarmEventDetector.h"
#include "SwarmSituationDigest.h"
#include "SwarmTrackFusion.h"
#include "SwarmTunnelProtocol.h"
#include "SwarmTypes.h"
#include "SwarmVideoHub.h"

class tst_p3modules : public QObject
{
    Q_OBJECT

private slots:
    void fusion_crossSensorMerge();
    void fusion_separateTargets();
    void fusion_stableSourceIdentity();
    void fusion_lifecycle();
    void fusion_rejectsInvalidLatLon();
    void fusion_dropsLowConfidence();
    void videoHub_pixelToGeo();
    void videoHub_timestamp();
    void event_batteryOffline();
    void event_gatheringSpeed();
    void digest_structure();
    void bridge_wireDecode();
};

// ---------------------------------------------------------------------------
static SwarmTrackFusion::TrackReport makeReport(int trackId, int sensorId, double lat, double lon, double ve = 0.0,
                                                double vn = 0.0, double conf = 0.8, int category = 2)
{
    SwarmTrackFusion::TrackReport r;
    r.trackId = trackId;
    r.sensorId = sensorId;
    r.category = category;
    r.lat = lat;
    r.lon = lon;
    r.ve = ve;
    r.vn = vn;
    r.confidence = conf;
    return r;
}

void tst_p3modules::fusion_crossSensorMerge()
{
    SwarmTrackFusion fusion;
    // 同一物理目标：传感器 1 报 (39.90000, 116.40000)，传感器 2 报偏 10 米
    fusion.ingest(makeReport(101, 1, 39.90000, 116.40000), 1000);
    const int id2 = fusion.ingest(makeReport(205, 2, 39.90005, 116.40008), 1100);
    const int id3 = fusion.ingest(makeReport(101, 1, 39.90002, 116.40003), 1200);
    QCOMPARE(fusion.allTracks().count(), 1);  // 未分裂
    QCOMPARE(id2, id3);
    const auto t = fusion.allTracks().first();
    QCOMPARE(t.sensors.count(), 2);  // 双源贡献
    QVERIFY(t.confidence > 0.8);     // 多源置信度提升
}

void tst_p3modules::fusion_separateTargets()
{
    SwarmTrackFusion fusion;
    fusion.ingest(makeReport(1, 1, 39.90000, 116.40000), 1000);
    fusion.ingest(makeReport(2, 1, 39.90200, 116.40000), 1000);  // 相距 ~220m
    QCOMPARE(fusion.allTracks().count(), 2);
}

void tst_p3modules::fusion_stableSourceIdentity()
{
    SwarmTrackFusion fusion;
    const int firstId = fusion.ingest(makeReport(1, 1, 39.90000, 116.40000), 1000);
    const int secondId = fusion.ingest(makeReport(2, 1, 39.90200, 116.40000), 1000);
    QVERIFY(firstId != secondId);

    // A stable upstream identity must not jump to a nearby fused track when targets cross.
    const int updatedId = fusion.ingest(makeReport(1, 1, 39.90199, 116.40000), 1100);
    QCOMPARE(updatedId, firstId);
    QCOMPARE(fusion.allTracks().count(), 2);
}

void tst_p3modules::fusion_lifecycle()
{
    SwarmTrackFusion::Config cfg;
    cfg.minHits = 2;
    cfg.coastMs = 1000;
    cfg.dropMs = 3000;
    SwarmTrackFusion fusion(cfg);
    fusion.ingest(makeReport(1, 1, 39.9, 116.4), 0);
    QCOMPARE(fusion.allTracks().first().state, QStringLiteral("tentative"));
    QCOMPARE(fusion.tracks().count(), 0);  // tentative 不上清单
    fusion.ingest(makeReport(1, 1, 39.9, 116.4), 100);
    QCOMPARE(fusion.allTracks().first().state, QStringLiteral("confirmed"));
    QCOMPARE(fusion.tracks().count(), 1);
    fusion.prune(2000);  // 超 coastMs
    QCOMPARE(fusion.allTracks().first().state, QStringLiteral("coasting"));
    fusion.prune(5000);  // 超 dropMs
    QCOMPARE(fusion.allTracks().count(), 0);
}

void tst_p3modules::videoHub_pixelToGeo()
{
    // 中心像素：视轴交点 = 正北 alt/tan(55°) ≈ 56.03m（heading=0）
    double lat, lon;
    QVERIFY(SwarmVideoHub::pixelToGeo(640, 360, 39.9, 116.4, 80, 0, 55, 60, 1280, 720, lat, lon));
    const double expectRange = 80.0 / std::tan(55.0 * M_PI / 180.0);
    double elat, elon;
    SwarmTypes::destinationPoint(39.9, 116.4, 0.0, expectRange, elat, elon);
    QVERIFY(qAbs(lat - elat) < 1e-7 && qAbs(lon - elon) < 1e-7);
    // 朝天拒绝
    double d1, d2;
    QVERIFY(!SwarmVideoHub::pixelToGeo(640, 100, 39.9, 116.4, 80, 0, 5, 60, 1280, 720, d1, d2));
    // 脚印四角均可落地（俯视 55°）
    QCOMPARE(SwarmVideoHub::footprint(39.9, 116.4, 80, 0, 55, 60, 1280, 720).count(), 4);
}

void tst_p3modules::videoHub_timestamp()
{
    SwarmVideoHub hub;
    const qint64 u1 = hub.noteFrame(1, 500000, 10000);  // 首帧标定
    QCOMPARE(u1, 10000 * 1000);
    const qint64 u2 = hub.noteFrame(1, 700000, 10200);  // +0.2s 流内
    QCOMPARE(u2, 10000 * 1000 + 200000);
}

void tst_p3modules::event_batteryOffline()
{
    SwarmEventDetector det;
    SwarmTypes::VehicleState v;
    v.id = 3;
    v.online = true;
    v.batteryPct = 15.0;  // critical
    auto alerts = det.checkVehicles({v}, 1000);
    QCOMPARE(alerts.count(), 1);
    QCOMPARE(alerts.first().level, QStringLiteral("critical"));
    QCOMPARE(alerts.first().rule, QStringLiteral("low_battery"));

    v.batteryPct = 25.0;  // warning
    alerts = det.checkVehicles({v}, 2000);
    QCOMPARE(alerts.count(), 1);
    QCOMPARE(alerts.first().level, QStringLiteral("warning"));

    alerts = det.checkVehicles({v}, 3000);  // 冷却期不再报
    QCOMPARE(alerts.count(), 0);

    v.online = false;  // 离线跳变
    alerts = det.checkVehicles({v}, 4000);
    QCOMPARE(alerts.count(), 1);
    QCOMPARE(alerts.first().rule, QStringLiteral("offline"));
}

void tst_p3modules::event_gatheringSpeed()
{
    SwarmEventDetector det;
    QList<SwarmTrackFusion::FusedTrack> tracks;
    for (int i = 0; i < 3; i++) {  // 三目标 ~20m 内
        SwarmTrackFusion::FusedTrack t;
        t.fusedId = i + 1;
        t.category = 2;
        t.lat = 39.9 + i * 0.0001;
        t.lon = 116.4;
        t.ve = 1.0;
        t.vn = 0.0;
        tracks.append(t);
    }
    auto alerts = det.checkTracks(tracks, 1000);
    QCOMPARE(alerts.count(), 1);
    QCOMPARE(alerts.first().rule, QStringLiteral("gathering"));

    // 速度突变：同一目标速度 1 → 12 m/s
    tracks.first().ve = 12.0;
    alerts = det.checkTracks(tracks, 2000);
    bool hasBurst = false;
    for (const auto& a : alerts) {
        if (a.rule == QStringLiteral("speed_burst")) {
            hasBurst = true;
        }
    }
    QVERIFY(hasBurst);
}

void tst_p3modules::digest_structure()
{
    SwarmTypes::VehicleState v;
    v.id = 1;
    v.online = true;
    v.batteryPct = 72.0;
    v.altRelM = 30.0;
    SwarmTrackFusion::FusedTrack t;
    t.fusedId = 5;
    t.category = 2;
    t.lat = 39.901;
    t.lon = 116.401;
    t.ve = 2.0;
    t.vn = 0.0;
    t.confidence = 0.85;
    t.sensors.insert(1);
    t.state = QStringLiteral("confirmed");
    SwarmEventDetector::Alert a{
        QStringLiteral("warning"), QStringLiteral("gathering"), QStringLiteral("3 目标聚集"), {1, 2, 3}, 0};

    const QJsonObject d = SwarmSituationDigest::build({v}, {t}, QStringLiteral("区域搜索中"), true, {a}, 12345);
    QCOMPARE(d.value(QStringLiteral("fleet")).toObject().value(QStringLiteral("source")).toString(),
             QStringLiteral("telemetry"));
    QCOMPARE(d.value(QStringLiteral("tracks")).toObject().value(QStringLiteral("source")).toString(),
             QStringLiteral("perception"));
    QCOMPARE(d.value(QStringLiteral("alerts")).toObject().value(QStringLiteral("source")).toString(),
             QStringLiteral("inferred"));
    QCOMPARE(d.value(QStringLiteral("tracks")).toObject().value(QStringLiteral("count")).toInt(), 1);

    const QString line = SwarmSituationDigest::oneLiner(d);
    QVERIFY(line.contains(QStringLiteral("在线 1/1")));
    QVERIFY(line.contains(QStringLiteral("目标 1 个")));
    QVERIFY(line.contains(QStringLiteral("告警 1")));
    // 来源角标
    QCOMPARE(SwarmSituationDigest::sourceLabel(SwarmSituationDigest::Source::Perception), QStringLiteral("[感知]"));
}

void tst_p3modules::bridge_wireDecode()
{
    // 与 Python swarm_msgs 同一字节布局：[pt:2B BE][seq:1B][JSON]
    const QByteArray json = R"({"kind":"track","data":[7,2,399051234,1164054321,-1,150,-50,870,1,0,123456]})";
    QByteArray dg;
    dg.append(char(42000 >> 8)).append(char(42000 & 0xFF)).append(char(0)).append(json);

    SwarmTunnel::Reassembly rx;
    const QJsonObject msg = SwarmTunnel::parseDatagram(dg, rx);
    QCOMPARE(msg.value(QStringLiteral("kind")).toString(), QStringLiteral("track"));
    const QJsonObject t = msg.value(QStringLiteral("data")).toObject();
    QCOMPARE(t.value(QStringLiteral("track_id")).toInt(), 7);
    QVERIFY(qAbs(t.value(QStringLiteral("lat")).toDouble() - 39.9051234) < 1e-7);
    QVERIFY(qAbs(t.value(QStringLiteral("ve")).toDouble() - 1.5) < 1e-6);
    QVERIFY(qAbs(t.value(QStringLiteral("confidence")).toDouble() - 0.87) < 1e-6);

    // 分片：同一 JSON 切两半，MIDDLE + LAST
    const int half = json.size() / 2;
    QByteArray dg1, dg2;
    dg1.append(char(42001 >> 8)).append(char(42001 & 0xFF)).append(char(1)).append(json.left(half));
    dg2.append(char(42002 >> 8)).append(char(42002 & 0xFF)).append(char(1)).append(json.mid(half));
    SwarmTunnel::Reassembly rx2;
    QVERIFY(SwarmTunnel::parseDatagram(dg1, rx2).isEmpty());  // 未完
    const QJsonObject msg2 = SwarmTunnel::parseDatagram(dg2, rx2);
    QCOMPARE(msg2.value(QStringLiteral("kind")).toString(), QStringLiteral("track"));

    // 丢首帧的孤立尾帧：容错按单帧尝试，本例应解析失败返回空
    SwarmTunnel::Reassembly rx3;
    QVERIFY(SwarmTunnel::parseDatagram(dg2, rx3).isEmpty());
}

// 感知输入校验闸门(第七轮):非法坐标/低置信航迹应被丢弃,防污染态势与 LLM
void tst_p3modules::fusion_rejectsInvalidLatLon()
{
    SwarmTrackFusion fusion;
    fusion.ingest(makeReport(1, 1, 999.0, 116.4), 1000);  // lat 非法
    fusion.ingest(makeReport(2, 1, 39.9, 999.0), 1000);   // lon 非法
    QVERIFY2(fusion.allTracks().isEmpty(), "非法坐标航迹应被感知校验闸门丢弃");
    QCOMPARE(fusion.rejectedCount(), 2);
}

void tst_p3modules::fusion_dropsLowConfidence()
{
    SwarmTrackFusion fusion;
    fusion.ingest(makeReport(1, 1, 39.9, 116.4, 0.0, 0.0, 0.05), 1000);  // conf 0.05 < 门槛 0.2
    QVERIFY2(fusion.allTracks().isEmpty(), "低置信度航迹应被门槛过滤");
    QCOMPARE(fusion.rejectedCount(), 1);
    fusion.ingest(makeReport(2, 1, 39.9, 116.4, 0.0, 0.0, 0.8), 1100);  // 门槛之上放行
    QCOMPARE(fusion.allTracks().count(), 1);
}

QTEST_GUILESS_MAIN(tst_p3modules)
#include "tst_p3modules.moc"
