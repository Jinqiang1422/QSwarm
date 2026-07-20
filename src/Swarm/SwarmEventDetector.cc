#include "SwarmEventDetector.h"

#include <QtCore/QtMath>

SwarmEventDetector::SwarmEventDetector()
    : SwarmEventDetector(Config())
{
}

SwarmEventDetector::SwarmEventDetector(const Config &cfg)
    : _cfg(cfg)
{
}

void SwarmEventDetector::setFences(const QList<QList<QPair<double, double>>> &includePolys,
                                   const QList<QList<QPair<double, double>>> &excludePolys)
{
    _includePolys = includePolys;
    _excludePolys = excludePolys;
}

bool SwarmEventDetector::_cooldownHit(const QString &key, const QString &level, qint64 monoMs)
{
    if (level == QStringLiteral("critical")) {
        _lastFired[key] = monoMs;                       // critical 不过冷却，每次都报
        return false;
    }
    const qint64 last = _lastFired.value(key, -1);
    if (last >= 0 && monoMs - last < _cfg.cooldownMs) {
        return true;
    }
    _lastFired[key] = monoMs;
    return false;
}

QString SwarmEventDetector::alertToText(const Alert &a)
{
    const QString icon = a.level == QStringLiteral("critical") ? QStringLiteral("🔴")
                       : a.level == QStringLiteral("warning")  ? QStringLiteral("🟠")
                                                               : QStringLiteral("🔵");
    return QStringLiteral("%1 [%2] %3").arg(icon, a.rule, a.message);
}

QList<SwarmEventDetector::Alert> SwarmEventDetector::checkVehicles(
        const QList<SwarmTypes::VehicleState> &vehicles, qint64 monoMs)
{
    QList<Alert> out;
    for (const SwarmTypes::VehicleState &v : vehicles) {
        // ---- 低电量 ----
        if (v.batteryPct >= 0.0) {
            if (v.batteryPct < _cfg.batteryCriticalPct
                    && !_cooldownHit(QStringLiteral("batt_c_%1").arg(v.id), QStringLiteral("critical"), monoMs)) {
                out.append({QStringLiteral("critical"), QStringLiteral("low_battery"),
                            QStringLiteral("机 %1 电量 %2%，低于紧急阈值 %3%")
                                .arg(v.id).arg(v.batteryPct, 0, 'f', 0).arg(_cfg.batteryCriticalPct),
                            {v.id}, monoMs});
            } else if (v.batteryPct < _cfg.batteryWarningPct
                    && !_cooldownHit(QStringLiteral("batt_w_%1").arg(v.id), QStringLiteral("warning"), monoMs)) {
                out.append({QStringLiteral("warning"), QStringLiteral("low_battery"),
                            QStringLiteral("机 %1 电量 %2%，低于预警阈值 %3%")
                                .arg(v.id).arg(v.batteryPct, 0, 'f', 0).arg(_cfg.batteryWarningPct),
                            {v.id}, monoMs});
            }
        }
        // ---- 离线/链路退化（在线→离线跳变） ----
        const bool was = _wasOnline.value(v.id, true);
        if (was && !v.online
                && !_cooldownHit(QStringLiteral("off_%1").arg(v.id), QStringLiteral("warning"), monoMs)) {
            out.append({QStringLiteral("warning"), QStringLiteral("offline"),
                        QStringLiteral("机 %1 链路中断（离线）").arg(v.id), {v.id}, monoMs});
        }
        _wasOnline[v.id] = v.online;
        // ---- 机体越出空域围栏 ----
        if (v.online && (!_includePolys.isEmpty() || !_excludePolys.isEmpty())) {
            bool breach = false;
            if (!_includePolys.isEmpty()) {
                bool insideAny = false;
                for (const auto &poly : _includePolys) {
                    if (SwarmTypes::pointInPolygon(v.lat, v.lon, poly)) { insideAny = true; break; }
                }
                breach = !insideAny;
            }
            if (!breach) {
                for (const auto &poly : _excludePolys) {
                    if (SwarmTypes::pointInPolygon(v.lat, v.lon, poly)) { breach = true; break; }
                }
            }
            if (breach && !_cooldownHit(QStringLiteral("fence_%1").arg(v.id), QStringLiteral("warning"), monoMs)) {
                out.append({QStringLiteral("warning"), QStringLiteral("fence_breach"),
                            QStringLiteral("机 %1 越出空域围栏").arg(v.id), {v.id}, monoMs});
            }
        }
    }
    return out;
}

