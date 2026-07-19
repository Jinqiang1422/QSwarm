#include "SwarmVideoHub.h"

#include "SwarmTypes.h"

#include <QtCore/QtMath>

void SwarmVideoHub::registerChannel(int sensorId, int vehicleId, const QString &streamUrl)
{
    Channel ch;
    ch.sensorId  = sensorId;
    ch.vehicleId = vehicleId;
    ch.streamUrl = streamUrl;
    ch.active    = true;
    _channels.insert(sensorId, ch);
}

void SwarmVideoHub::removeChannel(int sensorId)
{
    _channels.remove(sensorId);
}

QList<SwarmVideoHub::Channel> SwarmVideoHub::channels() const
{
    return _channels.values();
}

qint64 SwarmVideoHub::noteFrame(int sensorId, qint64 streamUsec, qint64 localMonoMs)
{
    Channel &ch = _channels[sensorId];                  // 未注册则隐式建通道
    if (ch.firstFrameMonoMs < 0) {
        ch.firstFrameMonoMs = localMonoMs;
        ch.firstFrameUsec   = streamUsec;
    }
    ch.frames++;
    // 统一时基：首帧本地时刻 + 流内相对偏移
    return ch.firstFrameMonoMs * 1000 + (streamUsec - ch.firstFrameUsec);
}

double SwarmVideoHub::vfovDeg(double hfovDeg, int widthPx, int heightPx)
{
    return qRadiansToDegrees(2.0 * qAtan(qTan(SwarmTypes::degToRad(hfovDeg) / 2.0)
                                         * double(heightPx) / double(widthPx)));
}

bool SwarmVideoHub::pixelToGeo(double px, double py,
                               double camLat, double camLon, double altM,
                               double headingDeg, double gimbalPitchDeg, double hfovDeg,
                               int widthPx, int heightPx,
                               double &outLat, double &outLon)
{
    const double vfov  = vfovDeg(hfovDeg, widthPx, heightPx);
    const double pitch = gimbalPitchDeg + (py / double(heightPx) - 0.5) * vfov;
    if (pitch <= 1.0) {
        return false;                                   // 朝天/近水平
    }
    const double groundRange = altM / qTan(SwarmTypes::degToRad(pitch));
    if (groundRange > 3000.0) {
        return false;                                   // 超可靠解算范围
    }
    const double yaw = headingDeg + (px / double(widthPx) - 0.5) * hfovDeg;
    SwarmTypes::destinationPoint(camLat, camLon, yaw, groundRange, outLat, outLon);
    return true;
}

QList<QPair<double, double>> SwarmVideoHub::footprint(
        double camLat, double camLon, double altM,
        double headingDeg, double gimbalPitchDeg, double hfovDeg,
        int widthPx, int heightPx)
{
    QList<QPair<double, double>> out;
    const double corners[4][2] = {{0, 0},
                                  {double(widthPx), 0},
                                  {double(widthPx), double(heightPx)},
                                  {0, double(heightPx)}};
    for (const auto &c : corners) {
        double lat, lon;
        if (pixelToGeo(c[0], c[1], camLat, camLon, altM,
                       headingDeg, gimbalPitchDeg, hfovDeg,
                       widthPx, heightPx, lat, lon)) {
            out.append({lat, lon});
        }
    }
    return out;
}
