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

    // ---- 场景④ 围捕 ----
    void encircle_coversRing();
    void encircle_nearestAssignment();
    void encircle_degenerate();

    // ---- 场景⑤ 拦截 ----
    void intercept_stationaryTarget();
    void intercept_movingTargetConsistency();
    void intercept_infeasibleWhenSlower();
    void intercept_zeroSpeed();

    // ---- 场景① 协同搜索 ----
    void search_partitionCoversWidth();
    void search_nearestAssignment();
    void search_degenerate();
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

// ============================================================================
// 场景④ 围捕：环形包围 + 拍卖就近分配
// ============================================================================
void TstTactics::encircle_coversRing()
{
    const double tgtLat = 39.9, tgtLon = 116.4, R = 150.0;
    const QList<SwarmTypes::VehicleState> v{
        mkVehicle(1, 39.905, 116.400),   // 北
        mkVehicle(2, 39.900, 116.406),   // 东
        mkVehicle(3, 39.895, 116.400),   // 南
        mkVehicle(4, 39.900, 116.394),   // 西
    };
    const QMap<int, GeoPoint> asg = encircle(v, tgtLat, tgtLon, R);
    QCOMPARE(asg.size(), 4);

    // 每机分到的包围点距目标 ≈ R，且四点互不重复（真正“包围”）
    QList<GeoPoint> seen;
    for (auto it = asg.constBegin(); it != asg.constEnd(); ++it) {
        const double d = SwarmTypes::haversineM(tgtLat, tgtLon, it.value().lat, it.value().lon);
        QVERIFY2(std::abs(d - R) < 1.0, "包围点应在半径 R 圆上");
        for (const GeoPoint &s : seen) {
            QVERIFY2(SwarmTypes::haversineM(s.lat, s.lon, it.value().lat, it.value().lon) > 1.0,
                     "各机应分到不同包围点");
        }
        seen.append(it.value());
    }
}

void TstTactics::encircle_nearestAssignment()
{
    // 四机分处目标 N/E/S/W，拍卖就近应各分到同方向包围点（最小转场）
    const double tgtLat = 39.9, tgtLon = 116.4, R = 150.0;
    const QList<SwarmTypes::VehicleState> v{
        mkVehicle(1, 39.905, 116.400),   // 北
        mkVehicle(2, 39.900, 116.406),   // 东
        mkVehicle(3, 39.895, 116.400),   // 南
        mkVehicle(4, 39.900, 116.394),   // 西
    };
    const QMap<int, GeoPoint> asg = encircle(v, tgtLat, tgtLon, R);
    QVERIFY(asg[1].lat > tgtLat && std::abs(asg[1].lon - tgtLon) < 1e-3);  // 北机→北点
    QVERIFY(asg[2].lon > tgtLon && std::abs(asg[2].lat - tgtLat) < 1e-3);  // 东机→东点
    QVERIFY(asg[3].lat < tgtLat && std::abs(asg[3].lon - tgtLon) < 1e-3);  // 南机→南点
    QVERIFY(asg[4].lon < tgtLon && std::abs(asg[4].lat - tgtLat) < 1e-3);  // 西机→西点
}

void TstTactics::encircle_degenerate()
{
    QVERIFY(encircle({}, 39.9, 116.4, 100.0).isEmpty());
}

// ============================================================================
// 场景⑤ 拦截：碰撞三角预测
// ============================================================================
void TstTactics::intercept_stationaryTarget()
{
    // 静止目标在正东 1000m，拦截机 10 m/s → t≈100s，拦截点=目标，航向≈90°(东)
    const double insLat = 39.9, insLon = 116.4;
    double tLat, tLon;
    SwarmTypes::destinationPoint(insLat, insLon, 90.0, 1000.0, tLat, tLon);
    MovingTarget tgt;
    tgt.lat = tLat; tgt.lon = tLon; tgt.speedMps = 0.0; tgt.headingDeg = 0.0;

    const InterceptSolution s = intercept(insLat, insLon, 10.0, tgt);
    QVERIFY(s.feasible);
    QVERIFY2(std::abs(s.timeS - 100.0) < 1.0, "t = dist/v ≈ 100s");
    QVERIFY2(std::abs(s.headingDeg - 90.0) < 1.0, "拦截静止目标应直飞其方位(东)");
    QVERIFY2(SwarmTypes::haversineM(s.lat, s.lon, tLat, tLon) < 3.0, "拦截点≈静止目标位置");
}

