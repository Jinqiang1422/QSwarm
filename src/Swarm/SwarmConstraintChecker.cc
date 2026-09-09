#include "SwarmConstraintChecker.h"

#include <QtCore/QJsonArray>
#include <QtCore/QtMath>
#include <algorithm>

namespace {
using Point = QPair<double, double>;
using Polygon = QList<Point>;

double cross(const Point& a, const Point& b, const Point& c)
{
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}

bool onSegment(const Point& a, const Point& b, const Point& p)
{
    constexpr double epsilon = 1e-12;
    return std::abs(cross(a, b, p)) <= epsilon && p.first >= qMin(a.first, b.first) - epsilon &&
           p.first <= qMax(a.first, b.first) + epsilon && p.second >= qMin(a.second, b.second) - epsilon &&
           p.second <= qMax(a.second, b.second) + epsilon;
}

bool intersects(const Point& a, const Point& b, const Point& c, const Point& d)
{
    return ((cross(a, b, c) * cross(a, b, d) < 0.0 && cross(c, d, a) * cross(c, d, b) < 0.0) || onSegment(a, b, c) ||
            onSegment(a, b, d) || onSegment(c, d, a) || onSegment(c, d, b));
}

bool crossesBoundary(const Point& a, const Point& b, const Polygon& polygon)
{
    for (int i = 0; i < polygon.size(); ++i) {
        if (intersects(a, b, polygon[i], polygon[(i + 1) % polygon.size()])) {
            return true;
        }
    }
    return false;
}

bool contains(const Polygon& polygon, const Point& p)
{
    return SwarmTypes::pointInPolygon(p.first, p.second, polygon);
}

bool containedBy(const Polygon& area, const Polygon& allowed)
{
    SwarmConstraintChecker::Config config;
    config.includePolygons = {allowed};
    for (int i = 0; i < area.size(); ++i) {
        if (!SwarmConstraintChecker::segmentAllowed(area[i], area[(i + 1) % area.size()], config)) {
            return false;
        }
    }
    return true;
}

bool overlaps(const Polygon& a, const Polygon& b)
{
    if (a.isEmpty() || b.isEmpty()) {
        return false;
    }
    if (contains(a, b.first()) || contains(b, a.first())) {
        return true;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (crossesBoundary(a[i], a[(i + 1) % a.size()], b)) {
            return true;
        }
    }
    return false;
}

Polygon coveragePolygon(const SwarmAction& action, double mapLat, double mapLon)
{
    const auto& p = action.params;
    const double centerLat = p.value(QStringLiteral("center_lat"), mapLat).toDouble();
    const double centerLon = p.value(QStringLiteral("center_lon"), mapLon).toDouble();
    const double length = p.value(QStringLiteral("length")).toDouble();
    const double width = p.value(QStringLiteral("width")).toDouble();
    const double angleDeg = p.value(QStringLiteral("angle"), 0.0).toDouble();
    // Ordered rectangle boundary: tests include edge crossings and an exclusion wholly inside the area.
    Polygon area;
    const double a = SwarmTypes::degToRad(angleDeg);
    for (const Point& corner : Polygon{{-1, -1}, {-1, 1}, {1, 1}, {1, -1}}) {
        const double x = corner.first * length / 2.0, y = corner.second * width / 2.0;
        const double east = x * qSin(a) + y * qCos(a);
        const double north = x * qCos(a) - y * qSin(a);
        double lat, lon;
        SwarmTypes::destinationPoint(centerLat, centerLon, qRadiansToDegrees(qAtan2(east, north)),
                                     std::hypot(east, north), lat, lon);
        area.append({lat, lon});
    }
    return area;
}
}  // namespace

