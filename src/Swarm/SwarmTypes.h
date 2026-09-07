/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmTypes
 *
 * 约束引擎/估算器/分配器共享的轻量类型与大地计算工具。
 * 仅依赖 QtCore（坐标用裸 double），可脱离 QGC 独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <cmath>
#include <limits>

namespace SwarmTypes {

/// 单机状态快照（由 SwarmManager 从 Vehicle 适配填充，引擎不反向依赖 QGC 类型）
struct VehicleState
{
    int id = 0;
    double lat = 0.0;
    double lon = 0.0;
    double altRelM = 0.0;
    double batteryPct = -1.0;  ///< <0 表示未知
    bool online = true;
    double homeLat = std::numeric_limits<double>::quiet_NaN();
    double homeLon = std::numeric_limits<double>::quiet_NaN();
};

inline double degToRad(double deg)
{
    return deg * M_PI / 180.0;
}

/// 球面大圆距离（米）
inline double haversineM(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double kEarthR = 6371000.0;
    const double dLat = degToRad(lat2 - lat1);
    const double dLon = degToRad(lon2 - lon1);
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(degToRad(lat1)) * std::cos(degToRad(lat2)) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2.0 * kEarthR * std::asin(std::sqrt(a));
}

/// 等距圆柱近似（短距离米级换算，用于成本矩阵/局部几何）
inline void latLonToLocalM(double refLat, double refLon, double lat, double lon, double& eastM, double& northM)
{
    const double latAvg = degToRad((refLat + lat) / 2.0);
    eastM = (lon - refLon) * std::cos(latAvg) * 111320.0;
    northM = (lat - refLat) * 110540.0;
}

/// 目的地推算：从 (lat,lon) 沿方位角 bearingDeg（北起）前进 distanceM 后的坐标
inline void destinationPoint(double lat, double lon, double bearingDeg, double distanceM, double& outLat,
                             double& outLon)
{
    constexpr double kEarthR = 6371000.0;
    const double brg = degToRad(bearingDeg);
    const double lat1 = degToRad(lat);
    const double d = distanceM / kEarthR;
    const double lat2 = std::asin(std::sin(lat1) * std::cos(d) + std::cos(lat1) * std::sin(d) * std::cos(brg));
    const double lon2 = degToRad(lon) + std::atan2(std::sin(brg) * std::sin(d) * std::cos(lat1),
                                                   std::cos(d) - std::sin(lat1) * std::sin(lat2));
    constexpr double kRadToDeg = 180.0 / M_PI;
    outLat = lat2 * kRadToDeg;
    outLon = lon2 * kRadToDeg;
}

/// 射线法：点是否在多边形内（polygon 为 [lat,lon] 顶点序列，首尾不必闭合）
inline bool pointInPolygon(double lat, double lon, const QList<QPair<double, double>>& polygon)
{
    if (polygon.count() < 3) {
        return false;
    }
    bool inside = false;
    for (int i = 0, j = polygon.count() - 1; i < polygon.count(); j = i++) {
        const double yi = polygon.at(i).first, xi = polygon.at(i).second;
        const double yj = polygon.at(j).first, xj = polygon.at(j).second;
        const bool intersect = ((yi > lat) != (yj > lat)) && (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi);
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

/// 经纬度是否在合法地理范围内（[-90,90]×[-180,180]，且非 (0,0) 占位）
inline bool isValidLatLon(double lat, double lon)
{
    if (!std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return false;
    }
    return !(std::abs(lat) < 1e-9 && std::abs(lon) < 1e-9);  // (0,0) 视为无效占位
}

}  // namespace SwarmTypes
