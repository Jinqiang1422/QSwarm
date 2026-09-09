/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmEventDetector（WP3.6 异常事件检测）
 *
 * 规则引擎，对两类异常出警：
 *   平台异常：低电量（critical<20% / warning<30%）、离线/链路退化、
 *             机体越出空域围栏（include 之外 / exclude 之内）
 *   目标异常：聚集（≥gatherCount 个目标间距 <gatherRadiusM）、越界
 *            （进入禁飞区/离开允许区）、速度突变（|Δv|>speedBurstMs）
 *
 * 告警去抖：同一 rule+对象在 cooldownMs 内不重复出警；critical 不过冷却。
 * 仅依赖 QtCore，可脱离 QGC 独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVariantList>

#include "SwarmTrackFusion.h"
#include "SwarmTypes.h"

class SwarmEventDetector
{
public:
    struct Alert
    {
        QString level;            ///< info / warning / critical
        QString rule;             ///< low_battery / offline / fence_breach /
                                  ///< gathering / boundary / speed_burst
        QString message;
        QVariantList relatedIds;  ///< 相关机体/航迹 ID
        qint64 monoMs = 0;
    };

    struct Config
    {
        double batteryCriticalPct = 20.0;
        double batteryWarningPct = 30.0;
        double gatherRadiusM = 50.0;
        int gatherCount = 3;
        double speedBurstMs = 8.0;  ///< |Δv| 阈值（m/s）
        double overspeedMs = 25.0;  ///< 绝对超速阈值（m/s）
        qint64 cooldownMs = 30000;
    };

    SwarmEventDetector();
    explicit SwarmEventDetector(const Config& cfg);

    void setConfig(const Config& cfg) { _cfg = cfg; }

    /// 空域围栏（与约束引擎 fences.json 同构），为空则跳过 fence 类检查
    void setFences(const QList<QList<QPair<double, double>>>& includePolys,
                   const QList<QList<QPair<double, double>>>& excludePolys);

    /// 平台侧检查：输入机体状态快照，返回新告警（内部维护在线/速度历史）
    QList<Alert> checkVehicles(const QList<SwarmTypes::VehicleState>& vehicles, qint64 monoMs);

    /// 目标侧检查：输入融合航迹清单
    QList<Alert> checkTracks(const QList<SwarmTrackFusion::FusedTrack>& tracks, qint64 monoMs);

    /// 告警 → 单行文本（UI/审计共用）
    static QString alertToText(const Alert& a);

    int cooldownRecordCount() const { return _lastFired.size(); }

    int speedHistoryCount() const { return _lastSpeed.size(); }

    quint64 capacityRejectedCount() const { return _capacityRejectedCount; }

private:
    bool _cooldownHit(const QString& key, const QString& level, qint64 monoMs);
    void _pruneCooldown(qint64 monoMs, const QSet<int>& alive, bool tracks);
    bool _fenceBreach(double lat, double lon) const;

    Config _cfg;
    QList<QList<QPair<double, double>>> _includePolys, _excludePolys;
    QHash<int, bool> _wasOnline;        ///< vehicleId → 上次在线
    QHash<int, double> _lastSpeed;      ///< fusedId → 上次地速
    QHash<QString, qint64> _lastFired;  ///< 去抖键 → 上次出警时刻
    quint64 _capacityRejectedCount = 0;
};