void TstTactics::intercept_movingTargetConsistency()
{
    // 目标正北 500m 向东 5 m/s，拦截机 20 m/s：可拦截，满足碰撞条件 |ins→拦截点| = v·t
    const double insLat = 39.9, insLon = 116.4;
    double tLat, tLon;
    SwarmTypes::destinationPoint(insLat, insLon, 0.0, 500.0, tLat, tLon);
    MovingTarget tgt;
    tgt.lat = tLat; tgt.lon = tLon; tgt.speedMps = 5.0; tgt.headingDeg = 90.0;

    const InterceptSolution s = intercept(insLat, insLon, 20.0, tgt);
    QVERIFY(s.feasible);
    QVERIFY(s.timeS > 0.0);
    // 碰撞条件：拦截机以 v 飞 t 秒的航程 = 到拦截点的直线距离
    const double flown = 20.0 * s.timeS;
    const double reach = SwarmTypes::haversineM(insLat, insLon, s.lat, s.lon);
    QVERIFY2(std::abs(flown - reach) < 2.0, "航程应等于到拦截点距离");
    // 目标向东逃逸，拦截点应落在其初始位置以东
    QVERIFY(s.lon > tLon);
}

void TstTactics::intercept_infeasibleWhenSlower()
{
    // 目标正东 1000m 向东逃逸 20 m/s，拦截机仅 10 m/s：追不上
    const double insLat = 39.9, insLon = 116.4;
    double tLat, tLon;
    SwarmTypes::destinationPoint(insLat, insLon, 90.0, 1000.0, tLat, tLon);
    MovingTarget tgt;
    tgt.lat = tLat; tgt.lon = tLon; tgt.speedMps = 20.0; tgt.headingDeg = 90.0;

    const InterceptSolution s = intercept(insLat, insLon, 10.0, tgt);
    QVERIFY2(!s.feasible, "慢于逃逸目标应判不可拦截");
}

void TstTactics::intercept_zeroSpeed()
{
    MovingTarget tgt;
    tgt.lat = 39.91; tgt.lon = 116.41; tgt.speedMps = 0.0;
    QVERIFY(!intercept(39.9, 116.4, 0.0, tgt).feasible);   // 拦截机速度 0
}

// ============================================================================
// 场景① 协同搜索：区域分区 + 拍卖就近分配
// ============================================================================
void TstTactics::search_partitionCoversWidth()
{
    // 2 机，1000×600 区域，航向 0°(南北航线)：切成 2 条各宽 300 的东西向条带
    const double cLat = 39.9, cLon = 116.4;
    const QList<SwarmTypes::VehicleState> v{ mkVehicle(1, cLat, cLon), mkVehicle(2, cLat, cLon) };
    const QList<SearchLane> lanes = partitionSearch(v, cLat, cLon, 1000.0, 600.0, 0.0);
    QCOMPARE(lanes.size(), 2);

    for (const SearchLane &ln : lanes) {
        QVERIFY2(std::abs(ln.laneWidthM - 300.0) < 1e-6, "条带宽 = 总宽/机数");
        QVERIFY2(std::abs(ln.laneLengthM - 1000.0) < 1e-6, "条带长 = 区域长");
        QCOMPARE(ln.bearingDeg, 0.0);
        // 条带中心到区域中心 ≈ 半条带宽 = 150m
        const double d = SwarmTypes::haversineM(cLat, cLon, ln.center.lat, ln.center.lon);
        QVERIFY2(std::abs(d - 150.0) < 1.0, "条带中心偏移 = W/(2n) = 150m");
    }
    // 两条带分列区域东西两侧（航向 0° → 垂直切分方向为东西）
    const bool oneWest = (lanes[0].center.lon < cLon) != (lanes[1].center.lon < cLon);
    QVERIFY2(oneWest, "两条带应分列中心线东西两侧，无缝覆盖全宽");
}

void TstTactics::search_nearestAssignment()
{
    // 机分处区域东西两侧，拍卖就近应各分到同侧条带
    const double cLat = 39.9, cLon = 116.4;
    double wLat, wLon, eLat, eLon;
    SwarmTypes::destinationPoint(cLat, cLon, 270.0, 500.0, wLat, wLon);   // 西 500m
    SwarmTypes::destinationPoint(cLat, cLon,  90.0, 500.0, eLat, eLon);   // 东 500m
    const QList<SwarmTypes::VehicleState> v{ mkVehicle(1, wLat, wLon), mkVehicle(2, eLat, eLon) };
    const QList<SearchLane> lanes = partitionSearch(v, cLat, cLon, 1000.0, 600.0, 0.0);
    QCOMPARE(lanes.size(), 2);

    // lanes 按输入机序：机1(西)应得西条带，机2(东)应得东条带
    QCOMPARE(lanes[0].vehicleId, 1);
    QVERIFY2(lanes[0].center.lon < cLon, "西侧机应分到西条带");
    QCOMPARE(lanes[1].vehicleId, 2);
    QVERIFY2(lanes[1].center.lon > cLon, "东侧机应分到东条带");
}

void TstTactics::search_degenerate()
{
    QVERIFY(partitionSearch({}, 39.9, 116.4, 1000.0, 600.0, 0.0).isEmpty());
}

QTEST_GUILESS_MAIN(TstTactics)
#include "tst_tactics.moc"
