#include "SwarmSituationDigest.h"

#include <QtCore/QJsonArray>

QString SwarmSituationDigest::sourceLabel(Source s)
{
    switch (s) {
        case Source::Telemetry:
            return QStringLiteral("[遥测]");
        case Source::Perception:
            return QStringLiteral("[感知]");
        case Source::Inferred:
            return QStringLiteral("[推测]");
    }
    return QStringLiteral("[未知]");
}

QString SwarmSituationDigest::sourceKey(Source s)
{
    switch (s) {
        case Source::Telemetry:
            return QStringLiteral("telemetry");
        case Source::Perception:
            return QStringLiteral("perception");
        case Source::Inferred:
            return QStringLiteral("inferred");
    }
    return QStringLiteral("unknown");
}

QJsonObject SwarmSituationDigest::build(const QList<SwarmTypes::VehicleState>& vehicles,
                                        const QList<SwarmTrackFusion::FusedTrack>& tracks, const QString& taskStateText,
                                        bool taskRunning, const QList<SwarmEventDetector::Alert>& recentAlerts,
                                        qint64 monoMs)
{
    QJsonObject digest;
    digest[QStringLiteral("generated_ms")] = static_cast<double>(monoMs);

    // ---- 机队（遥测） ----
    QJsonArray fleetArr;
    int online = 0;
    double minBatt = -1.0;
    for (const SwarmTypes::VehicleState& v : vehicles) {
        QJsonObject o;
        o[QStringLiteral("id")] = v.id;
        o[QStringLiteral("online")] = v.online;
        o[QStringLiteral("battery")] = v.batteryPct >= 0.0 ? v.batteryPct : QJsonValue();
        o[QStringLiteral("alt_rel")] = v.altRelM;
        o[QStringLiteral("lat")] = v.lat;
        o[QStringLiteral("lon")] = v.lon;
        fleetArr.append(o);
        if (v.online) {
            online++;
            if (v.batteryPct >= 0.0) {
                minBatt = minBatt < 0.0 ? v.batteryPct : qMin(minBatt, v.batteryPct);
            }
        }
    }
    QJsonObject fleet;
    fleet[QStringLiteral("source")] = sourceKey(Source::Telemetry);
    fleet[QStringLiteral("online")] = online;
    fleet[QStringLiteral("total")] = vehicles.count();
    fleet[QStringLiteral("min_battery")] = minBatt >= 0.0 ? minBatt : QJsonValue();
    fleet[QStringLiteral("vehicles")] = fleetArr;
    digest[QStringLiteral("fleet")] = fleet;

    // ---- 目标航迹（感知） ----
    QJsonArray trackArr;
    for (const SwarmTrackFusion::FusedTrack& t : tracks) {
        QJsonObject o;
        o[QStringLiteral("fused_id")] = t.fusedId;
        o[QStringLiteral("category")] = SwarmTrackFusion::categoryName(t.category);
        o[QStringLiteral("lat")] = t.lat;
        o[QStringLiteral("lon")] = t.lon;
        o[QStringLiteral("speed_ms")] = t.speedMs();
        o[QStringLiteral("confidence")] = t.confidence;
        o[QStringLiteral("state")] = t.state;
        o[QStringLiteral("sensors")] = t.sensors.count();
        trackArr.append(o);
    }
    QJsonObject tracksObj;
    tracksObj[QStringLiteral("source")] = sourceKey(Source::Perception);
    tracksObj[QStringLiteral("count")] = tracks.count();
    tracksObj[QStringLiteral("items")] = trackArr;
    digest[QStringLiteral("tracks")] = tracksObj;

    // ---- 任务状态（遥测/本地状态机） ----
    QJsonObject mission;
    mission[QStringLiteral("source")] = sourceKey(Source::Telemetry);
    mission[QStringLiteral("running")] = taskRunning;
    mission[QStringLiteral("state")] = taskStateText;
    digest[QStringLiteral("mission")] = mission;

    // ---- 告警（规则推导 → 推测） ----
    QJsonArray alertArr;
    for (const SwarmEventDetector::Alert& a : recentAlerts) {
        QJsonObject o;
        o[QStringLiteral("level")] = a.level;
        o[QStringLiteral("rule")] = a.rule;
        o[QStringLiteral("message")] = a.message;
        alertArr.append(o);
    }
    QJsonObject alerts;
    alerts[QStringLiteral("source")] = sourceKey(Source::Inferred);
    alerts[QStringLiteral("count")] = recentAlerts.count();
    alerts[QStringLiteral("items")] = alertArr;
    digest[QStringLiteral("alerts")] = alerts;

    return digest;
}

QString SwarmSituationDigest::oneLiner(const QJsonObject& digest)
{
    const QJsonObject fleet = digest.value(QStringLiteral("fleet")).toObject();
    const QJsonObject tracks = digest.value(QStringLiteral("tracks")).toObject();
    const QJsonObject mission = digest.value(QStringLiteral("mission")).toObject();
    const QJsonObject alerts = digest.value(QStringLiteral("alerts")).toObject();

    QStringList parts;
    parts.append(QStringLiteral("在线 %1/%2 架")
                     .arg(fleet.value(QStringLiteral("online")).toInt())
                     .arg(fleet.value(QStringLiteral("total")).toInt()));
    const QJsonValue mb = fleet.value(QStringLiteral("min_battery"));
    if (mb.isDouble()) {
        parts.append(QStringLiteral("最低电 %1%").arg(mb.toDouble(), 0, 'f', 0));
    }
    parts.append(QStringLiteral("目标 %1 个").arg(tracks.value(QStringLiteral("count")).toInt()));
    parts.append(QStringLiteral("任务:%1").arg(mission.value(QStringLiteral("state")).toString()));
    const int ac = alerts.value(QStringLiteral("count")).toInt();
    if (ac > 0) {
        parts.append(QStringLiteral("告警 %1").arg(ac));
    }
    return parts.join(QStringLiteral("｜"));
}
