#pragma once

#include <QtCore/QList>
#include <QtCore/QString>

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

private:
    QList<QList<double>> _items;
};
