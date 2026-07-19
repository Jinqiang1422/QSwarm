#include "SwarmPerceptionBridge.h"

#include <QtCore/QDateTime>
#include <QtNetwork/QUdpSocket>

SwarmPerceptionBridge::SwarmPerceptionBridge(QObject *parent)
    : QObject(parent)
{
}

SwarmPerceptionBridge::~SwarmPerceptionBridge()
{
    stop();
}

bool SwarmPerceptionBridge::listen(quint16 port)
{
    stop();
    _socket = new QUdpSocket(this);
    const bool ok = _socket->bind(QHostAddress::AnyIPv4, port);
    if (!ok) {
        delete _socket;
        _socket = nullptr;
        return false;
    }
    connect(_socket, &QUdpSocket::readyRead, this, &SwarmPerceptionBridge::_onReadyRead);
    _port = port;
    _listening = true;
    emit listeningChanged();
    return true;
}

void SwarmPerceptionBridge::stop()
{
    if (_socket) {
        _socket->close();
        _socket->deleteLater();
        _socket = nullptr;
    }
    _listening = false;
    emit listeningChanged();
}

bool SwarmPerceptionBridge::serviceOnline() const
{
    return _lastHeartbeatMonoMs > 0
            && QDateTime::currentMSecsSinceEpoch() - _lastHeartbeatMonoMs < 3000;
}

void SwarmPerceptionBridge::_onReadyRead()
{
    while (_socket && _socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(int(_socket->pendingDatagramSize()));
        _socket->readDatagram(datagram.data(), datagram.size());
        const QJsonObject msg = parseDatagram(datagram, _rx);
        if (!msg.isEmpty()) {
            _dispatch(msg);
        }
    }
}

void SwarmPerceptionBridge::_dispatch(const QJsonObject &msg)
{
    const QString kind = msg.value(QStringLiteral("kind")).toString();
    const QJsonObject data = msg.value(QStringLiteral("data")).toObject();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (kind == QStringLiteral("heartbeat")) {
        if (_lastHeartbeatMonoMs > 0) {
            const double interval = (nowMs - _lastHeartbeatMonoMs) / 1000.0;
            if (interval > 0.05) {
                _heartbeatHz = 0.7 * _heartbeatHz + 0.3 * (1.0 / interval);
            }
        }
        _lastHeartbeatMonoMs = nowMs;
        QVariantMap hb;
        hb[QStringLiteral("tracks_active")]   = data.value(QStringLiteral("tracks_active")).toInt();
        hb[QStringLiteral("fps_x10")]         = data.value(QStringLiteral("fps_x10")).toInt();
        hb[QStringLiteral("camera_channels")] = data.value(QStringLiteral("camera_channels")).toInt();
        hb[QStringLiteral("status")]          = data.value(QStringLiteral("status")).toInt();
        hb[QStringLiteral("time_usec")]       = data.value(QStringLiteral("time_usec")).toDouble();
        emit heartbeatReceived(hb);
        emit heartbeatChanged();
    } else if (kind == QStringLiteral("track_batch")) {
        const QJsonArray arr = data.value(QStringLiteral("tracks")).toArray();
        QVariantList tracks;
        tracks.reserve(arr.count());
        for (const QJsonValue &v : arr) {
            tracks.append(v.toObject().toVariantMap());
        }
        _lastTrackCount = tracks.count();
        const qint64 tUsec = qint64(data.value(QStringLiteral("time_usec")).toDouble());
        if (tUsec > 0) {                                // 同主机/PTP 时有效
            _wireLatencyMs = qMax(0.0, nowMs - tUsec / 1000.0);
        }
        emit trackBatchReceived(tracks, tUsec);
        emit tracksChanged();
    } else if (kind == QStringLiteral("track")) {
        _lastTrackCount = 1;
        emit trackBatchReceived({data.toVariantMap()},
                                qint64(data.value(QStringLiteral("time_usec")).toDouble()));
        emit tracksChanged();
    }
}
