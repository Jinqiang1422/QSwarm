#include "SwarmEventDetector.h"

#include <QtCore/QtMath>

namespace {
// Cartesian cells avoid longitude wrap and polar singularities. Every spherical neighbour within
// radius also has a chord shorter than radius; the final distance test retains the original metric.
struct Cell
{
    int x;
    int y;
    int z;
};

Cell cellFor(double lat, double lon, double width)
{
    const double latitude = SwarmTypes::degToRad(lat), longitude = SwarmTypes::degToRad(lon);
    constexpr double earthRadius = 6371000.0;
    return {int(std::floor(earthRadius * std::cos(latitude) * std::cos(longitude) / width)),
            int(std::floor(earthRadius * std::cos(latitude) * std::sin(longitude) / width)),
            int(std::floor(earthRadius * std::sin(latitude) / width))};
}

QString cellKey(const Cell& cell)
{
    return QStringLiteral("%1:%2:%3").arg(cell.x).arg(cell.y).arg(cell.z);
}
}  // namespace

SwarmEventDetector::SwarmEventDetector() : SwarmEventDetector(Config()) {}

SwarmEventDetector::SwarmEventDetector(const Config& cfg) : _cfg(cfg) {}

void SwarmEventDetector::setFences(const QList<QList<QPair<double, double>>>& includePolys,
                                   const QList<QList<QPair<double, double>>>& excludePolys)
{
    _includePolys = includePolys;
    _excludePolys = excludePolys;
}

bool SwarmEventDetector::_cooldownHit(const QString& key, const QString& level, qint64 monoMs)
{
    if (level == QStringLiteral("critical")) {
        return false;
    }
    const qint64 last = _lastFired.value(key, -1);
    if (last >= 0 && monoMs - last < _cfg.cooldownMs) {
        return true;
    }
    if (!_lastFired.contains(key) && _lastFired.size() >= 8192) {
        ++_capacityRejectedCount;
        return true;
    }
    _lastFired[key] = monoMs;
    return false;
}

void SwarmEventDetector::_pruneCooldown(qint64 monoMs, const QSet<int>& alive, bool tracks)
{
    for (auto it = _lastFired.begin(); it != _lastFired.end();) {
        const auto parts = it.key().split(QLatin1Char('_'));
        const auto prefix = parts.first();
        const bool isTrack = prefix == QStringLiteral("tb") || prefix == QStringLiteral("sb") ||
                             prefix == QStringLiteral("os") || prefix == QStringLiteral("gather");
        const int id = prefix == QStringLiteral("gather") ? parts.value(1).toInt() : parts.last().toInt();
        if (monoMs - it.value() >= _cfg.cooldownMs || (tracks == isTrack && !alive.contains(id)))
            it = _lastFired.erase(it);
        else
            ++it;
    }
}

bool SwarmEventDetector::_fenceBreach(double lat, double lon) const
{
    if (!SwarmTypes::isFiniteLatLon(lat, lon))
        return false;
    bool permitted = _includePolys.isEmpty();
    for (const auto& polygon : _includePolys)
        permitted |= SwarmTypes::pointInPolygon(lat, lon, polygon);
    for (const auto& polygon : _excludePolys)
        if (SwarmTypes::pointInPolygon(lat, lon, polygon))
            return true;
    return !permitted;
}

QString SwarmEventDetector::alertToText(const Alert& a)
{
    const QString icon = a.level == QStringLiteral("critical")  ? QStringLiteral("🔴")
                         : a.level == QStringLiteral("warning") ? QStringLiteral("🟠")
                                                                : QStringLiteral("🔵");
    return QStringLiteral("%1 [%2] %3").arg(icon, a.rule, a.message);
}

QList<SwarmEventDetector::Alert> SwarmEventDetector::checkVehicles(const QList<SwarmTypes::VehicleState>& vehicles,
                                                                   qint64 monoMs)
{
    QList<Alert> out;
    QSet<int> alive;
    for (const auto& vehicle : vehicles)
        alive.insert(vehicle.id);
    _wasOnline.removeIf([&alive](auto it) { return !alive.contains(it.key()); });
    _pruneCooldown(monoMs, alive, false);
    for (const SwarmTypes::VehicleState& v : vehicles) {
        // ---- 低电量 ----
        if (v.batteryPct >= 0.0) {
            if (v.batteryPct < _cfg.batteryCriticalPct &&
                !_cooldownHit(QStringLiteral("batt_c_%1").arg(v.id), QStringLiteral("critical"), monoMs)) {
                out.append({QStringLiteral("critical"),
                            QStringLiteral("low_battery"),
                            QStringLiteral("机 %1 电量 %2%，低于紧急阈值 %3%")
                                .arg(v.id)
                                .arg(v.batteryPct, 0, 'f', 0)
                                .arg(_cfg.batteryCriticalPct),
                            {v.id},
                            monoMs});
            } else if (v.batteryPct < _cfg.batteryWarningPct &&
                       !_cooldownHit(QStringLiteral("batt_w_%1").arg(v.id), QStringLiteral("warning"), monoMs)) {
                out.append({QStringLiteral("warning"),
                            QStringLiteral("low_battery"),
                            QStringLiteral("机 %1 电量 %2%，低于预警阈值 %3%")
                                .arg(v.id)
                                .arg(v.batteryPct, 0, 'f', 0)
                                .arg(_cfg.batteryWarningPct),
                            {v.id},
                            monoMs});
            }
        }
        // ---- 离线/链路退化（在线→离线跳变） ----
        const bool was = _wasOnline.value(v.id, true);
        if (was && !v.online && !_cooldownHit(QStringLiteral("off_%1").arg(v.id), QStringLiteral("warning"), monoMs)) {
            out.append({QStringLiteral("warning"),
                        QStringLiteral("offline"),
                        QStringLiteral("机 %1 链路中断（离线）").arg(v.id),
                        {v.id},
                        monoMs});
        }
        _wasOnline[v.id] = v.online;
        // ---- 机体越出空域围栏 ----
        if (v.online && (!_includePolys.isEmpty() || !_excludePolys.isEmpty())) {
            const bool breach = _fenceBreach(v.lat, v.lon);
            if (breach && !_cooldownHit(QStringLiteral("fence_%1").arg(v.id), QStringLiteral("warning"), monoMs)) {
                out.append({QStringLiteral("warning"),
                            QStringLiteral("fence_breach"),
                            QStringLiteral("机 %1 越出空域围栏").arg(v.id),
                            {v.id},
                            monoMs});
            }
        }
    }
    return out;
}