QList<SwarmEventDetector::Alert> SwarmEventDetector::checkTracks(
        const QList<SwarmTrackFusion::FusedTrack> &tracks, qint64 monoMs)
{
    QList<Alert> out;

    // ---- 聚集：任取 gatherCount 个目标两两互在半径内（简化为存在性判定） ----
    for (int i = 0; i < tracks.count(); i++) {
        QList<int> cluster{tracks[i].fusedId};
        for (int j = 0; j < tracks.count(); j++) {
            if (i == j) {
                continue;
            }
            const double d = SwarmTypes::haversineM(tracks[i].lat, tracks[i].lon,
                                                    tracks[j].lat, tracks[j].lon);
            if (d < _cfg.gatherRadiusM) {
                cluster.append(tracks[j].fusedId);
            }
        }
        if (cluster.count() >= _cfg.gatherCount) {
            std::sort(cluster.begin(), cluster.end());
            const QString key = QStringLiteral("gather_%1").arg(
                QString::number(cluster.first()) + QStringLiteral("_") + QString::number(cluster.count()));
            if (!_cooldownHit(key, QStringLiteral("warning"), monoMs)) {
                QVariantList ids;
                for (int id : cluster) { ids.append(id); }
                out.append({QStringLiteral("warning"), QStringLiteral("gathering"),
                            QStringLiteral("检测到 %1 个目标聚集（半径 %2 米内）")
                                .arg(cluster.count()).arg(_cfg.gatherRadiusM),
                            ids, monoMs});
            }
            break;                                      // 一轮只报一个聚集簇
        }
    }

    for (const SwarmTrackFusion::FusedTrack &t : tracks) {
        // ---- 目标越界 ----
        if (!_excludePolys.isEmpty() || !_includePolys.isEmpty()) {
            bool breach = false;
            for (const auto &poly : _excludePolys) {
                if (SwarmTypes::pointInPolygon(t.lat, t.lon, poly)) { breach = true; break; }
            }
            if (breach && !_cooldownHit(QStringLiteral("tb_%1").arg(t.fusedId), QStringLiteral("warning"), monoMs)) {
                out.append({QStringLiteral("warning"), QStringLiteral("boundary"),
                            QStringLiteral("目标 #%1（%2）进入禁飞区")
                                .arg(t.fusedId).arg(SwarmTrackFusion::categoryName(t.category)),
                            {t.fusedId}, monoMs});
            }
        }
        // ---- 速度突变 / 绝对超速 ----
        const double spd = t.speedMs();
        const double lastSpd = _lastSpeed.value(t.fusedId, -1.0);
        if (lastSpd >= 0.0 && qAbs(spd - lastSpd) > _cfg.speedBurstMs
                && !_cooldownHit(QStringLiteral("sb_%1").arg(t.fusedId), QStringLiteral("warning"), monoMs)) {
            out.append({QStringLiteral("warning"), QStringLiteral("speed_burst"),
                        QStringLiteral("目标 #%1 速度突变：%2 → %3 m/s")
                            .arg(t.fusedId).arg(lastSpd, 0, 'f', 1).arg(spd, 0, 'f', 1),
                        {t.fusedId}, monoMs});
        } else if (spd > _cfg.overspeedMs
                && !_cooldownHit(QStringLiteral("os_%1").arg(t.fusedId), QStringLiteral("warning"), monoMs)) {
            out.append({QStringLiteral("warning"), QStringLiteral("speed_burst"),
                        QStringLiteral("目标 #%1 超速移动（%2 m/s）")
                            .arg(t.fusedId).arg(spd, 0, 'f', 1),
                        {t.fusedId}, monoMs});
        }
        _lastSpeed[t.fusedId] = spd;
    }
    return out;
}
