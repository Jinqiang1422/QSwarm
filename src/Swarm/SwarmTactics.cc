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

} // namespace SwarmTactics
