/****************************************************************************
 *
 * QGroundControl 集群指挥控制扩展 — SwarmTunnelProtocol（WP3.7 协议核心）
 *
 * TUNNEL JSON 协议的 QtCore-only 头文件实现，与 Python 端
 * services/perception/swarm_msgs.py 严格同构（单源契约、两端镜像）：
 *   帧格式  [payload_type:uint16be][seq:uint8][JSON 分片]
 *   类型    42000 单帧 / 42001 中间帧 / 42002 尾帧
 *   线格式  track = 位置式数组 [id,cat,lat1e7,lon1e7,altdm,ve_cm,vn_cm,
 *                              conf0-1000,sensor,source,time_usec]
 *
 * SwarmPerceptionBridge（QObject/QUdpSocket）与单元测试共用本文件。
 *
 ****************************************************************************/

#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

namespace SwarmTunnel {

constexpr quint16 PT_SINGLE = 42000;
constexpr quint16 PT_MIDDLE = 42001;
constexpr quint16 PT_LAST   = 42002;

/// 分片重组器状态（镜像 swarm_msgs.Reassembler）
struct Reassembly {
    QByteArray buf;
    bool       open = false;
};

/// 位置式航迹数组 → 字段字典（镜像 swarm_msgs.track_from_wire）
inline QJsonObject trackFromWire(const QJsonArray &w)
{
    QJsonObject t;
    if (w.count() < 11) {
        return t;
    }
    t[QStringLiteral("track_id")]   = w.at(0).toInt();
    t[QStringLiteral("category")]   = w.at(1).toInt();
    t[QStringLiteral("lat")]        = w.at(2).toDouble() / 1e7;
    t[QStringLiteral("lon")]        = w.at(3).toDouble() / 1e7;
    const int altDm = w.at(4).toInt();
    t[QStringLiteral("alt_msl")]    = altDm < 0 ? QJsonValue() : QJsonValue(altDm / 10.0);
    t[QStringLiteral("ve")]         = w.at(5).toDouble() / 100.0;
    t[QStringLiteral("vn")]         = w.at(6).toDouble() / 100.0;
    t[QStringLiteral("confidence")] = w.at(7).toDouble() / 1000.0;
    t[QStringLiteral("sensor_id")]  = w.at(8).toInt();
    t[QStringLiteral("source")]     = w.at(9).toInt();
    t[QStringLiteral("time_usec")]  = w.at(10).toDouble();
    return t;
}

/// JSON 体 → {kind, data}（track/track_batch 自动还原位置式线格式）
inline QJsonObject decodePayload(const QByteArray &body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject msg = doc.object();
    const QString kind = msg.value(QStringLiteral("kind")).toString();
    QJsonObject out;
    out[QStringLiteral("kind")] = kind;
    if (kind == QStringLiteral("track")) {
        out[QStringLiteral("data")] = trackFromWire(msg.value(QStringLiteral("data")).toArray());
    } else if (kind == QStringLiteral("track_batch")) {
        const QJsonObject data = msg.value(QStringLiteral("data")).toObject();
        QJsonArray tracks;
        for (const QJsonValue &v : data.value(QStringLiteral("tracks")).toArray()) {
            tracks.append(trackFromWire(v.toArray()));
        }
        QJsonObject d;
        d[QStringLiteral("time_usec")] = data.value(QStringLiteral("t")).toDouble();
        d[QStringLiteral("tracks")] = tracks;
        out[QStringLiteral("data")] = d;
    } else {
        out[QStringLiteral("data")] = msg.value(QStringLiteral("data"));
    }
    return out;
}

/// 解析一个 UDP/TUNNEL 数据报 → 完成的消息 {kind, data}；分片未完返回空
inline QJsonObject parseDatagram(const QByteArray &datagram, Reassembly &rx)
{
    if (datagram.size() < 3) {
        return {};
    }
    const quint16 pt = (quint8(datagram.at(0)) << 8) | quint8(datagram.at(1));
    const QByteArray chunk = datagram.mid(3);           // 跳过 payload_type + seq

    QByteArray body;
    if (pt == PT_SINGLE) {
        rx.buf.clear();
        rx.open = false;
        body = chunk;
    } else if (pt == PT_MIDDLE) {
        rx.open = true;
        rx.buf += chunk;
        return {};
    } else if (pt == PT_LAST) {
        if (rx.open) {
            rx.buf += chunk;
            body = rx.buf;
        } else {
            body = chunk;                               // 丢首帧容错
        }
        rx.buf.clear();
        rx.open = false;
    } else {
        return {};
    }
    return decodePayload(body);
}

} // namespace SwarmTunnel
