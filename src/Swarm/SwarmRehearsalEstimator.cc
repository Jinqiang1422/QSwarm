#include "SwarmRehearsalEstimator.h"

#include <QtCore/QtMath>

SwarmRehearsalEstimator::Estimate SwarmRehearsalEstimator::estimate(
        const QList<SwarmAction> &plan,
        const QList<SwarmTypes::VehicleState> &vehicles,
        double mapCenterLat, double mapCenterLon,
        const Config &cfg) const
{
    Estimate est;
    const int nV = qMax(1, vehicles.count());
    est.vehiclesUsed = nV;

    double lastCoverageCenterLat = mapCenterLat;
    double lastCoverageCenterLon = mapCenterLon;
    double lastAlt = 30.0;
    bool   hasCoverage = false;

    for (const SwarmAction &action : plan) {
        const QString &type = action.type;
        const QVariantMap &p = action.params;

        if (type == QStringLiteral("takeoff")) {
            const double alt = p.value(QStringLiteral("altitude"), 30.0).toDouble();
            lastAlt = alt;
            est.durationMin += alt / cfg.climbRateMs / 60.0;
        } else if (type == QStringLiteral("plan_coverage")) {
            const double length  = p.value(QStringLiteral("length"), 0.0).toDouble();
            const double width   = p.value(QStringLiteral("width"), 0.0).toDouble();
            const double spacing = p.value(QStringLiteral("spacing"), 20.0).toDouble();
            const double cLat    = p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble();
            const double cLon    = p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble();
            const int bands = qMax(1, qCeil(width / qMax(1.0, spacing)));
            const double sweepM = bands * length + (bands - 1) * qMax(1.0, spacing);

            double maxTransitM = 0.0, sumTransitM = 0.0;
            for (const SwarmTypes::VehicleState &v : vehicles) {
                const double d = SwarmTypes::haversineM(v.lat, v.lon, cLat, cLon);
                maxTransitM = qMax(maxTransitM, d);
                sumTransitM += d;
            }
            const double perVehicleM = sweepM / nV + maxTransitM;
            est.durationMin += perVehicleM / cfg.cruiseSpeedMs / 60.0;
            est.distanceKm  += (sweepM + sumTransitM) / 1000.0;
            lastCoverageCenterLat = cLat;
            lastCoverageCenterLon = cLon;
            hasCoverage = true;
        } else if (type == QStringLiteral("start_mission")) {
            if (!hasCoverage) {
                est.notes.append(QStringLiteral("start_mission 无前置覆盖规划，按机载既有任务计（时长未含）"));
            }
        } else if (type == QStringLiteral("rtl")) {
            double maxRangeM = 0.0;
            for (const SwarmTypes::VehicleState &v : vehicles) {
                maxRangeM = qMax(maxRangeM, SwarmTypes::haversineM(
                                            v.lat, v.lon, lastCoverageCenterLat, lastCoverageCenterLon));
            }
            est.durationMin += maxRangeM / cfg.cruiseSpeedMs / 60.0
                             + lastAlt / cfg.descentRateMs / 60.0;
            est.distanceKm  += maxRangeM * nV / 1000.0;
        } else if (type == QStringLiteral("land")) {
            est.durationMin += lastAlt / cfg.descentRateMs / 60.0;
        } else if (type == QStringLiteral("start_formation")) {
            est.durationMin += 0.5;     // 队形就位估计
        }
        // arm/disarm/pause/clear_missions/set_selection/stop_formation：耗时忽略
    }

    // 电量裕度：最差机
    double minBatt = 100.0;
    for (const SwarmTypes::VehicleState &v : vehicles) {
        if (v.batteryPct >= 0.0) {
            minBatt = qMin(minBatt, v.batteryPct);
        }
    }
    const double usedPct = est.durationMin / cfg.enduranceMin * 100.0;
    est.batteryMarginPct = qMax(0.0, minBatt - usedPct);
    if (est.durationMin <= 0.0) {
        est.notes.append(QStringLiteral("计划无显著耗时动作"));
    }
    return est;
}

QString SwarmRehearsalEstimator::summary(const Estimate &est)
{
    return QStringLiteral("≈%1 分钟｜%2 km｜最低裕度 %3%")
            .arg(est.durationMin, 0, 'f', 1)
            .arg(est.distanceKm, 0, 'f', 1)
            .arg(est.batteryMarginPct, 0, 'f', 0);
}
