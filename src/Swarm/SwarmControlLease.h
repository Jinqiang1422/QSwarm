#pragma once
#include <QtCore/QMap>
#include <QtCore/QString>

// per-vehicle 控制租约状态机(纯 QtCore,可独立单测)。
// 语义:自动化路径下发前对涉及机 requestLease;操作员在 QGC 切飞行模式即自动释放。
class SwarmControlLease
{
public:
    enum State { OBSERVE_ONLY, LEASE_REQUESTED, LEASED, RELEASED, MANUAL_OVERRIDE };
    struct Lease {
        int     vehicleId = 0;
        QString taskId;
        QString op;                 ///< 操作员
        State   state = OBSERVE_ONLY;
        qint64  expireMs = 0;
        QString token;
    };

    QString requestLease(int vehicleId, const QString &taskId, const QString &op,
                         qint64 nowMs, qint64 ttlMs = 300000);
    void onFlightModeChanged(int vehicleId, const QString &mode);
    bool canAutomate(int vehicleId, qint64 nowMs) const;
    void release(int vehicleId);
    void expireCheck(qint64 nowMs);
    State stateOf(int vehicleId) const;

    /// 飞行模式是否属操作员接管模式(非 Mission/Offboard;未知一律视为操作员=保守)。
    static bool isOperatorMode(const QString &mode);

private:
    QMap<int, Lease> _leases;
};