bool SwarmConstraintChecker::segmentAllowed(const Point& from, const Point& to, const Config& cfg)
{
    if (!cfg.error.isEmpty() || !SwarmTypes::isFiniteLatLon(from.first, from.second) ||
        !SwarmTypes::isFiniteLatLon(to.first, to.second) || std::abs(to.second - from.second) > 180.0)
        return false;
    for (const auto& polygon : cfg.excludePolygons)
        if (contains(polygon, from) || contains(polygon, to) || crossesBoundary(from, to, polygon))
            return false;
    if (cfg.includePolygons.isEmpty())
        return true;
    const auto permitted = [&](const Point& point) {
        return std::any_of(cfg.includePolygons.cbegin(), cfg.includePolygons.cend(),
                           [&](const Polygon& polygon) { return contains(polygon, point); });
    };
    if (!permitted(from) || !permitted(to))
        return false;
    // Split at all boundary crossings. Testing every open interval handles concave and overlapping areas.
    QList<double> cuts{0.0, 1.0};
    const Point direction{to.first - from.first, to.second - from.second};
    for (const auto& polygon : cfg.includePolygons) {
        for (int i = 0; i < polygon.size(); ++i) {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % polygon.size()];
            const Point edge{b.first - a.first, b.second - a.second};
            const double denominator = direction.first * edge.second - direction.second * edge.first;
            if (std::abs(denominator) < 1e-18)
                continue;
            const double t =
                ((a.first - from.first) * edge.second - (a.second - from.second) * edge.first) / denominator;
            const double u =
                ((a.first - from.first) * direction.second - (a.second - from.second) * direction.first) / denominator;
            if (t > 0 && t < 1 && u >= 0 && u <= 1)
                cuts.append(t);
        }
    }
    std::sort(cuts.begin(), cuts.end());
    for (int i = 1; i < cuts.size(); ++i) {
        const double t = (cuts[i - 1] + cuts[i]) / 2.0;
        if (!permitted({from.first + direction.first * t, from.second + direction.second * t}))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
SwarmConstraintChecker::Config SwarmConstraintChecker::configFromJson(const QJsonObject& obj)
{
    Config cfg;
    const auto readPolys = [&obj, &cfg](const QString& key) {
        QList<QList<QPair<double, double>>> out;
        if (obj.contains(key) && !obj.value(key).isArray()) {
            cfg.error = QStringLiteral("围栏多边形列表格式无效");
        }
        const QJsonArray groups = obj.value(key).toArray();
        for (const QJsonValue& gv : groups) {
            const QJsonArray ring = gv.toObject().value(QStringLiteral("polygon")).toArray();
            QList<QPair<double, double>> poly;
            for (const QJsonValue& pv : ring) {
                const QJsonArray pt = pv.toArray();
                if (pt.count() == 2 && pt[0].isDouble() && pt[1].isDouble() && std::isfinite(pt[0].toDouble()) &&
                    std::isfinite(pt[1].toDouble()) && std::abs(pt[0].toDouble()) <= 90.0 &&
                    std::abs(pt[1].toDouble()) <= 180.0) {
                    poly.append({pt.at(0).toDouble(), pt.at(1).toDouble()});
                } else {
                    cfg.error = QStringLiteral("围栏顶点坐标无效");
                }
            }
            if (poly.count() >= 3) {
                if (poly.size() > 3 && poly.first() == poly.last())
                    poly.removeLast();
                double area = 0.0;
                for (int i = 0; i < poly.size(); ++i) {
                    const auto& a = poly[i];
                    const auto& b = poly[(i + 1) % poly.size()];
                    area += (b.first - a.first) * (b.second + a.second);
                    if (std::abs(b.second - a.second) > 180.0)
                        cfg.error = QStringLiteral("围栏不支持跨日期变更线");
                    for (int j = i + 2; j < poly.size(); ++j) {
                        if (i == 0 && j == poly.size() - 1)
                            continue;
                        if (intersects(a, b, poly[j], poly[(j + 1) % poly.size()]))
                            cfg.error = QStringLiteral("围栏多边形自交");
                    }
                }
                if (std::abs(area) < 1e-12)
                    cfg.error = QStringLiteral("围栏多边形面积为零");
                out.append(poly);
            } else {
                cfg.error = QStringLiteral("围栏至少需要三个有效顶点");
            }
        }
        return out;
    };
    cfg.includePolygons = readPolys(QStringLiteral("include"));
    cfg.excludePolygons = readPolys(QStringLiteral("exclude"));
    cfg.maxAltitudeM = obj.value(QStringLiteral("max_altitude_m")).toDouble(cfg.maxAltitudeM);
    cfg.commRangeM = obj.value(QStringLiteral("comm_range_m")).toDouble(cfg.commRangeM);
    cfg.homeLat = obj.value(QStringLiteral("home_lat")).toDouble(0.0);
    cfg.homeLon = obj.value(QStringLiteral("home_lon")).toDouble(0.0);
    cfg.cruiseSpeedMs = obj.value(QStringLiteral("cruise_speed_ms")).toDouble(cfg.cruiseSpeedMs);
    cfg.climbRateMs = obj.value(QStringLiteral("climb_rate_ms")).toDouble(cfg.climbRateMs);
    cfg.enduranceMin = obj.value(QStringLiteral("endurance_min")).toDouble(cfg.enduranceMin);
    cfg.reserveFactor = obj.value(QStringLiteral("reserve_factor")).toDouble(cfg.reserveFactor);
    cfg.minSeparationM = obj.value(QStringLiteral("min_separation_m")).toDouble(cfg.minSeparationM);
    cfg.commWarnFactor = obj.value(QStringLiteral("comm_warn_factor")).toDouble(cfg.commWarnFactor);
    cfg.commErrorFactor = obj.value(QStringLiteral("comm_error_factor")).toDouble(cfg.commErrorFactor);
    cfg.spacingWarnFactor = obj.value(QStringLiteral("spacing_warn_factor")).toDouble(cfg.spacingWarnFactor);
    cfg.deconflictFactor = obj.value(QStringLiteral("deconflict_factor")).toDouble(cfg.deconflictFactor);
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (it.key() != QStringLiteral("include") && it.key() != QStringLiteral("exclude") &&
            (!it.value().isDouble() || !std::isfinite(it.value().toDouble()))) {
            cfg.error = QStringLiteral("围栏配置数值类型无效");
        }
    }
    if (cfg.maxAltitudeM < 0 || cfg.commRangeM <= 0 || cfg.cruiseSpeedMs <= 0 || cfg.climbRateMs <= 0 ||
        cfg.enduranceMin <= 0 || cfg.reserveFactor <= 0 || cfg.reserveFactor > 1 || cfg.minSeparationM <= 0 ||
        cfg.commWarnFactor <= 0 || cfg.commErrorFactor < cfg.commWarnFactor || cfg.spacingWarnFactor < 1 ||
        cfg.deconflictFactor <= 0 || std::abs(cfg.homeLat) > 90 || std::abs(cfg.homeLon) > 180) {
        cfg.error = QStringLiteral("围栏配置数值超出范围");
    }
    return cfg;
}

