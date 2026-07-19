#include "SwarmCOPModel.h"

#include <QtCore/QVariantMap>

SwarmCOPModel::SwarmCOPModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int SwarmCOPModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : _tracks.count();
}

QHash<int, QByteArray> SwarmCOPModel::roleNames() const
{
    return {
        {FusedIdRole,      "fusedId"},
        {CategoryRole,     "category"},
        {CategoryNameRole, "categoryName"},
        {CoordinateRole,   "coordinate"},
        {LatRole,          "lat"},
        {LonRole,          "lon"},
        {SpeedRole,        "speed"},
        {ConfidenceRole,   "confidence"},
        {StateRole,        "trackState"},
        {SensorsRole,      "sensors"},
        {AgeSecRole,       "ageSec"},
    };
}

QVariant SwarmCOPModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= _tracks.count()) {
        return {};
    }
    const SwarmTrackFusion::FusedTrack &t = _tracks.at(index.row());
    switch (role) {
    case FusedIdRole:      return t.fusedId;
    case CategoryRole:     return t.category;
    case CategoryNameRole: return SwarmTrackFusion::categoryName(t.category);
    case CoordinateRole:   return QVariant::fromValue(QGeoCoordinate(t.lat, t.lon));
    case LatRole:          return t.lat;
    case LonRole:          return t.lon;
    case SpeedRole:        return t.speedMs();
    case ConfidenceRole:   return t.confidence;
    case StateRole:        return t.state;
    case SensorsRole:      return t.sensors.count();
    case AgeSecRole:       return qMax(0.0, (_monoMs - t.lastSeenMs) / 1000.0);
    default:               return {};
    }
}

void SwarmCOPModel::updateFromFusion(const QList<SwarmTrackFusion::FusedTrack> &tracks, qint64 monoMs)
{
    beginResetModel();
    _tracks = tracks;
    _monoMs = monoMs;
    endResetModel();
    emit tracksChanged();
}

void SwarmCOPModel::setThreatZones(const QVariantList &zones)
{
    if (_threatZones == zones) {
        return;
    }
    _threatZones = zones;
    emit threatZonesChanged();
}

void SwarmCOPModel::ingestCoverage(double lat, double lon, int maxCells)
{
    // 0.0005° ≈ 55m 格网量化，键 = lat*1e4 << 32 | lon*1e4（非负偏移防负数）
    const qint64 la = qint64(lat * 20000.0) + (1LL << 30);
    const qint64 lo = qint64(lon * 20000.0) + (1LL << 30);
    const qint64 key = (la << 32) | (lo & 0xFFFFFFFFLL);
    if (!_heat.contains(key) && _heat.count() >= maxCells) {
        return;                                         // 防内存膨胀
    }
    _heat[key] = qMin(10, _heat.value(key, 0) + 1);
    if (_heat.value(key) == 1 || _heat.value(key) % 3 == 0) {
        emit heatmapChanged();                          // 限频通知（1Hz 级）
    }
}

QVariantList SwarmCOPModel::heatmap() const
{
    QVariantList out;
    out.reserve(_heat.count());
    for (auto it = _heat.constBegin(); it != _heat.constEnd(); ++it) {
        const qint64 la = (it.key() >> 32) - (1LL << 30);
        const qint64 lo = (it.key() & 0xFFFFFFFFLL) - (1LL << 30);
        QVariantMap cell;
        cell[QStringLiteral("lat")]    = double(la) / 20000.0;
        cell[QStringLiteral("lon")]    = double(lo) / 20000.0;
        cell[QStringLiteral("weight")] = it.value();
        out.append(cell);
    }
    return out;
}

void SwarmCOPModel::clearHeatmap()
{
    _heat.clear();
    emit heatmapChanged();
}

QVariantMap SwarmCOPModel::trackAt(int row) const
{
    QVariantMap m;
    if (row < 0 || row >= _tracks.count()) {
        return m;
    }
    const SwarmTrackFusion::FusedTrack &t = _tracks.at(row);
    m[QStringLiteral("fusedId")]      = t.fusedId;
    m[QStringLiteral("category")]     = SwarmTrackFusion::categoryName(t.category);
    m[QStringLiteral("lat")]          = t.lat;
    m[QStringLiteral("lon")]          = t.lon;
    m[QStringLiteral("speed")]        = t.speedMs();
    m[QStringLiteral("confidence")]   = t.confidence;
    m[QStringLiteral("state")]        = t.state;
    m[QStringLiteral("sensors")]      = t.sensors.count();
    return m;
}
