/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmAction
 *
 * 集群动作的统一表示：大模型（SwarmAIAgent）、任务状态机（SwarmTaskController）
 * 与人工面板共用同一套动作结构与同一条解析/校验/执行管线（见 SwarmManager）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QString>
#include <QtCore/QVariantMap>

struct SwarmAction
{
    QString type;         ///< set_selection / arm / takeoff / land / rtl / pause /
                          ///< start_mission / disarm / emergency_disarm /
                          ///< start_formation / stop_formation / plan_coverage / clear_missions
    QVariantMap params;
    QString description;  ///< 中文描述，用于计划确认界面
    bool dangerous = false;

    /// 危险动作集合（需人工确认后才可执行）
    static bool isDangerousType(const QString& type)
    {
        static const QSet<QString> dangerousTypes = {QStringLiteral("arm"),
                                                     QStringLiteral("takeoff"),
                                                     QStringLiteral("land"),
                                                     QStringLiteral("rtl"),
                                                     QStringLiteral("start_mission"),
                                                     QStringLiteral("disarm"),
                                                     QStringLiteral("emergency_disarm"),
                                                     QStringLiteral("start_formation")};
        return dangerousTypes.contains(type);
    }
};
