#pragma once

#include <QtCore/QBitArray>
#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QtEndian>
#include <algorithm>
#include <cmath>

#include "SwarmClock.h"

namespace SwarmTunnel {
constexpr quint16 PT_V2 = 42003;
constexpr int HEADER_SIZE = 25;
constexpr int CHUNK_SIZE = 128 - HEADER_SIZE;
constexpr int MAX_MESSAGE = 65536;
constexpr int MAX_CONTEXTS = 64;
constexpr int MAX_PER_SOURCE = 8;
constexpr qint64 TTL_MS = 2000;

inline quint32 crc32(const QByteArray& bytes)
{
    quint32 crc = 0xffffffffU;
    for (unsigned char byte : bytes) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

struct Reassembly
{
    struct Message
    {
        QString source;
        QByteArray body;
        QBitArray received;
        quint32 checksum = 0;
        qint64 started = 0;
        int count = 0;
    };

    QHash<QString, Message> messages;
    QHash<QString, qint64> completed;
    quint64 rejected = 0;
    quint64 expired = 0;
    quint64 duplicates = 0;

    qsizetype bufferedBytes() const
    {
        qsizetype bytes = 0;
        for (const auto& message : messages)
            bytes += message.body.size();
        return bytes;
    }

    void expire(qint64 now)
    {
        for (auto it = messages.begin(); it != messages.end();) {
            if (now - it->started >= TTL_MS) {
                it = messages.erase(it);
                ++expired;
            } else
                ++it;
        }
        for (auto it = completed.begin(); it != completed.end();) {
            if (now - it.value() >= TTL_MS)
                it = completed.erase(it);
            else
                ++it;
        }
    }
};

/// 位置式航迹数组 → 字段字典（镜像 swarm_msgs.track_from_wire）
inline QJsonObject trackFromWire(const QJsonArray& w)
{
    QJsonObject t;
    if (w.count() != 11) {
        return t;
    }
    for (int i = 0; i < w.size(); ++i) {
        if (!w[i].isDouble() || !std::isfinite(w[i].toDouble()) || std::trunc(w[i].toDouble()) != w[i].toDouble() ||
            (i < 10 && (w[i].toDouble() < -2147483648.0 || w[i].toDouble() > 2147483647.0)))
            return {};
    }
    if (w[0].toDouble() < 0 || w[0].toDouble() > 2147483647 || w[1].toDouble() < 0 ||
        std::abs(w[2].toDouble()) > 900000000 || std::abs(w[3].toDouble()) > 1800000000 || w[7].toDouble() < 0 ||
        w[7].toDouble() > 1000 || w[8].toDouble() < 0 || w[8].toDouble() > 2147483647 || w[10].toDouble() < 0 ||
        w[10].toDouble() > 9007199254740991.0)
        return {};
    t[QStringLiteral("track_id")] = w.at(0).toInt();
    t[QStringLiteral("category")] = w.at(1).toInt();
    t[QStringLiteral("lat")] = w.at(2).toDouble() / 1e7;
    t[QStringLiteral("lon")] = w.at(3).toDouble() / 1e7;
    const int altDm = w.at(4).toInt();
    t[QStringLiteral("alt_msl")] = altDm == -1 ? QJsonValue() : QJsonValue(altDm / 10.0);
    t[QStringLiteral("ve")] = w.at(5).toDouble() / 100.0;
    t[QStringLiteral("vn")] = w.at(6).toDouble() / 100.0;
    t[QStringLiteral("confidence")] = w.at(7).toDouble() / 1000.0;
    t[QStringLiteral("sensor_id")] = w.at(8).toInt();
    t[QStringLiteral("source")] = w.at(9).toInt();
    t[QStringLiteral("time_usec")] = w.at(10).toDouble();
    return t;
}

/// JSON 体 → {kind, data}（track/track_batch 自动还原位置式线格式）
inline QJsonObject decodePayload(const QByteArray& body)
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
        const auto track = trackFromWire(msg.value(QStringLiteral("data")).toArray());
        if (track.isEmpty())
            return {};
        out[QStringLiteral("data")] = track;
    } else if (kind == QStringLiteral("track_batch")) {
        const QJsonObject data = msg.value(QStringLiteral("data")).toObject();
        const auto timestamp = data.value(QStringLiteral("t"));
        if (!data.value(QStringLiteral("tracks")).isArray() || !timestamp.isDouble() || timestamp.toDouble() < 0 ||
            timestamp.toDouble() > 9007199254740991.0 || std::trunc(timestamp.toDouble()) != timestamp.toDouble())
            return {};
        QJsonArray tracks;
        for (const QJsonValue& v : data.value(QStringLiteral("tracks")).toArray()) {
            const auto track = trackFromWire(v.toArray());
            if (track.isEmpty())
                return {};
            tracks.append(track);
        }
        QJsonObject d;
        d[QStringLiteral("time_usec")] = data.value(QStringLiteral("t")).toDouble();
        d[QStringLiteral("tracks")] = tracks;
        out[QStringLiteral("data")] = d;
    } else if ((kind == QStringLiteral("heartbeat") || kind == QStringLiteral("plan_meta")) &&
               msg.value(QStringLiteral("data")).isObject()) {
        out[QStringLiteral("data")] = msg.value(QStringLiteral("data"));
    } else {
        return {};
    }
    return out;
}

