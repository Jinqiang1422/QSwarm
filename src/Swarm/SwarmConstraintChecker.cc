#include "SwarmConstraintChecker.h"

#include <QtCore/QJsonArray>
#include <QtCore/QtMath>

#include <algorithm>

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
SwarmConstraintChecker::Config SwarmConstraintChecker::configFromJson(const QJsonObject &obj)
{
    Config cfg;
    const auto readPolys = [&obj](const QString &key) {
        QList<QList<QPair<double, double>>> out;
        const QJsonArray groups = obj.value(key).toArray();
        for (const QJsonValue &gv : groups) {
            const QJsonArray ring = gv.toObject().value(QStringLiteral("polygon")).toArray();
            QList<QPair<double, double>> poly;
            for (const QJsonValue &pv : ring) {
                const QJsonArray pt = pv.toArray();
                if (pt.count() >= 2) {
                    poly.append({pt.at(0).toDouble(), pt.at(1).toDouble()});
                }
            }
            if (poly.count() >= 3) {
                out.append(poly);
            }
        }
        return out;
    };
    cfg.includePolygons = readPolys(QStringLiteral("include"));
    cfg.excludePolygons = readPolys(QStringLiteral("exclude"));
    cfg.maxAltitudeM   = obj.value(QStringLiteral("max_altitude_m")).toDouble(cfg.maxAltitudeM);
    cfg.commRangeM     = obj.value(QStringLiteral("comm_range_m")).toDouble(cfg.commRangeM);
    cfg.homeLat        = obj.value(QStringLiteral("home_lat")).toDouble(0.0);
    cfg.homeLon        = obj.value(QStringLiteral("home_lon")).toDouble(0.0);
    cfg.cruiseSpeedMs  = obj.value(QStringLiteral("cruise_speed_ms")).toDouble(cfg.cruiseSpeedMs);
    cfg.climbRateMs    = obj.value(QStringLiteral("climb_rate_ms")).toDouble(cfg.climbRateMs);
    cfg.enduranceMin   = obj.value(QStringLiteral("endurance_min")).toDouble(cfg.enduranceMin);
    cfg.reserveFactor  = obj.value(QStringLiteral("reserve_factor")).toDouble(cfg.reserveFactor);
    cfg.minSeparationM = obj.value(QStringLiteral("min_separation_m")).toDouble(cfg.minSeparationM);
    return cfg;
}

QJsonObject SwarmConstraintChecker::configToJson(const Config &cfg)
{
    QJsonObject obj;
    obj[QStringLiteral("max_altitude_m")]   = cfg.maxAltitudeM;
    obj[QStringLiteral("comm_range_m")]     = cfg.commRangeM;
    obj[QStringLiteral("home_lat")]         = cfg.homeLat;
    obj[QStringLiteral("home_lon")]         = cfg.homeLon;
    obj[QStringLiteral("cruise_speed_ms")]  = cfg.cruiseSpeedMs;
    obj[QStringLiteral("climb_rate_ms")]    = cfg.climbRateMs;
    obj[QStringLiteral("endurance_min")]    = cfg.enduranceMin;
    obj[QStringLiteral("reserve_factor")]   = cfg.reserveFactor;
    obj[QStringLiteral("min_separation_m")] = cfg.minSeparationM;
    return obj;
}

// ---------------------------------------------------------------------------
// 覆盖几何
// ---------------------------------------------------------------------------
void SwarmConstraintChecker::coverageGeometry(double lengthMeters, double widthMeters,
                                              double spacingMeters, int &bands, double &sweepDistanceM)
{
    bands = qMax(1, qCeil(widthMeters / qMax(1.0, spacingMeters)));
    // 割草机：bands 个长边 + bands-1 次横移
    sweepDistanceM = bands * lengthMeters + (bands - 1) * qMax(1.0, spacingMeters);
}

