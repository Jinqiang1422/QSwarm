/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmTactics（集群协同战术算法库）
 *
 * 五类典型协同场景的教科书级基线算法，纯几何/解析实现：
 *   ① 协同搜索  partitionSearch  区域按机数分区，每机一条带（拍卖就近分配）
 *   ② 巡逻      partitionPatrol  多边形周界按机数均分为弧段，相位错开
 *   ③ 跟踪      standoffRing     N 机在目标周围定半径圆上相位均分持续观测
 *   ④ 围捕      encircle         目标周围环形包围点 + 拍卖就近分配（半径可收缩）
 *   ⑤ 拦截      intercept        碰撞三角预测匀速目标的最早拦截点/航向/时间
 *
 * 仅依赖 QtCore + SwarmTypes 大地计算 + SwarmTaskAllocator 分配，可独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include "SwarmTypes.h"

#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QPair>

namespace SwarmTactics {

/// 地理航点（度）
struct GeoPoint {
    double lat = 0.0;
    double lon = 0.0;
};

/// 匀速直线运动目标（用于拦截预测）
struct MovingTarget {
    double lat        = 0.0;
    double lon        = 0.0;
    double speedMps   = 0.0;
    double headingDeg = 0.0;   ///< 航向（北起顺时针，度）
};

/// 拦截解算结果
struct InterceptSolution {
    bool   feasible   = false;
    double lat        = 0.0;   ///< 拦截点
    double lon        = 0.0;
    double headingDeg = 0.0;   ///< 拦截机应飞航向（北起，度）
    double timeS      = 0.0;   ///< 预计拦截时间（秒）
};

/// 搜索分区（每机负责的一条带）
struct SearchLane {
    int      vehicleId   = 0;
    GeoPoint center;              ///< 条带中心
    double   laneWidthM  = 0.0;   ///< 条带宽度（垂直航向，米）
    double   laneLengthM = 0.0;   ///< 条带长度（沿搜索航向，米）
    double   bearingDeg  = 0.0;   ///< 搜索航线方位角（度）
};

// ---- 场景③ 跟踪：Standoff 环绕观测 ----
/// N 机在目标周围半径 R 的圆上相位均分，持续观测（随目标移动反复调用即可更新圆心）。
/// @param n 观测机数；phase0Deg 起始相位方位角（北起）。返回 n 个观测点（相位升序）。
QList<GeoPoint> standoffRing(double tgtLat, double tgtLon, double radiusM,
                             int n, double phase0Deg = 0.0);

// ---- 场景④ 围捕：环形包围 + 就近分配 ----
/// 目标周围半径 R 生成 |vehicles| 个均分包围点，用拍卖法把机分配到最近点（最小总里程）。
/// radiusM 可逐步减小实现收缩包围。@return vehicleId → 分配到的包围点。
QMap<int, GeoPoint> encircle(const QList<SwarmTypes::VehicleState> &vehicles,
                             double tgtLat, double tgtLon, double radiusM);

// ---- 场景⑤ 拦截：碰撞三角预测 ----
/// 解二次方程 a·t²+b·t+c=0，返回最小正实根;无正实根返回 -1（a≈0 时退化为线性方程）。
/// 独立纯函数，供拦截碰撞三角求解，便于穷举边界验证。
double solveQuadraticEarliestPositive(double a, double b, double c);

/// 拦截机 (insLat,insLon) 以速度 vMps 拦截匀速直线目标 tgt，解碰撞三角求最早拦截点。
/// 无正实根（速度不足追不上）时 feasible=false。
InterceptSolution intercept(double insLat, double insLon, double vMps,
                            const MovingTarget &tgt);

/// 从机群中挑选距目标最近的机作为拦截机，返回其 id；空列表返回 -1。
int pickInterceptor(const QList<SwarmTypes::VehicleState> &vehicles,
                    double tgtLat, double tgtLon);

// ---- 场景① 协同搜索：区域分区 ----
/// 矩形区域（中心 center、沿 angleDeg 航向长 lengthM、垂直宽 widthM）按机数沿宽度切条带，
/// 每机分一条带，用拍卖法把机就近分配到条带以减少进场转场。
/// @return 每机的搜索条带（含中心/宽度/航向），供航线生成器铺割草机航线。
QList<SearchLane> partitionSearch(const QList<SwarmTypes::VehicleState> &vehicles,
                                  double centerLat, double centerLon,
                                  double lengthM, double widthM, double angleDeg);

// ---- 场景② 巡逻：周界均分 ----
/// 多边形周界按机数均分为连续弧段，每机巡逻一段（相位错开覆盖不同区段，降低最大空隙时间）。
/// polygon 为 [lat,lon] 顶点（首尾自动闭合）；samplesPerLap 全周采样点数（0 时默认每机 8 点）。
/// @return vehicleId → 该机巡逻航点序列（按巡逻方向）。
QMap<int, QList<GeoPoint>> partitionPatrol(const QList<SwarmTypes::VehicleState> &vehicles,
                                           const QList<QPair<double, double>> &polygon,
                                           int samplesPerLap = 0);

} // namespace SwarmTactics
