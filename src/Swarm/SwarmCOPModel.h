/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmCOPModel（WP3.4 共同态势图）
 *
 * COP（Common Operational Picture）数据模型：
 *   - 统一目标清单：由 SwarmTrackFusion 融合航迹驱动（≥1Hz 刷新）
 *   - 威胁区/禁飞区多边形（与 fences.json 同构，红色叠加）
 *   - 搜索覆盖率热力网格：机体/航迹经过的地理格网累计权重（琥珀色叠加）
 *
 * QML 侧 SwarmSituationLayer.qml 以 MapItemView 绑定本模型渲染。
 * QAbstractListModel 属 QtCore，可脱离 QML 引擎单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QAbstractListModel>
#include <QtCore/QVariantList>
#include <QtPositioning/QGeoCoordinate>
#include <QtQmlIntegration/QtQmlIntegration>

#include "SwarmTrackFusion.h"

class SwarmCOPModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("")

    Q_PROPERTY(int trackCount READ trackCount NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList threatZones READ threatZones WRITE setThreatZones NOTIFY threatZonesChanged)
    Q_PROPERTY(QVariantList heatmap READ heatmap NOTIFY heatmapChanged)
    Q_PROPERTY(int heatmapCellCount READ heatmapCellCount NOTIFY heatmapChanged)
    Q_PROPERTY(quint64 rejectedCoverageCount READ rejectedCoverageCount NOTIFY coverageRejected)

public:
    enum Role
    {
        FusedIdRole = Qt::UserRole + 1,
        CategoryRole,
        CategoryNameRole,
        CoordinateRole,
        LatRole,
        LonRole,
        SpeedRole,
        ConfidenceRole,
        StateRole,
        SensorsRole,
        AgeSecRole,
    };

    explicit SwarmCOPModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int trackCount() const { return _tracks.count(); }

    /// 由融合引擎刷新统一目标清单（整体重置；目标数百级 @1Hz 成本可忽略）
    void updateFromFusion(const QList<SwarmTrackFusion::FusedTrack>& tracks, qint64 monoMs);

    QVariantList threatZones() const { return _threatZones; }

    void setThreatZones(const QVariantList& zones);  // [[ [lat,lon], ... ], ...]

    /// 覆盖热力：机体每经过一个格网（~55m）权重+1（上限 cap）
    void ingestCoverage(double lat, double lon, int maxCells = 500);
    QVariantList heatmap() const;

    int heatmapCellCount() const { return _heat.count(); }

    quint64 rejectedCoverageCount() const { return _rejectedCoverageCount; }

    void clearHeatmap();

    /// 单个融合航迹的 QVariantMap（供 QML 详情弹层/AI 上下文）
    Q_INVOKABLE QVariantMap trackAt(int row) const;

signals:
    void tracksChanged();
    void threatZonesChanged();
    void heatmapChanged();
    void coverageRejected();

private:
    QList<SwarmTrackFusion::FusedTrack> _tracks;
    qint64 _monoMs = 0;
    QVariantList _threatZones;
    QHash<qint64, int> _heat;  // 格网键（lat/lon 0.0005° 量化）→ 权重
    quint64 _rejectedCoverageCount = 0;
};
