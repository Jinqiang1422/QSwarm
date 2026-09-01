/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmSituationDigest（WP3.5 态势摘要）
 *
 * 周期性把「机队遥测 + 感知航迹 + 任务状态 + 告警」打包成结构化 JSON，
 * 供 LLM 生成自然语言态势摘要与应答态势提问。
 *
 * 防幻觉设计：每个事实块携带 source 字段（telemetry 遥测 / perception 感知 /
 * inferred 推测），LLM 系统提示词强制「事实性陈述必须标注来源」；
 * sourceLabel() 提供统一的中文角标，UI 与提示词共用。
 *
 * 仅依赖 QtCore，可独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include "SwarmEventDetector.h"
#include "SwarmTrackFusion.h"
#include "SwarmTypes.h"

class SwarmSituationDigest
{
public:
    /// 事实来源（与 WP3.5 契约一致）
    enum class Source
    {
        Telemetry,   ///< 遥测（机体状态）
        Perception,  ///< 感知（检测/融合航迹）
        Inferred     ///< 推测（规则推导/估算）
    };

    /// 来源中文角标：[遥测] / [感知] / [推测]
    static QString sourceLabel(Source s);
    static QString sourceKey(Source s);  ///< "telemetry" / "perception" / "inferred"

    /// 构建结构化态势摘要：
    /// {
    ///   "generated_ms": ..., "fleet": {"source":"telemetry", "online":N, "vehicles":[...]},
    ///   "tracks": {"source":"perception", "count":N, "items":[...]},
    ///   "mission": {"source":"telemetry", "state":"...", "running":bool},
    ///   "alerts":  {"source":"inferred", "count":N, "items":[...]},
    ///   "notes":   ["..."]
    /// }
    static QJsonObject build(const QList<SwarmTypes::VehicleState>& vehicles,
                             const QList<SwarmTrackFusion::FusedTrack>& tracks, const QString& taskStateText,
                             bool taskRunning, const QList<SwarmEventDetector::Alert>& recentAlerts, qint64 monoMs);

    /// 紧凑单行摘要（面板状态栏/LLM 上下文复用）：
    /// "在线6架｜最低电72%｜目标3个(车辆2/人员1)｜任务:区域搜索中｜告警1"
    static QString oneLiner(const QJsonObject& digest);
};
