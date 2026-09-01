/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmRehearsalEstimator（WP2.7 仿真预演 v1）
 *
 * 解析式任务预演：不经 SITL，直接对动作序列估算
 *   预计完成时长 / 总里程 / 最低电量裕度，
 * 供计划确认前的指标展示与多候选方案对比（WP2.2）。
 * SITL 快速预演（真飞行动力学）为后续增强。
 *
 * 仅依赖 QtCore + SwarmAction/SwarmTypes，可独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "SwarmAction.h"
#include "SwarmTypes.h"

class SwarmRehearsalEstimator
{
public:
    struct Config
    {
        double cruiseSpeedMs = 12.0;
        double climbRateMs = 2.5;
        double descentRateMs = 1.5;
        double enduranceMin = 25.0;  ///< 满电续航
    };

    struct Estimate
    {
        double durationMin = 0.0;         ///< 预计总时长（顺序执行累计）
        double distanceKm = 0.0;          ///< 全机队总里程
        double batteryMarginPct = 100.0;  ///< 最差机剩余电量裕度
        int vehiclesUsed = 0;
        QStringList notes;                ///< 假设与未知项说明
    };

    Estimate estimate(const QList<SwarmAction>& plan, const QList<SwarmTypes::VehicleState>& vehicles,
                      double mapCenterLat, double mapCenterLon) const;
    Estimate estimate(const QList<SwarmAction>& plan, const QList<SwarmTypes::VehicleState>& vehicles,
                      double mapCenterLat, double mapCenterLon, const Config& cfg) const;

    /// 摘要串，如 "≈12.3 分钟｜8.4 km｜最低裕度 31%"
    static QString summary(const Estimate& est);
};
