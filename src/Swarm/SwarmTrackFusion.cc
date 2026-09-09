#include "SwarmTrackFusion.h"

#include <QtCore/QtMath>
#include <limits>

#include "SwarmTypes.h"

SwarmTrackFusion::SwarmTrackFusion() : SwarmTrackFusion(Config()) {}

SwarmTrackFusion::SwarmTrackFusion(const Config& cfg) : _cfg(cfg) {}

void SwarmTrackFusion::setReference(double lat, double lon)
{
    _refLat = lat;
    _refLon = lon;
    _refSet = true;
}

double SwarmTrackFusion::FusedTrack::speedMs() const
{
    return qSqrt(ve * ve + vn * vn);
}

QString SwarmTrackFusion::categoryName(int category)
{
    switch (category) {
        case 1:
            return QStringLiteral("人员");
        case 2:
            return QStringLiteral("车辆");
        case 3:
            return QStringLiteral("船只");
        case 4:
            return QStringLiteral("动物");
        case 5:
            return QStringLiteral("火点");
        case 6:
            return QStringLiteral("烟雾");
        default:
            return QStringLiteral("未知");
    }
}

int SwarmTrackFusion::_associate(double eastM, double northM, qint64 monoMs) const
{
    int bestIdx = -1;
    double bestDist = _cfg.gateDistM;
    for (int i = 0; i < _tracks.count(); i++) {
        const FusedTrack& t = _tracks.at(i);
        if (t.state == QStringLiteral("coasting") && monoMs - t.lastSeenMs > _cfg.dropMs) {
            continue;  // 已失效的不参与关联
        }
        double te, tn;
        SwarmTypes::latLonToLocalM(_refLat, _refLon, t.lat, t.lon, te, tn);
        // 速度外推补偿上报间隔（上限 2s，防 coasting 期外推发散）
        const double dt = qBound(0.0, (monoMs - t.lastSeenMs) / 1000.0, 2.0);
        te += t.ve * dt;
        tn += t.vn * dt;
        const double d = qSqrt((te - eastM) * (te - eastM) + (tn - northM) * (tn - northM));
        if (d < bestDist) {
            bestDist = d;
            bestIdx = i;
        }
    }
    return bestIdx;
}

QString SwarmTrackFusion::_sourceKey(const TrackReport& r)
{
    return r.streamId + QLatin1Char('|') + QString::number(r.source) + QLatin1Char('|') + QString::number(r.sensorId) +
           QLatin1Char('|') + QString::number(r.trackId);
}

void SwarmTrackFusion::_rebuildTrackIndexes()
{
    _trackIndexes.clear();
    _trackIndexes.reserve(_tracks.size());
    for (int index = 0; index < _tracks.size(); ++index) {
        _trackIndexes.insert(_tracks.at(index).fusedId, index);
    }

    const auto keys = _sourceAssociations.keys();
    for (const auto& key : keys) {
        if (!_trackIndexes.contains(_sourceAssociations.value(key)))
            _removeSource(key);
    }
}

void SwarmTrackFusion::_removeSource(const QString& key)
{
    const int id = _sourceAssociations.take(key);
    auto it = _contributors.find(id);
    if (it != _contributors.end()) {
        it->remove(key);
        if (it->isEmpty())
            _contributors.erase(it);
    }
    _sourceReceived.remove(key);
    _sourceTimestamps.remove(key);
    _sourceMetadata.remove(key);
}

void SwarmTrackFusion::_refreshContributors(FusedTrack& track, qint64 monoMs)
{
    const auto keys = _contributors.value(track.fusedId);
    track.sensors.clear();
    track.sourceTrackIds.clear();
    for (const auto& key : keys) {
        if (monoMs - _sourceReceived.value(key) > ContributorLifetimeMs) {
            _removeSource(key);
            continue;
        }
        const auto metadata = _sourceMetadata.value(key);
        track.sensors.insert(metadata.first);
        if (!track.sourceTrackIds.contains(metadata.second))
            track.sourceTrackIds.append(metadata.second);
    }
}

void SwarmTrackFusion::_localToGeo(double eastM, double northM, double& lat, double& lon) const
{
    lat = _refLat + northM / 110540.0;
    const double latAvg = SwarmTypes::degToRad((_refLat + lat) / 2.0);
    lon = _refLon + eastM / (qCos(latAvg) * 111320.0);
}