// ---------------------------------------------------------------------------
// 围栏检查
// ---------------------------------------------------------------------------
void SwarmConstraintChecker::_checkFence(const SwarmAction &action, const Config &cfg,
                                         double mapCenterLat, double mapCenterLon,
                                         QList<Violation> &out) const
{
    if (cfg.includePolygons.isEmpty() && cfg.excludePolygons.isEmpty()) {
        return;
    }
    const QVariantMap &p = action.params;
    const double centerLat = p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble();
    const double centerLon = p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble();
    const double length    = p.value(QStringLiteral("length"), 0.0).toDouble();
    const double width     = p.value(QStringLiteral("width"), 0.0).toDouble();
    const double angleDeg  = p.value(QStringLiteral("angle"), 0.0).toDouble();

    // 区域中心 + 四角
    QList<QPair<double, double>> probePoints = {{centerLat, centerLon}};
    const double a = SwarmTypes::degToRad(angleDeg);
    for (int sx : {-1, 1}) {
        for (int sy : {-1, 1}) {
            const double x = sx * length / 2.0, y = sy * width / 2.0;
            const double east  = x * qSin(a) + y * qCos(a);
            const double north = x * qCos(a) - y * qSin(a);
            const double dist  = qSqrt(east * east + north * north);
            const double brg   = qRadiansToDegrees(qAtan2(east, north));
            double lat2, lon2;
            SwarmTypes::destinationPoint(centerLat, centerLon, brg, dist, lat2, lon2);
            probePoints.append({lat2, lon2});
        }
    }

    if (!cfg.includePolygons.isEmpty()) {
        bool insideAny = false;
        for (const auto &poly : cfg.includePolygons) {
            if (SwarmTypes::pointInPolygon(centerLat, centerLon, poly)) {
                insideAny = true;
                break;
            }
        }
        if (!insideAny) {
            out.append({QStringLiteral("error"), QStringLiteral("fence"),
                        QStringLiteral("作业区中心位于允许空域之外"),
                        QStringLiteral("调整区域中心至允许空域内，或更新围栏配置")});
        }
    }
    for (const auto &poly : cfg.excludePolygons) {
        for (const auto &pt : probePoints) {
            if (SwarmTypes::pointInPolygon(pt.first, pt.second, poly)) {
                out.append({QStringLiteral("error"), QStringLiteral("fence"),
                            QStringLiteral("作业区与禁飞区相交"),
                            QStringLiteral("缩小区域/平移避开禁飞区，或更新围栏配置")});
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 航程-电量检查
// ---------------------------------------------------------------------------
void SwarmConstraintChecker::_checkBattery(const SwarmAction &action,
                                           const QList<SwarmTypes::VehicleState> &vehicles,
                                           const Config &cfg,
                                           double mapCenterLat, double mapCenterLon,
                                           QList<Violation> &out) const
{
    const QVariantMap &p = action.params;
    const double length  = p.value(QStringLiteral("length"), 0.0).toDouble();
    const double width   = p.value(QStringLiteral("width"), 0.0).toDouble();
    const double spacing = p.value(QStringLiteral("spacing"), 20.0).toDouble();
    const double alt     = p.value(QStringLiteral("altitude"), 30.0).toDouble();
    const double cLat    = p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble();
    const double cLon    = p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble();

    int bands = 0;
    double sweepM = 0.0;
    coverageGeometry(length, width, spacing, bands, sweepM);

    const int nV = qMax(1, vehicles.count());
    double transitM = 0.0;
    for (const SwarmTypes::VehicleState &v : vehicles) {
        transitM = qMax(transitM, SwarmTypes::haversineM(v.lat, v.lon, cLat, cLon));
    }
    const double perVehicleM = sweepM / nV + transitM;
    const double missionMin  = perVehicleM / cfg.cruiseSpeedMs / 60.0
                             + 2.0 * alt / cfg.climbRateMs / 60.0;   // 爬升+下降
    const double neededPct   = missionMin / cfg.enduranceMin * 100.0;

    double minBatt = 100.0;
    bool battUnknown = true;
    for (const SwarmTypes::VehicleState &v : vehicles) {
        if (v.batteryPct >= 0.0) {
            minBatt = qMin(minBatt, v.batteryPct);
            battUnknown = false;
        }
    }
    if (battUnknown) {
        out.append({QStringLiteral("warning"), QStringLiteral("battery"),
                    QStringLiteral("机队电量未知，航程校验按满电估算"),
                    QStringLiteral("接入遥测后复核；建议保守参数")});
    }
    const double availablePct = minBatt * cfg.reserveFactor;

    if (neededPct > availablePct) {
        out.append({QStringLiteral("error"), QStringLiteral("battery"),
                    QStringLiteral("估算任务耗电 %1%，超过可用电量（%2%，含安全余量）")
                        .arg(neededPct, 0, 'f', 0).arg(availablePct, 0, 'f', 0),
                    QStringLiteral("缩小区域/增大行距/分组接力/缩短转场，或更换满电电池")});
    } else if (neededPct > availablePct * 0.85) {
        out.append({QStringLiteral("warning"), QStringLiteral("battery"),
                    QStringLiteral("估算任务耗电 %1%，电量裕度不足 15%").arg(neededPct, 0, 'f', 0),
                    QStringLiteral("建议缩小区域或增大行距以增大裕度")});
    }
}

// ---------------------------------------------------------------------------
// 主检查
// ---------------------------------------------------------------------------
void SwarmConstraintChecker::_checkTakeoffAlt(const SwarmAction &action, const Config &cfg,
                                              QList<Violation> &out) const
{
    const double alt = action.params.value(QStringLiteral("altitude"), 30.0).toDouble();
    if (cfg.maxAltitudeM > 0.0 && alt > cfg.maxAltitudeM) {
        out.append({QStringLiteral("error"), QStringLiteral("fence"),
                    QStringLiteral("起飞高度 %1 米超过围栏上限 %2 米").arg(alt).arg(cfg.maxAltitudeM),
                    QStringLiteral("降低高度至 %1 米以下").arg(cfg.maxAltitudeM)});
    }
}

void SwarmConstraintChecker::_checkComm(const SwarmAction &action, const Config &cfg,
                                        double mapCenterLat, double mapCenterLon,
                                        QList<Violation> &out) const
{
    const QVariantMap &p = action.params;
    const double cLat = p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble();
    const double cLon = p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble();
    // 链路覆盖：区域中心 + 半对角 距集结点
    const double halfDiag = qSqrt(qPow(p.value(QStringLiteral("length"), 0.0).toDouble() / 2.0, 2)
                                  + qPow(p.value(QStringLiteral("width"), 0.0).toDouble() / 2.0, 2));
    const double homeLat = (cfg.homeLat != 0.0) ? cfg.homeLat : mapCenterLat;
    const double homeLon = (cfg.homeLon != 0.0) ? cfg.homeLon : mapCenterLon;
    const double range = SwarmTypes::haversineM(homeLat, homeLon, cLat, cLon) + halfDiag;
    if (range > cfg.commRangeM * 1.5) {
        out.append({QStringLiteral("error"), QStringLiteral("comm"),
                    QStringLiteral("最远作业点 %1 米，远超链路半径 %2 米")
                        .arg(range, 0, 'f', 0).arg(cfg.commRangeM, 0, 'f', 0),
                    QStringLiteral("缩小作业半径/前移指控点/增设中继")});
    } else if (range > cfg.commRangeM) {
        out.append({QStringLiteral("warning"), QStringLiteral("comm"),
                    QStringLiteral("最远作业点 %1 米，超出链路半径 %2 米")
                        .arg(range, 0, 'f', 0).arg(cfg.commRangeM, 0, 'f', 0),
                    QStringLiteral("建议前移指控点或增设链路中继")});
    }
}

void SwarmConstraintChecker::_checkSpacing(const SwarmAction &action, const Config &cfg,
                                           QList<Violation> &out) const
{
    const double spacing = action.params.value(QStringLiteral("spacing"), 15.0).toDouble();
    if (spacing < cfg.minSeparationM) {
        out.append({QStringLiteral("error"), QStringLiteral("spacing"),
                    QStringLiteral("编队间距 %1 米低于安全下限 %2 米").arg(spacing).arg(cfg.minSeparationM),
                    QStringLiteral("增大间距至 %1 米以上").arg(cfg.minSeparationM)});
    } else if (spacing < cfg.minSeparationM * 2.0) {
        out.append({QStringLiteral("warning"), QStringLiteral("spacing"),
                    QStringLiteral("编队间距 %1 米接近下限").arg(spacing),
                    QStringLiteral("建议间距 ≥ %1 米").arg(cfg.minSeparationM * 2.0)});
    }
}

void SwarmConstraintChecker::_checkDeconflict(const QList<CoverageArea> &areas,
                                              QList<Violation> &out) const
{
    for (int i = 0; i < areas.count(); i++) {
        for (int j = i + 1; j < areas.count(); j++) {
            const double d = SwarmTypes::haversineM(areas.at(i).lat, areas.at(i).lon,
                                                    areas.at(j).lat, areas.at(j).lon);
            if (d < 50.0) {
                out.append({QStringLiteral("warning"), QStringLiteral("deconflict"),
                            QStringLiteral("两个作业区中心相距仅 %1 米，可能重叠").arg(d, 0, 'f', 0),
                            QStringLiteral("拉大区中心间距或合并为一个作业区")});
            }
        }
    }
}

QList<SwarmConstraintChecker::Violation> SwarmConstraintChecker::check(
        const QList<SwarmAction> &plan,
        const QList<SwarmTypes::VehicleState> &vehicles,
        const Config &cfg,
        double mapCenterLat, double mapCenterLon) const
{
    QList<Violation> out;
    QList<CoverageArea> coverageAreas;   // 收集作业区用于多区重叠检查
    for (const SwarmAction &action : plan) {
        const QString &type = action.type;
        if (type == QStringLiteral("takeoff")) {
            _checkTakeoffAlt(action, cfg, out);
        } else if (type == QStringLiteral("plan_coverage")) {
            _checkFence(action, cfg, mapCenterLat, mapCenterLon, out);
            _checkBattery(action, vehicles, cfg, mapCenterLat, mapCenterLon, out);
            _checkComm(action, cfg, mapCenterLat, mapCenterLon, out);
            const QVariantMap &p = action.params;
            coverageAreas.append({p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble(),
                                  p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble(),
                                  p.value(QStringLiteral("length"), 0.0).toDouble(),
                                  p.value(QStringLiteral("width"), 0.0).toDouble()});
        } else if (type == QStringLiteral("start_formation")) {
            _checkSpacing(action, cfg, out);
        }
    }
    _checkDeconflict(coverageAreas, out);
    return out;
}

QList<SwarmConstraintChecker::Violation> SwarmConstraintChecker::checkTemplate(
        int minFleet, const QString &requiredPayload, int onlineCount, bool fleetPayloadKnown) const
{
    QList<Violation> out;
    if (minFleet > 0 && onlineCount < minFleet) {
        out.append({QStringLiteral("error"), QStringLiteral("fleet"),
                    QStringLiteral("模板要求最少 %1 架在线，当前仅 %2 架").arg(minFleet).arg(onlineCount),
                    QStringLiteral("补充上线无人机，或改用适配当前规模的模板")});
    }
    if (!requiredPayload.isEmpty() && !fleetPayloadKnown) {
        out.append({QStringLiteral("warning"), QStringLiteral("payload"),
                    QStringLiteral("模板要求载荷「%1」，但机队载荷能力未知").arg(requiredPayload),
                    QStringLiteral("确认各机载荷配置后执行")});
    }
    return out;
}

bool SwarmConstraintChecker::hasError(const QList<Violation> &violations)
{
    for (const Violation &v : violations) {
        if (v.severity == QStringLiteral("error")) {
            return true;
        }
    }
    return false;
}
