#include "SwarmMissionSnapshot.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QStringList>

#include "Fact.h"
#include "FirmwarePlugin.h"
#include "MissionItem.h"
#include "Vehicle.h"

SwarmMissionSnapshot SwarmMissionSnapshot::capture(const QList<MissionItem*>& items, bool hasHome)
{
    SwarmMissionSnapshot result;
    for (int i = hasHome ? 1 : 0; i < items.size(); ++i) {
        const auto* item = items[i];
        if (!item)
            return {};
        int frame = item->frame();
        if (frame == MAV_FRAME_GLOBAL_INT)
            frame = MAV_FRAME_GLOBAL;
        if (frame == MAV_FRAME_GLOBAL_RELATIVE_ALT_INT)
            frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
        result._items.append({double(item->command()), double(frame), item->param1(), item->param2(), item->param3(),
                              item->param4(), item->param5(), item->param6(), item->param7(),
                              double(item->autoContinue())});
    }
    return result;
}

bool SwarmMissionSnapshot::equivalent(const SwarmMissionSnapshot& other) const
{
    if (_items.size() != other._items.size())
        return false;
    for (int i = 0; i < _items.size(); ++i) {
        for (int p = 0; p < _items[i].size(); ++p) {
            const double a = _items[i][p], b = other._items[i][p];
            if (std::isnan(a) && std::isnan(b))
                continue;
            const double tolerance = (p == 6 || p == 7) ? 1.1e-7 : 1e-4;
            if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > tolerance)
                return false;
        }
    }
    return true;
}

QString SwarmMissionSnapshot::fingerprint() const
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << _items;
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString SwarmMissionSnapshot::validate(Vehicle* vehicle, const SwarmConstraintChecker::Config& config,
                                       double batteryPct) const
{
    if (!config.error.isEmpty())
        return config.error;
    if (!vehicle || !vehicle->multiRotor() || _items.isEmpty())
        return QStringLiteral("航线为空或机型不支持可靠的集群航线校验");
    const auto home = vehicle->homePosition();
    QGeoCoordinate position = vehicle->coordinate();
    position.setAltitude(vehicle->altitudeAMSL()->rawValue().toDouble());
    if (!home.isValid() || !std::isfinite(home.altitude()) || !position.isValid() ||
        !std::isfinite(position.altitude()))
        return QStringLiteral("航线校验缺少有效位置、高度或 Home");
    QList<QGeoCoordinate> route{position};
    double dwellSeconds = 0.0;
    for (int index = 0; index < _items.size(); ++index) {
        const auto& item = _items[index];
        const int command = int(item[0]), frame = int(item[1]);
        if (item[9] != 1.0)
            return QStringLiteral("航线包含非连续执行项，无法可靠估算");
        if (command == MAV_CMD_NAV_RETURN_TO_LAUNCH) {
            if (index != _items.size() - 1)
                return QStringLiteral("返航必须是航线最后一项");
            QList<QGeoCoordinate> returning;
            QString reason;
            double returnDelay = 0;
            if (!vehicle->firmwarePlugin()->missionReturnPath(vehicle, route.last(), returning, returnDelay, reason))
                return reason;
            route.append(returning);
            dwellSeconds += returnDelay;
            continue;
        }
        if (command != MAV_CMD_NAV_WAYPOINT && command != MAV_CMD_NAV_TAKEOFF && command != MAV_CMD_NAV_LAND)
            return QStringLiteral("航线包含不支持的命令 %1（跳转、地形与动态路径须用标准 QGC 工作流）").arg(command);
        if (frame != MAV_FRAME_GLOBAL && frame != MAV_FRAME_GLOBAL_RELATIVE_ALT)
            return QStringLiteral("航线使用无法可靠校验的高度坐标系");
        if ((command == MAV_CMD_NAV_TAKEOFF && index != 0) ||
            (command == MAV_CMD_NAV_LAND && index != _items.size() - 1))
            return QStringLiteral("起飞/降落顺序不支持");
        if (!SwarmTypes::isFiniteLatLon(item[6], item[7]) || !std::isfinite(item[8]) || !std::isfinite(item[2]) ||
            !std::isfinite(item[3]) || !std::isfinite(item[4]) || item[2] < 0 || item[4] != 0)
            return QStringLiteral("航点坐标、停留时间或绕行参数无效");
        QGeoCoordinate point(item[6], item[7],
                             item[8] + (frame == MAV_FRAME_GLOBAL_RELATIVE_ALT ? home.altitude() : 0));
        if (command != MAV_CMD_NAV_WAYPOINT && item[6] == 0 && item[7] == 0) {
            point.setLatitude(route.last().latitude());
            point.setLongitude(route.last().longitude());
        }
        route.append(point);
        if (command == MAV_CMD_NAV_WAYPOINT)
            dwellSeconds += item[2];
    }
    // A waypoint-only mission must also leave a feasible return path and energy reserve.
    if (int(_items.last()[0]) == MAV_CMD_NAV_WAYPOINT) {
        QList<QGeoCoordinate> returning;
        QString reason;
        double returnDelay = 0;
        if (!vehicle->firmwarePlugin()->missionReturnPath(vehicle, route.last(), returning, returnDelay, reason))
            return reason;
        route.append(returning);
        dwellSeconds += returnDelay;
    }
    double distance = 0.0, vertical = 0.0;
    for (int i = 0; i < route.size(); ++i) {
        const auto& point = route[i];
        // The first point is measured state, not a commanded destination. Sensor drift after landing
        // can place it slightly below Home; apply the altitude floor to planned destinations only.
        if (!std::isfinite(point.altitude()) || (i > 0 && point.altitude() < home.altitude() - 0.5) ||
            (config.maxAltitudeM > 0 && point.altitude() - home.altitude() > config.maxAltitudeM))
            return QStringLiteral("航线高度超出当前限制");
        const auto& previous = route[qMax(0, i - 1)];
        if (!SwarmConstraintChecker::segmentAllowed({previous.latitude(), previous.longitude()},
                                                    {point.latitude(), point.longitude()}, config))
            return QStringLiteral("航线或转场/返航线段越出当前围栏");
        const bool commandPoint = SwarmTypes::isValidLatLon(config.homeLat, config.homeLon);
        const double range =
            SwarmTypes::haversineM(point.latitude(), point.longitude(), commandPoint ? config.homeLat : home.latitude(),
                                   commandPoint ? config.homeLon : home.longitude());
        if (range > config.commRangeM * config.commErrorFactor)
            return QStringLiteral("航线超出通信覆盖半径");
        distance += previous.distanceTo(point);
        vertical += std::abs(point.altitude() - previous.altitude());
    }
    const double minutes = (distance / config.cruiseSpeedMs + vertical / config.climbRateMs + dwellSeconds) / 60.0;
    const double available = (std::isfinite(batteryPct) && batteryPct >= 0 ? batteryPct : 100.0) * config.reserveFactor;
    if (minutes / config.enduranceMin * 100.0 > available)
        return QStringLiteral("航线估算耗电超过当前电量与返航余量");
    return {};
}

