#pragma once
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>

// per-vehicle 控制租约状态机(纯 QtCore,可独立单测)。
// 语义:自动化路径下发前对涉及机 requestLease;操作员在 QGC 切飞行模式即自动释放。
class SwarmControlLease
{
public:
    enum State
    {
        OBSERVE_ONLY,
        LEASE_REQUESTED,
        LEASED,
        RELEASED,
        MANUAL_OVERRIDE
    };

    struct Lease
    {
        int vehicleId = 0;
        QString taskId;
        QString op;  ///< 操作员
        State state = OBSERVE_ONLY;
        qint64 expireMs = 0;
        QString token;
    };

    QString requestLease(int vehicleId, const QString& taskId, const QString& op, qint64 nowMs, qint64 ttlMs = 300000);
    /// 仅续期仍有效的租约；已过期或人工接管时拒绝续期。
    bool renewLease(int vehicleId, qint64 nowMs, qint64 ttlMs = 300000);
    /// 设置/解除某机的人工接管态。overridden=true→MANUAL_OVERRIDE;false→若当前接管则
    /// 解除为 OBSERVE_ONLY。解除后需自动化环下一轮 requestLease(由 executeActions 负责)
    /// 才会重新 canAutomate。集成层仅在已有租约时把集合外模式视为接管，并用 Vehicle/FirmwarePlugin
    /// 规范飞行模式访问器 + isAutonomousMode 判定 overridden，保证 locale 安全。
    void setManualOverride(int vehicleId, bool overridden);
    bool canAutomate(int vehicleId, qint64 nowMs) const;
    void release(int vehicleId);
    void forgetVehicle(int vehicleId);
    void expireCheck(qint64 nowMs);
    State stateOf(int vehicleId) const;

    /// mode 是否属于给定的自主模式集合。集成层从 Vehicle 的 missionFlightMode()/
    /// pauseFlightMode()/rtlFlightMode()/smartRTLFlightMode()/landFlightMode()/gotoFlightMode()/
    /// followFlightMode() 及 FirmwarePlugin::takeOffFlightMode() 取集合——与 flightMode() 同 tr() 上下文、
    /// locale 一致。集合外(操作员手动直控 / Offboard 外部控制 / 未知)视为让路。
    /// 不在本地化显示名上硬编码字面量(会随语言失配)。
    static bool isAutonomousMode(const QString& mode, const QStringList& autonomousModes);

private:
    QMap<int, Lease> _leases;
};
