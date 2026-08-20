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
    /// 直接设置/解除某机的人工接管态(overridden=true→MANUAL_OVERRIDE;false→若当前
    /// 接管则解除为 OBSERVE_ONLY,可重新申请)。供集成层用固件规范模式判断后调用。
    void setManualOverride(int vehicleId, bool overridden);
    bool canAutomate(int vehicleId, qint64 nowMs) const;
    void release(int vehicleId);
    void expireCheck(qint64 nowMs);
    State stateOf(int vehicleId) const;

    /// 飞行模式是否属操作员接管模式:自主模式白名单(Mission/Hold/Takeoff/Land/RTL/
    /// Offboard,中英双语覆盖 PX4 本地化译名)之外一律保守视为接管(含未知模式)。
    static bool isOperatorMode(const QString &mode);

private:
    QMap<int, Lease> _leases;
};
