/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmAutonomy（WP1.3 自主分级形式化）
 *
 * 自主等级（Autonomy Level）定义与行为矩阵：
 *   L1 仅建议     —— AI 只生成计划与建议并展示，一律不下发执行（操作员手动直控不受限）
 *   L2 监督执行   —— AI 计划经人工确认后下发；含危险动作的计划强制挂起确认（默认档位）
 *   L3 限定域自主 —— 白名单任务域内的动作自动执行（仍过统一校验闸门 + 全程审计）；
 *                    域外危险动作仍需人工确认；紧急类动作永不自动执行
 *
 * 说明：
 *   - 本头文件仅依赖 QtCore + SwarmAction.h，可脱离 QGC 独立单元测试（tests/cpp）。
 *   - 等级切换、白名单裁剪由操作员在界面完成并持久化（SwarmManager 托管）。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include "SwarmAction.h"

namespace SwarmAutonomy {

enum class Level
{
    L1Suggest = 1,     ///< 仅建议
    L2Supervised = 2,  ///< 监督执行（人工确认后下发）
    L3Bounded = 3      ///< 限定域自主
};

inline QString levelName(Level level)
{
    switch (level) {
        case Level::L1Suggest:
            return QStringLiteral("L1 仅建议");
        case Level::L2Supervised:
            return QStringLiteral("L2 监督执行");
        case Level::L3Bounded:
            return QStringLiteral("L3 限定域自主");
    }
    return QStringLiteral("未知");
}

inline QString levelDescription(Level level)
{
    switch (level) {
        case Level::L1Suggest:
            return QStringLiteral("AI 只生成计划与建议并展示，一律不下发执行；操作员手动直控不受限。");
        case Level::L2Supervised:
            return QStringLiteral("AI 计划经人工确认后下发；含危险动作的计划强制挂起确认（默认档位）。");
        case Level::L3Bounded:
            return QStringLiteral(
                "白名单任务域内的动作自动执行（仍过统一校验闸门并全程审计）；"
                "域外危险动作仍需人工确认；紧急类动作永不自动执行。");
    }
    return QString();
}

/// L3 默认自主白名单：放开除“紧急上锁”外的全部危险动作，操作员可按任务域裁剪
inline QStringList defaultL3AllowedTypes()
{
    return {QStringLiteral("arm"),
            QStringLiteral("takeoff"),
            QStringLiteral("land"),
            QStringLiteral("rtl"),
            QStringLiteral("start_mission"),
            QStringLiteral("disarm"),
            QStringLiteral("start_formation")};
}

/// 任何等级下都绝不自动执行的动作（空中强制上锁等同坠机，只能人工显式触发）
inline QStringList neverAutoTypes()
{
    return {QStringLiteral("emergency_disarm")};
}

/// AI 侧执行是否被当前等级整体禁止（仅 L1）
inline bool aiExecutionBlocked(Level level)
{
    return level == Level::L1Suggest;
}

/// 判定动作序列在当前等级下是否需要人工确认挂起。
/// 返回 true 时 reason 给出触发原因（供界面显示与审计记录）。
inline bool needsConfirmation(Level level, const QStringList& l3AllowedTypes, const QList<SwarmAction>& actions,
                              QString* reason = nullptr)
{
    if (actions.isEmpty()) {
        return false;
    }

    if (level == Level::L1Suggest) {
        if (reason) {
            *reason = QStringLiteral("L1 仅建议档位：AI 计划仅展示，禁止下发执行");
        }
        return true;
    }

    QStringList dangerousTypes;
    for (const SwarmAction& action : actions) {
        if (action.dangerous && !dangerousTypes.contains(action.type)) {
            dangerousTypes.append(action.type);
        }
    }
    if (dangerousTypes.isEmpty()) {
        return false;  // 无危险动作，L2/L3 均可直接执行
    }

    if (level == Level::L2Supervised) {
        if (reason) {
            *reason = QStringLiteral("L2 监督执行：计划含危险动作（%1），需人工确认")
                          .arg(dangerousTypes.join(QStringLiteral("、")));
        }
        return true;
    }

    // L3：域内白名单自动执行；域外危险动作与“永不自动”动作需确认
    QStringList blockedTypes;
    for (const QString& type : dangerousTypes) {
        if (neverAutoTypes().contains(type) || !l3AllowedTypes.contains(type)) {
            blockedTypes.append(type);
        }
    }
    if (!blockedTypes.isEmpty()) {
        if (reason) {
            *reason = QStringLiteral("L3 限定域自主：动作（%1）不在自主白名单内，需人工确认")
                          .arg(blockedTypes.join(QStringLiteral("、")));
        }
        return true;
    }
    return false;
}

}  // namespace SwarmAutonomy