QList<SwarmEventDetector::Alert> SwarmEventDetector::checkTracks(const QList<SwarmTrackFusion::FusedTrack>& tracks,
                                                                 qint64 monoMs)
{
    QList<Alert> out;
    QSet<int> alive;
    QHash<QString, QList<int>> grid;
    const double width = std::isfinite(_cfg.gatherRadiusM) ? qMax(0.01, _cfg.gatherRadiusM) : 50.0;
    for (int i = 0; i < tracks.size(); ++i) {
        if (!SwarmTypes::isFiniteLatLon(tracks[i].lat, tracks[i].lon))
            continue;
        alive.insert(tracks[i].fusedId);
        grid[cellKey(cellFor(tracks[i].lat, tracks[i].lon, width))].append(i);
    }
    _lastSpeed.removeIf([&alive](auto it) { return !alive.contains(it.key()); });
    _pruneCooldown(monoMs, alive, true);

    // ---- 聚集：任取 gatherCount 个目标两两互在半径内（简化为存在性判定） ----
    for (int i = 0; i < tracks.count(); i++) {
        if (!alive.contains(tracks[i].fusedId))
            continue;
        QList<int> cluster{tracks[i].fusedId};
        const auto cell = cellFor(tracks[i].lat, tracks[i].lon, width);
        QList<int> candidates;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    candidates.append(grid.value(cellKey({cell.x + dx, cell.y + dy, cell.z + dz})));
        for (int j : candidates) {
            if (i == j) {
                continue;
            }
            const double d = SwarmTypes::haversineM(tracks[i].lat, tracks[i].lon, tracks[j].lat, tracks[j].lon);
            if (d < _cfg.gatherRadiusM) {
                cluster.append(tracks[j].fusedId);
            }
        }
        if (cluster.count() >= _cfg.gatherCount) {
            std::sort(cluster.begin(), cluster.end());
            const QString key =
                QStringLiteral("gather_%1")
                    .arg(QString::number(cluster.first()) + QStringLiteral("_") + QString::number(cluster.count()));
            if (!_cooldownHit(key, QStringLiteral("warning"), monoMs)) {
                QVariantList ids;
                for (int id : cluster) {
                    ids.append(id);
                }
                out.append({QStringLiteral("warning"), QStringLiteral("gathering"),
                            QStringLiteral("检测到 %1 个目标聚集（半径 %2 米内）")
                                .arg(cluster.count())
                                .arg(_cfg.gatherRadiusM),
                            ids, monoMs});
            }
            break;  // 一轮只报一个聚集簇
        }
    }

    for (const SwarmTrackFusion::FusedTrack& t : tracks) {
        if (!alive.contains(t.fusedId))
            continue;
        // ---- 目标越界 ----
        if (!_excludePolys.isEmpty() || !_includePolys.isEmpty()) {
            const bool breach = _fenceBreach(t.lat, t.lon);
            if (breach && !_cooldownHit(QStringLiteral("tb_%1").arg(t.fusedId), QStringLiteral("warning"), monoMs)) {
                out.append({QStringLiteral("warning"),
                            QStringLiteral("boundary"),
                            QStringLiteral("目标 #%1（%2）越出允许区或进入禁飞区")
                                .arg(t.fusedId)
                                .arg(SwarmTrackFusion::categoryName(t.category)),
                            {t.fusedId},
                            monoMs});
            }
        }
        // ---- 速度突变 / 绝对超速 ----
        const double spd = t.speedMs();
        const double lastSpd = _lastSpeed.value(t.fusedId, -1.0);
        if (lastSpd >= 0.0 && qAbs(spd - lastSpd) > _cfg.speedBurstMs &&
            !_cooldownHit(QStringLiteral("sb_%1").arg(t.fusedId), QStringLiteral("warning"), monoMs)) {
            out.append({QStringLiteral("warning"),
                        QStringLiteral("speed_burst"),
                        QStringLiteral("目标 #%1 速度突变：%2 → %3 m/s")
                            .arg(t.fusedId)
                            .arg(lastSpd, 0, 'f', 1)
                            .arg(spd, 0, 'f', 1),
                        {t.fusedId},
                        monoMs});
        } else if (spd > _cfg.overspeedMs &&
                   !_cooldownHit(QStringLiteral("os_%1").arg(t.fusedId), QStringLiteral("warning"), monoMs)) {
            out.append({QStringLiteral("warning"),
                        QStringLiteral("speed_burst"),
                        QStringLiteral("目标 #%1 超速移动（%2 m/s）").arg(t.fusedId).arg(spd, 0, 'f', 1),
                        {t.fusedId},
                        monoMs});
        }
        _lastSpeed[t.fusedId] = spd;
    }
    return out;
}
