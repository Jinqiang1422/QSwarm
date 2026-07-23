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

} // namespace SwarmTactics
