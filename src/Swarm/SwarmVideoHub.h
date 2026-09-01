/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmVideoHub（WP3.1 多路视频汇聚）
 *
 * 多路机载视频的汇聚管理：
 *   - 通道注册：sensorId ↔ 流地址 ↔ 机体映射（GStreamer 解码复用 QGC
 *     VideoReceiver，本模块负责注册表、状态与元数据对齐，不重造解码）
 *   - 统一时间戳：各路视频帧单调钟 → 统一时基（首帧标定偏移）
 *   - 坐标解算：像素→WGS84（针孔模型+平地假设，与感知服务 detector.py
 *     pixel_to_geo 同一公式，两端独立实现、测试交叉验证）
 *   - 视场脚印：相机四视锥地面交点（COP 图层显示当前可视区域）
 *
 * 仅依赖 QtCore，可独立单元测试。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QPair>
#include <QtCore/QString>

class SwarmVideoHub
{
public:
    struct Channel
    {
        int sensorId = 0;
        int vehicleId = 0;
        QString streamUrl;
        bool active = false;
        qint64 firstFrameMonoMs = -1;  ///< 首帧到达（本地单调钟）
        qint64 firstFrameUsec = -1;    ///< 首帧时间戳（流内时基）
        qint64 frames = 0;
    };

    /// 通道注册/注销/查询
    void registerChannel(int sensorId, int vehicleId, const QString& streamUrl);
    void removeChannel(int sensorId);
    QList<Channel> channels() const;

    int channelCount() const { return _channels.count(); }

    /// 帧到达登记：返回统一时基时间戳（微秒）。
    /// 统一时基 = 本地单调钟首帧时刻 + (流内时戳 - 首帧流内时戳)。
    qint64 noteFrame(int sensorId, qint64 streamUsec, qint64 localMonoMs);

    /// 像素→WGS84（与 services/perception/detector.py::pixel_to_geo 同公式）。
    /// 射线不落地（朝天/超 3000m）返回 false。
    static bool pixelToGeo(double px, double py, double camLat, double camLon, double altM, double headingDeg,
                           double gimbalPitchDeg, double hfovDeg, int widthPx, int heightPx, double& outLat,
                           double& outLon);

    /// 视场脚印（四角地面交点，用于 COP 显示当前可视区；任角不落地则该角跳过）
    static QList<QPair<double, double>> footprint(double camLat, double camLon, double altM, double headingDeg,
                                                  double gimbalPitchDeg, double hfovDeg, int widthPx, int heightPx);

    /// 垂直视场角（度），与 Python CameraPose.vfov_deg 同公式
    static double vfovDeg(double hfovDeg, int widthPx, int heightPx);

private:
    QHash<int, Channel> _channels;  ///< sensorId → Channel
};