/// UDP carries the two-byte payload type before the TUNNEL payload.
inline QList<QByteArray> encodeDatagrams(const QByteArray& body, quint64 session, quint32 messageId)
{
    if (body.isEmpty() || body.size() > MAX_MESSAGE)
        return {};
    const int count = (body.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
    const quint32 checksum = crc32(body);
    QList<QByteArray> frames;
    for (int index = 0; index < count; ++index) {
        QByteArray frame(2 + HEADER_SIZE, char(0));
        qToBigEndian(PT_V2, frame.data());
        frame[2] = 2;
        qToBigEndian(session, frame.data() + 3);
        qToBigEndian(messageId, frame.data() + 11);
        qToBigEndian(quint16(index), frame.data() + 15);
        qToBigEndian(quint16(count), frame.data() + 17);
        qToBigEndian(quint32(body.size()), frame.data() + 19);
        qToBigEndian(checksum, frame.data() + 23);
        frames.append(frame + body.mid(index * CHUNK_SIZE, CHUNK_SIZE));
    }
    return frames;
}

inline QJsonObject parseDatagram(const QByteArray& frame, Reassembly& rx,
                                 const QString& source = QStringLiteral("local"), qint64 now = SwarmClock::nowMs())
{
    rx.expire(now);
    auto reject = [&rx]() -> QJsonObject {
        ++rx.rejected;
        return {};
    };
    if (frame.size() < 2 + HEADER_SIZE || frame.size() > 130 || qFromBigEndian<quint16>(frame.constData()) != PT_V2 ||
        frame[2] != 2)
        return reject();
    const quint64 session = qFromBigEndian<quint64>(frame.constData() + 3);
    const quint32 id = qFromBigEndian<quint32>(frame.constData() + 11);
    const quint16 index = qFromBigEndian<quint16>(frame.constData() + 15);
    const quint16 count = qFromBigEndian<quint16>(frame.constData() + 17);
    const quint32 size = qFromBigEndian<quint32>(frame.constData() + 19);
    const quint32 crc = qFromBigEndian<quint32>(frame.constData() + 23);
    if (size == 0 || size > MAX_MESSAGE || count != (size + CHUNK_SIZE - 1) / CHUNK_SIZE || index >= count ||
        frame.size() - 2 - HEADER_SIZE != qMin(quint32(CHUNK_SIZE), size - index * CHUNK_SIZE))
        return reject();
    const QString key = source + QLatin1Char('|') + QString::number(session) + QLatin1Char('|') + QString::number(id);
    if (rx.completed.contains(key)) {
        ++rx.duplicates;
        return {};
    }
    if (!rx.messages.contains(key)) {
        int perSource = 0;
        for (const auto& message : rx.messages)
            if (message.source == source)
                ++perSource;
        if (rx.messages.size() >= MAX_CONTEXTS || perSource >= MAX_PER_SOURCE)
            return reject();
        rx.messages.insert(key, {source, QByteArray(size, char(0)), QBitArray(count), crc, now, 0});
    }
    auto& message = rx.messages[key];
    if (message.body.size() != size || message.checksum != crc || message.received.size() != count) {
        rx.messages.remove(key);
        return reject();
    }
    const QByteArray chunk = frame.mid(2 + HEADER_SIZE);
    const int offset = index * CHUNK_SIZE;
    if (message.received.testBit(index)) {
        if (message.body.mid(offset, chunk.size()) != chunk) {
            rx.messages.remove(key);
            return reject();
        }
        ++rx.duplicates;
        return {};
    }
    std::copy(chunk.begin(), chunk.end(), message.body.begin() + offset);
    message.received.setBit(index);
    if (++message.count != count)
        return {};
    const QByteArray body = message.body;
    rx.messages.remove(key);
    if (crc32(body) != crc)
        return reject();
    const auto result = decodePayload(body);
    if (result.isEmpty())
        return reject();
    if (rx.completed.size() >= 512) {
        auto oldest = rx.completed.begin();
        for (auto it = rx.completed.begin(); it != rx.completed.end(); ++it)
            if (it.value() < oldest.value())
                oldest = it;
        rx.completed.erase(oldest);
    }
    rx.completed.insert(key, now);
    return result;
}
}  // namespace SwarmTunnel