QJsonObject SwarmConstraintChecker::configToJson(const Config& cfg)
{
    QJsonObject obj;
    obj[QStringLiteral("max_altitude_m")] = cfg.maxAltitudeM;
    obj[QStringLiteral("comm_range_m")] = cfg.commRangeM;
    obj[QStringLiteral("home_lat")] = cfg.homeLat;
    obj[QStringLiteral("home_lon")] = cfg.homeLon;
    obj[QStringLiteral("cruise_speed_ms")] = cfg.cruiseSpeedMs;
    obj[QStringLiteral("climb_rate_ms")] = cfg.climbRateMs;
    obj[QStringLiteral("endurance_min")] = cfg.enduranceMin;
    obj[QStringLiteral("reserve_factor")] = cfg.reserveFactor;
    obj[QStringLiteral("min_separation_m")] = cfg.minSeparationM;
    obj[QStringLiteral("comm_warn_factor")] = cfg.commWarnFactor;
    obj[QStringLiteral("comm_error_factor")] = cfg.commErrorFactor;
    obj[QStringLiteral("spacing_warn_factor")] = cfg.spacingWarnFactor;
    obj[QStringLiteral("deconflict_factor")] = cfg.deconflictFactor;
    return obj;
}

// ---------------------------------------------------------------------------
// 覆盖几何
// ---------------------------------------------------------------------------
void SwarmConstraintChecker::coverageGeometry(double lengthMeters, double widthMeters, double spacingMeters, int& bands,
                                              double& sweepDistanceM)
{
    bands = qMax(1, qCeil(widthMeters / qMax(1.0, spacingMeters)));
    // 割草机：bands 个长边 + bands-1 次横移
    sweepDistanceM = bands * lengthMeters + (bands - 1) * qMax(1.0, spacingMeters);
}

