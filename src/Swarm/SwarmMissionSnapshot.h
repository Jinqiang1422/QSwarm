#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtPositioning/QGeoCoordinate>

#include "SwarmConstraintChecker.h"

class MissionItem;
class Vehicle;

/// An immutable-by-value copy of mission content, independent of execution state and Home placeholders.
class SwarmMissionSnapshot
{
public:
    static SwarmMissionSnapshot capture(const QList<MissionItem*>& items, bool hasHome);
    bool equivalent(const SwarmMissionSnapshot& other) const;

    bool empty() const { return _items.isEmpty(); }

    QString fingerprint() const;
    QString validate(Vehicle* vehicle, const SwarmConstraintChecker::Config& config, double batteryPct) const;

    /// 编队航线可用的航点（AMSL 高度）：首项起飞跳过；末项降落/返航不由编队执行（置 trailingRecovery）；
    /// 其余必须是全局或相对高度坐标系的 NAV_WAYPOINT。不满足时返回空并写 reason。
    QList<QGeoCoordinate> formationWaypoints(double homeAltitude, bool& trailingRecovery, QString& reason) const;

private:
    QList<QList<double>> _items;
};
