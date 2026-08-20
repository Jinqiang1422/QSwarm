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
    setManualOverride(vehicleId, isOperatorMode(mode));
}

void SwarmControlLease::setManualOverride(int vehicleId, bool overridden)
{
    auto it = _leases.find(vehicleId);
    if (overridden) {
        if (it != _leases.end()) {
            it->state = MANUAL_OVERRIDE;
        } else {
            Lease ls; ls.vehicleId = vehicleId; ls.state = MANUAL_OVERRIDE;
            _leases.insert(vehicleId, ls);
        }
    } else if (it != _leases.end() && it->state == MANUAL_OVERRIDE) {
        it->state = OBSERVE_ONLY;   // 操作员交还控制 → 解除接管,可重新申请租约
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
    // 自主/系统模式白名单(中英双语,覆盖 PX4 本地化译名):这些不算操作员接管。
    // 其余(操作员手动直控模式如 Position/Manual,及未知模式)保守视为接管。
    static const QStringList kAutonomous = {
        QStringLiteral("Mission"), QStringLiteral("Hold"),   QStringLiteral("Takeoff"),
        QStringLiteral("Land"),    QStringLiteral("Return"), QStringLiteral("Offboard"),
        QStringLiteral("Loiter"),  QStringLiteral("RTL"),
        QStringLiteral("任务"),    QStringLiteral("悬停"),   QStringLiteral("起飞"),
        QStringLiteral("降落"),    QStringLiteral("返航"),   QStringLiteral("保持")
    };
    for (const QString &a : kAutonomous) {
        if (mode.compare(a, Qt::CaseInsensitive) == 0) {
            return false;   // 自主模式 → 非接管
        }
    }
    return true;   // 操作员手动模式 + 未知 → 保守视为接管
}
