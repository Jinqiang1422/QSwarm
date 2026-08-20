#include "SwarmControlLease.h"

QString SwarmControlLease::requestLease(int vehicleId, const QString &taskId,
                                        const QString &op, qint64 nowMs, qint64 ttlMs)
{
    Lease ls;
    ls.vehicleId = vehicleId;
    ls.taskId    = taskId;
    ls.op        = op;
    ls.state     = LEASED;
    ls.expireMs  = nowMs + ttlMs;
    ls.token     = QStringLiteral("lease-%1-%2").arg(vehicleId).arg(nowMs);
    _leases.insert(vehicleId, ls);
    return ls.token;
}

void SwarmControlLease::onFlightModeChanged(int vehicleId, const QString &mode)
{
    if (!isOperatorMode(mode)) {
        return;
    }
    auto it = _leases.find(vehicleId);
    if (it != _leases.end()) {
        it->state = MANUAL_OVERRIDE;
    } else {
        Lease ls; ls.vehicleId = vehicleId; ls.state = MANUAL_OVERRIDE;
        _leases.insert(vehicleId, ls);
    }
}

bool SwarmControlLease::canAutomate(int vehicleId, qint64 nowMs) const
{
    const auto it = _leases.constFind(vehicleId);
    if (it == _leases.constEnd()) {
        return false;
    }
    return it->state == LEASED && nowMs <= it->expireMs;
}

void SwarmControlLease::release(int vehicleId)
{
    auto it = _leases.find(vehicleId);
    if (it != _leases.end()) {
        it->state = RELEASED;
    }
}

void SwarmControlLease::expireCheck(qint64 nowMs)
{
    for (auto it = _leases.begin(); it != _leases.end(); ++it) {
        if (it->state == LEASED && nowMs > it->expireMs) {
            it->state = RELEASED;
        }
    }
}

SwarmControlLease::State SwarmControlLease::stateOf(int vehicleId) const
{
    const auto it = _leases.constFind(vehicleId);
    return (it == _leases.constEnd()) ? OBSERVE_ONLY : it->state;
}

bool SwarmControlLease::isOperatorMode(const QString &mode)
{
    static const QStringList kAutomation = {
        QStringLiteral("Mission"), QStringLiteral("Offboard")
    };
    for (const QString &a : kAutomation) {
        if (mode.compare(a, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}
