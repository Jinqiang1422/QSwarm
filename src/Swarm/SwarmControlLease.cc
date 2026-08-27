#include "SwarmControlLease.h"

QString SwarmControlLease::requestLease(int vehicleId, const QString& taskId, const QString& op, qint64 nowMs,
                                        qint64 ttlMs)
{
    const auto existing = _leases.constFind(vehicleId);
    if (vehicleId <= 0 || ttlMs <= 0 || (existing != _leases.constEnd() && existing->state == MANUAL_OVERRIDE)) {
        return {};
    }

    Lease ls;
    ls.vehicleId = vehicleId;
    ls.taskId = taskId;
    ls.op = op;
    ls.state = LEASED;
    ls.expireMs = nowMs + ttlMs;
    ls.token = QStringLiteral("lease-%1-%2").arg(vehicleId).arg(nowMs);
    _leases.insert(vehicleId, ls);
    return ls.token;
}

bool SwarmControlLease::renewLease(int vehicleId, qint64 nowMs, qint64 ttlMs)
{
    auto it = _leases.find(vehicleId);
    if (ttlMs <= 0 || it == _leases.end() || it->state != LEASED || nowMs > it->expireMs) {
        return false;
    }
    it->expireMs = nowMs + ttlMs;
    return true;
}

void SwarmControlLease::setManualOverride(int vehicleId, bool overridden)
{
    if (vehicleId <= 0) {
        return;
    }
    auto it = _leases.find(vehicleId);
    if (overridden) {
        if (it != _leases.end()) {
            it->state = MANUAL_OVERRIDE;
        } else {
            Lease ls;
            ls.vehicleId = vehicleId;
            ls.state = MANUAL_OVERRIDE;
            _leases.insert(vehicleId, ls);
        }
    } else if (it != _leases.end() && it->state == MANUAL_OVERRIDE) {
        it->state = OBSERVE_ONLY;  // 操作员交还控制 → 解除接管,可重新申请租约
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
    if (it != _leases.end() && it->state != MANUAL_OVERRIDE) {
        it->state = RELEASED;
    }
}

void SwarmControlLease::forgetVehicle(int vehicleId)
{
    _leases.remove(vehicleId);
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

bool SwarmControlLease::isAutonomousMode(const QString& mode, const QStringList& autonomousModes)
{
    if (mode.isEmpty()) {
        return false;
    }
    for (const QString& a : autonomousModes) {
        if (!a.isEmpty() && mode.compare(a, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;  // 集合外:操作员手动直控 / Offboard 外部控制 / 未知 → 让路
}
