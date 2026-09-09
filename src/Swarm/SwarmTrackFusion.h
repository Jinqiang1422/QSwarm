/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmTrackFusion（WP3.3 多源航迹融合）
 *
 * 把多传感器（多路机载视频、后续可扩展 ADS-B/光电球等）上报的目标航迹
 * 融合为统一目标清单：
 *   - 最近邻关联（ENU 平面距离门限，含速度外推补偿上报时延）
 *   - 置信度加权位置/速度/类别融合（同目标多源上报去重）
 *   - 生命周期：tentative(命中<minHits) → confirmed → coasting(超时未更新) → 删除
 *
 * 仅依赖 QtCore（坐标经 SwarmTypes 局部平面化），可脱离 QGC 独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QSet>
#include <QtCore/QString>

class SwarmTrackFusion
{
public:
    static constexpr int MaxTracks = 1024;
    static constexpr int MaxSources = 8192;
    static constexpr int MaxContributors = 64;
    static constexpr qint64 ContributorLifetimeMs = 10000;

    /// 上游航迹上报（与 swarm_msgs.make_track / SWARM_TARGET_TRACK 字段一一对应）
    struct TrackReport
    {
        QString streamId;
        int trackId = 0;   ///< 服务侧航迹 ID
        int sensorId = 0;  ///< 来源传感器
        int category = 0;  ///< SWARM_TARGET_CATEGORY
        double lat = 0.0;
        double lon = 0.0;
        double ve = 0.0;      ///< 东向速度 m/s
        double vn = 0.0;      ///< 北向速度 m/s
        double confidence = 0.5;
        int source = 0;       ///< SWARM_TRACK_SOURCE
        qint64 timeUsec = 0;  ///< 量测时间戳
    };

    /// 融合后的统一航迹（COP/事件检测/态势摘要共用）
    struct FusedTrack
    {
        int fusedId = 0;
        int category = 0;
        double lat = 0.0;
        double lon = 0.0;
        double ve = 0.0;
        double vn = 0.0;
        double confidence = 0.0;
        QSet<int> sensors;          ///< 贡献传感器集合
        QList<int> sourceTrackIds;  ///< 贡献上游航迹
        int hits = 0;
        qint64 firstSeenMs = 0;
        qint64 lastSeenMs = 0;
        QString state;           ///< tentative / confirmed / coasting
        double speedMs() const;  ///< 地速
    };

    struct Config
    {
        double gateDistM = 25.0;     ///< 关联距离门限（米）
        double coastMs = 3000;       ///< 超过则进入 coasting
        double dropMs = 10000;       ///< 超过则删除
        int minHits = 2;             ///< tentative→confirmed 所需命中
        double speedDamp = 0.4;      ///< 速度融合阻尼（防瞬时跳变）
        double minConfidence = 0.2;  ///< 感知输入置信度门槛（低于则丢弃）
    };

    SwarmTrackFusion();
    explicit SwarmTrackFusion(const Config& cfg);

    void setConfig(const Config& cfg) { _cfg = cfg; }

    void setReference(double lat, double lon);  ///< ENU 参考点（默认首报自动设定）

    /// 摄入一条上游航迹，返回命中的融合航迹 ID（新建则为新 ID）
    int ingest(const TrackReport& r, qint64 monoMs);

    /// 周期维护：状态迁移与过期删除（建议 ≥2Hz 调用）
    void prune(qint64 monoMs);

    /// 清空当前融合会话；保留配置，但重置参考点、ID 和诊断计数。
    void clear();

    /// 统一目标清单（confirmed + coasting，供 COP 显示与事件检测）
    QList<FusedTrack> tracks() const;

    /// 含 tentative 的全量（诊断/测试用）
    QList<FusedTrack> allTracks() const { return _tracks; }

    /// 因坐标非法/置信度不足被感知校验闸门丢弃的航迹累计数（供审计/诊断）
    int rejectedCount() const { return _rejectedCount; }

    quint64 capacityRejectedCount() const { return _capacityRejectedCount; }

    int sourceCount() const { return _sourceReceived.size(); }

    /// 类别名（与 mavlink/swarm_perception.xml 对齐）
    static QString categoryName(int category);

private:
    int _associate(double eastM, double northM, qint64 monoMs) const;  ///< 最近邻（门限内）
    static QString _sourceKey(const TrackReport& report);
    void _rebuildTrackIndexes();
    void _removeSource(const QString& key);
    void _refreshContributors(FusedTrack& track, qint64 monoMs);
    void _localToGeo(double eastM, double northM, double& lat, double& lon) const;

    Config _cfg;
    QList<FusedTrack> _tracks;
    QHash<QString, int> _sourceAssociations;
    QHash<QString, qint64> _sourceTimestamps;
    QHash<QString, qint64> _sourceReceived;           ///< (sensorId, trackId) → fusedId
    QHash<QString, QPair<int, int>> _sourceMetadata;  // sensorId, trackId
    QHash<int, QSet<QString>> _contributors;
    QHash<int, int> _trackIndexes;                    ///< fusedId → _tracks index
    double _refLat = 0.0, _refLon = 0.0;
    bool _refSet = false;
    int _nextFusedId = 1;
    int _rejectedCount = 0;
    quint64 _capacityRejectedCount = 0;
};
