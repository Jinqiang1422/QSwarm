/****************************************************************************
 * SwarmTactics 集群协同战术算法单元测试
 * 五场景基线：协同搜索 / 巡逻 / 跟踪 / 围捕 / 拦截
 * 仅依赖 Qt6 Core/Test，可在 CI 快速构建运行。
 ****************************************************************************/

#include <QtTest/QtTest>

#include "SwarmTactics.h"
#include "SwarmTypes.h"

#include <cmath>

using namespace SwarmTactics;

namespace {
/// 构造一架处于给定经纬度的在线机
SwarmTypes::VehicleState mkVehicle(int id, double lat, double lon)
{
    SwarmTypes::VehicleState v;
    v.id  = id;
    v.lat = lat;
    v.lon = lon;
    return v;
}
} // namespace

class TstTactics : public QObject
{
    Q_OBJECT

private slots:
    // ---- 场景③ 跟踪 ----
    void tracking_standoffRingGeometry();
    void tracking_standoffRingPhaseOffset();
    void tracking_standoffRingDegenerate();
};

// ============================================================================
// 场景③ 跟踪：Standoff 环绕观测
// ============================================================================
void TstTactics::tracking_standoffRingGeometry()
{
    const double tgtLat = 39.9, tgtLon = 116.4, R = 120.0;
    const QList<GeoPoint> ring = standoffRing(tgtLat, tgtLon, R, 4, 0.0);
    QCOMPARE(ring.size(), 4);

    // 每个观测点到目标的距离 ≈ R
    for (const GeoPoint &p : ring) {
        const double d = SwarmTypes::haversineM(tgtLat, tgtLon, p.lat, p.lon);
        QVERIFY2(std::abs(d - R) < 1.0, "观测点应落在半径 R 的圆上");
    }
    // 第 0 机相位 0°=正北：纬度增大、经度基本不变
    QVERIFY(ring[0].lat > tgtLat);
    QVERIFY(std::abs(ring[0].lon - tgtLon) < 1e-4);
    // 第 1 机相位 90°=正东：经度增大
    QVERIFY(ring[1].lon > tgtLon);
    // 正 n 边形：相邻观测点弦长相等
    const double e01 = SwarmTypes::haversineM(ring[0].lat, ring[0].lon, ring[1].lat, ring[1].lon);
    const double e12 = SwarmTypes::haversineM(ring[1].lat, ring[1].lon, ring[2].lat, ring[2].lon);
    QVERIFY2(std::abs(e01 - e12) < 1.0, "均分环相邻弦长应相等");
}

void TstTactics::tracking_standoffRingPhaseOffset()
{
    // 起始相位 45°：第 0 机应偏向东北（纬度、经度都增大）
    const double tgtLat = 39.9, tgtLon = 116.4;
    const QList<GeoPoint> ring = standoffRing(tgtLat, tgtLon, 100.0, 4, 45.0);
    QCOMPARE(ring.size(), 4);
    QVERIFY(ring[0].lat > tgtLat);
    QVERIFY(ring[0].lon > tgtLon);
}

void TstTactics::tracking_standoffRingDegenerate()
{
    QVERIFY(standoffRing(39.9, 116.4, 100.0, 0, 0.0).isEmpty());   // 机数 0
    QVERIFY(standoffRing(39.9, 116.4, 0.0, 4, 0.0).isEmpty());     // 半径 0
}

QTEST_GUILESS_MAIN(TstTactics)
#include "tst_tactics.moc"