int SwarmTrackFusion::ingest(const TrackReport& r, qint64 monoMs)
{
    // 感知输入校验闸门(纵深防御):丢弃非法坐标/低置信航迹,防伪造或污染的外部
    // 航迹进入融合 → COP → LLM 态势解读(与指挥侧输入闸门对称);须在设参考点前拦截。
    if (!SwarmTypes::isFiniteLatLon(r.lat, r.lon) || !std::isfinite(r.confidence) || !std::isfinite(r.ve) ||
        !std::isfinite(r.vn) || r.confidence > 1.0 || r.confidence < _cfg.minConfidence || r.streamId.size() > 256) {
        _rejectedCount++;
        return -1;
    }
    if (!_refSet) {
        setReference(r.lat, r.lon);
    }
    double eastM, northM;
    SwarmTypes::latLonToLocalM(_refLat, _refLon, r.lat, r.lon, eastM, northM);

    const QString sourceKey = _sourceKey(r);
    if (r.timeUsec > 0 && _sourceTimestamps.contains(sourceKey) && r.timeUsec <= _sourceTimestamps.value(sourceKey)) {
        ++_rejectedCount;
        return -1;
    }
    if (!_sourceReceived.contains(sourceKey) && _sourceReceived.size() >= MaxSources) {
        prune(monoMs);
        if (_sourceReceived.size() >= MaxSources) {
            ++_capacityRejectedCount;
            return -1;
        }
    }
    int idx = -1;
    const int knownFusedId = _sourceAssociations.value(sourceKey, -1);
    const int knownIndex = _trackIndexes.value(knownFusedId, -1);
    if (knownIndex >= 0 && knownIndex < _tracks.size()) {
        const FusedTrack& knownTrack = _tracks.at(knownIndex);
        if (knownTrack.fusedId == knownFusedId && monoMs - knownTrack.lastSeenMs <= _cfg.dropMs) {
            idx = knownIndex;
        }
    }
    if (idx < 0) {
        _removeSource(sourceKey);
        idx = _associate(eastM, northM, monoMs);
    }
    if (idx >= 0) {
        _refreshContributors(_tracks[idx], monoMs);
        const auto& contributors = _contributors.value(_tracks[idx].fusedId);
        if (!contributors.contains(sourceKey) && contributors.size() >= MaxContributors) {
            ++_capacityRejectedCount;
            return -1;
        }
    } else if (_tracks.size() >= MaxTracks || _nextFusedId == std::numeric_limits<int>::max()) {
        ++_capacityRejectedCount;
        return -1;
    }
    _sourceTimestamps[sourceKey] = r.timeUsec;
    _sourceReceived[sourceKey] = monoMs;
    _sourceMetadata[sourceKey] = {r.sensorId, r.trackId};
    if (idx < 0) {  // 新建融合航迹
        FusedTrack t;
        t.fusedId = _nextFusedId++;
        t.category = r.category;
        t.lat = r.lat;
        t.lon = r.lon;
        t.ve = r.ve;
        t.vn = r.vn;
        t.confidence = r.confidence;
        t.sensors.insert(r.sensorId);
        t.sourceTrackIds.append(r.trackId);
        t.hits = 1;
        t.firstSeenMs = monoMs;
        t.lastSeenMs = monoMs;
        t.state = QStringLiteral("tentative");
        _tracks.append(t);
        _trackIndexes.insert(t.fusedId, _tracks.size() - 1);
        _sourceAssociations.insert(sourceKey, t.fusedId);
        _contributors[t.fusedId].insert(sourceKey);
        return t.fusedId;
    }

    FusedTrack& t = _tracks[idx];
    _sourceAssociations.insert(sourceKey, t.fusedId);
    _contributors[t.fusedId].insert(sourceKey);
    // 置信度加权融合（权重 = 上报置信度，融合置信度递推）
    const double w = qBound(0.05, r.confidence, 1.0);
    const double k = w / (t.confidence + w);  // 归一化新息权重
    double te, tn;
    SwarmTypes::latLonToLocalM(_refLat, _refLon, t.lat, t.lon, te, tn);
    te += (eastM - te) * k * 2.0;  // k*2 ≈ 位置新息增益（经验）
    tn += (northM - tn) * k * 2.0;
    _localToGeo(te, tn, t.lat, t.lon);
    t.ve += (r.ve - t.ve) * _cfg.speedDamp;
    t.vn += (r.vn - t.vn) * _cfg.speedDamp;
    t.confidence = qMin(0.99, t.confidence + (1.0 - t.confidence) * w * 0.5);
    if (r.category != 0) {  // 非 unknown 类别覆盖
        t.category = r.category;
    }
    _refreshContributors(t, monoMs);
    if (t.hits < std::numeric_limits<int>::max())
        ++t.hits;
    t.lastSeenMs = monoMs;
    if (t.state == QStringLiteral("tentative") && t.hits >= _cfg.minHits) {
        t.state = QStringLiteral("confirmed");
    } else if (t.state == QStringLiteral("coasting")) {
        t.state = QStringLiteral("confirmed");  // 重获量测
    }
    return t.fusedId;
}

void SwarmTrackFusion::prune(qint64 monoMs)
{
    const auto keys = _sourceReceived.keys();
    for (const auto& key : keys) {
        if (monoMs - _sourceReceived.value(key) > ContributorLifetimeMs)
            _removeSource(key);
    }
    for (FusedTrack& t : _tracks) {
        _refreshContributors(t, monoMs);
        const qint64 age = monoMs - t.lastSeenMs;
        if (t.state != QStringLiteral("tentative") && age > _cfg.coastMs) {
            t.state = QStringLiteral("coasting");
        }
    }
    for (int i = _tracks.count() - 1; i >= 0; i--) {
        const FusedTrack& t = _tracks.at(i);
        const qint64 age = monoMs - t.lastSeenMs;
        if (age > _cfg.dropMs || (t.state == QStringLiteral("tentative") && age > _cfg.coastMs)) {
            _tracks.removeAt(i);
        }
    }
    _rebuildTrackIndexes();
}

void SwarmTrackFusion::clear()
{
    _tracks.clear();
    _sourceAssociations.clear();
    _sourceTimestamps.clear();
    _sourceReceived.clear();
    _sourceMetadata.clear();
    _contributors.clear();
    _trackIndexes.clear();
    _refLat = 0.0;
    _refLon = 0.0;
    _refSet = false;
    _nextFusedId = 1;
    _rejectedCount = 0;
    _capacityRejectedCount = 0;
}

QList<SwarmTrackFusion::FusedTrack> SwarmTrackFusion::tracks() const
{
    QList<FusedTrack> out;
    for (const FusedTrack& t : _tracks) {
        if (t.state != QStringLiteral("tentative")) {
            out.append(t);
        }
    }
    return out;
}
