#include "SwarmTactics.h"

#include "SwarmTaskAllocator.h"

#include <QtCore/QtMath>

#include <cmath>

namespace SwarmTactics {

// ============================================================================
// 场景③ 跟踪：Standoff 环绕观测
// ============================================================================
QList<GeoPoint> standoffRing(double tgtLat, double tgtLon, double radiusM,
                             int n, double phase0Deg)
{
    QList<GeoPoint> out;
    if (n <= 0 || radiusM <= 0.0) {
        return out;
    }
    // 相位均分：第 i 机方位角 = phase0 + i·(360/n)，沿该方位角在半径 R 处布点
    for (int i = 0; i < n; i++) {
        const double bearing = std::fmod(phase0Deg + 360.0 * i / n, 360.0);
        GeoPoint p;
        SwarmTypes::destinationPoint(tgtLat, tgtLon, bearing, radiusM, p.lat, p.lon);
        out.append(p);
    }
    return out;
}

// ============================================================================
// 场景④ 围捕：环形包围 + 拍卖就近分配（半径可收缩）
// ============================================================================
QMap<int, GeoPoint> encircle(const QList<SwarmTypes::VehicleState> &vehicles,
                             double tgtLat, double tgtLon, double radiusM)
{
    QMap<int, GeoPoint> out;
    const int n = vehicles.count();
    if (n <= 0 || radiusM <= 0.0) {
        return out;
    }
    // 目标周围 n 个均分包围点
    const QList<GeoPoint> ring = standoffRing(tgtLat, tgtLon, radiusM, n, 0.0);

    // 拍卖就近分配：机、包围点均以目标为原点转本地米，最小化总转场里程
    SwarmTaskAllocator allocator;
    QList<SwarmTaskAllocator::Node> vNodes, pNodes;
    for (int i = 0; i < n; i++) {
        double e, nth;
        SwarmTypes::latLonToLocalM(tgtLat, tgtLon, vehicles.at(i).lat, vehicles.at(i).lon, e, nth);
        vNodes.append({vehicles.at(i).id, e, nth});
    }
    for (int i = 0; i < n; i++) {
        double e, nth;
        SwarmTypes::latLonToLocalM(tgtLat, tgtLon, ring.at(i).lat, ring.at(i).lon, e, nth);
        pNodes.append({i, e, nth});   // 包围点 Node id = 环索引
    }
    const QMap<int, int> assign = allocator.auctionAssign(vNodes, pNodes);
    for (int i = 0; i < n; i++) {
        const int vid  = vehicles.at(i).id;
        const int pIdx = assign.value(vid, i);   // 缺省回退同序
        out.insert(vid, ring.at(pIdx));
    }
    return out;
}

// ============================================================================
// 场景⑤ 拦截：碰撞三角预测（匀速直线目标）
// ============================================================================
InterceptSolution intercept(double insLat, double insLon, double vMps,
                            const MovingTarget &tgt)
{
    InterceptSolution sol;
    if (vMps <= 0.0) {
        return sol;
    }
    // 以拦截机为原点的本地米坐标：目标初始位置与速度向量（东、北）
    double tE, tN;
    SwarmTypes::latLonToLocalM(insLat, insLon, tgt.lat, tgt.lon, tE, tN);
    const double hd = SwarmTypes::degToRad(tgt.headingDeg);
    const double vE = tgt.speedMps * std::sin(hd);
    const double vN = tgt.speedMps * std::cos(hd);

    // 拦截机以速度 v 直飞目标 t 时刻位置 T(t)，要求 |T(t)| = v·t，
    // 展开得碰撞三角二次方程 a·t² + b·t + c = 0
    const double a = tgt.speedMps * tgt.speedMps - vMps * vMps;
    const double b = 2.0 * (tE * vE + tN * vN);
    const double c = tE * tE + tN * tN;

    double t = -1.0;
    const double kEps = 1e-9;
    if (std::abs(a) < kEps) {
        // 双方速度相等：退化为线性方程
        if (std::abs(b) > kEps) {
            const double cand = -c / b;
            if (cand > 0.0) {
                t = cand;
            }
        }
    } else {
        const double disc = b * b - 4.0 * a * c;
        if (disc >= 0.0) {
            const double sq = std::sqrt(disc);
            const double roots[2] = {(-b + sq) / (2.0 * a), (-b - sq) / (2.0 * a)};
            for (const double cand : roots) {   // 取最小正根 = 最早拦截
                if (cand > 0.0 && (t < 0.0 || cand < t)) {
                    t = cand;
                }
            }
        }
    }
    if (t <= 0.0) {
        return sol;   // 追不上（速度不足或目标远离）
    }

    // 拦截点（本地米）→ 方位角/距离 → 地理坐标
    const double iE = tE + vE * t;
    const double iN = tN + vN * t;
    const double dist = std::sqrt(iE * iE + iN * iN);
    double bearing = std::atan2(iE, iN) * 180.0 / M_PI;   // (东,北) → 北起顺时针方位角
    if (bearing < 0.0) {
        bearing += 360.0;
    }
    SwarmTypes::destinationPoint(insLat, insLon, bearing, dist, sol.lat, sol.lon);
    sol.feasible   = true;
    sol.headingDeg = bearing;
    sol.timeS      = t;
    return sol;
}

// ============================================================================
// 场景① 协同搜索：矩形区域按机数分区 + 拍卖就近分配
// ============================================================================
QList<SearchLane> partitionSearch(const QList<SwarmTypes::VehicleState> &vehicles,
                                  double centerLat, double centerLon,
                                  double lengthM, double widthM, double angleDeg)
{
    QList<SearchLane> out;
    const int n = vehicles.count();
    if (n <= 0 || widthM <= 0.0) {
        return out;
    }
    const double laneW       = widthM / n;
    const double perpBearing = std::fmod(angleDeg + 90.0, 360.0);   // 垂直航向=切分方向

    // 沿垂直航向把区域均匀切成 n 条带，求各条带中心
    QList<GeoPoint> laneCenters;
    for (int i = 0; i < n; i++) {
        const double offset = -widthM / 2.0 + (i + 0.5) * laneW;    // [-W/2, +W/2]
        GeoPoint p{centerLat, centerLon};
        if (std::abs(offset) > 1e-6) {
            const double brg = (offset > 0.0) ? perpBearing
                                              : std::fmod(perpBearing + 180.0, 360.0);
            SwarmTypes::destinationPoint(centerLat, centerLon, brg, std::abs(offset), p.lat, p.lon);
        }
        laneCenters.append(p);
    }

    // 拍卖就近分配：机、条带中心均以区域中心为原点转本地米，最小化总进场转场
    SwarmTaskAllocator allocator;
    QList<SwarmTaskAllocator::Node> vNodes, lNodes;
    for (int i = 0; i < n; i++) {
        double e, nth;
        SwarmTypes::latLonToLocalM(centerLat, centerLon, vehicles.at(i).lat, vehicles.at(i).lon, e, nth);
        vNodes.append({vehicles.at(i).id, e, nth});
    }
    for (int i = 0; i < n; i++) {
        double e, nth;
        SwarmTypes::latLonToLocalM(centerLat, centerLon, laneCenters.at(i).lat, laneCenters.at(i).lon, e, nth);
        lNodes.append({i, e, nth});
    }
    const QMap<int, int> assign = allocator.auctionAssign(vNodes, lNodes);
    for (int i = 0; i < n; i++) {
        const int vid  = vehicles.at(i).id;
        const int lIdx = assign.value(vid, i);
        SearchLane lane;
        lane.vehicleId   = vid;
        lane.center      = laneCenters.at(lIdx);
        lane.laneWidthM  = laneW;
        lane.laneLengthM = lengthM;
        lane.bearingDeg  = std::fmod(angleDeg, 360.0);
        out.append(lane);
    }
    return out;
}

} // namespace SwarmTactics