QList<QGeoCoordinate> SwarmMissionSnapshot::formationWaypoints(double homeAltitude, bool& trailingRecovery,
                                                               QString& reason) const
{
    trailingRecovery = false;
    if (!std::isfinite(homeAltitude)) {
        reason = QStringLiteral("长机 Home 高度无效");
        return {};
    }
    QList<QGeoCoordinate> waypoints;
    QStringList unsupported;
    for (int index = 0; index < _items.size(); ++index) {
        const auto& item = _items[index];
        const int command = int(item[0]), frame = int(item[1]);
        if (command == MAV_CMD_NAV_TAKEOFF && index == 0)
            continue;
        if (index == _items.size() - 1 && (command == MAV_CMD_NAV_LAND || command == MAV_CMD_NAV_RETURN_TO_LAUNCH)) {
            trailingRecovery = true;
            continue;
        }
        if (command != MAV_CMD_NAV_WAYPOINT) {
            unsupported.append(QStringLiteral("#%1(指令 %2)").arg(index + 1).arg(command));
            continue;
        }
        if (frame != MAV_FRAME_GLOBAL && frame != MAV_FRAME_GLOBAL_RELATIVE_ALT) {
            unsupported.append(QStringLiteral("#%1(坐标系 %2)").arg(index + 1).arg(frame));
            continue;
        }
        if (!SwarmTypes::isValidLatLon(item[6], item[7]) || !std::isfinite(item[8])) {
            unsupported.append(QStringLiteral("#%1(坐标无效)").arg(index + 1));
            continue;
        }
        if (item[2] != 0.0) {
            unsupported.append(QStringLiteral("#%1(悬停)").arg(index + 1));
            continue;
        }
        if (item[9] != 1.0) {
            unsupported.append(QStringLiteral("#%1(非自动继续)").arg(index + 1));
            continue;
        }
        const double altitudeAmsl = item[8] + (frame == MAV_FRAME_GLOBAL_RELATIVE_ALT ? homeAltitude : 0.0);
        if (altitudeAmsl < homeAltitude - 0.5) {
            unsupported.append(QStringLiteral("#%1(低于 Home)").arg(index + 1));
            continue;
        }
        waypoints.append(QGeoCoordinate(item[6], item[7], altitudeAmsl));
    }
    if (!unsupported.isEmpty()) {
        reason = QStringLiteral(
                     "航线包含编队不支持的项：%1（编队航线只支持全局/相对高度坐标系、无悬停的航点；"
                     "首项起飞、末项降落/返航除外）")
                     .arg(unsupported.join(QStringLiteral("、")));
        return {};
    }
    if (waypoints.isEmpty()) {
        reason = QStringLiteral("航线没有可执行的航点");
        return {};
    }
    return waypoints;
}