// ---------------------------------------------------------------------------
// 围栏检查
// ---------------------------------------------------------------------------
void SwarmConstraintChecker::_checkFence(const SwarmAction& action, const Config& cfg, double mapCenterLat,
                                         double mapCenterLon, QList<Violation>& out) const
{
    if (cfg.includePolygons.isEmpty() && cfg.excludePolygons.isEmpty()) {
        return;
    }
    const Polygon area = coveragePolygon(action, mapCenterLat, mapCenterLon);
    if (!cfg.includePolygons.isEmpty()) {
        const bool inside = std::any_of(cfg.includePolygons.cbegin(), cfg.includePolygons.cend(),
                                        [&area](const Polygon& polygon) { return containedBy(area, polygon); });
        if (!inside) {
            out.append({QStringLiteral("error"), QStringLiteral("fence"),
                        QStringLiteral("完整作业区未处于同一允许空域内"),
                        QStringLiteral("缩小或移动作业区，避开允许区边界")});
        }
    }
    for (const auto& polygon : cfg.excludePolygons) {
        if (overlaps(area, polygon)) {
            out.append({QStringLiteral("error"), QStringLiteral("fence"), QStringLiteral("作业区与禁飞区相交"),
                        QStringLiteral("缩小或平移作业区以避开禁飞区")});
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 航程-电量检查
// ---------------------------------------------------------------------------
void SwarmConstraintChecker::_checkBattery(const SwarmAction& action, const QList<SwarmTypes::VehicleState>& vehicles,
                                           const Config& cfg, double mapCenterLat, double mapCenterLon,
                                           QList<Violation>& out) const
{
    const QVariantMap& p = action.params;
    const double length = p.value(QStringLiteral("length"), 0.0).toDouble();
    const double width = p.value(QStringLiteral("width"), 0.0).toDouble();
    const double spacing = p.value(QStringLiteral("spacing"), 20.0).toDouble();
    const double alt = p.value(QStringLiteral("altitude"), 30.0).toDouble();
    const double cLat = p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble();
    const double cLon = p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble();

    int bands = 0;
    double sweepM = 0.0;
    coverageGeometry(length, width, spacing, bands, sweepM);

    const int nV = qMax(1, vehicles.count());
    double transitM = 0.0;
    for (const SwarmTypes::VehicleState& v : vehicles) {
        const double returnLat = std::isfinite(v.homeLat) ? v.homeLat : v.lat;
        const double returnLon = std::isfinite(v.homeLon) ? v.homeLon : v.lon;
        transitM = qMax(transitM, SwarmTypes::haversineM(v.lat, v.lon, cLat, cLon) +
                                      SwarmTypes::haversineM(cLat, cLon, returnLat, returnLon));
    }
    const double perVehicleM = sweepM / nV + transitM;
    const double missionMin = perVehicleM / cfg.cruiseSpeedMs / 60.0 + 2.0 * alt / cfg.climbRateMs / 60.0;  // 爬升+下降
    const double neededPct = missionMin / cfg.enduranceMin * 100.0;

    double minBatt = 100.0;
    bool battUnknown = true;
    for (const SwarmTypes::VehicleState& v : vehicles) {
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
                        .arg(neededPct, 0, 'f', 0)
                        .arg(availablePct, 0, 'f', 0),
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
void SwarmConstraintChecker::_checkTakeoffAlt(const SwarmAction& action, const Config& cfg, QList<Violation>& out) const
{
    const double alt = action.params.value(QStringLiteral("altitude"), 30.0).toDouble();
    if (cfg.maxAltitudeM > 0.0 && alt > cfg.maxAltitudeM) {
        out.append({QStringLiteral("error"), QStringLiteral("fence"),
                    QStringLiteral("任务高度 %1 米超过围栏上限 %2 米").arg(alt).arg(cfg.maxAltitudeM),
                    QStringLiteral("降低高度至 %1 米以下").arg(cfg.maxAltitudeM)});
    }
}

void SwarmConstraintChecker::_checkComm(const SwarmAction& action, const Config& cfg, double mapCenterLat,
                                        double mapCenterLon, QList<Violation>& out) const
{
    const QVariantMap& p = action.params;
    const double cLat = p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble();
    const double cLon = p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble();
    // 链路覆盖：区域中心 + 半对角 距集结点
    const double halfDiag = qSqrt(qPow(p.value(QStringLiteral("length"), 0.0).toDouble() / 2.0, 2) +
                                  qPow(p.value(QStringLiteral("width"), 0.0).toDouble() / 2.0, 2));
    const bool homeConfigured = cfg.homeLat != 0.0 || cfg.homeLon != 0.0;
    const double homeLat = homeConfigured ? cfg.homeLat : mapCenterLat;
    const double homeLon = homeConfigured ? cfg.homeLon : mapCenterLon;
    const double range = SwarmTypes::haversineM(homeLat, homeLon, cLat, cLon) + halfDiag;
    if (range > cfg.commRangeM * cfg.commErrorFactor) {
        out.append({QStringLiteral("error"), QStringLiteral("comm"),
                    QStringLiteral("最远作业点 %1 米，远超链路半径 %2 米")
                        .arg(range, 0, 'f', 0)
                        .arg(cfg.commRangeM, 0, 'f', 0),
                    QStringLiteral("缩小作业半径/前移指控点/增设中继")});
    } else if (range > cfg.commRangeM * cfg.commWarnFactor) {
        out.append({QStringLiteral("warning"), QStringLiteral("comm"),
                    QStringLiteral("最远作业点 %1 米，超出链路半径 %2 米")
                        .arg(range, 0, 'f', 0)
                        .arg(cfg.commRangeM, 0, 'f', 0),
                    QStringLiteral("建议前移指控点或增设链路中继")});
    }
}

void SwarmConstraintChecker::_checkSpacing(const SwarmAction& action, const Config& cfg, QList<Violation>& out) const
{
    const double spacing = action.params.value(QStringLiteral("spacing"), 15.0).toDouble();
    if (spacing < cfg.minSeparationM) {
        out.append({QStringLiteral("error"), QStringLiteral("spacing"),
                    QStringLiteral("编队间距 %1 米低于安全下限 %2 米").arg(spacing).arg(cfg.minSeparationM),
                    QStringLiteral("增大间距至 %1 米以上").arg(cfg.minSeparationM)});
    } else if (spacing < cfg.minSeparationM * cfg.spacingWarnFactor) {
        out.append({QStringLiteral("warning"), QStringLiteral("spacing"),
                    QStringLiteral("编队间距 %1 米接近下限").arg(spacing),
                    QStringLiteral("建议间距 ≥ %1 米").arg(cfg.minSeparationM * 2.0)});
    }
}

void SwarmConstraintChecker::_checkDeconflict(const QList<CoverageArea>& areas, const Config& cfg,
                                              QList<Violation>& out) const
{
    // 按作业区实际尺寸判重叠：中心距 < (两区半对角之和)×系数 视为重叠,
    // 修复原固定 50m 阈值的大区漏报 / 小区误报。
    for (int i = 0; i < areas.count(); i++) {
        for (int j = i + 1; j < areas.count(); j++) {
            const double d = SwarmTypes::haversineM(areas.at(i).lat, areas.at(i).lon, areas.at(j).lat, areas.at(j).lon);
            const double halfDiagI = 0.5 * qSqrt(qPow(areas.at(i).lengthM, 2) + qPow(areas.at(i).widthM, 2));
            const double halfDiagJ = 0.5 * qSqrt(qPow(areas.at(j).lengthM, 2) + qPow(areas.at(j).widthM, 2));
            const double threshold = (halfDiagI + halfDiagJ) * cfg.deconflictFactor;
            if (d < threshold) {
                out.append({QStringLiteral("warning"), QStringLiteral("deconflict"),
                            QStringLiteral("两作业区中心相距 %1 米 < 安全间隔 %2 米，可能重叠")
                                .arg(d, 0, 'f', 0)
                                .arg(threshold, 0, 'f', 0),
                            QStringLiteral("拉大区中心间距或合并为一个作业区")});
            }
        }
    }
}

QList<SwarmConstraintChecker::Violation> SwarmConstraintChecker::checkDeconfliction(const QList<SwarmAction>& plan,
                                                                                    const Config& cfg,
                                                                                    double mapCenterLat,
                                                                                    double mapCenterLon) const
{
    QList<CoverageArea> areas;
    for (const auto& action : plan) {
        if (action.type != QStringLiteral("plan_coverage"))
            continue;
        const auto& p = action.params;
        areas.append({p.value(QStringLiteral("center_lat"), mapCenterLat).toDouble(),
                      p.value(QStringLiteral("center_lon"), mapCenterLon).toDouble(),
                      p.value(QStringLiteral("length"), 0.0).toDouble(),
                      p.value(QStringLiteral("width"), 0.0).toDouble()});
    }
    QList<Violation> out;
    _checkDeconflict(areas, cfg, out);
    return out;
}

QList<SwarmConstraintChecker::Violation> SwarmConstraintChecker::check(const QList<SwarmAction>& plan,
                                                                       const QList<SwarmTypes::VehicleState>& vehicles,
                                                                       const Config& cfg, double mapCenterLat,
                                                                       double mapCenterLon) const
{
    QList<Violation> out;
    const bool usesConstraints = std::any_of(plan.cbegin(), plan.cend(), [](const auto& action) {
        return action.type == QStringLiteral("takeoff") || action.type == QStringLiteral("plan_coverage") ||
               action.type == QStringLiteral("start_formation") || action.type == QStringLiteral("start_mission");
    });
    if (usesConstraints && !cfg.error.isEmpty()) {
        out.append(
            {QStringLiteral("error"), QStringLiteral("config"), cfg.error, QStringLiteral("修正围栏配置后重试")});
        return out;
    }
    for (const SwarmAction& action : plan) {
        const QString& type = action.type;
        if (type == QStringLiteral("takeoff")) {
            _checkTakeoffAlt(action, cfg, out);
        } else if (type == QStringLiteral("plan_coverage")) {
            _checkFence(action, cfg, mapCenterLat, mapCenterLon, out);
            _checkTakeoffAlt(action, cfg, out);
            const Polygon area = coveragePolygon(action, mapCenterLat, mapCenterLon);
            QList<Point> endpoints;
            for (const auto& vehicle : vehicles) {
                endpoints.append({vehicle.lat, vehicle.lon});
                if (std::isfinite(vehicle.homeLat) && std::isfinite(vehicle.homeLon))
                    endpoints.append({vehicle.homeLat, vehicle.homeLon});
            }
            for (const Point& start : endpoints) {
                bool safe = std::isfinite(start.first) && std::isfinite(start.second);
                // The fan covers all straight transfers from this vehicle to the survey rectangle.
                for (int edge = 0; edge < area.size() && safe; ++edge) {
                    const Polygon corridor{start, area[edge], area[(edge + 1) % area.size()]};
                    if (!cfg.includePolygons.isEmpty()) {
                        safe = std::any_of(cfg.includePolygons.cbegin(), cfg.includePolygons.cend(),
                                           [&](const Polygon& polygon) { return containedBy(corridor, polygon); });
                    }
                    for (const auto& polygon : cfg.excludePolygons)
                        safe = safe && !overlaps(corridor, polygon);
                }
                if (!safe) {
                    out.append({QStringLiteral("error"), QStringLiteral("fence"),
                                QStringLiteral("转场路径无效或穿越围栏/禁飞区"), QStringLiteral("调整作业区或起点")});
                    break;
                }
            }
            _checkBattery(action, vehicles, cfg, mapCenterLat, mapCenterLon, out);
            _checkComm(action, cfg, mapCenterLat, mapCenterLon, out);
        } else if (type == QStringLiteral("start_formation")) {
            _checkSpacing(action, cfg, out);
        }
    }
    out.append(checkDeconfliction(plan, cfg, mapCenterLat, mapCenterLon));
    return out;
}

QList<SwarmConstraintChecker::Violation> SwarmConstraintChecker::checkTemplate(int minFleet,
                                                                               const QString& requiredPayload,
                                                                               int onlineCount,
                                                                               bool fleetPayloadKnown) const
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

bool SwarmConstraintChecker::hasError(const QList<Violation>& violations)
{
    for (const Violation& v : violations) {
        if (v.severity == QStringLiteral("error")) {
            return true;
        }
    }
    return false;
}
